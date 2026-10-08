#include "MiniMap.h"

#include "Icons.h"
#include "MapMesh.h"
#include "Settings.h"

// third-party header: deprecated <codecvt>, mixed enums and so on - not our warnings
#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
#pragma warning(push, 0)
#include <SKSEMenuFramework.h>
#pragma warning(pop)

namespace ImGui = ImGuiMCP;

namespace MiniMap
{
    namespace
    {
        float                   markerRange = 0.0f;       // main thread: markers are gathered as far as the map shows
        constexpr float         kStaticEvery = 0.25f;     // seconds: doors, containers, items, bodies, plants, quests
        constexpr float         kDeg = 0.01745329252f;
        constexpr float         kLocalReach = 10240.0f;   // the local map: as far as the game keeps cells loaded around

        constexpr ImGui::ImU32 Rgb(int r, int g, int b, int a = 255)
        {
            return (static_cast<ImGui::ImU32>(a) << 24) | (static_cast<ImGui::ImU32>(b) << 16) | (static_cast<ImGui::ImU32>(g) << 8) | static_cast<ImGui::ImU32>(r);
        }

        // backbuffer size in pixels, known after the first HUD frame
        std::atomic<float> screenW = 0.0f;
        std::atomic<float> screenH = 0.0f;

        constexpr std::uint32_t     kEscape = 1;
        constexpr float             kShowFade = 0.3f;     // seconds the minimap takes to fade in or out (its key)
        std::atomic<std::uint32_t*> captureSlot = nullptr;  // the key setting being bound from the menu
        float                       shown = 0.0f;         // main thread: how much of the minimap shows, 0..1 (fading to Settings visible; in on the first frame)

        // the mouse, for the local map (main thread: input events and the map menu's update)
        bool mouseHeld = false;  // the left button down
        int  wheel = 0;          // wheel steps since the last map update (up: +)

        // free gameplay: no menu takes the input, nothing paused
        bool InGameplay()
        {
            const auto ui = RE::UI::GetSingleton();
            const auto controls = RE::ControlMap::GetSingleton();
            if (!ui || !controls || ui->GameIsPaused() || ui->IsMenuOpen(RE::Console::MENU_NAME)) {
                return false;
            }
            const auto& stack = controls->GetRuntimeData().contextPriorityStack;
            return !stack.empty() && stack.back() == RE::UserEvents::INPUT_CONTEXT_ID::kGameplay;
        }

        // the HUD is up: not paused, not loading, not hidden (by a menu, a scene, the console's tm)
        bool HudShown()
        {
            const auto ui = RE::UI::GetSingleton();
            return ui && !ui->GameIsPaused() && ui->IsShowingMenus() && ui->IsMenuOpen(RE::HUDMenu::MENU_NAME) &&
                   !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
        }

        // where the camera looks, as a heading (radians, 0 = north, clockwise); north up: always 0
        float CameraYaw(RE::PlayerCharacter* a_player)
        {
            if (Settings::Map().northUp) {
                return 0.0f;
            }
            const auto camera = RE::PlayerCamera::GetSingleton();
            const auto root = camera ? camera->cameraRoot.get() : nullptr;
            if (root) {
                // the camera node looks along its Y axis
                const auto& m = root->world.rotate;
                const float fx = m.entry[0][1], fy = m.entry[1][1];
                if (fx * fx + fy * fy > 1e-4f) {
                    return std::atan2(fx, fy);
                }
            }
            return a_player->GetAngleZ();
        }

        // world units from the minimap's centre to its edge: outside, or in caves and houses
        float MinimapRange(bool a_inside)
        {
            const auto& s = Settings::Map();
            return a_inside ? s.minimapRangeInside : s.minimapRange;
        }

        // how far from the character the minimap's square reaches: across half its side, ahead stretched by the tilt;
        // the corner is the farthest
        float MinimapReach(bool a_inside)
        {
            const auto& s = Settings::Map();
            const float st = std::max(std::sin(s.minimapTilt * kDeg), 0.3f);
            return MinimapRange(a_inside) * std::sqrt(1.0f + 1.0f / (st * st)) * 1.05f;
        }

        bool Shown(Icons::Kind a_kind)
        {
            return Settings::Map().show[static_cast<std::size_t>(a_kind)];
        }

        // ---- markers: what gets an icon, in world space (main thread)
        using Kind = Icons::Kind;

        struct Marker
        {
            RE::NiPoint3 pos;
            Kind         kind;
            RE::FormID   id = 0;
            float        alpha = 1.0f;  // fading in when it turns up, out when it is gone
        };
        std::vector<Marker> statics;  // doors, containers, items, bodies, plants: gathered a few times a second
        float               sinceStatics = kStaticEvery;

        // the fade, per marker and kind (a resident turned enemy: one icon fades out, the other in). The alpha
        // walks towards 1 while the thing is there and towards 0 once it is gone, never jumps (main thread)
        struct Seen
        {
            float  alpha = 0.0f;
            float  at = 0.0f;  // iconClock it was last brought up to date
            Marker marker;
        };
        std::unordered_map<std::uint64_t, Seen> seen;
        float                                   iconClock = 0.0f;

        std::optional<Kind> ItemKind(RE::TESBoundObject* a_base)
        {
            switch (a_base->GetFormType()) {
            case RE::FormType::AlchemyItem:
                return static_cast<RE::AlchemyItem*>(a_base)->IsFood() ? Kind::kFood : Kind::kPotion;
            case RE::FormType::Weapon:
            case RE::FormType::Ammo:
                return a_base->GetPlayable() ? std::optional{ Kind::kWeapon } : std::nullopt;
            case RE::FormType::Armor:
                return a_base->GetPlayable() ? std::optional{ Kind::kArmor } : std::nullopt;
            case RE::FormType::Misc:
            case RE::FormType::Book:
            case RE::FormType::Scroll:
            case RE::FormType::SoulGem:
            case RE::FormType::KeyMaster:
            case RE::FormType::Ingredient:
                return Kind::kLoot;
            default:
                return std::nullopt;
            }
        }

        // the merchants' chests (hidden under the floor, where a vendor's goods are kept): every faction's vendor
        // container, read once
        const std::unordered_set<RE::FormID>& MerchantChests()
        {
            static const auto chests = [] {
                std::unordered_set<RE::FormID> out;
                if (const auto data = RE::TESDataHandler::GetSingleton()) {
                    for (const auto faction : data->GetFormArray<RE::TESFaction>()) {
                        if (const auto chest = faction ? faction->vendorData.merchantContainer : nullptr) {
                            out.insert(chest->GetFormID());
                        }
                    }
                }
                logger::info("map: {} merchant chests left off the map", out.size());
                return out;
            }();
            return chests;
        }

        // ---- bodies: a dead one keeps its icon until its inventory was opened, or while it has something to take
        std::mutex                                           bodyLock;  // the container menu's event may come on another thread
        std::unordered_set<RE::FormID>                       lookedInto;
        std::unordered_map<RE::FormID, std::pair<float, bool>> lootCache;  // per body: iconClock it was looked at, something to take

        bool WorthALook(RE::Actor* a_body)
        {
            const auto id = a_body->GetFormID();
            {
                std::scoped_lock lock(bodyLock);
                if (lookedInto.contains(id)) {
                    return false;
                }
            }
            // the inventory every 2 seconds at most (taking it is not free)
            auto& [at, worth] = lootCache.try_emplace(id, -100.0f, false).first->second;
            if (iconClock - at >= 2.0f || iconClock < at) {
                at = iconClock;
                const auto items = a_body->GetInventory([](RE::TESBoundObject& a_object) { return a_object.GetPlayable(); });
                worth = std::ranges::any_of(items, [](const auto& a_item) { return a_item.second.first > 0; });
            }
            return worth;
        }

        // a plant still to pick: flora or a bush with an ingredient, not picked yet
        bool Pickable(RE::TESObjectREFR* a_ref, RE::TESBoundObject* a_base)
        {
            if ((a_ref->formFlags & RE::TESObjectREFR::RecordFlags::kHarvested) != 0) {
                return false;
            }
            if (const auto flora = a_base->As<RE::TESFlora>()) {
                return flora->produceItem != nullptr;
            }
            if (const auto tree = a_base->As<RE::TESObjectTREE>()) {
                return tree->produceItem != nullptr;
            }
            return false;
        }

        // every kind is gathered (the menu can switch one back on any moment); hidden ones are skipped when drawn
        void GatherStatics(RE::PlayerCharacter* a_player)
        {
            statics.clear();
            const auto  tes = RE::TES::GetSingleton();
            const auto& merchants = MerchantChests();
            if (!tes) {
                return;
            }
            tes->ForEachReferenceInRange(a_player, markerRange, [&](RE::TESObjectREFR* a_ref) {
                const auto base = a_ref ? a_ref->GetBaseObject() : nullptr;
                if (!base || a_ref->IsDisabled() || a_ref->IsDeleted()) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                if (a_ref->Is(RE::FormType::ActorCharacter)) {
                    // the living are taken every frame (Characters); here the dead
                    const auto actor = a_ref->As<RE::Actor>();
                    if (actor && actor != a_player && actor->IsDead() && actor->Is3DLoaded() && WorthALook(actor)) {
                        statics.push_back({ a_ref->GetPosition(), Kind::kBody, a_ref->GetFormID() });
                    }
                } else if (base->Is(RE::FormType::Door)) {
                    if (a_ref->extraList.HasType<RE::ExtraTeleport>()) {
                        statics.push_back({ a_ref->GetPosition(), Kind::kDoor, a_ref->GetFormID() });
                    }
                } else if (base->Is(RE::FormType::Container)) {
                    if (merchants.contains(a_ref->GetFormID())) {
                        return RE::BSContainer::ForEachResult::kContinue;
                    }
                    statics.push_back({ a_ref->GetPosition(), Kind::kContainer, a_ref->GetFormID() });
                } else if (base->Is(RE::FormType::Flora) || base->Is(RE::FormType::Tree)) {
                    if (Pickable(a_ref, base)) {
                        statics.push_back({ a_ref->GetPosition(), Kind::kFlora, a_ref->GetFormID() });
                    }
                } else if (a_ref->Is3DLoaded()) {
                    if (const auto kind = ItemKind(base)) {
                        statics.push_back({ a_ref->GetPosition(), *kind, a_ref->GetFormID() });
                    }
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }

        // the characters as they stand this frame, within a_range
        void Characters(RE::PlayerCharacter* a_player, float a_range, std::vector<Marker>& a_out)
        {
            const auto pos = a_player->GetPosition();
            const auto lists = RE::ProcessLists::GetSingleton();
            for (std::uint32_t i = 0; lists && i < lists->highActorHandles.size(); ++i) {
                const auto ptr = lists->highActorHandles[i].get();
                const auto actor = ptr.get();
                if (!actor || actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded() || actor->GetPosition().GetDistance(pos) > a_range) {
                    continue;
                }
                Kind kind;
                if (actor->IsHostileToActor(a_player)) {
                    kind = Kind::kEnemy;
                } else if (actor->IsPlayerTeammate()) {
                    kind = Kind::kFollower;
                } else if (actor->IsGuard()) {
                    kind = Kind::kGuard;
                } else if (actor->HasKeywordString("ActorTypeNPC")) {
                    kind = Kind::kNpc;
                } else {
                    kind = Kind::kCreature;
                }
                a_out.push_back({ actor->GetPosition(), kind, actor->GetFormID() });
            }
        }

        // statics + the characters, fading in and out
        std::vector<Marker> Markers(RE::PlayerCharacter* a_player)
        {
            auto out = statics;
            Characters(a_player, markerRange, out);
            // fade in what turned up; what is gone stays a moment longer, fading out
            const auto& s = Settings::Map();
            const auto  step = [](float a_fade, float a_dt) { return a_fade > 0.0f ? a_dt / a_fade : 1.0f; };
            const auto  key = [](const Marker& a_m) { return (static_cast<std::uint64_t>(a_m.id) << 8) | static_cast<std::uint64_t>(a_m.kind); };
            for (auto& m : out) {
                auto& e = seen.try_emplace(key(m), Seen{ 0.0f, iconClock, m }).first->second;
                e.alpha = std::min(e.alpha + step(s.iconFadeIn, e.at < 0.0f ? 0.0f : iconClock - e.at), 1.0f);
                e.at = -1.0f;  // there this time
                e.marker = m;
                m.alpha = e.alpha;
            }
            for (auto it = seen.begin(); it != seen.end();) {
                auto& e = it->second;
                if (e.at < 0.0f) {
                    e.at = iconClock;
                    ++it;
                    continue;
                }
                e.alpha -= step(s.iconFadeOut, iconClock - e.at);
                e.at = iconClock;
                if (e.alpha <= 0.0f) {
                    it = seen.erase(it);
                    continue;
                }
                auto m = e.marker;
                m.alpha = e.alpha;
                out.push_back(m);
                ++it;
            }
            return out;
        }

        // ---- quest targets: where the compass points for each tracked quest - the target itself when it is in this
        // world space (or room), else the first door on the way to it that is
        struct QuestPoint
        {
            RE::ObjectRefHandle ref;  // followed every frame (a target walking about)
            RE::NiPoint3        pos;
        };
        std::vector<QuestPoint> quests;  // main thread: gathered with the statics

        bool HereToo(RE::TESObjectREFR* a_ref, RE::PlayerCharacter* a_player)
        {
            const auto cell = a_player->GetParentCell();
            if (!cell || !a_ref) {
                return false;
            }
            if (cell->IsInteriorCell()) {
                return a_ref->GetParentCell() == cell;
            }
            const auto world = a_player->GetWorldspace();
            return world && a_ref->GetWorldspace() == world;
        }

        void GatherQuests(RE::PlayerCharacter* a_player)
        {
            quests.clear();
            // taken under the player's lock, resolved after it (resolving takes the quests' own locks)
            std::vector<std::pair<RE::TESQuest*, RE::TESQuestTarget*>> targets;
            {
                RE::BSSpinLockGuard guard(a_player->GetQuestTargetsLock());
                for (const auto& [quest, list] : a_player->GetQuestTargets()) {
                    if (!quest || !list) {
                        continue;
                    }
                    for (const auto target : *list) {
                        if (target) {
                            targets.emplace_back(quest, target);
                        }
                    }
                }
            }
            for (const auto& [quest, target] : targets) {
                RE::TESObjectREFR*               at = nullptr;
                RE::NiPointer<RE::TESObjectREFR> held;
                RE::ObjectRefHandle              handle;
                target->GetTargetRef(handle, false, quest);
                held = handle.get();
                if (held && HereToo(held.get(), a_player)) {
                    at = held.get();
                } else {
                    for (const auto& link : target->teleportPath.teleportRefs) {
                        if (link.ref && HereToo(link.ref, a_player)) {
                            at = link.ref;
                            break;
                        }
                    }
                }
                if (at) {
                    quests.push_back({ at->CreateRefHandle(), at->GetPosition() });
                }
            }
        }

        // ---- what a map picture needs from the game, taken on the main thread
        struct World
        {
            RE::NiPoint3              player;
            float                     tall = 128.0f;
            float                     heading = 0.0f;  // the character's
            float                     yaw = 0.0f;      // the map's up (camera or north)
            bool                      inside = false;
            RE::FormID                space = 0;
            std::vector<Marker>       markers;
            std::vector<RE::NiPoint3> quests;
        };

        World TakeWorld(RE::PlayerCharacter* a_player, bool a_local)
        {
            World w;
            w.player = a_player->GetPosition();
            w.tall = a_player->GetHeight() > 1.0f ? a_player->GetHeight() : 128.0f;
            w.heading = a_player->GetAngleZ();
            w.yaw = a_local ? 0.0f : CameraYaw(a_player);
            const auto cell = a_player->GetParentCell();
            w.inside = cell && cell->IsInteriorCell();
            w.space = MapMesh::CurrentSpace();
            if (a_local) {
                // the game paused: no fading, everything as it is
                w.markers = statics;
                Characters(a_player, markerRange, w.markers);
            } else {
                w.markers = Markers(a_player);
            }
            for (auto& q : quests) {
                if (const auto ref = q.ref.get()) {
                    q.pos = ref->GetPosition();
                }
                w.quests.push_back(q.pos);
            }
            return w;
        }

        // a tilted orthographic view: x across the screen, y up it, seen from a_tilt degrees above the floor
        struct Ortho
        {
            RE::NiPoint3 center;
            float        yaw, tilt;
            float        halfH;  // world units from the centre to the top edge
            float        width, height;

            float Scale() const { return height * 0.5f / halfH; }  // px per world unit

            std::pair<float, float> ToScreen(const RE::NiPoint3& a_p) const
            {
                const float fx = std::sin(yaw), fy = std::cos(yaw), rx = std::cos(yaw), ry = -std::sin(yaw);
                const float st = std::sin(tilt), ct = std::cos(tilt);
                const float dx = a_p.x - center.x, dy = a_p.y - center.y, dz = a_p.z - center.z;
                const float s = Scale();
                return { width * 0.5f + (dx * rx + dy * ry) * s, height * 0.5f - ((dx * fx + dy * fy) * st + dz * ct) * s };
            }

            // the same projection as a clip-space matrix for MapMesh
            void Fill(MapMesh::View& a_view) const
            {
                const float fx = std::sin(yaw), fy = std::cos(yaw), rx = std::cos(yaw), ry = -std::sin(yaw);
                const float st = std::sin(tilt), ct = std::cos(tilt);
                const float R = halfH;
                const float rows[3][3] = { { rx, ry, 0.0f }, { fx * st, fy * st, ct }, { fx * ct, fy * ct, -st } };
                const float axis[3] = { height / (width * R), 1.0f / R, 1.0f / (8.0f * R) };  // the depth spans 8 R either way
                for (int r = 0; r < 3; ++r) {
                    a_view.rows[r][0] = rows[r][0] * axis[r];
                    a_view.rows[r][1] = rows[r][1] * axis[r];
                    a_view.rows[r][2] = rows[r][2] * axis[r];
                    a_view.rows[r][3] = -(rows[r][0] * center.x + rows[r][1] * center.y + rows[r][2] * center.z) * axis[r];
                }
                a_view.rows[2][3] += 0.5f;
                a_view.rows[3][0] = a_view.rows[3][1] = a_view.rows[3][2] = 0.0f;
                a_view.rows[3][3] = 1.0f;
            }
        };

        // the look and the cut of a picture (any thread: settings and a World only)
        MapMesh::View MeshView(const World& a_world, float a_w, float a_h)
        {
            const auto&   s = Settings::Map();
            MapMesh::View v;
            v.width = static_cast<int>(a_w);
            v.height = static_cast<int>(a_h);
            v.player = a_world.player;
            // inside the ceiling goes; outside nothing by height, only what covers the character (the occlusion cut)
            v.cut = s.cut && a_world.inside ? s.cutHeight : 1.0e7f;
            v.space = a_world.space;
            const auto& l = s.look;
            for (int i = 0; i < 3; ++i) {
                v.ground[i] = l.groundColor[i] * l.groundBrightness;
                v.geometry[i] = l.geometryColor[i] * l.geometryBrightness;
                v.roads[i] = l.roadColor[i];
                v.water[i] = l.waterColor[i];
            }
            v.ground[3] = l.groundOpacity;
            v.geometry[3] = l.geometryOpacity;
            v.lines[0] = l.grid ? l.gridSize : 0.0f;
            v.lines[1] = l.contours ? l.contourStep : 0.0f;
            v.lines[2] = l.lineStrength;
            v.lines[3] = 0.0f;
            v.shade[0] = l.depthShade;
            v.shade[1] = a_world.inside ? 1.0f : 0.0f;
            v.shade[2] = static_cast<float>(l.style);
            v.fadeTime = l.fadeTime;
            v.roads[3] = l.roads ? 1.0f + (l.roadWidth - 1.0f) * 150.0f : 0.0f;  // on; past 1: widened by that many world units
            v.water[3] = l.water ? 1.0f : 0.0f;
            return v;
        }

        // the occlusion cut: where the character stands on this picture and how far along the view; whatever lies
        // inside that outline and nearer is cut away
        void SetOccluder(MapMesh::View& a_view, const World& a_world)
        {
            if (!Settings::Map().cut) {
                return;
            }
            const auto& pos = a_world.player;
            struct Screen
            {
                float x, y, depth;
            };
            const auto project = [&](const RE::NiPoint3& a_p) -> Screen {
                float c[3];
                for (int r = 0; r < 3; ++r) {
                    c[r] = a_view.rows[r][0] * a_p.x + a_view.rows[r][1] * a_p.y + a_view.rows[r][2] * a_p.z + a_view.rows[r][3];
                }
                return { (c[0] * 0.5f + 0.5f) * a_view.width, (0.5f - c[1] * 0.5f) * a_view.height, c[2] };
            };
            const auto feet = project(pos);
            const auto head = project(pos + RE::NiPoint3{ 0.0f, 0.0f, a_world.tall });
            const auto middle = project(pos + RE::NiPoint3{ 0.0f, 0.0f, a_world.tall * 0.5f });
            // the outline: the character's height on this picture, a bit wider than they are, never smaller than a few px
            const float ry = std::max(std::abs(feet.y - head.y) * 0.65f, 6.0f) + 4.0f;
            a_view.occluder[0] = middle.x;
            a_view.occluder[1] = middle.y;
            a_view.occluder[2] = std::max(ry * 0.7f, 8.0f);
            a_view.occluder[3] = ry;
            a_view.occluder2[0] = middle.depth;
            a_view.occluder2[1] = pos.z + 40.0f;  // the ground at their feet stays
        }

        // ---- a map picture with its markers, built on the main thread, drawn by the HUD element
        using P2 = std::pair<float, float>;

        struct ScreenIcon
        {
            float x, y;
            Kind  kind;
            float alpha;
        };
        struct Frame
        {
            bool                         has = false;
            bool                         local = false;  // the local map (in the map menu), not the minimap
            bool                         round = false;
            float                        mx0 = 0, my0 = 0, mx1 = 0, my1 = 0;  // the picture on the screen
            float                        px = 0, py = 0;                      // the character on it
            std::vector<ScreenIcon>      icons;
            std::vector<ScreenIcon>      quests;         // quest targets (pulled onto the rim when past it)
            bool                         beam = false;   // the beam to the nearest quest target, to (bx, by)
            float                        bx = 0, by = 0;
            float                        arrowAngle = 0.0f;
            float                        alpha = 1.0f;   // the whole map: fading in or out
            float                        cursorX = -1.0f, cursorY = -1.0f;  // the local map: the menu's cursor (drawn over the map)
            std::string                  title;          // the local map: where the character is
            std::optional<MapMesh::View> view;
        };
        std::mutex frameLock;
        Frame      frame;       // the minimap
        Frame      localFrame;  // the local map

        void ClearFrame()
        {
            std::scoped_lock lock(frameLock);
            frame = {};
        }

        // the markers, the quest targets and the beam of a picture; a_inside(x, y): on the picture, a_rim(x, y): pulled
        // in to its edge along the line from the character
        template <class ToScreen, class Inside, class Rim>
        void PlaceMarkers(Frame& a_f, const World& a_world, ToScreen&& a_toScreen, Inside&& a_inside, Rim&& a_rim)
        {
            for (const auto& m : a_world.markers) {
                if (!Shown(m.kind)) {
                    continue;
                }
                const auto [x, y] = a_toScreen(m.pos);
                if (const float edge = a_inside(x, y); edge > 0.0f) {
                    a_f.icons.push_back({ x, y, m.kind, m.alpha * edge });
                }
            }
            if (!Shown(Kind::kQuest) && !Settings::Map().questBeam) {
                return;
            }
            float nearest = FLT_MAX;
            for (const auto& q : a_world.quests) {
                const auto [x, y] = a_rim(a_toScreen(q));
                if (Shown(Kind::kQuest)) {
                    a_f.quests.push_back({ x, y, Kind::kQuest, 1.0f });
                }
                const float dx = q.x - a_world.player.x, dy = q.y - a_world.player.y, d = dx * dx + dy * dy;
                if (Settings::Map().questBeam && d < nearest) {
                    nearest = d;
                    a_f.beam = true;
                    a_f.bx = x;
                    a_f.by = y;
                }
            }
        }

        Frame BuildMinimap(const World& a_world, float a_w, float a_h)
        {
            const auto& s = Settings::Map();
            const float k = a_h / 1080.0f;
            const float size = s.minimapSize * k;
            Frame       f;
            f.has = true;
            // in its corner of the screen, this far from the edges (kept on the screen)
            const bool right = (s.anchor & 1) != 0, bottom = (s.anchor & 2) != 0;
            f.mx0 = std::clamp(right ? a_w - s.offsetX * k - size : s.offsetX * k, 0.0f, std::max(a_w - size, 0.0f));
            f.my0 = std::clamp(bottom ? a_h - s.offsetY * k - size : s.offsetY * k, 0.0f, std::max(a_h - size, 0.0f));
            f.mx1 = f.mx0 + size;
            f.my1 = f.my0 + size;
            f.px = (f.mx0 + f.mx1) * 0.5f;
            f.py = (f.my0 + f.my1) * 0.5f;
            f.arrowAngle = a_world.heading - a_world.yaw;  // the character's heading relative to the map's up

            const Ortho o{ a_world.player, a_world.yaw, s.minimapTilt * kDeg, MinimapRange(a_world.inside), size, size };
            const float margin = 12.0f * k;
            const float cx = f.px, cy = f.py, rim = size * 0.5f;
            f.round = s.minimapRound;
            PlaceMarkers(
                f, a_world,
                [&](const RE::NiPoint3& a_p) {
                    const auto p = o.ToScreen(a_p);
                    return P2{ f.mx0 + p.first, f.my0 + p.second };
                },
                [&](float x, float y) {
                    if (f.round) {
                        // round: the icons fade out over the last few px before the rim instead of being cut by it
                        const float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
                        return std::clamp((rim - 4.0f * k - d) / (10.0f * k), 0.0f, 1.0f);
                    }
                    return x > f.mx0 - margin && x < f.mx1 + margin && y > f.my0 - margin && y < f.my1 + margin ? 1.0f : 0.0f;
                },
                [&](P2 a_p) {
                    const float inset = 13.0f * k * s.iconSize;
                    const float dx = a_p.first - cx, dy = a_p.second - cy;
                    float       t = 1.0f;
                    if (f.round) {
                        const float d = std::sqrt(dx * dx + dy * dy);
                        t = d > rim - inset ? (rim - inset) / d : 1.0f;
                    } else {
                        const float half = rim - inset;
                        t = std::min({ 1.0f, std::abs(dx) > half ? half / std::abs(dx) : 1.0f, std::abs(dy) > half ? half / std::abs(dy) : 1.0f });
                    }
                    return P2{ cx + dx * t, cy + dy * t };
                });
            auto view = MeshView(a_world, size, size);
            o.Fill(view);
            view.range = MinimapReach(a_world.inside);
            SetOccluder(view, a_world);
            f.view = view;
            return f;
        }

        // ---- the local map: in the map menu, over the game's own; dragged with the mouse, zoomed with the wheel
        struct Local
        {
            bool         open = false;
            RE::NiPoint3 centre;
            float        range = 0.0f;  // world units from the centre to the top edge
            float        lastX = 0.0f, lastY = 0.0f;
            bool         wasHeld = false;
        };
        Local local;

        // the menu's cursor in screen pixels
        std::pair<float, float> Cursor(float a_w, float a_h)
        {
            const auto cursor = RE::MenuCursor::GetSingleton();
            if (!cursor) {
                return { -1.0f, -1.0f };
            }
            const auto& c = cursor->GetRuntimeData();
            const float sx = c.screenWidthX > 1.0f ? a_w / c.screenWidthX : 1.0f;
            const float sy = c.screenWidthY > 1.0f ? a_h / c.screenWidthY : 1.0f;
            return { c.cursorPosX * sx, c.cursorPosY * sy };
        }

        // the local map's picture on the screen: most of it, the menu's bottom bar left free
        void LocalRect(float a_w, float a_h, float& a_x0, float& a_y0, float& a_x1, float& a_y1)
        {
            a_x0 = a_w * 0.04f;
            a_x1 = a_w * 0.96f;
            a_y0 = a_h * 0.07f;
            a_y1 = a_h * 0.86f;
        }

        std::string PlaceName(RE::PlayerCharacter* a_player)
        {
            const auto cell = a_player->GetParentCell();
            if (cell && cell->IsInteriorCell() && cell->GetFullName() && *cell->GetFullName()) {
                return cell->GetFullName();
            }
            if (const auto location = a_player->GetCurrentLocation(); location && location->GetFullName() && *location->GetFullName()) {
                return location->GetFullName();
            }
            const auto world = a_player->GetWorldspace();
            return world && world->GetFullName() ? world->GetFullName() : "";
        }

        Frame BuildLocal(RE::PlayerCharacter* a_player, const World& a_world, float a_w, float a_h)
        {
            const auto& s = Settings::Map();
            const float k = a_h / 1080.0f;
            Frame       f;
            f.has = true;
            f.local = true;
            LocalRect(a_w, a_h, f.mx0, f.my0, f.mx1, f.my1);
            const float W = f.mx1 - f.mx0, H = f.my1 - f.my0;
            const Ortho o{ local.centre, 0.0f, s.minimapTilt * kDeg, local.range, W, H };
            const auto  toScreen = [&](const RE::NiPoint3& a_p) {
                const auto p = o.ToScreen(a_p);
                return P2{ f.mx0 + p.first, f.my0 + p.second };
            };
            std::tie(f.px, f.py) = toScreen(a_world.player);
            f.arrowAngle = a_world.heading;
            const float inset = 14.0f * k * s.iconSize;
            PlaceMarkers(
                f, a_world, toScreen, [&](float x, float y) { return x > f.mx0 && x < f.mx1 && y > f.my0 && y < f.my1 ? 1.0f : 0.0f; },
                [&](P2 a_p) {
                    // pulled in along the line from the character (kept inside even when they are off the picture)
                    const float cx = std::clamp(f.px, f.mx0 + inset, f.mx1 - inset), cy = std::clamp(f.py, f.my0 + inset, f.my1 - inset);
                    const float dx = a_p.first - cx, dy = a_p.second - cy;
                    float       t = 1.0f;
                    if (dx > 0.0f && cx + dx > f.mx1 - inset) {
                        t = std::min(t, (f.mx1 - inset - cx) / dx);
                    } else if (dx < 0.0f && cx + dx < f.mx0 + inset) {
                        t = std::min(t, (f.mx0 + inset - cx) / dx);
                    }
                    if (dy > 0.0f && cy + dy > f.my1 - inset) {
                        t = std::min(t, (f.my1 - inset - cy) / dy);
                    } else if (dy < 0.0f && cy + dy < f.my0 + inset) {
                        t = std::min(t, (f.my0 + inset - cy) / dy);
                    }
                    return P2{ cx + dx * t, cy + dy * t };
                });
            auto view = MeshView(a_world, W, H);
            o.Fill(view);
            view.range = 1.0e9f;  // everything loaded that falls on the picture
            SetOccluder(view, a_world);
            f.view = view;
            std::tie(f.cursorX, f.cursorY) = Cursor(a_w, a_h);
            f.title = PlaceName(a_player);
            return f;
        }

        void CloseLocal()
        {
            local.open = false;
            std::scoped_lock lock(frameLock);
            localFrame = {};
        }

        // main thread, every frame of the map menu
        void UpdateLocal(RE::MapMenu* a_menu)
        {
            const auto& s = Settings::Map();
            const auto  player = RE::PlayerCharacter::GetSingleton();
            const auto  data = a_menu ? a_menu->GetRuntimeData() : nullptr;
            const bool  showing = s.enabled && s.localMap && data && data->localMapMenu.GetRuntimeData().showingMap;
            const float w = screenW, h = screenH;
            if (!showing || !player || !player->GetParentCell() || w <= 0.0f || h <= 0.0f) {
                if (local.open) {
                    CloseLocal();
                }
                return;
            }
            const bool inside = player->GetParentCell()->IsInteriorCell();
            const auto [cx, cy] = Cursor(w, h);
            const bool opened = !local.open;
            if (opened) {
                // opened: on the character, as wide as the minimap shows twice
                local.open = true;
                local.centre = player->GetPosition();
                local.range = std::clamp(MinimapRange(inside) * 2.0f, 600.0f, 16000.0f);
                local.wasHeld = false;
                wheel = 0;
                markerRange = inside ? std::max(MinimapReach(true) * 4.0f, 6000.0f) : kLocalReach;
                GatherStatics(player);
                GatherQuests(player);
                logger::info("local map: opened at {:.0f} {:.0f}, cursor {:.0f} {:.0f} on a {:.0f} x {:.0f} screen", local.centre.x, local.centre.y, cx, cy, w, h);
            }
            // the wheel zooms (up: in), dragging moves the map along
            if (wheel != 0) {
                local.range = std::clamp(local.range * std::pow(0.85f, static_cast<float>(wheel)), 300.0f, 20000.0f);
                wheel = 0;
            }
            float x0, y0, x1, y1;
            LocalRect(w, h, x0, y0, x1, y1);
            const float scale = (y1 - y0) * 0.5f / local.range;
            const float st = std::max(std::sin(s.minimapTilt * kDeg), 0.3f);
            if (mouseHeld && local.wasHeld) {
                local.centre.x -= (cx - local.lastX) / scale;
                local.centre.y += (cy - local.lastY) / (scale * st);
                // never farther from the character than the loaded world reaches
                const auto  p = player->GetPosition();
                const float reach = inside ? 20000.0f : kLocalReach;
                local.centre.x = std::clamp(local.centre.x, p.x - reach, p.x + reach);
                local.centre.y = std::clamp(local.centre.y, p.y - reach, p.y + reach);
            }
            local.wasHeld = mouseHeld;
            local.lastX = cx;
            local.lastY = cy;
            const auto world = TakeWorld(player, true);
            auto       next = BuildLocal(player, world, w, h);
            std::scoped_lock lock(frameLock);
            localFrame = std::move(next);
        }

        // main thread, once a frame after the camera is final (the HUD advances after the camera update)
        void Update()
        {
            const auto  start = std::chrono::steady_clock::now();
            const auto  player = RE::PlayerCharacter::GetSingleton();
            const auto& s = Settings::Map();
            const float w = screenW, h = screenH;
            if (local.open && !RE::UI::GetSingleton()->IsMenuOpen(RE::MapMenu::MENU_NAME)) {
                CloseLocal();  // the map menu is closed
            }
            // the map off: nothing is done, the geometry unloads
            if (!s.enabled || !player || !player->Get3D() || !player->GetParentCell()) {
                ClearFrame();
                statics.clear();
                quests.clear();
                seen.clear();
                if (player) {
                    MapMesh::Update(player, 0.0f, s.enabled ? 0.0f : -1.0f);
                }
                return;
            }
            if (!HudShown() || w <= 0.0f || h <= 0.0f) {
                ClearFrame();
                return;  // hidden for a moment (a menu, a scene): nothing done, nothing unloaded
            }
            const auto  here = player->GetParentCell();
            const bool  inside = here->IsInteriorCell();
            const float dt = std::clamp(RE::GetSecondsSinceLastFrame(), 0.0f, 0.1f);
            // the geometry: as far as the minimap's square shows, or (the local map on) all the game keeps loaded
            const float reach = MinimapReach(inside);
            const float harvest = s.localMap && !inside ? std::max(reach, kLocalReach) : reach;
            // shown / hidden by its key: a fade; hidden through, nothing is done but the geometry for the local map
            shown = std::clamp(shown + (s.visible ? dt : -dt) / kShowFade, 0.0f, 1.0f);
            if (shown <= 0.0f) {
                ClearFrame();
                MapMesh::Update(player, dt, s.localMap ? harvest : 0.0f);
                return;
            }
            markerRange = reach;
            sinceStatics += dt;
            iconClock += dt;
            if (sinceStatics >= kStaticEvery) {
                sinceStatics = 0.0f;
                GatherStatics(player);
                GatherQuests(player);
            }
            MapMesh::Update(player, dt, harvest);
            const auto world = TakeWorld(player, false);
            auto       next = BuildMinimap(world, w, h);
            next.alpha = shown * shown * (3.0f - 2.0f * shown);
            {
                std::scoped_lock lock(frameLock);
                frame = std::move(next);
            }
            if (s.debugLog) {
                MapMesh::NoteFrame(static_cast<float>(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()));
            }
        }

        // ---- drawing (render thread)

        // a colour with its alpha scaled (the minimap fading in or out)
        ImGui::ImU32 Faded(ImGui::ImU32 a_col, float a_alpha)
        {
            const auto a = static_cast<ImGui::ImU32>(static_cast<float>(a_col >> 24) * std::clamp(a_alpha, 0.0f, 1.0f) + 0.5f);
            return (a_col & 0x00FFFFFFu) | (a << 24);
        }

        float Seconds()  // render thread: the beam's running light
        {
            static const auto start = std::chrono::steady_clock::now();
            return std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
        }

        // the vanilla style's frame, as round the game's local map: a light silver line outside, a thin dim one inside,
        // dark between them
        void DrawVanillaFrame(ImGui::ImDrawList* a_dl, float a_x0, float a_y0, float a_x1, float a_y1, float a_corner, bool a_round, float a_k, float a_alpha)
        {
            using V2 = ImGui::ImVec2;
            namespace D = ImGui::ImDrawListManager;
            const auto kLight = Faded(Rgb(214, 212, 204), a_alpha), kDim = Faded(Rgb(140, 138, 130), a_alpha), kDark = Faded(Rgb(18, 17, 15, 225), a_alpha);
            const float    out = 4.0f * a_k;  // the outer line past the map's edge
            const V2       centre{ (a_x0 + a_x1) * 0.5f, (a_y0 + a_y1) * 0.5f };
            const float    rim = (a_x1 - a_x0) * 0.5f;
            if (a_round) {
                D::AddCircle(a_dl, centre, rim + out * 0.5f, kDark, 96, out);
                D::AddCircle(a_dl, centre, rim + out, kLight, 96, 2.0f * a_k);
                D::AddCircle(a_dl, centre, rim, kDim, 96, 1.0f * a_k);
            } else {
                D::AddRect(a_dl, V2{ a_x0 - out * 0.5f, a_y0 - out * 0.5f }, V2{ a_x1 + out * 0.5f, a_y1 + out * 0.5f }, kDark, a_corner + out * 0.5f, 0, out);
                D::AddRect(a_dl, V2{ a_x0 - out, a_y0 - out }, V2{ a_x1 + out, a_y1 + out }, kLight, a_corner > 0.0f ? a_corner + out : 0.0f, 0, 2.0f * a_k);
                D::AddRect(a_dl, V2{ a_x0, a_y0 }, V2{ a_x1, a_y1 }, kDim, a_corner, 0, 1.0f * a_k);
            }
        }

        // the beam: from the character to the quest target, a soft glow with a bright core and a light running along it
        void DrawBeam(ImGui::ImDrawList* a_dl, const Frame& a_f, float a_k, float a_alpha)
        {
            using V2 = ImGui::ImVec2;
            namespace D = ImGui::ImDrawListManager;
            const float dx = a_f.bx - a_f.px, dy = a_f.by - a_f.py;
            const float len = std::sqrt(dx * dx + dy * dy);
            const float from = 15.0f * a_k * Settings::Map().iconSize;  // past the character's badge
            const float to = len - 9.0f * a_k * Settings::Map().iconSize;  // short of the target's
            if (to - from < 6.0f * a_k) {
                return;
            }
            const float ux = dx / len, uy = dy / len;
            const auto  at = [&](float a_d) { return V2{ a_f.px + ux * a_d, a_f.py + uy * a_d }; };
            D::AddLine(a_dl, at(from), at(to), Faded(Rgb(255, 196, 80, 60), a_alpha), 8.0f * a_k);
            D::AddLine(a_dl, at(from), at(to), Faded(Rgb(255, 222, 140, 215), a_alpha), 2.2f * a_k);
            const float step = 24.0f * a_k;
            for (float d = from + std::fmod(Seconds() * 55.0f * a_k, step); d < to; d += step) {
                // brightest in the middle of the beam, fading at its ends
                const float t = std::min(d - from, to - d) / (12.0f * a_k);
                D::AddCircleFilled(a_dl, at(d), 2.3f * a_k, Faded(Rgb(255, 245, 205, 235), a_alpha * std::clamp(t, 0.0f, 1.0f)), 10);
            }
        }

        // the icons, the quest targets, the beam and the character over a picture
        void DrawMarkers(ImGui::ImDrawList* a_dl, const Frame& a_f, float a_k)
        {
            const auto& s = Settings::Map();
            const float a = a_f.alpha;
            if (a_f.beam) {
                DrawBeam(a_dl, a_f, a_k, a);
            }
            const float size = 16.0f * a_k * s.iconSize;
            for (const auto& i : a_f.icons) {
                Icons::Draw(a_dl, i.kind, i.x, i.y, size, i.alpha * a);
            }
            for (const auto& q : a_f.quests) {
                Icons::Draw(a_dl, q.kind, q.x, q.y, size * 1.15f, q.alpha * a);
            }
            Icons::DrawPlayer(a_dl, a_f.px, a_f.py, 18.0f * a_k * s.iconSize, a_f.arrowAngle, a);
        }

        void DrawMinimap(ImGui::ImDrawList* a_dl, const Frame& a_f, float a_k)
        {
            using V2 = ImGui::ImVec2;
            const bool  vanilla = Settings::Map().look.style == 1;
            const V2    centre{ (a_f.mx0 + a_f.mx1) * 0.5f, (a_f.my0 + a_f.my1) * 0.5f };
            const float rim = (a_f.mx1 - a_f.mx0) * 0.5f;
            // the square's corners: rounded as set (never past a circle)
            const float corner = a_f.round ? rim : std::clamp(Settings::Map().minimapCorner * a_k, 0.0f, rim);
            const float a = a_f.alpha;
            const auto  background = Faded(vanilla ? Rgb(16, 15, 13, 200) : Rgb(8, 10, 16, 175), a);
            if (a_f.round) {
                ImGui::ImDrawListManager::AddCircleFilled(a_dl, centre, rim, background, 96);
            } else {
                ImGui::ImDrawListManager::AddRectFilled(a_dl, V2{ a_f.mx0, a_f.my0 }, V2{ a_f.mx1, a_f.my1 }, background, corner, 0);
            }
            ImGui::ImDrawListManager::PushClipRect(a_dl, V2{ a_f.mx0, a_f.my0 }, V2{ a_f.mx1, a_f.my1 }, true);
            if (a_f.view) {
                // drawn again every frame (an unchanged picture is shown again by MapMesh without drawing)
                const auto tex = MapMesh::Render(*a_f.view);
                if (tex && corner > 0.5f) {  // the picture's corners rounded off (a circle: all the way)
                    ImGui::ImDrawListManager::AddImageRounded(a_dl, tex, V2{ a_f.mx0, a_f.my0 }, V2{ a_f.mx1, a_f.my1 }, V2{ 0.0f, 0.0f }, V2{ 1.0f, 1.0f }, Faded(Rgb(255, 255, 255), a), corner, 0);
                } else if (tex) {
                    ImGui::ImDrawListManager::AddImage(a_dl, tex, V2{ a_f.mx0, a_f.my0 }, V2{ a_f.mx1, a_f.my1 }, V2{ 0.0f, 0.0f }, V2{ 1.0f, 1.0f }, Faded(Rgb(255, 255, 255), a));
                }
            }
            DrawMarkers(a_dl, a_f, a_k);
            ImGui::ImDrawListManager::PopClipRect(a_dl);
            if (vanilla) {
                DrawVanillaFrame(a_dl, a_f.mx0, a_f.my0, a_f.mx1, a_f.my1, corner, a_f.round, a_k, a);
            } else if (a_f.round) {
                ImGui::ImDrawListManager::AddCircle(a_dl, centre, rim, Faded(Rgb(150, 120, 75), a), 96, 1.5f * a_k);
            } else {
                ImGui::ImDrawListManager::AddRect(a_dl, V2{ a_f.mx0, a_f.my0 }, V2{ a_f.mx1, a_f.my1 }, Faded(Rgb(150, 120, 75), a), corner, 0, 1.5f * a_k);
            }
        }

        // the local map: a dark backdrop over the game's own (its bottom bar left free), the picture in a frame, the
        // place's name over it, and the cursor (the menu's own is under all this)
        void DrawLocal(ImGui::ImDrawList* a_dl, const Frame& a_f, float a_w, float a_k)
        {
            using V2 = ImGui::ImVec2;
            namespace D = ImGui::ImDrawListManager;
            const bool  vanilla = Settings::Map().look.style == 1;
            const float bottom = a_f.my1 + 10.0f * a_k;
            D::AddRectFilled(a_dl, V2{ 0.0f, 0.0f }, V2{ a_w, bottom }, vanilla ? Rgb(12, 11, 10, 248) : Rgb(7, 9, 14, 248), 0.0f, 0);
            D::AddRectFilled(a_dl, V2{ a_f.mx0, a_f.my0 }, V2{ a_f.mx1, a_f.my1 }, vanilla ? Rgb(20, 19, 16) : Rgb(10, 12, 18), 0.0f, 0);
            D::PushClipRect(a_dl, V2{ a_f.mx0, a_f.my0 }, V2{ a_f.mx1, a_f.my1 }, true);
            if (a_f.view) {
                if (const auto tex = MapMesh::Render(*a_f.view)) {
                    D::AddImage(a_dl, tex, V2{ a_f.mx0, a_f.my0 }, V2{ a_f.mx1, a_f.my1 }, V2{ 0.0f, 0.0f }, V2{ 1.0f, 1.0f }, Rgb(255, 255, 255));
                }
            }
            DrawMarkers(a_dl, a_f, a_k);
            D::PopClipRect(a_dl);
            if (vanilla) {
                DrawVanillaFrame(a_dl, a_f.mx0, a_f.my0, a_f.mx1, a_f.my1, 0.0f, false, a_k, 1.0f);
            } else {
                D::AddRect(a_dl, V2{ a_f.mx0, a_f.my0 }, V2{ a_f.mx1, a_f.my1 }, Rgb(150, 120, 75), 0.0f, 0, 1.5f * a_k);
            }
            if (!a_f.title.empty()) {
                if (const auto font = ImGui::GetFont()) {
                    const float base = ImGui::GetFontSize();
                    const float size = 30.0f * a_k;
                    const auto  ts = ImGui::CalcTextSize(a_f.title.c_str());
                    const float kk = base > 0.0f ? size / base : 1.0f;
                    const V2    at{ (a_f.mx0 + a_f.mx1) * 0.5f - ts.x * kk * 0.5f, (a_f.my0 - ts.y * kk) * 0.5f };
                    D::AddText(a_dl, font, size, V2{ at.x + 2.0f * a_k, at.y + 2.0f * a_k }, Rgb(0, 0, 0, 200), a_f.title.c_str());
                    D::AddText(a_dl, font, size, at, vanilla ? Rgb(226, 222, 210) : Rgb(220, 225, 235), a_f.title.c_str());
                }
            }
            // the cursor, while it is over what covers the menu's own
            const float x = a_f.cursorX, y = a_f.cursorY;
            if (x >= 0.0f && y >= 0.0f && y < bottom) {
                const float s = 22.0f * a_k;
                const V2    tip{ x, y }, left{ x, y + s }, right{ x + s * 0.72f, y + s * 0.72f };
                D::AddTriangleFilled(a_dl, tip, left, right, Rgb(235, 230, 215));
                D::AddTriangle(a_dl, tip, left, right, Rgb(20, 18, 15), 1.5f * a_k);
            }
        }

        // the HUD element
        void __stdcall Draw()
        {
            const auto io = ImGui::GetIO();
            const auto ui = RE::UI::GetSingleton();
            if (!io || !ui) {
                return;
            }
            screenW = io->DisplaySize.x;
            screenH = io->DisplaySize.y;
            const float k = io->DisplaySize.y / 1080.0f;
            // the background layer: under every window (SMF) and other HUD elements
            const auto dl = ImGui::GetBackgroundDrawList();
            Frame      f;
            {
                std::scoped_lock lock(frameLock);
                f = ui->IsMenuOpen(RE::MapMenu::MENU_NAME) && localFrame.has ? localFrame : frame;
            }
            if (f.has && f.local) {
                if (ui->IsShowingMenus()) {
                    DrawLocal(dl, f, io->DisplaySize.x, k);
                }
                return;
            }
            if (ui->GameIsPaused() || !ui->IsShowingMenus()) {
                return;
            }
            if (f.has) {
                DrawMinimap(dl, f, k);
            }
        }

        // ---- hooks

        // HUDMenu::AdvanceMovie: every frame on the main thread, after the camera has been placed for it
        struct HudAdvanceHook
        {
            static void thunk(RE::HUDMenu* a_this, float a_interval, std::uint32_t a_currentTime)
            {
                func(a_this, a_interval, a_currentTime);
                Update();
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };

        // MapMenu::AdvanceMovie: every frame of the map menu (the HUD does not run then)
        struct MapAdvanceHook
        {
            static void thunk(RE::MapMenu* a_this, float a_interval, std::uint32_t a_currentTime)
            {
                func(a_this, a_interval, a_currentTime);
                UpdateLocal(a_this);
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };

        // the keys during gameplay (the minimap's, the beam's: shown / hidden, remembered in the ini) and the mouse for
        // the local map
        class InputSink final : public RE::BSTEventSink<RE::InputEvent*>
        {
        public:
            static InputSink* GetSingleton()
            {
                static InputSink sink;
                return &sink;
            }

            RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override
            {
                auto& s = Settings::Map();
                for (auto event = a_event ? *a_event : nullptr; event; event = event->next) {
                    const auto button = event->AsButtonEvent();
                    if (!button) {
                        continue;
                    }
                    if (button->GetDevice() == RE::INPUT_DEVICE::kMouse) {
                        using Mouse = RE::BSWin32MouseDevice::Key;
                        const auto id = button->GetIDCode();
                        if (id == Mouse::kLeftButton) {
                            mouseHeld = button->IsPressed();
                        } else if (button->IsDown() && (id == Mouse::kWheelUp || id == Mouse::kWheelDown) && local.open) {
                            wheel += id == Mouse::kWheelUp ? 1 : -1;
                        }
                        continue;
                    }
                    if (captureSlot || !s.enabled || !button->IsDown() || button->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
                        continue;
                    }
                    const auto id = button->GetIDCode();
                    if ((id == s.toggleKey || id == s.beamKey) && id != 0 && InGameplay()) {
                        (id == s.toggleKey ? s.visible : s.questBeam) ^= true;
                        Settings::Save();
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // a body's inventory opened: its icon goes
        class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
        {
        public:
            static MenuSink* GetSingleton()
            {
                static MenuSink sink;
                return &sink;
            }

            RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
            {
                if (a_event && a_event->menuName == RE::ContainerMenu::MENU_NAME) {
                    const auto ref = RE::TESObjectREFR::LookupByHandle(RE::ContainerMenu::GetTargetRefHandle());
                    if (ref && ref->Is(RE::FormType::ActorCharacter)) {
                        std::scoped_lock lock(bodyLock);
                        lookedInto.insert(ref->GetFormID());
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    void Register()
    {
        if (const auto input = RE::BSInputDeviceManager::GetSingleton()) {
            input->AddEventSink(InputSink::GetSingleton());
        }
        if (const auto ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuSink::GetSingleton());
        }
        if (SKSEMenuFramework::IsInstalled()) {
            SKSEMenuFramework::AddHudElement(Draw);
        } else {
            logger::error("SKSE Menu Framework not found: the map cannot be drawn");
        }
    }

    void Install()
    {
        REL::Relocation<std::uintptr_t> hud{ RE::VTABLE_HUDMenu[0] };
        HudAdvanceHook::func = hud.write_vfunc(0x5, HudAdvanceHook::thunk);
        REL::Relocation<std::uintptr_t> map{ RE::VTABLE_MapMenu[0] };
        MapAdvanceHook::func = map.write_vfunc(0x5, MapAdvanceHook::thunk);
        logger::info("hooks installed");
    }

    void StartCapture(std::uint32_t* a_slot)
    {
        captureSlot = a_slot;
    }

    bool IsCapturing(const std::uint32_t* a_slot)
    {
        const auto slot = captureSlot.load();
        return slot && (!a_slot || slot == a_slot);
    }

    bool OnFrameworkInput(RE::InputEvent* a_event)
    {
        const auto slot = captureSlot.load();
        if (!slot || !a_event || a_event->eventType != RE::INPUT_EVENT_TYPE::kButton) {
            return false;
        }
        const auto button = a_event->AsButtonEvent();
        if (button && button->IsDown() && button->GetDevice() == RE::INPUT_DEVICE::kKeyboard) {
            if (button->GetIDCode() != kEscape) {  // Esc cancels
                *slot = button->GetIDCode();
                Settings::Save();
            }
            captureSlot = nullptr;
        }
        return true;
    }

    void OnGameLoaded()
    {
        std::scoped_lock lock(bodyLock);
        lookedInto.clear();
        lootCache.clear();
    }
}

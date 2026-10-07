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
        constexpr float         kStaticEvery = 0.25f;     // seconds: doors, containers, items
        constexpr float         kDeg = 0.01745329252f;

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
        std::vector<Marker> statics;  // doors, containers, items: gathered a few times a second
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
                if (!base || a_ref->IsDisabled() || a_ref->IsDeleted() || a_ref->Is(RE::FormType::ActorCharacter)) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                if (base->Is(RE::FormType::Door)) {
                    if (a_ref->extraList.HasType<RE::ExtraTeleport>()) {
                        statics.push_back({ a_ref->GetPosition(), Kind::kDoor, a_ref->GetFormID() });
                    }
                } else if (base->Is(RE::FormType::Container)) {
                    if (merchants.contains(a_ref->GetFormID())) {
                        return RE::BSContainer::ForEachResult::kContinue;
                    }
                    statics.push_back({ a_ref->GetPosition(), Kind::kContainer, a_ref->GetFormID() });
                } else if (a_ref->Is3DLoaded()) {
                    if (const auto kind = ItemKind(base)) {
                        statics.push_back({ a_ref->GetPosition(), *kind, a_ref->GetFormID() });
                    }
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }

        // statics + the characters as they stand this frame
        std::vector<Marker> Markers(RE::PlayerCharacter* a_player)
        {
            auto       out = statics;
            const auto pos = a_player->GetPosition();
            const auto lists = RE::ProcessLists::GetSingleton();
            for (std::uint32_t i = 0; lists && i < lists->highActorHandles.size(); ++i) {
                const auto ptr = lists->highActorHandles[i].get();
                const auto actor = ptr.get();
                if (!actor || actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded() || actor->GetPosition().GetDistance(pos) > markerRange) {
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
                out.push_back({ actor->GetPosition(), kind, actor->GetFormID() });
            }
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

        // ---- what a map picture needs from the game, taken on the main thread
        struct World
        {
            RE::NiPoint3                 player;
            float                        tall = 128.0f;
            float                        heading = 0.0f;  // the character's
            float                        yaw = 0.0f;      // the map's up (camera or north)
            bool                         inside = false;
            RE::FormID                   space = 0;
            std::vector<Marker>          markers;
        };

        World TakeWorld(RE::PlayerCharacter* a_player)
        {
            World w;
            w.player = a_player->GetPosition();
            w.tall = a_player->GetHeight() > 1.0f ? a_player->GetHeight() : 128.0f;
            w.heading = a_player->GetAngleZ();
            w.yaw = CameraYaw(a_player);
            const auto cell = a_player->GetParentCell();
            w.inside = cell && cell->IsInteriorCell();
            w.space = MapMesh::CurrentSpace();
            w.markers = Markers(a_player);
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

        // ---- the minimap, built on the main thread, drawn by the HUD element
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
            bool                         round = false;
            float                        mx0 = 0, my0 = 0, mx1 = 0, my1 = 0;
            std::vector<ScreenIcon>      icons;
            float                        arrowAngle = 0.0f;
            float                        alpha = 1.0f;  // the whole minimap: fading in or out
            std::optional<MapMesh::View> view;
        };
        std::mutex frameLock;
        Frame      frame;

        void ClearFrame()
        {
            std::scoped_lock lock(frameLock);
            frame = {};
        }

        Frame BuildMinimap(const World& a_world, float a_w, float a_h)
        {
            const auto& s = Settings::Map();
            const float k = a_h / 1080.0f;
            const float size = s.minimapSize * k;
            Frame       f;
            f.has = true;
            f.mx1 = a_w - 18.0f * k;
            f.mx0 = f.mx1 - size;
            f.my0 = 18.0f * k;
            f.my1 = f.my0 + size;
            f.arrowAngle = a_world.heading - a_world.yaw;  // the character's heading relative to the map's up

            const Ortho o{ a_world.player, a_world.yaw, s.minimapTilt * kDeg, MinimapRange(a_world.inside), size, size };
            const auto  toScreen = [&](const RE::NiPoint3& a_p) {
                const auto p = o.ToScreen(a_p);
                return P2{ f.mx0 + p.first, f.my0 + p.second };
            };
            const float margin = 12.0f * k;
            const float cx = (f.mx0 + f.mx1) * 0.5f, cy = (f.my0 + f.my1) * 0.5f, rim = size * 0.5f;
            f.round = s.minimapRound;
            for (const auto& m : a_world.markers) {
                if (!Shown(m.kind)) {
                    continue;
                }
                const auto [x, y] = toScreen(m.pos);
                if (f.round) {
                    // round: the icons fade out over the last few px before the rim instead of being cut by it
                    const float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
                    const float edge = std::clamp((rim - 4.0f * k - d) / (10.0f * k), 0.0f, 1.0f);
                    if (edge > 0.0f) {
                        f.icons.push_back({ x, y, m.kind, m.alpha * edge });
                    }
                } else if (x > f.mx0 - margin && x < f.mx1 + margin && y > f.my0 - margin && y < f.my1 + margin) {
                    f.icons.push_back({ x, y, m.kind, m.alpha });
                }
            }
            auto view = MeshView(a_world, size, size);
            o.Fill(view);
            view.range = MinimapReach(a_world.inside);
            SetOccluder(view, a_world);
            f.view = view;
            return f;
        }

        // main thread, once a frame after the camera is final (the HUD advances after the camera update)
        void Update()
        {
            const auto  player = RE::PlayerCharacter::GetSingleton();
            const auto& s = Settings::Map();
            const float w = screenW, h = screenH;
            // the map off: nothing is done, the geometry unloads
            if (!s.enabled || !player || !player->Get3D() || !player->GetParentCell()) {
                ClearFrame();
                statics.clear();
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
            // shown / hidden by its key: a fade; hidden through, nothing is done (what is loaded stays)
            const float dt = std::clamp(RE::GetSecondsSinceLastFrame(), 0.0f, 0.1f);
            shown = std::clamp(shown + (s.visible ? dt : -dt) / kShowFade, 0.0f, 1.0f);
            if (shown <= 0.0f) {
                ClearFrame();
                MapMesh::Update(player, 0.0f, 0.0f);
                return;
            }
            // as far as the minimap's square shows
            const auto  here = player->GetParentCell();
            const float radius = MinimapReach(here->IsInteriorCell());
            markerRange = radius;
            sinceStatics += dt;
            iconClock += dt;
            if (sinceStatics >= kStaticEvery) {
                sinceStatics = 0.0f;
                GatherStatics(player);
            }
            MapMesh::Update(player, dt, radius);
            const auto world = TakeWorld(player);
            auto       next = BuildMinimap(world, w, h);
            next.alpha = shown * shown * (3.0f - 2.0f * shown);
            std::scoped_lock lock(frameLock);
            frame = std::move(next);
        }

        // ---- drawing (render thread)

        // a colour with its alpha scaled (the minimap fading in or out)
        ImGui::ImU32 Faded(ImGui::ImU32 a_col, float a_alpha)
        {
            const auto a = static_cast<ImGui::ImU32>(static_cast<float>(a_col >> 24) * std::clamp(a_alpha, 0.0f, 1.0f) + 0.5f);
            return (a_col & 0x00FFFFFFu) | (a << 24);
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
            const float size = 16.0f * a_k * Settings::Map().iconSize;
            for (const auto& i : a_f.icons) {
                Icons::Draw(a_dl, i.kind, i.x, i.y, size, i.alpha * a);
            }
            Icons::DrawPlayer(a_dl, (a_f.mx0 + a_f.mx1) * 0.5f, (a_f.my0 + a_f.my1) * 0.5f, 18.0f * a_k * Settings::Map().iconSize, a_f.arrowAngle, a);
            ImGui::ImDrawListManager::PopClipRect(a_dl);
            if (vanilla) {
                DrawVanillaFrame(a_dl, a_f.mx0, a_f.my0, a_f.mx1, a_f.my1, corner, a_f.round, a_k, a);
            } else if (a_f.round) {
                ImGui::ImDrawListManager::AddCircle(a_dl, centre, rim, Faded(Rgb(150, 120, 75), a), 96, 1.5f * a_k);
            } else {
                ImGui::ImDrawListManager::AddRect(a_dl, V2{ a_f.mx0, a_f.my0 }, V2{ a_f.mx1, a_f.my1 }, Faded(Rgb(150, 120, 75), a), corner, 0, 1.5f * a_k);
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
            if (ui->GameIsPaused() || !ui->IsShowingMenus()) {
                return;
            }
            const float k = io->DisplaySize.y / 1080.0f;
            // the background layer: under every window (SMF) and other HUD elements
            const auto dl = ImGui::GetBackgroundDrawList();
            Frame f;
            {
                std::scoped_lock lock(frameLock);
                f = frame;
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

        // the minimap's key during gameplay: shown / hidden (it fades), remembered in the ini
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
                if (captureSlot || !s.enabled || s.toggleKey == 0) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                for (auto event = a_event ? *a_event : nullptr; event; event = event->next) {
                    const auto button = event->AsButtonEvent();
                    if (button && button->IsDown() && button->GetDevice() == RE::INPUT_DEVICE::kKeyboard && button->GetIDCode() == s.toggleKey && InGameplay()) {
                        s.visible = !s.visible;
                        Settings::Save();
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
}

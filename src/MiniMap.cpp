#include "MiniMap.h"

#include "Icons.h"
#include "Lang.h"
#include "MapMesh.h"
#include "Overlay.h"
#include "Pathing.h"
#include "Settings.h"

// third-party header: deprecated <codecvt>, mixed enums and so on - not our warnings
#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
#pragma warning(push, 0)
#include <SKSEMenuFramework.h>
#pragma warning(pop)
#undef PlaySound  // Windows' macro, not RE::PlaySound

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
        constexpr RE::FormID        kMannequinRace = 0x0010760A;  // ManakinRace (Skyrim.esm)
        constexpr float             kShowFade = 0.3f;     // seconds the minimap takes to fade in or out (its key)
        std::atomic<std::uint32_t*> captureSlot = nullptr;  // the key setting being bound from the menu
        float                       shown = 0.0f;         // main thread: how much of the minimap shows, 0..1 (fading to Settings visible; in on the first frame)
        bool                        holdShown = false;    // ...its key held now (shown only while held: the setting)

        // the mouse, for the local map (main thread: input events and the map menu's update)
        bool mouseHeld = false;     // the left button down
        bool mouseClicked = false;  // ...pressed since the last local map update
        bool legendHeld = false;    // ...pressed on the legend: no dragging the map with it
        int  wheel = 0;          // wheel steps since the last map update (up: +)
        std::pair<float, float> leftStick{}, rightStick{};  // the gamepad's sticks (the local map: moved, zoomed)
        float saveIn = -1.0f;    // seconds until a zoomed minimap range is written to the ini
        std::atomic<std::uint32_t*> captureMod = nullptr;  // the bind being set: where the key held with it goes
        std::atomic<std::uint32_t>  captureFirst = 0;        // ...the first key pressed for it, waiting: let go alone, or another pressed with it
        std::atomic<std::uint32_t*> capturePad = nullptr;     // ...where a gamepad key goes instead (its own bind), and the key held with it
        std::atomic<std::uint32_t*> capturePadMod = nullptr;

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

        // a menu over the HUD: any open one that is not a part of it. Not just "the game paused": SkyrimSouls RE (and the
        // like) keep the game running under their menus, so a menu is told by its kind - the flags a menu of its own has
        // (a cursor, the menu controls, modal...), or the name of one of the game's whose pause flag such a mod takes off
        std::atomic_bool menuOver = false;  // main thread writes, the HUD element reads

        bool MenuOver()
        {
            using Flag = RE::UI_MENU_FLAGS;
            const auto ui = RE::UI::GetSingleton();
            if (!ui) {
                return true;
            }
            for (const auto& menu : ui->menuStack) {
                if (menu && menu->menuFlags.any(Flag::kPausesGame, Flag::kUsesCursor, Flag::kUsesMenuContext, Flag::kModal, Flag::kUpdateUsesCursor, Flag::kInventoryItemMenu,
                                Flag::kApplicationMenu, Flag::kFreezeFrameBackground, Flag::kDisablePauseMenu)) {
                    return true;
                }
            }
            static constexpr std::string_view kMenus[] = { "Book Menu", "Lockpicking Menu", "Sleep/Wait Menu", "Training Menu", "Tutorial Menu", "MessageBoxMenu", "Console",
                "Dialogue Menu", "MapMenu", "LevelUp Menu", "StatsMenu", "Journal Menu", "TweenMenu", "Loading Menu", "Main Menu", "RaceSex Menu", "Crafting Menu",
                "InventoryMenu", "ContainerMenu", "BarterMenu", "GiftMenu", "MagicMenu", "FavoritesMenu", "Mod Manager Menu", "Creation Club Menu", "Quantity Menu",
                "Credits Menu", "CustomMenu" };
            return std::ranges::any_of(kMenus, [&](std::string_view a_name) { return ui->IsMenuOpen(a_name); });
        }

        // the HUD is up: no menu over it, not loading, not hidden (by a scene, the console's tm)
        bool HudShown()
        {
            const auto ui = RE::UI::GetSingleton();
            menuOver = MenuOver();
            return ui && !ui->GameIsPaused() && !menuOver && ui->IsShowingMenus() && ui->IsMenuOpen(RE::HUDMenu::MENU_NAME) &&
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

        // an item one can take: not a leveled list (an inventory lists those of the form too - what it might hold,
        // not what it does) and playable
        bool Takeable(RE::TESBoundObject& a_object)
        {
            return !a_object.Is(RE::FormType::LeveledItem) && a_object.GetPlayable();
        }

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
                const auto items = a_body->GetInventory(Takeable);
                worth = std::ranges::any_of(items, [](const auto& a_item) { return a_item.second.first > 0; });
            }
            return worth;
        }

        // a container with something to take (looked at every 2 seconds at most). One the game has not filled yet (never
        // opened: its leveled lists not rolled) counts by what its form holds
        std::unordered_map<RE::FormID, std::pair<float, bool>> itemCache;  // per container: iconClock it was looked at, something in it

        bool HasLoot(RE::TESObjectREFR* a_ref, RE::TESBoundObject* a_base)
        {
            auto& [at, full] = itemCache.try_emplace(a_ref->GetFormID(), -100.0f, true).first->second;
            if (iconClock - at < 2.0f && iconClock >= at) {
                return full;
            }
            at = iconClock;
            if (!a_ref->extraList.HasType<RE::ExtraContainerChanges>()) {
                const auto container = a_base->As<RE::TESContainer>();
                full = false;
                if (container) {
                    container->ForEachContainerObject([&](RE::ContainerObject& a_entry) {
                        full = full || a_entry.count > 0;
                        return full ? RE::BSContainer::ForEachResult::kStop : RE::BSContainer::ForEachResult::kContinue;
                    });
                }
                return full;
            }
            const auto items = a_ref->GetInventory(Takeable);
            full = std::ranges::any_of(items, [](const auto& a_item) { return a_item.second.first > 0; });
            return full;
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

        // an ore vein with ore left: an activator running the game's MineOreScript (mods' veins use it too), its
        // ResourceCountCurrent not down to 0 (-1: never mined)
        bool OreLeft(RE::TESObjectREFR* a_ref)
        {
            const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            const auto policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
            if (!policy) {
                return false;
            }
            const auto handle = policy->GetHandleForObject(RE::TESObjectREFR::FORMTYPE, a_ref);
            if (handle == policy->EmptyHandle()) {
                return false;
            }
            RE::BSTSmartPointer<RE::BSScript::Object> script;
            if (!vm->FindBoundObject(handle, "MineOreScript", script) || !script) {
                return false;
            }
            const auto count = script->GetProperty("ResourceCountCurrent");
            return !count || !count->IsInt() || count->GetSInt() != 0;
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
                    if (merchants.contains(a_ref->GetFormID()) || (Settings::Map().hideEmpty && !HasLoot(a_ref, base))) {
                        return RE::BSContainer::ForEachResult::kContinue;
                    }
                    statics.push_back({ a_ref->GetPosition(), Kind::kContainer, a_ref->GetFormID() });
                } else if (base->Is(RE::FormType::Flora) || base->Is(RE::FormType::Tree)) {
                    if (Pickable(a_ref, base)) {
                        statics.push_back({ a_ref->GetPosition(), Kind::kFlora, a_ref->GetFormID() });
                    }
                } else if (base->Is(RE::FormType::Activator)) {
                    if (a_ref->Is3DLoaded() && OreLeft(a_ref)) {
                        statics.push_back({ a_ref->GetPosition(), Kind::kOre, a_ref->GetFormID() });
                    }
                } else if (a_ref->Is3DLoaded()) {
                    if (const auto kind = ItemKind(base)) {
                        statics.push_back({ a_ref->GetPosition(), *kind, a_ref->GetFormID() });
                    }
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }

        // an enemy: fighting the player or a follower right now and not running from them (a fox or a deer in "combat"
        // only flees; a guard turned on you fights), or, not in that fight yet, hostile and aggressive by nature (a
        // wolf that has not seen you; a fox is "hostile" too, but unaggressive)
        bool Threat(RE::Actor* a_actor, RE::PlayerCharacter* a_player)
        {
            const auto& rd = a_actor->GetActorRuntimeData();
            if (const auto target = rd.currentCombatTarget.get(); target && (target.get() == a_player || target->IsPlayerTeammate())) {
                const auto combat = rd.combatController;
                return !(combat && combat->state && combat->IsFleeing());
            }
            if (!a_actor->IsHostileToActor(a_player)) {
                return false;
            }
            const auto base = a_actor->GetActorBase();
            const bool aggressive = (base && base->GetAggressionLevel() != RE::ACTOR_AGGRESSION::kUnaggressive) ||
                                    a_actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kAggression) > 0.0f;
            return aggressive;
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
                // mannequins (Hearthfire homes, Breezehome): actors of their own race, standing there for the armour
                if (const auto race = actor->GetRace(); race && race->GetFormID() == kMannequinRace) {
                    continue;
                }
                Kind kind;
                if (Threat(actor, a_player)) {
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
            const auto& s = Settings::Map();
            // only what is near enough, when the icons' distance is set (it fades out past it)
            if (s.iconRange > 0.0f) {
                const auto at = a_player->GetPosition();
                std::erase_if(out, [&](const Marker& a_m) { return a_m.pos.GetDistance(at) > s.iconRange; });
            }
            // fade in what turned up; what is gone stays a moment longer, fading out
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
            std::string         name;            // the quest's
            RE::FormID          quest = 0;
            bool                active = false;  // set active in the journal (the compass shows it)
            float               beyond = 0.0f;   // a target past doors: the way on from the door here to it (straight, world units)
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

        // where a door on a quest target's way lets out: its own teleport's marker, else what the way says
        RE::NiPoint3 ExitOf(const RE::TeleportPath::TeleportLink& a_link)
        {
            if (const auto tele = a_link.ref ? a_link.ref->extraList.GetByType<RE::ExtraTeleport>() : nullptr; tele && tele->teleportData) {
                return tele->teleportData->position;
            }
            return a_link.teleportLocation;
        }

        void GatherQuests(RE::PlayerCharacter* a_player)
        {
            quests.clear();
            // taken under the player's lock, resolved after it (resolving takes the quests' own locks)
            std::vector<std::pair<RE::TESQuest*, RE::TESQuestTarget*>> targets;
            {
                RE::BSSpinLockGuard guard(a_player->GetQuestTargetsLock());
                for (const auto& [quest, list] : a_player->GetQuestTargets()) {
                    if (!quest || !list || !quest->IsActive()) {  // only the quests set active in the journal (as the compass shows them)
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
                float beyond = 0.0f;
                if (held && HereToo(held.get(), a_player)) {
                    at = held.get();
                } else {
                    // the first door on the way that is here; past it the way goes on: from where each door lets out to
                    // the next door, and from the last to the target (straight, a space at a time)
                    const auto& links = target->teleportPath.teleportRefs;
                    for (std::uint32_t i = 0; i < links.size(); ++i) {
                        if (!links[i].ref || !HereToo(links[i].ref, a_player)) {
                            continue;
                        }
                        at = links[i].ref;
                        for (std::uint32_t j = i; j < links.size(); ++j) {
                            const auto next = j + 1 < links.size() && links[j + 1].ref ? links[j + 1].ref->GetPosition() :
                                              held                                    ? held->GetPosition() :
                                                                                        target->teleportPath.end;
                            beyond += ExitOf(links[j]).GetDistance(next);
                        }
                        break;
                    }
                }
                if (at) {
                    quests.push_back({ at->CreateRefHandle(), at->GetPosition(), quest->GetFullName() ? quest->GetFullName() : "", quest->GetFormID(), quest->IsActive(), beyond });
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
            std::vector<RE::NiPoint3> beams;  // where the beams go
        };

        // the beams' targets: one a quest set active in the journal, to its nearest target; or (the setting) one beam
        // only, to the nearest target of any quest. Nearest by the whole way: to where it is shown here (the target, or
        // the door towards it) and on past the doors to the target itself - a door close by to a far place is far
        std::vector<RE::NiPoint3> BeamTargets(const RE::NiPoint3& a_player)
        {
            std::vector<RE::NiPoint3> out;
            if (!Settings::Map().questBeam) {
                return out;
            }
            const auto distance = [&](const QuestPoint& a_q) {
                const float dx = a_q.pos.x - a_player.x, dy = a_q.pos.y - a_player.y;
                return std::sqrt(dx * dx + dy * dy) + a_q.beyond;
            };
            std::map<RE::FormID, const QuestPoint*> nearest;  // per quest, or (one beam) all under 0
            const bool                              one = Settings::Map().beamNearest;
            for (const auto& q : quests) {
                if (!one && !q.active) {
                    continue;
                }
                auto& best = nearest[one ? 0 : q.quest];
                if (!best || distance(q) < distance(*best)) {
                    best = &q;
                }
            }
            for (const auto& [quest, q] : nearest) {
                out.push_back(q->pos);
            }
            return out;
        }

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
                if (const float range = Settings::Map().iconRange; range > 0.0f) {
                    std::erase_if(w.markers, [&](const Marker& a_m) { return a_m.pos.GetDistance(w.player) > range; });
                }
            } else {
                w.markers = Markers(a_player);
            }
            for (auto& q : quests) {
                if (const auto ref = q.ref.get()) {
                    q.pos = ref->GetPosition();
                }
                w.quests.push_back(q.pos);
            }
            w.beams = BeamTargets(w.player);
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
            // inside the ceiling goes (everything this far over the feet); outside nothing is cut
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

        // ---- a map picture with its markers, built on the main thread, drawn by the HUD element
        using P2 = std::pair<float, float>;

        struct ScreenIcon
        {
            float x, y;
            Kind  kind;
            float alpha;
            float      shade = 1.0f;  // darker on another level than the character's
            RE::FormID id = 0;         // the reference (a quest target: its index in quests)
            int        count = 1;      // icons of its kind merged into it (close together on the picture)
            float      wx = 0, wy = 0; // where it is in the world (grouped by: the same whichever way the map turns)
        };
        // on another level than the character (an upper floor, a cellar, a ledge above): dimmer the farther up or down
        float LevelShade(float a_dz)
        {
            return 1.0f - 0.6f * std::clamp((std::abs(a_dz) - 200.0f) / 400.0f, 0.0f, 1.0f);
        }

        struct Frame
        {
            bool                         has = false;
            bool                         local = false;  // the local map (in the map menu), not the minimap
            bool                         round = false;
            float                        mx0 = 0, my0 = 0, mx1 = 0, my1 = 0;  // the picture on the screen
            float                        px = 0, py = 0;                      // the character on it
            std::vector<ScreenIcon>      icons;
            std::vector<ScreenIcon>      quests;         // quest targets (pulled onto the rim when past it)
            std::vector<std::vector<P2>> beams;          // the beams to the quest targets (each from the character)
            float                        arrowAngle = 0.0f;
            float                        alpha = 1.0f;   // the whole map: fading in or out
            float                        cursorX = -1.0f, cursorY = -1.0f;  // the local map: the menu's cursor (drawn over the map)
            std::string                  title;          // the local map: where the character is
            int                          hover = -1;     // the local map: the icon under the cursor (in icons, or in quests)
            bool                         hoverQuest = false;
            std::string                  hoverName;      // ...and what it is (a door: where it leads)
            std::optional<MapMesh::View> view;
        };
        std::mutex frameLock;
        Frame      frame;       // the minimap
        Frame      localFrame;  // the local map

        // ---- the beams' ways: the walking path over the navmesh (Pathing) while it is for that target and still runs
        // by the character, else straight (main thread)
        std::vector<Pathing::Found> pathsNow;  // the last paths found, with their targets
        float                       sincePath = 1.0f;

        float ToSegment2D(const RE::NiPoint3& a_p, const RE::NiPoint3& a_a, const RE::NiPoint3& a_b)
        {
            const float dx = a_b.x - a_a.x, dy = a_b.y - a_a.y, len = dx * dx + dy * dy;
            const float t = len > 0.0f ? std::clamp(((a_p.x - a_a.x) * dx + (a_p.y - a_a.y) * dy) / len, 0.0f, 1.0f) : 0.0f;
            const float x = a_a.x + dx * t - a_p.x, y = a_a.y + dy * t - a_p.y;
            return std::sqrt(x * x + y * y);
        }

        // a way from where the character is now: the rest of a_path from its leg nearest to the character (among the
        // first a_legs), then straight on to the target where the way came short of it; none: the character left it
        std::optional<std::vector<RE::NiPoint3>> Along(const RE::NiPoint3& a_player, const std::vector<RE::NiPoint3>& a_path, std::size_t a_legs, const RE::NiPoint3& a_target)
        {
            std::size_t leg = 0;
            float       best = FLT_MAX;
            for (std::size_t i = 0; i + 1 < a_path.size() && i < a_legs; ++i) {
                if (const float d = ToSegment2D(a_player, a_path[i], a_path[i + 1]); d < best) {
                    best = d;
                    leg = i;
                }
            }
            if (best >= 400.0f) {
                return std::nullopt;
            }
            std::vector<RE::NiPoint3> out{ a_player };
            out.insert(out.end(), a_path.begin() + static_cast<std::ptrdiff_t>(leg) + 1, a_path.end());
            if (out.back().GetDistance(a_target) > 1.0f) {
                out.push_back(a_target);
            }
            return out;
        }

        std::vector<RE::NiPoint3> BeamWay(const RE::NiPoint3& a_player, const RE::NiPoint3& a_target)
        {
            for (const auto& [target, path] : pathsNow) {
                if (path.size() < 2 || target.GetDistance(a_target) >= 200.0f) {
                    continue;
                }
                if (auto way = Along(a_player, path, 8, a_target)) {
                    return std::move(*way);
                }
                break;
            }
            return { a_player, a_target };
        }

        // main thread: the navmeshes kept up to date and new paths asked for twice a second
        void UpdatePath(RE::PlayerCharacter* a_player, float a_dt, bool a_now)
        {
            if (!Settings::Map().questBeam || quests.empty()) {
                return;
            }
            sincePath += a_dt;
            if (sincePath >= 0.5f || a_now) {
                sincePath = 0.0f;
                Pathing::Update(a_player);
                Pathing::Request(a_player->GetPosition(), BeamTargets(a_player->GetPosition()));
            }
            pathsNow = Pathing::Paths();
        }

        void ClearFrame()
        {
            std::scoped_lock lock(frameLock);
            frame = {};
        }

        // icons of one kind close together (a room of barrels) merged into one at their middle, bigger the more it
        // holds; zoomed in they part again (the setting). Close in the world, by the map's scale (a_pxPerUnit): the
        // same groups whichever way a tilted map turns (on the screen the distances change as it turns - icons would
        // join and part all the time); measured from each group's first icon, so a group does not creep
        void GroupIcons(std::vector<ScreenIcon>& a_icons, float a_pxPerUnit)
        {
            const auto& s = Settings::Map();
            if (!s.groupIcons || a_icons.size() < 2 || a_pxPerUnit <= 0.0f) {
                return;
            }
            const float reach = 14.0f * (screenH / 1080.0f) * s.iconSize / a_pxPerUnit;  // about an icon across, in world units
            std::vector<ScreenIcon> out;
            std::vector<P2>         first;  // per group: its first icon in the world
            std::vector<P2>         sum;    // per group: the positions on the picture added up
            for (const auto& i : a_icons) {
                std::size_t at = out.size();
                for (std::size_t g = 0; g < out.size(); ++g) {
                    const float dx = first[g].first - i.wx, dy = first[g].second - i.wy;
                    if (out[g].kind == i.kind && dx * dx + dy * dy < reach * reach) {
                        at = g;
                        break;
                    }
                }
                if (at == out.size()) {
                    out.push_back(i);
                    first.push_back({ i.wx, i.wy });
                    sum.push_back({ i.x, i.y });
                    continue;
                }
                auto& o = out[at];
                ++o.count;
                sum[at].first += i.x;
                sum[at].second += i.y;
                o.x = sum[at].first / static_cast<float>(o.count);
                o.y = sum[at].second / static_cast<float>(o.count);
                o.alpha = std::max(o.alpha, i.alpha);
                o.shade = std::max(o.shade, i.shade);
            }
            a_icons = std::move(out);
        }

        // the markers, the quest targets and the beams of a picture (a_pxPerUnit: its scale); a_inside(x, y): on the picture, a_rim(x, y): pulled
        // in to its edge along the line from the character
        template <class ToScreen, class Inside, class Rim>
        void PlaceMarkers(Frame& a_f, const World& a_world, float a_pxPerUnit, ToScreen&& a_toScreen, Inside&& a_inside, Rim&& a_rim)
        {
            for (const auto& m : a_world.markers) {
                if (!Shown(m.kind)) {
                    continue;
                }
                const auto [x, y] = a_toScreen(m.pos);
                if (const float edge = a_inside(x, y); edge > 0.0f) {
                    a_f.icons.push_back({ x, y, m.kind, m.alpha * edge, LevelShade(m.pos.z - a_world.player.z), m.id, 1, m.pos.x, m.pos.y });
                }
            }
            GroupIcons(a_f.icons, a_pxPerUnit);
            if (Shown(Kind::kQuest)) {
                for (std::size_t i = 0; i < a_world.quests.size(); ++i) {
                    const auto& q = a_world.quests[i];
                    const auto [x, y] = a_rim(a_toScreen(q));
                    a_f.quests.push_back({ x, y, Kind::kQuest, 1.0f, LevelShade(q.z - a_world.player.z), static_cast<RE::FormID>(i) });
                }
            }
            // the beams: along their ways, on the screen, ending where they leave the picture
            for (const auto& target : a_world.beams) {
                const auto      way = BeamWay(a_world.player, target);
                std::vector<P2> line;
                P2              prev = a_toScreen(way[0]);
                const bool      clip = a_inside(prev.first, prev.second) > 0.0f;  // the character off the picture: the picture's own clip does
                line.push_back(prev);
                for (std::size_t i = 1; i < way.size(); ++i) {
                    const P2 next = a_toScreen(way[i]);
                    if (!clip || a_inside(next.first, next.second) > 0.0f) {
                        line.push_back(next);
                        prev = next;
                        continue;
                    }
                    P2 in = prev, out = next;  // the edge between them
                    for (int k = 0; k < 14; ++k) {
                        const P2 mid{ (in.first + out.first) * 0.5f, (in.second + out.second) * 0.5f };
                        (a_inside(mid.first, mid.second) > 0.0f ? in : out) = mid;
                    }
                    line.push_back(in);
                    break;
                }
                if (line.size() >= 2) {
                    a_f.beams.push_back(std::move(line));
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
                f, a_world, o.Scale(),
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
            f.view = view;
            return f;
        }

        // ---- the local map: in the map menu, over the game's own; dragged with the mouse, zoomed with the wheel
        struct Local
        {
            bool         open = false;
            RE::NiPoint3 centre;
            float        range = 0.0f;  // world units from the centre to the top edge
            float        lastX = 0.0f, lastY = 0.0f;  // the cursor last frame
            // the mouse's own movement to cursor pixels, per axis (sign and speed), learned while the cursor moves
            // freely: dragging goes by the mouse, not by the cursor (the menu holds or clamps it meanwhile)
            float        gainX = 1.0f, gainY = 1.0f;
            std::chrono::steady_clock::time_point last;   // the last update (the game is paused: its clock stands)
        };
        Local local;
        float rawX = 0.0f, rawY = 0.0f;  // main thread: the mouse's movement since the last map update
        // keys moving the local map, held: W A S D and the arrows
        std::array<bool, 4> panKeys{};    // up, left, down, right

        int PanKey(std::uint32_t a_key)
        {
            switch (a_key) {
            case 17:
            case 200:
                return 0;
            case 30:
            case 203:
                return 1;
            case 31:
            case 208:
                return 2;
            case 32:
            case 205:
                return 3;
            default:
                return -1;
            }
        }

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

        // the local map's picture on the screen: all of it
        void LocalRect(float a_w, float a_h, float& a_x0, float& a_y0, float& a_x1, float& a_y1)
        {
            a_x0 = 0.0f;
            a_x1 = a_w;
            a_y0 = 0.0f;
            a_y1 = a_h;
        }

        // the local map's legend: a row an icon kind down its right side, clicked to show / hide that kind
        struct Legend
        {
            float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // the panel
            float top = 0, row = 0;                 // the first row's top, a row's height

            bool In(float a_x, float a_y) const { return a_x >= x0 && a_x <= x1 && a_y >= y0 && a_y <= y1; }
            // the row (icon kind) at a point, -1 = none
            int At(float a_x, float a_y) const
            {
                if (!In(a_x, a_y) || a_y < top) {
                    return -1;
                }
                const int i = static_cast<int>((a_y - top) / row);
                return i < static_cast<int>(Icons::kCount) ? i : -1;
            }
        };

        Legend LegendOf(float a_mx1, float a_my0, float a_k)
        {
            Legend l;
            l.row = 30.0f * a_k;
            l.x1 = a_mx1 - 28.0f * a_k;
            l.x0 = l.x1 - 250.0f * a_k;
            l.y0 = a_my0 + 90.0f * a_k;
            l.top = l.y0 + 44.0f * a_k;  // under its title
            l.y1 = l.top + l.row * static_cast<float>(Icons::kCount) + 10.0f * a_k;
            return l;
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
                f, a_world, o.Scale(), toScreen, [&](float x, float y) { return x > f.mx0 && x < f.mx1 && y > f.my0 && y < f.my1 ? 1.0f : 0.0f; },
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
            view.slot = 1;
            f.view = view;
            std::tie(f.cursorX, f.cursorY) = Cursor(a_w, a_h);
            f.title = PlaceName(a_player);
            return f;
        }

        bool vanillaHidden = false;  // the game's local map clip hidden by us

        // our local map is a menu of its own (it pauses the game, the game's cursor over it): opened by its key from the
        // game, or instead of the game's own when the map menu switches to that
        constexpr std::string_view kLocalMenu = "DetailedMiniMap LocalMap";

        bool LocalMenuOpen()
        {
            const auto ui = RE::UI::GetSingleton();
            return ui && ui->IsMenuOpen(kLocalMenu);
        }

        // ours opened over the map menu's world map (L there): closed by L it goes back to it, the map menu stays
        bool overWorld = false;
        bool localHeld = false;  // ours opened from the game by its key held (the setting): closed when it is let go
        bool openAsked = false;  // shown, not up yet (the map menu's switch and the key may both ask in one frame)

        void OpenLocalMap(bool a_overWorld = false)
        {
            if (openAsked || LocalMenuOpen()) {
                return;
            }
            openAsked = true;
            overWorld = a_overWorld;
            if (const auto queue = RE::UIMessageQueue::GetSingleton()) {
                queue->AddMessage(kLocalMenu, RE::UI_MESSAGE_TYPE::kShow, nullptr);
            }
        }

        // what a reference is called on the map: a door by where it leads, anything else by its name
        std::string RefName(RE::FormID a_id)
        {
            const auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_id);
            if (!ref) {
                return {};
            }
            const auto named = [](const char* a_name) { return a_name && *a_name; };
            if (const auto tele = ref->extraList.GetByType<RE::ExtraTeleport>(); tele && tele->teleportData) {
                if (const auto linked = tele->teleportData->linkedDoor.get()) {
                    const auto cell = linked->GetParentCell();
                    if (cell && cell->IsInteriorCell() && named(cell->GetFullName())) {
                        return cell->GetFullName();
                    }
                    if (const auto location = linked->GetCurrentLocation(); location && named(location->GetFullName())) {
                        return location->GetFullName();
                    }
                    if (const auto world = linked->GetWorldspace(); world && named(world->GetFullName())) {
                        return world->GetFullName();
                    }
                }
            }
            const auto name = ref->GetDisplayFullName();
            return named(name) ? name : "";
        }

        // the icon under the cursor (the nearest within its badge) and its name
        void Hover(Frame& a_f, float a_x, float a_y, float a_k)
        {
            const float reach = 11.0f * a_k * Settings::Map().iconSize;
            float       best = reach * reach;
            const auto  look = [&](const std::vector<ScreenIcon>& a_list, bool a_quest) {
                for (std::size_t i = 0; i < a_list.size(); ++i) {
                    const float dx = a_list[i].x - a_x, dy = a_list[i].y - a_y, d = dx * dx + dy * dy;
                    if (d < best) {
                        best = d;
                        a_f.hover = static_cast<int>(i);
                        a_f.hoverQuest = a_quest;
                    }
                }
            };
            look(a_f.icons, false);
            look(a_f.quests, true);
            if (a_f.hover < 0) {
                return;
            }
            if (a_f.hoverQuest) {
                const auto at = a_f.quests[a_f.hover].id;
                a_f.hoverName = at < quests.size() ? quests[at].name : "";
            } else {
                const auto& icon = a_f.icons[a_f.hover];
                a_f.hoverName = icon.count > 1 ? std::format("{} ({})", Icons::Name(icon.kind), icon.count) : RefName(icon.id);  // a group: its kind and how many
            }
        }

        void CloseLocal()
        {
            local.open = false;
            overWorld = false;
            openAsked = false;
            std::scoped_lock lock(frameLock);
            localFrame = {};
        }

        // main thread, every frame of the map menu: switched to the game's local map, it closes and ours opens instead
        // (the game's then never runs: no markers of its own picked under the cursor behind ours)
        bool switching = false;
        // the world map asked for from ours (M): the map menu opens on the local map inside, so it is switched to the
        // world map (what the game's own World Map button does) instead of handing over to ours again
        bool worldWanted = false;
        int  worldWait = 0;          // frames the switch has been waited for
        bool mapSawWorld = false;    // the map menu has shown its world map since it opened (cleared when it closes)

        void ToWorldMap(RE::MapMenu* a_menu)
        {
            const auto   movie = a_menu->uiMovie;
            RE::GFxValue delegate;
            if (!movie || !movie->GetVariable(&delegate, "_global.gfx.io.GameDelegate") || !delegate.IsObject()) {
                logger::warn("local map: the map menu has no GameDelegate - no world map switch");
                return;
            }
            RE::GFxValue args[2];
            args[0] = "ToggleMapCallback";
            movie->CreateArray(&args[1]);
            delegate.Invoke("call", nullptr, args, 2);
        }

        void UpdateMapMenu(RE::MapMenu* a_menu)
        {
            const auto& s = Settings::Map();
            const auto  data = a_menu ? a_menu->GetRuntimeData() : nullptr;
            const bool  ours = s.enabled && s.localMap;
            const bool  showing = ours && data && data->localMapMenu.GetRuntimeData().showingMap;
            // the game's own local map not shown at all while ours replaces it (back when ours is switched off)
            if (data) {
                auto& movie = data->localMapMenu.GetRuntimeData().localMapMovie;
                if (movie.IsDisplayObject() && (ours || vanillaHidden)) {
                    movie.SetMember("_visible", RE::GFxValue(!ours));
                    if (ours && !vanillaHidden) {
                        RE::GFxValue name;
                        movie.GetMember("_name", &name);
                        logger::info("local map: the game's own ('{}') hidden", name.IsString() ? name.GetString() : "?");
                    }
                    vanillaHidden = ours;
                }
            }
            if (showing && worldWanted) {
                if (worldWait++ == 0) {
                    ToWorldMap(a_menu);
                } else if (worldWait > 30) {
                    worldWanted = false;  // it did not switch: ours after all
                }
                return;
            }
            worldWanted = false;
            worldWait = 0;
            if (showing && !switching) {
                switching = true;
                if (mapSawWorld) {
                    // switched from the world map: ours over it, the map menu back on its world map underneath
                    ToWorldMap(a_menu);
                    OpenLocalMap(true);
                } else {
                    // opened on the local map (inside): ours instead of the map menu
                    if (const auto queue = RE::UIMessageQueue::GetSingleton()) {
                        queue->AddMessage(RE::MapMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
                    }
                    OpenLocalMap();
                }
            } else if (!showing) {
                switching = false;
                mapSawWorld = mapSawWorld || data;
            }
        }

        // main thread, every frame of our local map menu
        void UpdateLocal()
        {
            const auto& s = Settings::Map();
            const auto  player = RE::PlayerCharacter::GetSingleton();
            const float w = screenW, h = screenH;
            if (!overWorld) {  // over the map menu: it is still switching back to its world map
                switching = false;
            }
            openAsked = false;  // up
            if (!s.enabled || !player || !player->GetParentCell() || w <= 0.0f || h <= 0.0f) {
                if (local.open) {
                    CloseLocal();
                }
                return;
            }
            const bool inside = player->GetParentCell()->IsInteriorCell();
            const auto [cx, cy] = Cursor(w, h);
            const auto now = std::chrono::steady_clock::now();
            if (!local.open) {
                // opened: on the character, as wide as the minimap shows twice
                local.open = true;
                local.centre = player->GetPosition();
                local.range = std::clamp(MinimapRange(inside) * 2.0f, 600.0f, 16000.0f);
                local.lastX = cx;
                local.lastY = cy;
                local.last = now;
                wheel = 0;
                rawX = rawY = 0.0f;
                panKeys = {};
                leftStick = rightStick = {};
                markerRange = inside ? std::max(MinimapReach(true) * 4.0f, 6000.0f) : kLocalReach;
                GatherStatics(player);
                GatherQuests(player);
                UpdatePath(player, 0.0f, true);
                logger::info("local map: opened at {:.0f} {:.0f}, cursor {:.0f} {:.0f} on a {:.0f} x {:.0f} screen", local.centre.x, local.centre.y, cx, cy, w, h);
            }
            const float dt = std::clamp(std::chrono::duration<float>(now - local.last).count(), 0.0f, 0.1f);
            local.last = now;
            pathsNow = Pathing::Paths();  // the ones asked for on opening, once found
            float x0, y0, x1, y1;
            LocalRect(w, h, x0, y0, x1, y1);
            const float st = std::max(std::sin(s.minimapTilt * kDeg), 0.3f);
            // a click on the legend shows / hides that kind of icon (and the press drags nothing)
            const auto legend = LegendOf(x1, y0, h / 1080.0f);
            if (mouseClicked && legend.In(cx, cy)) {
                legendHeld = true;
                if (const int i = legend.At(cx, cy); i >= 0) {
                    auto& show = Settings::Map().show[i];
                    show = !show;
                    Settings::Save();
                    RE::PlaySound("UIMenuFocus");
                }
            }
            mouseClicked = false;
            legendHeld = legendHeld && mouseHeld;
            // the wheel zooms (up: in) towards the cursor: the point under it stays where it is
            if (wheel != 0) {
                const float before = (y1 - y0) * 0.5f / local.range;
                local.range = std::clamp(local.range * std::pow(0.85f, static_cast<float>(wheel)), 300.0f, 20000.0f);
                const float after = (y1 - y0) * 0.5f / local.range;
                if (cx >= x0 && cx <= x1 && cy >= y0 && cy <= y1) {
                    const float ox = cx - (x0 + x1) * 0.5f, oy = cy - (y0 + y1) * 0.5f;
                    local.centre.x += ox / before - ox / after;
                    local.centre.y -= (oy / before - oy / after) / st;
                }
                wheel = 0;
            }
            const float scale = (y1 - y0) * 0.5f / local.range;
            // the mouse's movement in cursor pixels: learned from frames the cursor follows it freely
            const float cdx = cx - local.lastX, cdy = cy - local.lastY;
            const bool  free = !mouseHeld && cx > 2.0f && cx < w - 2.0f && cy > 2.0f && cy < h - 2.0f;
            const auto  learn = [&](float& a_gain, float a_cursor, float a_raw) {
                if (free && std::abs(a_raw) >= 2.0f && std::abs(a_cursor) >= 1.0f) {
                    const float g = std::clamp(a_cursor / a_raw, -10.0f, 10.0f);
                    a_gain = std::abs(g) >= 0.05f ? a_gain * 0.8f + g * 0.2f : a_gain;
                }
            };
            learn(local.gainX, cdx, rawX);
            learn(local.gainY, cdy, rawY);
            // dragging moves the map with the mouse; the keys move it a screen's half a second
            float dx = 0.0f, dy = 0.0f;
            if (mouseHeld && !legendHeld) {
                dx += rawX * local.gainX;
                dy += rawY * local.gainY;
            }
            const float keyStep = (y1 - y0) * 0.9f * dt;
            dx += (panKeys[1] ? keyStep : 0.0f) - (panKeys[3] ? keyStep : 0.0f);
            dy += (panKeys[0] ? keyStep : 0.0f) - (panKeys[2] ? keyStep : 0.0f);
            // the gamepad: the left stick moves it, the right one zooms (up: in)
            const auto stick = [](float a_v) { return std::abs(a_v) > 0.15f ? a_v : 0.0f; };
            dx -= stick(leftStick.first) * keyStep * 1.2f;
            dy += stick(leftStick.second) * keyStep * 1.2f;
            if (const float zoom = stick(rightStick.second); zoom != 0.0f) {
                local.range = std::clamp(local.range * std::pow(0.85f, zoom * 8.0f * dt), 300.0f, 20000.0f);
            }
            if (dx != 0.0f || dy != 0.0f) {
                local.centre.x -= dx / scale;
                local.centre.y += dy / (scale * st);
                // never farther from the character than the loaded world reaches
                const auto  p = player->GetPosition();
                const float reach = inside ? 20000.0f : kLocalReach;
                local.centre.x = std::clamp(local.centre.x, p.x - reach, p.x + reach);
                local.centre.y = std::clamp(local.centre.y, p.y - reach, p.y + reach);
            }
            rawX = rawY = 0.0f;
            local.lastX = cx;
            local.lastY = cy;
            const auto world = TakeWorld(player, true);
            auto       next = BuildLocal(player, world, w, h);
            if (cx >= next.mx0 && cx <= next.mx1 && cy >= next.my0 && cy <= next.my1 && !mouseHeld && !legend.In(cx, cy)) {
                Hover(next, cx, cy, h / 1080.0f);
            }
            std::scoped_lock lock(frameLock);
            localFrame = std::move(next);
        }

        // main thread, once a frame after the camera is final (the HUD advances after the camera update)
        void Update()
        {
            const auto  start = std::chrono::steady_clock::now();
            // a zoomed range written once the zooming stopped
            if (saveIn > 0.0f && (saveIn -= std::clamp(RE::GetSecondsSinceLastFrame(), 0.0f, 0.1f)) <= 0.0f) {
                Settings::Save();
            }
            const auto  player = RE::PlayerCharacter::GetSingleton();
            const auto& s = Settings::Map();
            const float w = screenW, h = screenH;
            if (local.open && !LocalMenuOpen()) {
                CloseLocal();  // the map menu is closed
            }
            // the map off: nothing is done, the geometry unloads
            if (!s.enabled || !player || !player->Get3D() || !player->GetParentCell()) {
                ClearFrame();
                statics.clear();
                quests.clear();
                seen.clear();
                Pathing::Clear();
                pathsNow.clear();
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
            const bool want = s.toggleHold ? holdShown : s.visible;  // held to show, or switched
            shown = std::clamp(shown + (want ? dt : -dt) / kShowFade, 0.0f, 1.0f);
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
            UpdatePath(player, dt, false);
            const auto world = TakeWorld(player, false);
            auto       next = BuildMinimap(world, w, h);
            next.alpha = shown * shown * (3.0f - 2.0f * shown) * s.minimapOpacity;  // the fade, times the opacity set
            {
                std::scoped_lock lock(frameLock);
                frame = std::move(next);
            }
            if (s.debugLog) {
                MapMesh::NoteFrame(static_cast<float>(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()));
            }
        }

        // ---- drawing (render thread; the local map's own: the main thread, inside the map menu's drawing)

        // a colour with its alpha scaled (the minimap fading in or out)
        ImGui::ImU32 Faded(ImGui::ImU32 a_col, float a_alpha)
        {
            const auto a = static_cast<ImGui::ImU32>(static_cast<float>(a_col >> 24) * std::clamp(a_alpha, 0.0f, 1.0f) + 0.5f);
            return (a_col & 0x00FFFFFFu) | (a << 24);
        }

        float Seconds()  // the beam's running light
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

        // the beam: along its way from the character to the quest target, a soft glow with a bright core and a light
        // running along it
        void DrawBeam(Canvas& a_c, const std::vector<P2>& a_pts, float a_k, float a_alpha)
        {
            const auto& pts = a_pts;
            // its length along the way, and where it starts (past the character's badge) and ends (short of the
            // target's icon)
            std::vector<float> at{ 0.0f };
            for (std::size_t i = 1; i < pts.size(); ++i) {
                const float dx = pts[i].first - pts[i - 1].first, dy = pts[i].second - pts[i - 1].second;
                at.push_back(at.back() + std::sqrt(dx * dx + dy * dy));
            }
            const float from = 15.0f * a_k * Settings::Map().iconSize;
            const float to = at.back() - 9.0f * a_k * Settings::Map().iconSize;
            if (to - from < 6.0f * a_k) {
                return;
            }
            const auto point = [&](float a_d) {
                std::size_t i = 1;
                while (i + 1 < pts.size() && at[i] < a_d) {
                    ++i;
                }
                const float t = at[i] > at[i - 1] ? std::clamp((a_d - at[i - 1]) / (at[i] - at[i - 1]), 0.0f, 1.0f) : 0.0f;
                return Canvas::V2{ pts[i - 1].first + (pts[i].first - pts[i - 1].first) * t, pts[i - 1].second + (pts[i].second - pts[i - 1].second) * t };
            };
            // the legs between from and to, joints rounded
            std::vector<Canvas::V2> legs{ point(from) };
            for (std::size_t i = 1; i + 1 < pts.size(); ++i) {
                if (at[i] > from && at[i] < to) {
                    legs.push_back({ pts[i].first, pts[i].second });
                }
            }
            legs.push_back(point(to));
            const auto glow = Faded(Rgb(255, 196, 80, 60), a_alpha), core = Faded(Rgb(255, 222, 140, 215), a_alpha);
            for (std::size_t i = 1; i < legs.size(); ++i) {
                a_c.Line(legs[i - 1], legs[i], glow, 8.0f * a_k);
            }
            for (std::size_t i = 1; i + 1 < legs.size(); ++i) {
                a_c.Disc(legs[i], 4.0f * a_k, glow);
            }
            for (std::size_t i = 1; i < legs.size(); ++i) {
                a_c.Line(legs[i - 1], legs[i], core, 2.2f * a_k);
            }
            for (std::size_t i = 1; i + 1 < legs.size(); ++i) {
                a_c.Disc(legs[i], 1.1f * a_k, core);
            }
            const float step = 24.0f * a_k;
            for (float d = from + std::fmod(Seconds() * 55.0f * a_k, step); d < to; d += step) {
                // brightest in the middle of the beam, fading at its ends
                const float t = std::min(d - from, to - d) / (12.0f * a_k);
                a_c.Disc(point(d), 2.3f * a_k, Faded(Rgb(255, 245, 205, 235), a_alpha * std::clamp(t, 0.0f, 1.0f)));
            }
        }

        // the icons, the quest targets, the beam and the character over a picture
        void DrawMarkers(Canvas& a_c, const Frame& a_f, float a_k)
        {
            const auto& s = Settings::Map();
            const float a = a_f.alpha;
            for (const auto& beam : a_f.beams) {
                DrawBeam(a_c, beam, a_k, a);
            }
            const float size = 16.0f * a_k * s.iconSize;
            for (const auto& i : a_f.icons) {
                const float grown = i.count > 1 ? std::min(1.0f + 0.18f * std::log2(static_cast<float>(i.count)), 1.6f) : 1.0f;  // a group: bigger the more
                Icons::Draw(a_c, i.kind, i.x, i.y, size * grown, i.alpha * a, i.shade);
            }
            for (const auto& q : a_f.quests) {
                Icons::Draw(a_c, q.kind, q.x, q.y, size * 1.15f, q.alpha * a, q.shade);
            }
            Icons::DrawPlayer(a_c, a_f.px, a_f.py, 18.0f * a_k * s.iconSize, a_f.arrowAngle, a);
            // the one under the cursor: larger, on top, at full light
            const auto& list = a_f.hoverQuest ? a_f.quests : a_f.icons;
            if (a_f.hover >= 0 && static_cast<std::size_t>(a_f.hover) < list.size()) {
                const auto& i = list[a_f.hover];
                Icons::Draw(a_c, i.kind, i.x, i.y, size * 1.45f, a);
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
            ImGuiCanvas canvas(a_dl);
            DrawMarkers(canvas, a_f, a_k);
            ImGui::ImDrawListManager::PopClipRect(a_dl);
            if (!Settings::Map().minimapFrame) {
                // no frame: the picture's own edge only
            } else if (vanilla) {
                DrawVanillaFrame(a_dl, a_f.mx0, a_f.my0, a_f.mx1, a_f.my1, corner, a_f.round, a_k, a);
            } else if (a_f.round) {
                ImGui::ImDrawListManager::AddCircle(a_dl, centre, rim, Faded(Rgb(150, 120, 75), a), 96, 1.5f * a_k);
            } else {
                ImGui::ImDrawListManager::AddRect(a_dl, V2{ a_f.mx0, a_f.my0 }, V2{ a_f.mx1, a_f.my1 }, Faded(Rgb(150, 120, 75), a), corner, 0, 1.5f * a_k);
            }
        }

        // the legend's panel and icons (the names are text: DrawLegendNames); a hidden kind dimmed, the row under the
        // cursor lit
        void DrawLegend(Canvas& a_c, const Frame& a_f, float a_k)
        {
            const bool  vanilla = Settings::Map().look.style == 1;
            const auto& show = Settings::Map().show;
            const auto  l = LegendOf(a_f.mx1, a_f.my0, a_k);
            a_c.Fill({ l.x0, l.y0 }, { l.x1, l.y1 }, vanilla ? Rgb(20, 19, 16, 215) : Rgb(10, 12, 18, 215));
            a_c.Outline({ l.x0, l.y0 }, { l.x1, l.y1 }, vanilla ? Rgb(140, 138, 130) : Rgb(150, 120, 75), 1.0f * a_k);
            const int under = l.At(a_f.cursorX, a_f.cursorY);
            for (std::size_t i = 0; i < Icons::kCount; ++i) {
                const float y = l.top + l.row * static_cast<float>(i);
                if (static_cast<int>(i) == under) {
                    a_c.Fill({ l.x0 + 3.0f * a_k, y }, { l.x1 - 3.0f * a_k, y + l.row }, Rgb(255, 255, 255, 30));
                }
                Icons::Draw(a_c, static_cast<Icons::Kind>(i), l.x0 + 24.0f * a_k, y + l.row * 0.5f, 22.0f * a_k, show[i] ? 1.0f : 0.3f);
            }
        }

        // the local map: the picture over the game's own (its bottom bar left free), the markers, a frame just inside
        // its edges (it fills the screen)
        void DrawLocal(Canvas& a_c, const Frame& a_f, float a_k)
        {
            const bool vanilla = Settings::Map().look.style == 1;
            a_c.Fill({ a_f.mx0, a_f.my0 }, { a_f.mx1, a_f.my1 }, vanilla ? Rgb(20, 19, 16) : Rgb(10, 12, 18));
            if (a_f.view) {
                if (const auto tex = MapMesh::Render(*a_f.view)) {
                    a_c.Image(tex, { a_f.mx0, a_f.my0 }, { a_f.mx1, a_f.my1 }, Rgb(255, 255, 255));
                }
            }
            DrawMarkers(a_c, a_f, a_k);
            const float in = 6.0f * a_k;
            const float x0 = a_f.mx0 + in, y0 = a_f.my0 + in, x1 = a_f.mx1 - in, y1 = a_f.my1 - in;
            if (vanilla) {
                const float out = 4.0f * a_k;
                a_c.Outline({ x0 - out * 0.5f, y0 - out * 0.5f }, { x1 + out * 0.5f, y1 + out * 0.5f }, Rgb(18, 17, 15, 225), out);
                a_c.Outline({ x0 - out, y0 - out }, { x1 + out, y1 + out }, Rgb(214, 212, 204), 2.0f * a_k);
                a_c.Outline({ x0, y0 }, { x1, y1 }, Rgb(140, 138, 130), 1.0f * a_k);
            } else {
                a_c.Outline({ x0, y0 }, { x1, y1 }, Rgb(150, 120, 75), 1.5f * a_k);
            }
            DrawLegend(a_c, a_f, a_k);
        }

        // the place's name over the local map (ImGui: the only text)
        void DrawTitle(ImGui::ImDrawList* a_dl, const Frame& a_f, float a_k)
        {
            using V2 = ImGui::ImVec2;
            namespace D = ImGui::ImDrawListManager;
            const auto font = ImGui::GetFont();
            if (a_f.title.empty() || !font) {
                return;
            }
            const bool  vanilla = Settings::Map().look.style == 1;
            const float base = ImGui::GetFontSize();
            const float size = 30.0f * a_k;
            const auto  ts = ImGui::CalcTextSize(a_f.title.c_str());
            const float kk = base > 0.0f ? size / base : 1.0f;
            const V2    at{ (a_f.mx0 + a_f.mx1) * 0.5f - ts.x * kk * 0.5f, a_f.my0 + 18.0f * a_k };
            D::AddText(a_dl, font, size, V2{ at.x + 2.0f * a_k, at.y + 2.0f * a_k }, Rgb(0, 0, 0, 220), a_f.title.c_str());
            D::AddText(a_dl, font, size, at, vanilla ? Rgb(236, 232, 220) : Rgb(225, 230, 240), a_f.title.c_str());
        }

        // the keys, at the bottom: on to the world map, closed
        void DrawHint(ImGui::ImDrawList* a_dl, const Frame& a_f, float a_k)
        {
            using V2 = ImGui::ImVec2;
            namespace D = ImGui::ImDrawListManager;
            const auto font = ImGui::GetFont();
            if (!font) {
                return;
            }
            const auto  text = std::format("M  {}        Esc  {}", Lang::T(Lang::S::HintWorld), Lang::T(Lang::S::HintClose));
            const float base = ImGui::GetFontSize();
            const float size = 24.0f * a_k;
            const auto  ts = ImGui::CalcTextSize(text.c_str());
            const float kk = base > 0.0f ? size / base : 1.0f;
            const V2    at{ (a_f.mx0 + a_f.mx1) * 0.5f - ts.x * kk * 0.5f, a_f.my1 - 48.0f * a_k };
            D::AddText(a_dl, font, size, V2{ at.x + 2.0f * a_k, at.y + 2.0f * a_k }, Rgb(0, 0, 0, 200), text.c_str());
            D::AddText(a_dl, font, size, at, Rgb(225, 220, 205, 230), text.c_str());
        }

        // the legend's title and names (its panel and icons: DrawLegend)
        void DrawLegendNames(ImGui::ImDrawList* a_dl, const Frame& a_f, float a_k)
        {
            using V2 = ImGui::ImVec2;
            namespace D = ImGui::ImDrawListManager;
            const auto font = ImGui::GetFont();
            if (!font) {
                return;
            }
            const auto& show = Settings::Map().show;
            const auto  l = LegendOf(a_f.mx1, a_f.my0, a_k);
            const float base = ImGui::GetFontSize();
            const auto  text = [&](const char* a_text, float a_x, float a_cy, float a_size, ImGui::ImU32 a_col) {
                const float kk = base > 0.0f ? a_size / base : 1.0f;
                const float y = a_cy - ImGui::CalcTextSize(a_text).y * kk * 0.5f;
                D::AddText(a_dl, font, a_size, V2{ a_x + 1.5f * a_k, y + 1.5f * a_k }, Rgb(0, 0, 0, 200), a_text);
                D::AddText(a_dl, font, a_size, V2{ a_x, y }, a_col, a_text);
            };
            text(Lang::T(Lang::S::Legend), l.x0 + 14.0f * a_k, l.y0 + 24.0f * a_k, 24.0f * a_k, Rgb(236, 232, 220));
            for (std::size_t i = 0; i < Icons::kCount; ++i) {
                const float cy = l.top + l.row * (static_cast<float>(i) + 0.5f);
                text(Icons::Name(static_cast<Icons::Kind>(i)), l.x0 + 46.0f * a_k, cy, 21.0f * a_k, show[i] ? Rgb(236, 232, 220) : Rgb(130, 126, 118));
            }
        }

        // the name of the icon under the cursor, to its right, as the game's map has it
        void DrawHoverName(ImGui::ImDrawList* a_dl, const Frame& a_f, float a_k)
        {
            using V2 = ImGui::ImVec2;
            namespace D = ImGui::ImDrawListManager;
            const auto  font = ImGui::GetFont();
            const auto& list = a_f.hoverQuest ? a_f.quests : a_f.icons;
            if (a_f.hoverName.empty() || !font || a_f.hover < 0 || static_cast<std::size_t>(a_f.hover) >= list.size()) {
                return;
            }
            const auto& i = list[a_f.hover];
            const float base = ImGui::GetFontSize();
            const float size = 28.0f * a_k;
            const auto  ts = ImGui::CalcTextSize(a_f.hoverName.c_str());
            const float kk = base > 0.0f ? size / base : 1.0f;
            const V2    at{ i.x + 17.0f * a_k * Settings::Map().iconSize, i.y - ts.y * kk * 0.5f };
            D::AddText(a_dl, font, size, V2{ at.x + 2.0f * a_k, at.y + 2.0f * a_k }, Rgb(0, 0, 0, 220), a_f.hoverName.c_str());
            D::AddText(a_dl, font, size, at, Rgb(245, 243, 236), a_f.hoverName.c_str());
        }

        // the local map drawn inside the map menu's own drawing (main thread), before the cursor menu: the game's
        // cursor over it. When that cannot draw, the HUD element draws it with ImGui instead
        std::atomic<std::int64_t> localDrawnAt = 0;  // steady clock ticks of the last time it was drawn there

        std::int64_t Ticks()
        {
            return std::chrono::steady_clock::now().time_since_epoch().count();
        }

        bool DrawnInMenu()
        {
            return Ticks() - localDrawnAt.load() < std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(250)).count();
        }

        void DrawLocalInMenu()
        {
            Frame f;
            {
                std::scoped_lock lock(frameLock);
                f = localFrame;
            }
            const float w = screenW, h = screenH;
            if (!f.has || w <= 0.0f || h <= 0.0f) {
                return;
            }
            Overlay::Sprites sprites;
            DrawLocal(sprites, f, h / 1080.0f);
            const float clip[4]{ f.mx0, f.my0, f.mx1, f.my1 };
            if (Overlay::Draw(sprites.batches, w, h, clip)) {
                localDrawnAt = Ticks();
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
                f = ui->IsMenuOpen(kLocalMenu) && localFrame.has ? localFrame : frame;
            }
            if (f.has && f.local) {
                if (!ui->IsShowingMenus()) {
                    return;
                }
                if (!DrawnInMenu()) {
                    // the map menu's drawing did not take it: all of it here, with a cursor of our own over it
                    ImGuiCanvas canvas(dl);
                    DrawLocal(canvas, f, k);
                    if (f.cursorX >= 0.0f && f.cursorY >= 0.0f && f.cursorY < f.my1) {
                        using V2 = ImGui::ImVec2;
                        const float s = 22.0f * k;
                        const V2    tip{ f.cursorX, f.cursorY }, left{ f.cursorX, f.cursorY + s }, right{ f.cursorX + s * 0.72f, f.cursorY + s * 0.72f };
                        ImGui::ImDrawListManager::AddTriangleFilled(dl, tip, left, right, Rgb(235, 230, 215));
                        ImGui::ImDrawListManager::AddTriangle(dl, tip, left, right, Rgb(20, 18, 15), 1.5f * k);
                    }
                }
                DrawTitle(dl, f, k);
                DrawHint(dl, f, k);
                DrawLegendNames(dl, f, k);
                DrawHoverName(dl, f, k);
                return;
            }
            if (ui->GameIsPaused() || menuOver || !ui->IsShowingMenus()) {
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
                if (overWorld && LocalMenuOpen()) {
                    return;  // ours over it: the world map waits (no markers picked under our cursor, no sounds)
                }
                func(a_this, a_interval, a_currentTime);
                UpdateMapMenu(a_this);
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };

        // the map menu's handlers (moving, looking, zooming the world map, picking its markers; its local map key, L,
        // which would switch it to the game's local map as ours closes on the same key): idle while ours is over it
        template <int N>
        struct MapHandlerHook
        {
            static bool thunk(RE::MenuEventHandler* a_this, RE::InputEvent* a_event)
            {
                return !(overWorld && LocalMenuOpen()) && func(a_this, a_event);
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };

        // our local map's menu: no movie of its own - updated and drawn (our sprites) where the game updates and draws
        // a menu, so the cursor menu still comes after it
        class LocalMenu final : public RE::IMenu
        {
        public:
            LocalMenu()
            {
                using Flag = RE::UI_MENU_FLAGS;
                menuFlags.set(Flag::kPausesGame, Flag::kUsesCursor, Flag::kUsesMenuContext, Flag::kModal, Flag::kDisablePauseMenu, Flag::kCustomRendering);
                depthPriority = 4;  // over the map menu (3) when opened on its world map
                inputContext = Context::kMenuMode;
            }

            static RE::IMenu* Create() { return new LocalMenu(); }

            void AdvanceMovie(float, std::uint32_t) override { UpdateLocal(); }
            void PostDisplay() override { DrawLocalInMenu(); }

            RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage& a_message) override
            {
                if (a_message.type == RE::UI_MESSAGE_TYPE::kHide || a_message.type == RE::UI_MESSAGE_TYPE::kForceHide) {
                    CloseLocal();
                }
                return RE::IMenu::ProcessMessage(a_message);
            }
        };

        void CloseLocalMenu(bool a_toWorldMap)
        {
            if (const auto queue = RE::UIMessageQueue::GetSingleton()) {
                queue->AddMessage(kLocalMenu, RE::UI_MESSAGE_TYPE::kHide, nullptr);
                if (a_toWorldMap) {
                    worldWanted = true;
                    worldWait = 0;
                    queue->AddMessage(RE::MapMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
                }
            }
        }

        // ---- the keys: one code for every device, as SKSE counts them (the keyboard's scan codes, 256 + a mouse button,
        // 264 / 265 the wheel, 266 + a gamepad button); a bind is a key and, optionally, another held with it
        constexpr std::uint32_t kMouseBase = 256, kPadBase = 266;
        constexpr std::uint32_t kWheelUp = kMouseBase + 8, kWheelDown = kMouseBase + 9;

        std::uint32_t Code(const RE::ButtonEvent* a_button)
        {
            const auto id = a_button->GetIDCode();
            switch (a_button->GetDevice()) {
            case RE::INPUT_DEVICE::kKeyboard:
                return id;
            case RE::INPUT_DEVICE::kMouse:
                return kMouseBase + id;
            case RE::INPUT_DEVICE::kGamepad:
                {
                    // XInput's button bits, the triggers as 9 and 10
                    constexpr std::uint32_t kButtons[] = { 0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080, 0x0100, 0x0200, 0x1000, 0x2000, 0x4000, 0x8000, 0x0009, 0x000A };
                    for (std::uint32_t i = 0; i < std::size(kButtons); ++i) {
                        if (id == kButtons[i]) {
                            return kPadBase + i;
                        }
                    }
                    return 0;
                }
            default:
                return 0;
            }
        }

        // the game's own keys for the map (Quick Map) and for its world / local switch (LocalMap): from ours to the world map
        bool WorldMapEvent(const RE::ButtonEvent* a_button)
        {
            const auto events = RE::UserEvents::GetSingleton();
            const auto& name = a_button->QUserEvent();
            return events && !name.empty() && (name == events->quickMap || name == events->localMap);
        }

        // what is held down now (the wheel never is)
        std::mutex                        heldLock;
        std::unordered_set<std::uint32_t> held;

        void NoteHeld(const RE::ButtonEvent* a_button)
        {
            const auto code = Code(a_button);
            if (code == 0 || code == kWheelUp || code == kWheelDown) {
                return;
            }
            std::scoped_lock lock(heldLock);
            if (a_button->IsPressed()) {
                held.insert(code);
            } else {
                held.erase(code);
            }
        }

        bool Held(std::uint32_t a_code)
        {
            std::scoped_lock lock(heldLock);
            return held.contains(a_code);
        }

        enum class Action
        {
            kToggle,
            kBeam,
            kLocalMap,
            kZoomIn,
            kZoomOut
        };

        // what a key does now: a bind whose other key is held first (Shift + N over N alone)
        std::optional<Action> ActionOf(std::uint32_t a_code)
        {
            const auto& s = Settings::Map();
            const std::tuple<std::uint32_t, std::uint32_t, Action> binds[] = { { s.toggleKey, s.toggleMod, Action::kToggle }, { s.beamKey, s.beamMod, Action::kBeam },
                { s.localMapKey, s.localMapMod, Action::kLocalMap }, { s.zoomInKey, s.zoomInMod, Action::kZoomIn }, { s.zoomOutKey, s.zoomOutMod, Action::kZoomOut },
                // the gamepad's own binds, working beside the keyboard's
                { s.toggleKeyPad, s.toggleModPad, Action::kToggle }, { s.beamKeyPad, s.beamModPad, Action::kBeam }, { s.localMapKeyPad, s.localMapModPad, Action::kLocalMap },
                { s.zoomInKeyPad, s.zoomInModPad, Action::kZoomIn }, { s.zoomOutKeyPad, s.zoomOutModPad, Action::kZoomOut } };
            std::optional<Action> alone;
            for (const auto& [key, mod, action] : binds) {
                if (key == 0 || key != a_code) {
                    continue;
                }
                if (mod != 0 && Held(mod)) {
                    return action;
                }
                if (mod == 0 && !alone) {
                    alone = action;
                }
            }
            return alone;
        }

        // the minimap's range where the character is (outside or inside), a_steps notches in (out: < 0)
        void Zoom(float a_steps)
        {
            const auto player = RE::PlayerCharacter::GetSingleton();
            const auto cell = player ? player->GetParentCell() : nullptr;
            auto&      s = Settings::Map();
            float&     range = cell && cell->IsInteriorCell() ? s.minimapRangeInside : s.minimapRange;
            range = std::clamp(range * std::pow(0.85f, a_steps), 300.0f, 12000.0f);
            saveIn = 1.0f;  // written once the zooming stops
        }

        // the keys during gameplay (the minimap's, the beam's: shown / hidden, remembered in the ini; the local map's;
        // the zoom) and the mouse and the sticks for the local map
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
                    if (const auto move = event->AsMouseMoveEvent(); move && local.open) {
                        rawX += static_cast<float>(move->mouseInputX);
                        rawY += static_cast<float>(move->mouseInputY);
                        continue;
                    }
                    if (const auto stick = event->AsThumbstickEvent()) {
                        (stick->IsLeft() ? leftStick : rightStick) = { stick->xValue, stick->yValue };
                        continue;
                    }
                    const auto button = event->AsButtonEvent();
                    if (!button) {
                        continue;
                    }
                    NoteHeld(button);
                    const auto code = Code(button);
                    if (button->GetDevice() == RE::INPUT_DEVICE::kKeyboard) {
                        if (const int pan = PanKey(button->GetIDCode()); pan >= 0) {
                            panKeys[pan] = local.open && button->IsPressed();
                        }
                    }
                    if (code == kMouseBase) {
                        mouseHeld = button->IsPressed();
                        mouseClicked = mouseClicked || (button->IsDown() && local.open);
                    } else if (button->IsDown() && (code == kWheelUp || code == kWheelDown) && local.open) {
                        wheel += code == kWheelUp ? 1 : -1;
                    }
                    // our local map up: closed by Esc, Tab, gamepad B or its own key; M goes on to the world map
                    // over the map menu's world map: its own key or the map's keys go back to it, Esc closes both
                    if (!captureSlot && code != 0 && button->IsDown() && LocalMenuOpen()) {
                        const bool shut = code == kEscape || code == 15 || code == kPadBase + 11;
                        const bool own = ActionOf(code) == Action::kLocalMap;
                        const bool world = code == 50 || WorldMapEvent(button);
                        if (overWorld) {
                            if (own || world) {
                                CloseLocalMenu(false);
                                worldWanted = true;  // should the map menu switch to its local map on the same key: back to the world map
                                worldWait = 0;
                            } else if (shut) {
                                CloseLocalMenu(false);
                                if (const auto queue = RE::UIMessageQueue::GetSingleton()) {
                                    queue->AddMessage(RE::MapMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
                                }
                            }
                        } else if (shut || own) {
                            CloseLocalMenu(false);
                        } else if (world) {
                            CloseLocalMenu(true);
                        }
                        continue;
                    }
                    // its key on the map menu's world map: ours over it
                    if (!captureSlot && code != 0 && button->IsDown() && s.enabled && s.localMap && ActionOf(code) == Action::kLocalMap) {
                        if (const auto ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen(RE::MapMenu::MENU_NAME)) {
                            OpenLocalMap(true);
                            continue;
                        }
                    }
                    // held to show: its key (or the gamepad's) let go hides it, whatever else is held then
                    if (s.toggleHold && button->IsUp() && code != 0 && (code == s.toggleKey || code == s.toggleKeyPad)) {
                        holdShown = false;
                    }
                    // the local map held open: its key let go closes it
                    if (localHeld && button->IsUp() && code != 0 && (code == s.localMapKey || code == s.localMapKeyPad)) {
                        localHeld = false;
                        CloseLocalMenu(false);
                    }
                    if (captureSlot || !s.enabled || code == 0 || !InGameplay()) {
                        continue;
                    }
                    const auto action = ActionOf(code);
                    if (!action) {
                        continue;
                    }
                    if (button->IsDown()) {
                        switch (*action) {
                        case Action::kToggle:
                            if (s.toggleHold) {
                                holdShown = true;  // shown while held
                            } else {
                                s.visible = !s.visible;
                                Settings::Save();
                            }
                            break;
                        case Action::kBeam:
                            s.questBeam = !s.questBeam;
                            Settings::Save();
                            break;
                        case Action::kLocalMap:
                            if (s.localMap) {
                                OpenLocalMap();
                                localHeld = s.localMapHold;  // held to look: let go, it closes
                            }
                            break;
                        case Action::kZoomIn:
                        case Action::kZoomOut:
                            Zoom(*action == Action::kZoomIn ? 1.0f : -1.0f);
                            break;
                        }
                    } else if (button->IsHeld() && button->HeldDuration() > 0.35f && (*action == Action::kZoomIn || *action == Action::kZoomOut)) {
                        // a key held: on zooming smoothly
                        Zoom((*action == Action::kZoomIn ? 5.0f : -5.0f) * std::clamp(RE::GetSecondsSinceLastFrame(), 0.0f, 0.1f));
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // PlayerControls' input: what our binds take is not passed on to the game (Shift + the wheel does not zoom the
        // camera too, a gamepad combination does not open favourites); the events are put back in their chain after
        struct ControlsHook
        {
            static RE::BSEventNotifyControl thunk(RE::PlayerControls* a_this, RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>* a_source)
            {
                if (!a_event || !*a_event || captureSlot || !Settings::Map().enabled) {
                    return func(a_this, a_event, a_source);
                }
                std::vector<RE::InputEvent*> all, kept;
                for (auto e = *a_event; e; e = e->next) {
                    all.push_back(e);
                    const auto button = e->AsButtonEvent();
                    const auto code = button ? Code(button) : 0;
                    if (code == 0 || !ActionOf(code)) {
                        kept.push_back(e);
                    }
                }
                if (kept.size() == all.size()) {
                    return func(a_this, a_event, a_source);
                }
                for (std::size_t i = 0; i < kept.size(); ++i) {
                    kept[i]->next = i + 1 < kept.size() ? kept[i + 1] : nullptr;
                }
                RE::InputEvent* head = kept.empty() ? nullptr : kept.front();
                const auto      result = head ? func(a_this, &head, a_source) : RE::BSEventNotifyControl::kContinue;
                for (std::size_t i = 0; i < all.size(); ++i) {
                    all[i]->next = i + 1 < all.size() ? all[i + 1] : nullptr;
                }
                return result;
            }
            static inline REL::Relocation<decltype(thunk)> func;
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
                if (a_event && a_event->menuName == RE::MapMenu::MENU_NAME && !a_event->opening) {
                    mapSawWorld = false;  // opened again: from its first frame
                }
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
            ui->Register(kLocalMenu, LocalMenu::Create);
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
        REL::Relocation<std::uintptr_t> controls{ RE::VTABLE_PlayerControls[0] };
        ControlsHook::func = controls.write_vfunc(0x1, ControlsHook::thunk);
        REL::Relocation<std::uintptr_t> map{ RE::VTABLE_MapMenu[0] };
        MapAdvanceHook::func = map.write_vfunc(0x5, MapAdvanceHook::thunk);
        REL::Relocation<std::uintptr_t> mapMove{ RE::VTABLE_MapMoveHandler[0] }, mapLook{ RE::VTABLE_MapLookHandler[0] }, mapZoom{ RE::VTABLE_MapZoomHandler[0] };
        MapHandlerHook<0>::func = mapMove.write_vfunc(0x1, MapHandlerHook<0>::thunk);
        MapHandlerHook<1>::func = mapLook.write_vfunc(0x1, MapHandlerHook<1>::thunk);
        MapHandlerHook<2>::func = mapZoom.write_vfunc(0x1, MapHandlerHook<2>::thunk);
        REL::Relocation<std::uintptr_t> localInput{ RE::VTABLE_LocalMapMenu__InputHandler[0] };  // its L: the game's own local map
        MapHandlerHook<3>::func = localInput.write_vfunc(0x1, MapHandlerHook<3>::thunk);
        logger::info("hooks installed");
    }

    void StartCapture(std::uint32_t* a_slot, std::uint32_t* a_modSlot, std::uint32_t* a_padSlot, std::uint32_t* a_padModSlot)
    {
        captureFirst = 0;
        captureMod = a_modSlot;
        capturePad = a_padSlot;
        capturePadMod = a_padModSlot;
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
        const auto button = a_event && a_event->eventType == RE::INPUT_EVENT_TYPE::kButton ? a_event->AsButtonEvent() : nullptr;
        if (button) {
            NoteHeld(button);  // the menu may keep the game's input from us meanwhile
        }
        if (!slot || !button) {
            return false;
        }
        const auto code = Code(button);
        if (code == kMouseBase || code == kMouseBase + 1) {
            return false;  // the clicks run the menu
        }
        if (code == 0) {
            return true;
        }
        const auto bind = [&](std::uint32_t a_key, std::uint32_t a_mod) {
            // a gamepad key into the gamepad's bind, anything else into the keyboard's: the other one stays
            const bool pad = a_key >= kPadBase && capturePad.load();
            *(pad ? capturePad.load() : slot) = a_key;
            if (const auto modSlot = pad ? capturePadMod.load() : captureMod.load()) {
                *modSlot = a_mod;
            }
            Settings::Save();
            captureFirst = 0;
            captureSlot = nullptr;
        };
        // the first key pressed waits: let go alone it is the bind; another pressed meanwhile (the wheel too) makes a
        // combination with it held (Shift, then the wheel: Shift + Wheel Up)
        const auto first = captureFirst.load();
        if (button->IsDown()) {
            if (code == kEscape) {  // Esc cancels: the bind as it was
                captureFirst = 0;
                captureSlot = nullptr;
            } else if (first == 0) {
                if (code == kWheelUp || code == kWheelDown) {
                    bind(code, 0);  // never held: alone at once
                } else {
                    captureFirst = code;
                }
            } else if (code != first) {
                bind(code, first);
            }
        } else if (button->IsUp() && code == first) {
            bind(first, 0);
        }
        return true;
    }

    void OnGameLoaded()
    {
        std::scoped_lock lock(bodyLock);
        lookedInto.clear();
        lootCache.clear();
        itemCache.clear();
    }
}

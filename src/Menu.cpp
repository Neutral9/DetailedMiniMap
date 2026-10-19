#include "Menu.h"

#include "Icons.h"
#include "Fog.h"
#include "Lang.h"
#include "MapMesh.h"
#include "MiniMap.h"
#include "Settings.h"

// third-party header: deprecated <codecvt>, mixed enums and so on - not our warnings
#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
#pragma warning(push, 0)
#include <SKSEMenuFramework.h>
#pragma warning(pop)

namespace ImGui = ImGuiMCP;

namespace Menu
{
    namespace
    {
        void NameTabs();  // below: the tabs in the menu's language

        // the value applies live while dragging, the file is written once the slider is let go
        void SaveAfterEdit()
        {
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                Settings::Save();
            }
        }

        void Check(const char* a_label, bool& a_value)
        {
            if (ImGui::Checkbox(a_label, &a_value)) {
                Settings::Save();
            }
        }

        // "label: [key | gamepad key]" - a click waits for the next key press, a combination if another key is held then
        // (Esc cancels); a gamepad key goes into the gamepad's bind, any other into the keyboard's - both work
        void KeyButton(const char* a_label, std::uint32_t& a_key, std::uint32_t& a_mod, std::uint32_t& a_padKey, std::uint32_t& a_padMod, const char* a_id)
        {
            ImGui::Text("%s", a_label);
            ImGui::SameLine();
            const bool waiting = MiniMap::IsCapturing(&a_key);
            std::string label;
            if (waiting) {
                label = Lang::T(Lang::S::PressKey);
            } else if (a_key != 0 && a_padKey != 0) {
                label = std::format("{}  |  {}", Settings::BindName(a_key, a_mod), Settings::BindName(a_padKey, a_padMod));
            } else {
                label = a_padKey != 0 ? Settings::BindName(a_padKey, a_padMod) : Settings::BindName(a_key, a_mod);
            }
            // as wide as its text needs (360 at least: the buttons line up)
            const float width = std::max(360.0f, ImGui::CalcTextSize(label.c_str()).x + 24.0f);
            if (ImGui::Button(std::format("{}###{}", label, a_id).c_str(), ImGui::ImVec2{ width, 0.0f })) {
                // a second click: no longer waiting, the binds as they were
                waiting ? MiniMap::StartCapture(nullptr, nullptr) : MiniMap::StartCapture(&a_key, &a_mod, &a_padKey, &a_padMod);
            }
            // no key at all, on either device: the action off
            ImGui::SameLine();
            ImGui::BeginDisabled(a_key == 0 && a_padKey == 0 && !waiting);
            if (ImGui::Button(std::format("{}###unbind{}", Lang::T(Lang::S::Unbind), a_id).c_str())) {
                MiniMap::StartCapture(nullptr, nullptr);
                a_key = a_mod = a_padKey = a_padMod = 0;
                Settings::Save();
            }
            ImGui::EndDisabled();
        }

        void Color(const char* a_label, float* a_rgb)
        {
            if (ImGui::ColorEdit3(a_label, a_rgb)) {
                Settings::Save();
            }
        }

        // a slider in the menu's language: the value applies live, the file is written once it is let go
        void Slider(Lang::S a_text, float& a_value, float a_min, float a_max, const char* a_format, bool a_log = false)
        {
            ImGui::SliderFloat(Lang::L(a_text).c_str(), &a_value, a_min, a_max, a_format, a_log ? ImGui::ImGuiSliderFlags_Logarithmic : 0);
            SaveAfterEdit();
        }


        // presets: every setting but the keys, the language and the item the map needs - chosen from the list, loaded,
        // saved over, deleted, or saved under a new name
        // the preset picked in the list (a page reset picks the Default one: what the settings then are)
        std::string chosen;

        void Presets()
        {
            using Lang::S;
            static std::vector<std::string> list;
            static char                     name[96] = {};
            static bool                     looked = false;
            if (!looked || ImGui::IsWindowAppearing()) {
                list = Settings::Presets();
                looked = true;
            }
            ImGui::SeparatorText(Lang::T(S::SecPresets));
            ImGui::SetNextItemWidth(360.0f);
            if (ImGui::BeginCombo(Lang::L(S::SecPresets).c_str(), chosen.empty() ? Lang::T(S::PresetNone) : chosen.c_str())) {
                for (const auto& p : list) {
                    if (ImGui::Selectable(p.c_str(), p == chosen)) {
                        chosen = p;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::BeginDisabled(chosen.empty());
            if (ImGui::Button(Lang::L(S::PresetLoad).c_str())) {
                MiniMap::StartCapture(nullptr, nullptr);
                Settings::LoadPreset(chosen);
                Icons::UseStyle(Settings::Map().look.iconStyle);
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(Settings::IsDefaultPreset(chosen));  // the Default one is the defaults: never written over, never deleted
            if (ImGui::Button(Lang::L(S::PresetSave).c_str())) {
                Settings::SavePreset(chosen);
            }
            ImGui::SameLine();
            if (ImGui::Button(Lang::L(S::PresetDelete).c_str())) {
                Settings::DeletePreset(chosen);
                chosen.clear();
                list = Settings::Presets();
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            ImGui::SetNextItemWidth(360.0f);
            ImGui::InputText(Lang::L(S::PresetName).c_str(), name, sizeof(name));
            ImGui::SameLine();
            ImGui::BeginDisabled(!*name || Settings::IsDefaultPreset(name));
            if (ImGui::Button(Lang::L(S::PresetSaveAs).c_str()) && Settings::SavePreset(name)) {
                list = Settings::Presets();
                for (const auto& p : list) {
                    if (_stricmp(p.c_str(), name) == 0) {
                        chosen = p;
                    }
                }
                *name = 0;
            }
            ImGui::EndDisabled();
        }

        // the map page: what the map does - when it shows, its keys, how far it reaches, what it marks, the beams
        void __stdcall RenderMap()
        {
            using Lang::S;
            auto& m = Settings::Map();
            ImGui::TextWrapped("%s", Lang::T(S::Intro));
            {
                // the game's language first, then every language by its English name, A to Z (as Hotkey Atlas has them)
                static constexpr const char* kNames[] = { "Game language", "Chinese", "English", "French", "German", "Italian", "Japanese", "Polish", "Russian", "Spanish" };
                static constexpr int         kOrder[] = { -1, 7, 1, 2, 4, 3, 8, 6, 0, 5 };  // Lang's index of each (zh, en, fr, de, it, ja, pl, ru, es)
                int current = 0;
                for (int i = 0; i < static_cast<int>(std::size(kOrder)); ++i) {
                    if (kOrder[i] == (m.language >= 0 && m.language < Lang::kLanguages ? m.language : -1)) {
                        current = i;
                    }
                }
                if (ImGui::Combo(Lang::L(S::Language).c_str(), &current, kNames, static_cast<int>(std::size(kNames)))) {
                    m.language = kOrder[current];
                    Settings::Save();
                    // the tabs renamed outside the menu's own drawing (on the main thread)
                    if (const auto tasks = SKSE::GetTaskInterface()) {
                        tasks->AddTask([] { NameTabs(); });
                    }
                }
            }
            Presets();

            ImGui::SeparatorText(Lang::T(S::SecMinimap));
            Check(Lang::L(S::Enabled).c_str(), m.enabled);
            {
                const char* places[] = { Lang::T(S::ShowEverywhere), Lang::T(S::ShowInside), Lang::T(S::ShowOutside) };
                if (ImGui::Combo(Lang::L(S::ShowWhere).c_str(), &m.showWhere, places, 3)) {
                    Settings::Save();
                }
            }
            KeyButton(Lang::T(S::ToggleKey), m.toggleKey, m.toggleMod, m.toggleKeyPad, m.toggleModPad, "toggle");
            Check(Lang::L(S::ToggleHold).c_str(), m.toggleHold);
            Check(Lang::L(S::NorthUp).c_str(), m.northUp);
            Slider(S::RangeOutside, m.minimapRange, 300.0f, 12000.0f, "%.0f", true);
            Slider(S::RangeInside, m.minimapRangeInside, 300.0f, 12000.0f, "%.0f", true);
            KeyButton(Lang::T(S::ZoomInKey), m.zoomInKey, m.zoomInMod, m.zoomInKeyPad, m.zoomInModPad, "zoomin");
            KeyButton(Lang::T(S::ZoomOutKey), m.zoomOutKey, m.zoomOutMod, m.zoomOutKeyPad, m.zoomOutModPad, "zoomout");
            Check(Lang::L(S::DynamicZoom).c_str(), m.dynamicZoom);
            if (m.dynamicZoom) {
                Slider(S::DynamicZoomStrength, m.dynamicZoomStrength, 0.25f, 2.0f, "%.2f");
            }
            // the map only with an item in the inventory: its EditorID (or Plugin.esp|0x800) typed under the box
            Check(Lang::L(S::NeedItem).c_str(), m.needItem);
            {
                static char        text[256] = {};
                static bool        editing = false;
                static std::string lookedFor = "\n";  // what the line under it was found for (nothing yet)
                static std::optional<std::string> found;
                if (!editing) {  // as saved (a reset changes it too)
                    const auto id = Settings::NeedItemId();
                    strncpy_s(text, id.c_str(), _TRUNCATE);
                }
                ImGui::BeginDisabled(!m.needItem);
                ImGui::SetNextItemWidth(360.0f);
                if (ImGui::InputText(Lang::L(S::NeedItemId).c_str(), text, sizeof(text))) {
                    Settings::SetNeedItemId(text);
                }
                editing = ImGui::IsItemActive();
                if (m.needItem && *text) {
                    if (lookedFor != text) {
                        lookedFor = text;
                        found = MiniMap::ItemName(lookedFor);
                    }
                    if (found) {
                        ImGui::TextColored(ImGui::ImVec4{ 0.45f, 0.85f, 0.45f, 1.0f }, "%s %s", Lang::T(S::ItemFound), found->c_str());
                    } else {
                        ImGui::TextColored(ImGui::ImVec4{ 0.95f, 0.45f, 0.35f, 1.0f }, "%s", Lang::T(S::ItemNotFound));
                    }
                }
                ImGui::EndDisabled();
            }
            // Helps To Have A Map: one of its maps for where the character is (as its own map menu asks); off without it
            {
                const bool loaded = MiniMap::MapModLoaded();
                ImGui::BeginDisabled(!loaded);
                Check(Lang::L(S::NeedMapMod).c_str(), m.needMapMod);
                ImGui::EndDisabled();
                if (!loaded) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImGui::ImVec4{ 0.6f, 0.6f, 0.6f, 1.0f }, "%s", Lang::T(S::MapModMissing));
                }
            }

            ImGui::SeparatorText(Lang::T(S::LocalMap));
            Check(Lang::L(S::LocalMap).c_str(), m.localMap);
            if (m.localMap) {
                KeyButton(Lang::T(S::LocalMapKey), m.localMapKey, m.localMapMod, m.localMapKeyPad, m.localMapModPad, "localmap");
                Check(Lang::L(S::LocalMapHold).c_str(), m.localMapHold);
                Check(Lang::L(S::RotateInvertX).c_str(), m.rotateInvertX);
                Check(Lang::L(S::RotateInvertY).c_str(), m.rotateInvertY);
                Slider(S::RotateSpeed, m.rotateSpeed, 0.2f, 3.0f, "%.2f");
            }

            ImGui::SeparatorText(Lang::T(S::SecIcons));
            Slider(S::IconFadeIn, m.iconFadeIn, 0.0f, 3.0f, m.iconFadeIn > 0.0f ? "%.2f" : Lang::T(S::AtOnce));
            Slider(S::IconFadeOut, m.iconFadeOut, 0.0f, 3.0f, m.iconFadeOut > 0.0f ? "%.2f" : Lang::T(S::AtOnce));
            Slider(S::IconRange, m.iconRange, 0.0f, 20000.0f, m.iconRange > 0.0f ? "%.0f" : Lang::T(S::Everywhere));
            Slider(S::LootHeight, m.lootHeight, 0.0f, 1000.0f, m.lootHeight > 0.0f ? "%.0f" : Lang::T(S::AllHeights));
            Check(Lang::L(S::HideEmpty).c_str(), m.hideEmpty);
            Check(Lang::L(S::HideSearched).c_str(), m.hideSearched);
            Check(Lang::L(S::GroupIcons).c_str(), m.groupIcons);
            {
                const char* detect[] = { Lang::T(S::DetectOff), Lang::T(S::DetectEnemies), Lang::T(S::DetectAll) };
                if (ImGui::Combo(Lang::L(S::DetectOnly).c_str(), &m.detectOnly, detect, 3)) {
                    Settings::Save();
                }
                if (m.detectOnly != 0) {
                    Check(Lang::L(S::DetectFighting).c_str(), m.detectFighting);
                }
            }
            // four columns, as wide as the longest name needs (a checkbox and a gap before it)
            float column = 180.0f;
            for (std::size_t i = 0; i < m.show.size(); ++i) {
                column = std::max(column, ImGui::CalcTextSize(Icons::Name(static_cast<Icons::Kind>(i))).x + 56.0f);
            }
            for (std::size_t i = 0; i < m.show.size(); ++i) {
                bool on = m.show[i];
                if (ImGui::Checkbox(std::format("{}###show{}", Icons::Name(static_cast<Icons::Kind>(i)), i).c_str(), &on)) {
                    m.show[i] = on;
                    Settings::Save();
                }
                if (i % 4 != 3 && i + 1 < m.show.size()) {
                    ImGui::SameLine(column * static_cast<float>(i % 4 + 1));
                }
            }

            ImGui::SeparatorText(Lang::T(S::SecCut));
            Check(Lang::L(S::CutCover).c_str(), m.cut);
            Slider(S::CutHeight, m.cutHeight, 80.0f, 1000.0f, "%.0f");
            Check(Lang::L(S::CutUnderground).c_str(), m.cutUnderground);
            Check(Lang::L(S::CutOutside).c_str(), m.cutOutside);
            Slider(S::CutHeightOutside, m.cutHeightOutside, 200.0f, 5000.0f, "%.0f");

            ImGui::SeparatorText(Lang::T(S::SecQuests));
            Check(Lang::L(S::QuestBeam).c_str(), m.questBeam);
            Check(Lang::L(S::BeamNearest).c_str(), m.beamNearest);
            Check(Lang::L(S::DestinationBeam).c_str(), m.destinationBeam);
            KeyButton(Lang::T(S::BeamKey), m.beamKey, m.beamMod, m.beamKeyPad, m.beamModPad, "beam");

            ImGui::SeparatorText(Lang::T(S::SecFog));
            Check(Lang::L(S::Fog).c_str(), m.fog);
            if (m.fog) {
                Check(Lang::L(S::FogMinimap).c_str(), m.fogMinimap);
                Check(Lang::L(S::FogLocal).c_str(), m.fogLocal);
                Slider(S::FogRadius, m.fogRadius, 500.0f, 10000.0f, "%.0f");
                Slider(S::FogRadiusInside, m.fogRadiusInside, 200.0f, 5000.0f, "%.0f");
                Check(Lang::L(S::FogHideIcons).c_str(), m.fogHideIcons);
                // all that was seen forgotten - a second click within a few seconds does it (a first one only asks)
                static auto asked = std::chrono::steady_clock::time_point{};
                const bool  asking = std::chrono::steady_clock::now() - asked < std::chrono::seconds(4);
                if (ImGui::Button(std::format("{}###fogreset", Lang::T(asking ? S::FogResetConfirm : S::FogReset)).c_str())) {
                    if (asking) {
                        Fog::Clear();
                        asked = {};
                    } else {
                        asked = std::chrono::steady_clock::now();
                    }
                }
            }

            ImGui::SeparatorText(Lang::T(S::SecDebug));
            Check(Lang::L(S::DebugLog).c_str(), m.debugLog);
            Check(Lang::L(S::DebugColours).c_str(), m.debugColours);

            ImGui::Separator();
            if (ImGui::Button(Lang::L(S::ResetMap).c_str())) {
                MiniMap::StartCapture(nullptr, nullptr);  // a bind being waited for: no more
                Settings::ResetMap();
                chosen = Settings::kDefaultPreset;
                // the language back to the game's: the tabs renamed outside the menu's own drawing (on the main thread)
                if (const auto tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask([] { NameTabs(); });
                }
            }
        }

        // the look page: how the map looks - its style, shape, size and place, colours, icons, frame and beams
        void __stdcall RenderLook()
        {
            using Lang::S;
            auto& m = Settings::Map();
            auto& l = m.look;
            {
                const char* styles[] = { Lang::T(S::StyleColour), Lang::T(S::StyleVanilla) };
                if (ImGui::Combo(Lang::L(S::Style).c_str(), &l.style, styles, 2)) {
                    Settings::Save();
                }
            }
            {
                // the icons' styles: the folders found when the list opens (a new one shows without a restart)
                static std::vector<std::string> found;
                if (ImGui::BeginCombo(Lang::L(S::IconStyle).c_str(), l.iconStyle.c_str())) {
                    if (found.empty() || ImGui::IsWindowAppearing()) {
                        found = Icons::Styles();
                    }
                    for (const auto& name : found) {
                        if (ImGui::Selectable(name.c_str(), name == l.iconStyle)) {
                            l.iconStyle = name;
                            Icons::UseStyle(name);
                            Settings::Save();
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            {
                // the minimap's frame: none, the drawn one (the style's), or a folder of pictures (found when the list opens)
                static std::vector<std::string> found;
                const char*                     none = Lang::T(S::FrameNone);
                const char*                     drawn = Lang::T(S::FrameDrawn);
                const char*                     shown = !m.minimapFrame ? none : l.frameStyle.empty() ? drawn : l.frameStyle.c_str();
                if (ImGui::BeginCombo(Lang::L(S::FrameStyle).c_str(), shown)) {
                    if (found.empty() || ImGui::IsWindowAppearing()) {
                        found = Icons::Frames();
                    }
                    if (ImGui::Selectable(none, !m.minimapFrame)) {
                        m.minimapFrame = false;
                        Settings::Save();
                    }
                    if (ImGui::Selectable(drawn, m.minimapFrame && l.frameStyle.empty())) {
                        m.minimapFrame = true;
                        l.frameStyle.clear();
                        Settings::Save();
                    }
                    for (const auto& name : found) {
                        if (ImGui::Selectable(name.c_str(), m.minimapFrame && name == l.frameStyle)) {
                            m.minimapFrame = true;
                            l.frameStyle = name;
                            Settings::Save();
                        }
                    }
                    ImGui::EndCombo();
                }
                if (m.minimapFrame) {
                    Slider(S::FrameOpacity, l.frameOpacity, 0.0f, 1.0f, "%.2f");
                    Color(Lang::L(S::FrameColor).c_str(), l.frameColor);
                }
            }

            ImGui::SeparatorText(Lang::T(S::SecMinimapLook));
            {
                int         shape = m.minimapRound ? 1 : 0;
                const char* shapes[] = { Lang::T(S::Square), Lang::T(S::Round) };
                if (ImGui::Combo(Lang::L(S::Shape).c_str(), &shape, shapes, 2)) {
                    m.minimapRound = shape == 1;
                    Settings::Save();
                }
                if (!m.minimapRound) {
                    Slider(S::Corner, m.minimapCorner, 0.0f, 150.0f, "%.0f");
                }
            }
            Slider(S::Size, m.minimapSize, 100.0f, 600.0f, "%.0f");
            Slider(S::Tilt, m.minimapTilt, 20.0f, 90.0f, "%.0f");
            Slider(S::MinimapOpacity, m.minimapOpacity, 0.1f, 1.0f, "%.2f");
            Check(Lang::L(S::Compass).c_str(), m.compass);
            if (m.compass) {
                Slider(S::CompassSize, m.compassSize, 0.5f, 2.5f, "%.2f");
            }
            Check(Lang::L(S::PointerCamera).c_str(), m.pointerCamera);

            ImGui::SeparatorText(Lang::T(S::SecZone));
            Check(Lang::L(S::ZoneName).c_str(), m.zoneName);
            if (m.zoneName) {
                int         place = m.zoneAbove ? 1 : 0;
                const char* places[] = { Lang::T(S::ZoneBelow), Lang::T(S::ZoneAbove) };
                if (ImGui::Combo(Lang::L(S::ZonePlace).c_str(), &place, places, 2)) {
                    m.zoneAbove = place == 1;
                    Settings::Save();
                }
                Slider(S::ZoneSize, m.zoneSize, 10.0f, 48.0f, "%.0f");
                Color(Lang::L(S::ZoneColor).c_str(), m.zoneColor);
            }

            ImGui::SeparatorText(Lang::T(S::SecPosition));
            {
                const char* corners[] = { Lang::T(S::TopLeft), Lang::T(S::TopRight), Lang::T(S::BottomLeft), Lang::T(S::BottomRight), Lang::T(S::Centre) };
                const int   was = m.anchor;
                if (ImGui::Combo(Lang::L(S::Anchor).c_str(), &m.anchor, corners, 5)) {
                    if ((was == 4) != (m.anchor == 4)) {  // the offsets from the middle start at it, from a corner as they first were
                        m.offsetX = m.offsetY = m.anchor == 4 ? 0.0f : 18.0f;
                    }
                    Settings::Save();
                }
            }
            if (m.anchor == 4) {
                Slider(S::OffsetX, m.offsetX, -900.0f, 900.0f, "%.0f");
                Slider(S::OffsetY, m.offsetY, -500.0f, 500.0f, "%.0f");
            } else {
                Slider(S::OffsetX, m.offsetX, 0.0f, 1500.0f, "%.0f");
                Slider(S::OffsetY, m.offsetY, 0.0f, 1000.0f, "%.0f");
            }

            ImGui::SeparatorText(Lang::T(S::SecIcons));
            Slider(S::IconSize, m.iconSize, 0.4f, 3.0f, "%.2f");
            Slider(S::IconOpacity, l.iconOpacity, 0.05f, 1.0f, "%.2f");

            ImGui::SeparatorText(Lang::T(S::SecBeams));
            Color(Lang::L(S::BeamColor).c_str(), l.beamColor);
            Color(Lang::L(S::DestinationColor).c_str(), l.destinationColor);
            Slider(S::BeamWidth, l.beamWidth, 0.5f, 3.0f, "%.2f");
            Slider(S::BeamOpacity, l.beamOpacity, 0.0f, 1.0f, "%.2f");
            Check(Lang::L(S::BeamColours).c_str(), l.beamColours);
            Check(Lang::L(S::BeamLights).c_str(), l.beamLights);
            const bool colour = l.style == 0;  // the vanilla style has its own palette: no colours to set
            ImGui::SeparatorText(Lang::T(S::SecGround));
            if (colour) {
                Color(Lang::L(S::GroundColor).c_str(), l.groundColor);
                Slider(S::GroundBrightness, l.groundBrightness, 0.0f, 3.0f, "%.2f");
            }
            Slider(S::GroundOpacity, l.groundOpacity, 0.0f, 1.0f, "%.2f");
            Check(Lang::L(S::Grid).c_str(), l.grid);
            if (l.grid) {
                Slider(S::GridStep, l.gridSize, 32.0f, 1024.0f, "%.0f");
            }
            Check(Lang::L(S::Contours).c_str(), l.contours);
            if (l.contours) {
                Slider(S::ContourStep, l.contourStep, 16.0f, 512.0f, "%.0f");
            }
            Slider(S::LineBrightness, l.lineStrength, 0.0f, 3.0f, "%.2f");

            ImGui::SeparatorText(Lang::T(S::SecGeometry));
            if (colour) {
                Color(Lang::L(S::GeometryColor).c_str(), l.geometryColor);
                Slider(S::GeometryBrightness, l.geometryBrightness, 0.0f, 3.0f, "%.2f");
            }
            Slider(S::GeometryOpacity, l.geometryOpacity, 0.0f, 1.0f, "%.2f");

            ImGui::SeparatorText(Lang::T(S::SecRoadsWater));
            Check(Lang::L(S::Roads).c_str(), l.roads);
            if (l.roads) {
                if (colour) {
                    Color(Lang::L(S::RoadColor).c_str(), l.roadColor);
                }
                Slider(S::RoadWidth, l.roadWidth, 1.0f, 3.0f, "%.2f");
            }
            Check(Lang::L(S::Water).c_str(), l.water);
            if (l.water) {
                Check(Lang::L(S::SeeUnderwater).c_str(), l.seeUnderwater);
            }
            if (l.water && colour) {
                Color(Lang::L(S::WaterColor).c_str(), l.waterColor);
            }
            Check(Lang::L(S::DistantLand).c_str(), l.distant);

            ImGui::SeparatorText(Lang::T(S::SecFog));
            Color(Lang::L(S::FogColor).c_str(), l.fogColor);
            Slider(S::FogOpacity, l.fogOpacity, 0.1f, 1.0f, "%.2f");
            Slider(S::FogFade, l.fogFade, 0.0f, 3.0f, l.fogFade > 0.0f ? "%.2f" : Lang::T(S::AtOnce));
            Slider(S::FogNoise, l.fogNoise, 0.0f, 2.0f, "%.2f");

            ImGui::SeparatorText(Lang::T(S::SecOther));
            Slider(S::DepthShade, l.depthShade, 0.0f, 1.0f, "%.2f");
            Slider(S::FadeTime, l.fadeTime, 0.0f, 3.0f, "%.2f");
            if (ImGui::Button(Lang::L(S::ResetLook).c_str())) {
                Settings::ResetLook();
                Icons::UseStyle(Settings::Map().look.iconStyle);
                chosen = Settings::kDefaultPreset;
            }
        }

        bool __stdcall OnFrameworkInput(RE::InputEvent* a_event)
        {
            return MiniMap::OnFrameworkInput(a_event);
        }

        // the two tabs in the menu's language: added once, renamed when it changes (an older Menu Framework without
        // renaming: both taken out and added again, in order)
        void NameTabs()
        {
            static std::string map, look;  // as they are named now
            const std::string  newMap = Lang::T(Lang::S::TabMap), newLook = Lang::T(Lang::S::TabLook);
            if (map.empty()) {
                SKSEMenuFramework::AddSectionItem(newMap, RenderMap);
                SKSEMenuFramework::AddSectionItem(newLook, RenderLook);
            } else if (newMap != map || newLook != look) {
                const std::string base = "Detailed MiniMap/";
                const bool        renamed = (newMap == map || SKSEMenuFramework::RenameSection(base + map, newMap)) &&
                                     (newLook == look || SKSEMenuFramework::RenameSection(base + look, newLook));
                if (!renamed) {
                    SKSEMenuFramework::DeleteSection(base + map);
                    SKSEMenuFramework::DeleteSection(base + look);
                    SKSEMenuFramework::AddSectionItem(newMap, RenderMap);
                    SKSEMenuFramework::AddSectionItem(newLook, RenderLook);
                }
            }
            map = newMap;
            look = newLook;
        }
    }

    void Register()
    {
        if (!SKSEMenuFramework::IsInstalled()) {
            logger::warn("SKSE Menu Framework not found: settings only through DetailedMiniMap.ini");
            return;
        }
        SKSEMenuFramework::SetSection("Detailed MiniMap");
        NameTabs();
        SKSEMenuFramework::AddInputEvent(OnFrameworkInput);
    }
}

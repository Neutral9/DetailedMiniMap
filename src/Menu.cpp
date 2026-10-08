#include "Menu.h"

#include "Icons.h"
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

        // "label: [key]" - a click waits for the next key press, a combination if another key is held then (Esc cancels)
        void KeyButton(const char* a_label, std::uint32_t& a_key, std::uint32_t& a_mod, const char* a_id)
        {
            ImGui::Text("%s", a_label);
            ImGui::SameLine();
            const auto label = MiniMap::IsCapturing(&a_key) ? std::string(Lang::T(Lang::S::PressKey)) : Settings::BindName(a_key, a_mod);
            if (ImGui::Button(std::format("{}###{}", label, a_id).c_str(), ImGui::ImVec2{ 240.0f, 0.0f })) {
                MiniMap::StartCapture(&a_key, &a_mod);
            }
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

        void __stdcall RenderMap()
        {
            using Lang::S;
            auto& m = Settings::Map();
            ImGui::TextWrapped("%s", Lang::T(S::Intro));
            {
                // every language by its own name
                int         current = Lang::Current();
                const char* names[Lang::kLanguages];
                for (int i = 0; i < Lang::kLanguages; ++i) {
                    names[i] = Lang::Name(i);
                }
                if (ImGui::Combo(Lang::L(S::Language).c_str(), &current, names, Lang::kLanguages)) {
                    m.language = current;
                    Settings::Save();
                    // the tabs renamed outside the menu's own drawing (on the main thread)
                    if (const auto tasks = SKSE::GetTaskInterface()) {
                        tasks->AddTask([] { NameTabs(); });
                    }
                }
            }
            Check(Lang::L(S::Enabled).c_str(), m.enabled);
            KeyButton(Lang::T(S::ToggleKey), m.toggleKey, m.toggleMod, "toggle");
            Check(Lang::L(S::LocalMap).c_str(), m.localMap);
            if (m.localMap) {
                KeyButton(Lang::T(S::LocalMapKey), m.localMapKey, m.localMapMod, "localmap");
            }

            ImGui::SeparatorText(Lang::T(S::SecMinimap));
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
            Check(Lang::L(S::NorthUp).c_str(), m.northUp);
            Slider(S::Size, m.minimapSize, 100.0f, 600.0f, "%.0f");
            Slider(S::RangeOutside, m.minimapRange, 300.0f, 12000.0f, "%.0f", true);
            Slider(S::RangeInside, m.minimapRangeInside, 300.0f, 12000.0f, "%.0f", true);
            KeyButton(Lang::T(S::ZoomInKey), m.zoomInKey, m.zoomInMod, "zoomin");
            KeyButton(Lang::T(S::ZoomOutKey), m.zoomOutKey, m.zoomOutMod, "zoomout");
            Slider(S::Tilt, m.minimapTilt, 20.0f, 90.0f, "%.0f");

            ImGui::SeparatorText(Lang::T(S::SecPosition));
            {
                const char* corners[] = { Lang::T(S::TopLeft), Lang::T(S::TopRight), Lang::T(S::BottomLeft), Lang::T(S::BottomRight) };
                if (ImGui::Combo(Lang::L(S::Anchor).c_str(), &m.anchor, corners, 4)) {
                    Settings::Save();
                }
            }
            Slider(S::OffsetX, m.offsetX, 0.0f, 1500.0f, "%.0f");
            Slider(S::OffsetY, m.offsetY, 0.0f, 1000.0f, "%.0f");

            ImGui::SeparatorText(Lang::T(S::SecIcons));
            Slider(S::IconSize, m.iconSize, 0.4f, 3.0f, "%.2f");
            Slider(S::IconFadeIn, m.iconFadeIn, 0.0f, 3.0f, m.iconFadeIn > 0.0f ? "%.2f" : Lang::T(S::AtOnce));
            Slider(S::IconFadeOut, m.iconFadeOut, 0.0f, 3.0f, m.iconFadeOut > 0.0f ? "%.2f" : Lang::T(S::AtOnce));
            for (std::size_t i = 0; i < m.show.size(); ++i) {
                bool on = m.show[i];
                if (ImGui::Checkbox(std::format("{}###show{}", Icons::Name(static_cast<Icons::Kind>(i)), i).c_str(), &on)) {
                    m.show[i] = on;
                    Settings::Save();
                }
                if (i % 4 != 3 && i + 1 < m.show.size()) {
                    ImGui::SameLine(180.0f * static_cast<float>(i % 4 + 1));
                }
            }

            ImGui::SeparatorText(Lang::T(S::SecCut));
            Check(Lang::L(S::CutCover).c_str(), m.cut);
            Slider(S::CutHeight, m.cutHeight, 80.0f, 1000.0f, "%.0f");

            ImGui::SeparatorText(Lang::T(S::SecQuests));
            Check(Lang::L(S::QuestBeam).c_str(), m.questBeam);
            KeyButton(Lang::T(S::BeamKey), m.beamKey, m.beamMod, "beam");

            ImGui::SeparatorText(Lang::T(S::SecDebug));
            Check(Lang::L(S::DebugLog).c_str(), m.debugLog);
        }

        void __stdcall RenderLook()
        {
            using Lang::S;
            auto& l = Settings::Map().look;
            {
                const char* styles[] = { Lang::T(S::StyleColour), Lang::T(S::StyleVanilla) };
                if (ImGui::Combo(Lang::L(S::Style).c_str(), &l.style, styles, 2)) {
                    Settings::Save();
                }
            }
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
            if (l.water && colour) {
                Color(Lang::L(S::WaterColor).c_str(), l.waterColor);
            }

            ImGui::SeparatorText(Lang::T(S::SecOther));
            Slider(S::DepthShade, l.depthShade, 0.0f, 1.0f, "%.2f");
            Slider(S::FadeTime, l.fadeTime, 0.0f, 3.0f, "%.2f");
            if (ImGui::Button(Lang::L(S::ResetLook).c_str())) {
                const int style = l.style;  // the style stays
                l = {};
                l.style = style;
                Settings::Save();
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

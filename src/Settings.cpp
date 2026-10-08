#include "Settings.h"

#include "Icons.h"
#include "Lang.h"

#include <Windows.h>

namespace Settings
{
    namespace
    {
        MapSettings map;
        static_assert(std::tuple_size_v<decltype(MapSettings::show)> == Icons::kCount);

        // warnings only, or (the detailed log on) everything, written out every few seconds
        void ApplyLog()
        {
            const auto log = spdlog::default_logger();
            if (!log) {
                return;
            }
            log->set_level(map.debugLog ? spdlog::level::info : spdlog::level::warn);
        }

        std::string IniPath(const char* a_name)
        {
            return (std::filesystem::current_path() / "Data/SKSE/Plugins" / a_name).string();
        }

        // the user ini overrides the default one key by key
        float ReadFloat(const char* a_section, const char* a_key, float a_default)
        {
            char buf[64];
            for (const auto file : { "DetailedMiniMap.ini", "DetailedMiniMap_User.ini" }) {
                if (::GetPrivateProfileStringA(a_section, a_key, "", buf, sizeof(buf), IniPath(file).c_str()) > 0) {
                    try {
                        a_default = std::stof(buf);
                    } catch (...) {
                        logger::warn("{}: [{}] {}={} is not a number", file, a_section, a_key, buf);
                    }
                }
            }
            return a_default;
        }

        void WriteFloat(const char* a_section, const char* a_key, float a_value)
        {
            ::WritePrivateProfileStringA(a_section, a_key, std::format("{:g}", a_value).c_str(), IniPath("DetailedMiniMap_User.ini").c_str());
        }

        // every setting once: read (with its range) or written
        struct Reader
        {
            void operator()(const char* a_section, const char* a_key, bool& a_value) const
            {
                a_value = ReadFloat(a_section, a_key, a_value ? 1.0f : 0.0f) != 0.0f;
            }
            void operator()(const char* a_section, const char* a_key, float& a_value, float a_min, float a_max) const
            {
                a_value = std::clamp(ReadFloat(a_section, a_key, a_value), a_min, a_max);
            }
            void operator()(const char* a_section, const char* a_key, int& a_value, int a_min, int a_max) const
            {
                a_value = std::clamp(static_cast<int>(ReadFloat(a_section, a_key, static_cast<float>(a_value))), a_min, a_max);
            }
            void operator()(const char* a_section, const char* a_key, std::uint32_t& a_value) const
            {
                a_value = static_cast<std::uint32_t>(std::max(ReadFloat(a_section, a_key, static_cast<float>(a_value)), 0.0f));
            }
        };

        struct Writer
        {
            void operator()(const char* a_section, const char* a_key, bool& a_value) const
            {
                WriteFloat(a_section, a_key, a_value ? 1.0f : 0.0f);
            }
            void operator()(const char* a_section, const char* a_key, float& a_value, float, float) const
            {
                WriteFloat(a_section, a_key, a_value);
            }
            void operator()(const char* a_section, const char* a_key, int& a_value, int, int) const
            {
                WriteFloat(a_section, a_key, static_cast<float>(a_value));
            }
            void operator()(const char* a_section, const char* a_key, std::uint32_t& a_value) const
            {
                WriteFloat(a_section, a_key, static_cast<float>(a_value));
            }
        };

        template <class F>
        void Each(F&& a_f)
        {
            auto& m = map;
            a_f("Map", "Enabled", m.enabled);
            a_f("Map", "ToggleKey", m.toggleKey);
            a_f("Map", "Visible", m.visible);
            a_f("Map", "Language", m.language, -1, Lang::kLanguages - 1);
            a_f("Map", "MinimapSize", m.minimapSize, 100.0f, 600.0f);
            a_f("Map", "MinimapRange", m.minimapRange, 300.0f, 12000.0f);
            a_f("Map", "MinimapRangeInside", m.minimapRangeInside, 300.0f, 12000.0f);
            a_f("Map", "MinimapTilt", m.minimapTilt, 20.0f, 90.0f);
            a_f("Map", "NorthUp", m.northUp);
            a_f("Map", "MinimapRound", m.minimapRound);
            a_f("Map", "MinimapCorner", m.minimapCorner, 0.0f, 150.0f);
            a_f("Map", "Anchor", m.anchor, 0, 3);
            a_f("Map", "OffsetX", m.offsetX, 0.0f, 2000.0f);
            a_f("Map", "OffsetY", m.offsetY, 0.0f, 2000.0f);
            a_f("Map", "IconSize", m.iconSize, 0.4f, 3.0f);
            a_f("Map", "IconFadeIn", m.iconFadeIn, 0.0f, 3.0f);
            a_f("Map", "IconFadeOut", m.iconFadeOut, 0.0f, 3.0f);
            for (std::size_t i = 0; i < m.show.size(); ++i) {
                const std::string key = std::string("Show_") + Icons::File(static_cast<Icons::Kind>(i));
                a_f("Icons", key.c_str(), m.show[i]);
            }
            a_f("Map", "Cut", m.cut);
            a_f("Map", "CutHeight", m.cutHeight, 80.0f, 2000.0f);
            a_f("Map", "QuestBeam", m.questBeam);
            a_f("Map", "BeamKey", m.beamKey);
            a_f("Map", "LocalMap", m.localMap);
            a_f("Map", "DebugLog", m.debugLog);

            auto& l = m.look;
            a_f("MapLook", "Grid", l.grid);
            a_f("MapLook", "GridSize", l.gridSize, 16.0f, 2048.0f);
            a_f("MapLook", "Contours", l.contours);
            a_f("MapLook", "ContourStep", l.contourStep, 8.0f, 1024.0f);
            a_f("MapLook", "LineStrength", l.lineStrength, 0.0f, 4.0f);
            a_f("MapLook", "GroundBrightness", l.groundBrightness, 0.0f, 4.0f);
            a_f("MapLook", "GroundOpacity", l.groundOpacity, 0.0f, 1.0f);
            a_f("MapLook", "GeometryBrightness", l.geometryBrightness, 0.0f, 4.0f);
            a_f("MapLook", "GeometryOpacity", l.geometryOpacity, 0.0f, 1.0f);
            a_f("MapLook", "DepthShade", l.depthShade, 0.0f, 1.0f);
            a_f("MapLook", "FadeTime", l.fadeTime, 0.0f, 5.0f);
            a_f("MapLook", "Roads", l.roads);
            a_f("MapLook", "RoadWidth", l.roadWidth, 1.0f, 3.0f);
            a_f("MapLook", "Water", l.water);
            a_f("MapLook", "Style", l.style, 0, 1);
            static const std::string channels[] = { "R", "G", "B" };
            for (int i = 0; i < 3; ++i) {
                const auto& c = channels[i];
                a_f("MapLook", ("Ground" + c).c_str(), l.groundColor[i], 0.0f, 1.0f);
                a_f("MapLook", ("Geometry" + c).c_str(), l.geometryColor[i], 0.0f, 1.0f);
                a_f("MapLook", ("Road" + c).c_str(), l.roadColor[i], 0.0f, 1.0f);
                a_f("MapLook", ("Water" + c).c_str(), l.waterColor[i], 0.0f, 1.0f);
            }
        }
    }

    MapSettings& Map()
    {
        return map;
    }

    void Load()
    {
        Each(Reader{});
        ApplyLog();
    }

    void Save()
    {
        Each(Writer{});
        ApplyLog();
    }

    std::string KeyName(std::uint32_t a_key)
    {
        static const std::unordered_map<std::uint32_t, const char*> names{
            { 1, "Esc" }, { 2, "1" }, { 3, "2" }, { 4, "3" }, { 5, "4" }, { 6, "5" }, { 7, "6" }, { 8, "7" }, { 9, "8" },
            { 10, "9" }, { 11, "0" }, { 12, "-" }, { 13, "=" }, { 14, "Backspace" }, { 15, "Tab" }, { 16, "Q" },
            { 17, "W" }, { 18, "E" }, { 19, "R" }, { 20, "T" }, { 21, "Y" }, { 22, "U" }, { 23, "I" }, { 24, "O" },
            { 25, "P" }, { 26, "[" }, { 27, "]" }, { 28, "Enter" }, { 29, "Left Ctrl" }, { 30, "A" }, { 31, "S" },
            { 32, "D" }, { 33, "F" }, { 34, "G" }, { 35, "H" }, { 36, "J" }, { 37, "K" }, { 38, "L" }, { 39, ";" },
            { 40, "'" }, { 41, "`" }, { 42, "Left Shift" }, { 43, "\\" }, { 44, "Z" }, { 45, "X" }, { 46, "C" },
            { 47, "V" }, { 48, "B" }, { 49, "N" }, { 50, "M" }, { 51, "," }, { 52, "." }, { 53, "/" },
            { 54, "Right Shift" }, { 55, "Num *" }, { 56, "Left Alt" }, { 57, "Space" }, { 58, "Caps Lock" },
            { 59, "F1" }, { 60, "F2" }, { 61, "F3" }, { 62, "F4" }, { 63, "F5" }, { 64, "F6" }, { 65, "F7" },
            { 66, "F8" }, { 67, "F9" }, { 68, "F10" }, { 69, "Num Lock" }, { 70, "Scroll Lock" }, { 71, "Num 7" },
            { 72, "Num 8" }, { 73, "Num 9" }, { 74, "Num -" }, { 75, "Num 4" }, { 76, "Num 5" }, { 77, "Num 6" },
            { 78, "Num +" }, { 79, "Num 1" }, { 80, "Num 2" }, { 81, "Num 3" }, { 82, "Num 0" }, { 83, "Num ." },
            { 87, "F11" }, { 88, "F12" }, { 156, "Num Enter" }, { 157, "Right Ctrl" }, { 181, "Num /" },
            { 184, "Right Alt" }, { 199, "Home" }, { 200, "Up" }, { 201, "Page Up" }, { 203, "Left" },
            { 205, "Right" }, { 207, "End" }, { 208, "Down" }, { 209, "Page Down" }, { 210, "Insert" },
            { 211, "Delete" }
        };
        if (a_key == 0) {
            return Lang::T(Lang::S::KeyNone);
        }
        const auto it = names.find(a_key);
        return it != names.end() ? it->second : std::format("#{}", a_key);
    }
}

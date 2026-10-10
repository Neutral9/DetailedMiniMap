#include "Settings.h"

#include "Icons.h"
#include "Lang.h"

#include <fstream>

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

        std::filesystem::path IniPath(const char* a_name)
        {
            return std::filesystem::current_path() / "Data/SKSE/Plugins" / a_name;
        }

        // an ini read whole: "section\nkey" (lower case) -> value. Read and written with plain file I/O, not the
        // Windows profile API: under Mod Organizer's virtual file system that one did not always write over a file
        // already there (a setting changed, then lost)
        using Ini = std::unordered_map<std::string, std::string>;

        std::string Lower(std::string_view a_s)
        {
            std::string out(a_s);
            std::ranges::transform(out, out.begin(), [](char a_c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(a_c))); });
            return out;
        }

        std::string Trim(std::string_view a_s)
        {
            const auto first = a_s.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
            const auto last = a_s.find_last_not_of(" \t\r\n");
            return first == std::string_view::npos ? std::string() : std::string(a_s.substr(first, last - first + 1));
        }

        Ini ReadIni(const char* a_name)
        {
            Ini           out;
            std::ifstream file(IniPath(a_name));
            std::string   line, section;
            while (std::getline(file, line)) {
                const auto text = Trim(line);
                if (text.empty() || text.front() == ';' || text.front() == '#') {
                    continue;
                }
                if (text.front() == '[') {
                    section = Lower(Trim(std::string_view(text).substr(1, text.find(']') - 1)));
                    continue;
                }
                if (const auto eq = text.find('='); eq != std::string::npos) {
                    out[section + '\n' + Lower(Trim(std::string_view(text).substr(0, eq)))] = Trim(std::string_view(text).substr(eq + 1));
                }
            }
            return out;
        }

        // every setting once: read (with its range) or written
        struct Reader
        {
            std::vector<Ini> files;  // the shipped defaults, then the user's (it overrides them key by key)

            explicit Reader(bool a_user = true)
            {
                files.push_back(ReadIni("DetailedMiniMap.ini"));
                if (a_user) {  // not on a reset: the defaults only
                    files.push_back(ReadIni("DetailedMiniMap_User.ini"));
                }
            }

            const std::string* Find(const char* a_section, const char* a_key) const
            {
                const auto         key = Lower(a_section) + '\n' + Lower(a_key);
                const std::string* found = nullptr;
                for (const auto& file : files) {
                    if (const auto it = file.find(key); it != file.end() && !it->second.empty()) {
                        found = &it->second;
                    }
                }
                return found;
            }

            float Number(const char* a_section, const char* a_key, float a_default) const
            {
                const auto text = Find(a_section, a_key);
                if (!text) {
                    return a_default;
                }
                try {
                    return std::stof(*text);
                } catch (...) {
                    logger::warn("ini: [{}] {}={} is not a number", a_section, a_key, *text);
                    return a_default;
                }
            }

            void operator()(const char* a_section, const char* a_key, bool& a_value) const
            {
                a_value = Number(a_section, a_key, a_value ? 1.0f : 0.0f) != 0.0f;
            }
            void operator()(const char* a_section, const char* a_key, float& a_value, float a_min, float a_max) const
            {
                a_value = std::clamp(Number(a_section, a_key, a_value), a_min, a_max);
            }
            void operator()(const char* a_section, const char* a_key, int& a_value, int a_min, int a_max) const
            {
                a_value = std::clamp(static_cast<int>(Number(a_section, a_key, static_cast<float>(a_value))), a_min, a_max);
            }
            void operator()(const char* a_section, const char* a_key, std::uint32_t& a_value) const
            {
                a_value = static_cast<std::uint32_t>(std::max(Number(a_section, a_key, static_cast<float>(a_value)), 0.0f));
            }
            void operator()(const char* a_section, const char* a_key, std::string& a_value) const
            {
                if (const auto text = Find(a_section, a_key)) {
                    a_value = *text;
                }
            }
        };

        // every setting gathered, then the user ini written whole in one go
        struct Writer
        {
            std::vector<std::pair<std::string, std::string>> lines;  // section, "key=value"

            void Add(const char* a_section, const char* a_key, std::string a_value) { lines.emplace_back(a_section, std::format("{}={}", a_key, a_value)); }

            void operator()(const char* a_section, const char* a_key, bool& a_value) { Add(a_section, a_key, a_value ? "1" : "0"); }
            void operator()(const char* a_section, const char* a_key, float& a_value, float, float) { Add(a_section, a_key, std::format("{:g}", a_value)); }
            void operator()(const char* a_section, const char* a_key, int& a_value, int, int) { Add(a_section, a_key, std::to_string(a_value)); }
            void operator()(const char* a_section, const char* a_key, std::uint32_t& a_value) { Add(a_section, a_key, std::to_string(a_value)); }
            void operator()(const char* a_section, const char* a_key, std::string& a_value) { Add(a_section, a_key, a_value); }

            void Write() const
            {
                std::string text = "; Detailed MiniMap - your settings (the in-game menu writes this file; it overrides DetailedMiniMap.ini)\n";
                std::vector<std::string> sections;
                for (const auto& [section, line] : lines) {
                    if (std::ranges::find(sections, section) == sections.end()) {
                        sections.push_back(section);
                    }
                }
                for (const auto& section : sections) {
                    text += std::format("\n[{}]\n", section);
                    for (const auto& [s, line] : lines) {
                        if (s == section) {
                            text += line + '\n';
                        }
                    }
                }
                const auto    path = IniPath("DetailedMiniMap_User.ini");
                std::ofstream file(path, std::ios::binary | std::ios::trunc);
                if (!file || !file.write(text.data(), static_cast<std::streamsize>(text.size()))) {
                    logger::warn("ini: {} could not be written", path.string());
                }
            }
        };

        template <class F>
        void Each(F&& a_f, MapSettings& m = map)
        {
            a_f("Map", "Enabled", m.enabled);
            a_f("Map", "ShowWhere", m.showWhere, 0, 2);
            a_f("Map", "ToggleKey", m.toggleKey);
            a_f("Map", "ToggleMod", m.toggleMod);
            a_f("Map", "ToggleKeyPad", m.toggleKeyPad);
            a_f("Map", "ToggleModPad", m.toggleModPad);
            a_f("Map", "ToggleHold", m.toggleHold);
            a_f("Map", "Visible", m.visible);
            a_f("Map", "Language", m.language, -1, Lang::kLanguages - 1);
            a_f("Map", "MinimapSize", m.minimapSize, 100.0f, 600.0f);
            a_f("Map", "MinimapRange", m.minimapRange, 300.0f, 12000.0f);
            a_f("Map", "MinimapRangeInside", m.minimapRangeInside, 300.0f, 12000.0f);
            a_f("Map", "MinimapTilt", m.minimapTilt, 20.0f, 90.0f);
            a_f("Map", "NorthUp", m.northUp);
            a_f("Map", "MinimapRound", m.minimapRound);
            a_f("Map", "MinimapCorner", m.minimapCorner, 0.0f, 150.0f);
            a_f("Map", "MinimapFrame", m.minimapFrame);
            a_f("Map", "PointerCamera", m.pointerCamera);
            a_f("Map", "Anchor", m.anchor, 0, 3);
            a_f("Map", "OffsetX", m.offsetX, 0.0f, 2000.0f);
            a_f("Map", "OffsetY", m.offsetY, 0.0f, 2000.0f);
            a_f("Map", "IconSize", m.iconSize, 0.4f, 3.0f);
            a_f("Map", "IconFadeIn", m.iconFadeIn, 0.0f, 3.0f);
            a_f("Map", "IconFadeOut", m.iconFadeOut, 0.0f, 3.0f);
            a_f("Map", "IconRange", m.iconRange, 0.0f, 20000.0f);
            a_f("Map", "LootHeight", m.lootHeight, 0.0f, 2000.0f);
            a_f("Map", "MinimapOpacity", m.minimapOpacity, 0.1f, 1.0f);
            a_f("Map", "HideEmpty", m.hideEmpty);
            a_f("Map", "GroupIcons", m.groupIcons);
            for (std::size_t i = 0; i < m.show.size(); ++i) {
                const std::string key = std::string("Show_") + Icons::File(static_cast<Icons::Kind>(i));
                a_f("Icons", key.c_str(), m.show[i]);
            }
            a_f("Map", "Cut", m.cut);
            a_f("Map", "CutHeight", m.cutHeight, 80.0f, 2000.0f);
            a_f("Map", "QuestBeam", m.questBeam);
            a_f("Map", "BeamNearest", m.beamNearest);
            a_f("Map", "BeamKey", m.beamKey);
            a_f("Map", "BeamMod", m.beamMod);
            a_f("Map", "BeamKeyPad", m.beamKeyPad);
            a_f("Map", "BeamModPad", m.beamModPad);
            a_f("Map", "LocalMap", m.localMap);
            a_f("Map", "LocalMapKey", m.localMapKey);
            a_f("Map", "LocalMapMod", m.localMapMod);
            a_f("Map", "LocalMapKeyPad", m.localMapKeyPad);
            a_f("Map", "LocalMapModPad", m.localMapModPad);
            a_f("Map", "LocalMapHold", m.localMapHold);
            a_f("Map", "RotateKey", m.rotateKey);
            a_f("Map", "RotateMod", m.rotateMod);
            a_f("Map", "RotateKeyPad", m.rotateKeyPad);
            a_f("Map", "RotateModPad", m.rotateModPad);
            a_f("Map", "RotateInvertX", m.rotateInvertX);
            a_f("Map", "RotateInvertY", m.rotateInvertY);
            a_f("Map", "RotateSpeed", m.rotateSpeed, 0.2f, 3.0f);
            a_f("Map", "NeedItem", m.needItem);
            a_f("Map", "NeedItemID", m.needItemId);
            a_f("Map", "ZoomInKey", m.zoomInKey);
            a_f("Map", "ZoomInMod", m.zoomInMod);
            a_f("Map", "ZoomInKeyPad", m.zoomInKeyPad);
            a_f("Map", "ZoomInModPad", m.zoomInModPad);
            a_f("Map", "ZoomOutKey", m.zoomOutKey);
            a_f("Map", "ZoomOutMod", m.zoomOutMod);
            a_f("Map", "ZoomOutKeyPad", m.zoomOutKeyPad);
            a_f("Map", "ZoomOutModPad", m.zoomOutModPad);
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
            a_f("MapLook", "SeeUnderwater", l.seeUnderwater);
            a_f("MapLook", "Style", l.style, 0, 1);
            a_f("MapLook", "IconStyle", l.iconStyle);
            a_f("MapLook", "FrameStyle", l.frameStyle);
            a_f("MapLook", "FrameOpacity", l.frameOpacity, 0.0f, 1.0f);
            a_f("MapLook", "IconOpacity", l.iconOpacity, 0.05f, 1.0f);
            a_f("MapLook", "BeamWidth", l.beamWidth, 0.5f, 3.0f);
            a_f("MapLook", "BeamOpacity", l.beamOpacity, 0.0f, 1.0f);
            a_f("MapLook", "BeamLights", l.beamLights);
            static const std::string channels[] = { "R", "G", "B" };
            for (int i = 0; i < 3; ++i) {
                const auto& c = channels[i];
                a_f("MapLook", ("Ground" + c).c_str(), l.groundColor[i], 0.0f, 1.0f);
                a_f("MapLook", ("Geometry" + c).c_str(), l.geometryColor[i], 0.0f, 1.0f);
                a_f("MapLook", ("Road" + c).c_str(), l.roadColor[i], 0.0f, 1.0f);
                a_f("MapLook", ("Water" + c).c_str(), l.waterColor[i], 0.0f, 1.0f);
                a_f("MapLook", ("Frame" + c).c_str(), l.frameColor[i], 0.0f, 1.0f);
                a_f("MapLook", ("Beam" + c).c_str(), l.beamColor[i], 0.0f, 1.0f);
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

    namespace
    {
        std::mutex textLock;  // the item's id: the menu writes it, the game reads it
    }

    void Save()
    {
        static std::mutex lock;  // the menu and the keys both save: one at a time
        std::scoped_lock  guard(lock);
        std::scoped_lock  text(textLock);
        Writer            writer;
        Each(writer);
        writer.Write();
        ApplyLog();
    }

    // the defaults: built in, then the shipped DetailedMiniMap.ini over them (not the user's own)
    namespace
    {
        MapSettings Defaults()
        {
            MapSettings d;
            Each(Reader{ false }, d);
            return d;
        }
    }

    void ResetMap()
    {
        auto       d = Defaults();
        d.language = map.language;
        d.look = map.look;
        {
            std::scoped_lock text(textLock);
            map = d;
        }
        Save();
    }

    std::string NeedItemId()
    {
        std::scoped_lock guard(textLock);
        return map.needItemId;
    }

    void SetNeedItemId(std::string a_id)
    {
        {
            std::scoped_lock guard(textLock);
            map.needItemId = std::move(a_id);
        }
        Save();
    }

    void ResetLook()
    {
        // the map's style, the icons' and the frame stay (chosen, not tuned)
        const int  style = map.look.style;
        const auto icons = map.look.iconStyle;
        const auto frame = map.look.frameStyle;
        map.look = Defaults().look;
        map.look.style = style;
        map.look.iconStyle = icons;
        map.look.frameStyle = frame;
        Save();
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
        // the mouse from 256, the gamepad from 266 (as SKSE counts them)
        static constexpr const char* kMouse[] = { "Mouse Left", "Mouse Right", "Mouse Middle", "Mouse 4", "Mouse 5", "Mouse 6", "Mouse 7", "Mouse 8", "Wheel Up", "Wheel Down" };
        static constexpr const char* kPad[] = { "Dpad Up", "Dpad Down", "Dpad Left", "Dpad Right", "Start", "Back", "LS", "RS", "LB", "RB", "A", "B", "X", "Y", "LT", "RT" };
        if (a_key == 0) {
            return Lang::T(Lang::S::KeyNone);
        }
        if (a_key >= 256 && a_key < 256 + std::size(kMouse)) {
            return kMouse[a_key - 256];
        }
        if (a_key >= 266 && a_key < 266 + std::size(kPad)) {
            return std::format("Gamepad {}", kPad[a_key - 266]);
        }
        const auto it = names.find(a_key);
        return it != names.end() ? it->second : std::format("#{}", a_key);
    }

    std::string BindName(std::uint32_t a_key, std::uint32_t a_mod)
    {
        return a_key != 0 && a_mod != 0 ? std::format("{} + {}", KeyName(a_mod), KeyName(a_key)) : KeyName(a_key);
    }
}

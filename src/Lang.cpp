#include "Lang.h"

#include "Settings.h"

#include <fstream>

namespace Lang
{
    namespace
    {
        // the files' keys, in S order
        constexpr const char* kKeys[static_cast<int>(S::kCount)] = { "Intro", "Language", "Enabled", "ToggleKey", "PressKey", "KeyNone", "SecMinimap", "Shape", "Square",
            "Round", "Corner", "NorthUp", "Size", "RangeOutside", "RangeInside", "Tilt", "SecIcons", "IconSize", "IconFadeIn", "IconFadeOut", "AtOnce", "SecCut", "CutCover",
            "CutHeight", "Style", "StyleColour", "StyleVanilla", "SecGround", "GroundColor", "GroundBrightness", "GroundOpacity", "Grid", "GridStep", "Contours", "ContourStep",
            "LineBrightness", "SecGeometry", "GeometryColor", "GeometryBrightness", "GeometryOpacity", "SecRoadsWater", "Roads", "RoadColor", "RoadWidth", "Water", "WaterColor",
            "SecOther", "DepthShade", "FadeTime", "ResetLook", "TabMap", "TabLook", "Enemies", "Guards", "Residents", "Followers", "Animals", "Doors", "Food", "Potions",
            "Chests", "Weapons", "Armor", "Loot", "Quests", "Bodies", "Plants", "Ores", "TreasureChests", "Clutter", "SecPosition", "Anchor", "TopLeft", "TopRight", "BottomLeft", "BottomRight",
            "OffsetX", "OffsetY", "SecQuests", "QuestBeam", "BeamKey", "LocalMap", "SecDebug", "DebugLog", "LocalMapKey", "ZoomInKey", "ZoomOutKey",
            "MinimapOpacity", "IconRange", "Everywhere", "ResetMap", "HintWorld", "HintClose", "Legend", "Unbind", "IconStyle", "ToggleHold", "MinimapFrame", "BeamNearest", "HideEmpty", "GroupIcons", "LocalMapHold", "FrameStyle", "FrameDrawn", "PointerCamera", "FrameOpacity", "IconOpacity", "NeedItem", "NeedItemId", "ItemFound", "ItemNotFound", "ShowWhere", "ShowEverywhere", "ShowInside", "ShowOutside", "LootHeight", "AllHeights" };

        // Data/SKSE/Plugins/DetailedMiniMap/Translations/<code>.txt, in the order of the Language setting
        constexpr const char* kCodes[kLanguages] = { "ru", "en", "fr", "it", "de", "es", "pl", "zh", "ja" };
        constexpr int         kEnglish = 1;

        struct Language
        {
            std::string                                            name;
            std::array<std::string, static_cast<int>(S::kCount)> texts;
        };

        std::string Trim(std::string_view a_s)
        {
            const auto first = a_s.find_first_not_of(" \t\r");
            const auto last = a_s.find_last_not_of(" \t\r");
            return first == std::string_view::npos ? std::string() : std::string(a_s.substr(first, last - first + 1));
        }

        // "Key = text" lines (";" starts a comment); what a file lacks: English, then the key itself
        const std::array<Language, kLanguages>& Languages()
        {
            static const auto languages = [] {
                std::array<std::unordered_map<std::string, std::string>, kLanguages> read;
                const auto dir = std::filesystem::current_path() / "Data/SKSE/Plugins/DetailedMiniMap/Translations";
                for (int i = 0; i < kLanguages; ++i) {
                    std::ifstream file(dir / std::format("{}.txt", kCodes[i]), std::ios::binary);
                    if (!file) {
                        logger::warn("translations: {}.txt not found", kCodes[i]);
                        continue;
                    }
                    std::string line;
                    while (std::getline(file, line)) {
                        if (line.starts_with("\xEF\xBB\xBF")) {
                            line.erase(0, 3);  // the BOM
                        }
                        const auto eq = line.find('=');
                        if (line.empty() || line[0] == ';' || line[0] == '#' || eq == std::string::npos) {
                            continue;
                        }
                        read[i][Trim(std::string_view(line).substr(0, eq))] = Trim(std::string_view(line).substr(eq + 1));
                    }
                }
                std::array<Language, kLanguages> out;
                for (int i = 0; i < kLanguages; ++i) {
                    const auto find = [&](int a_lang, const std::string& a_key) -> const std::string* {
                        const auto it = read[a_lang].find(a_key);
                        return it != read[a_lang].end() && !it->second.empty() ? &it->second : nullptr;
                    };
                    const auto pick = [&](const std::string& a_key) {
                        const auto own = find(i, a_key);
                        const auto english = find(kEnglish, a_key);
                        return own ? *own : english ? *english : a_key;
                    };
                    const auto name = find(i, "Name");
                    out[i].name = name ? *name : kCodes[i];
                    for (int t = 0; t < static_cast<int>(S::kCount); ++t) {
                        out[i].texts[t] = pick(kKeys[t]);
                    }
                }
                return out;
            }();
            return languages;
        }

        // the game's language (sLanguage:General), as one of ours; English if it is none of them
        int GameLanguage()
        {
            static const int language = [] {
                constexpr std::pair<std::string_view, int> kGame[] = { { "RUSSIAN", 0 }, { "ENGLISH", 1 }, { "FRENCH", 2 }, { "ITALIAN", 3 }, { "GERMAN", 4 },
                    { "SPANISH", 5 }, { "POLISH", 6 }, { "CHINESE", 7 }, { "JAPANESE", 8 } };
                const auto ini = RE::INISettingCollection::GetSingleton();
                const auto setting = ini ? ini->GetSetting("sLanguage:General") : nullptr;
                const auto value = setting && setting->GetType() == RE::Setting::Type::kString && setting->GetString() ? std::string(setting->GetString()) : std::string();
                for (const auto& [name, id] : kGame) {
                    if (_stricmp(value.c_str(), name.data()) == 0) {
                        return id;
                    }
                }
                return kEnglish;
            }();
            return language;
        }
    }

    const char* Name(int a_language)
    {
        return Languages()[std::clamp(a_language, 0, kLanguages - 1)].name.c_str();
    }

    int Current()
    {
        const int set = Settings::Map().language;
        return set >= 0 && set < kLanguages ? set : GameLanguage();
    }

    const char* T(S a_text)
    {
        return Languages()[Current()].texts[static_cast<int>(a_text)].c_str();
    }

    std::string L(S a_text)
    {
        return std::format("{}###t{}", T(a_text), static_cast<int>(a_text));
    }
}

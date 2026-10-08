#pragma once

#include "Canvas.h"

// Map icons: Data/Textures/DetailedMiniMap/icons/<style>/<file>.dds, a folder a style (made by tools/MakeIcons.java:
// Default and Vanilla; replaceable, or a new folder for a new style - square DDS of any size). A file missing everywhere
// falls back to a badge with a Font Awesome glyph (where the canvas has the font).
// Render thread (the main thread inside the map menu).
namespace Icons
{
    enum class Kind : std::uint8_t
    {
        kEnemy,
        kGuard,
        kNpc,
        kFollower,
        kCreature,
        kDoor,
        kFood,
        kPotion,
        kContainer,
        kWeapon,
        kArmor,
        kLoot,
        kQuest,     // a quest target (on the rim when off the map)
        kBody,      // a dead body not looked into yet
        kFlora,     // a plant to pick (not picked yet)
        kOre,       // an ore vein not mined out
        kTotal
    };
    inline constexpr std::size_t kCount = static_cast<std::size_t>(Kind::kTotal);

    const char* Name(Kind a_kind);  // for the menu, in its language
    const char* File(Kind a_kind);  // the texture's file name without .dds; also the ini key suffix

    // the styles: every folder of Data/Textures/DetailedMiniMap/icons (a new folder with its own <file>.dds is a new
    // style; a picture it lacks comes from Default)
    std::vector<std::string> Styles();
    void                     UseStyle(const std::string& a_style);

    // a_size: the badge's diameter in px
    void Draw(Canvas& a_canvas, Kind a_kind, float a_x, float a_y, float a_size, float a_alpha = 1.0f, float a_shade = 1.0f);  // a_shade: 1 as it is, less darker

    // the character: its badge (icons/player.dds) upright, a pointer on the rim turned to a_angle (radians, 0 = up,
    // clockwise on the screen)
    void DrawPlayer(Canvas& a_canvas, float a_x, float a_y, float a_size, float a_angle, float a_alpha = 1.0f);
}

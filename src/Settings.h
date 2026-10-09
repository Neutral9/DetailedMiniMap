#pragma once

// DetailedMiniMap.ini (shipped defaults) + DetailedMiniMap_User.ini (what the menu saves), key by key
namespace Settings
{
    struct MapSettings
    {
        bool          enabled = true;
        // the keys: a key code (keyboard scan code; 256 + mouse button, 264 / 265 the wheel up / down; 266 + gamepad button,
        // as SKSE counts them) and a key that must be held with it (0 = none)
        std::uint32_t toggleKey = 49;        // DirectInput scan code (49 = N): the minimap shown / hidden, fading (0 = none)
        std::uint32_t toggleMod = 0;
        std::uint32_t toggleKeyPad = 0;     // ...and on the gamepad (both work; 0 = none)
        std::uint32_t toggleModPad = 0;
        bool          toggleHold = false;    // the minimap shown only while its key is held (else the key switches it)
        bool          visible = true;        // shown (the key switches it; remembered)
        int           language = -1;       // the menu's language (Lang order: ru en fr it de es pl zh ja); -1 = the game's
        float         minimapSize = 600.0f;  // pixels at 1080p
        float         minimapRange = 3700.0f;  // world units from the centre to the edge (outside)
        float         minimapRangeInside = 1200.0f;  // ...in caves and houses
        float         minimapTilt = 55.0f;   // degrees above the floor the minimap is seen from (90 = straight down)
        bool          northUp = false;       // the minimap keeps north up instead of turning with the camera
        bool          minimapRound = true;   // a round minimap instead of the square
        float         minimapCorner = 40.0f; // the square minimap: its corners rounded this far (px at 1080p; 0 = sharp)
        bool          minimapFrame = true;   // its frame (the vanilla style's or the ring) drawn
        int           anchor = 1;            // the screen corner the minimap sits in: 0 top left, 1 top right, 2 bottom left, 3 bottom right
        float         offsetX = 18.0f;       // ...this far from that corner (px at 1080p)
        float         offsetY = 18.0f;
        // icons
        float         iconSize = 1.0f;       // scale of every icon
        float         iconFadeIn = 0.35f;    // seconds an icon takes to show up
        float         iconFadeOut = 0.35f;   // ...and to go once its thing is gone (picked up, dead, out of range)
        float         iconRange = 0.0f;      // icons only for things this close to the character (world units; 0 = as far as the map shows; quests always)
        float         minimapOpacity = 1.0f; // the whole minimap (picture, frame, icons) this opaque
        bool          hideEmpty = true;      // containers with nothing to take left off
        bool          groupIcons = true;     // icons of one kind overlapping on the map merged into one
        std::array<bool, 16> show{ true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true };  // per icon kind (Icons::Kind order); the menu toggles them
        bool          cut = true;            // cut away what is over the character (roofs, ceilings)
        float         cutHeight = 80.0f;     // inside: geometry this far over the feet is cut away (roofs, ceilings)
        // quests
        bool          questBeam = true;      // beams from the character to the quest targets
        bool          beamNearest = true;    // one beam only, to the nearest target of any quest (by the whole way, through doors); else one to each quest set active in the journal
        std::uint32_t beamKey = 48;          // its key (48 = B; 0 = none): the beam shown / hidden
        std::uint32_t beamMod = 0;
        std::uint32_t beamKeyPad = 0;     // ...and on the gamepad (both work; 0 = none)
        std::uint32_t beamModPad = 0;
        // the game's local map (in the map menu) replaced by this one: dragged with the mouse, zoomed with the wheel
        bool          localMap = true;
        std::uint32_t localMapKey = 38;      // the key that opens the local map straight from the game (38 = L; 0 = none)
        std::uint32_t localMapMod = 0;
        std::uint32_t localMapKeyPad = 0;     // ...and on the gamepad (both work; 0 = none)
        std::uint32_t localMapModPad = 0;
        bool          localMapHold = false;  // the local map open only while its key is held (else the key opens and closes it)
        // the minimap's range (outside or inside, where the character is) zoomed in and out: Shift + the wheel
        std::uint32_t zoomInKey = 264;
        std::uint32_t zoomInMod = 42;
        std::uint32_t zoomInKeyPad = 0;     // ...and on the gamepad (both work; 0 = none)
        std::uint32_t zoomInModPad = 0;
        std::uint32_t zoomOutKey = 265;
        std::uint32_t zoomOutMod = 42;
        std::uint32_t zoomOutKeyPad = 0;     // ...and on the gamepad (both work; 0 = none)
        std::uint32_t zoomOutModPad = 0;
        bool          debugLog = false;      // a detailed log (timings, rebuilds, the reasons) in DetailedMiniMap.log
        // the look (the menu's second page; {} = the defaults)
        struct Look
        {
            bool  grid = false;                               // lines on the ground every gridSize units
            float gridSize = 128.0f;
            bool  contours = false;                           // height lines every contourStep units
            float contourStep = 64.0f;
            float lineStrength = 1.0f;                        // how bright both kinds of lines are
            float groundColor[3] = { 0.333f, 0.466f, 0.229f };  // the landscape (inside: flat floors)
            float groundBrightness = 1.0f;
            float groundOpacity = 1.0f;
            float geometryColor[3] = { 0.597f, 0.607f, 0.626f };  // everything else: buildings, rocks, furniture...
            float geometryBrightness = 1.0f;
            float geometryOpacity = 1.0f;
            float depthShade = 0.5f;                          // how dark the lowest levels get (0 = not at all)
            float fadeTime = 0.8f;                            // seconds a newly scanned part takes to fade in
            // roads: the navmesh's preferred triangles (orange in the Creation Kit), drawn over the ground
            bool  roads = true;
            float roadColor[3] = { 0.632f, 0.486f, 0.262f };
            float roadWidth = 1.0f;                           // 1 .. 3: 1 = as the navmesh has it, every step past 1 widens by 150 units a side
            bool  water = true;                               // lakes, rivers, the sea
            float waterColor[3] = { 0.146f, 0.392f, 0.637f };
            int   style = 1;                                  // 0 = the colours above, 1 = vanilla (the game's local map: sepia, its frame)
            // the icons: a folder of Data/Textures/DetailedMiniMap/icons (every folder there is a style to pick)
            std::string iconStyle = "Vanilla";
        } look;
    };

    MapSettings& Map();

    void Load();
    void Save();
    void ResetMap();   // the map page to the defaults (DetailedMiniMap.ini, else the built-in ones); the language stays
    void ResetLook();  // the look page to the defaults; the style stays

    std::string KeyName(std::uint32_t a_key);                       // for the menu
    std::string BindName(std::uint32_t a_key, std::uint32_t a_mod);  // "Left Shift + Wheel Up"
}

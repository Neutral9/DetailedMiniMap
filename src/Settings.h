#pragma once

// DetailedMiniMap.ini (shipped defaults) + DetailedMiniMap_User.ini (what the menu saves), key by key
namespace Settings
{
    struct MapSettings
    {
        bool          enabled = true;
        int           showWhere = 0;         // the minimap shown 0 everywhere, 1 only inside (caves, houses), 2 only outside
        // the keys: a key code (keyboard scan code; 256 + mouse button, 264 / 265 the wheel up / down; 266 + gamepad button,
        // as SKSE counts them) and a key that must be held with it (0 = none)
        std::uint32_t toggleKey = 49;        // DirectInput scan code (49 = N): the minimap shown / hidden, fading (0 = none)
        std::uint32_t toggleMod = 0;
        std::uint32_t toggleKeyPad = 269;   // ...and on the gamepad (both work; 0 = none): D-pad right
        std::uint32_t toggleModPad = 0;
        bool          toggleHold = false;    // the minimap shown only while its key is held (else the key switches it)
        bool          visible = true;        // shown (the key switches it; remembered)
        int           language = -1;       // the menu's language (Lang order: ru en fr it de es pl zh ja); -1 = the game's
        float         minimapSize = 260.0f;  // pixels at 1080p
        float         minimapRange = 3000.0f;  // world units from the centre to the edge (outside)
        float         minimapRangeInside = 1200.0f;  // ...in caves and houses
        float         minimapTilt = 55.0f;   // degrees above the floor the minimap is seen from (90 = straight down)
        bool          northUp = false;       // the minimap keeps north up instead of turning with the camera
        bool          compass = true;        // N, E, S, W on the rim (the minimap, the local map): where north is as the map turns
        float         compassSize = 1.0f;    // its letters this big (0.5 .. 2.5)
        bool          legendOpen = true;     // the local map's legend unfolded (its button folds it)
        // the minimap's range following the pace: closer slow (a low speed), farther sprinting or riding (GTA, RDR)
        bool          dynamicZoom = true;
        float         dynamicZoomStrength = 1.5f;  // 0.25 .. 2
        // the characters only while a detect spell is on (Detect Life, Detect Dead, Aura Whisper): 0 always shown,
        // 1 the enemies only then, 2 every character but the followers only then
        int           detectOnly = 0;
        bool          minimapRound = true;   // a round minimap instead of the square
        float         minimapCorner = 40.0f; // the square minimap: its corners rounded this far (px at 1080p; 0 = sharp)
        bool          minimapFrame = true;   // its frame (the vanilla style's or the ring) drawn
        bool          pointerCamera = false; // the character's pointer shows where the camera looks (else where the body faces)
        int           anchor = 1;            // the screen corner the minimap sits in: 0 top left, 1 top right, 2 bottom left, 3 bottom right, 4 the middle
        float         offsetX = 18.0f;       // ...this far from that corner (from the middle: off it, either way) (px at 1080p)
        float         offsetY = 18.0f;
        // icons
        float         iconSize = 1.2f;       // scale of every icon
        float         iconFadeIn = 0.35f;    // seconds an icon takes to show up
        float         iconFadeOut = 0.35f;   // ...and to go once its thing is gone (picked up, dead, out of range)
        float         iconRange = 3000.0f;      // icons only for things this close to the character (world units; 0 = as far as the map shows; quests always)
        float         lootHeight = 0.0f;     // inside: things to take (not characters, doors, quests) only this far over or under the feet (world units; 0 = all) - another storey's loot left off
        float         minimapOpacity = 1.0f; // the whole minimap (picture, frame, icons) this opaque
        bool          hideEmpty = true;      // containers with nothing to take left off
        bool          groupIcons = true;     // icons of one kind overlapping on the map merged into one
        std::array<bool, 21> show{ true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true };  // per icon kind (Icons::Kind order); the menu toggles them
        bool          cut = true;            // cut away what is over the character (roofs, ceilings)
        float         cutHeight = 80.0f;     // inside: geometry this far over the feet is cut away (roofs, ceilings)
        // quests
        bool          questBeam = true;      // beams from the character to the quest targets
        bool          beamNearest = false;    // one beam only, to the nearest target of any quest (by the whole way, through doors); else one to each quest set active in the journal
        bool          destinationBeam = true;  // a beam to the destination set on the world map (Set Destination) too
        std::uint32_t beamKey = 48;          // its key (48 = B; 0 = none): the beam shown / hidden
        std::uint32_t beamMod = 0;
        std::uint32_t beamKeyPad = 266;   // ...and on the gamepad (both work; 0 = none): D-pad up
        std::uint32_t beamModPad = 0;
        // the game's local map (in the map menu) replaced by this one: dragged with the mouse, zoomed with the wheel
        bool          localMap = true;
        std::uint32_t localMapKey = 38;      // the key that opens the local map straight from the game (38 = L; 0 = none)
        std::uint32_t localMapMod = 0;
        std::uint32_t localMapKeyPad = 268;   // ...and on the gamepad (both work; 0 = none): D-pad left
        std::uint32_t localMapModPad = 0;
        bool          localMapHold = false;  // the local map open only while its key is held (else the key opens and closes it)
        // the local map turned and tilted: the right mouse button held (the gamepad: its right stick)
        bool          rotateInvertX = false;  // the turn the other way round (left / right)
        bool          rotateInvertY = false;  // the tilt the other way round (up / down)
        float         rotateSpeed = 1.0f;     // how fast it turns (0.2 .. 3)
        // the minimap and the local map only while the character carries an item (read with NeedItemId: the menu edits it)
        bool          needItem = false;
        std::string   needItemId;            // its EditorID, or "Plugin.esp|0x800" (its form id in that plugin)
        bool          needMapMod = false;    // ...and one of Helps To Have A Map's maps for where the character is (that mod loaded)
        // the minimap's range (outside or inside, where the character is) zoomed in and out: Shift + the wheel
        std::uint32_t zoomInKey = 264;
        std::uint32_t zoomInMod = 42;
        std::uint32_t zoomInKeyPad = 281;   // ...and on the gamepad (both work; 0 = none): RT, held
        std::uint32_t zoomInModPad = 0;
        std::uint32_t zoomOutKey = 265;
        std::uint32_t zoomOutMod = 42;
        std::uint32_t zoomOutKeyPad = 280;   // ...and on the gamepad (both work; 0 = none): LT, held
        std::uint32_t zoomOutModPad = 0;
        // the fog of war: the map hidden where the character has not been (what was seen kept in each save's .skse)
        bool          fog = true;
        bool          fogMinimap = true;      // on the minimap
        bool          fogLocal = true;        // ...on the local map
        float         fogRadius = 2500.0f;    // how far round the character is seen (world units): outside
        float         fogRadiusInside = 1000.0f;  // ...inside
        bool          fogHideIcons = true;    // the icons under the fog hidden too (not the quests')
        bool          debugLog = false;      // a detailed log (timings, rebuilds, the reasons) in DetailedMiniMap.log
        bool          padBinds = false;      // the gamepad's default binds given once (an older ini had none)
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
            bool  seeUnderwater = true;                       // the character under water: the water seen through (the bottom, wrecks, chests)
            float waterColor[3] = { 0.146f, 0.392f, 0.637f };
            int   style = 1;                                  // 0 = the colours above, 1 = vanilla (the game's local map: sepia, its frame)
            // the icons: a folder of Data/Textures/DetailedMiniMap/icons (every folder there is a style to pick)
            std::string iconStyle = "Vanilla";
            // the minimap's frame: a folder of Data/Textures/DetailedMiniMap/frames, or empty: the drawn one (the style's)
            std::string frameStyle = "Leather";
            float       frameOpacity = 1.0f;  // the minimap's frame this opaque (the picture, the vanilla one or the ring)
            float       iconOpacity = 1.0f;   // the icons (not the character's) this opaque
            float       frameColor[3] = { 1.0f, 1.0f, 1.0f };  // the frame tinted (white: as it is)
            float       beamColor[3] = { 1.0f, 0.769f, 0.314f };  // the quest beams: their glow (the core and the running light lighter)
            float       beamWidth = 1.0f;                        // 0.5 .. 3
            float       beamOpacity = 1.0f;
            bool        beamLights = true;                       // the light running along the beams
            bool        beamColours = true;                      // several beams (one a quest): each its own colour, the first as set
            float       destinationColor[3] = { 0.40f, 0.95f, 0.85f };  // the beam to the destination set on the world map
            // the fog of war's look
            float       fogColor[3] = { 0.075f, 0.08f, 0.095f };
            float       fogOpacity = 1.0f;  // how thick: 1 hides all, less lets the map show through
            float       fogFade = 0.6f;     // seconds a place takes to clear once seen
            float       fogNoise = 1.0f;    // its edge broken up this much (0: a smooth round edge)
        } look;
    };

    MapSettings& Map();

    void Load();
    void Save();
    void ResetMap();   // the map page to the defaults (DetailedMiniMap.ini, else the built-in ones), the language too (the game's)
    void ResetLook();  // the look page to the defaults; the style stays

    // the item the map needs: copies, as the menu edits it while the game reads it
    std::string NeedItemId();
    void        SetNeedItemId(std::string a_id);

    // presets: Data/SKSE/Plugins/DetailedMiniMap/Presets/<name>.ini - every setting but the keys, the language and the
    // item the map needs (a preset loaded: the defaults, then what it sets)
    inline constexpr std::string_view kDefaultPreset = "Default";  // the built-in preset: the defaults (first in the list, never a file)
    bool                     IsDefaultPreset(std::string_view a_name);
    std::vector<std::string> Presets();
    bool                     SavePreset(const std::string& a_name);
    bool                     LoadPreset(const std::string& a_name);
    bool                     DeletePreset(const std::string& a_name);

    std::string KeyName(std::uint32_t a_key);                       // for the menu
    std::string BindName(std::uint32_t a_key, std::uint32_t a_mod);  // "Left Shift + Wheel Up"
}

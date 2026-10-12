#pragma once

// The 3D map: the game's own render geometry - every mesh of the loaded cells (statics, architecture, rocks,
// pebbles, stairs, furniture, clutter) and the landscape - drawn by a small D3D11 renderer of our own into a texture
// (depth tested, 4x MSAA, shaded), which the HUD shows as the minimap. Gathered without hitches: the loaded references
// are looked at a frame's share at a time (and at once when the game says one's 3D came or went), each mesh is decoded
// once on a thread of its own into a model shared by every reference placing it, and the models go up to the GPU a
// frame's budget at a time; the pictures draw them instanced.
// Inside, what stands higher than the character's head is cut away (ceilings, upper floors); everywhere, whatever
// covers the character on the picture (roofs, crowns, bridges); lower floors fade into the dark. A cell new to the
// map fades in. Water (the water shader's meshes and the cells' water planes) is drawn as water; roads come from
// the navmesh's preferred triangles, drawn into a mask per picture.
namespace MapMesh
{
    // main thread: geometry harvest - only within a_radius of the character (what the map shows); 0: nothing
    // shown, nothing done (what is loaded stays); < 0: the map off, the geometry unloaded
    void Update(RE::PlayerCharacter* a_player, float a_delta, float a_radius);

    // once the game's data is loaded: the shaders start compiling on a thread of their own
    void Prepare();

    // one picture of the map, a tilted orthographic view (built on the main thread, drawn on the render thread)
    struct View
    {
        int          width = 0, height = 0;
        int          slot = 0;         // which picture: 0 the minimap, 1 the local map (each its own texture)
        float        rows[4][4]{};  // world point -> clip space
        RE::NiPoint3 player;
        float        cut = 250.0f;     // geometry this far above the character's feet is cut away
        float        range = 5000.0f;  // meshes further than this from the character are not drawn
        RE::FormID   space = 0;
        // the look (Settings::Map().look): rgb * brightness and opacity; grid size, contour step (0 = off), line
        // strength; depth shade, interior, style (0 colour, 1 vanilla)
        float        ground[4]{}, geometry[4]{}, lines[4]{}, shade[4]{};
        float        occluder[4]{}, occluder2[4]{};  // the occlusion cut: the character on this picture (px centre, px radii; depth, feet z)
        float        fadeTime = 0.8f;  // seconds a mesh new to the map takes to fade in
        float        roads[4]{};       // rgb, a: 0 no roads, 1 + world units they are widened by
        float        water[4]{};       // rgb, a = 1: water drawn
        float        under[4]{};       // x the water's level, y 0..1 the character under water: the water fades away, what lies below its level is tinted
        float        fog[4]{};         // the fog of war: rgb, a how thick (0: none on this picture)
        float        fogNoise = 1.0f;  // its edge broken up this much (0: smooth)
    };
    void* Render(const View& a_view);  // render thread: the picture as an ImTextureID, nullptr if not ready

    RE::FormID CurrentSpace();  // main thread

    // main thread, a game loaded: Data/SKSE/Plugins/DetailedMiniMap/MeshFilter.json read again (changed: the map rebuilt)
    void ReloadFilter();

    // main thread, the detailed log: what the minimap's whole update took this frame (ms)
    void NoteFrame(float a_ms);

    // what the map costs, for the on-screen panel (the detailed log on): taken once a second
    struct Stats
    {
        bool        valid = false;
        float       fps = 0.0f, frameMs = 0.0f;
        float       updateMs = 0.0f, updateMax = 0.0f;  // the minimap's whole update a frame (main thread)
        float       workMs = 0.0f, workMax = 0.0f;      // ...the gathering in it
        float       renderMs = 0.0f, renderMax = 0.0f;  // a picture drawn (render thread, CPU)
        int         redraws = 0, reused = 0;            // pictures drawn, shown again as they were (a second)
        int         refsTaken = 0, modelsMade = 0, uploads = 0, passes = 0;  // a second
        std::size_t refs = 0, placed = 0, placedTriangles = 0;
        std::size_t models = 0, shared = 0, onGpu = 0, decoding = 0, failed = 0;
        std::size_t decodeQueue = 0, gpuReads = 0, events = 0;
        float       gpuMB = 0.0f, picturesMB = 0.0f, cpuMB = 0.0f;
        std::size_t drawnInstances = 0, drawnTriangles = 0, drawCalls = 0;  // the last picture
    };
    Stats Live();  // any thread
}

#pragma once

// The 3D map: the game's own render geometry - every mesh of the loaded cells (statics, architecture, rocks,
// pebbles, stairs, furniture, clutter) and the landscape - copied into world space per cell and drawn by a small
// D3D11 renderer of our own into a texture (depth tested, 4x MSAA, shaded), which the HUD shows as the minimap.
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
    };
    void* Render(const View& a_view);  // render thread: the picture as an ImTextureID, nullptr if not ready

    RE::FormID CurrentSpace();  // main thread
}

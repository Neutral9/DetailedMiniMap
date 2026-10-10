#pragma once

// What the HUD shows of the 3D map (MapMesh): the minimap in a corner of the screen (tilted, turning with the camera
// or north up, square or round), shown and hidden by its key with a fade; and, in the map menu, the game's local map
// replaced by a large one (dragged with the mouse, zoomed with the wheel). Markers on them: characters, bodies, load
// doors, containers, items, plants, quest targets (on the rim when past it) and a beam to the nearest quest target.
namespace MiniMap
{
    void Register();  // HUD element and event sinks, after the Menu Framework is up
    void Install();   // hooks: the per-frame updates (the HUD, the map menu)

    // the menu's "press a key" button: the next press (keyboard, mouse, gamepad) is written into a_slot, a key held meanwhile into a_modSlot
    // the next key pressed goes into a_slot / a_modSlot - a gamepad's into a_padSlot / a_padModSlot when given (each device its own bind)
    void StartCapture(std::uint32_t* a_slot, std::uint32_t* a_modSlot, std::uint32_t* a_padSlot = nullptr, std::uint32_t* a_padModSlot = nullptr);
    bool IsCapturing(const std::uint32_t* a_slot = nullptr);
    bool OnFrameworkInput(RE::InputEvent* a_event);  // from SKSE Menu Framework; true = swallow the event

    void OnGameLoaded();  // a save loaded or a new game: the bodies looked into are forgotten (their ids mean others now)

    // the menu: what the map's item id finds (its name), nothing when it finds nothing
    std::optional<std::string> ItemName(std::string_view a_id);
}

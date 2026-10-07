#pragma once

// What the HUD shows of the 3D map (MapMesh): the minimap square in the top right corner (tilted, turning with the
// camera or north up, or round), shown and hidden by its key with a fade. Markers on it: enemies, followers, load
// doors, containers, items.
namespace MiniMap
{
    void Register();  // HUD element, after the Menu Framework is up
    void Install();   // hook: the per-frame update

    // the menu's "press a key" button: the next keyboard press is written into a_slot (a key setting)
    void StartCapture(std::uint32_t* a_slot);
    bool IsCapturing(const std::uint32_t* a_slot = nullptr);
    bool OnFrameworkInput(RE::InputEvent* a_event);  // from SKSE Menu Framework; true = swallow the event
}

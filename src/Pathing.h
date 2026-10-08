#pragma once

// A walking path over the navmeshes of the loaded cells (the ones the game's characters walk by), for the beam to a
// quest target: the navmeshes are copied when the loaded cells change (main thread), the path is searched on a
// thread of its own (A* over the triangles, then pulled tight through the edges it crosses). A target outside the
// loaded navmesh: the path goes as near to it as the loaded part allows.
namespace Pathing
{
    // main thread, a few times a second: the navmeshes taken again when the loaded cells changed
    void Update(RE::PlayerCharacter* a_player);

    // main thread: a path from a_from to a_to wanted (the last one stays until the new one is found)
    void Request(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to);

    // the last path found and the target it was for; empty: none (yet)
    std::vector<RE::NiPoint3> Path(RE::NiPoint3& a_target);

    void Clear();  // another world space, the map off
}

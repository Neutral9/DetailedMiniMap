#pragma once

// Walking paths over the navmeshes of the loaded cells (the ones the game's characters walk by), for the beams to
// quest targets: the navmeshes are copied when the loaded cells change (main thread), the paths are searched on a
// thread of their own (A* over the triangles, then pulled tight through the edges they cross). A target outside the
// loaded navmesh: the path goes as near to it as the loaded part allows.
namespace Pathing
{
    struct Found
    {
        RE::NiPoint3              target;
        std::vector<RE::NiPoint3> path;  // empty: none
    };

    // main thread, a few times a second: the navmeshes taken again when the loaded cells changed
    void Update(RE::PlayerCharacter* a_player);

    // main thread: paths from a_from to each of a_to wanted (the last ones stay until the new ones are found)
    void Request(const RE::NiPoint3& a_from, const std::vector<RE::NiPoint3>& a_to);

    // the last paths found, with the targets they were for
    std::vector<Found> Paths();
    std::uint64_t      Version();  // changes with every new set of paths

    void Clear();  // another world space, the map off
}

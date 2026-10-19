#pragma once

// The fog of war: where the character has been, per world space (and per interior), a grid of how far each square is
// seen (0 .. 255) - kept in the SKSE co-save of each save (its .skse beside it: the game's own save does not grow, a
// new game starts clear, an older save loads as it was then). Only the squares round where one has been are kept,
// in blocks, run-length packed. Seen as one goes: round the character, softly towards the edge, easing in.
namespace Fog
{
    void Register();  // at load: the co-save's callbacks

    // the journal's Miscellaneous entry: set active (its quests' targets shown) or not - kept in the same co-save (the
    // game keeps it nowhere we can read it; read off the journal while it is up)
    bool MiscShown();
    void NoteMiscShown(bool a_shown);

    // the containers looked into (their menu opened), with the day they were: off the map for a while (kept in the same co-save)
    bool Searched(RE::FormID a_ref);
    void NoteSearched(RE::FormID a_ref);

    // the menu: all that was seen in this game forgotten (the fog whole again; the next save keeps it so)
    void Clear();

    // main thread, every frame: what is round the character seen
    void Update(RE::PlayerCharacter* a_player, float a_delta);

    // main thread: how far a point of a_space has been seen (0 not, 1 all)
    float Seen(RE::FormID a_space, float a_x, float a_y);

    // the window of the fog the map's pictures read (a texture kTex a side): round the character, moved by blocks
    constexpr int kTex = 512;
    struct Window
    {
        RE::FormID    space = 0;
        int           ox = 0, oy = 0;  // the square at texel 0, 0
        float         unit = 0.0f;     // world units a square
        std::uint64_t version = 0;     // the fog as it was when taken
        std::uint32_t generation = 0;  // ...from which save (another loaded: all of it again)
    };
    // render thread: a_io brought up to the fog round a_x, a_y in a_space; a_texels (kTex * kTex, kept by the caller)
    // filled where it changed - a_rect (x0, y0, x1, y1 exclusive) that part; false when nothing changed
    bool Take(RE::FormID a_space, float a_x, float a_y, Window& a_io, std::vector<std::uint8_t>& a_texels, int (&a_rect)[4]);

    // the detailed log: the fog's state on the CPU (the settings, where the character is, what is seen there, what it did
    // since the last report)
    std::string Report();
}

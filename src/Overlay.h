#pragma once

#include "Canvas.h"

// 2D sprites drawn with our own small D3D11 pass into whatever the game is drawing its menus to: the local map is drawn
// right after the map menu's own movie, before the cursor menu, so the game's cursor stays on top of it.
namespace Overlay
{
    struct Vertex
    {
        float         x, y;    // screen px (the ImGui display's)
        float         u, v;    // texture coordinates; a disc's or a line's own -1..1
        std::uint32_t color;   // 0xAABBGGRR
        float         mode;    // 0 textured, 1 solid, 2 disc, 3 line (soft edges)
    };

    struct Batch
    {
        void*               texture = nullptr;  // ID3D11ShaderResourceView, nullptr: none textured in it yet
        std::vector<Vertex> vertices;           // triangles
    };

    // a Canvas that collects sprites
    class Sprites final : public Canvas
    {
    public:
        void Image(void* a_texture, V2 a_min, V2 a_max, Color a_tint) override;
        void Fill(V2 a_min, V2 a_max, Color a_color) override;
        void Outline(V2 a_min, V2 a_max, Color a_color, float a_width) override;
        void Line(V2 a_from, V2 a_to, Color a_color, float a_width) override;
        void Disc(V2 a_centre, float a_radius, Color a_color) override;
        void Ring(V2 a_centre, float a_radius, Color a_color, float a_width) override;
        void Triangle(V2 a_a, V2 a_b, V2 a_c, Color a_color) override;

        std::vector<Batch> batches;

    private:
        std::vector<Vertex>& Into(void* a_texture);
        void                 Quad(void* a_texture, V2 a_p0, V2 a_p1, V2 a_p2, V2 a_p3, float a_u0, float a_v0, float a_u1, float a_v1, Color a_color, float a_mode);
    };

    void Prepare();  // once the game's data is loaded: the shaders compile on a thread of their own

    // render thread (inside a menu's drawing): the sprites over what is drawn so far, a_w x a_h the ImGui display (its
    // pixels are scaled to the target's), clipped to a_clip (x0 y0 x1 y1 in those pixels); false: nothing could be drawn
    bool Draw(const std::vector<Batch>& a_batches, float a_w, float a_h, const float (&a_clip)[4]);
}

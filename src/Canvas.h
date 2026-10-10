#pragma once

// What the map's markers are drawn with: the HUD's ImGui (the minimap), or our own sprites drawn inside the map menu
// (the local map: under the game's cursor). Screen pixels; colours ImGui's packed 0xAABBGGRR.
struct Canvas
{
    struct V2
    {
        float x, y;
    };
    using Color = std::uint32_t;

    virtual ~Canvas() = default;

    virtual void Image(void* a_texture, V2 a_min, V2 a_max, Color a_tint) = 0;  // a_texture: an ID3D11ShaderResourceView
    virtual void Fill(V2 a_min, V2 a_max, Color a_color) = 0;
    virtual void Outline(V2 a_min, V2 a_max, Color a_color, float a_width) = 0;
    virtual void Line(V2 a_from, V2 a_to, Color a_color, float a_width) = 0;
    virtual void Disc(V2 a_centre, float a_radius, Color a_color) = 0;
    virtual void Ring(V2 a_centre, float a_radius, Color a_color, float a_width) = 0;
    virtual void Triangle(V2 a_a, V2 a_b, V2 a_c, Color a_color) = 0;
    // the texture on any four corners (turned): a_p0 its top left, then clockwise
    virtual void ImageQuad(void* a_texture, V2 a_p0, V2 a_p1, V2 a_p2, V2 a_p3, Color a_tint) = 0;
    // a Font Awesome glyph centred on a_centre; false: this canvas has no font
    virtual bool Glyph(unsigned int, V2, float, Color) { return false; }
};

namespace ImGuiMCP
{
    struct ImDrawList;
}

// the HUD's ImGui (render thread, inside an ImGui frame)
class ImGuiCanvas final : public Canvas
{
public:
    explicit ImGuiCanvas(ImGuiMCP::ImDrawList* a_list) :
        list(a_list) {}

    void Image(void* a_texture, V2 a_min, V2 a_max, Color a_tint) override;
    void Fill(V2 a_min, V2 a_max, Color a_color) override;
    void Outline(V2 a_min, V2 a_max, Color a_color, float a_width) override;
    void Line(V2 a_from, V2 a_to, Color a_color, float a_width) override;
    void Disc(V2 a_centre, float a_radius, Color a_color) override;
    void Ring(V2 a_centre, float a_radius, Color a_color, float a_width) override;
    void Triangle(V2 a_a, V2 a_b, V2 a_c, Color a_color) override;
    void ImageQuad(void* a_texture, V2 a_p0, V2 a_p1, V2 a_p2, V2 a_p3, Color a_tint) override;
    bool Glyph(unsigned int a_glyph, V2 a_centre, float a_size, Color a_color) override;

    ImGuiMCP::ImDrawList* list;
};

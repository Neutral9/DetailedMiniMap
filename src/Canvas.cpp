#include "Canvas.h"

// third-party header: deprecated <codecvt>, mixed enums and so on - not our warnings
#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
#pragma warning(push, 0)
#include <SKSEMenuFramework.h>
#pragma warning(pop)

namespace ImGui = ImGuiMCP;
namespace D = ImGuiMCP::ImDrawListManager;

namespace
{
    ImGui::ImVec2 Im(Canvas::V2 a_p) { return { a_p.x, a_p.y }; }
}

void ImGuiCanvas::Image(void* a_texture, V2 a_min, V2 a_max, Color a_tint)
{
    D::AddImage(list, reinterpret_cast<ImGui::ImTextureID>(a_texture), Im(a_min), Im(a_max), ImGui::ImVec2{ 0.0f, 0.0f }, ImGui::ImVec2{ 1.0f, 1.0f }, a_tint);
}

void ImGuiCanvas::Fill(V2 a_min, V2 a_max, Color a_color)
{
    D::AddRectFilled(list, Im(a_min), Im(a_max), a_color, 0.0f, 0);
}

void ImGuiCanvas::Outline(V2 a_min, V2 a_max, Color a_color, float a_width)
{
    D::AddRect(list, Im(a_min), Im(a_max), a_color, 0.0f, 0, a_width);
}

void ImGuiCanvas::Line(V2 a_from, V2 a_to, Color a_color, float a_width)
{
    D::AddLine(list, Im(a_from), Im(a_to), a_color, a_width);
}

void ImGuiCanvas::Disc(V2 a_centre, float a_radius, Color a_color)
{
    D::AddCircleFilled(list, Im(a_centre), a_radius, a_color, a_radius > 8.0f ? 24 : 12);
}

void ImGuiCanvas::Ring(V2 a_centre, float a_radius, Color a_color, float a_width)
{
    D::AddCircle(list, Im(a_centre), a_radius, a_color, a_radius > 40.0f ? 96 : 24, a_width);
}

void ImGuiCanvas::Triangle(V2 a_a, V2 a_b, V2 a_c, Color a_color)
{
    D::AddTriangleFilled(list, Im(a_a), Im(a_b), Im(a_c), a_color);
}

bool ImGuiCanvas::Glyph(unsigned int a_glyph, V2 a_centre, float a_size, Color a_color)
{
    FontAwesome::PushSolid();
    const auto font = ImGui::GetFont();
    bool       drawn = false;
    if (font && ImGui::ImFontManger::FindGlyphNoFallback(font, static_cast<ImGui::ImWchar>(a_glyph))) {
        const unsigned cp = a_glyph;
        const char     text[4] = { static_cast<char>(0xE0 | (cp >> 12)), static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), static_cast<char>(0x80 | (cp & 0x3F)), 0 };
        const float    base = ImGui::GetFontSize();
        const auto     ts = ImGui::CalcTextSize(text);
        const float    k = base > 0.0f ? a_size / base : 1.0f;
        D::AddText(list, font, a_size, ImGui::ImVec2{ a_centre.x - ts.x * k * 0.5f, a_centre.y - ts.y * k * 0.5f }, a_color, text);
        drawn = true;
    }
    FontAwesome::Pop();
    return drawn;
}

void ImGuiCanvas::ImageQuad(void* a_texture, V2 a_p0, V2 a_p1, V2 a_p2, V2 a_p3, Color a_tint)
{
    D::AddImageQuad(list, reinterpret_cast<ImGui::ImTextureID>(a_texture), Im(a_p0), Im(a_p1), Im(a_p2), Im(a_p3), ImGui::ImVec2{ 0.0f, 0.0f }, ImGui::ImVec2{ 1.0f, 0.0f },
        ImGui::ImVec2{ 1.0f, 1.0f }, ImGui::ImVec2{ 0.0f, 1.0f }, a_tint);
}

#include "Icons.h"

#include "Lang.h"

// third-party header: deprecated <codecvt>, mixed enums and so on - not our warnings
#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
#pragma warning(push, 0)
#include <SKSEMenuFramework.h>
#pragma warning(pop)

namespace ImGui = ImGuiMCP;

namespace Icons
{
    namespace
    {
        struct Style
        {
            const char*  file;
            unsigned int glyph;  // fallback: Font Awesome 6 Free Solid codepoint, 0 = drawn
            int          r, g, b;
        };
        // in Kind order
        constexpr Style kStyles[] = {
            { "enemy", 0xF54C, 235, 70, 55 },
            { "guard", 0xF505, 90, 150, 235 },
            { "npc", 0xF007, 215, 215, 215 },
            { "follower", 0xF007, 90, 205, 120 },
            { "creature", 0xF1B0, 190, 165, 130 },
            { "door", 0xF52B, 240, 150, 50 },
            { "food", 0xF6D7, 230, 190, 90 },
            { "potion", 0xF0C3, 215, 95, 225 },
            { "container", 0xF187, 195, 135, 75 },
            { "weapon", 0, 200, 210, 225 },
            { "armor", 0xF3ED, 140, 180, 220 },
            { "loot", 0xF51E, 245, 210, 90 },
            { "quest", 0x21, 255, 200, 60 },
            { "body", 0xF714, 175, 170, 160 },
            { "flora", 0xF06C, 120, 200, 90 },
        };
        static_assert(std::size(kStyles) == kCount);

        constexpr ImGui::ImU32 Rgb(int r, int g, int b, int a)
        {
            return (static_cast<ImGui::ImU32>(a) << 24) | (static_cast<ImGui::ImU32>(b) << 16) | (static_cast<ImGui::ImU32>(g) << 8) | static_cast<ImGui::ImU32>(r);
        }

        // the textures, loaded on first use (0 = no file: the fallback)
        ImGui::ImTextureID Texture(Kind a_kind)
        {
            static std::array<ImGui::ImTextureID, kCount> cache{};
            static std::array<bool, kCount>               tried{};
            const auto                                    i = static_cast<std::size_t>(a_kind);
            if (!tried[i]) {
                tried[i] = true;
                const auto path = std::format(R"(Data\Textures\DetailedMiniMap\icons\{}.dds)", kStyles[i].file);
                cache[i] = SKSEMenuFramework::LoadTexture(path);
                if (!cache[i]) {
                    logger::warn("icons: '{}' did not load, a drawn badge instead", path);
                }
            }
            return cache[i];
        }

        void Sword(Canvas& a_c, float a_x, float a_y, float a_s, Canvas::Color a_col)
        {
            const auto p = [&](float u, float v) { return Canvas::V2{ a_x + u * a_s, a_y + v * a_s }; };
            a_c.Line(p(-0.08f, 0.08f), p(0.22f, -0.22f), a_col, std::max(a_s * 0.09f, 1.5f));
            a_c.Triangle(p(0.18f, -0.27f), p(0.28f, -0.28f), p(0.27f, -0.18f), a_col);
            a_c.Line(p(-0.21f, -0.02f), p(0.02f, 0.21f), a_col, std::max(a_s * 0.08f, 1.2f));
            a_c.Line(p(-0.08f, 0.08f), p(-0.20f, 0.20f), a_col, std::max(a_s * 0.07f, 1.2f));
        }

        // no texture: a drawn badge, the glyph from the Menu Framework's Font Awesome if it has it (and the canvas a font)
        void Fallback(Canvas& a_c, Kind a_kind, float a_x, float a_y, float a_size, int a_alpha)
        {
            const auto& st = kStyles[static_cast<std::size_t>(a_kind)];
            const auto  col = Rgb(st.r, st.g, st.b, a_alpha);
            const float r = a_size * 0.5f;
            a_c.Disc({ a_x, a_y }, r, Rgb(10, 12, 18, a_alpha * 215 / 255));
            a_c.Ring({ a_x, a_y }, r, col, std::max(a_size * 0.07f, 1.0f));
            if (!st.glyph) {
                Sword(a_c, a_x, a_y, a_size, col);
            } else if (!a_c.Glyph(st.glyph, { a_x, a_y }, a_size * 0.58f, col)) {
                a_c.Disc({ a_x, a_y }, r * 0.45f, col);
            }
        }
    }

    const char* Name(Kind a_kind)
    {
        return Lang::T(static_cast<Lang::S>(static_cast<int>(Lang::S::Enemies) + static_cast<int>(a_kind)));
    }

    const char* File(Kind a_kind)
    {
        return kStyles[static_cast<std::size_t>(a_kind)].file;
    }

    void Draw(Canvas& a_canvas, Kind a_kind, float a_x, float a_y, float a_size, float a_alpha, float a_shade)
    {
        const int alpha = static_cast<int>(255.0f * std::clamp(a_alpha, 0.0f, 1.0f));
        if (alpha <= 0) {
            return;
        }
        if (const auto tex = Texture(a_kind)) {
            // on whole pixels, an even size: the badge's ring comes out round, the glyph in its middle (between
            // pixels the small picture is resampled lopsided)
            const float size = std::max(std::round(a_size * 0.5f) * 2.0f, 4.0f);
            const float x0 = std::round(a_x - size * 0.5f), y0 = std::round(a_y - size * 0.5f);
            const int lit = static_cast<int>(255.0f * std::clamp(a_shade, 0.0f, 1.0f));
            a_canvas.Image(tex, { x0, y0 }, { x0 + size, y0 + size }, Rgb(lit, lit, lit, alpha));
            return;
        }
        Fallback(a_canvas, a_kind, a_x, a_y, a_size, alpha);
    }

    void DrawPlayer(Canvas& a_canvas, float a_x, float a_y, float a_size, float a_angle, float a_alpha)
    {
        const auto                A = [&](int a_a) { return static_cast<int>(static_cast<float>(a_a) * std::clamp(a_alpha, 0.0f, 1.0f)); };
        static ImGui::ImTextureID tex = nullptr;
        static bool               tried = false;
        if (!tried) {
            tried = true;
            tex = SKSEMenuFramework::LoadTexture(R"(Data\Textures\DetailedMiniMap\icons\player.dds)");
            if (!tex) {
                logger::warn("icons: 'player.dds' did not load, a drawn badge instead");
            }
        }
        const float r = a_size * 0.5f;
        // the pointer first, under the badge: a dark outline, then gold, its tip a bit past the rim
        const auto at = [&](float a_a, float a_d) { return Canvas::V2{ a_x + std::sin(a_angle + a_a) * a_d, a_y - std::cos(a_angle + a_a) * a_d }; };
        a_canvas.Triangle(at(0.0f, r * 1.62f), at(-0.62f, r * 0.80f), at(0.62f, r * 0.80f), Rgb(15, 12, 8, A(235)));
        a_canvas.Triangle(at(0.0f, r * 1.45f), at(-0.52f, r * 0.85f), at(0.52f, r * 0.85f), Rgb(255, 210, 90, A(255)));
        if (tex) {
            const float size = std::max(std::round(r) * 2.0f, 4.0f);
            const float x0 = std::round(a_x - size * 0.5f), y0 = std::round(a_y - size * 0.5f);
            a_canvas.Image(tex, { x0, y0 }, { x0 + size, y0 + size }, Rgb(255, 255, 255, A(255)));
            return;
        }
        a_canvas.Disc({ a_x, a_y }, r, Rgb(10, 12, 18, A(230)));
        a_canvas.Ring({ a_x, a_y }, r, Rgb(255, 210, 90, A(255)), std::max(a_size * 0.08f, 1.0f));
        a_canvas.Disc({ a_x, a_y }, r * 0.35f, Rgb(255, 210, 90, A(255)));
    }
}

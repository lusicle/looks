#include "ui/text.h"

#include <cmath>

namespace looks::ui {

Vec2 measure_text(const Font& font, std::string_view text, float size) {
    float advance_em = 0.0f;
    size_t cursor = 0;
    while (cursor < text.size()) {
        const uint32_t cp = utf8_decode(text.data(), text.size(), &cursor);
        if (const Glyph* g = font.find_glyph(cp)) advance_em += g->advance;
    }
    return {advance_em * size, font.line_height() * size};
}

float draw_text(Canvas2D& canvas, const Font& font, std::string_view text,
                Vec2 top_left, float size, Color color) {
    const UiTexture* atlas = font.texture();
    if (!atlas) return 0.0f;

    // Snap the baseline to the physical pixel grid to avoid shimmer; with
    // the 8x8 debug font at integer scales this keeps glyphs pixel-exact.
    const float scale = canvas.scale();
    auto snap = [&](float v) {
        return std::floor(v * scale + 0.5f) / scale;
    };

    const float baseline = snap(top_left.y + font.ascender() * size);
    float cursor_x = snap(top_left.x);

    size_t cursor = 0;
    while (cursor < text.size()) {
        const uint32_t cp = utf8_decode(text.data(), text.size(), &cursor);
        const Glyph* g = font.find_glyph(cp);
        if (!g) continue;
        if (g->has_geometry) {
            const float x0 = cursor_x + g->plane_l * size;
            const float x1 = cursor_x + g->plane_r * size;
            const float y0 = baseline - g->plane_t * size;
            const float y1 = baseline - g->plane_b * size;
            canvas.draw_glyph_quad({x0, y0, x1 - x0, y1 - y0},
                                   g->uv_l, g->uv_t, g->uv_r, g->uv_b,
                                   color, atlas);
        }
        cursor_x += g->advance * size;
    }
    return cursor_x - top_left.x;
}

}  // namespace looks::ui

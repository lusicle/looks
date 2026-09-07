#include "ui/text.h"

#include <cmath>
#include <algorithm>

namespace looks::ui {

template <class Line>
Vec2 text_lines(const Font& font, std::string_view text, float size,
                 float width, Line line) {
    size_t start = 0;
    float y = 0, widest = 0;
    const float line_h = font.line_height() * size;
    while (start < text.size()) {
        size_t cursor = start, end = start, word = start;
        float advance = 0, word_advance = 0;
        while (cursor < text.size()) {
            const size_t previous = cursor;
            const uint32_t cp = utf8_decode(text.data(), text.size(), &cursor);
            if (cp == '\n') { end = previous; break; }
            const Glyph* glyph = font.find_glyph(cp);
            const float next = advance + (glyph ? glyph->advance * size : 0);
            if (next > width && previous > start) {
                cursor = previous;
                if (word > start) { end = word; advance = word_advance; cursor = word + 1; }
                break;
            }
            if (cp == ' ') { word = previous; word_advance = advance; }
            advance = next;
            end = cursor;
        }
        line(text.substr(start, end - start), y);
        widest = std::max(widest, advance);
        y += line_h;
        start = cursor;
        while (start < text.size() && text[start] == ' ') ++start;
    }
    return {widest, std::max(line_h, y)};
}

Vec2 measure_text_wrapped(const Font& font, std::string_view text,
                           float size, float width) {
    return text_lines(font, text, size, width, [](std::string_view, float) {});
}

void draw_text_wrapped(Canvas2D& canvas, const Font& font, std::string_view text,
                       Vec2 top_left, float size, float width, Color color) {
    text_lines(font, text, size, width, [&](std::string_view line, float y) {
        draw_text(canvas, font, line, {top_left.x, top_left.y + y}, size, color);
    });
}

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

    // Snap the baseline to the physical pixel grid to stop shimmer.
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

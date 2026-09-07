// size is the em height in logical px.

#pragma once

#include <string_view>

#include "ui/canvas2d.h"
#include "ui/font.h"

namespace looks::ui {

// Returns {advance width, line height} in logical px.
Vec2 measure_text(const Font& font, std::string_view text, float size);
Vec2 measure_text_wrapped(const Font& font, std::string_view text,
                           float size, float width);
void draw_text_wrapped(Canvas2D& canvas, const Font& font, std::string_view text,
                       Vec2 top_left, float size, float width, Color color);

// top_left is the glyph box top, not the baseline. Advance is in logical px.
float draw_text(Canvas2D& canvas, const Font& font, std::string_view text,
                Vec2 top_left, float size, Color color);

}  // namespace looks::ui

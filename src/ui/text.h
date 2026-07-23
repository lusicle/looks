// Text emission over Canvas2D. Owns no GPU state — glyph quads go through
// the same draw list as everything else (Text batches keyed on the font's
// atlas texture). `size` is the em height in logical px.

#pragma once

#include <string_view>

#include "ui/canvas2d.h"
#include "ui/font.h"

namespace looks::ui {

// Advance-width of a run (no kerning in the debug font; baked fonts may add
// it later). Returns {width, line_height} in logical px.
Vec2 measure_text(const Font& font, std::string_view text, float size);

// Draws a single line; `top_left` is the glyph box's top-left (baseline is
// derived from the font ascender). Returns the advance in logical px.
float draw_text(Canvas2D& canvas, const Font& font, std::string_view text,
                Vec2 top_left, float size, Color color);

}  // namespace looks::ui

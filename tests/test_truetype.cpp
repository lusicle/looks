// In-repo TrueType loader + string SDF rasterizer (docs/flow_canvas.md
// v5.5b): parse the shipped fonts, rasterize strings, verify the
// distance encoding and that output is a pure function of its inputs.

#include <filesystem>

#include "test_framework.h"
#include "ui/truetype.h"

using looks::ui::TtfFont;

namespace {

std::filesystem::path fonts_dir() {
    return std::filesystem::path(LOOKS_REPO_ROOT) / "assets" / "fonts";
}

}  // namespace

TEST(truetype_parses_shipped_fonts) {
    auto roboto = TtfFont::load(fonts_dir() / "Roboto.ttf");
    CHECK(roboto.has_value());
    auto cormorant = TtfFont::load(fonts_dir() / "Cormorant.ttf");
    CHECK(cormorant.has_value());
    // Non-font bytes must be rejected, not crash.
    auto not_font = TtfFont::load(std::filesystem::path(LOOKS_REPO_ROOT) /
                                  "assets" / "presets" / "minidv.json");
    CHECK(!not_font.has_value());
}

TEST(truetype_rasterizes_string_sdf) {
    auto font = TtfFont::load(fonts_dir() / "Roboto.ttf");
    CHECK(font.has_value());
    if (!font) return;

    // Spread 4 so a 48px glyph stroke's ~2px interior half-width reaches
    // deep into the encoding (0.5 + 0.5 * 2/4 = 0.75).
    const TtfFont::Sdf sdf = font->rasterize("AVij", 48.0f, 4.0f);
    CHECK(sdf.width > 20);
    CHECK(sdf.height > 20);
    CHECK_EQ(sdf.pixels.size(), size_t{sdf.width} * sdf.height);

    // The encoding must reach well inside (> edge 128) and fully
    // outside (near 0), and the bitmap corners sit outside.
    uint8_t lo = 255, hi = 0;
    for (uint8_t v : sdf.pixels) {
        lo = v < lo ? v : lo;
        hi = v > hi ? v : hi;
    }
    CHECK(hi > 170);
    CHECK(lo < 40);
    CHECK(sdf.pixels[0] < 128);   // corner: outside (< edge value)

    // Deterministic: same inputs, identical bytes.
    const TtfFont::Sdf again = font->rasterize("AVij", 48.0f, 4.0f);
    CHECK(again.pixels == sdf.pixels);

    // Size scales the bitmap.
    const TtfFont::Sdf big = font->rasterize("AVij", 96.0f, 8.0f);
    CHECK(big.width > sdf.width);
    CHECK(big.height > sdf.height);

    // Whitespace-only strings draw nothing.
    const TtfFont::Sdf blank = font->rasterize("   ", 48.0f, 8.0f);
    CHECK_EQ(blank.width, uint32_t{0});
}

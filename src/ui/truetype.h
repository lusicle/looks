// Hand-rolled TrueType loader + string SDF rasterizer: parses
// glyf-flavored TTFs (cmap formats 4/12,
// simple + composite glyphs, hmtx advances, kern format 0) and renders
// whole strings into single-channel signed-distance bitmaps — the Text
// effect's runtime font path, no bake step, any dropped .ttf. Output is
// a pure function of (font bytes, text, size, spread), so preview and
// export stay bit-identical. CFF/PostScript outlines (.otf) are out of
// scope — load() rejects fonts without a glyf table.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace looks::ui {

class TtfFont {
public:
    // Whole-file parse; empty on malformed/unsupported fonts. Every
    // table read is bounds-checked — user-dropped files are untrusted.
    static std::optional<TtfFont> load(const std::filesystem::path& path);

    // The string rendered at `size_px` as a signed-distance bitmap:
    // 0.5 = the outline edge, encoded across ±spread_px (inside > 0.5).
    // y-down, string box centered (pad = spread on every side); width 0
    // when nothing is drawable. Overlong strings are truncated at the
    // width cap.
    struct Sdf {
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> pixels;   // R8, y-down
        float spread_px = 0.0f;
    };
    Sdf rasterize(std::string_view utf8, float size_px,
                  float spread_px) const;

private:
    TtfFont() = default;

    uint32_t glyph_index(uint32_t codepoint) const;
    float advance_units(uint32_t glyph) const;
    float kern_units(uint32_t left, uint32_t right) const;
    // Appends the glyph's flattened contours in px (y-up), transformed
    // by the font-unit affine xf {a, b, c, d, e, f} then scaled.
    // Composite glyphs recurse (depth-capped).
    void append_outline(uint32_t glyph, const float xf[6], float scale_px,
                        std::vector<std::vector<float>>* contours,
                        int depth) const;

    std::vector<uint8_t> bytes_;
    // Table offsets/lengths into bytes_ (0 = absent).
    uint32_t glyf_off_ = 0, glyf_len_ = 0;
    uint32_t loca_off_ = 0, loca_len_ = 0;
    uint32_t hmtx_off_ = 0, hmtx_len_ = 0;
    uint32_t cmap_sub_off_ = 0;   // the chosen cmap subtable
    uint32_t kern_pairs_off_ = 0; // format-0 pair array
    uint32_t kern_pair_count_ = 0;
    uint16_t cmap_format_ = 0;
    uint16_t units_per_em_ = 1000;
    int16_t loca_long_ = 0;
    uint16_t num_glyphs_ = 0;
    uint16_t num_hmetrics_ = 0;
    float ascent_units_ = 800.0f;
    float descent_units_ = -200.0f;   // negative below baseline
};

}  // namespace looks::ui

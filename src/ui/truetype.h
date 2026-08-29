// The SDF output is a pure function of (font bytes, text, size, spread).
// load() rejects fonts without a glyf table; CFF outlines are unsupported.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace looks::ui {

class TtfFont {
public:
    // Font bytes are untrusted; bounds-check every table read.
    static std::optional<TtfFont> load(const std::filesystem::path& path);

    // SDF encode: 0.5 is the outline edge, inside > 0.5, span is +-spread_px.
    // Bitmap is y-down, padded spread_px each side; overlong text truncates.
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
    // Emits contours in px, y-up; xf is the font-unit affine {a,b,c,d,e,f}.
    void append_outline(uint32_t glyph, const float xf[6], float scale_px,
                        std::vector<std::vector<float>>* contours,
                        int depth) const;

    std::vector<uint8_t> bytes_;
    // Table offsets/lengths into bytes_; 0 = absent.
    uint32_t glyf_off_ = 0, glyf_len_ = 0;
    uint32_t loca_off_ = 0, loca_len_ = 0;
    uint32_t hmtx_off_ = 0, hmtx_len_ = 0;
    uint32_t cmap_sub_off_ = 0;
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

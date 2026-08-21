// Bitmap font (baked bitmap atlas; the offline fontbake tool
// will emit atlas + metrics in exactly this model). Until fontbake lands,
// Font::create_debug() provides a compiled-in 8x8 monospace pixel font so
// the toolkit has text from day one.
//
// Metrics are in em units, baseline-relative, y-up (matching the reference
// toolkit's glyph model so a future variable-width baked font drops in
// without touching the text renderer). UVs are normalized, v-down.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace looks::ui {

struct UiTexture;

struct Glyph {
    uint32_t codepoint = 0;
    float advance = 0.0f;                       // em units
    float plane_l = 0.0f, plane_b = 0.0f;       // em units vs. baseline, y-up
    float plane_r = 0.0f, plane_t = 0.0f;
    float uv_l = 0.0f, uv_t = 0.0f;             // normalized atlas coords
    float uv_r = 0.0f, uv_b = 0.0f;
    bool has_geometry = false;
};

class Font {
public:
    // Compiled-in 8x8 ASCII debug font.
    static Font create_debug();

    // Baked MSDF font (fontbake): atlas PNG + metrics JSON as
    // emitted by msdf-atlas-gen (-yorigin top). Empty on missing files or
    // parse failure — callers fall back to create_debug().
    static std::optional<Font> load_msdf(
        const std::filesystem::path& atlas_png,
        const std::filesystem::path& metrics_json);

    // True for MSDF fonts: RGBA atlas, median-of-RGB decode in the text
    // shader, linear sampling.
    bool msdf() const { return msdf_; }
    const std::vector<uint8_t>& atlas_rgba() const { return atlas_rgba_; }
    // The bake's -pxrange: distance-field span in atlas pixels. The text
    // shader derives its screen-space AA band from this analytically.
    float px_range() const { return px_range_; }

    // Binary search over the sorted glyph table; missing codepoints fall
    // back to '?' (then to any glyph). Never returns null on a valid font.
    const Glyph* find_glyph(uint32_t codepoint) const;

    float ascender() const { return ascender_; }        // em units
    float line_height() const { return line_height_; }  // em units

    uint32_t atlas_width() const { return atlas_width_; }
    uint32_t atlas_height() const { return atlas_height_; }
    const std::vector<uint8_t>& atlas_pixels() const { return atlas_; }  // A8

    // Set by UiRenderer::register_font after the GPU upload.
    const UiTexture* texture() const { return texture_; }
    void set_texture(const UiTexture* t) { texture_ = t; }

private:
    std::vector<Glyph> glyphs_;   // sorted by codepoint
    std::vector<uint8_t> atlas_;
    std::vector<uint8_t> atlas_rgba_;   // MSDF fonts only
    bool msdf_ = false;
    float px_range_ = 4.0f;
    uint32_t atlas_width_ = 0;
    uint32_t atlas_height_ = 0;
    float ascender_ = 0.875f;
    float line_height_ = 1.25f;
    const UiTexture* texture_ = nullptr;
};

// Incremental UTF-8 decoder: reads one codepoint at *cursor, advances it.
// Malformed input yields U+FFFD and advances one byte.
uint32_t utf8_decode(const char* text, size_t length, size_t* cursor);

}  // namespace looks::ui

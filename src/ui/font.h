// Glyph metrics are em units, baseline-relative, y-up.
// UVs are normalized atlas coords, v-down.

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
    float plane_l = 0.0f, plane_b = 0.0f;       // em units from baseline, y-up
    float plane_r = 0.0f, plane_t = 0.0f;
    float uv_l = 0.0f, uv_t = 0.0f;             // normalized atlas coords
    float uv_r = 0.0f, uv_b = 0.0f;
    bool has_geometry = false;
};

class Font {
public:
    static Font create_debug();

    // The metrics JSON must come from msdf-atlas-gen with -yorigin top.
    static std::optional<Font> load_msdf(
        const std::filesystem::path& atlas_png,
        const std::filesystem::path& metrics_json);

    // MSDF: RGBA atlas, median-of-RGB decode, linear sampling.
    bool msdf() const { return msdf_; }
    const std::vector<uint8_t>& atlas_rgba() const { return atlas_rgba_; }
    // Distance-field span in atlas px; the shader derives the AA band from it.
    float px_range() const { return px_range_; }

    // Missing codepoints fall back to '?'; never null on a valid font.
    const Glyph* find_glyph(uint32_t codepoint) const;

    float ascender() const { return ascender_; }        // em units
    float line_height() const { return line_height_; }  // em units

    uint32_t atlas_width() const { return atlas_width_; }
    uint32_t atlas_height() const { return atlas_height_; }
    const std::vector<uint8_t>& atlas_pixels() const { return atlas_; }  // A8

    // Null until UiRenderer::register_font uploads the atlas.
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

// Decodes one codepoint at *cursor and advances it; bad input gives U+FFFD.
uint32_t utf8_decode(const char* text, size_t length, size_t* cursor);

}  // namespace looks::ui

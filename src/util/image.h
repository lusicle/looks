// PNG decoder + TGA loader — glyph atlases, LUT strips,
// dust/leak/screen textures. PNG scope: 8-bit gray / RGB / RGBA, critical
// chunks, CRC-verified, no interlace, own inflate. TGA: uncompressed + RLE
// truecolor 24/32-bit. Both decode to tightly-packed RGBA8.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace looks {

struct ImageRgba {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;   // RGBA8, stride = width * 4
};

bool decode_png(const uint8_t* data, size_t size, ImageRgba* out,
                std::string* error = nullptr);
bool decode_tga(const uint8_t* data, size_t size, ImageRgba* out,
                std::string* error = nullptr);

// Loads by extension (.png / .tga).
bool load_image(const std::filesystem::path& path, ImageRgba* out,
                std::string* error = nullptr);

// Minimal RGBA8 PNG writer (stored deflate blocks - uncompressed, valid
// everywhere). The script screenshot op and dump tooling write through
// this; scope is single still frames.
std::vector<uint8_t> encode_png_rgba(const uint8_t* rgba, uint32_t width,
                                     uint32_t height);
bool write_png(const std::filesystem::path& path, const uint8_t* rgba,
               uint32_t width, uint32_t height);

}  // namespace looks

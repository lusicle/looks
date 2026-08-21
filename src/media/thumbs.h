// The .thumbs sidecar: 'THM1' u16 w, u16 h, u16 count (little-endian)
// then count packed RGB thumbs of w*h*3 bytes. Writer and reader live
// together so the header is spelled exactly once.
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace looks::media {

struct ThumbStripData {
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t count = 0;
    std::vector<uint8_t> rgb;   // count thumbs, each w * h * 3
};

// Refuses an empty strip and anything the u16 header fields cannot
// carry - a silent 16-bit wrap writes a strip the reader mis-slices.
bool write_thumbs(const std::filesystem::path& path,
                  const ThumbStripData& strip);

// Validates dimensions, the payload size, and the one-texture-row cap
// (w * count <= 16384) the UI atlas requires.
bool read_thumbs(const std::filesystem::path& path, ThumbStripData* out);

}  // namespace looks::media

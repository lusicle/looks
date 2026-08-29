// .thumbs layout, little-endian: 'THM1' u16 w, u16 h, u16 count,
// then count packed RGB cells of w*h*3 bytes.
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace looks::media {

// Sidecars with a shorter cell height count as stale and regenerate.
constexpr uint32_t kThumbStripH = 90;

struct ThumbStripData {
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t count = 0;
    std::vector<uint8_t> rgb;   // count thumbs, each w * h * 3
};

bool write_thumbs(const std::filesystem::path& path,
                  const ThumbStripData& strip);

bool read_thumbs(const std::filesystem::path& path, ThumbStripData* out);

// Reads the header only; out->rgb stays empty.
bool read_thumbs_header(const std::filesystem::path& path,
                        ThumbStripData* out);

}  // namespace looks::media

// The .thumbs sidecar: 'THM1' u16 w, u16 h, u16 count (little-endian)
// then count packed RGB thumbs of w*h*3 bytes. Writer and reader live
// together so the header is spelled exactly once.
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace looks::media {

// Strip cell height ingest writes (width follows the source aspect):
// sized so a gallery card reads one cell crisply at the entity atlas
// bar (160x90 for 16:9). Consumers derive their own sizes from the
// cells (the timeline lane box-filters down); the app treats shorter
// sidecars as stale and regenerates them.
constexpr uint32_t kThumbStripH = 90;

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

// Validates dimensions and the payload size.
bool read_thumbs(const std::filesystem::path& path, ThumbStripData* out);

// Header only (dimensions and count, `rgb` left empty): the staleness
// probe, so a rescan never pulls whole strips off disk.
bool read_thumbs_header(const std::filesystem::path& path,
                        ThumbStripData* out);

}  // namespace looks::media

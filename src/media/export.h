// Export: rendered frames -> H.264 + AAC encoder MFTs ->
// hand-rolled BMFF mux -> MP4. Frame pixels arrive through a producer
// callback (the app supplies mezzanine decode + engine render + readback),
// keeping this module free of any GPU dependency; audio comes from the PCM
// sidecar. Offline and correctness-first — software MFT fallback is fine.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace looks::media {

struct ExportOptions {
    uint32_t video_bitrate_bps = 8'000'000;
    uint32_t audio_bitrate_bps = 128'000;
    // Source-audio offset: seconds of PCM skipped before the
    // first exported video frame — clip trim plus the user nudge. Negative
    // delays the audio with leading silence. Out-of-range reads are
    // silence, so any offset is safe.
    double audio_offset_seconds = 0.0;
};

struct ExportProgress {
    std::atomic<uint32_t> frames_done{0};
    std::atomic<uint32_t> frames_total{0};
    std::atomic<bool> cancel{false};
};

struct ExportResult {
    bool ok = false;
    std::string error;
};

// Fills `nv12` with a packed frame (stride == width, Y then interleaved
// UV) for frame `index`. Returning false aborts the export.
using FrameProducer =
    std::function<bool(uint32_t index, std::vector<uint8_t>& nv12)>;

ExportResult export_movie(uint32_t width, uint32_t height, uint32_t fps_num,
                          uint32_t fps_den, uint32_t frame_count,
                          const FrameProducer& producer,
                          const std::filesystem::path& pcm_path,  // "" = silent
                          const std::filesystem::path& out_mp4,
                          const ExportOptions& options = {},
                          ExportProgress* progress = nullptr);

}  // namespace looks::media

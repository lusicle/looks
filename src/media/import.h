// Import transcode: MP4/MOV -> per-asset bundle on disk:
// <stem>.mez (intra-only mezzanine) + <stem>.pcm (s16 sidecar). The
// .analysis pass and thumbnail strip join in the modulation milestone.
//
// Video decode is serial (the MFT reorders internally); mezzanine encoding
// runs on a worker batch pool — frames are independent, so batches of N
// encode in parallel and commit in order, keeping the file deterministic.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace looks::media {

struct ImportOptions {
    int quality = 90;          // 0 = lossless (optional mode)
    int encode_threads = 0;    // 0 = hardware_concurrency
    bool proxy = true;         // also write <stem>.proxy.mez at half res
    int thumb_count = 120;     // thumbnail strip entries (0 = none)
};

struct ImportResult {
    bool ok = false;
    std::string error;
    std::filesystem::path mez_path;
    std::filesystem::path pcm_path;       // empty if the source has no audio
    std::filesystem::path analysis_path;  // audio/video mod-source curves
    std::filesystem::path proxy_path;     // half-res mezzanine
    std::filesystem::path thumbs_path;    // thumbnail strip
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t frame_count = 0;
    double fps = 0.0;
    uint32_t audio_channels = 0;
    uint32_t audio_sample_rate = 0;
    uint64_t audio_frames = 0;
};

// Progress: frames_done climbs to frames_total; poll from the UI thread.
struct ImportProgress {
    std::atomic<uint32_t> frames_done{0};
    std::atomic<uint32_t> frames_total{0};
    std::atomic<bool> cancel{false};
};

ImportResult import_media(const std::filesystem::path& source,
                          const std::filesystem::path& dest_dir,
                          const ImportOptions& options = {},
                          ImportProgress* progress = nullptr);

// Sidechain audio: pull just the audio out of a WAV or MP4/MOV
// into a .pcm sidecar at dest_pcm. Fails with `error` set when the source
// has no usable audio track.
bool extract_audio_pcm(const std::filesystem::path& source,
                       const std::filesystem::path& dest_pcm,
                       std::string* error);

}  // namespace looks::media

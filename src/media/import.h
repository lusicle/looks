#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace looks::media {

struct PcmBuffer;

struct ImportOptions {
    int quality = 90;          // still/cover-art mezzanine (0 = lossless)
    bool proxy = true;         // still bundles also write a half-res proxy
    int thumb_count = 120;     // thumbnail strip entries (0 = none)
};

struct ImportResult {
    bool ok = false;
    std::string error;
    std::filesystem::path mez_path;       // stills/cover art only
    std::filesystem::path pcm_path;       // empty if the source has no audio
    std::filesystem::path analysis_path;  // audio/video mod-source curves
    std::filesystem::path proxy_path;     // still bundles only
    std::filesystem::path thumbs_path;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t frame_count = 0;
    double fps = 0.0;
    uint32_t audio_channels = 0;
    uint32_t audio_sample_rate = 0;
    uint64_t audio_frames = 0;
};

// ready flips when the asset is usable, while the job keeps running.
// Poll these fields from the UI thread.
struct ImportProgress {
    std::atomic<uint32_t> frames_done{0};
    std::atomic<uint32_t> frames_total{0};
    std::atomic<bool> ready{false};
    std::atomic<bool> cancel{false};
    // Which call the video pass is inside: 1 read, 2 feed, 3 pump, 0 between.
    std::atomic<int> stage{0};
    // The job thread sets this before it flips ready; that store orders
    // the read.
    std::shared_ptr<const PcmBuffer> pcm;
};

ImportResult import_media(const std::filesystem::path& source,
                          const std::filesystem::path& dest_dir,
                          const ImportOptions& options = {},
                          ImportProgress* progress = nullptr);

// Rewrites .analysis and .thumbs; it sets ready immediately.
ImportResult resume_video_pass(const std::filesystem::path& source,
                               const std::filesystem::path& dest_dir,
                               const ImportOptions& options = {},
                               ImportProgress* progress = nullptr);

bool rebuild_still_thumbs(const std::filesystem::path& mez_path,
                          const std::filesystem::path& thumbs_path);

bool is_still_image(const std::filesystem::path& source);

// Writes an all-intra H.264 at <stem>.intra.mp4 beside the sidecars.
// Preview decode prefers it; export keeps the original. Video only.
ImportResult consolidate_video(const std::filesystem::path& source,
                               const std::filesystem::path& dest_dir,
                               ImportProgress* progress = nullptr);

// Fails with error set when the source has no usable audio track.
bool extract_audio_pcm(const std::filesystem::path& source,
                       const std::filesystem::path& dest_pcm,
                       std::string* error);

}  // namespace looks::media

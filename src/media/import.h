// Ingest: MP4/MOV video plays NATIVELY (the pool decodes the source in
// place), so video import writes no transcode - only the sidecars the
// source cannot provide: <stem>.pcm (s16 audio), <stem>.analysis
// (modulation curves), <stem>.thumbs (timeline strip). The fast stage
// (facts + PCM + audio curves) flips `ready` in seconds and the asset is
// usable; the video pass (one full decode feeding the video curves and
// thumbs) finishes in the background of the same job.
//
// Stills, and audio files with cover art, still transcode to a one-frame
// mezzanine - hold-frame entries make a still a stream downstream.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace looks::media {

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
    std::filesystem::path thumbs_path;    // thumbnail strip
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t frame_count = 0;
    double fps = 0.0;
    uint32_t audio_channels = 0;
    uint32_t audio_sample_rate = 0;
    uint64_t audio_frames = 0;
};

// Progress: frames_done climbs to frames_total across the video pass;
// `ready` flips once the asset is usable (facts + PCM + audio curves on
// disk) while the job keeps running. Poll from the UI thread.
struct ImportProgress {
    std::atomic<uint32_t> frames_done{0};
    std::atomic<uint32_t> frames_total{0};
    std::atomic<bool> ready{false};
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

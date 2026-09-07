#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include "util/json.h"

namespace looks::media {

enum class RenderFormat : uint8_t { Mp4, Gif, PngFrame, PngSequence };

struct RenderSettings {
    RenderFormat format = RenderFormat::Mp4;
    uint32_t width = 0, height = 0;
    double fps = 0;
    float bitrate_mbps = 8;
    bool audio = true;
    bool alpha = true;
    uint32_t gif_colors = 256;
    bool gif_dither = true;
    uint32_t gif_loops = 1;
    float gif_alpha_threshold = 50;
};

struct ExportRequest {
    RenderSettings render;
    uint64_t source = 0;
    uint32_t first = 0, last = 0;
    bool range_override = false;
    bool overwrite = false;
};

const char* render_format_name(RenderFormat format);
json::Value render_settings_json(const RenderSettings& settings);
bool read_render_settings(const json::Value& value, RenderSettings& settings, std::string& error);

struct ExportOptions {
    uint32_t video_bitrate_bps = 8'000'000;
    uint32_t audio_bitrate_bps = 128'000;
    // 0 = encoder default cadence; > 0 pins the keyframe interval.
    uint32_t gop_frames = 0;
    // Count of mix samples to skip before the first video frame.
    // A negative value delays the audio with leading silence.
    int64_t audio_skip_samples = 0;
};

struct ExportProgress {
    std::atomic<uint32_t> frames_done{0};
    std::atomic<uint32_t> frames_total{0};
    std::atomic<uint32_t> history_done{0};
    std::atomic<uint32_t> history_total{0};
    std::atomic<bool> cancel{false};
};

struct ExportResult {
    bool ok = false;
    std::string error;
};

// nv12 is packed: stride equals width, Y plane then interleaved UV.
// A false return aborts the export.
using FrameProducer =
    std::function<bool(uint32_t index, std::vector<uint8_t>& nv12)>;

// channels == 0 exports a silent movie.
struct ExportAudio {
    uint32_t channels = 0;
    uint32_t rate = 0;
    // first can be negative; positions outside all sources give silence.
    std::function<void(int64_t first, int16_t* out, uint32_t frames)> fill;
};

ExportResult export_movie(uint32_t width, uint32_t height, uint32_t fps_num,
                          uint32_t fps_den, uint32_t frame_count,
                          const FrameProducer& producer,
                          const ExportAudio& audio,
                          const std::filesystem::path& out_mp4,
                          const ExportOptions& options = {},
                          ExportProgress* progress = nullptr);

std::filesystem::path image_output_path(const std::filesystem::path& path,
                                        RenderFormat format, uint32_t index);
ExportResult export_images(uint32_t width, uint32_t height, uint32_t fps_num,
                           uint32_t fps_den, uint32_t frame_count,
                           const FrameProducer& producer,
                           const std::filesystem::path& path,
                           const RenderSettings& settings, bool overwrite,
                           ExportProgress* progress = nullptr);

}  // namespace looks::media

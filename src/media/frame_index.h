// Frame i is the i-th video sample in presentation order on a CFR grid.
// VFR sources resample to that grid.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "media/bmff.h"

namespace looks::media {

struct FrameIndex {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t timescale = 0;
    uint32_t frame_duration = 0;   // MEDIAN sample duration: the CFR grid
    std::vector<uint8_t> avcc;     // AVCDecoderConfigurationRecord

    // Samples are in decode (stbl) order.
    struct Sample {
        uint64_t offset = 0;
        uint32_t size = 0;
        int64_t pts_100ns = 0;        // composition time, stamped on feed
        int64_t duration_100ns = 0;
        bool keyframe = false;
    };
    std::vector<Sample> samples;

    // Presentation order sorts by composition time; ties keep decode order.
    std::vector<uint32_t> present_to_decode;
    std::vector<uint32_t> decode_to_present;
    // Composition times in presentation order, ascending.
    std::vector<int64_t> present_pts;
    // Sync-sample indices in decode order, ascending.
    std::vector<uint32_t> keyframes;

    uint32_t frame_count() const {
        return static_cast<uint32_t>(samples.size());
    }
    double fps() const {
        return frame_duration
            ? static_cast<double>(timescale) / frame_duration : 0.0;
    }

    // Nearest match, so 100ns rounding can never misfile a frame.
    uint32_t present_of_pts(int64_t pts_100ns) const;
    // Returns the decode index of the last keyframe at or before it.
    uint32_t keyframe_before(uint32_t present) const;
};

bool build_frame_index(const TrackInfo& track, FrameIndex* out,
                       std::string* error);
bool load_frame_index(const std::filesystem::path& path, FrameIndex* out,
                      std::string* error);

struct VideoFacts {
    uint32_t frames = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t timescale = 0;
    uint32_t frame_duration = 0;
    double fps = 0.0;
};
bool probe_video_facts(const std::filesystem::path& path, VideoFacts* out,
                       std::string* error);

}  // namespace looks::media

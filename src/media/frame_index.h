// Native-playback frame index: the per-frame facts needed to decode an
// H.264 source in place - sample offsets and sizes from the container's
// tables, presentation order resolved from composition times, keyframes
// from the sync-sample table. A projection of what BmffFile::open already
// parses; rebuilt at stream-open time (milliseconds), never cached to
// disk, so it can neither go stale nor go partial.
//
// Frame identity: frame i is the i-th video sample in PRESENTATION order,
// on a CFR grid taken from the first sample's duration (VFR sources are
// implicitly resampled to that grid - the same definition the old import
// transcode used, so timelines built before the cutover keep their clocks).

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
    std::vector<uint8_t> avcc;     // decoder config for the MFT

    // Samples in DECODE (stbl) order - the order the decoder is fed.
    struct Sample {
        uint64_t offset = 0;
        uint32_t size = 0;
        int64_t pts_100ns = 0;        // composition time, stamped on feed
        int64_t duration_100ns = 0;
        bool keyframe = false;
    };
    std::vector<Sample> samples;

    // presentation index <-> decode index (identity when there are no
    // B-frames). Presentation order sorts by composition time, ties kept
    // in decode order.
    std::vector<uint32_t> present_to_decode;
    std::vector<uint32_t> decode_to_present;
    // Composition times in presentation order (ascending): maps a decoded
    // output's stamped pts back to its presentation index exactly, instead
    // of trusting decoder arrival order.
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

    // Presentation index of a decoded output by its stamped pts (nearest
    // match, so 100ns rounding can never misfile a frame).
    uint32_t present_of_pts(int64_t pts_100ns) const;
    // The sync sample a cold roll to `present` must start feeding from
    // (decode index of the last keyframe at or before its sample).
    uint32_t keyframe_before(uint32_t present) const;
};

bool build_frame_index(const TrackInfo& track, FrameIndex* out,
                       std::string* error);
// Opens the container, finds the H.264 video track, builds the index.
bool load_frame_index(const std::filesystem::path& path, FrameIndex* out,
                      std::string* error);

// Container facts for the bundle table - what the asset list needs before
// any decoder opens. Same track walk as load_frame_index without keeping
// the per-sample arrays.
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

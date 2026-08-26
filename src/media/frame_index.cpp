#include "media/frame_index.h"

#include <algorithm>
#include <numeric>

namespace looks::media {

uint32_t FrameIndex::present_of_pts(int64_t pts_100ns) const {
    if (present_pts.empty()) return 0;
    const auto it = std::lower_bound(present_pts.begin(), present_pts.end(),
                                     pts_100ns);
    if (it == present_pts.begin()) return 0;
    if (it == present_pts.end())
        return static_cast<uint32_t>(present_pts.size() - 1);
    const size_t hi = static_cast<size_t>(it - present_pts.begin());
    const size_t lo = hi - 1;
    return static_cast<uint32_t>(
        pts_100ns - present_pts[lo] <= present_pts[hi] - pts_100ns ? lo : hi);
}

uint32_t FrameIndex::keyframe_before(uint32_t present) const {
    if (keyframes.empty()) return 0;
    const uint32_t decode =
        present < present_to_decode.size() ? present_to_decode[present] : 0;
    const auto it =
        std::upper_bound(keyframes.begin(), keyframes.end(), decode);
    return it == keyframes.begin() ? keyframes.front() : *(it - 1);
}

namespace {

// The CFR grid duration: the MEDIAN sample duration. Captures are
// routinely VFR-flagged with an outlier FIRST frame (doubled while
// the encoder spins up), and taking samples[0] halved a 120fps
// probe - the clip then conformed as 60fps and played half speed.
// The median is the honest grid for near-CFR content and is immune
// to leading/trailing outliers and occasional dropped-frame doubles.
uint32_t grid_duration(const std::vector<SampleInfo>& samples) {
    std::vector<uint32_t> d;
    d.reserve(samples.size());
    for (const SampleInfo& s : samples) d.push_back(s.duration);
    const auto mid = d.begin() + static_cast<ptrdiff_t>(d.size() / 2);
    std::nth_element(d.begin(), mid, d.end());
    return std::max(1u, *mid);
}

}  // namespace

bool build_frame_index(const TrackInfo& track, FrameIndex* out,
                       std::string* error) {
    if (track.kind != TrackInfo::Kind::Video || track.samples.empty() ||
        track.avcc.empty() || track.timescale == 0) {
        if (error) *error = "no usable H.264 video track";
        return false;
    }
    FrameIndex idx;
    idx.width = track.width;
    idx.height = track.height;
    idx.timescale = track.timescale;
    idx.frame_duration = grid_duration(track.samples);
    idx.avcc = track.avcc;

    const double to_100ns = 1.0e7 / track.timescale;
    idx.samples.reserve(track.samples.size());
    for (const SampleInfo& s : track.samples) {
        FrameIndex::Sample e;
        e.offset = s.file_offset;
        e.size = s.size;
        e.pts_100ns = static_cast<int64_t>(
            static_cast<double>(static_cast<int64_t>(s.dts) + s.cts_offset) *
            to_100ns);
        e.duration_100ns = static_cast<int64_t>(s.duration * to_100ns);
        e.keyframe = s.keyframe;
        idx.samples.push_back(e);
    }

    const uint32_t n = idx.frame_count();
    idx.present_to_decode.resize(n);
    std::iota(idx.present_to_decode.begin(), idx.present_to_decode.end(), 0u);
    std::stable_sort(idx.present_to_decode.begin(),
                     idx.present_to_decode.end(),
                     [&](uint32_t a, uint32_t b) {
                         return idx.samples[a].pts_100ns <
                                idx.samples[b].pts_100ns;
                     });
    idx.decode_to_present.resize(n);
    idx.present_pts.resize(n);
    for (uint32_t p = 0; p < n; ++p) {
        idx.decode_to_present[idx.present_to_decode[p]] = p;
        idx.present_pts[p] = idx.samples[idx.present_to_decode[p]].pts_100ns;
    }
    for (uint32_t d = 0; d < n; ++d)
        if (idx.samples[d].keyframe) idx.keyframes.push_back(d);
    // A stream with no marked sync samples decodes from the top or not at
    // all; sample 0 is the only honest start.
    if (idx.keyframes.empty()) idx.keyframes.push_back(0);

    *out = std::move(idx);
    return true;
}

namespace {

const TrackInfo* find_avc_track(const MovieInfo& movie) {
    const TrackInfo* video = movie.first_video();
    if (!video || std::string(video->fourcc) != "avc1") return nullptr;
    return video;
}

}  // namespace

bool load_frame_index(const std::filesystem::path& path, FrameIndex* out,
                      std::string* error) {
    BmffFile file;
    if (!file.open(path, error)) return false;
    const TrackInfo* video = find_avc_track(file.movie());
    if (!video) {
        if (error) *error = "no H.264 video track";
        return false;
    }
    return build_frame_index(*video, out, error);
}

bool probe_video_facts(const std::filesystem::path& path, VideoFacts* out,
                       std::string* error) {
    BmffFile file;
    if (!file.open(path, error)) return false;
    const TrackInfo* video = find_avc_track(file.movie());
    if (!video || video->samples.empty() || video->timescale == 0) {
        if (error) *error = "no H.264 video track";
        return false;
    }
    VideoFacts f;
    f.frames = static_cast<uint32_t>(video->samples.size());
    f.width = video->width;
    f.height = video->height;
    f.timescale = video->timescale;
    f.frame_duration = grid_duration(video->samples);
    f.fps = static_cast<double>(f.timescale) / f.frame_duration;
    *out = f;
    return true;
}

}  // namespace looks::media

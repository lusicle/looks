#include <string>

#include "media/frame_index.h"
#include "test_framework.h"

using namespace looks::media;

namespace {

TrackInfo make_track(std::initializer_list<int64_t> cts_offsets,
                     std::initializer_list<int> keyframes,
                     uint32_t duration = 512) {
    TrackInfo t;
    t.kind = TrackInfo::Kind::Video;
    t.timescale = 12800;
    t.width = 640;
    t.height = 360;
    t.avcc = {1, 0x64, 0x00, 0x1F, 0xFF, 0xE1};
    uint64_t dts = 0;
    uint64_t offset = 1000;
    int i = 0;
    for (const int64_t cts : cts_offsets) {
        SampleInfo s;
        s.file_offset = offset;
        s.size = 100;
        s.dts = dts;
        s.cts_offset = cts;
        s.duration = duration;
        s.keyframe = false;
        for (const int k : keyframes)
            if (k == i) s.keyframe = true;
        t.samples.push_back(s);
        dts += duration;
        offset += s.size;
        ++i;
    }
    return t;
}

}  // namespace

TEST(frame_index_identity_without_reorder) {
    const TrackInfo t = make_track({0, 0, 0, 0, 0, 0}, {0, 3});
    FrameIndex idx;
    std::string error;
    CHECK(build_frame_index(t, &idx, &error));
    CHECK_EQ(idx.frame_count(), 6u);
    CHECK_EQ(idx.frame_duration, 512u);
    CHECK(idx.fps() == 25.0);
    for (uint32_t i = 0; i < 6; ++i) {
        CHECK_EQ(idx.present_to_decode[i], i);
        CHECK_EQ(idx.decode_to_present[i], i);
    }
    CHECK_EQ(idx.keyframes.size(), size_t{2});
    CHECK_EQ(idx.keyframe_before(0), 0u);
    CHECK_EQ(idx.keyframe_before(2), 0u);
    CHECK_EQ(idx.keyframe_before(3), 3u);
    CHECK_EQ(idx.keyframe_before(5), 3u);
}

TEST(frame_index_presentation_order_resolves_b_frames) {
    const TrackInfo t =
        make_track({512, 1536, 0, 0, 512, 512}, {0, 4});
    FrameIndex idx;
    std::string error;
    CHECK(build_frame_index(t, &idx, &error));

    CHECK_EQ(idx.present_to_decode[0], 0u);
    CHECK_EQ(idx.present_to_decode[1], 2u);
    CHECK_EQ(idx.present_to_decode[2], 3u);
    CHECK_EQ(idx.present_to_decode[3], 1u);
    CHECK_EQ(idx.present_to_decode[4], 4u);
    CHECK_EQ(idx.present_to_decode[5], 5u);
    CHECK_EQ(idx.decode_to_present[1], 3u);
    CHECK_EQ(idx.decode_to_present[2], 1u);

    for (size_t i = 1; i < idx.present_pts.size(); ++i)
        CHECK(idx.present_pts[i - 1] < idx.present_pts[i]);

    CHECK_EQ(idx.keyframe_before(3), 0u);
    CHECK_EQ(idx.keyframe_before(4), 4u);
    CHECK_EQ(idx.keyframe_before(5), 4u);

    // present_of_pts must absorb a +/-1 tick rounding wobble.
    for (uint32_t p = 0; p < idx.frame_count(); ++p) {
        CHECK_EQ(idx.present_of_pts(idx.present_pts[p]), p);
        CHECK_EQ(idx.present_of_pts(idx.present_pts[p] + 1), p);
        CHECK_EQ(idx.present_of_pts(idx.present_pts[p] - 1), p);
    }
}

TEST(frame_index_vfr_grid_takes_the_median_duration) {
    // The grid is the median duration; first frames are often outliers.
    TrackInfo t = make_track({0, 0, 0, 0}, {0}, 512);
    t.samples[2].duration = 1024;
    t.samples[3].duration = 256;
    FrameIndex idx;
    std::string error;
    CHECK(build_frame_index(t, &idx, &error));
    CHECK_EQ(idx.frame_duration, 512u);
    CHECK(idx.fps() == 25.0);
    CHECK_EQ(idx.frame_count(), 4u);

    TrackInfo hfr = make_track({0, 0, 0, 0, 0}, {0}, 128);
    hfr.samples[0].duration = 256;
    FrameIndex idx2;
    CHECK(build_frame_index(hfr, &idx2, &error));
    CHECK_EQ(idx2.frame_duration, 128u);
}

TEST(frame_index_refuses_non_video) {
    TrackInfo t = make_track({0, 0}, {0});
    t.avcc.clear();
    FrameIndex idx;
    std::string error;
    CHECK(!build_frame_index(t, &idx, &error));
    CHECK(!error.empty());
}

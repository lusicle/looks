// Frame-index math: the presentation/decode mapping, keyframe lookup and
// pts round-trip the native decode sessions steer by. Pure - synthetic
// TrackInfo in, no decoder, no files.

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
    // No composition offsets: presentation order IS decode order.
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
    // Decode order I P B B I P with composition offsets shaping the
    // presentation order I B B P I P - the classic reorder.
    //   decode:  s0 I (pts  512)   s1 P (pts 2048)  s2 B (pts 1024)
    //            s3 B (pts 1536)   s4 I (pts 2560)  s5 P (pts 3072)
    const TrackInfo t =
        make_track({512, 1536, 0, 0, 512, 512}, {0, 4});
    FrameIndex idx;
    std::string error;
    CHECK(build_frame_index(t, &idx, &error));

    // presentation -> decode: [s0, s2, s3, s1, s4, s5]
    CHECK_EQ(idx.present_to_decode[0], 0u);
    CHECK_EQ(idx.present_to_decode[1], 2u);
    CHECK_EQ(idx.present_to_decode[2], 3u);
    CHECK_EQ(idx.present_to_decode[3], 1u);
    CHECK_EQ(idx.present_to_decode[4], 4u);
    CHECK_EQ(idx.present_to_decode[5], 5u);
    CHECK_EQ(idx.decode_to_present[1], 3u);
    CHECK_EQ(idx.decode_to_present[2], 1u);

    // Composition times ascend in presentation order.
    for (size_t i = 1; i < idx.present_pts.size(); ++i)
        CHECK(idx.present_pts[i - 1] < idx.present_pts[i]);

    // The roll for presentation 3 (decode s1) starts at s0; the second
    // GOP's frames start at s4.
    CHECK_EQ(idx.keyframe_before(3), 0u);
    CHECK_EQ(idx.keyframe_before(4), 4u);
    CHECK_EQ(idx.keyframe_before(5), 4u);

    // Output pts maps back to its presentation slot exactly, and a 100ns
    // rounding wobble still lands on the nearest frame.
    for (uint32_t p = 0; p < idx.frame_count(); ++p) {
        CHECK_EQ(idx.present_of_pts(idx.present_pts[p]), p);
        CHECK_EQ(idx.present_of_pts(idx.present_pts[p] + 1), p);
        CHECK_EQ(idx.present_of_pts(idx.present_pts[p] - 1), p);
    }
}

TEST(frame_index_vfr_grid_takes_the_median_duration) {
    // The CFR grid is the MEDIAN sample duration: captures routinely
    // flag VFR with an outlier FIRST frame (doubled while the encoder
    // spins up), and a first-sample grid halved a 120fps probe - the
    // clip then conformed as 60fps and played half speed.
    TrackInfo t = make_track({0, 0, 0, 0}, {0}, 512);
    t.samples[2].duration = 1024;
    t.samples[3].duration = 256;
    FrameIndex idx;
    std::string error;
    CHECK(build_frame_index(t, &idx, &error));
    CHECK_EQ(idx.frame_duration, 512u);
    CHECK(idx.fps() == 25.0);
    CHECK_EQ(idx.frame_count(), 4u);

    // The outlier first frame no longer halves the grid.
    TrackInfo hfr = make_track({0, 0, 0, 0, 0}, {0}, 128);
    hfr.samples[0].duration = 256;   // doubled spin-up frame
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

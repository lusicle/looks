#include "media/track.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>

#include "test_framework.h"
#include "util/hash.h"
#include "util/numerics.h"

using looks::media::GrayFrame;
using looks::media::TrackData;

namespace {

constexpr uint32_t kW = 320, kH = 240;

// The synthetic scene has many corners for the tracker to find.
float scene(float x, float y) {
    const float a = std::sin(x * 0.11f + std::sin(y * 0.07f) * 2.0f);
    const float b = std::cos(y * 0.13f + std::sin(x * 0.05f) * 3.0f);
    const float c = std::sin((x + y) * 0.041f);
    return 128.0f + 55.0f * a * b + 30.0f * c;
}

struct Synth {
    std::vector<uint8_t> buf;
    float dx, dy;   // content shift per frame, px
    uint32_t cut_at = ~0u;   // frames >= this sample a distant patch

    Synth(float sx, float sy) : dx(sx), dy(sy) {
        buf.resize(static_cast<size_t>(kW) * kH);
    }
    bool frame(uint32_t f, GrayFrame* out) {
        const float shot = f >= cut_at ? 5000.0f : 0.0f;
        const float ox = static_cast<float>(f) * dx + shot;
        const float oy = static_cast<float>(f) * dy + shot * 0.7f;
        for (uint32_t y = 0; y < kH; ++y)
            for (uint32_t x = 0; x < kW; ++x) {
                const float v = scene(static_cast<float>(x) + ox,
                                      static_cast<float>(y) + oy);
                buf[static_cast<size_t>(y) * kW + x] =
                    static_cast<uint8_t>(
                        std::clamp(v, 0.0f, 255.0f));
            }
        out->data = buf.data();
        out->width = kW;
        out->height = kH;
        out->stride = kW;
        return true;
    }
};

}  // namespace

TEST(track_solve_recovers_translation_deterministically) {
    // A positive sample shift moves the content left, so tx goes negative.
    // tx and ty units are fractions of width and height.
    Synth s(1.6f, 0.9f);
    TrackData a, b;
    auto feed = [&](uint32_t f, GrayFrame* g) { return s.frame(f, g); };
    CHECK(looks::media::track_run(0, 12, feed, &a));
    CHECK(looks::media::track_run(0, 12, feed, &b));
    CHECK_EQ(a.solve.size(), size_t{12});

    // Byte determinism: identical input, identical solve.
    CHECK_EQ(a.tracks.size(), b.tracks.size());
    for (size_t i = 0; i < a.solve.size(); ++i) {
        CHECK_EQ(a.solve[i].tx, b.solve[i].tx);
        CHECK_EQ(a.solve[i].ty, b.solve[i].ty);
        CHECK_EQ(a.solve[i].rot, b.solve[i].rot);
        CHECK_EQ(a.solve[i].scale, b.solve[i].scale);
    }

    // Accuracy: 11 frames of motion accumulated.
    const float ex = -11.0f * 1.6f / static_cast<float>(kW);
    const float ey = -11.0f * 0.9f / static_cast<float>(kH);
    const float tol = 0.006f;
    CHECK(std::fabs(a.solve[11].tx - ex) < tol);
    CHECK(std::fabs(a.solve[11].ty - ey) < tol);
    CHECK(std::fabs(a.solve[11].rot) < 0.01f);
    CHECK(std::fabs(a.solve[11].scale - 1.0f) < 0.01f);
    CHECK(a.solve[11].inliers >= 20u);
    CHECK_EQ(a.solve[0].tx, 0.0f);
    CHECK_EQ(a.solve[0].scale, 1.0f);
    CHECK(a.tracks.size() >= 30u);
}

TEST(track_cache_roundtrip) {
    Synth s(1.0f, 0.0f);
    TrackData a;
    auto feed = [&](uint32_t f, GrayFrame* g) { return s.frame(f, g); };
    CHECK(looks::media::track_run(0, 6, feed, &a));
    a.settings_hash = 0xABCDEF123456ull;

    const std::filesystem::path p =
        std::filesystem::temp_directory_path() / "looks_track_test.track";
    CHECK(looks::media::track_save(p, a));
    TrackData b;
    CHECK(looks::media::track_load(p, &b));
    CHECK_EQ(b.settings_hash, a.settings_hash);
    CHECK_EQ(b.start, a.start);
    CHECK_EQ(b.end, a.end);
    CHECK_EQ(b.solve.size(), a.solve.size());
    for (size_t i = 0; i < a.solve.size(); ++i) {
        CHECK_EQ(b.solve[i].tx, a.solve[i].tx);
        CHECK_EQ(b.solve[i].inliers, a.solve[i].inliers);
    }
    CHECK_EQ(b.tracks.size(), a.tracks.size());
    for (size_t i = 0; i < a.tracks.size(); ++i) {
        CHECK_EQ(b.tracks[i].id, a.tracks[i].id);
        CHECK_EQ(b.tracks[i].points.size(), a.tracks[i].points.size());
    }
    std::filesystem::remove(p);
}

TEST(track_plane_solve_recovers_translation) {
    Synth s(1.6f, 0.9f);
    TrackData a;
    auto feed = [&](uint32_t f, GrayFrame* g) { return s.frame(f, g); };
    CHECK(looks::media::track_run(0, 12, feed, &a));
    const looks::media::PlaneSolve* p =
        looks::media::ensure_plane(&a, 0.5f, 0.5f, 0.6f, 0.6f);
    CHECK(p != nullptr);
    CHECK_EQ(p->h.size(), size_t{12 * 9});
    // Frame 0 is identity and the homography works in uv units.
    CHECK_EQ(p->h[0], 1.0f);
    CHECK_EQ(p->h[2], 0.0f);
    const float* h = p->h.data() + 11 * 9;
    const float ex = -11.0f * 1.6f / static_cast<float>(kW);
    const float ey = -11.0f * 0.9f / static_cast<float>(kH);
    CHECK(std::fabs(h[2] - ex) < 0.006f);
    CHECK(std::fabs(h[5] - ey) < 0.006f);
    CHECK(std::fabs(h[0] - 1.0f) < 0.03f);
    CHECK(std::fabs(h[4] - 1.0f) < 0.03f);
    CHECK(std::fabs(h[6]) < 0.05f);
    CHECK(std::fabs(h[7]) < 0.05f);
    // A repeat request reuses the cached plane.
    looks::media::ensure_plane(&a, 0.5f, 0.5f, 0.6f, 0.6f);
    CHECK_EQ(a.planes.size(), size_t{1});
    const std::filesystem::path pth =
        std::filesystem::temp_directory_path() / "looks_plane_test.track";
    CHECK(looks::media::track_save(pth, a));
    TrackData b;
    CHECK(looks::media::track_load(pth, &b));
    CHECK_EQ(b.planes.size(), size_t{1});
    CHECK_EQ(b.planes[0].h.size(), a.planes[0].h.size());
    CHECK_EQ(b.planes[0].h[11 * 9 + 2], a.planes[0].h[11 * 9 + 2]);
    std::filesystem::remove(pth);
}

TEST(track_cut_resets_chains) {
    // The two shots hold unrelated content, and the cut is at frame 12.
    Synth s(1.6f, 0.9f);
    s.cut_at = 12;
    TrackData a;
    auto feed = [&](uint32_t f, GrayFrame* g) { return s.frame(f, g); };
    CHECK(looks::media::track_run(0, 24, feed, &a, {12}));
    CHECK_EQ(a.cuts.size(), size_t{1});
    CHECK_EQ(a.cuts[0], 12u);
    CHECK_EQ(a.solve[12].tx, 0.0f);
    CHECK_EQ(a.solve[12].rot, 0.0f);
    CHECK_EQ(a.solve[12].scale, 1.0f);
    CHECK_EQ(a.solve[12].inliers, 0u);
    const float ex = -11.0f * 1.6f / static_cast<float>(kW);
    CHECK(std::fabs(a.solve[11].tx - ex) < 0.006f);
    for (const looks::media::FeatureTrack& t : a.tracks) {
        const bool spans = t.points.front().frame <= 11u &&
                           t.points.back().frame >= 12u;
        CHECK(!spans);
    }
    // The plane chain freezes at the cut and does not re-lock.
    const looks::media::PlaneSolve* p =
        looks::media::ensure_plane(&a, 0.5f, 0.5f, 0.6f, 0.6f);
    CHECK(p != nullptr);
    for (uint32_t k = 12; k < 24; ++k)
        for (int q = 0; q < 9; ++q)
            CHECK_EQ(p->h[k * 9 + q], p->h[11 * 9 + q]);
    // A flat pan has no parallax, so the solve reports it, not a fake.
    looks::media::sfm_solve(&a);
    CHECK_EQ(a.sfm.size(), size_t{2});
    CHECK_EQ(a.sfm[0].start, 0u);
    CHECK_EQ(a.sfm[0].end, 12u);
    CHECK_EQ(a.sfm[1].start, 12u);
    CHECK_EQ(a.sfm[1].end, 24u);
    CHECK_EQ(a.sfm[0].status, looks::media::kSfmLowParallax);
    CHECK_EQ(a.sfm[1].status, looks::media::kSfmLowParallax);
}

TEST(track_sfm_synthetic_orbit_recovers_poses) {
    // The solve is up to gauge, so every check below is gauge-invariant.
    using looks::hash_combine;
    using looks::media::FeatureTrack;
    using looks::util::m3_transpose;
    using looks::util::rodrigues;
    constexpr uint32_t kFrames = 40;
    constexpr double kFocalGt = 1.2;
    constexpr double kAspect = 4.0 / 3.0;
    double px[64], py[64], pz[64];
    for (int i = 0; i < 64; ++i) {
        const uint64_t h = hash_combine(7ull, static_cast<uint64_t>(i));
        px[i] = ((i % 8) - 3.5) * 0.4;
        py[i] = ((i / 8) - 3.5) * 0.3;
        pz[i] = 5.0 + static_cast<double>(h % 1000u) / 1000.0 * 3.0 - 1.5;
    }
    double R[kFrames][9], T[kFrames][3], C[kFrames][3];
    for (uint32_t k = 0; k < kFrames; ++k) {
        const double aa[3] = {0.0, 0.004 * k, 0.002 * k};
        rodrigues(aa, R[k]);
        C[k][0] = 0.03 * k;
        C[k][1] = 0.015 * k;
        C[k][2] = 0.0;
        for (int r = 0; r < 3; ++r)
            T[k][r] = -(R[k][r * 3] * C[k][0] + R[k][r * 3 + 1] * C[k][1] +
                        R[k][r * 3 + 2] * C[k][2]);
    }
    TrackData d;
    d.start = 0;
    d.end = kFrames;
    d.aspect = static_cast<float>(kAspect);
    d.solve.resize(kFrames);
    auto project = [&](int i, uint32_t k, float* u, float* v) -> bool {
        double c[3];
        for (int r = 0; r < 3; ++r)
            c[r] = R[k][r * 3] * px[i] + R[k][r * 3 + 1] * py[i] +
                   R[k][r * 3 + 2] * pz[i] + T[k][r];
        if (c[2] < 0.1) return false;
        const double mu = kFocalGt * c[0] / c[2];
        const double mv = kFocalGt * c[1] / c[2];
        const double uu = mu / kAspect + 0.5;
        const double vv = mv + 0.5;
        if (uu < 0.03 || uu > 0.97 || vv < 0.03 || vv > 0.97) return false;
        *u = static_cast<float>(uu);
        *v = static_cast<float>(vv);
        return true;
    };
    for (int i = 0; i < 64; ++i) {
        FeatureTrack t;
        t.id = static_cast<uint32_t>(i + 1);
        for (uint32_t k = 0; k < kFrames; ++k) {
            float u, v;
            if (!project(i, k, &u, &v)) {
                if (t.points.empty()) continue;
                break;   // tracks stay consecutive-frame runs
            }
            t.points.push_back({k, u, v});
        }
        if (t.points.size() >= 2) d.tracks.push_back(std::move(t));
    }
    CHECK(d.tracks.size() >= 50u);
    TrackData d2 = d;

    CHECK(looks::media::sfm_solve(&d));
    CHECK_EQ(d.sfm.size(), size_t{1});
    const looks::media::SfmSegment& seg = d.sfm[0];
    CHECK_EQ(seg.status, looks::media::kSfmSolved);
    CHECK_EQ(seg.cams.size(), static_cast<size_t>(kFrames));
    CHECK(seg.points.size() >= 30u);
    CHECK(seg.mean_reproj < 2.0e-3f);
    CHECK(std::fabs(seg.focal - kFocalGt) < 0.1);

    // Relative rotation between two frames is gauge-free.
    auto rel_rot_err = [&](uint32_t a, uint32_t b) {
        double ra[9], rb[9], rat[9], rel[9];
        rodrigues(seg.cams[a].aa, ra);
        rodrigues(seg.cams[b].aa, rb);
        m3_transpose(ra, rat);
        looks::util::m3_mul(rb, rat, rel);
        double ga[9], gb[9], gat[9], grel[9];
        std::memcpy(ga, R[a], sizeof(ga));
        std::memcpy(gb, R[b], sizeof(gb));
        m3_transpose(ga, gat);
        looks::util::m3_mul(gb, gat, grel);
        double diff[9], grelt[9];
        m3_transpose(grel, grelt);
        looks::util::m3_mul(rel, grelt, diff);
        const double tr = diff[0] + diff[4] + diff[8];
        return std::acos(std::min(1.0, std::max(-1.0, (tr - 1.0) * 0.5)));
    };
    CHECK(rel_rot_err(5, 20) < 0.01);
    CHECK(rel_rot_err(5, 35) < 0.01);

    // Angles and distance ratios survive any rigid gauge and scale.
    auto center = [&](uint32_t k, double out[3]) {
        double r9[9], rt[9];
        rodrigues(seg.cams[k].aa, r9);
        m3_transpose(r9, rt);
        looks::util::m3_mul_v(rt, seg.cams[k].t, out);
        for (int q = 0; q < 3; ++q) out[q] = -out[q];
    };
    double c5[3], c20[3], c35[3];
    center(5, c5);
    center(20, c20);
    center(35, c35);
    auto vdiff = [](const double a[3], const double b[3], double o[3]) {
        for (int q = 0; q < 3; ++q) o[q] = b[q] - a[q];
    };
    auto vlen = [](const double a[3]) {
        return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    };
    double e1[3], e2[3], g1[3], g2[3];
    vdiff(c5, c20, e1);
    vdiff(c5, c35, e2);
    vdiff(C[5], C[20], g1);
    vdiff(C[5], C[35], g2);
    const double cos_e =
        (e1[0] * e2[0] + e1[1] * e2[1] + e1[2] * e2[2]) /
        (vlen(e1) * vlen(e2));
    const double cos_g =
        (g1[0] * g2[0] + g1[1] * g2[1] + g1[2] * g2[2]) /
        (vlen(g1) * vlen(g2));
    CHECK(std::fabs(std::acos(std::min(1.0, std::max(-1.0, cos_e))) -
                    std::acos(std::min(1.0, std::max(-1.0, cos_g)))) <
          0.02);
    const double s1 = vlen(e1) / vlen(g1);
    const double s2 = vlen(e2) / vlen(g2);
    CHECK(std::fabs(s1 / s2 - 1.0) < 0.02);

    {
        const looks::media::SfmPoint& sp = seg.points.front();
        const FeatureTrack* trk = nullptr;
        for (const FeatureTrack& t : d.tracks)
            if (t.id == sp.track) trk = &t;
        CHECK(trk != nullptr);
        const uint32_t f = trk->points.front().frame +
                           static_cast<uint32_t>(trk->points.size()) / 2;
        const looks::media::TrackPoint& tp =
            trk->points[f - trk->points.front().frame];
        double r9[9];
        rodrigues(seg.cams[f].aa, r9);
        double c3[3];
        for (int r = 0; r < 3; ++r)
            c3[r] = r9[r * 3] * sp.x + r9[r * 3 + 1] * sp.y +
                    r9[r * 3 + 2] * sp.z + seg.cams[f].t[r];
        CHECK(c3[2] > 0.0);
        const double uu = (seg.focal * c3[0] / c3[2]) / kAspect + 0.5;
        const double vv = seg.focal * c3[1] / c3[2] + 0.5;
        CHECK(std::fabs(uu - tp.x) < 2.0e-3);
        CHECK(std::fabs(vv - tp.y) < 2.0e-3);
    }

    // Byte determinism: a second solve of the same tracks matches.
    CHECK(looks::media::sfm_solve(&d2));
    CHECK_EQ(d2.sfm.size(), d.sfm.size());
    CHECK_EQ(d2.sfm[0].focal, d.sfm[0].focal);
    CHECK_EQ(d2.sfm[0].cams.size(), d.sfm[0].cams.size());
    CHECK(std::memcmp(d2.sfm[0].cams.data(), d.sfm[0].cams.data(),
                      d.sfm[0].cams.size() *
                          sizeof(looks::media::SfmCamera)) == 0);
    CHECK_EQ(d2.sfm[0].points.size(), d.sfm[0].points.size());
    CHECK(std::memcmp(d2.sfm[0].points.data(), d.sfm[0].points.data(),
                      d.sfm[0].points.size() *
                          sizeof(looks::media::SfmPoint)) == 0);

    // The jittered grid is near-planar, so the homography is near identity.
    const looks::media::PlaneSolve* p =
        looks::media::ensure_plane(&d, 0.5f, 0.5f, 0.5f, 0.5f);
    if (p) {
        CHECK_EQ(p->h.size(), static_cast<size_t>(kFrames) * 9);
        CHECK(std::fabs(p->h[0] - 1.0f) < 1.0e-4f);
        CHECK(std::fabs(p->h[2]) < 1.0e-4f);
    }

    const std::filesystem::path pth =
        std::filesystem::temp_directory_path() / "looks_sfm_test.track";
    d.cuts = {7u};   // set a cut to exercise the field: this solve has none
    CHECK(looks::media::track_save(pth, d));
    TrackData b;
    CHECK(looks::media::track_load(pth, &b));
    CHECK_EQ(b.aspect, d.aspect);
    CHECK_EQ(b.cuts.size(), size_t{1});
    CHECK_EQ(b.cuts[0], 7u);
    CHECK_EQ(b.sfm.size(), d.sfm.size());
    CHECK_EQ(b.sfm[0].status, d.sfm[0].status);
    CHECK_EQ(b.sfm[0].focal, d.sfm[0].focal);
    CHECK_EQ(b.sfm[0].cams.size(), d.sfm[0].cams.size());
    CHECK_EQ(b.sfm[0].cams[20].t[0], d.sfm[0].cams[20].t[0]);
    CHECK_EQ(b.sfm[0].points.size(), d.sfm[0].points.size());
    CHECK_EQ(b.sfm[0].points[3].z, d.sfm[0].points[3].z);
    std::filesystem::remove(pth);
}

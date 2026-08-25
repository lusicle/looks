// Offline motion tracking: Harris features + pyramidal KLT over a
// frame range, a per-frame global similarity solve (the camera value
// node's stab channels), per-shot planar homography chains, and a full
// per-shot 3D structure-from-motion solve (poses + points + focal for
// the anchor channels and 3D plane fits). Everything is DETERMINISTIC
// - fixed feature budgets, fixed iteration counts, fixed orderings,
// seeded counter-hash RANSAC, no threading - so a re-solve
// byte-matches and preview equals export. Results cache in the asset's
// <stem>.track sidecar; the caller owns decode and feeds gray frames
// in order.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>

namespace looks::media {

struct TrackPoint {
    uint32_t frame = 0;
    float x = 0.0f, y = 0.0f;   // uv fractions of the source frame
};

struct FeatureTrack {
    uint32_t id = 0;
    std::vector<TrackPoint> points;   // consecutive frames
};

// Cumulative similarity vs the range start, uv units (x in width
// fractions, y in height fractions, rot radians): where the camera
// LOOKS relative to frame `start` - invert to stabilize.
struct SolveFrame {
    float tx = 0.0f, ty = 0.0f;
    float rot = 0.0f;
    float scale = 1.0f;
    float error = 0.0f;    // mean inlier residual, canvas heights
    uint32_t inliers = 0;
};

// A planar surface tracked inside the range: the seed region (uv rect
// at the range-start frame) and one 3x3 row-major homography per frame
// mapping RANGE-START uv onto that frame's uv. Solved lazily from the
// stored tracks (no re-decode) and cached with them. When the 3D solve
// covers the range-start shot the homographies come from a true 3D
// plane fit through the camera poses; otherwise from the chained 2D
// fits. Frames past the shot's end freeze at the last in-shot value.
struct PlaneSolve {
    float rx = 0.0f, ry = 0.0f, rw = 0.0f, rh = 0.0f;
    std::vector<float> h;   // 9 floats per frame, (end - start) frames
};

// 3D solve of one shot: world-to-camera poses (x_cam = R(aa)·X + t,
// y down, z forward), points keyed by feature-track id, one shared
// focal (metric units: focal length over frame height). Gauge: the
// bootstrap camera is identity and the bootstrap baseline unit length.
struct SfmCamera {
    double aa[3] = {0.0, 0.0, 0.0};   // angle-axis rotation
    double t[3] = {0.0, 0.0, 0.0};
};
struct SfmPoint {
    uint32_t track = 0;               // FeatureTrack id
    double x = 0.0, y = 0.0, z = 0.0; // bootstrap-camera coordinates
};
constexpr uint32_t kSfmUnsolved = 0;     // solver could not converge
constexpr uint32_t kSfmSolved = 1;
constexpr uint32_t kSfmLowParallax = 2;  // homography-degenerate motion:
                                         // 2D/planar stay the honest read
constexpr uint32_t kSfmTooShort = 3;
struct SfmSegment {
    uint32_t start = 0, end = 0;      // [start, end) absolute frames
    uint32_t status = kSfmUnsolved;
    double focal = 1.0;
    float mean_reproj = 0.0f;         // active observations, heights
    std::vector<SfmCamera> cams;      // end - start entries when solved
    std::vector<SfmPoint> points;
};

struct TrackData {
    uint32_t start = 0, end = 0;    // [start, end)
    uint64_t settings_hash = 0;
    std::vector<SolveFrame> solve;  // end - start entries
    std::vector<FeatureTrack> tracks;
    std::vector<PlaneSolve> planes;
    float mean_error = 0.0f;        // solve residual average, heights
    float aspect = 1.0f;            // working-base w/h (uv -> metric)
    // Scene cuts inside (start, end): each entry is the FIRST frame of
    // a new shot. Tracks never span a cut, the similarity chain resets
    // to identity there, and plane/3D solves stay per shot.
    std::vector<uint32_t> cuts;
    std::vector<SfmSegment> sfm;    // one entry per shot, in order
};

struct GrayFrame {
    const uint8_t* data = nullptr;  // luma plane, row-major
    uint32_t width = 0, height = 0, stride = 0;
};

// Runs the tracker over [start, end): `next` must yield the frames in
// order (false = decode failure, aborts). `cuts` lists frames that
// begin a new shot (the import analysis cut curve); at each one the
// tracker drops every live feature and re-anchors the similarity chain
// at identity. progress ticks once per frame; cancelled is polled once
// per frame.
bool track_run(uint32_t start, uint32_t end,
               const std::function<bool(uint32_t, GrayFrame*)>& next,
               TrackData* out, const std::vector<uint32_t>& cuts = {},
               const std::function<void(uint32_t)>& progress = {},
               const std::function<bool()>& cancelled = {});

// The full 3D solve over the stored tracks, one segment per shot:
// keyframe-pair bootstrap (8-point essential, cheirality), incremental
// resection, triangulation, then Levenberg-Marquardt bundle adjustment
// with the point blocks Schur-eliminated. Focal is estimated by a
// candidate sweep and refined in the adjustment. Homography-degenerate
// segments (pans, statics) report kSfmLowParallax instead of a fake
// solve. Deterministic; pure CPU over data->tracks. Returns true when
// any segment solved.
bool sfm_solve(TrackData* data);

bool track_load(const std::filesystem::path& path, TrackData* out);
bool track_save(const std::filesystem::path& path, const TrackData& data);

// The plane solve for `region` (uv rect), computed from the stored
// tracks on first request and appended to data->planes (match epsilon
// folds slider noise). Chained per-frame DLT + RANSAC homographies -
// deterministic, milliseconds on cached tracks. Returns null when the
// region never holds enough tracked pairs.
const PlaneSolve* ensure_plane(TrackData* data, float rx, float ry,
                               float rw, float rh);

}  // namespace looks::media

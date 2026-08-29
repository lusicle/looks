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

// Cumulative similarity from the range start; uv units, rot in radians.
// It gives the camera motion; invert it to stabilize.
struct SolveFrame {
    float tx = 0.0f, ty = 0.0f;
    float rot = 0.0f;
    float scale = 1.0f;
    float error = 0.0f;    // mean inlier residual, canvas heights
    uint32_t inliers = 0;
};

// rx, ry, rw, rh give the seed uv rect at the range-start frame.
// Each 3x3 row-major homography maps range-start uv to that frame uv.
// Frames after the shot end hold the last in-shot value.
struct PlaneSolve {
    float rx = 0.0f, ry = 0.0f, rw = 0.0f, rh = 0.0f;
    std::vector<float> h;   // 9 floats per frame, (end - start) frames
};

// Poses are world-to-camera: x_cam = R(aa)*X + t; y down, z forward.
// focal is the focal length over the frame height.
// The bootstrap camera is identity and its baseline is unit length.
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
constexpr uint32_t kSfmLowParallax = 2;  // degenerate motion: use 2D solves
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
    // Each cut is the first frame of a new shot; no track spans a cut.
    // The similarity chain resets to identity at a cut.
    std::vector<uint32_t> cuts;
    std::vector<SfmSegment> sfm;    // one entry per shot, in order
};

struct GrayFrame {
    const uint8_t* data = nullptr;  // luma plane, row-major
    uint32_t width = 0, height = 0, stride = 0;
};

// next must yield frames in order; a false return aborts the run.
// cancelled is polled once per frame.
bool track_run(uint32_t start, uint32_t end,
               const std::function<bool(uint32_t, GrayFrame*)>& next,
               TrackData* out, const std::vector<uint32_t>& cuts = {},
               const std::function<void(uint32_t)>& progress = {},
               const std::function<bool()>& cancelled = {});

// Returns true when any segment solves. Never cache a cancelled result.
bool sfm_solve(TrackData* data,
               const std::function<bool()>& cancelled = {});

bool track_load(const std::filesystem::path& path, TrackData* out);
bool track_save(const std::filesystem::path& path, const TrackData& data);

// Rects match within an epsilon, so slider noise reuses one cached plane.
// Returns null when the region has too few tracked pairs.
const PlaneSolve* ensure_plane(TrackData* data, float rx, float ry,
                               float rw, float rh);

}  // namespace looks::media

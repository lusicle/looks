#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "doc/document.h"

namespace looks::mod {

// An empty curve reads as 0. A missing sidecar turns those sources off.
struct AnalysisCurves {
    std::vector<float> low, mid, high;   // band energy, normalized 0..1
    std::vector<float> onset;            // 0/1-ish spectral-flux peaks
    std::vector<float> motion;           // mean |frame delta|, 0..1
    std::vector<float> brightness;       // mean luma, 0..1
    std::vector<float> cut;              // 1 at detected scene cuts
    float bpm = 0.0f;
    // The curves' frame grid rate.
    double fps = 0.0;

    float sample(const std::vector<float>& curve, uint32_t frame) const {
        if (curve.empty()) return 0.0f;
        return curve[frame < curve.size() ? frame : curve.size() - 1];
    }
};

// Media frame behind look-local L is floor(L * rate) + slip + offset.
// Keyed by value-node id. A missing entry reads 0, never the global curves.
struct NodeAudio {
    std::shared_ptr<const AnalysisCurves> curves;
    uint32_t slip = 0;
    int64_t offset = 0;
    double rate = 1.0;   // media frames per look-clock frame
};
using NodeAudioMap = std::unordered_map<uint64_t, NodeAudio>;

// The index is the media frame from start. Frames outside clamp.
// tx and ty are uv fractions, rot is radians, and scale is a ratio.
struct CameraCurves {
    uint32_t start = 0;
    std::vector<float> tx, ty, rot, scale;
    // Region quad corners in uv, per frame; empty without a region.
    std::vector<float> corner[8];   // tlx tly trx try blx bly brx bry
    // Anchor uv projection; out-of-shot frames hold nearest. Empty = none.
    std::vector<float> anchor_x, anchor_y;
    float sample(const std::vector<float>& c, uint32_t media_frame) const {
        if (c.empty()) return 0.0f;
        const size_t i = media_frame <= start
            ? 0
            : std::min<size_t>(media_frame - start, c.size() - 1);
        return c[i];
    }
};
struct NodeCamera {
    std::shared_ptr<const CameraCurves> curves;
    uint32_t slip = 0;
    int64_t offset = 0;
    double rate = 1.0;   // media frames per look-clock frame
};
using NodeCameraMap = std::unordered_map<uint64_t, NodeCamera>;

// Callers pass the same frame they render so determinism holds.
// A null view or null planes read as 0.
struct SourceFrameView {
    const uint8_t* y = nullptr;
    int y_stride = 0;
    const uint8_t* u = nullptr;
    int u_stride = 0;
    const uint8_t* v = nullptr;
    int v_stride = 0;
    int width = 0, height = 0;
    // nv12: u is the interleaved CbCr plane; v is unused.
    bool nv12 = false;
};

// t is seconds = frame / fps. Positive audio offset moves audio later.
// key_time -1 = none. A null video makes video sources read 0.
float eval_source(const doc::ModSource& source, double t_seconds,
                  uint32_t frame_index, const AnalysisCurves* analysis,
                  double fps = 30.0, double audio_offset_seconds = 0.0,
                  double key_time = -1.0,
                  const SourceFrameView* video = nullptr);

struct ValueEnv {
    const doc::Look* look = nullptr;
    double t = 0.0;
    uint32_t frame = 0;
    const AnalysisCurves* analysis = nullptr;
    double fps = 30.0;
    double audio_off = 0.0;
    double key_time = -1.0;
    const SourceFrameView* video = nullptr;
    const NodeAudioMap* node_audio = nullptr;
    const NodeCameraMap* node_camera = nullptr;
};

// A dangling id reads 0. Unwired inputs read the node's constants.
// Commands keep the graph acyclic; the depth cap only guards corrupt files.
float eval_value_node(const ValueEnv& env, uint64_t node_id, int depth = 0);

// Linear passes x raw; shaped curves clamp x to [0,1] first.
float apply_curve(doc::ResponseCurve curve, float x);

// Keys must be sorted.
float eval_lane(const doc::KeyframeLane& lane, double frame);

// The out parameter must be a copy of look.
void resolve_look(const doc::Look& look, doc::Look& out,
                  uint32_t local_frame, double fps,
                  const AnalysisCurves* analysis, double audio_off,
                  double live_seconds = -1.0, double key_time = -1.0,
                  const SourceFrameView* video = nullptr,
                  const NodeAudioMap* node_audio = nullptr,
                  const NodeCameraMap* node_camera = nullptr);

// live_seconds >= 0 moves LFO/drift onto the live clock.
// Analysis sources and lanes stay on the playhead either way.
doc::Document resolve(const doc::Document& doc, uint32_t frame_index,
                      double fps, const AnalysisCurves* analysis,
                      double live_seconds = -1.0, double key_time = -1.0,
                      const SourceFrameView* video = nullptr,
                      const NodeAudioMap* node_audio = nullptr,
                      const NodeCameraMap* node_camera = nullptr);

// Clamped to [0, doc::kMaxSpeed].
float speed_at(const doc::Document& doc, uint32_t frame_index, double fps,
               const AnalysisCurves* analysis, double live_seconds = -1.0);

bool time_remap_active(const doc::Document& doc);

// Source position at t is the prefix sum of speed over frames [0, t).
// Incremental forward; a backward seek recomputes from zero.
struct TimeRemap {
    double position = 0.0;
    uint32_t next_frame = 0;   // frame the accumulated position belongs to
    bool valid = false;

    void reset() { valid = false; }
    uint32_t source_frame(const doc::Document& doc, uint32_t frame,
                          double fps, const AnalysisCurves* analysis,
                          uint32_t source_count, double live_seconds = -1.0);
};

}  // namespace looks::mod

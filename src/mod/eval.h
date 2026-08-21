// Modulation evaluation: deterministic, fixed-timestep on
// frame index — never wall clock. resolve() bakes lanes + routes into a
// copy of the document; the engine renders the copy untouched, so preview
// and export share one code path by construction.
//
//   base  = keyframe lane value at frame (else the stored param)
//   final = wired ? clamp(min + span · curve(node), min, max) : base
//
// A wire REPLACES the base: the node's output maps onto the param's
// range, and depth/offset shaping lives in the value graph (math,
// normalise), never on the wire. Generator nodes emit [0,1]; helper
// nodes may leave that range - the Linear curve passes values through
// raw, shaped curves clamp to their [0,1] domain, and the param's own
// range clamp is the final bound.

#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "doc/document.h"

namespace looks::mod {

// Import-time analysis curves, sampled per video frame. Empty
// vectors read as 0 — a missing .analysis silently disables those sources.
struct AnalysisCurves {
    std::vector<float> low, mid, high;   // band energy, normalized 0..1
    std::vector<float> onset;            // 0/1-ish spectral-flux peaks
    std::vector<float> motion;           // mean |frame delta|, 0..1
    std::vector<float> brightness;       // mean luma, 0..1
    std::vector<float> cut;              // 1 at detected scene cuts
    float bpm = 0.0f;

    float sample(const std::vector<float>& curve, uint32_t frame) const {
        if (curve.empty()) return 0.0f;
        return curve[frame < curve.size() ? frame : curve.size() - 1];
    }
};

// Runtime analysis for audio-driven nodes (ValueNode.audio_src): curves
// computed from the wired chain's PROCESSED audio, media-frame indexed;
// slip + offset (Offset shims on the chain) map the look clock onto the
// curve. Keyed by value-node id (ids are globally unique). The input is
// REQUIRED: an unwired node or a wire with no entry reads 0 - never the
// global curves. Beat/LfoBeat clocks anchor on the same media position,
// so cuts of one media beat-match by construction.
struct NodeAudio {
    std::shared_ptr<const AnalysisCurves> curves;
    uint32_t slip = 0;
    int64_t offset = 0;
};
using NodeAudioMap = std::unordered_map<uint64_t, NodeAudio>;

// CPU view of the decoded source frame (I420) for the video-sampling
// sources (sample-at-point / region-average).
// Callers pass the SAME frame they are about to render, so preview and
// export sample identical decoded pixels and determinism holds.
// A null view (or null planes) reads as 0.
struct SourceFrameView {
    const uint8_t* y = nullptr;
    int y_stride = 0;
    const uint8_t* u = nullptr;
    int u_stride = 0;
    const uint8_t* v = nullptr;
    int v_stride = 0;
    int width = 0, height = 0;
    // NV12: `u` is the interleaved CbCr plane (2 bytes per sample), `v`
    // unused - same layout the native decode path uploads.
    bool nv12 = false;
};

// One source's value at time t (seconds, = frame/fps). Deterministic.
// fps feeds the envelope's frame->seconds conversion and the BPM-synced
// LFO's fallback clock; sources that don't need it ignore it.
// audio_offset_seconds (nudge): shifts every audio-derived source
// (beat/BPM clocks, envelope onset triggers, wired analysis nodes)
// against video — positive = audio later. key_time: seconds of the last live keypress trigger (-1 = none;
// live mode only, exempts it). video: the current source frame
// for VideoSample/VideoRegion; those sources read 0 without it — notably
// on the speed target, where the sampled frame would itself depend on
// speed (speed_at never passes a view).
float eval_source(const doc::ModSource& source, double t_seconds,
                  uint32_t frame_index, const AnalysisCurves* analysis,
                  double fps = 30.0, double audio_offset_seconds = 0.0,
                  double key_time = -1.0,
                  const SourceFrameView* video = nullptr);

// Everything one value-graph evaluation needs, bundled so the recursive
// walk stays a two-argument call.
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
};

// One value node's output at the env's frame: generators via
// eval_source, helpers recursing through their inputs (unwired inputs
// read the node's constants). A dangling id reads 0. The graph is
// acyclic by command guard; the depth cap only defends corrupt files.
float eval_value_node(const ValueEnv& env, uint64_t node_id, int depth = 0);

// Response shaping. Linear passes x through RAW (helper chains go
// bipolar); the shaped curves clamp to their [0,1] domain first.
float apply_curve(doc::ResponseCurve curve, float x);

// Lane value at `frame` (bezier segments, hold keys). Keys must be sorted.
float eval_lane(const doc::KeyframeLane& lane, double frame);

// Bakes ONE look's modulation at a LOCAL frame into `out` (a copy of
// `look`). The per-look primitive: two instances of the same look resolve
// it at different local frames, which is what nesting needs.
void resolve_look(const doc::Look& look, doc::Look& out,
                  uint32_t local_frame, double fps,
                  const AnalysisCurves* analysis, double audio_off,
                  double live_seconds = -1.0, double key_time = -1.0,
                  const SourceFrameView* video = nullptr,
                  const NodeAudioMap* node_audio = nullptr);

// Bakes modulation into a document copy for one frame. live_seconds >= 0
// switches LFO/drift onto that clock instead of frame/fps (live mode,
// — explicitly exempt from determinism, ); analysis-backed
// sources and lanes stay on the playhead either way.
doc::Document resolve(const doc::Document& doc, uint32_t frame_index,
                      double fps, const AnalysisCurves* analysis,
                      double live_seconds = -1.0, double key_time = -1.0,
                      const SourceFrameView* video = nullptr,
                      const NodeAudioMap* node_audio = nullptr);

// Playback speed of the root timeline: the project scalar, clamped to
// [0, doc::kMaxSpeed]. Sequences carry no keyframes or routes; ramps
// live per-block or inside looks.
float speed_at(const doc::Document& doc, uint32_t frame_index, double fps,
               const AnalysisCurves* analysis, double live_seconds = -1.0);

// True when playback is remapped at all: non-1x base speed or a
// non-forward mode.
bool time_remap_active(const doc::Document& doc);

// Deterministic time remap: the source position at frame
// t is the prefix sum of speed over frames [0, t) — fixed timestep on the
// frame index, identical in preview and export. Incremental during
// sequential playback; a backward seek recomputes the prefix from zero.
// Forward wraps around the media, reverse runs from the end, ping-pong
// folds over the media; a 1x forward document is the identity mapping.
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

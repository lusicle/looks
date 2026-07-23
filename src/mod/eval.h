// Modulation evaluation (spec §7, §11): deterministic, fixed-timestep on
// frame index — never wall clock. resolve() bakes lanes + routes into a
// copy of the document; the engine renders the copy untouched, so preview
// and export share one code path by construction.
//
//   base  = keyframe lane value at frame (else the stored param)
//   final = clamp(base + Σ amount_i · span · curve_i(source_i), min, max)
//
// Sources emit [0,1]; amount is signed and scaled by the param's range.

#pragma once

#include <cstdint>
#include <vector>

#include "doc/document.h"

namespace looks::mod {

// Import-time analysis curves, sampled per video frame (spec §7). Empty
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

// One source's value at time t (seconds, = frame/fps). Deterministic.
// fps feeds the envelope's frame->seconds conversion and the BPM-synced
// LFO's fallback clock; sources that don't need it ignore it.
// audio_offset_seconds (spec §7 nudge): shifts every audio-derived source
// (bands, onsets, beat, BPM clocks) against video — positive = audio
// later. key_time: seconds of the last live keypress trigger (-1 = none;
// live mode only, spec §11 exempts it).
float eval_source(const doc::ModSource& source, double t_seconds,
                  uint32_t frame_index, const AnalysisCurves* analysis,
                  double fps = 30.0, double audio_offset_seconds = 0.0,
                  double key_time = -1.0);

float apply_curve(doc::ResponseCurve curve, float x);   // [0,1] -> [0,1]

// Lane value at `frame` (bezier segments, hold keys). Keys must be sorted.
float eval_lane(const doc::KeyframeLane& lane, double frame);

// Bakes modulation into a document copy for one frame. live_seconds >= 0
// switches LFO/drift onto that clock instead of frame/fps (live mode,
// spec §9 — explicitly exempt from determinism, spec §11); analysis-backed
// sources and lanes stay on the playhead either way.
doc::Document resolve(const doc::Document& doc, uint32_t frame_index,
                      double fps, const AnalysisCurves* analysis,
                      double live_seconds = -1.0, double key_time = -1.0);

// Playback speed at one timeline frame (spec §6.1 speed ramp): doc.speed,
// overridden by a lane on ParamKey {0, 1} ("global.speed"), plus routes on
// that key. Clamped to [0, doc::kMaxSpeed].
float speed_at(const doc::Document& doc, uint32_t frame_index, double fps,
               const AnalysisCurves* analysis, double live_seconds = -1.0);

// True when playback is remapped at all: non-1x base speed, a non-forward
// mode, or any lane/route on the speed param.
bool time_remap_active(const doc::Document& doc);

// Deterministic time remap (spec §6.1 / §11): the source position at frame
// t is the prefix sum of speed over frames [0, t) — fixed timestep on the
// frame index, identical in preview and export. Incremental during
// sequential playback; a backward seek recomputes the prefix from zero.
// Forward wraps around the clip, reverse runs from the end, ping-pong
// folds over the clip; a 1x forward document is the identity mapping.
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

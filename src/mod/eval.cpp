#include "mod/eval.h"

#include <algorithm>
#include <cmath>

#include "doc/stack_commands.h"
#include "mod/param_table.h"
#include "util/hash.h"

namespace looks::mod {

namespace {

constexpr double kPi = 3.14159265358979323846;

float smooth01(float x) { return x * x * (3.0f - 2.0f * x); }

// Shape evaluation at phase position pt (in cycles).
float lfo_shape_at(const doc::ModSource& s, double pt) {
    const double frac = pt - std::floor(pt);
    switch (s.shape) {
        case doc::LfoShape::Sine:
            return 0.5f + 0.5f * static_cast<float>(std::sin(2.0 * kPi * pt));
        case doc::LfoShape::Triangle:
            return 1.0f - std::fabs(2.0f * static_cast<float>(frac) - 1.0f);
        case doc::LfoShape::Square:
            return frac < 0.5 ? 1.0f : 0.0f;
        case doc::LfoShape::SampleHold:
            return hash_float01(s.seed,
                                static_cast<uint64_t>(std::floor(pt) + 1.0e9));
        default:
            return 0.0f;
    }
}

float eval_lfo(const doc::ModSource& s, double t) {
    return lfo_shape_at(s, static_cast<double>(s.rate_hz) * t +
                               static_cast<double>(s.phase));
}

// BPM-synced LFO (spec §7): rate_hz holds beats-per-cycle. Falls back to
// 120 BPM when the clip has no analysis/estimate.
float eval_lfo_beat(const doc::ModSource& s, double t,
                    const AnalysisCurves* analysis) {
    const double bpm =
        analysis && analysis->bpm > 1.0f ? analysis->bpm : 120.0;
    const double beats = bpm / 60.0 * t;
    const double per_cycle =
        std::max(static_cast<double>(s.rate_hz), 0.0625);
    return lfo_shape_at(s, beats / per_cycle + static_cast<double>(s.phase));
}

// Deterministic pulse train on the BPM grid (spec §7 beat trigger): sharp
// attack into an exponential decay, one burst per beat.
float eval_beat(const doc::ModSource& s, double t,
                const AnalysisCurves* analysis) {
    const double bpm =
        analysis && analysis->bpm > 1.0f ? analysis->bpm : 120.0;
    const double beat_len = 60.0 / bpm;
    double dt = std::fmod(t, beat_len);
    if (dt < 0.0) dt += beat_len;
    const double attack =
        std::clamp(static_cast<double>(s.attack), 1e-4, beat_len * 0.5);
    const double decay = std::max(static_cast<double>(s.decay), 1e-3);
    if (dt < attack) return static_cast<float>(dt / attack);
    return static_cast<float>(std::exp(-(dt - attack) / decay));
}

// Envelope (spec §7): attack-decay burst fired by the most recent trigger
// (audio onset / scene cut / beat / live keypress) at or before this
// frame. Onset/cut/beat variants are pure functions of the frame index —
// scrubbing and export land on identical values; keypress is live-only.
float eval_envelope(const doc::ModSource& s, uint32_t frame_index,
                    const AnalysisCurves* analysis, double fps,
                    double t_seconds, double key_time) {
    if (s.trigger == 2) return eval_beat(s, t_seconds, analysis);
    if (s.trigger == 3) {
        if (key_time < 0.0 || t_seconds < key_time) return 0.0f;
        const double dt = t_seconds - key_time;
        const double attack = std::max(static_cast<double>(s.attack), 1e-4);
        const double decay = std::max(static_cast<double>(s.decay), 1e-3);
        if (dt < attack) return static_cast<float>(dt / attack);
        return static_cast<float>(std::exp(-(dt - attack) / decay));
    }
    if (!analysis || fps <= 0.0) return 0.0f;
    const std::vector<float>& trig =
        s.trigger == 1 ? analysis->cut : analysis->onset;
    if (trig.empty()) return 0.0f;
    const uint32_t last = static_cast<uint32_t>(trig.size()) - 1;
    const uint32_t start = std::min(frame_index, last);
    // Bounded backward scan: past ~6 decay constants the burst is silent.
    const uint32_t window = std::min<uint32_t>(
        600, static_cast<uint32_t>((s.attack + 6.0f * s.decay) * fps) + 2);
    for (uint32_t k = 0; k <= std::min(start, window); ++k) {
        if (trig[start - k] <= 0.5f) continue;
        // Sample mid-frame so a sub-frame attack still peaks on the
        // trigger frame instead of reading 0 at dt = 0.
        const double dt = k / fps + 0.5 / fps;
        const double attack = std::max(static_cast<double>(s.attack), 1e-4);
        const double decay = std::max(static_cast<double>(s.decay), 1e-3);
        if (dt < attack) return static_cast<float>(dt / attack);
        return static_cast<float>(std::exp(-(dt - attack) / decay));
    }
    return 0.0f;
}

// Video sampling (docs/flow_canvas.md v4). Averages the channel over a
// decimated tap grid: the point variant uses a small fixed box and the
// region variant caps its grid, so cost stays bounded and single 8-bit
// code-value steps get band-averaged instead of popping when a large
// amount scales them (the CLAUDE.md luma rule).
float eval_video(const doc::ModSource& s, const SourceFrameView* video) {
    if (!video || !video->y || video->width <= 0 || video->height <= 0)
        return 0.0f;
    const float w = static_cast<float>(video->width);
    const float h = static_cast<float>(video->height);
    float half_w, half_h;
    if (s.type == doc::ModSourceType::VideoSample) {
        half_w = half_h = 3.0f;   // pixels: 7x7 box around the point
    } else {
        half_w = std::max(s.pw, 0.01f) * w * 0.5f;
        half_h = std::max(s.ph, 0.01f) * h * 0.5f;
    }
    const float cx = std::clamp(s.px, 0.0f, 1.0f) * (w - 1.0f);
    const float cy = std::clamp(s.py, 0.0f, 1.0f) * (h - 1.0f);
    const int x0 = std::clamp(static_cast<int>(cx - half_w), 0,
                              video->width - 1);
    const int x1 = std::clamp(static_cast<int>(cx + half_w), 0,
                              video->width - 1);
    const int y0 = std::clamp(static_cast<int>(cy - half_h), 0,
                              video->height - 1);
    const int y1 = std::clamp(static_cast<int>(cy + half_h), 0,
                              video->height - 1);
    const int nx = std::min(x1 - x0 + 1, 48);
    const int ny = std::min(y1 - y0 + 1, 48);
    const bool luma = s.channel == 0 || !video->u || !video->v;
    float sum = 0.0f;
    for (int j = 0; j < ny; ++j) {
        const int py = y0 + ((y1 - y0) * j) / std::max(ny - 1, 1);
        const uint8_t* yrow = video->y + py * video->y_stride;
        for (int i = 0; i < nx; ++i) {
            const int px = x0 + ((x1 - x0) * i) / std::max(nx - 1, 1);
            const float Y = static_cast<float>(yrow[px]);
            if (luma) {
                sum += Y;
                continue;
            }
            // I420 chroma at half resolution; BT.601 — close enough for a
            // control value, and identical preview vs export.
            const float U = static_cast<float>(
                video->u[(py >> 1) * video->u_stride + (px >> 1)]) - 128.0f;
            const float V = static_cast<float>(
                video->v[(py >> 1) * video->v_stride + (px >> 1)]) - 128.0f;
            switch (s.channel) {
                case 1: sum += Y + 1.402f * V; break;
                case 2: sum += Y - 0.344f * U - 0.714f * V; break;
                default: sum += Y + 1.772f * U; break;
            }
        }
    }
    const float mean = sum / (static_cast<float>(nx) *
                              static_cast<float>(ny) * 255.0f);
    return std::clamp(mean, 0.0f, 1.0f);
}

float eval_drift(const doc::ModSource& s, double t) {
    // Value noise: smooth interpolation across a seeded lattice.
    const double pt = static_cast<double>(s.rate_hz) * t +
                      static_cast<double>(s.phase);
    const double cell = std::floor(pt);
    const float frac = static_cast<float>(pt - cell);
    const uint64_t i = static_cast<uint64_t>(cell + 1.0e9);
    const float a = hash_float01(s.seed, i);
    const float b = hash_float01(s.seed, i + 1);
    return a + (b - a) * smooth01(frac);
}

// Cubic bezier segment between k0 and k1: solve x(t) = frame, return y(t).
float eval_bezier(const doc::Keyframe& k0, const doc::Keyframe& k1,
                  double frame) {
    const double span = k1.frame - k0.frame;
    if (span <= 0.0) return k1.value;

    // Zero handles = linear.
    if (k0.out_dx == 0.0f && k0.out_dy == 0.0f && k1.in_dx == 0.0f &&
        k1.in_dy == 0.0f) {
        const double u = (frame - k0.frame) / span;
        return k0.value +
               static_cast<float>(u) * (k1.value - k0.value);
    }

    // Control points; x handles clamped inside the segment so x(t) stays
    // monotonic and the solve below converges.
    const double x0 = k0.frame;
    const double x3 = k1.frame;
    const double x1 = std::clamp(x0 + static_cast<double>(k0.out_dx), x0, x3);
    const double x2 = std::clamp(x3 + static_cast<double>(k1.in_dx), x0, x3);
    const float y0 = k0.value;
    const float y1 = k0.value + k0.out_dy;
    const float y2 = k1.value + k1.in_dy;
    const float y3 = k1.value;

    auto bez = [](double a, double b, double c, double d, double t) {
        const double s = 1.0 - t;
        return s * s * s * a + 3.0 * s * s * t * b + 3.0 * s * t * t * c +
               t * t * t * d;
    };

    // Bisection on monotonic x(t) — deterministic, 24 steps ≈ 1e-7.
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 24; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (bez(x0, x1, x2, x3, mid) < frame) lo = mid;
        else hi = mid;
    }
    const double t = 0.5 * (lo + hi);
    return static_cast<float>(bez(y0, y1, y2, y3, t));
}

}  // namespace

float eval_source(const doc::ModSource& source, double t_seconds,
                  uint32_t frame_index, const AnalysisCurves* analysis,
                  double fps, double audio_offset_seconds, double key_time,
                  const SourceFrameView* video) {
    // Audio nudge (spec §7): audio-derived sources read a shifted clock so
    // a positive offset delays audio-driven wiggles against video.
    const double ta = t_seconds - audio_offset_seconds;
    uint32_t af = frame_index;
    if (audio_offset_seconds != 0.0 && fps > 0.0) {
        const double shifted =
            static_cast<double>(frame_index) - audio_offset_seconds * fps;
        af = shifted <= 0.0 ? 0u : static_cast<uint32_t>(shifted + 0.5);
    }
    switch (source.type) {
        case doc::ModSourceType::Lfo:
            return eval_lfo(source, t_seconds);
        case doc::ModSourceType::LfoBeat:
            return eval_lfo_beat(source, ta, analysis);
        case doc::ModSourceType::Beat:
            return eval_beat(source, ta, analysis);
        case doc::ModSourceType::Envelope:
            // Onset triggers ride the audio clock; cuts are video-derived.
            return eval_envelope(source,
                                 source.trigger == 0 ? af : frame_index,
                                 analysis, fps, source.trigger == 2 ? ta
                                                                    : t_seconds,
                                 key_time);
        case doc::ModSourceType::VideoCut:
            return analysis ? analysis->sample(analysis->cut, frame_index)
                            : 0.0f;
        case doc::ModSourceType::Drift:
            return eval_drift(source, t_seconds);
        case doc::ModSourceType::AudioLow:
            return analysis ? analysis->sample(analysis->low, af) : 0.0f;
        case doc::ModSourceType::AudioMid:
            return analysis ? analysis->sample(analysis->mid, af) : 0.0f;
        case doc::ModSourceType::AudioHigh:
            return analysis ? analysis->sample(analysis->high, af) : 0.0f;
        case doc::ModSourceType::AudioOnset:
            return analysis ? analysis->sample(analysis->onset, af) : 0.0f;
        case doc::ModSourceType::VideoMotion:
            return analysis ? analysis->sample(analysis->motion, frame_index) : 0.0f;
        case doc::ModSourceType::VideoBrightness:
            return analysis ? analysis->sample(analysis->brightness, frame_index)
                            : 0.0f;
        case doc::ModSourceType::VideoSample:
        case doc::ModSourceType::VideoRegion:
            return eval_video(source, video);
        default:
            return 0.0f;
    }
}

float apply_curve(doc::ResponseCurve curve, float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    switch (curve) {
        case doc::ResponseCurve::Linear: return x;
        case doc::ResponseCurve::Exp: return x * x;
        case doc::ResponseCurve::SCurve: return smooth01(x);
        case doc::ResponseCurve::Inverted: return 1.0f - x;
        default: return x;
    }
}

float eval_lane(const doc::KeyframeLane& lane, double frame) {
    const auto& keys = lane.keys;
    if (keys.empty()) return 0.0f;
    // Loopable region (spec §7): wrap through the key span once inside it.
    if (lane.loop && keys.size() >= 2) {
        const double start = keys.front().frame;
        const double span = keys.back().frame - start;
        if (span > 0.0 && frame > start)
            frame = start + std::fmod(frame - start, span);
    }
    if (frame <= keys.front().frame) return keys.front().value;
    if (frame >= keys.back().frame) return keys.back().value;

    // Find the segment [k0, k1] containing `frame`.
    size_t hi = 1;
    while (hi < keys.size() && keys[hi].frame < frame) ++hi;
    const doc::Keyframe& k0 = keys[hi - 1];
    const doc::Keyframe& k1 = keys[hi];
    if (k0.hold) return k0.value;
    return eval_bezier(k0, k1, frame);
}

doc::Document resolve(const doc::Document& doc, uint32_t frame_index,
                      double fps, const AnalysisCurves* analysis,
                      double live_seconds, double key_time,
                      const SourceFrameView* video) {
    doc::Document out = doc;
    const double t = live_seconds >= 0.0
                         ? live_seconds
                         : (fps > 0.0 ? frame_index / fps : 0.0);
    const double audio_off =
        static_cast<double>(doc.audio_offset_ms) * 0.001;

    auto param_slot = [](doc::EffectInstance& fx, int param_index) -> float* {
        if (param_index == doc::kWetParam) return &fx.wet;
        if (param_index == doc::kOpacityParam) return &fx.opacity;
        if (param_index >= 0 &&
            static_cast<size_t>(param_index) < fx.params.size())
            return &fx.params[static_cast<size_t>(param_index)];
        return nullptr;
    };

    // Morph (spec §7): interpolate between two snapshot slots; the position
    // itself is a mod target (ParamKey {0, 0}) so routes are applied to it
    // first. Runs before lanes — morph sets base values like snapshots do.
    {
        float pos = doc.morph_pos;
        for (const doc::ModRoute& route : doc.mod_routes) {
            if (route.target.effect_id != 0 || route.target.param_index != 0)
                continue;
            const float value =
                apply_curve(route.curve,
                            eval_source(route.source, t, frame_index,
                                        analysis, fps, audio_off, key_time,
                                        video));
            pos += route.amount * value;
        }
        pos = std::clamp(pos, 0.0f, 1.0f);
        const bool from_ok = doc.morph_from >= 0 && doc.morph_from < 3 &&
                             doc.snapshots[doc.morph_from].valid;
        const bool to_ok = doc.morph_to >= 0 && doc.morph_to < 3 &&
                           doc.snapshots[doc.morph_to].valid;
        if (from_ok && to_ok && pos > 0.0f) {
            const doc::Snapshot& a = doc.snapshots[doc.morph_from];
            const doc::Snapshot& b = doc.snapshots[doc.morph_to];
            for (const doc::SnapshotEntry& ea : a.entries) {
                const doc::SnapshotEntry* eb = nullptr;
                for (const doc::SnapshotEntry& e : b.entries)
                    if (e.effect_id == ea.effect_id) eb = &e;
                if (!eb) continue;
                size_t layer = 0, index = 0;
                if (!find_effect(out, ea.effect_id, &layer, &index)) continue;
                doc::EffectInstance& fx = out.layers[layer].stack[index];
                const size_t np =
                    std::min({fx.params.size(), ea.params.size(),
                              eb->params.size()});
                for (size_t p = 0; p < np; ++p)
                    fx.params[p] =
                        ea.params[p] + (eb->params[p] - ea.params[p]) * pos;
                fx.wet = ea.wet + (eb->wet - ea.wet) * pos;
                fx.opacity = ea.opacity + (eb->opacity - ea.opacity) * pos;
            }
        }
    }

    // Mask targets (spec §8): keys carry kMaskParamBit + the mask id.
    auto mask_slot = [&](const doc::ParamKey& key, float* min_v,
                         float* max_v) -> float* {
        if (!(key.effect_id & doc::kMaskParamBit)) return nullptr;
        const uint64_t id = key.effect_id & ~doc::kMaskParamBit;
        for (doc::Mask& m : out.masks)
            if (m.id == id) {
                mask_param_range(key.param_index, min_v, max_v);
                return mask_param_slot(m, key.param_index);
            }
        return nullptr;
    };

    // Keyframe lanes set the base (muted lanes keep keys, drive nothing).
    for (const doc::KeyframeLane& lane : doc.lanes) {
        if (lane.keys.empty() || lane.muted) continue;
        float min_v = 0.0f, max_v = 1.0f;
        if (float* mslot = mask_slot(lane.target, &min_v, &max_v)) {
            *mslot = std::clamp(eval_lane(lane, frame_index), min_v, max_v);
            continue;
        }
        size_t layer = 0, index = 0;
        if (!find_effect(out, lane.target.effect_id, &layer, &index)) continue;
        doc::EffectInstance& fx = out.layers[layer].stack[index];
        float* slot = param_slot(fx, lane.target.param_index);
        if (!slot) continue;
        param_range(fx.type, lane.target.param_index, &min_v, &max_v);
        *slot = std::clamp(eval_lane(lane, frame_index), min_v, max_v);
    }

    // Routes add on top. (Group faces are DIRECT param aliases — v5.3 —
    // they have no resolve-time behavior of their own.)
    for (const doc::ModRoute& route : doc.mod_routes) {
        float min_v = 0.0f, max_v = 1.0f;
        float* slot = mask_slot(route.target, &min_v, &max_v);
        if (!slot) {
            size_t layer = 0, index = 0;
            if (!find_effect(out, route.target.effect_id, &layer, &index))
                continue;
            doc::EffectInstance& fx = out.layers[layer].stack[index];
            slot = param_slot(fx, route.target.param_index);
            if (!slot) continue;
            param_range(fx.type, route.target.param_index, &min_v, &max_v);
        }
        const float value =
            apply_curve(route.curve,
                        eval_source(route.source, t, frame_index, analysis,
                                    fps, audio_off, key_time, video));
        *slot = std::clamp(*slot + route.amount * (max_v - min_v) * value,
                           min_v, max_v);
    }
    return out;
}

float speed_at(const doc::Document& doc, uint32_t frame_index, double fps,
               const AnalysisCurves* analysis, double live_seconds) {
    float speed = doc.speed;
    for (const doc::KeyframeLane& lane : doc.lanes) {
        if (lane.target.effect_id != 0 || lane.target.param_index != 1 ||
            lane.keys.empty() || lane.muted)
            continue;
        speed = eval_lane(lane, frame_index);   // lane sets the base
    }
    const double t = live_seconds >= 0.0
                         ? live_seconds
                         : (fps > 0.0 ? frame_index / fps : 0.0);
    for (const doc::ModRoute& route : doc.mod_routes) {
        if (route.target.effect_id != 0 || route.target.param_index != 1)
            continue;
        const float value = apply_curve(
            route.curve,
            eval_source(route.source, t, frame_index, analysis, fps,
                        static_cast<double>(doc.audio_offset_ms) * 0.001));
        speed += route.amount * doc::kMaxSpeed * value;
    }
    return std::clamp(speed, 0.0f, doc::kMaxSpeed);
}

bool time_remap_active(const doc::Document& doc) {
    if (doc.time_mode != 0 || doc.speed != 1.0f) return true;
    for (const doc::KeyframeLane& lane : doc.lanes)
        if (lane.target.effect_id == 0 && lane.target.param_index == 1 &&
            !lane.keys.empty() && !lane.muted)
            return true;
    for (const doc::ModRoute& route : doc.mod_routes)
        if (route.target.effect_id == 0 && route.target.param_index == 1)
            return true;
    return false;
}

uint32_t TimeRemap::source_frame(const doc::Document& doc, uint32_t frame,
                                 double fps, const AnalysisCurves* analysis,
                                 uint32_t source_count, double live_seconds) {
    if (source_count == 0) return 0;
    if (!time_remap_active(doc)) {
        valid = false;
        return std::min(frame, source_count - 1);
    }
    if (!valid || frame < next_frame) {
        position = 0.0;
        next_frame = 0;
        valid = true;
    }
    while (next_frame < frame) {
        position += speed_at(doc, next_frame, fps, analysis, live_seconds);
        ++next_frame;
    }
    const double count = static_cast<double>(source_count);
    const double last = count - 1.0;
    switch (doc.time_mode) {
        case 1: {   // reverse: run backwards from the end, wrapping
            const double p = std::floor(std::fmod(position, count));
            return static_cast<uint32_t>(last - p);
        }
        case 2: {   // ping-pong: fold over [0, last]
            if (source_count == 1) return 0;
            const double period = 2.0 * last;
            const double p = std::fmod(position, period);
            const double folded = p <= last ? p : period - p;
            return static_cast<uint32_t>(std::floor(folded));
        }
        default: {  // forward: wrap around the clip
            return static_cast<uint32_t>(std::floor(std::fmod(position, count)));
        }
    }
}

}  // namespace looks::mod

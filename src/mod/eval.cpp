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

// BPM-synced LFO: rate_hz holds beats-per-cycle. Falls back to
// 120 BPM when the media has no analysis/estimate.
float eval_lfo_beat(const doc::ModSource& s, double t,
                    const AnalysisCurves* analysis) {
    const double bpm =
        analysis && analysis->bpm > 1.0f ? analysis->bpm : 120.0;
    const double beats = bpm / 60.0 * t;
    const double per_cycle =
        std::max(static_cast<double>(s.rate_hz), 0.0625);
    return lfo_shape_at(s, beats / per_cycle + static_cast<double>(s.phase));
}

// Deterministic pulse train on the BPM grid (beat trigger): sharp
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

// Envelope: attack-decay burst fired by the most recent trigger
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

// Video sampling. Averages the channel over a decimated tap grid: the
// point variant uses a small fixed box and the region variant caps its
// grid, so cost stays bounded and single 8-bit code-value steps get
// band-averaged instead of popping when downstream shaping magnifies
// them.
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
    const bool luma =
        s.channel == 0 || !video->u || (!video->nv12 && !video->v);
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
            // Half-res chroma; BT.601 — close enough for a control
            // value, and identical preview vs export.
            float U, V;
            if (video->nv12) {
                const uint8_t* uv = video->u +
                                    (py >> 1) * video->u_stride +
                                    ((px >> 1) << 1);
                U = static_cast<float>(uv[0]) - 128.0f;
                V = static_cast<float>(uv[1]) - 128.0f;
            } else {
                U = static_cast<float>(
                        video->u[(py >> 1) * video->u_stride +
                                 (px >> 1)]) - 128.0f;
                V = static_cast<float>(
                        video->v[(py >> 1) * video->v_stride +
                                 (px >> 1)]) - 128.0f;
            }
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
    // Audio-driven kinds (bands, onset, Beat, LfoBeat, Envelope's
    // onset/beat triggers) evaluate ONLY on the wired path in
    // eval_value_node - they require their media input, and the global
    // curves carry no audio for them. The nudge rides that path too.
    (void)audio_offset_seconds;
    switch (source.type) {
        case doc::ModSourceType::Lfo:
            return eval_lfo(source, t_seconds);
        case doc::ModSourceType::Envelope:
            // Only the video-cut and live-keypress triggers are not
            // audio; the onset/beat triggers read 0 here (unwired =
            // never fires).
            if (source.trigger != 1 && source.trigger != 3) return 0.0f;
            return eval_envelope(source, frame_index, analysis, fps,
                                 t_seconds, key_time);
        case doc::ModSourceType::VideoCut:
            return analysis ? analysis->sample(analysis->cut, frame_index)
                            : 0.0f;
        case doc::ModSourceType::Drift:
            return eval_drift(source, t_seconds);
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

float eval_value_node(const ValueEnv& env, uint64_t node_id, int depth) {
    if (!env.look || depth >= 64) return 0.0f;
    const doc::ValueNode* n = doc::find_value_node(*env.look, node_id);
    if (!n) return 0.0f;
    auto input = [&](uint64_t id, float constant) {
        return id ? eval_value_node(env, id, depth + 1) : constant;
    };
    switch (n->source.type) {
        case doc::ModSourceType::Math: {
            const float a = input(n->in_a, n->const_a);
            const float b = input(n->in_b, n->const_b);
            switch (n->op) {
                case doc::ValueOp::Add: return a + b;
                case doc::ValueOp::Subtract: return a - b;
                case doc::ValueOp::Multiply: return a * b;
                case doc::ValueOp::Divide:
                    return std::fabs(b) < 1e-6f ? 0.0f : a / b;
                case doc::ValueOp::Min: return std::min(a, b);
                case doc::ValueOp::Max: return std::max(a, b);
                case doc::ValueOp::Floor: return std::floor(a);
                case doc::ValueOp::Absolute: return std::fabs(a);
                default: return a;
            }
        }
        case doc::ModSourceType::Normalise: {
            const float a = input(n->in_a, n->const_a);
            const float m = n->const_b;
            const float span = (n->in_max - n->in_min) * m;
            if (std::fabs(span) < 1e-6f) return 0.0f;
            return std::clamp((a - n->in_min * m) / span, 0.0f, 1.0f);
        }
        case doc::ModSourceType::AudioLow:
        case doc::ModSourceType::AudioMid:
        case doc::ModSourceType::AudioHigh:
        case doc::ModSourceType::AudioOnset:
        case doc::ModSourceType::Beat:
        case doc::ModSourceType::LfoBeat:
        case doc::ModSourceType::Envelope: {
            // An audio-driven node REQUIRES its input: the wire is the
            // only audio source. It taps its own connection - curves of
            // the wired chain's processed audio, media-frame indexed
            // (slip + Offset shims map the look clock in). Unwired or
            // unresolvable reads 0, never the global curves. Beat
            // clocks anchor on the MEDIA position, so cuts of one media
            // beat-match. Envelope's cut/keypress triggers are not
            // audio - they fall through to eval_source.
            if (n->source.type == doc::ModSourceType::Envelope &&
                n->source.trigger != 0 && n->source.trigger != 2)
                break;
            if (!n->audio_src || !env.node_audio) return 0.0f;
            const auto it = env.node_audio->find(n->id);
            if (it == env.node_audio->end() || !it->second.curves)
                return 0.0f;
            const AnalysisCurves& c = *it->second.curves;
            // The same audio-nudge shift eval_source used to apply.
            const double shifted = static_cast<double>(env.frame) -
                                   env.audio_off * env.fps;
            const uint32_t local =
                shifted <= 0.0 ? 0u
                               : static_cast<uint32_t>(shifted + 0.5);
            const int64_t pos = static_cast<int64_t>(local) +
                                static_cast<int64_t>(it->second.slip) +
                                it->second.offset;
            const uint32_t mf =
                pos < 0 ? 0u : static_cast<uint32_t>(pos);
            switch (n->source.type) {
                case doc::ModSourceType::AudioLow:
                    return c.sample(c.low, mf);
                case doc::ModSourceType::AudioMid:
                    return c.sample(c.mid, mf);
                case doc::ModSourceType::AudioHigh:
                    return c.sample(c.high, mf);
                case doc::ModSourceType::AudioOnset:
                    return c.sample(c.onset, mf);
                default: {
                    // Beat / LfoBeat / Envelope on the chain's clock.
                    const double tm =
                        env.fps > 0.0
                            ? static_cast<double>(mf) / env.fps
                            : 0.0;
                    if (n->source.type == doc::ModSourceType::LfoBeat)
                        return eval_lfo_beat(n->source, tm, &c);
                    if (n->source.type == doc::ModSourceType::Beat)
                        return eval_beat(n->source, tm, &c);
                    return eval_envelope(n->source, mf, &c, env.fps, tm,
                                         env.key_time);
                }
            }
        }
        default:
            break;
    }
    return eval_source(n->source, env.t, env.frame, env.analysis,
                       env.fps, env.audio_off, env.key_time,
                       env.video);
}

float apply_curve(doc::ResponseCurve curve, float x) {
    // Linear stays raw: helper chains legitimately leave [0,1] (a
    // centered LFO swings negative) and the param clamp is the bound.
    if (curve == doc::ResponseCurve::Linear) return x;
    x = std::clamp(x, 0.0f, 1.0f);
    switch (curve) {
        case doc::ResponseCurve::Exp: return x * x;
        case doc::ResponseCurve::SCurve: return smooth01(x);
        case doc::ResponseCurve::Inverted: return 1.0f - x;
        default: return x;
    }
}

float eval_lane(const doc::KeyframeLane& lane, double frame) {
    const auto& keys = lane.keys;
    if (keys.empty()) return 0.0f;
    // Loopable region: wrap through the key span once inside it.
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

void resolve_look(const doc::Look& look, doc::Look& out,
                  uint32_t local_frame, double fps,
                  const AnalysisCurves* analysis, double audio_off,
                  double live_seconds, double key_time,
                  const SourceFrameView* video,
                  const NodeAudioMap* node_audio) {
    const uint32_t frame_index = local_frame;
    const double t = live_seconds >= 0.0
                         ? live_seconds
                         : (fps > 0.0 ? frame_index / fps : 0.0);
    // Value nodes read the PRE-RESOLVE look: node params are not
    // themselves mod targets, so the copy-in-progress never feeds back.
    const ValueEnv env{&look, t,         frame_index, analysis,
                       fps,   audio_off, key_time,    video,
                       node_audio};

    auto param_slot = [](doc::EffectInstance& fx, int param_index) -> float* {
        if (param_index == doc::kWetParam) return &fx.wet;
        if (param_index == doc::kOpacityParam) return &fx.opacity;
        if (param_index >= 0 &&
            static_cast<size_t>(param_index) < fx.params.size())
            return &fx.params[static_cast<size_t>(param_index)];
        return nullptr;
    };

    // Morph: interpolate between two snapshot slots; the position
    // itself is a mod target (ParamKey {0, 0}) so routes are applied to it
    // first. Runs before lanes — morph sets base values like snapshots do.
    {
        float pos = look.morph_pos;
        for (const doc::ModRoute& route : look.mod_routes) {
            if (route.target.effect_id != 0 || route.target.param_index != 0)
                continue;
            if (!route.node) continue;
            // Wired = graph-driven: the wire replaces the slider.
            pos = apply_curve(route.curve, eval_value_node(env, route.node));
        }
        pos = std::clamp(pos, 0.0f, 1.0f);
        const bool from_ok = look.morph_from >= 0 && look.morph_from < 3 &&
                             look.snapshots[look.morph_from].valid;
        const bool to_ok = look.morph_to >= 0 && look.morph_to < 3 &&
                           look.snapshots[look.morph_to].valid;
        if (from_ok && to_ok && pos > 0.0f) {
            const doc::Snapshot& a = look.snapshots[look.morph_from];
            const doc::Snapshot& b = look.snapshots[look.morph_to];
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

    // Layer targets: keys carry kLayerParamBit + the layer id.
    auto layer_slot = [&](const doc::ParamKey& key, float* min_v,
                          float* max_v) -> float* {
        if (!(key.effect_id & doc::kLayerParamBit)) return nullptr;
        const uint64_t id = key.effect_id & ~doc::kLayerParamBit;
        for (doc::Layer& l : out.layers)
            if (l.id == id) {
                layer_param_range(key.param_index, min_v, max_v);
                return layer_param_slot(l, key.param_index);
            }
        return nullptr;
    };

    // Keyframe lanes set the base (muted lanes keep keys, drive nothing).
    for (const doc::KeyframeLane& lane : look.lanes) {
        if (lane.keys.empty() || lane.muted) continue;
        float min_v = 0.0f, max_v = 1.0f;
        if (float* lslot = layer_slot(lane.target, &min_v, &max_v)) {
            *lslot = std::clamp(eval_lane(lane, frame_index), min_v, max_v);
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

    // Wires REPLACE: a driven param maps the node's output onto its
    // range, ignoring base and lanes. Commands keep one wire per param;
    // in a hand-edited file the last one wins. (Group faces are DIRECT
    // param aliases — they have no resolve-time behavior of their own.)
    for (const doc::ModRoute& route : look.mod_routes) {
        if (!route.node) continue;
        float min_v = 0.0f, max_v = 1.0f;
        float* slot = layer_slot(route.target, &min_v, &max_v);
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
            apply_curve(route.curve, eval_value_node(env, route.node));
        *slot = std::clamp(min_v + (max_v - min_v) * value, min_v, max_v);
    }

    // Discrete params (selectors + flagged counts) snap to whole numbers
    // after every driver — lanes, routes, and morph all interpolate
    // fractionally, and fractional counts alias kernel math (a dither
    // `levels` of 2.2 cuts a hard band through the frame). Untouched
    // params already sit on integers, so this only affects driven ones.
    for (doc::Layer& snap_layer : out.layers)
        for (doc::EffectInstance& fx : snap_layer.stack)
            for (size_t p = 0; p < fx.params.size(); ++p)
                if (param_discrete(fx.type, static_cast<int>(p)))
                    fx.params[p] = std::round(fx.params[p]);
}

doc::Document resolve(const doc::Document& doc, uint32_t frame_index,
                      double fps, const AnalysisCurves* analysis,
                      double live_seconds, double key_time,
                      const SourceFrameView* video,
                      const NodeAudioMap* node_audio) {
    doc::Document out = doc;
    const double audio_off =
        static_cast<double>(doc.audio_offset_ms) * 0.001;
    // Every look resolves at `frame_index`. That IS its local frame for
    // the root and for the single-instance case; per-instance local times
    // arrive with instance paths, which is why the per-look primitive
    // above takes the frame explicitly.
    for (size_t i = 0; i < out.looks.size(); ++i)
        resolve_look(doc.looks[i], out.looks[i], frame_index, fps, analysis,
                     audio_off, live_seconds, key_time, video, node_audio);
    return out;
}

float speed_at(const doc::Document& doc, uint32_t frame_index, double fps,
               const AnalysisCurves* analysis, double live_seconds) {
    // Playback speed belongs to the root sequence, and sequences carry
    // no keyframes or routes - the project speed is the scalar. Ramps
    // live per-block (Placement::speed) or inside looks.
    (void)frame_index;
    (void)fps;
    (void)analysis;
    (void)live_seconds;
    return std::clamp(doc.speed, 0.0f, doc::kMaxSpeed);
}

bool time_remap_active(const doc::Document& doc) {
    return doc.time_mode != 0 || doc.speed != 1.0f;
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
        default: {  // forward: wrap around the media
            return static_cast<uint32_t>(std::floor(std::fmod(position, count)));
        }
    }
}

}  // namespace looks::mod

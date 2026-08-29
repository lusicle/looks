#include "media/audio_mix.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "media/pcm.h"
#include "media/sample_clock.h"

namespace looks::media {

namespace {

constexpr int kDelayTapMax = 12;  // echoes below ~-48 dB truncate anyway
// Leaf fetches per output sample per channel; it bounds chained wide ops.
constexpr int kFetchBudget = 4096;

// Returns a raw s16-scale sample at fractional frame pos; 0 outside.
float fetch_pcm(const PcmBuffer& pcm, int64_t pcm_frames, double pos,
                uint32_t k) {
    if (pos < 0.0 || pos >= static_cast<double>(pcm_frames)) return 0.0f;
    const int64_t i0 = static_cast<int64_t>(pos);
    const int64_t i1 = std::min(i0 + 1, pcm_frames - 1);
    const float frac = static_cast<float>(pos - static_cast<double>(i0));
    const uint32_t sc = std::min(k, pcm.channels - 1);
    const int16_t* base = pcm.samples.data();
    const float v0 = static_cast<float>(
        base[static_cast<size_t>(i0) * pcm.channels + sc]);
    const float v1 = static_cast<float>(
        base[static_cast<size_t>(i1) * pcm.channels + sc]);
    return v0 + (v1 - v0) * frac;
}

// Windowed-sinc Hann lowpass at unity DC; a highpass inverts the kernel.
void build_fir(const MixOp& op, float* w) {
    constexpr float kPi = 3.14159265358979f;
    const float fc = 0.002f + std::clamp(op.p[0], 0.0f, 1.0f) * 0.497f;
    const int half = kFirTaps / 2;
    float sum = 0.0f;
    for (int i = 0; i < kFirTaps; ++i) {
        const int n = i - half;
        const float sinc =
            n == 0 ? 2.0f * fc
                   : std::sin(2.0f * kPi * fc * static_cast<float>(n)) /
                         (kPi * static_cast<float>(n));
        const float hann =
            0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(i) /
                                   static_cast<float>(kFirTaps - 1));
        w[i] = sinc * hann;
        sum += w[i];
    }
    for (int i = 0; i < kFirTaps; ++i) w[i] /= sum;
    if (op.p[1] >= 0.5f) {
        for (int i = 0; i < kFirTaps; ++i) w[i] = -w[i];
        w[half] += 1.0f;
    }
}

float eval_node(const MixState& mix, int idx, double pos, uint32_t k,
                int& budget);

float eval_inputs(const MixState& mix, const MixNode& n, double pos,
                  uint32_t k, int& budget) {
    float v = 0.0f;
    for (int i : n.inputs) v += eval_node(mix, i, pos, k, budget);
    return v;
}

// Returns the program value at fractional output sample pos, in s16 units.
// Every op must stay a pure function of pos; budget counts leaf fetches.
float eval_node(const MixState& mix, int idx, double pos, uint32_t k,
                int& budget) {
    const MixNode& n = mix.nodes[static_cast<size_t>(idx)];
    float level = n.gain;
    if (n.windowed) {
        if (pos < n.s0 || pos >= n.s1) return 0.0f;
        if (n.ramp > 0) {
            const double edge =
                std::min(pos - n.s0, n.s1 - 1.0 - pos);
            if (edge < static_cast<double>(n.ramp))
                level *= static_cast<float>(
                    (edge + 1.0) / static_cast<double>(n.ramp + 1));
        }
    }
    if (n.pcm) {
        if (budget <= 0) return 0.0f;
        --budget;
        const int64_t frames = static_cast<int64_t>(n.pcm->frames());
        return fetch_pcm(*n.pcm, frames, n.A * pos + n.B, k) * level;
    }
    float v = eval_inputs(mix, n, pos, k, budget);
    if (!n.has_op) return v * level;
    const MixOp& o = n.op;
    const float dry = v;
    float fx = dry;
    // Taps shift in node-local samples; La converts them to output positions.
    // A frozen clock (La <= 0) has no time axis, so time ops act pointwise.
    const bool ticking = n.La > 1e-12;
    switch (o.kind) {
        case MixOpKind::Gain:
            fx = dry * o.p[0];
            break;
        case MixOpKind::Bitcrush: {
            const float bits = std::clamp(o.p[0], 1.0f, 16.0f);
            const float step = 65536.0f / std::exp2(bits);
            fx = std::floor(dry / step + 0.5f) * step;
            break;
        }
        case MixOpKind::Downsample: {
            if (!ticking) break;
            const double hold =
                static_cast<double>(std::max(1.0f, o.p[0]));
            const double local = n.La * pos + n.Lb;
            const double held = std::floor(local / hold) * hold;
            fx = eval_inputs(mix, n, (held - n.Lb) / n.La, k, budget);
            break;
        }
        case MixOpKind::Distortion: {
            const float g =
                1.0f + std::clamp(o.p[0], 0.0f, 1.0f) * 24.0f;
            fx = std::tanh(dry / 32768.0f * g) / std::tanh(g) * 32768.0f;
            break;
        }
        case MixOpKind::Delay: {
            if (!ticking) break;
            const double d = std::max(
                1.0, static_cast<double>(o.p[0]) / 1000.0 *
                         static_cast<double>(mix.rate));
            const float fb = std::clamp(o.p[1], 0.0f, 0.95f);
            float g = fb;
            for (int t = 1; t <= kDelayTapMax && g > (1.0f / 256.0f);
                 ++t, g *= fb)
                fx += g * eval_inputs(
                             mix, n,
                             pos - d * static_cast<double>(t) / n.La, k,
                             budget);
            break;
        }
        case MixOpKind::Filter: {
            if (!ticking) break;
            const int half = kFirTaps / 2;
            float acc = 0.0f;
            for (int i = 0; i < kFirTaps; ++i)
                acc += n.fir[i] *
                       eval_inputs(mix, n,
                                   pos + static_cast<double>(i - half) /
                                             n.La,
                                   k, budget);
            fx = acc;
            break;
        }
    }
    return (dry + (fx - dry) * o.wet) * level;
}

}  // namespace

std::shared_ptr<const PcmBuffer> load_pcm(const std::filesystem::path& path,
                                          std::string* error) {
    if (path.empty()) return nullptr;
    PcmReader reader;
    if (!reader.open(path, error)) return nullptr;
    auto buffer = std::make_shared<PcmBuffer>();
    buffer->channels = reader.channels();
    buffer->rate = reader.sample_rate();
    if (!buffer->channels || !buffer->rate) return nullptr;
    const uint64_t frames = reader.frame_count();
    buffer->samples.resize(static_cast<size_t>(frames) * buffer->channels);
    reader.read(0, buffer->samples.data(), static_cast<size_t>(frames));
    return buffer;
}

void prepare_mix(MixState& mix) {
    const double fps = mix.fps > 0.0 ? mix.fps : 30.0;
    const double rate = static_cast<double>(mix.rate ? mix.rate : 48000);
    for (MixNode& n : mix.nodes) {
        const double lf = n.local_fps > 0.0 ? n.local_fps : fps;
        // Node-local sample position at the mix rate: local = La * s + Lb.
        n.La = n.a * fps / lf;
        n.Lb = n.b * rate / lf;
        if (n.pcm && n.pcm->rate) {
            const double pr = static_cast<double>(n.pcm->rate);
            n.A = n.La * pr / rate;
            n.B = n.Lb * pr / rate;
        }
        if (n.windowed) {
            n.s0 = static_cast<double>(
                frame_to_sample(std::max(n.w0, 0.0), fps, rate));
            n.s1 = n.w1 >= kMixUnbounded * 0.5
                       ? kMixUnbounded
                       : static_cast<double>(
                             frame_to_sample(n.w1, fps, rate));
            int64_t r = kRampSamples;
            if (n.s1 < kMixUnbounded)
                r = std::min<int64_t>(
                    r, static_cast<int64_t>((n.s1 - n.s0) / 2.0));
            n.ramp = std::max<int64_t>(r, 0);
        }
        if (n.has_op && n.op.kind == MixOpKind::Filter)
            build_fir(n.op, n.fir);
    }
}

void render_mix(const MixState& mix, int64_t start, int16_t* out,
                uint32_t count, std::vector<float>& scratch) {
    const uint32_t ch = mix.channels ? mix.channels : 2;
    std::memset(out, 0, static_cast<size_t>(count) * ch * sizeof(int16_t));
    if (!count || mix.rate == 0 || mix.fps <= 0.0) return;
    if (mix.root < 0 ||
        static_cast<size_t>(mix.root) >= mix.nodes.size())
        return;

    // Accumulate in float; the sum must clip once at the end, not per node.
    const size_t total = static_cast<size_t>(count) * ch;
    if (scratch.size() < total) scratch.resize(total);
    std::vector<float>& acc = scratch;
    std::fill(acc.begin(), acc.begin() + static_cast<ptrdiff_t>(total), 0.0f);

    const MixNode& root = mix.nodes[static_cast<size_t>(mix.root)];
    int64_t lo = start;
    int64_t hi = start + static_cast<int64_t>(count);
    if (root.windowed) {
        lo = std::max<int64_t>(lo, static_cast<int64_t>(root.s0));
        if (root.s1 < kMixUnbounded)
            hi = std::min<int64_t>(hi, static_cast<int64_t>(root.s1));
    }
    for (int64_t s = lo; s < hi; ++s) {
        float* dst = acc.data() + static_cast<size_t>(s - start) * ch;
        for (uint32_t k = 0; k < ch; ++k) {
            int budget = kFetchBudget;
            dst[k] += eval_node(mix, mix.root,
                                static_cast<double>(s), k, budget);
        }
    }

    for (size_t i = 0; i < total; ++i)
        out[i] = static_cast<int16_t>(
            std::clamp(acc[i], -32768.0f, 32767.0f));
}

namespace {

// op < 0 reads the buffer itself; this chain keeps the same purity rule.
float eval_chain(const PcmBuffer& pcm, int64_t frames,
                 const std::vector<MixOp>& ops,
                 const std::vector<std::array<float, kFirTaps>>& firs,
                 int op, double pos, uint32_t k, int& budget) {
    if (op < 0) {
        if (budget <= 0) return 0.0f;
        --budget;
        return fetch_pcm(pcm, frames, pos, k);
    }
    const MixOp& o = ops[static_cast<size_t>(op)];
    const float dry =
        eval_chain(pcm, frames, ops, firs, op - 1, pos, k, budget);
    float fx = dry;
    switch (o.kind) {
        case MixOpKind::Gain:
            fx = dry * o.p[0];
            break;
        case MixOpKind::Bitcrush: {
            const float bits = std::clamp(o.p[0], 1.0f, 16.0f);
            const float step = 65536.0f / std::exp2(bits);
            fx = std::floor(dry / step + 0.5f) * step;
            break;
        }
        case MixOpKind::Downsample: {
            const double hold =
                static_cast<double>(std::max(1.0f, o.p[0]));
            const double held = std::floor(pos / hold) * hold;
            fx = eval_chain(pcm, frames, ops, firs, op - 1, held, k,
                            budget);
            break;
        }
        case MixOpKind::Distortion: {
            const float g =
                1.0f + std::clamp(o.p[0], 0.0f, 1.0f) * 24.0f;
            fx = std::tanh(dry / 32768.0f * g) / std::tanh(g) * 32768.0f;
            break;
        }
        case MixOpKind::Delay: {
            const double d = std::max(
                1.0, static_cast<double>(o.p[0]) / 1000.0 *
                         static_cast<double>(pcm.rate));
            const float fb = std::clamp(o.p[1], 0.0f, 0.95f);
            float g = fb;
            for (int t = 1; t <= kDelayTapMax && g > (1.0f / 256.0f);
                 ++t, g *= fb)
                fx += g * eval_chain(pcm, frames, ops, firs, op - 1,
                                     pos - d * static_cast<double>(t), k,
                                     budget);
            break;
        }
        case MixOpKind::Filter: {
            const float* w = firs[static_cast<size_t>(op)].data();
            const int half = kFirTaps / 2;
            float acc = 0.0f;
            for (int i = 0; i < kFirTaps; ++i)
                acc += w[i] *
                       eval_chain(pcm, frames, ops, firs, op - 1,
                                  pos + static_cast<double>(i - half), k,
                                  budget);
            fx = acc;
            break;
        }
    }
    return dry + (fx - dry) * o.wet;
}

}  // namespace

void build_wave_pyramid(const MixState& mix, int64_t s0, int64_t s1,
                        uint32_t base, WavePyramid* out,
                        const std::atomic<bool>* cancel) {
    out->levels.clear();
    out->start = s0;
    out->base = base ? base : 64;
    out->rate = mix.rate;
    if (s1 <= s0 || mix.root < 0 || !mix.rate) return;
    const uint32_t ch = mix.channels ? mix.channels : 2;
    const uint64_t total = static_cast<uint64_t>(s1 - s0);
    std::vector<WaveSpan> l0(
        static_cast<size_t>((total + out->base - 1) / out->base),
        WaveSpan{3.4e38f, -3.4e38f});
    constexpr uint32_t kChunk = 65536;
    std::vector<int16_t> buf(static_cast<size_t>(kChunk) * ch);
    std::vector<float> scratch;
    for (uint64_t p = 0; p < total; p += kChunk) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            out->levels.clear();
            return;
        }
        const uint32_t n =
            static_cast<uint32_t>(std::min<uint64_t>(kChunk, total - p));
        render_mix(mix, s0 + static_cast<int64_t>(p), buf.data(), n,
                   scratch);
        for (uint32_t i = 0; i < n; ++i) {
            float v = 0.0f;
            for (uint32_t k = 0; k < ch; ++k)
                v += static_cast<float>(buf[static_cast<size_t>(i) * ch + k]);
            v /= static_cast<float>(ch);
            WaveSpan& w = l0[static_cast<size_t>((p + i) / out->base)];
            w.lo = std::min(w.lo, v);
            w.hi = std::max(w.hi, v);
        }
    }
    out->levels.push_back(std::move(l0));
    while (out->levels.back().size() > 4) {
        const std::vector<WaveSpan>& prev = out->levels.back();
        std::vector<WaveSpan> next((prev.size() + 3) / 4,
                                   WaveSpan{3.4e38f, -3.4e38f});
        for (size_t i = 0; i < prev.size(); ++i) {
            WaveSpan& w = next[i / 4];
            w.lo = std::min(w.lo, prev[i].lo);
            w.hi = std::max(w.hi, prev[i].hi);
        }
        out->levels.push_back(std::move(next));
    }
}

void render_node_envelopes(const MixState& mix, int node, int64_t s0,
                           int64_t s1, uint32_t columns,
                           std::vector<WaveSpan>* in_env,
                           std::vector<WaveSpan>* out_env) {
    in_env->assign(columns, WaveSpan{});
    out_env->assign(columns, WaveSpan{});
    if (node < 0 || static_cast<size_t>(node) >= mix.nodes.size() ||
        s1 <= s0 || !columns)
        return;
    const MixNode& n = mix.nodes[static_cast<size_t>(node)];
    const uint32_t ch = mix.channels ? mix.channels : 2;
    const double span = static_cast<double>(s1 - s0);
    for (uint32_t c = 0; c < columns; ++c) {
        const int64_t c0 =
            s0 + static_cast<int64_t>(span * c / columns);
        int64_t c1 = s0 + static_cast<int64_t>(span * (c + 1) / columns);
        if (c1 <= c0) c1 = c0 + 1;
        // Probe a deterministic stride so every rebuild draws the same
        // envelope.
        const int64_t stride = std::max<int64_t>(
            1, (c1 - c0) / static_cast<int64_t>(kMaxColumnProbes));
        WaveSpan wi{3.4e38f, -3.4e38f};
        WaveSpan wo{3.4e38f, -3.4e38f};
        for (int64_t s = c0; s < c1; s += stride) {
            float vi = 0.0f, vo = 0.0f;
            for (uint32_t k = 0; k < ch; ++k) {
                int bi = kFetchBudget;
                int bo = kFetchBudget;
                vi += eval_inputs(mix, n, static_cast<double>(s), k, bi);
                vo += eval_node(mix, node, static_cast<double>(s), k, bo);
            }
            vi /= static_cast<float>(ch);
            vo /= static_cast<float>(ch);
            wi.lo = std::min(wi.lo, vi);
            wi.hi = std::max(wi.hi, vi);
            wo.lo = std::min(wo.lo, vo);
            wo.hi = std::max(wo.hi, vo);
        }
        (*in_env)[c] = wi;
        (*out_env)[c] = wo;
    }
}

void render_processed_pcm(const PcmBuffer& src,
                          const std::vector<MixOp>& ops, PcmBuffer* out) {
    out->channels = src.channels;
    out->rate = src.rate;
    if (ops.empty()) {
        out->samples = src.samples;
        return;
    }
    const int64_t frames = static_cast<int64_t>(src.frames());
    out->samples.assign(src.samples.size(), 0);
    if (frames <= 0 || src.channels == 0) return;
    std::vector<std::array<float, kFirTaps>> firs(ops.size());
    for (size_t i = 0; i < ops.size(); ++i)
        if (ops[i].kind == MixOpKind::Filter)
            build_fir(ops[i], firs[i].data());
    const int top = static_cast<int>(ops.size()) - 1;
    for (int64_t f = 0; f < frames; ++f)
        for (uint32_t k = 0; k < src.channels; ++k) {
            int budget = kFetchBudget;
            const float v = eval_chain(src, frames, ops, firs, top,
                                       static_cast<double>(f), k, budget);
            out->samples[static_cast<size_t>(f) * src.channels + k] =
                static_cast<int16_t>(
                    std::clamp(v, -32768.0f, 32767.0f));
        }
}

}  // namespace looks::media

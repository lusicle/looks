#include "media/audio_mix.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "media/pcm.h"
#include "media/sample_clock.h"

namespace looks::media {

namespace {

constexpr int kFirTaps = 63;      // odd: symmetric kernel, exact center
constexpr int kDelayTapMax = 12;  // echoes below ~-48 dB truncate anyway
// Base fetches per output sample per channel: bounds chained wide ops
// (filter-into-filter) deterministically instead of letting a
// pathological chain stall the audio callback.
constexpr int kFetchBudget = 4096;

// Raw s16-scale sample of the source PCM at fractional frame `pos`,
// channel k; 0 outside - a gap sounds like a gap at every tap depth.
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

// Per-source render context; FIR kernels build once per call (filters
// are the only op with derived coefficients), on the stack - the
// monitor callback stays allocation-free.
struct VoiceCtx {
    const MixSource* src = nullptr;
    const PcmBuffer* pcm = nullptr;
    int64_t pcm_frames = 0;
    float fir[kMaxMixOps][kFirTaps];
};

// Windowed-sinc (Hann) lowpass, unity DC; highpass by spectral
// inversion of the same kernel.
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

// The op chain's value at fractional source frame `pos`, channel k, in
// raw s16 units; op < 0 is the PCM itself. PURE: every op is a function
// of absolute position - delay taps, sample holds and FIR windows
// RE-READ upstream at shifted positions instead of carrying state - so
// any chunking mixes the same bytes and preview equals export. `budget`
// counts base fetches; exhaustion silences the deepest taps
// deterministically.
float eval_voice(const VoiceCtx& ctx, int op, double pos, uint32_t k,
                 int& budget) {
    if (op < 0) {
        if (budget <= 0) return 0.0f;
        --budget;
        return fetch_pcm(*ctx.pcm, ctx.pcm_frames, pos, k);
    }
    const MixOp& o = ctx.src->ops[static_cast<size_t>(op)];
    const float dry = eval_voice(ctx, op - 1, pos, k, budget);
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
            fx = eval_voice(ctx, op - 1, held, k, budget);
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
                         static_cast<double>(ctx.pcm->rate));
            const float fb = std::clamp(o.p[1], 0.0f, 0.95f);
            float g = fb;
            for (int t = 1; t <= kDelayTapMax && g > (1.0f / 256.0f);
                 ++t, g *= fb)
                fx += g * eval_voice(ctx, op - 1,
                                     pos - d * static_cast<double>(t),
                                     k, budget);
            break;
        }
        case MixOpKind::Filter: {
            const float* w = ctx.fir[op];
            const int half = kFirTaps / 2;
            float acc = 0.0f;
            for (int i = 0; i < kFirTaps; ++i)
                acc += w[i] *
                       eval_voice(ctx, op - 1,
                                  pos + static_cast<double>(i - half), k,
                                  budget);
            fx = acc;
            break;
        }
    }
    return dry + (fx - dry) * o.wet;
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

void render_mix(const MixState& mix, int64_t start, int16_t* out,
                uint32_t count, std::vector<float>& scratch) {
    const uint32_t ch = mix.channels ? mix.channels : 2;
    std::memset(out, 0, static_cast<size_t>(count) * ch * sizeof(int16_t));
    if (!count || mix.rate == 0 || mix.fps <= 0.0) return;

    // Accumulate in float: several sources summing into s16 must clip once,
    // at the end, not per source.
    const size_t total = static_cast<size_t>(count) * ch;
    if (scratch.size() < total) scratch.resize(total);
    std::vector<float>& acc = scratch;
    std::fill(acc.begin(), acc.begin() + static_cast<ptrdiff_t>(total), 0.0f);
    const double rate = static_cast<double>(mix.rate);

    for (const MixSource& src : mix.sources) {
        if (!src.pcm || src.gain <= 0.0f || src.speed <= 0.0) continue;
        const PcmBuffer& pcm = *src.pcm;
        const int64_t pcm_frames = static_cast<int64_t>(pcm.frames());
        if (pcm_frames <= 0 || pcm.channels == 0 || pcm.rate == 0) continue;

        // The placement's span in OUTPUT samples, on the same rounding
        // rule as the transport cursor (sample_clock.h) so a block's
        // first sample is exactly the cursor of its first frame.
        const int64_t s0 = frame_to_sample(src.t_in, mix.fps, rate);
        const int64_t s1 = frame_to_sample(src.t_out, mix.fps, rate);
        const int64_t lo = std::max(s0, start);
        const int64_t hi =
            std::min(s1, start + static_cast<int64_t>(count));
        if (hi <= lo) continue;

        // Output sample -> source sample is affine, because placement is:
        // src_sample = a * s + b.
        const double src_rate = static_cast<double>(pcm.rate);
        const double a = src.speed * src_rate / rate;
        const double b =
            (src.source_in - src.t_in * src.speed) * src_rate / mix.fps;
        // Ramps never exceed half the span, so a very short block fades in
        // and straight out instead of stepping.
        const int64_t ramp = std::min<int64_t>(kRampSamples, (s1 - s0) / 2);

        // The voice's DSP chain. Without ops the loop keeps the plain
        // fetch (and its outside-the-media skip); with ops every sample
        // in the window evaluates - delay tails ring past the media end
        // until the placement window cuts them.
        const int top =
            static_cast<int>(std::min(src.ops.size(), kMaxMixOps)) - 1;
        VoiceCtx ctx;
        if (top >= 0) {
            ctx.src = &src;
            ctx.pcm = &pcm;
            ctx.pcm_frames = pcm_frames;
            for (int i = 0; i <= top; ++i)
                if (src.ops[static_cast<size_t>(i)].kind ==
                    MixOpKind::Filter)
                    build_fir(src.ops[static_cast<size_t>(i)], ctx.fir[i]);
        }

        for (int64_t s = lo; s < hi; ++s) {
            const double pos = a * static_cast<double>(s) + b;
            if (top < 0 &&
                (pos < 0.0 || pos >= static_cast<double>(pcm_frames)))
                continue;
            float level = src.gain;
            if (ramp > 0) {
                const int64_t from_in = s - s0;
                const int64_t to_out = s1 - 1 - s;
                const int64_t edge = std::min(from_in, to_out);
                if (edge < ramp)
                    level *= static_cast<float>(edge + 1) /
                             static_cast<float>(ramp + 1);
            }
            float* dst = acc.data() + static_cast<size_t>(s - start) * ch;
            for (uint32_t k = 0; k < ch; ++k) {
                float v;
                if (top >= 0) {
                    int budget = kFetchBudget;
                    v = eval_voice(ctx, top, pos, k, budget);
                } else {
                    v = fetch_pcm(pcm, pcm_frames, pos, k);
                }
                dst[k] += v * level;
            }
        }
    }

    for (size_t i = 0; i < total; ++i)
        out[i] = static_cast<int16_t>(
            std::clamp(acc[i], -32768.0f, 32767.0f));
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
    MixSource holder;   // op carrier only; the PCM rides the context
    holder.ops = ops;
    VoiceCtx ctx;
    ctx.src = &holder;
    ctx.pcm = &src;
    ctx.pcm_frames = frames;
    const int top =
        static_cast<int>(std::min(ops.size(), kMaxMixOps)) - 1;
    for (int i = 0; i <= top; ++i)
        if (ops[static_cast<size_t>(i)].kind == MixOpKind::Filter)
            build_fir(ops[static_cast<size_t>(i)], ctx.fir[i]);
    for (int64_t f = 0; f < frames; ++f)
        for (uint32_t k = 0; k < src.channels; ++k) {
            int budget = kFetchBudget;
            const float v = eval_voice(ctx, top,
                                       static_cast<double>(f), k, budget);
            out->samples[static_cast<size_t>(f) * src.channels + k] =
                static_cast<int16_t>(
                    std::clamp(v, -32768.0f, 32767.0f));
        }
}

}  // namespace looks::media

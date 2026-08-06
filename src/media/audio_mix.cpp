#include "media/audio_mix.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "media/pcm.h"

namespace looks::media {

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

        // The placement's span in OUTPUT samples.
        const double per_frame = rate / mix.fps;
        const int64_t s0 =
            static_cast<int64_t>(std::ceil(src.t_in * per_frame));
        const int64_t s1 =
            static_cast<int64_t>(std::ceil(src.t_out * per_frame));
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

        for (int64_t s = lo; s < hi; ++s) {
            const double pos = a * static_cast<double>(s) + b;
            if (pos < 0.0 || pos >= static_cast<double>(pcm_frames)) continue;
            const int64_t i0 = static_cast<int64_t>(pos);
            const int64_t i1 = std::min(i0 + 1, pcm_frames - 1);
            const float frac = static_cast<float>(pos - static_cast<double>(i0));
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
                const uint32_t sc = std::min(k, pcm.channels - 1);
                const int16_t* base = pcm.samples.data();
                const float v0 = static_cast<float>(
                    base[static_cast<size_t>(i0) * pcm.channels + sc]);
                const float v1 = static_cast<float>(
                    base[static_cast<size_t>(i1) * pcm.channels + sc]);
                dst[k] += (v0 + (v1 - v0) * frac) * level;
            }
        }
    }

    for (size_t i = 0; i < total; ++i)
        out[i] = static_cast<int16_t>(
            std::clamp(acc[i], -32768.0f, 32767.0f));
}

}  // namespace looks::media

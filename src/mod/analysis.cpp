#include "mod/analysis.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "mod/fft.h"
#include "util/bytes.h"
#include "util/file.h"

namespace looks::mod {

namespace {

constexpr uint32_t kFftSize = 1024;

float percentile95(const std::vector<float>& curve) {
    if (curve.empty()) return 0.0f;
    std::vector<float> sorted = curve;
    std::sort(sorted.begin(), sorted.end());
    return sorted[static_cast<size_t>(
        static_cast<double>(sorted.size() - 1) * 0.95)];
}

void scale_curve(std::vector<float>& curve, float scale) {
    if (scale <= 1.0e-9f) {
        std::fill(curve.begin(), curve.end(), 0.0f);
        return;
    }
    for (float& v : curve) v = std::min(1.0f, v / scale);
}

void normalize_curve(std::vector<float>& curve) {
    scale_curve(curve, percentile95(curve));
}

void smooth_curve(std::vector<float>& curve, double fps, float attack_s,
                  float release_s) {
    if (curve.empty() || fps <= 0.0) return;
    const float dt = static_cast<float>(1.0 / fps);
    const float a = 1.0f - std::exp(-dt / std::max(attack_s, 1.0e-4f));
    const float r = 1.0f - std::exp(-dt / std::max(release_s, 1.0e-4f));
    float state = curve[0];
    for (float& v : curve) {
        state += (v - state) * (v > state ? a : r);
        v = state;
    }
}

float band_energy(const std::vector<float>& mags, uint32_t sample_rate,
                  float lo_hz, float hi_hz) {
    const float hz_per_bin =
        static_cast<float>(sample_rate) / static_cast<float>(kFftSize);
    const size_t lo = static_cast<size_t>(lo_hz / hz_per_bin);
    const size_t hi = std::min(mags.size() - 1,
                               static_cast<size_t>(hi_hz / hz_per_bin));
    if (hi <= lo) return 0.0f;
    float sum = 0.0f;
    for (size_t i = lo; i <= hi; ++i) sum += mags[i] * mags[i];
    return std::sqrt(sum / static_cast<float>(hi - lo + 1));
}

constexpr double kTempoRate = 100.0;
constexpr uint32_t kTempoFft = 512;
constexpr double kTempoBpmMin = 60.0;
constexpr double kTempoBpmMax = 200.0;
constexpr uint32_t kTempoHarm = 8;
constexpr double kBeatTightness = 100.0;

struct OnsetEnvelope {
    std::vector<float> det;
    double rate = 0.0;
    double latency = 0.0;
};

bool build_onset_envelope(const int16_t* samples, uint64_t frame_total,
                          uint32_t channels, uint32_t sample_rate,
                          const std::atomic<bool>* cancel,
                          OnsetEnvelope* out) {
    if (!samples || frame_total == 0 || channels == 0 || sample_rate == 0)
        return false;
    const uint32_t dec = std::max(1u, sample_rate / 11025u);
    const double ds_rate = static_cast<double>(sample_rate) / dec;
    const uint64_t ds_count = frame_total / dec;
    if (ds_count <= kTempoFft) return false;

    std::vector<float> mono(static_cast<size_t>(ds_count));
    const float norm = 32768.0f * static_cast<float>(dec) *
                       static_cast<float>(channels);
    for (uint64_t j = 0; j < ds_count; ++j) {
        float acc = 0.0f;
        const uint64_t base = j * dec;
        for (uint32_t d = 0; d < dec; ++d) {
            const uint64_t s = base + d;
            if (s >= frame_total) break;
            for (uint32_t c = 0; c < channels; ++c)
                acc += static_cast<float>(samples[s * channels + c]);
        }
        mono[static_cast<size_t>(j)] = acc / norm;
    }

    const uint32_t hop =
        std::max(1u, static_cast<uint32_t>(ds_rate / kTempoRate + 0.5));
    out->rate = ds_rate / hop;
    out->latency = static_cast<double>(kTempoFft) * 0.5 / ds_rate;
    const size_t env_n = static_cast<size_t>((ds_count - kTempoFft) / hop);
    if (env_n < 32) return false;

    std::vector<float> win(kTempoFft);
    for (uint32_t i = 0; i < kTempoFft; ++i)
        win[i] = 0.5f - 0.5f * std::cos(6.2831853071795864f *
                                        static_cast<float>(i) /
                                        static_cast<float>(kTempoFft - 1));

    std::vector<float> flux(env_n, 0.0f);
    std::vector<float> prev, buf(kTempoFft);
    for (size_t k = 0; k < env_n; ++k) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return false;
        const uint64_t base = static_cast<uint64_t>(k) * hop;
        for (uint32_t i = 0; i < kTempoFft; ++i)
            buf[i] = mono[static_cast<size_t>(base + i)] * win[i];
        std::vector<float> mags =
            magnitude_spectrum(buf.data(), buf.size(), kTempoFft);
        if (!prev.empty()) {
            float sum = 0.0f;
            for (size_t i = 0; i < mags.size(); ++i)
                sum += std::max(0.0f, mags[i] - prev[i]);
            flux[k] = sum;
        }
        prev = std::move(mags);
    }

    const size_t mwin =
        std::max<size_t>(3, static_cast<size_t>(out->rate * 0.5));
    out->det.assign(env_n, 0.0f);
    double run = 0.0;
    for (size_t i = 0; i < env_n; ++i) {
        run += flux[i];
        if (i >= mwin) run -= flux[i - mwin];
        const double mean = run / static_cast<double>(std::min(i + 1, mwin));
        out->det[i] = std::max(0.0f, flux[i] - static_cast<float>(mean));
    }
    double sq = 0.0;
    for (float v : out->det) sq += static_cast<double>(v) * v;
    const double rms = std::sqrt(sq / static_cast<double>(env_n));
    if (rms <= 0.0) return false;
    for (float& v : out->det) v = static_cast<float>(v / rms);
    return true;
}

double estimate_period(const OnsetEnvelope& env) {
    const std::vector<float>& det = env.det;
    const size_t env_n = det.size();
    const size_t lag_min = std::max<size_t>(
        2, static_cast<size_t>(env.rate * 60.0 / kTempoBpmMax));
    const size_t lag_max = std::min<size_t>(
        env_n / 2, static_cast<size_t>(env.rate * 60.0 / kTempoBpmMin));
    if (lag_max <= lag_min + 2) return 0.0;

    const size_t r_max =
        std::min<size_t>(env_n / 2, lag_max * kTempoHarm + 2);
    std::vector<double> r(r_max + 2, 0.0);
    for (size_t lag = lag_min - 1; lag <= r_max; ++lag) {
        double acc = 0.0;
        for (size_t i = lag; i < env_n; ++i)
            acc += static_cast<double>(det[i]) * det[i - lag];
        r[lag] = acc / static_cast<double>(env_n - lag);
    }

    double best = 0.0;
    for (size_t lag = lag_min; lag <= lag_max; ++lag)
        best = std::max(best, r[lag]);
    if (best <= 0.0) return 0.0;

    size_t pick = 0;
    for (size_t lag = lag_min; lag <= lag_max; ++lag) {
        if (r[lag] < best * 0.85) continue;
        if (r[lag] < r[lag - 1] || r[lag] < r[lag + 1]) continue;
        pick = lag;
        break;
    }
    if (pick == 0) return 0.0;

    auto r_at = [&](double lag) {
        if (lag < 2.0 || lag + 2.0 >= static_cast<double>(r.size()))
            return 0.0;
        const double mid = std::floor(lag + 0.5);
        const size_t i = static_cast<size_t>(mid);
        const double d = lag - mid;
        const double y0 = r[i - 1];
        const double y1 = r[i];
        const double y2 = r[i + 1];
        return y1 + 0.5 * d * (y2 - y0) +
               0.5 * d * d * (y2 - 2.0 * y1 + y0);
    };

    double refined = static_cast<double>(pick);
    double span = 1.0;
    for (int pass = 0; pass < 5; ++pass) {
        const double step = span / 32.0;
        double top = refined;
        double top_score = -1.0;
        for (int s = -32; s <= 32; ++s) {
            const double cand = refined + step * s;
            if (cand < 1.0) continue;
            double sum = 0.0;
            for (uint32_t k = 1; k <= kTempoHarm; ++k) {
                const double h = cand * k;
                if (h + 2.0 >= static_cast<double>(r.size())) break;
                sum += r_at(h);
            }
            if (sum > top_score) {
                top_score = sum;
                top = cand;
            }
        }
        refined = top;
        span = step * 2.0;
    }
    return refined > 0.0 ? refined : 0.0;
}

void track_beats(const OnsetEnvelope& env, double period,
                 std::vector<float>* beats) {
    beats->clear();
    const std::vector<float>& det = env.det;
    const size_t n = det.size();
    if (period < 2.0 || n < 8 || env.rate <= 0.0) return;
    const size_t lo_step =
        std::max<size_t>(1, static_cast<size_t>(period * 0.5));
    const size_t hi_step = static_cast<size_t>(period * 2.0);
    if (hi_step <= lo_step) return;

    std::vector<double> score(n, 0.0);
    std::vector<int64_t> back(n, -1);
    for (size_t i = 0; i < n; ++i) {
        double best = 0.0;
        int64_t bj = -1;
        const size_t j0 = i > hi_step ? i - hi_step : 0;
        const size_t j1 = i > lo_step ? i - lo_step : 0;
        for (size_t j = j0; j < j1; ++j) {
            const double ratio = static_cast<double>(i - j) / period;
            const double lg = std::log(ratio);
            const double s = score[j] - kBeatTightness * lg * lg;
            if (bj < 0 || s > best) {
                best = s;
                bj = static_cast<int64_t>(j);
            }
        }
        score[i] = det[i] + (bj >= 0 ? best : 0.0);
        back[i] = bj;
    }

    size_t tail = n - 1;
    double top = score[tail];
    const size_t from = n > hi_step ? n - hi_step : 0;
    for (size_t i = from; i < n; ++i)
        if (score[i] > top) {
            top = score[i];
            tail = i;
        }

    std::vector<size_t> rev;
    int64_t at = static_cast<int64_t>(tail);
    while (at >= 0) {
        rev.push_back(static_cast<size_t>(at));
        at = back[static_cast<size_t>(at)];
    }
    beats->reserve(rev.size());
    for (size_t i = rev.size(); i-- > 0;)
        beats->push_back(static_cast<float>(
            static_cast<double>(rev[i]) / env.rate + env.latency));
}
}  // namespace

void analyze_audio(const int16_t* samples, uint64_t frame_total,
                   uint32_t channels, uint32_t sample_rate, double video_fps,
                   uint32_t video_frames, AnalysisData* out,
                   const std::atomic<bool>* cancel) {
    out->low.assign(video_frames, 0.0f);
    out->mid.assign(video_frames, 0.0f);
    out->high.assign(video_frames, 0.0f);
    out->onset.assign(video_frames, 0.0f);
    out->beats.clear();
    out->bpm = 0.0f;
    if (!samples || frame_total == 0 || channels == 0 || video_fps <= 0.0)
        return;

    std::vector<float> window(kFftSize);
    std::vector<float> prev_mags;
    std::vector<float> flux(video_frames, 0.0f);

    for (uint32_t f = 0; f < video_frames; ++f) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return;
        const uint64_t start = static_cast<uint64_t>(
            static_cast<double>(f) / video_fps * sample_rate);
        for (uint32_t i = 0; i < kFftSize; ++i) {
            const uint64_t s = start + i;
            float mix = 0.0f;
            if (s < frame_total) {
                for (uint32_t c = 0; c < channels; ++c)
                    mix += static_cast<float>(samples[s * channels + c]);
                mix /= 32768.0f * static_cast<float>(channels);
            }
            window[i] = mix;
        }
        std::vector<float> mags =
            magnitude_spectrum(window.data(), window.size(), kFftSize);

        out->low[f] = band_energy(mags, sample_rate, 20.0f, 250.0f);
        out->mid[f] = band_energy(mags, sample_rate, 250.0f, 2000.0f);
        out->high[f] = band_energy(mags, sample_rate, 2000.0f, 8000.0f);

        if (!prev_mags.empty()) {
            float sum = 0.0f;
            for (size_t i = 0; i < mags.size(); ++i)
                sum += std::max(0.0f, mags[i] - prev_mags[i]);
            flux[f] = sum;
        }
        prev_mags = std::move(mags);
    }

    // Use one shared scale so relative band levels survive.
    const float scale =
        std::max({percentile95(out->low), percentile95(out->mid),
                  percentile95(out->high)});
    scale_curve(out->low, scale);
    scale_curve(out->mid, scale);
    scale_curve(out->high, scale);
    smooth_curve(out->low, video_fps, 0.01f, 0.15f);
    smooth_curve(out->mid, video_fps, 0.01f, 0.15f);
    smooth_curve(out->high, video_fps, 0.01f, 0.15f);

    normalize_curve(flux);
    for (uint32_t f = 1; f + 1 < video_frames; ++f) {
        float local = 0.0f;
        const uint32_t lo = f > 8 ? f - 8 : 0;
        for (uint32_t i = lo; i < f; ++i) local = std::max(local, flux[i]);
        const bool peak = flux[f] > 0.25f && flux[f] >= flux[f - 1] &&
                          flux[f] >= flux[f + 1] && flux[f] > local * 0.8f;
        if (peak) out->onset[f] = 1.0f;
    }

    OnsetEnvelope oe;
    if (build_onset_envelope(samples, frame_total, channels, sample_rate,
                             cancel, &oe)) {
        const double period = estimate_period(oe);
        if (period > 0.0) {
            out->bpm = static_cast<float>(oe.rate * 60.0 / period);
            track_beats(oe, period, &out->beats);
        }
    }
}

void VideoAnalyzer::push_frame(const uint8_t* y, size_t stride, uint32_t width,
                               uint32_t height) {
    const uint32_t step = std::max(1u, width / 120);
    const uint32_t sw = width / step;
    const uint32_t sh = height / step;
    std::vector<uint8_t> sub(static_cast<size_t>(sw) * sh);
    uint64_t sum = 0;
    for (uint32_t r = 0; r < sh; ++r) {
        const uint8_t* row = y + static_cast<size_t>(r) * step * stride;
        for (uint32_t c = 0; c < sw; ++c) {
            const uint8_t v = row[c * step];
            sub[static_cast<size_t>(r) * sw + c] = v;
            sum += v;
        }
    }
    const float mean_luma =
        static_cast<float>(sum) / (static_cast<float>(sub.size()) * 255.0f);
    brightness_.push_back(std::clamp((mean_luma * 255.0f - 16.0f) / 219.0f,
                                     0.0f, 1.0f));

    if (prev_.size() == sub.size()) {
        uint64_t diff = 0;
        for (size_t i = 0; i < sub.size(); ++i)
            diff += static_cast<uint64_t>(
                std::abs(static_cast<int>(sub[i]) - static_cast<int>(prev_[i])));
        const float mean_diff =
            static_cast<float>(diff) / (static_cast<float>(sub.size()) * 255.0f);
        motion_.push_back(std::min(1.0f, mean_diff * 4.0f));
        cut_.push_back(mean_diff > 0.18f ? 1.0f : 0.0f);
    } else {
        motion_.push_back(0.0f);
        cut_.push_back(0.0f);
    }
    prev_ = std::move(sub);
}

void VideoAnalyzer::finish(AnalysisData* out) {
    out->motion = std::move(motion_);
    out->brightness = std::move(brightness_);
    out->cut = std::move(cut_);
}

namespace {

using bytes::app_f32;
using bytes::app_le32;
using Reader = bytes::LeReader;

void put_curve(std::vector<uint8_t>& out, const char* name,
               const std::vector<float>& curve, uint32_t frame_count) {
    const size_t len = std::strlen(name);
    out.push_back(static_cast<uint8_t>(len));
    out.insert(out.end(), name, name + len);
    for (uint32_t i = 0; i < frame_count; ++i)
        app_f32(out, i < curve.size() ? curve[i] : 0.0f);
}

}  // namespace

bool write_analysis(const std::filesystem::path& path,
                    const AnalysisData& data) {
    std::vector<uint8_t> out;
    out.insert(out.end(), {'A', 'N', 'L', '1'});
    app_le32(out, 2);
    uint64_t fps_bits;
    std::memcpy(&fps_bits, &data.fps, 8);
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>(fps_bits >> (i * 8)));
    app_le32(out, data.frame_count);
    app_f32(out, data.bpm);

    const std::pair<const char*, const std::vector<float>*> curves[] = {
        {"low", &data.low},           {"mid", &data.mid},
        {"high", &data.high},         {"onset", &data.onset},
        {"motion", &data.motion},     {"brightness", &data.brightness},
        {"cut", &data.cut},
    };
    app_le32(out, static_cast<uint32_t>(std::size(curves)));
    for (const auto& [name, curve] : curves)
        put_curve(out, name, *curve, data.frame_count);

    app_le32(out, static_cast<uint32_t>(data.beats.size()));
    for (float b : data.beats) app_f32(out, b);

    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

bool load_analysis(const std::filesystem::path& path, AnalysisData* out) {
    const auto bytes = read_file_bytes(path);
    if (!bytes || bytes->size() < 24) return false;
    if (std::memcmp(bytes->data(), "ANL1", 4) != 0) return false;

    Reader r{bytes->data() + 4, bytes->data() + bytes->size()};
    if (r.u32() != 2) return false;
    out->fps = r.f64();
    out->frame_count = r.u32();
    out->bpm = r.f32();
    const uint32_t curve_count = r.u32();
    for (uint32_t c = 0; c < curve_count && r.ok; ++c) {
        if (r.p >= r.end) return false;
        const uint8_t len = *r.p++;
        if (r.end - r.p < len) return false;
        std::string name(reinterpret_cast<const char*>(r.p), len);
        r.p += len;
        std::vector<float> curve(out->frame_count);
        for (uint32_t i = 0; i < out->frame_count; ++i) curve[i] = r.f32();
        if (!r.ok) return false;
        if (name == "low") out->low = std::move(curve);
        else if (name == "mid") out->mid = std::move(curve);
        else if (name == "high") out->high = std::move(curve);
        else if (name == "onset") out->onset = std::move(curve);
        else if (name == "motion") out->motion = std::move(curve);
        else if (name == "brightness") out->brightness = std::move(curve);
        else if (name == "cut") out->cut = std::move(curve);
    }
    const uint32_t beat_count = r.u32();
    if (!r.ok) return false;
    out->beats.resize(beat_count);
    for (uint32_t i = 0; i < beat_count; ++i) out->beats[i] = r.f32();
    return r.ok;
}

}  // namespace looks::mod

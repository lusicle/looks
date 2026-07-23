#include "mod/analysis.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "mod/fft.h"

namespace looks::mod {

namespace {

constexpr uint32_t kFftSize = 1024;

// 95th percentile (robust against single spikes); deterministic.
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

// Normalize a curve to 0..1 by its own 95th percentile.
void normalize_curve(std::vector<float>& curve) {
    scale_curve(curve, percentile95(curve));
}

// Envelope-follower smoothing baked at import (spec §7: attack/release).
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

}  // namespace

void analyze_audio(const int16_t* samples, uint64_t frame_total,
                   uint32_t channels, uint32_t sample_rate, double video_fps,
                   uint32_t video_frames, AnalysisData* out) {
    out->low.assign(video_frames, 0.0f);
    out->mid.assign(video_frames, 0.0f);
    out->high.assign(video_frames, 0.0f);
    out->onset.assign(video_frames, 0.0f);
    out->bpm = 0.0f;
    if (!samples || frame_total == 0 || channels == 0 || video_fps <= 0.0)
        return;

    std::vector<float> window(kFftSize);
    std::vector<float> prev_mags;
    std::vector<float> flux(video_frames, 0.0f);

    for (uint32_t f = 0; f < video_frames; ++f) {
        // Window starting at the frame's timestamp; mono mix.
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

    // One shared scale across the bands: relative levels survive (a bass-
    // only track must not light up the high band via self-normalization).
    const float scale =
        std::max({percentile95(out->low), percentile95(out->mid),
                  percentile95(out->high)});
    scale_curve(out->low, scale);
    scale_curve(out->mid, scale);
    scale_curve(out->high, scale);
    smooth_curve(out->low, video_fps, 0.01f, 0.15f);
    smooth_curve(out->mid, video_fps, 0.01f, 0.15f);
    smooth_curve(out->high, video_fps, 0.01f, 0.15f);

    // Onsets: flux peaks above a moving median-ish threshold.
    normalize_curve(flux);
    for (uint32_t f = 1; f + 1 < video_frames; ++f) {
        float local = 0.0f;
        const uint32_t lo = f > 8 ? f - 8 : 0;
        for (uint32_t i = lo; i < f; ++i) local = std::max(local, flux[i]);
        const bool peak = flux[f] > 0.25f && flux[f] >= flux[f - 1] &&
                          flux[f] >= flux[f + 1] && flux[f] > local * 0.8f;
        if (peak) out->onset[f] = 1.0f;
    }

    // Naive BPM: autocorrelation of the flux curve over 60-180 BPM lags.
    if (video_frames > 8) {
        float best_score = 0.0f;
        float best_bpm = 0.0f;
        for (float bpm = 60.0f; bpm <= 180.0f; bpm += 1.0f) {
            const double lag_frames = video_fps * 60.0 / bpm;
            const uint32_t lag = static_cast<uint32_t>(lag_frames + 0.5);
            if (lag == 0 || lag >= video_frames) continue;
            float score = 0.0f;
            for (uint32_t f = lag; f < video_frames; ++f)
                score += flux[f] * flux[f - lag];
            score /= static_cast<float>(video_frames - lag);
            if (score > best_score) {
                best_score = score;
                best_bpm = bpm;
            }
        }
        out->bpm = best_bpm;
    }
}

// ------------------------------------------------------------- video

void VideoAnalyzer::push_frame(const uint8_t* y, size_t stride, uint32_t width,
                               uint32_t height) {
    // Subsample to a ~120-wide luma thumbnail for the diff.
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

// -------------------------------------------------------------- file io

namespace {

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 24));
}

void put_f32(std::vector<uint8_t>& out, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    put_u32(out, bits);
}

void put_curve(std::vector<uint8_t>& out, const char* name,
               const std::vector<float>& curve, uint32_t frame_count) {
    const size_t len = std::strlen(name);
    out.push_back(static_cast<uint8_t>(len));
    out.insert(out.end(), name, name + len);
    for (uint32_t i = 0; i < frame_count; ++i)
        put_f32(out, i < curve.size() ? curve[i] : 0.0f);
}

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;

    uint32_t u32() {
        if (end - p < 4) { ok = false; return 0; }
        uint32_t v = static_cast<uint32_t>(p[0]) | (p[1] << 8) | (p[2] << 16) |
                     (static_cast<uint32_t>(p[3]) << 24);
        p += 4;
        return v;
    }
    float f32() {
        const uint32_t bits = u32();
        float v;
        std::memcpy(&v, &bits, 4);
        return v;
    }
    double f64() {
        if (end - p < 8) { ok = false; return 0; }
        double v;
        std::memcpy(&v, p, 8);
        p += 8;
        return v;
    }
};

}  // namespace

bool write_analysis(const std::filesystem::path& path,
                    const AnalysisData& data) {
    std::vector<uint8_t> out;
    out.insert(out.end(), {'A', 'N', 'L', '1'});
    put_u32(out, 1);
    uint64_t fps_bits;
    std::memcpy(&fps_bits, &data.fps, 8);
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>(fps_bits >> (i * 8)));
    put_u32(out, data.frame_count);
    put_f32(out, data.bpm);

    const std::pair<const char*, const std::vector<float>*> curves[] = {
        {"low", &data.low},           {"mid", &data.mid},
        {"high", &data.high},         {"onset", &data.onset},
        {"motion", &data.motion},     {"brightness", &data.brightness},
        {"cut", &data.cut},
    };
    put_u32(out, static_cast<uint32_t>(std::size(curves)));
    for (const auto& [name, curve] : curves)
        put_curve(out, name, *curve, data.frame_count);

    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

bool load_analysis(const std::filesystem::path& path, AnalysisData* out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> bytes(static_cast<size_t>(std::max(0l, size)));
    const bool read_ok =
        std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    if (!read_ok || bytes.size() < 24) return false;
    if (std::memcmp(bytes.data(), "ANL1", 4) != 0) return false;

    Reader r{bytes.data() + 4, bytes.data() + bytes.size()};
    if (r.u32() != 1) return false;
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
    return r.ok;
}

}  // namespace looks::mod

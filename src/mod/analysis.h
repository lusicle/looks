// The curves have one sample for each video frame.
// The .analysis file format is little-endian.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "mod/eval.h"

namespace looks::mod {

struct AnalysisData {
    double fps = 0.0;
    uint32_t frame_count = 0;
    float bpm = 0.0f;
    std::vector<float> low, mid, high;     // smoothed band energy, 0..1
    std::vector<float> onset;              // 1 at onset frames, else 0
    std::vector<float> motion, brightness; // video curves, 0..1
    std::vector<float> cut;                // 1 at detected scene cuts
    std::vector<float> beats;

    AnalysisCurves curves() const {
        AnalysisCurves c;
        c.low = low;
        c.mid = mid;
        c.high = high;
        c.onset = onset;
        c.motion = motion;
        c.brightness = brightness;
        c.cut = cut;
        c.beats = beats;
        c.bpm = bpm;
        c.fps = fps;
        return c;
    }
};

// The samples are interleaved s16. cancel is polled once per frame.
// After a cancel, out is incomplete and the caller must not save it.
void analyze_audio(const int16_t* samples, uint64_t frame_total,
                   uint32_t channels, uint32_t sample_rate, double video_fps,
                   uint32_t video_frames, AnalysisData* out,
                   const std::atomic<bool>* cancel = nullptr);

// Feed one luma plane per frame.
class VideoAnalyzer {
public:
    void push_frame(const uint8_t* y, size_t stride, uint32_t width,
                    uint32_t height);
    void finish(AnalysisData* out);

private:
    std::vector<uint8_t> prev_;     // subsampled luma
    std::vector<float> motion_;
    std::vector<float> brightness_;
    std::vector<float> cut_;
};

bool write_analysis(const std::filesystem::path& path, const AnalysisData& data);
bool load_analysis(const std::filesystem::path& path, AnalysisData* out);

}  // namespace looks::mod

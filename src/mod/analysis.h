// Import-time analysis: PCM -> FFT band-energy curves +
// spectral-flux onsets + naive BPM; video pass -> motion/brightness/cut
// curves. Everything is sampled per VIDEO frame and stored in the
// `.analysis` sidecar so audio-reactive params scrub instantly and
// deterministically. Live mode computes the same sources in realtime later.
//
// .analysis layout (little-endian):
//   'ANL1' u32 version=1 f64 fps u32 frame_count f32 bpm u32 curve_count
//   then per curve: u8 name_len, name, frame_count × f32

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

    AnalysisCurves curves() const {
        AnalysisCurves c;
        c.low = low;
        c.mid = mid;
        c.high = high;
        c.onset = onset;
        c.motion = motion;
        c.brightness = brightness;
        c.cut = cut;
        c.bpm = bpm;
        c.fps = fps;
        return c;
    }
};

// Interleaved s16 -> band/onset/bpm curves sampled at video frame times.
// Minutes of FFT on a long track: `cancel` (when given) is polled per
// frame so a closing app never waits out the loop - a cancelled run
// leaves `out` partial and the caller must not persist it.
void analyze_audio(const int16_t* samples, uint64_t frame_total,
                   uint32_t channels, uint32_t sample_rate, double video_fps,
                   uint32_t video_frames, AnalysisData* out,
                   const std::atomic<bool>* cancel = nullptr);

// Streaming video analysis fed one I420 luma plane per frame.
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

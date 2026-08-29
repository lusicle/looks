// render_mix is pure: a sample range mixes to the same bytes from any
// thread, in any order. DSP ops read shifted positions and keep no state.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace looks::media {

struct PcmBuffer {
    std::vector<int16_t> samples;   // interleaved
    uint32_t channels = 0;
    uint32_t rate = 0;

    uint64_t frames() const {
        return channels ? samples.size() / channels : 0;
    }
};

// Returns null on failure; a silent source is not an error to callers.
std::shared_ptr<const PcmBuffer> load_pcm(const std::filesystem::path& path,
                                          std::string* error = nullptr);

// p[] meanings per kind:
//   Gain        p0 linear level
//   Bitcrush    p0 bits (1..16)
//   Downsample  p0 hold length in node-local samples
//   Distortion  p0 drive 0..1
//   Delay       p0 time ms, p1 feedback 0..0.95
//   Filter      p0 cutoff 0..1 of the node Nyquist, p1 mode 0=LP 1=HP
// Time params use the owning look clock, in local samples at the mix rate.
enum class MixOpKind : uint32_t {
    Gain = 0,
    Bitcrush,
    Downsample,
    Distortion,
    Delay,
    Filter,
};

struct MixOp {
    MixOpKind kind = MixOpKind::Gain;
    float p[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float wet = 1.0f;
};

// Keep the windowed-sinc kernel width odd for an exact center tap.
inline constexpr int kFirTaps = 63;

// Must match doc::kUnbounded; it marks an unbounded window edge.
inline constexpr double kMixUnbounded = 1e18;

// Semantic fields use ROOT frames; prepare_mix derives the eval constants.
struct MixNode {
    // A null buffer makes this an interior node that sums its inputs.
    std::shared_ptr<const PcmBuffer> pcm;
    // Composed clock: local = a * root_frame + b; local_fps 0 means mix fps.
    // Leaves read PCM at local seconds; op taps use local samples at mix rate.
    double a = 1.0, b = 0.0;
    double local_fps = 0.0;
    // Hop window in ROOT frames; outside it the node gives 0 after a ramp.
    bool windowed = false;
    double w0 = 0.0, w1 = kMixUnbounded;
    // Applies to this node output.
    float gain = 1.0f;
    bool has_op = false;
    MixOp op;
    std::vector<int> inputs;

    // Derived by prepare_mix - never set by hand.
    double A = 0.0, B = 0.0;    // output sample -> pcm frame (leaves)
    double La = 1.0, Lb = 0.0;  // output sample -> node-local sample
    double s0 = 0.0, s1 = 0.0;  // window in output samples
    int64_t ramp = 0;           // edge ramp width in samples
    float fir[kFirTaps] = {};   // Filter kernel
};

struct MixState {
    std::vector<MixNode> nodes;
    int root = -1;
    double fps = 30.0;
    uint32_t rate = 48000;
    uint32_t channels = 2;
};

// Edge ramp width in output samples; keep it short so a cut cannot click.
inline constexpr int64_t kRampSamples = 192;

// Call once after you build or change a MixState, before render_mix.
void prepare_mix(MixState& mix);

// Writes output samples [start, start + count) interleaved; positions
// outside the program are silence. scratch is reused, so this never allocates.
void render_mix(const MixState& mix, int64_t start, int16_t* out,
                uint32_t count, std::vector<float>& scratch);

// Renders the source at its native rate and channels, identity time map.
void render_processed_pcm(const PcmBuffer& src,
                          const std::vector<MixOp>& ops, PcmBuffer* out);

// Min and max over the covered samples, in raw s16 scale.
struct WaveSpan {
    float lo = 0.0f;
    float hi = 0.0f;
};

// Level 0 buckets base samples per span; each level folds 4 spans into 1.
// Channels average to mono.
struct WavePyramid {
    int64_t start = 0;     // output sample of level-0 span 0
    uint32_t base = 64;    // samples per level-0 span
    uint32_t rate = 48000;
    std::vector<std::vector<WaveSpan>> levels;
};

// cancel aborts between chunks and leaves out empty.
void build_wave_pyramid(const MixState& mix, int64_t s0, int64_t s1,
                        uint32_t base, WavePyramid* out,
                        const std::atomic<bool>* cancel = nullptr);

// Gives the input sum and output envelopes over [s0, s1), columns spans.
// A column wider than this probes a deterministic stride, not every sample.
inline constexpr uint32_t kMaxColumnProbes = 256;
void render_node_envelopes(const MixState& mix, int node, int64_t s0,
                           int64_t s1, uint32_t columns,
                           std::vector<WaveSpan>* in_env,
                           std::vector<WaveSpan>* out_env);

}  // namespace looks::media

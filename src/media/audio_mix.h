// Graph audio mix.
//
// A look's audio out is its VOICE: the wired graph itself, evaluated.
// The mix receives a flattened AUDIO PROGRAM - a DAG whose leaves read
// PCM through composed affine clocks, whose interior nodes SUM their
// fan-in (combine-all: every live feed contributes, exactly the
// connection rule the video composite merges by), and whose op nodes
// apply DSP to the SUMMED signal on the owning look's clock. Sequence
// placements and nested duration cuts are windowed hop nodes; the cut
// sits ABOVE the DSP, so a razor is structural for sound the way it is
// for picture.
//
// render_mix is PURE: a sample range always mixes to the same bytes,
// from any thread, in any order. That is what lets the monitor callback
// and the export encoder share one implementation. DSP ops keep the
// contract by being pure functions of absolute position: delays and
// holds re-read their INPUT SUM at shifted positions instead of
// carrying state, so any chunking yields the same bytes.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace looks::media {

// A whole PCM sidecar in RAM, shared by every placement of its asset.
struct PcmBuffer {
    std::vector<int16_t> samples;   // interleaved
    uint32_t channels = 0;
    uint32_t rate = 0;

    uint64_t frames() const {
        return channels ? samples.size() / channels : 0;
    }
};

// Loads a .pcm sidecar whole. Null on failure (a silent source is not an
// error anywhere else, so callers just carry on).
std::shared_ptr<const PcmBuffer> load_pcm(const std::filesystem::path& path,
                                          std::string* error = nullptr);

// One DSP hop - the doc-free mirror of doc::AudioOp (the mix never
// includes doc headers). p[] meanings per kind:
//   Gain        p0 linear level
//   Bitcrush    p0 bits (1..16)
//   Downsample  p0 hold length in node-local samples
//   Distortion  p0 drive 0..1 (normalized tanh shaper)
//   Delay       p0 time ms, p1 feedback 0..0.95
//   Filter      p0 cutoff 0..1 of the node's Nyquist, p1 mode 0=LP 1=HP
// Time-domain params live on the OWNING LOOK's clock (node-local
// samples at the mix rate), so placement speed scales them with the
// pitch, like everything else about a placed look.
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
    float wet = 1.0f;   // dry/wet per hop, same meaning as effect wet
};

// Windowed-sinc filters use this kernel width (odd: exact center tap).
inline constexpr int kFirTaps = 63;

// An unbounded window edge (matches doc::kUnbounded's meaning).
inline constexpr double kMixUnbounded = 1e18;

// One node of the audio program. Semantic fields are in ROOT frames /
// the owning look's clock; prepare_mix derives the per-sample eval
// constants once per rebuild so the render callback stays pure and
// allocation-free.
struct MixNode {
    // Leaf read; null makes this an interior node (sum of `inputs`,
    // optionally through `op`). A leaf with a missing sidecar is
    // simply silent (null pcm, no inputs).
    std::shared_ptr<const PcmBuffer> pcm;
    // Composed clock: local = a * root_frame + b, local frames ticking
    // at local_fps (0 = the mix fps). Leaves read the PCM at local
    // seconds; op taps measure in local samples at the mix rate.
    double a = 1.0, b = 0.0;
    double local_fps = 0.0;
    // Hop window in ROOT frames: contribution is 0 outside, with a
    // short deterministic edge ramp so a cut cannot click. Only hop
    // nodes (placements, nested duration cuts, the scope horizon) are
    // windowed - the cut sits ABOVE the DSP, and leaves taper on their
    // own PCM bounds.
    bool windowed = false;
    double w0 = 0.0, w1 = kMixUnbounded;
    // Applied to this node's output (track/placement gains ride hops).
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

// Output samples the deterministic edge ramps run over: a few ms, so a cut
// cannot click, short enough that no one hears it as a fade.
inline constexpr int64_t kRampSamples = 192;

// Derives every node's eval constants (clock affines, sample windows,
// FIR kernels) from the semantic fields and the mix format. Call once
// after building or mutating a MixState, before render_mix.
void prepare_mix(MixState& mix);

// Mixes output samples [start, start + count) into `out` (interleaved,
// mix.channels per sample), overwriting it. Sample positions outside the
// program are silence, which is what a gap in the timeline is.
// `scratch` is the caller's float accumulator: it grows once and is reused,
// so the monitor callback never allocates.
void render_mix(const MixState& mix, int64_t start, int16_t* out,
                uint32_t count, std::vector<float>& scratch);

// Renders the whole source through a linear op chain at native
// rate/channels - the runtime-analysis path analyzes one media stream
// (analysis chains stay single-stream: beat clocks anchor on media
// time). Identity time map; no ops = a straight copy.
void render_processed_pcm(const PcmBuffer& src,
                          const std::vector<MixOp>& ops, PcmBuffer* out);

// One display span of a signal: its min/max over the covered samples,
// in raw s16 scale.
struct WaveSpan {
    float lo = 0.0f;
    float hi = 0.0f;
};

// Min/max peak pyramid of the program's ACTUAL output (the same bytes
// render_mix plays, channels averaged to mono): level 0 buckets `base`
// samples per span, each level above folds 4 spans into 1. A display
// picks the level at or below its samples-per-pixel and combines
// spans per column - exact envelopes at any zoom, no resampling.
struct WavePyramid {
    int64_t start = 0;     // output sample of level-0 span 0
    uint32_t base = 64;    // samples per level-0 span
    uint32_t rate = 48000;
    std::vector<std::vector<WaveSpan>> levels;
};

// Renders output samples [s0, s1) and folds them into a pyramid.
// `cancel` (optional) aborts between chunks, leaving `out` empty.
void build_wave_pyramid(const MixState& mix, int64_t s0, int64_t s1,
                        uint32_t base, WavePyramid* out,
                        const std::atomic<bool>* cancel = nullptr);

// The min/max envelopes of one program node's INPUT SUM and OUTPUT
// over output samples [s0, s1), `columns` spans each - a card graph's
// two traces (what feeds the node vs what leaves it). Channels average
// to mono; columns wider than kMaxColumnProbes samples probe a
// deterministic stride instead of every sample.
inline constexpr uint32_t kMaxColumnProbes = 256;
void render_node_envelopes(const MixState& mix, int node, int64_t s0,
                           int64_t s1, uint32_t columns,
                           std::vector<WaveSpan>* in_env,
                           std::vector<WaveSpan>* out_env);

}  // namespace looks::media

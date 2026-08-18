// Tree audio mix.
//
// A look's audio out is its VOICE - the chain wired into its Output -
// carried per instance as PCM plus an ordered DSP op list; the mix sums
// voices through their time maps. There is no mixer panel: per-source
// gain/mute and audio-modifier nodes in the look graph are the whole
// control surface. Because picture and sound travel through the SAME
// instance tree, razoring or sliding a block carries its audio by
// construction - they cannot desync.
//
// render_mix is PURE: a sample range always mixes to the same bytes, from
// any thread, in any order. That is what lets the monitor callback and the
// export encoder share one implementation, and it is why the mix is a
// closed form over flattened placements (doc/instances.h) rather than
// something the render loop walks. DSP ops keep the contract by being
// pure functions of absolute source position: delays and holds re-read
// the source at shifted positions instead of carrying state, so any
// chunking yields the same bytes.

#pragma once

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

// One DSP hop on a voice - the doc-free mirror of doc::AudioOp (the mix
// never includes doc headers). p[] meanings per kind:
//   Gain        p0 linear level
//   Bitcrush    p0 bits (1..16)
//   Downsample  p0 hold length in SOURCE samples
//   Distortion  p0 drive 0..1 (normalized tanh shaper)
//   Delay       p0 time ms (source-rate), p1 feedback 0..0.95
//   Filter      p0 cutoff 0..1 of source Nyquist, p1 mode 0=LP 1=HP
// Time-domain params live in SOURCE samples/ms, so placement speed
// scales them with the pitch, like everything else about a source.
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

// Voice op chains cap here (matches doc::kMaxVoiceOps).
inline constexpr size_t kMaxMixOps = 8;

// One placement's audio, in ROOT timeline frames - the audio half of
// doc::MediaInstance. `ops` apply in order, source first.
struct MixSource {
    std::shared_ptr<const PcmBuffer> pcm;
    double t_in = 0.0;
    double t_out = 0.0;
    double source_in = 0.0;
    double speed = 1.0;
    float gain = 1.0f;
    std::vector<MixOp> ops;
};

struct MixState {
    std::vector<MixSource> sources;
    double fps = 30.0;
    uint32_t rate = 48000;
    uint32_t channels = 2;
};

// Output samples the deterministic edge ramps run over: a few ms, so a cut
// cannot click, short enough that no one hears it as a fade.
inline constexpr int64_t kRampSamples = 192;

// Mixes output samples [start, start + count) into `out` (interleaved,
// mix.channels per sample), overwriting it. Sample positions outside every
// source are silence, which is what a gap in the timeline is.
// `scratch` is the caller's float accumulator: it grows once and is reused,
// so the monitor callback never allocates.
void render_mix(const MixState& mix, int64_t start, int16_t* out,
                uint32_t count, std::vector<float>& scratch);

// Renders the whole source through `ops` at native rate/channels - the
// runtime-analysis path analyzes exactly the audio the mix would play.
// Identity time map; no ops = a straight copy.
void render_processed_pcm(const PcmBuffer& src,
                          const std::vector<MixOp>& ops, PcmBuffer* out);

}  // namespace looks::media

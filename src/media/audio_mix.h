// Tree audio mix (docs/look.md phase 4).
//
// A look's audio out is the sum of its active clip sources' PCM through
// their time maps. There is no audio node graph, no audio effects and no
// mixer panel: per-source gain/mute and a look-level mute are the whole
// control surface. Because picture and sound travel through the SAME
// instance tree, razoring or sliding a block carries its audio by
// construction - they cannot desync.
//
// render_mix is PURE: a sample range always mixes to the same bytes, from
// any thread, in any order. That is what lets the monitor callback and the
// export encoder share one implementation, and it is why the mix is a
// closed form over flattened placements (doc/instances.h) rather than
// something the render loop walks.

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

// One placement's audio, in ROOT timeline frames - the audio half of
// doc::ClipInstance.
struct MixSource {
    std::shared_ptr<const PcmBuffer> pcm;
    double t_in = 0.0;
    double t_out = 0.0;
    double source_in = 0.0;
    double speed = 1.0;
    float gain = 1.0f;
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

}  // namespace looks::media

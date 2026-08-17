// Tree audio mix: a look's audio is the sum of its
// active clip sources' PCM through their time maps.
//
// render_mix is a PURE function of (mix, sample range) - the monitor
// callback and the export encoder share it, so any position dependence
// would desync what you hear from what you get.

#include "media/audio_mix.h"

#include "test_framework.h"

using looks::media::MixSource;
using looks::media::MixState;
using looks::media::PcmBuffer;
using looks::media::render_mix;

namespace {

// A ramp: sample n holds value n, so a mapping error is readable straight
// off the output.
std::shared_ptr<const PcmBuffer> ramp_pcm(uint32_t frames, uint32_t channels,
                                          uint32_t rate) {
    auto pcm = std::make_shared<PcmBuffer>();
    pcm->channels = channels;
    pcm->rate = rate;
    pcm->samples.resize(static_cast<size_t>(frames) * channels);
    for (uint32_t f = 0; f < frames; ++f)
        for (uint32_t c = 0; c < channels; ++c)
            pcm->samples[f * channels + c] = static_cast<int16_t>(f);
    return pcm;
}

MixState one_source_mix(std::shared_ptr<const PcmBuffer> pcm, double t_in,
                        double t_out, double source_in, double speed) {
    MixState mix;
    mix.fps = 30.0;
    mix.rate = pcm->rate;
    mix.channels = pcm->channels;
    MixSource src;
    src.pcm = std::move(pcm);
    src.t_in = t_in;
    src.t_out = t_out;
    src.source_in = source_in;
    src.speed = speed;
    mix.sources.push_back(std::move(src));
    return mix;
}

}  // namespace

TEST(mix_gap_is_silence) {
    // A timeline position no source covers is silence - which is what a
    // gap between two blocks is.
    const MixState mix = one_source_mix(ramp_pcm(4800, 1, 48000), 30.0, 60.0,
                                        0.0, 1.0);
    std::vector<float> scratch;
    std::vector<int16_t> out(64, 999);
    render_mix(mix, 0, out.data(), 64, scratch);
    for (int16_t v : out) CHECK_EQ(v, int16_t{0});
}

TEST(mix_maps_output_samples_onto_source_samples) {
    // Speed 1, matching rates: output sample s reads source sample s,
    // offset by where the placement starts.
    const MixState mix = one_source_mix(ramp_pcm(48000, 1, 48000), 0.0, 100.0,
                                        0.0, 1.0);
    std::vector<float> scratch;
    std::vector<int16_t> out(8, 0);
    // Past the edge ramp so the level is unity.
    const int64_t at = 4000;
    render_mix(mix, at, out.data(), 8, scratch);
    for (uint32_t i = 0; i < 8; ++i)
        CHECK_EQ(out[i], static_cast<int16_t>(at + i));

    // source_in 10 frames at 30 fps = 16000 samples in.
    const MixState offset = one_source_mix(ramp_pcm(48000, 1, 48000), 0.0,
                                           100.0, 10.0, 1.0);
    render_mix(offset, at, out.data(), 8, scratch);
    for (uint32_t i = 0; i < 8; ++i)
        CHECK_EQ(out[i], static_cast<int16_t>(at + i + 16000));
}

TEST(mix_speed_resamples) {
    // Double speed walks the source twice as fast.
    const MixState mix = one_source_mix(ramp_pcm(48000, 1, 48000), 0.0, 100.0,
                                        0.0, 2.0);
    std::vector<float> scratch;
    std::vector<int16_t> out(4, 0);
    const int64_t at = 4000;
    render_mix(mix, at, out.data(), 4, scratch);
    for (uint32_t i = 0; i < 4; ++i)
        CHECK_EQ(out[i], static_cast<int16_t>((at + i) * 2));
}

TEST(mix_sums_sources_and_applies_gain) {
    auto pcm = ramp_pcm(48000, 1, 48000);
    MixState mix = one_source_mix(pcm, 0.0, 100.0, 0.0, 1.0);
    mix.sources[0].gain = 0.5f;
    MixSource second;
    second.pcm = pcm;
    second.t_in = 0.0;
    second.t_out = 100.0;
    second.gain = 0.25f;
    mix.sources.push_back(second);

    std::vector<float> scratch;
    std::vector<int16_t> out(4, 0);
    const int64_t at = 4000;
    render_mix(mix, at, out.data(), 4, scratch);
    for (uint32_t i = 0; i < 4; ++i)
        CHECK_EQ(out[i], static_cast<int16_t>((at + i) * 3 / 4));
}

TEST(mix_is_position_independent) {
    // The callback pulls in blocks whose size the device picks; export
    // pulls in 1024s. Same samples either way, or preview and export
    // disagree about the soundtrack.
    MixState mix = one_source_mix(ramp_pcm(48000, 2, 48000), 5.0, 40.0, 3.0,
                                  1.5);
    mix.sources[0].gain = 0.8f;

    std::vector<float> scratch;
    std::vector<int16_t> whole(256 * 2, 0);
    render_mix(mix, 9000, whole.data(), 256, scratch);

    std::vector<int16_t> chunked(256 * 2, 0);
    for (uint32_t off = 0; off < 256; off += 37) {
        const uint32_t n = std::min<uint32_t>(37, 256 - off);
        render_mix(mix, 9000 + off, chunked.data() + off * 2, n, scratch);
    }
    for (size_t i = 0; i < whole.size(); ++i) CHECK_EQ(whole[i], chunked[i]);
}

TEST(mix_ramps_the_edges) {
    // A cut must not click: the level fades in over a few ms at the
    // in-point, deterministically, and is at unity well clear of it. A
    // flat source makes the fade the only thing that can vary.
    auto flat = std::make_shared<PcmBuffer>();
    flat->channels = 1;
    flat->rate = 48000;
    flat->samples.assign(48000, int16_t{1000});

    const MixState mix = one_source_mix(flat, 0.0, 100.0, 0.0, 1.0);
    std::vector<float> scratch;
    std::vector<int16_t> edge(4, 0);
    render_mix(mix, 0, edge.data(), 4, scratch);
    CHECK(edge[0] > 0);
    CHECK(edge[0] < edge[1]);
    CHECK(edge[1] < edge[2]);
    CHECK(edge[3] < int16_t{1000});

    std::vector<int16_t> middle(4, 0);
    render_mix(mix, 4000, middle.data(), 4, scratch);
    for (int16_t v : middle) CHECK_EQ(v, int16_t{1000});
}

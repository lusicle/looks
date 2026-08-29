#include "media/audio_mix.h"

#include <cstring>

#include "test_framework.h"

using looks::media::MixNode;
using looks::media::MixOp;
using looks::media::MixOpKind;
using looks::media::MixState;
using looks::media::PcmBuffer;
using looks::media::prepare_mix;
using looks::media::render_mix;

namespace {

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

std::shared_ptr<const PcmBuffer> flat_pcm(int16_t value, uint32_t rate) {
    auto pcm = std::make_shared<PcmBuffer>();
    pcm->channels = 1;
    pcm->rate = rate;
    pcm->samples.assign(rate, value);
    return pcm;
}

int add_leaf(MixState& mix, std::shared_ptr<const PcmBuffer> pcm,
             double t_in = 0.0, double source_in = 0.0,
             double speed = 1.0) {
    MixNode leaf;
    leaf.pcm = std::move(pcm);
    leaf.a = speed;
    leaf.b = source_in - t_in * speed;
    mix.nodes.push_back(std::move(leaf));
    return static_cast<int>(mix.nodes.size()) - 1;
}

int add_op(MixState& mix, MixOp op, int input) {
    MixNode n;
    n.has_op = true;
    n.op = op;
    n.inputs.push_back(input);
    mix.nodes.push_back(std::move(n));
    return static_cast<int>(mix.nodes.size()) - 1;
}

int add_hop(MixState& mix, double t_in, double t_out, float gain,
            int input) {
    MixNode n;
    n.windowed = true;
    n.w0 = t_in;
    n.w1 = t_out;
    n.gain = gain;
    n.inputs.push_back(input);
    mix.nodes.push_back(std::move(n));
    return static_cast<int>(mix.nodes.size()) - 1;
}

MixState one_source_mix(std::shared_ptr<const PcmBuffer> pcm, double t_in,
                        double t_out, double source_in, double speed) {
    MixState mix;
    mix.fps = 30.0;
    mix.rate = pcm->rate;
    mix.channels = pcm->channels;
    const int leaf = add_leaf(mix, std::move(pcm), t_in, source_in, speed);
    mix.root = add_hop(mix, t_in, t_out, 1.0f, leaf);
    prepare_mix(mix);
    return mix;
}

}  // namespace

TEST(mix_gap_is_silence) {
    const MixState mix = one_source_mix(ramp_pcm(4800, 1, 48000), 30.0, 60.0,
                                        0.0, 1.0);
    std::vector<float> scratch;
    std::vector<int16_t> out(64, 999);
    render_mix(mix, 0, out.data(), 64, scratch);
    for (int16_t v : out) CHECK_EQ(v, int16_t{0});
}

TEST(mix_maps_output_samples_onto_source_samples) {
    const MixState mix = one_source_mix(ramp_pcm(48000, 1, 48000), 0.0, 100.0,
                                        0.0, 1.0);
    std::vector<float> scratch;
    std::vector<int16_t> out(8, 0);
    // Start after the edge fade so the level is unity.
    const int64_t at = 4000;
    render_mix(mix, at, out.data(), 8, scratch);
    for (uint32_t i = 0; i < 8; ++i)
        CHECK_EQ(out[i], static_cast<int16_t>(at + i));

    // source_in of 10 frames at 30 fps equals 16000 samples.
    const MixState offset = one_source_mix(ramp_pcm(48000, 1, 48000), 0.0,
                                           100.0, 10.0, 1.0);
    render_mix(offset, at, out.data(), 8, scratch);
    for (uint32_t i = 0; i < 8; ++i)
        CHECK_EQ(out[i], static_cast<int16_t>(at + i + 16000));
}

TEST(mix_speed_resamples) {
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
    // The two hop gains 0.5 and 0.25 sum to 0.75 at the root.
    auto pcm = ramp_pcm(48000, 1, 48000);
    MixState mix;
    mix.fps = 30.0;
    mix.rate = pcm->rate;
    mix.channels = pcm->channels;
    const int a = add_hop(mix, 0.0, 100.0, 0.5f, add_leaf(mix, pcm));
    const int b = add_hop(mix, 0.0, 100.0, 0.25f, add_leaf(mix, pcm));
    MixNode sum;
    sum.inputs = {a, b};
    mix.nodes.push_back(std::move(sum));
    mix.root = static_cast<int>(mix.nodes.size()) - 1;
    prepare_mix(mix);

    std::vector<float> scratch;
    std::vector<int16_t> out(4, 0);
    const int64_t at = 4000;
    render_mix(mix, at, out.data(), 4, scratch);
    for (uint32_t i = 0; i < 4; ++i)
        CHECK_EQ(out[i], static_cast<int16_t>((at + i) * 3 / 4));
}

TEST(mix_is_position_independent) {
    // The device picks the block size, so any chunking mixes the same bytes.
    MixState mix = one_source_mix(ramp_pcm(48000, 2, 48000), 5.0, 40.0, 3.0,
                                  1.5);
    mix.nodes[static_cast<size_t>(mix.root)].gain = 0.8f;

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
    // The level fades in at the in-point so a cut does not click.
    // A flat source makes the fade the only thing that can change.
    const MixState mix = one_source_mix(flat_pcm(1000, 48000), 0.0, 100.0,
                                        0.0, 1.0);
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

TEST(mix_ops_gain_scales_the_voice) {
    auto pcm = ramp_pcm(48000, 1, 48000);
    MixState mix;
    mix.fps = 30.0;
    mix.rate = pcm->rate;
    mix.channels = 1;
    MixOp gain;
    gain.kind = MixOpKind::Gain;
    gain.p[0] = 2.0f;
    mix.root = add_hop(mix, 0.0, 100.0, 1.0f,
                       add_op(mix, gain, add_leaf(mix, pcm)));
    prepare_mix(mix);
    std::vector<float> scratch;
    std::vector<int16_t> out(8, 0);
    render_mix(mix, 4000, out.data(), 8, scratch);
    for (uint32_t i = 0; i < 8; ++i)
        CHECK_EQ(out[i], static_cast<int16_t>((4000 + i) * 2));
}

TEST(mix_ops_bitcrush_quantizes_amplitude) {
    auto flat = flat_pcm(1000, 48000);
    MixState mix;
    mix.fps = 30.0;
    mix.rate = 48000;
    mix.channels = 1;
    MixOp crush;
    crush.kind = MixOpKind::Bitcrush;
    crush.p[0] = 8.0f;   // step 65536/256 = 256 -> 1000 snaps to 1024
    mix.root = add_hop(mix, 0.0, 100.0, 1.0f,
                       add_op(mix, crush, add_leaf(mix, flat)));
    prepare_mix(mix);
    std::vector<float> scratch;
    std::vector<int16_t> out(4, 0);
    render_mix(mix, 4000, out.data(), 4, scratch);
    for (int16_t v : out) CHECK_EQ(v, int16_t{1024});
}

TEST(mix_fan_in_sums_before_the_op) {
    // The op crushes the sum: 100 + 60 snaps to 256, each branch to 0.
    MixState mix;
    mix.fps = 30.0;
    mix.rate = 48000;
    mix.channels = 1;
    const int a = add_leaf(mix, flat_pcm(100, 48000));
    const int b = add_leaf(mix, flat_pcm(60, 48000));
    MixOp crush;
    crush.kind = MixOpKind::Bitcrush;
    crush.p[0] = 8.0f;
    MixNode op;
    op.has_op = true;
    op.op = crush;
    op.inputs = {a, b};
    mix.nodes.push_back(std::move(op));
    mix.root = add_hop(mix, 0.0, 100.0, 1.0f,
                       static_cast<int>(mix.nodes.size()) - 1);
    prepare_mix(mix);
    std::vector<float> scratch;
    std::vector<int16_t> out(4, 0);
    render_mix(mix, 4000, out.data(), 4, scratch);
    for (int16_t v : out) CHECK_EQ(v, int16_t{256});
}

TEST(mix_ops_delay_echoes_the_past) {
    // 100 ms at 48 kHz is 4800 samples.
    // The taps re-read the input at shifted positions, with no carried state.
    auto pcm = std::make_shared<PcmBuffer>();
    pcm->channels = 1;
    pcm->rate = 48000;
    pcm->samples.assign(48000, int16_t{0});
    pcm->samples[0] = 16000;
    MixState mix;
    mix.fps = 30.0;
    mix.rate = 48000;
    mix.channels = 1;
    MixOp delay;
    delay.kind = MixOpKind::Delay;
    delay.p[0] = 100.0f;
    delay.p[1] = 0.5f;
    mix.root = add_hop(mix, 0.0, 100.0, 1.0f,
                       add_op(mix, delay, add_leaf(mix, pcm)));
    prepare_mix(mix);
    std::vector<float> scratch;
    std::vector<int16_t> out(1, 0);
    render_mix(mix, 4800, out.data(), 1, scratch);
    CHECK_EQ(out[0], int16_t{8000});    // first tap: fb^1
    render_mix(mix, 9600, out.data(), 1, scratch);
    CHECK_EQ(out[0], int16_t{4000});    // second tap: fb^2
    render_mix(mix, 7200, out.data(), 1, scratch);
    CHECK_EQ(out[0], int16_t{0});       // between taps: silence
}

TEST(mix_render_processed_pcm_applies_ops) {
    auto pcm = ramp_pcm(64, 1, 48000);
    looks::media::PcmBuffer out;
    looks::media::render_processed_pcm(*pcm, {}, &out);
    CHECK_EQ(out.samples.size(), pcm->samples.size());
    CHECK_EQ(out.samples[10], pcm->samples[10]);
    MixOp gain;
    gain.kind = MixOpKind::Gain;
    gain.p[0] = 2.0f;
    looks::media::render_processed_pcm(*pcm, {gain}, &out);
    CHECK_EQ(out.samples[10], int16_t{20});
}

TEST(mix_wave_pyramid_folds_exact_envelopes) {
    // Level 0 buckets at base samples, and upper levels fold 4:1.
    auto pcm = std::make_shared<PcmBuffer>();
    pcm->channels = 1;
    pcm->rate = 48000;
    pcm->samples.assign(48000, int16_t{0});
    pcm->samples[10000] = 12000;
    pcm->samples[10001] = -9000;
    MixState mix;
    mix.fps = 30.0;
    mix.rate = 48000;
    mix.channels = 1;
    mix.root = add_hop(mix, 0.0, 30.0, 1.0f, add_leaf(mix, pcm));
    prepare_mix(mix);

    looks::media::WavePyramid pyr;
    looks::media::build_wave_pyramid(mix, 0, 48000, 64, &pyr);
    CHECK(!pyr.levels.empty());
    if (pyr.levels.empty()) return;
    CHECK_EQ(pyr.levels[0].size(), size_t{750});
    const looks::media::WaveSpan& hit = pyr.levels[0][10000 / 64];
    CHECK(hit.hi > 11000.0f);
    CHECK(hit.lo < -8000.0f);
    for (const auto& level : pyr.levels) {
        float hi = 0.0f, lo = 0.0f;
        for (const looks::media::WaveSpan& w : level) {
            hi = std::max(hi, w.hi);
            lo = std::min(lo, w.lo);
        }
        CHECK(hi > 11000.0f);
        CHECK(lo < -8000.0f);
    }
}

TEST(mix_node_envelopes_show_input_vs_output) {
    auto flat = flat_pcm(1000, 48000);
    MixState mix;
    mix.fps = 30.0;
    mix.rate = 48000;
    mix.channels = 1;
    MixOp gain;
    gain.kind = MixOpKind::Gain;
    gain.p[0] = 2.0f;
    const int op = add_op(mix, gain, add_leaf(mix, flat));
    mix.root = add_hop(mix, 0.0, 100.0, 1.0f, op);
    prepare_mix(mix);

    std::vector<looks::media::WaveSpan> in_env, out_env;
    looks::media::render_node_envelopes(mix, op, 0, 48000, 16, &in_env,
                                        &out_env);
    CHECK_EQ(in_env.size(), size_t{16});
    CHECK_EQ(out_env.size(), size_t{16});
    if (in_env.size() < 16) return;
    CHECK_EQ(in_env[8].hi, 1000.0f);
    CHECK_EQ(in_env[8].lo, 1000.0f);
    CHECK_EQ(out_env[8].hi, 2000.0f);
    CHECK_EQ(out_env[8].lo, 2000.0f);
}

TEST(mix_ops_are_pure_under_any_chunking) {
    // Ops are pure functions of absolute sample position.
    auto pcm = ramp_pcm(48000, 1, 48000);
    MixState mix;
    mix.fps = 30.0;
    mix.rate = 48000;
    mix.channels = 1;
    MixOp delay;
    delay.kind = MixOpKind::Delay;
    delay.p[0] = 50.0f;
    delay.p[1] = 0.6f;
    MixOp filt;
    filt.kind = MixOpKind::Filter;
    filt.p[0] = 0.2f;
    mix.root = add_hop(
        mix, 0.0, 100.0, 1.0f,
        add_op(mix, filt, add_op(mix, delay, add_leaf(mix, pcm))));
    prepare_mix(mix);

    std::vector<float> scratch;
    constexpr uint32_t kN = 192;
    const int64_t at = 10000;
    std::vector<int16_t> whole(kN, 0);
    render_mix(mix, at, whole.data(), kN, scratch);

    std::vector<int16_t> chunked(kN, 0);
    for (uint32_t off = 0; off < kN; off += 29) {
        const uint32_t n = std::min<uint32_t>(29, kN - off);
        render_mix(mix, at + off, chunked.data() + off, n, scratch);
    }
    std::vector<int16_t> single(kN, 0);
    for (uint32_t off = 0; off < kN; ++off)
        render_mix(mix, at + off, single.data() + off, 1, scratch);

    for (size_t i = 0; i < kN; ++i) {
        CHECK_EQ(whole[i], chunked[i]);
        CHECK_EQ(whole[i], single[i]);
    }
}

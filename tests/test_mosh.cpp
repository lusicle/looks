#include "codec/mosh/mosh.h"

#include <cmath>
#include <cstdlib>

#include "test_framework.h"

using namespace looks::codec;

namespace {

constexpr uint32_t kW = 64;
constexpr uint32_t kH = 48;

struct TestFrame {
    std::vector<uint8_t> y, u, v;

    explicit TestFrame(uint32_t pattern) {
        y.resize(kW * kH);
        u.resize((kW / 2) * (kH / 2));
        v.resize((kW / 2) * (kH / 2));
        for (uint32_t r = 0; r < kH; ++r)
            for (uint32_t c = 0; c < kW; ++c)
                y[r * kW + c] =
                    static_cast<uint8_t>(16 + ((c * 3 + r * 2 + pattern * 40) & 0xBF));
        for (size_t i = 0; i < u.size(); ++i) {
            u[i] = static_cast<uint8_t>(96 + ((i + pattern * 17) & 63));
            v[i] = static_cast<uint8_t>(160 - ((i * 2 + pattern * 29) & 63));
        }
    }

    FrameView view() const {
        return {{y.data(), kW}, {u.data(), kW / 2}, {v.data(), kW / 2},
                kW, kH};
    }
};

double mean_abs_diff(const std::vector<uint8_t>& a,
                     const std::vector<uint8_t>& b) {
    double sum = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        sum += std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
    return sum / static_cast<double>(a.size());
}

}  // namespace

TEST(mosh_intra_roundtrip_quality) {
    TestFrame frame(0);
    MoshCodec codec;
    MoshParams params;
    params.quality = 90;
    DecodedFrame out;
    codec.process(frame.view(), 0, params, {}, out);
    CHECK(codec.has_state());
    CHECK_EQ(out.width, kW);
    CHECK(mean_abs_diff(out.y, frame.y) < 3.0);

    MoshCodec crushed;
    MoshParams low = params;
    low.quality = 5;
    DecodedFrame out_low;
    crushed.process(frame.view(), 0, low, {}, out_low);
    CHECK(mean_abs_diff(out_low.y, frame.y) >
          mean_abs_diff(out.y, frame.y));
}

TEST(mosh_p_frame_static_converges) {
    TestFrame frame(0);
    MoshCodec codec;
    MoshParams params;
    params.quality = 80;
    params.gop_length = 30;
    DecodedFrame a, b;
    codec.process(frame.view(), 0, params, {}, a);
    codec.process(frame.view(), 1, params, {}, b);
    CHECK(mean_abs_diff(b.y, frame.y) < 4.0);
}

TEST(mosh_datamosh_drop_holds_stale_reference) {
    TestFrame first(0);
    TestFrame second(3);   // very different content
    MoshParams params;
    params.quality = 80;
    params.gop_length = 1;        // gop 1 makes every frame an I frame
    params.drop_iframes = true;   // a dropped I frame freezes the output

    MoshCodec codec;
    DecodedFrame a, b;
    codec.process(first.view(), 0, params, {}, a);
    codec.process(second.view(), 1, params, {}, b);
    CHECK(b.y == a.y);

    // A later P frame aims at the clean reference, not the frozen output.
    MoshParams melt = params;
    melt.gop_length = 2;
    MoshCodec moshed;
    DecodedFrame m;
    moshed.process(first.view(), 0, melt, {}, m);
    moshed.process(first.view(), 1, melt, {}, m);
    moshed.process(second.view(), 2, melt, {}, m);
    moshed.process(second.view(), 3, melt, {}, m);

    MoshCodec honest;
    MoshParams clean = melt;
    clean.drop_iframes = false;
    DecodedFrame hn;
    honest.process(first.view(), 0, clean, {}, hn);
    honest.process(first.view(), 1, clean, {}, hn);
    honest.process(second.view(), 2, clean, {}, hn);
    honest.process(second.view(), 3, clean, {}, hn);

    CHECK(mean_abs_diff(hn.y, second.y) < 4.0);
    CHECK(mean_abs_diff(m.y, second.y) > 8.0);
    CHECK(mean_abs_diff(m.y, first.y) < mean_abs_diff(m.y, second.y));
}

TEST(mosh_open_loop_scar_persists) {
    // The encoder corrects against its clean reference, so a scar stays.
    TestFrame f0(0), f1(3);
    MoshParams params;
    params.quality = 90;
    params.gop_length = 0;   // no I refresh after the first frame
    MoshCodec codec;
    DecodedFrame a, b, c;
    codec.process(f0.view(), 0, params, {}, a);
    MoshParams wound = params;
    wound.residual_corrupt = 1.0f;   // 1.0 drops all residuals of this frame
    codec.process(f1.view(), 1, wound, {}, b);
    codec.process(f1.view(), 2, params, {}, c);
    CHECK(mean_abs_diff(b.y, f1.y) > 8.0);
    CHECK(mean_abs_diff(c.y, f1.y) > 8.0);
    CHECK(mean_abs_diff(c.y, b.y) < 2.0);
}

TEST(mosh_mv_field_advects) {
    TestFrame frame(0);
    MoshParams params;
    params.quality = 80;
    params.gop_length = 100;
    params.residual_corrupt = 1.0f;   // isolate the prediction path

    // MV units are pixels: this field is +8 px right.
    const uint32_t bw = (kW + 15) / 16;
    const uint32_t bh = (kH + 15) / 16;
    std::vector<float> mx(bw * bh, 8.0f), my(bw * bh, 0.0f);
    MvField mvs{mx.data(), my.data(), bw, bh};

    MoshCodec codec;
    DecodedFrame a, b;
    codec.process(frame.view(), 0, params, {}, a);
    codec.process(frame.view(), 1, params, mvs, b);

    int matches = 0, total = 0;
    for (uint32_t r = 8; r < kH - 8; r += 4) {
        for (uint32_t c = 16; c < kW - 16; c += 4) {
            ++total;
            if (std::abs(static_cast<int>(b.y[r * kW + c]) -
                         static_cast<int>(a.y[r * kW + c - 8])) <= 2)
                ++matches;
        }
    }
    CHECK(matches > total * 3 / 4);

    MoshCodec scaled;
    MoshParams double_mv = params;
    double_mv.mv_scale = 2.0f;
    DecodedFrame c1, c2;
    scaled.process(frame.view(), 0, double_mv, {}, c1);
    scaled.process(frame.view(), 1, double_mv, mvs, c2);
    matches = 0;
    total = 0;
    for (uint32_t r = 8; r < kH - 8; r += 4) {
        for (uint32_t c = 24; c < kW - 24; c += 4) {
            ++total;
            if (std::abs(static_cast<int>(c2.y[r * kW + c]) -
                         static_cast<int>(c1.y[r * kW + c - 16])) <= 2)
                ++matches;
        }
    }
    CHECK(matches > total * 3 / 4);
}

TEST(mosh_generation_loss_matches_repeated_fixed_quality_encoding) {
    TestFrame frame(0);
    MoshParams params;
    params.quality = 35;

    MoshCodec once;
    DecodedFrame a;
    once.process(frame.view(), 0, params, {}, a);

    MoshCodec many;
    MoshParams gen = params;
    gen.generations = 8;
    DecodedFrame b;
    many.process(frame.view(), 0, gen, {}, b);

    DecodedFrame repeated = a;
    for (int pass = 1; pass < 8; ++pass) {
        MoshCodec encoder;
        DecodedFrame next;
        encoder.process(repeated.view(), 0, params, {}, next);
        repeated = std::move(next);
    }
    CHECK(b.y == repeated.y);
    CHECK(b.u == repeated.u);
    CHECK(b.v == repeated.v);
}

TEST(mosh_repeat_reapplies_residual) {
    TestFrame first(0), second(0);
    std::fill(first.y.begin(), first.y.end(), uint8_t{64});
    std::fill(second.y.begin(), second.y.end(), uint8_t{80});
    MoshParams params;
    params.quality = 100;
    params.gop_length = 100;
    params.p_repeat = 2;
    MoshCodec codec;
    DecodedFrame out;
    codec.process(first.view(), 0, params, {}, out);
    codec.process(second.view(), 1, params, {}, out);
    CHECK(std::abs(static_cast<int>(out.y[0]) - 112) <= 2);
    params.p_repeat = 0;
    codec.process(second.view(), 2, params, {}, out);
    CHECK(std::abs(static_cast<int>(out.y[0]) - 112) <= 2);
}

TEST(mosh_bitrate_starvation_caps_quality) {
    TestFrame frame(0);
    MoshParams params;
    params.quality = 95;
    params.bitrate_budget = 400;   // absurdly small for 64x48

    MoshCodec codec;
    DecodedFrame out;
    codec.process(frame.view(), 0, params, {}, out);
    MoshCodec free_codec;
    MoshParams free_params = params;
    free_params.bitrate_budget = 0;
    DecodedFrame free_out;
    free_codec.process(frame.view(), 0, free_params, {}, free_out);
    CHECK(mean_abs_diff(out.y, frame.y) >
          mean_abs_diff(free_out.y, frame.y) * 2.0);
}

TEST(mosh_deterministic) {
    TestFrame f0(0), f1(1), f2(2);
    MoshParams params;
    params.quality = 60;
    params.gop_length = 2;
    params.residual_corrupt = 0.3f;
    params.byte_flips = 4;
    params.mv_random = 3.0f;
    params.seed = 77;

    auto run = [&](std::vector<uint8_t>& out_y) {
        MoshCodec codec;
        DecodedFrame out;
        codec.process(f0.view(), 0, params, {}, out);
        codec.process(f1.view(), 1, params, {}, out);
        // Capture the P frame: an I frame does not use the seed.
        out_y = out.y;
        codec.process(f2.view(), 2, params, {}, out);
    };
    std::vector<uint8_t> a, b;
    run(a);
    run(b);
    CHECK(a == b);

    params.seed = 78;
    std::vector<uint8_t> c;
    run(c);
    CHECK(a != c);
}

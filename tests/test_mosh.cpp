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
    // High quality: close to the source.
    CHECK(mean_abs_diff(out.y, frame.y) < 3.0);

    // Low quality (JPEG Blocking territory): visibly worse but bounded.
    MoshCodec crushed;
    MoshParams low = params;
    low.quality = 5;
    DecodedFrame out_low;
    crushed.process(frame.view(), 0, low, {}, out_low);
    CHECK(mean_abs_diff(out_low.y, frame.y) >
          mean_abs_diff(out.y, frame.y));
}

TEST(mosh_p_frame_static_converges) {
    // Same frame twice: the P frame's residual is small, output stays close.
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

TEST(mosh_datamosh_holds_stale_reference) {
    TestFrame first(0);
    TestFrame second(3);   // very different content
    MoshParams params;
    params.quality = 80;
    params.gop_length = 1;        // every frame wants to be an I frame...
    params.drop_iframes = true;   // ...but the mosh drops them

    MoshCodec codec;
    DecodedFrame a, b;
    codec.process(first.view(), 0, params, {}, a);   // first I establishes
    codec.process(second.view(), 1, params, {}, b);  // dropped I -> P

    // Without the drop, frame 1 would be an I frame of `second`.
    MoshCodec honest;
    MoshParams clean = params;
    clean.drop_iframes = false;
    DecodedFrame c, d;
    honest.process(first.view(), 0, clean, {}, c);
    honest.process(second.view(), 1, clean, {}, d);

    // The moshed frame differs from the honest decode of `second`
    // (residual-only update over a stale reference at P quality).
    CHECK(mean_abs_diff(b.y, d.y) > 0.05);
    // And with residual corruption the stale content bleeds through hard.
    MoshCodec bleeding;
    MoshParams corrupt = params;
    corrupt.residual_corrupt = 1.0f;   // no residuals at all
    DecodedFrame e, f;
    bleeding.process(first.view(), 0, corrupt, {}, e);
    bleeding.process(second.view(), 1, corrupt, {}, f);
    // Frame 1's output is (almost) exactly the frame-0 reference: pure hold.
    CHECK(mean_abs_diff(f.y, e.y) < 0.5);
    CHECK(mean_abs_diff(f.y, second.y) > 8.0);
}

TEST(mosh_mv_field_advects) {
    TestFrame frame(0);
    MoshParams params;
    params.quality = 80;
    params.gop_length = 100;
    params.residual_corrupt = 1.0f;   // isolate the prediction path

    // Constant MV field: +8 px right.
    const uint32_t bw = (kW + 15) / 16;
    const uint32_t bh = (kH + 15) / 16;
    std::vector<float> mx(bw * bh, 8.0f), my(bw * bh, 0.0f);
    MvField mvs{mx.data(), my.data(), bw, bh};

    MoshCodec codec;
    DecodedFrame a, b;
    codec.process(frame.view(), 0, params, {}, a);
    codec.process(frame.view(), 1, params, mvs, b);

    // Row content should have shifted right by ~8 px (interior sample).
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

    // MV scale doubles the shift.
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

TEST(mosh_generation_loss_degrades) {
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

    CHECK(mean_abs_diff(b.y, frame.y) > mean_abs_diff(a.y, frame.y));
}

TEST(mosh_bitrate_starvation_caps_quality) {
    TestFrame frame(0);
    MoshParams params;
    params.quality = 95;
    params.bitrate_budget = 400;   // absurdly small for 64x48

    MoshCodec codec;
    DecodedFrame out;
    codec.process(frame.view(), 0, params, {}, out);
    // Starved output is much worse than unconstrained q95.
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
        // Capture the P frame — that's where the seeded ops live (an I
        // frame is seed-independent by design).
        out_y = out.y;
        codec.process(f2.view(), 2, params, {}, out);
    };
    std::vector<uint8_t> a, b;
    run(a);
    run(b);
    CHECK(a == b);

    // Different seed -> different bytes.
    params.seed = 78;
    std::vector<uint8_t> c;
    run(c);
    CHECK(a != c);
}

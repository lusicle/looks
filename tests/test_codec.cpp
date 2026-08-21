// codec_core + mezzanine tests: bit I/O, DCT roundtrip, frame quality,
// bit-exact determinism, .mez file roundtrip.

#include <cmath>
#include <cstdint>
#include <filesystem>

#include "codec/bitio.h"
#include "codec/core.h"
#include "codec/mez.h"
#include "test_framework.h"

using namespace looks::codec;

namespace {

// Deterministic pseudo-random (counter-hash style, like the engine will use).
uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

// Synthetic I420 frame: gradient + checkerboard + hashed noise.
DecodedFrame make_test_frame(uint32_t w, uint32_t h, uint32_t seed) {
    DecodedFrame f;
    f.width = w;
    f.height = h;
    f.y_stride = w;
    f.uv_stride = (w + 1) / 2;
    const uint32_t cw = (w + 1) / 2;
    const uint32_t ch = (h + 1) / 2;
    f.y.resize(static_cast<size_t>(w) * h);
    f.u.resize(static_cast<size_t>(cw) * ch);
    f.v.resize(static_cast<size_t>(cw) * ch);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t checker = ((x / 8 + y / 8) & 1) ? 40 : 0;
            const uint32_t noise = hash32(seed ^ (y * w + x)) % 17;
            f.y[y * w + x] = static_cast<uint8_t>(
                std::min(255u, 40 + (x * 160) / w + checker + noise));
        }
    }
    for (uint32_t y = 0; y < ch; ++y) {
        for (uint32_t x = 0; x < cw; ++x) {
            f.u[y * cw + x] = static_cast<uint8_t>(96 + (x * 64) / cw);
            f.v[y * cw + x] = static_cast<uint8_t>(160 - (y * 64) / ch);
        }
    }
    return f;
}

double plane_psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0;
    double mse = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double d = static_cast<double>(a[i]) - b[i];
        mse += d * d;
    }
    mse /= static_cast<double>(a.size());
    if (mse <= 1e-9) return 99.0;
    return 10.0 * std::log10(255.0 * 255.0 / mse);
}

}  // namespace

TEST(codec_bitio_roundtrip) {
    std::vector<uint8_t> buf;
    BitWriter bw(buf);
    for (uint32_t v = 0; v < 300; ++v) bw.put_ue(v);
    for (int32_t v = -150; v <= 150; ++v) bw.put_se(v);
    bw.put_bits(0xABC, 12);
    bw.finish();

    BitReader br(buf.data(), buf.size());
    for (uint32_t v = 0; v < 300; ++v) CHECK_EQ(br.get_ue(), v);
    for (int32_t v = -150; v <= 150; ++v) CHECK_EQ(br.get_se(), v);
    CHECK_EQ(br.get_bits(12), 0xABCu);
    CHECK(br.ok());
}

TEST(codec_dct_roundtrip) {
    int16_t block[kBlockCoeffs];
    int16_t original[kBlockCoeffs];
    for (int i = 0; i < kBlockCoeffs; ++i) {
        original[i] = static_cast<int16_t>(
            static_cast<int32_t>(hash32(static_cast<uint32_t>(i) * 7919u) % 256) - 128);
        block[i] = original[i];
    }
    fdct8x8(block);
    idct8x8(block);
    int max_err = 0;
    for (int i = 0; i < kBlockCoeffs; ++i)
        max_err = std::max(max_err, std::abs(block[i] - original[i]));
    CHECK(max_err <= 2);   // 13-bit fixed point, two rounding passes
}

TEST(codec_block_entropy_roundtrip) {
    int16_t block[kBlockCoeffs] = {};
    block[0] = -37;                 // DC
    block[kZigzag[1]] = 12;
    block[kZigzag[5]] = -3;
    block[kZigzag[62]] = 1;         // long zero run
    std::vector<uint8_t> buf;
    BitWriter bw(buf);
    int16_t dc_pred_enc = 0;
    encode_block(bw, block, &dc_pred_enc);
    bw.finish();

    BitReader br(buf.data(), buf.size());
    int16_t decoded[kBlockCoeffs];
    int16_t dc_pred_dec = 0;
    CHECK(decode_block(br, decoded, &dc_pred_dec));
    for (int i = 0; i < kBlockCoeffs; ++i) CHECK_EQ(decoded[i], block[i]);
    CHECK_EQ(dc_pred_dec, dc_pred_enc);
}

TEST(codec_frame_roundtrip_quality) {
    const DecodedFrame src = make_test_frame(128, 96, 1);
    std::vector<uint8_t> encoded;
    encode_frame(src.view(), 90, encoded);
    CHECK(!encoded.empty());
    // Better than uncompressed? (sanity: it should compress a lot)
    CHECK(encoded.size() < src.y.size());

    DecodedFrame out;
    CHECK(decode_frame(encoded.data(), encoded.size(), 128, 96, out));
    CHECK_EQ(out.width, 128u);
    CHECK_EQ(out.y.size(), src.y.size());
    CHECK(plane_psnr(src.y, out.y) > 30.0);
    CHECK(plane_psnr(src.u, out.u) > 32.0);
    CHECK(plane_psnr(src.v, out.v) > 32.0);

    // Lower quality -> smaller stream, still decodable.
    std::vector<uint8_t> low;
    encode_frame(src.view(), 20, low);
    CHECK(low.size() < encoded.size());
    DecodedFrame low_out;
    CHECK(decode_frame(low.data(), low.size(), 128, 96, low_out));
    CHECK(plane_psnr(src.y, low_out.y) > 20.0);
}

TEST(codec_lossless_roundtrip) {
    // Lossless mode: quality 0 must reproduce every byte of
    // every plane, including odd dimensions.
    for (const auto [w, h] : {std::pair{128u, 96u}, std::pair{71u, 53u}}) {
        const DecodedFrame f = make_test_frame(w, h, 5);
        std::vector<uint8_t> encoded;
        encode_frame(f.view(), 0, encoded);
        CHECK(!encoded.empty());
        CHECK_EQ(encoded[0], uint8_t{0});
        DecodedFrame out;
        CHECK(decode_frame(encoded.data(), encoded.size(), w, h, out));
        CHECK(out.y == f.y);
        CHECK(out.u == f.u);
        CHECK(out.v == f.v);
        // The parallel flag must not change lossless output either.
        DecodedFrame out2;
        CHECK(decode_frame(encoded.data(), encoded.size(), w, h, out2,
                           /*parallel=*/true));
        CHECK(out2.y == f.y);
    }
}

TEST(codec_two_phase_intra_matches) {
    // The rate-loop path (DCT once + entropy per quality) must produce the
    // exact bytes of the one-shot encoder — odd size hits flat edge blocks.
    const DecodedFrame f = make_test_frame(70, 50, 9);
    for (const int q : {12, 37, 85}) {
        std::vector<uint8_t> one_shot, two_phase;
        encode_frame(f.view(), q, one_shot);
        IntraDct dct;
        intra_dct(f.view(), dct, /*parallel=*/true);
        intra_entropy(dct, q, two_phase);
        CHECK(one_shot == two_phase);
    }
}

TEST(codec_intra_recon_matches_decode) {
    // The entropy-free wire must reproduce encode+decode EXACTLY: the mosh
    // box's output pixels ride on this equivalence. Odd size hits flat
    // edge blocks.
    const DecodedFrame f = make_test_frame(70, 50, 4);
    for (const int q : {5, 35, 90}) {
        IntraDct dct;
        intra_dct(f.view(), dct, /*parallel=*/true);
        std::vector<uint8_t> encoded;
        intra_entropy(dct, q, encoded);
        DecodedFrame via_stream;
        CHECK(decode_frame(encoded.data(), encoded.size(), 70, 50,
                           via_stream, /*parallel=*/true));
        DecodedFrame direct;
        intra_recon(dct, q, direct, /*parallel=*/true);
        CHECK(direct.y == via_stream.y);
        CHECK(direct.u == via_stream.u);
        CHECK(direct.v == via_stream.v);
    }
}

TEST(codec_intra_entropy_bytes_exact) {
    // The rate probe must count the writer's bytes exactly: quality
    // selection in the starvation loop depends on it, and a one-byte drift
    // would change which quality ships.
    const DecodedFrame f = make_test_frame(70, 50, 8);
    IntraDct dct;
    intra_dct(f.view(), dct, /*parallel=*/true);
    for (const int q : {1, 12, 37, 60, 85, 100}) {
        std::vector<uint8_t> encoded;
        intra_entropy(dct, q, encoded);
        CHECK_EQ(intra_entropy_bytes(dct, q), encoded.size());
    }
}

TEST(codec_frame_odd_dimensions) {
    // 50x34: partial macroblocks on both axes.
    const DecodedFrame src = make_test_frame(50, 34, 2);
    std::vector<uint8_t> encoded;
    encode_frame(src.view(), 85, encoded);
    DecodedFrame out;
    CHECK(decode_frame(encoded.data(), encoded.size(), 50, 34, out));
    CHECK_EQ(out.y.size(), size_t{50 * 34});
    CHECK_EQ(out.u.size(), size_t{25 * 17});
    CHECK(plane_psnr(src.y, out.y) > 28.0);
}

TEST(codec_determinism) {
    const DecodedFrame src = make_test_frame(64, 64, 3);
    std::vector<uint8_t> a, b;
    encode_frame(src.view(), 77, a);
    encode_frame(src.view(), 77, b);
    CHECK(a == b);   // bit-exact

    DecodedFrame da, db;
    CHECK(decode_frame(a.data(), a.size(), 64, 64, da));
    CHECK(decode_frame(a.data(), a.size(), 64, 64, db));
    CHECK(da.y == db.y);
    CHECK(da.u == db.u);
}

TEST(codec_mez_file_roundtrip) {
    const auto path = std::filesystem::temp_directory_path() / "looks_test.mez";
    {
        MezWriter writer;
        CHECK(writer.open(path, 96, 64, 30000, 1001, 88));
        for (uint32_t i = 0; i < 3; ++i) {
            const DecodedFrame f = make_test_frame(96, 64, 10 + i);
            CHECK(writer.add_frame(f.view()));
        }
        CHECK(writer.finish());
        CHECK_EQ(writer.frame_count(), 3u);
    }
    {
        MezReader reader;
        std::string error;
        CHECK(reader.open(path, &error));
        CHECK_EQ(reader.width(), 96u);
        CHECK_EQ(reader.height(), 64u);
        CHECK_EQ(reader.frame_count(), 3u);
        CHECK_EQ(reader.timescale(), 30000u);
        CHECK_EQ(reader.frame_duration(), 1001u);

        // Random access: decode frame 2 first, then 0.
        DecodedFrame f2, f0;
        CHECK(reader.decode(2, f2));
        CHECK(reader.decode(0, f0));
        const DecodedFrame src2 = make_test_frame(96, 64, 12);
        const DecodedFrame src0 = make_test_frame(96, 64, 10);
        CHECK(plane_psnr(src2.y, f2.y) > 30.0);
        CHECK(plane_psnr(src0.y, f0.y) > 30.0);
        CHECK(!reader.decode(3, f0));   // out of range
    }
    std::filesystem::remove(path);
}

TEST(codec_mez_partial_writer_removes_its_file) {
    // An unfinished writer is an aborted import: the destructor must
    // DELETE the partial, never seal it - a sealed partial reads as a
    // valid shorter clip and poisons the bundle cache.
    const auto path =
        std::filesystem::temp_directory_path() / "looks_partial.mez";
    {
        MezWriter writer;
        CHECK(writer.open(path, 96, 64, 30000, 1001, 88));
        CHECK(writer.add_frame(make_test_frame(96, 64, 3).view()));
        // No finish(): simulated cancel/error path.
    }
    CHECK(!std::filesystem::exists(path));

    // A finished writer's file survives and reopens.
    {
        MezWriter writer;
        CHECK(writer.open(path, 96, 64, 30000, 1001, 88));
        CHECK(writer.add_frame(make_test_frame(96, 64, 3).view()));
        CHECK(writer.finish());
    }
    MezReader reader;
    std::string error;
    CHECK(reader.open(path, &error));
    CHECK_EQ(reader.frame_count(), 1u);
    CHECK(reader.fps() > 29.0 && reader.fps() < 30.5);
    reader.close();
    std::filesystem::remove(path);
}

TEST(codec_mez_set_frame_count) {
    const auto path =
        std::filesystem::temp_directory_path() / "looks_test_dur.mez";
    {
        MezWriter writer;
        CHECK(writer.open(path, 96, 64, 30000, 1000, 88));
        CHECK(writer.add_frame(make_test_frame(96, 64, 7).view()));
        CHECK(writer.add_hold_frames(4));   // a 5-frame still
        CHECK(writer.finish());
    }
    const DecodedFrame src = make_test_frame(96, 64, 7);

    // Grow: new entries repeat the last payload.
    CHECK(mez_set_frame_count(path, 9));
    {
        MezReader reader;
        std::string error;
        CHECK(reader.open(path, &error));
        CHECK_EQ(reader.frame_count(), 9u);
        DecodedFrame f;
        CHECK(reader.decode(8, f));
        CHECK(plane_psnr(src.y, f.y) > 30.0);
        CHECK(!reader.decode(9, f));
    }

    // Shrink below the original count.
    CHECK(mez_set_frame_count(path, 3));
    {
        MezReader reader;
        std::string error;
        CHECK(reader.open(path, &error));
        CHECK_EQ(reader.frame_count(), 3u);
        DecodedFrame f;
        CHECK(reader.decode(2, f));
        CHECK(plane_psnr(src.y, f.y) > 30.0);
        CHECK(!reader.decode(3, f));
    }

    CHECK(!mez_set_frame_count(path, 0));   // rejected, file untouched
    {
        MezReader reader;
        std::string error;
        CHECK(reader.open(path, &error));
        CHECK_EQ(reader.frame_count(), 3u);
    }
    std::filesystem::remove(path);
}

TEST(codec_mez_payload_offset_aliases_hold_frames) {
    // Hold-frame entries repeat one payload offset - the contract the
    // decode pool's still alias rides (decode once, serve every frame
    // that points at the same payload). Distinct frames get distinct
    // offsets.
    const auto path =
        std::filesystem::temp_directory_path() / "looks_test_alias.mez";
    {
        MezWriter writer;
        CHECK(writer.open(path, 96, 64, 30000, 1000, 88));
        CHECK(writer.add_frame(make_test_frame(96, 64, 7).view()));
        CHECK(writer.add_hold_frames(3));
        CHECK(writer.add_frame(make_test_frame(96, 64, 9).view()));
        CHECK(writer.finish());
    }
    MezReader reader;
    std::string error;
    CHECK(reader.open(path, &error));
    CHECK_EQ(reader.frame_count(), 5u);
    const uint64_t held = reader.payload_offset(0);
    CHECK(held != 0);
    CHECK_EQ(reader.payload_offset(1), held);
    CHECK_EQ(reader.payload_offset(3), held);
    CHECK(reader.payload_offset(4) != held);
    CHECK_EQ(reader.payload_offset(5), uint64_t{0});   // out of range
    reader.close();
    std::filesystem::remove(path);
}

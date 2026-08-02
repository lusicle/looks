#include "codec/core.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace looks::codec {

const uint8_t kZigzag[kBlockCoeffs] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// ITU T.81 Annex K example tables (the de-facto JPEG baseline).
const uint8_t kQuantBaseLuma[kBlockCoeffs] = {
    16, 11, 10, 16, 24,  40,  51,  61,
    12, 12, 14, 19, 26,  58,  60,  55,
    14, 13, 16, 24, 40,  57,  69,  56,
    14, 17, 22, 29, 51,  87,  80,  62,
    18, 22, 37, 56, 68,  109, 103, 77,
    24, 35, 55, 64, 81,  104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103, 99};

const uint8_t kQuantBaseChroma[kBlockCoeffs] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99};

void build_quant_table(const uint8_t* base, int quality,
                       uint16_t out[kBlockCoeffs]) {
    quality = std::clamp(quality, 1, 100);
    const int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    for (int i = 0; i < kBlockCoeffs; ++i) {
        const int q = (base[i] * scale + 50) / 100;
        out[i] = static_cast<uint16_t>(std::clamp(q, 1, 255));
    }
}

// ---------------------------------------------------------------- DCT
//
// Orthonormal 8-point DCT-II as a fixed-point matrix product: 13-bit
// coefficients, int32 accumulators, symmetric rounding per pass. Chosen for
// exact cross-platform determinism and clarity over speed; SIMD/AAN can
// replace the inner loops later without changing the bitstream (encoder and
// decoder always ship in the same binary).

namespace {

// C[u][x] = c(u)·cos((2x+1)uπ/16) rounded to 13-bit fixed point. HARDCODED
// (not computed via cos at init) so the bitstream is bit-exact across
// platforms/CRTs — a last-ulp libm difference must never change a frame.
constexpr int32_t kDctFwd[kBlockSize][kBlockSize] = {
    {2896, 2896, 2896, 2896, 2896, 2896, 2896, 2896},
    {4017, 3406, 2276, 799, -799, -2276, -3406, -4017},
    {3784, 1567, -1567, -3784, -3784, -1567, 1567, 3784},
    {3406, -799, -4017, -2276, 2276, 4017, 799, -3406},
    {2896, -2896, -2896, 2896, 2896, -2896, -2896, 2896},
    {2276, -4017, 799, 3406, -3406, -799, 4017, -2276},
    {1567, -3784, 3784, -1567, -1567, 3784, -3784, 1567},
    {799, -2276, 3406, -4017, 4017, -3406, 2276, -799}};

struct DctTables {
    int32_t fwd[kBlockSize][kBlockSize];
    int32_t inv[kBlockSize][kBlockSize];   // transpose
    constexpr DctTables() : fwd{}, inv{} {
        for (int u = 0; u < kBlockSize; ++u) {
            for (int x = 0; x < kBlockSize; ++x) {
                fwd[u][x] = kDctFwd[u][x];
                inv[x][u] = kDctFwd[u][x];
            }
        }
    }
};

constexpr DctTables kDct;

void transform_pass(const int32_t mat[kBlockSize][kBlockSize],
                    const int16_t* in, int16_t* out, bool rows) {
    // rows=true: out[u][k] = sum_x mat[u][x] * in[x][k] (column transform of
    // row-major data when applied twice with transposes folded in).
    for (int u = 0; u < kBlockSize; ++u) {
        for (int k = 0; k < kBlockSize; ++k) {
            int32_t acc = 0;
            for (int x = 0; x < kBlockSize; ++x) {
                const int16_t v = rows ? in[u * kBlockSize + x]
                                       : in[x * kBlockSize + k];
                const int32_t c = rows ? mat[k][x] : mat[u][x];
                acc += c * v;
            }
            const int32_t r = (acc + 4096) >> 13;
            out[rows ? u * kBlockSize + k : u * kBlockSize + k] =
                static_cast<int16_t>(std::clamp(r, -32768, 32767));
        }
    }
}

}  // namespace

void fdct8x8(int16_t block[kBlockCoeffs]) {
    int16_t tmp[kBlockCoeffs];
    // Row pass: each row transformed by C (tmp = f · C^T).
    transform_pass(kDct.fwd, block, tmp, true);
    // Column pass: F = C · tmp.
    transform_pass(kDct.fwd, tmp, block, false);
}

void idct8x8(int16_t block[kBlockCoeffs]) {
    int16_t tmp[kBlockCoeffs];
    transform_pass(kDct.inv, block, tmp, true);
    transform_pass(kDct.inv, tmp, block, false);
}

// ---------------------------------------------------------------- quant

void quantize(const int16_t in[kBlockCoeffs], const uint16_t qtab[kBlockCoeffs],
              int16_t out[kBlockCoeffs]) {
    for (int i = 0; i < kBlockCoeffs; ++i) {
        const int32_t v = in[i];
        const int32_t q = qtab[i];
        const int32_t mag = (std::abs(v) + q / 2) / q;
        out[i] = static_cast<int16_t>(v < 0 ? -mag : mag);
    }
}

void dequantize(const int16_t in[kBlockCoeffs], const uint16_t qtab[kBlockCoeffs],
                int16_t out[kBlockCoeffs]) {
    for (int i = 0; i < kBlockCoeffs; ++i)
        out[i] = static_cast<int16_t>(
            std::clamp(in[i] * qtab[i], -32768, 32767));
}

// ---------------------------------------------------------------- entropy

namespace {
constexpr uint32_t kEobRun = 63;   // run sentinel: no more nonzero coeffs
}

void encode_block(BitWriter& bw, const int16_t block[kBlockCoeffs],
                  int16_t* dc_pred) {
    bw.put_se(block[0] - *dc_pred);
    *dc_pred = block[0];

    int last_nonzero = 0;
    for (int i = 1; i < kBlockCoeffs; ++i)
        if (block[kZigzag[i]] != 0) last_nonzero = i;

    int run = 0;
    for (int i = 1; i <= last_nonzero; ++i) {
        const int16_t v = block[kZigzag[i]];
        if (v == 0) {
            ++run;
            continue;
        }
        bw.put_ue(static_cast<uint32_t>(run));
        const uint32_t mag = static_cast<uint32_t>(std::abs(v));
        bw.put_ue(mag - 1);
        bw.put_bit(v < 0 ? 1u : 0u);
        run = 0;
    }
    bw.put_ue(kEobRun);
}

uint32_t ue_bit_count(uint32_t v) {
    const uint32_t x = v + 1;
    int bits = 0;
    while ((x >> bits) > 1) ++bits;
    return static_cast<uint32_t>(2 * bits + 1);
}

uint32_t se_bit_count(int32_t v) {
    return ue_bit_count(v > 0 ? static_cast<uint32_t>(v) * 2 - 1
                              : static_cast<uint32_t>(-v) * 2);
}

uint32_t ac_bit_count(const int16_t block[kBlockCoeffs]) {
    int last_nonzero = 0;
    for (int i = 1; i < kBlockCoeffs; ++i)
        if (block[kZigzag[i]] != 0) last_nonzero = i;
    uint32_t bits = 0;
    int run = 0;
    for (int i = 1; i <= last_nonzero; ++i) {
        const int16_t v = block[kZigzag[i]];
        if (v == 0) {
            ++run;
            continue;
        }
        bits += ue_bit_count(static_cast<uint32_t>(run));
        bits += ue_bit_count(static_cast<uint32_t>(std::abs(v)) - 1) + 1;
        run = 0;
    }
    return bits + ue_bit_count(kEobRun);
}

bool decode_block(BitReader& br, int16_t block[kBlockCoeffs], int16_t* dc_pred) {
    std::memset(block, 0, sizeof(int16_t) * kBlockCoeffs);
    const int32_t dc = *dc_pred + br.get_se();
    block[0] = static_cast<int16_t>(dc);
    *dc_pred = static_cast<int16_t>(dc);

    int i = 1;
    for (;;) {
        const uint32_t run = br.get_ue();
        if (!br.ok()) return false;
        if (run == kEobRun) break;
        i += static_cast<int>(run);
        if (i >= kBlockCoeffs) return false;
        const uint32_t mag = br.get_ue() + 1;
        const uint32_t sign = br.get_bit();
        if (!br.ok() || mag > 32767) return false;
        block[kZigzag[i]] =
            static_cast<int16_t>(sign ? -static_cast<int32_t>(mag) : mag);
        ++i;
    }
    return true;
}

// ------------------------------------------------------- pixel pipeline

void encode_pixel_block(BitWriter& bw, const uint8_t* src, size_t stride,
                        int avail_w, int avail_h,
                        const uint16_t qtab[kBlockCoeffs], int16_t* dc_pred) {
    int16_t block[kBlockCoeffs];
    extract_dct_block(src, stride, avail_w, avail_h, block);
    int16_t quantized[kBlockCoeffs];
    quantize(block, qtab, quantized);
    encode_block(bw, quantized, dc_pred);
}

void extract_dct_block(const uint8_t* src, size_t stride, int avail_w,
                       int avail_h, int16_t out[kBlockCoeffs]) {
    // Extract with edge replication for partial edge blocks.
    for (int y = 0; y < kBlockSize; ++y) {
        const int sy = std::min(y, avail_h - 1);
        for (int x = 0; x < kBlockSize; ++x) {
            const int sx = std::min(x, avail_w - 1);
            out[y * kBlockSize + x] =
                static_cast<int16_t>(src[sy * stride + sx] - 128);
        }
    }
    fdct8x8(out);
}

void residual_dct_block(const uint8_t* cur, size_t cur_stride,
                        const uint8_t* pred, size_t pred_stride, int avail_w,
                        int avail_h, int16_t out[kBlockCoeffs]) {
    for (int y = 0; y < kBlockSize; ++y) {
        const int cy = std::min(y, avail_h - 1);
        for (int x = 0; x < kBlockSize; ++x) {
            const int cx = std::min(x, avail_w - 1);
            out[y * kBlockSize + x] = static_cast<int16_t>(
                static_cast<int>(cur[cy * cur_stride + cx]) -
                static_cast<int>(pred[cy * pred_stride + cx]));
        }
    }
    fdct8x8(out);
}

void parallel_blocks(int count, bool parallel,
                     const std::function<void(int, int)>& fn) {
    if (count <= 0) return;
    const int threads = parallel
        ? static_cast<int>(std::min<unsigned>(
              std::max(1u, std::thread::hardware_concurrency()), 16u))
        : 1;
    if (threads <= 1 || count < threads * 4) {
        fn(0, count);
        return;
    }
    const int chunk = (count + threads - 1) / threads;
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(threads) - 1);
    for (int t = 1; t < threads; ++t) {
        const int begin = t * chunk;
        const int end = std::min(count, begin + chunk);
        if (begin >= end) break;
        pool.emplace_back([&fn, begin, end] { fn(begin, end); });
    }
    fn(0, std::min(count, chunk));
    for (std::thread& t : pool) t.join();
}

bool decode_pixel_block(BitReader& br, uint8_t* dst, size_t stride,
                        int avail_w, int avail_h,
                        const uint16_t qtab[kBlockCoeffs], int16_t* dc_pred) {
    int16_t quantized[kBlockCoeffs];
    if (!decode_block(br, quantized, dc_pred)) return false;
    int16_t block[kBlockCoeffs];
    dequantize(quantized, qtab, block);
    idct8x8(block);
    const int copy_w = std::min(kBlockSize, avail_w);
    const int copy_h = std::min(kBlockSize, avail_h);
    for (int y = 0; y < copy_h; ++y) {
        for (int x = 0; x < copy_w; ++x) {
            const int v = block[y * kBlockSize + x] + 128;
            dst[y * stride + x] = static_cast<uint8_t>(std::clamp(v, 0, 255));
        }
    }
    return true;
}

}  // namespace looks::codec

// codec_core — transform/quant/entropy shared by the mezzanine codec and
// the Codec-Box mosh codec (one core, two wrappers).
//
// - 8x8 fixed-point DCT-II / inverse (13-bit coefficients, int32
//   accumulators) — bit-exact across platforms, no float in the loop.
// - JPEG-style quant tables (ITU T.81 Annex K examples) scaled by a 1..100
//   quality knob.
// - Block entropy coding: DC delta prediction + (run, level) exp-Golomb
//   over the zigzag scan. Table-free, deterministic, shared with mosh.

#pragma once

#include <cstdint>
#include <functional>

#include "codec/bitio.h"

namespace looks::codec {

inline constexpr int kBlockSize = 8;
inline constexpr int kBlockCoeffs = 64;

extern const uint8_t kZigzag[kBlockCoeffs];
extern const uint8_t kQuantBaseLuma[kBlockCoeffs];     // T.81 Annex K
extern const uint8_t kQuantBaseChroma[kBlockCoeffs];

// Builds a scaled quant table for quality 1..100 (values clamped to 1..255).
void build_quant_table(const uint8_t* base, int quality, uint16_t out[kBlockCoeffs]);

// Forward DCT on a centered block (input range ~[-128, 127] as int16).
// In-place: block is 64 coefficients, row-major. Output range fits int16.
void fdct8x8(int16_t block[kBlockCoeffs]);
void idct8x8(int16_t block[kBlockCoeffs]);

// Quantize/dequantize one transformed block.
void quantize(const int16_t in[kBlockCoeffs], const uint16_t qtab[kBlockCoeffs],
              int16_t out[kBlockCoeffs]);
void dequantize(const int16_t in[kBlockCoeffs], const uint16_t qtab[kBlockCoeffs],
                int16_t out[kBlockCoeffs]);

// Entropy: writes/reads one quantized block. `dc_pred` is the running DC
// predictor for the component (updated in place).
void encode_block(BitWriter& bw, const int16_t block[kBlockCoeffs],
                  int16_t* dc_pred);
bool decode_block(BitReader& br, int16_t block[kBlockCoeffs], int16_t* dc_pred);

// Exact code lengths of the entropy layer, with no bitstream — rate loops
// pick quality from these counts, so they MUST track the writers bit for
// bit. ac_bit_count covers everything after the DC delta code: the
// (run, level) scan plus the end-of-block marker.
uint32_t ue_bit_count(uint32_t v);
uint32_t se_bit_count(int32_t v);
uint32_t ac_bit_count(const int16_t block[kBlockCoeffs]);

// Full pixel-block pipeline used by both wrappers: extract (with edge
// replication), center, transform, quantize, entropy — and the reverse.
// `src` points at the top-left of the block within a plane.
void encode_pixel_block(BitWriter& bw, const uint8_t* src, size_t stride,
                        int avail_w, int avail_h,
                        const uint16_t qtab[kBlockCoeffs], int16_t* dc_pred);
bool decode_pixel_block(BitReader& br, uint8_t* dst, size_t stride,
                        int avail_w, int avail_h,
                        const uint16_t qtab[kBlockCoeffs], int16_t* dc_pred);

// Transform-only halves (two-phase encoders, ): the DCT of a
// source is quality-independent, so rate-control loops transform once and
// re-run only quantize+entropy per quality step. Both produce the exact
// coefficients the one-shot pipeline would.
void extract_dct_block(const uint8_t* src, size_t stride, int avail_w,
                       int avail_h, int16_t out[kBlockCoeffs]);
void residual_dct_block(const uint8_t* cur, size_t cur_stride,
                        const uint8_t* pred, size_t pred_stride, int avail_w,
                        int avail_h, int16_t out[kBlockCoeffs]);

// Deterministic parallel-for over [0, count): chunks run on hardware
// threads and must write disjoint data. `parallel` false = inline loop
// (callers already parallel at a higher level, e.g. import).
void parallel_blocks(int count, bool parallel,
                     const std::function<void(int, int)>& fn);

}  // namespace looks::codec

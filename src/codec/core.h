// Fixed-point math only: output is bit-exact on all platforms.

#pragma once

#include <cstdint>
#include <functional>

#include "codec/bitio.h"

namespace looks::codec {

inline constexpr int kBlockSize = 8;
inline constexpr int kBlockCoeffs = 64;

extern const uint8_t kZigzag[kBlockCoeffs];
extern const uint8_t kQuantBaseLuma[kBlockCoeffs];
extern const uint8_t kQuantBaseChroma[kBlockCoeffs];

// quality range is 1..100.
void build_quant_table(const uint8_t* base, int quality, uint16_t out[kBlockCoeffs]);

// In-place, row-major. Input is centered near [-128, 127]; output fits int16.
void fdct8x8(int16_t block[kBlockCoeffs]);
void idct8x8(int16_t block[kBlockCoeffs]);

void quantize(const int16_t in[kBlockCoeffs], const uint16_t qtab[kBlockCoeffs],
              int16_t out[kBlockCoeffs]);
void dequantize(const int16_t in[kBlockCoeffs], const uint16_t qtab[kBlockCoeffs],
                int16_t out[kBlockCoeffs]);

// dc_pred is the running per-component DC predictor; updated in place.
void encode_block(BitWriter& bw, const int16_t block[kBlockCoeffs],
                  int16_t* dc_pred);
bool decode_block(BitReader& br, int16_t block[kBlockCoeffs], int16_t* dc_pred);

// These counts must match the writers bit for bit.
// ac_bit_count covers all bits after the DC delta code.
uint32_t ue_bit_count(uint32_t v);
uint32_t se_bit_count(int32_t v);
uint32_t ac_bit_count(const int16_t block[kBlockCoeffs]);

// src points at the top-left pixel of the block, not the plane origin.
void encode_pixel_block(BitWriter& bw, const uint8_t* src, size_t stride,
                        int avail_w, int avail_h,
                        const uint16_t qtab[kBlockCoeffs], int16_t* dc_pred);
bool decode_pixel_block(BitReader& br, uint8_t* dst, size_t stride,
                        int avail_w, int avail_h,
                        const uint16_t qtab[kBlockCoeffs], int16_t* dc_pred);

// Must produce the same coefficients as the one-shot pipeline.
void extract_dct_block(const uint8_t* src, size_t stride, int avail_w,
                       int avail_h, int16_t out[kBlockCoeffs]);
void residual_dct_block(const uint8_t* cur, size_t cur_stride,
                        const uint8_t* pred, size_t pred_stride, int avail_w,
                        int avail_h, int16_t out[kBlockCoeffs]);

// Chunks run on worker threads; fn must write disjoint data.
void parallel_blocks(int count, bool parallel,
                     const std::function<void(int, int)>& fn);

// Always one pool task per index; no serial fallback by intent.
void parallel_tasks(int count, const std::function<void(int)>& fn);

}  // namespace looks::codec

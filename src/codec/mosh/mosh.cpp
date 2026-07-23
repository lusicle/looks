#include "codec/mosh/mosh.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "codec/core.h"
#include "util/hash.h"

namespace looks::codec {

namespace {

void alloc_frame(DecodedFrame& f, uint32_t width, uint32_t height) {
    const uint32_t cw = (width + 1) / 2;
    const uint32_t ch = (height + 1) / 2;
    f.width = width;
    f.height = height;
    f.y_stride = width;
    f.uv_stride = cw;
    f.y.assign(static_cast<size_t>(width) * height, 16);
    f.u.assign(static_cast<size_t>(cw) * ch, 128);
    f.v.assign(static_cast<size_t>(cw) * ch, 128);
}

// Motion-compensated block copy with edge clamping (full-pel).
void copy_block(const uint8_t* src, size_t src_stride, int src_w, int src_h,
                int sx, int sy, uint8_t* dst, size_t dst_stride, int dx,
                int dy, int bw, int bh) {
    for (int y = 0; y < bh; ++y) {
        const int cy = std::clamp(sy + y, 0, src_h - 1);
        uint8_t* out_row = dst + static_cast<size_t>(dy + y) * dst_stride + dx;
        const uint8_t* in_row = src + static_cast<size_t>(cy) * src_stride;
        for (int x = 0; x < bw; ++x) {
            const int cx = std::clamp(sx + x, 0, src_w - 1);
            out_row[x] = in_row[cx];
        }
    }
}

// Encode the residual (in - pred) of one 8x8 region into the bitstream.
void encode_residual_block(BitWriter& bw, const uint8_t* cur, size_t cur_stride,
                           const uint8_t* pred, size_t pred_stride, int avail_w,
                           int avail_h, const uint16_t qtab[kBlockCoeffs],
                           int16_t* dc_pred) {
    int16_t block[kBlockCoeffs];
    residual_dct_block(cur, cur_stride, pred, pred_stride, avail_w, avail_h,
                       block);
    int16_t quantized[kBlockCoeffs];
    quantize(block, qtab, quantized);
    encode_block(bw, quantized, dc_pred);
}

// Decode one residual block and add it onto the prediction in place.
// Returns false when the bitstream desyncs (corruption) — caller keeps the
// bare prediction from then on (no error resets: that's the aesthetic).
bool decode_residual_block(BitReader& br, uint8_t* dst, size_t dst_stride,
                           int avail_w, int avail_h,
                           const uint16_t qtab[kBlockCoeffs],
                           int16_t* dc_pred) {
    int16_t quantized[kBlockCoeffs];
    if (!decode_block(br, quantized, dc_pred)) return false;
    int16_t block[kBlockCoeffs];
    dequantize(quantized, qtab, block);
    idct8x8(block);
    const int bw = std::min(kBlockSize, avail_w);
    const int bh = std::min(kBlockSize, avail_h);
    for (int y = 0; y < bh; ++y) {
        uint8_t* row = dst + static_cast<size_t>(y) * dst_stride;
        for (int x = 0; x < bw; ++x) {
            const int v = static_cast<int>(row[x]) + block[y * kBlockSize + x];
            row[x] = static_cast<uint8_t>(std::clamp(v, 0, 255));
        }
    }
    return true;
}

}  // namespace

void MoshCodec::reset() {
    has_state_ = false;
}

void MoshCodec::encode_decode_intra(const FrameView& in, int quality,
                                    DecodedFrame& out) {
    // Two-phase + threaded transform halves; bytes and pixels identical to
    // encode_frame + serial decode_frame. (in may alias out — the DCT pass
    // consumes it fully before decode writes.)
    intra_dct(in, intra_scratch_, /*parallel=*/true);
    intra_entropy(intra_scratch_, std::clamp(quality, 1, 100), bitstream_);
    decode_frame(bitstream_.data(), bitstream_.size(), in.width, in.height,
                 out, /*parallel=*/true);
}

void MoshCodec::process(const FrameView& in, uint32_t frame_index,
                        const MoshParams& params, const MvField& mvs,
                        DecodedFrame& out) {
    const uint32_t w = in.width;
    const uint32_t h = in.height;
    if (has_state_ && (state_.width != w || state_.height != h))
        has_state_ = false;

    const bool gop_i = params.gop_length <= 0
        ? !has_state_
        : frame_index % static_cast<uint32_t>(params.gop_length) == 0;
    const bool intra = !has_state_ || (gop_i && !params.drop_iframes);

    if (intra) {
        int quality = std::clamp(params.quality, 1, 100);
        // Two-phase (spec §6.3 rate loop): DCT once (parallel), then only
        // quantize+entropy per quality step; decode once at the end. The
        // bitstream is byte-identical to the one-shot encoder's.
        intra_dct(in, intra_scratch_, /*parallel=*/true);
        intra_entropy(intra_scratch_, quality, bitstream_);
        while (params.bitrate_budget > 0 &&
               bitstream_.size() > params.bitrate_budget && quality > 1) {
            quality = std::max(1, quality - 15);
            intra_entropy(intra_scratch_, quality, bitstream_);
        }
        decode_frame(bitstream_.data(), bitstream_.size(), w, h, out,
                     /*parallel=*/true);
        // Generation loss: run the wire again N times. The integer pipeline
        // is idempotent at a fixed quality, so alternate the quantizer a
        // notch between passes — like every real dub chain, no two
        // generations quantize identically, and the error accumulates.
        for (int g = 0; g < std::min(params.generations, 12); ++g) {
            const int gq = std::clamp(
                params.quality - ((g & 1) ? 9 : 0), 1, 100);
            encode_decode_intra(out.view(), gq, out);
        }
        state_ = out;
        has_state_ = true;
        return;
    }

    // ---- P frame: motion-compensated prediction from the persistent state.
    DecodedFrame pred;
    predict_from_state(frame_index, params, mvs, pred);
    // Bloom: re-apply the motion field to the state N extra times.
    for (int r = 0; r < std::min(params.p_repeat, 8); ++r) {
        state_ = pred;
        predict_from_state(frame_index, params, mvs, pred);
    }

    // Residual encode against the prediction.
    uint16_t qy[kBlockCoeffs], qc[kBlockCoeffs];
    int quality = std::clamp(params.quality, 1, 100);
    const uint32_t cw = (w + 1) / 2;
    const uint32_t ch = (h + 1) / 2;
    const int mb_w = static_cast<int>((w + 15) / 16);
    const int mb_h = static_cast<int>((h + 15) / 16);
    const uint64_t frame_seed =
        hash_combine(hash_combine(params.seed, 0x9E3779B9u), frame_index);

    // Two-phase residual encode: the residual DCT is quality-independent,
    // so transform every block once (parallel across MBs), then the rate
    // loop re-runs only quantize+entropy. Bitstream identical to the
    // one-shot walk.
    const int mb_count = mb_w * mb_h;
    p_coeffs_.resize(static_cast<size_t>(mb_count) * 6 * kBlockCoeffs);
    p_zero_.assign(static_cast<size_t>(mb_count) * 6, 0);
    parallel_blocks(mb_count, true, [&](int begin, int end) {
        for (int mb = begin; mb < end; ++mb) {
            const int mx = mb % mb_w;
            const int my = mb / mb_w;
            const size_t base = static_cast<size_t>(mb) * 6;
            // Residual corruption: seeded per-MB, skip the residual
            // entirely (prediction-only hole).
            const bool corrupt =
                params.residual_corrupt > 0.0f &&
                hash_float01(frame_seed,
                             static_cast<uint64_t>(my) * 4096 + mx) <
                    params.residual_corrupt;
            for (int b = 0; b < 4; ++b) {
                const int px = mx * 16 + (b & 1) * 8;
                const int py = my * 16 + (b >> 1) * 8;
                if (px >= static_cast<int>(w) || py >= static_cast<int>(h) ||
                    corrupt) {
                    p_zero_[base + b] = 1;
                    continue;
                }
                residual_dct_block(
                    in.y.data + py * in.y.stride + px, in.y.stride,
                    pred.y.data() + static_cast<size_t>(py) * pred.y_stride +
                        px,
                    pred.y_stride, static_cast<int>(w) - px,
                    static_cast<int>(h) - py,
                    p_coeffs_.data() + (base + b) * kBlockCoeffs);
            }
            const int cx = mx * 8;
            const int cy = my * 8;
            if (cx >= static_cast<int>(cw) || cy >= static_cast<int>(ch) ||
                corrupt) {
                p_zero_[base + 4] = 1;
                p_zero_[base + 5] = 1;
            } else {
                residual_dct_block(
                    in.u.data + cy * in.u.stride + cx, in.u.stride,
                    pred.u.data() + static_cast<size_t>(cy) * pred.uv_stride +
                        cx,
                    pred.uv_stride, static_cast<int>(cw) - cx,
                    static_cast<int>(ch) - cy,
                    p_coeffs_.data() + (base + 4) * kBlockCoeffs);
                residual_dct_block(
                    in.v.data + cy * in.v.stride + cx, in.v.stride,
                    pred.v.data() + static_cast<size_t>(cy) * pred.uv_stride +
                        cx,
                    pred.uv_stride, static_cast<int>(cw) - cx,
                    static_cast<int>(ch) - cy,
                    p_coeffs_.data() + (base + 5) * kBlockCoeffs);
            }
        }
    });

    for (;;) {
        build_quant_table(kQuantBaseLuma, quality, qy);
        build_quant_table(kQuantBaseChroma, quality, qc);
        bitstream_.clear();
        BitWriter bw(bitstream_);
        int16_t dc_y = 0, dc_u = 0, dc_v = 0;
        int16_t quantized[kBlockCoeffs];
        const int16_t zero[kBlockCoeffs] = {};
        for (int mb = 0; mb < mb_count; ++mb) {
            const size_t base = static_cast<size_t>(mb) * 6;
            for (int b = 0; b < 6; ++b) {
                int16_t* dc = b < 4 ? &dc_y : (b == 4 ? &dc_u : &dc_v);
                if (p_zero_[base + b]) {
                    encode_block(bw, zero, dc);
                    continue;
                }
                quantize(p_coeffs_.data() + (base + b) * kBlockCoeffs,
                         b < 4 ? qy : qc, quantized);
                encode_block(bw, quantized, dc);
            }
        }
        bw.finish();
        if (params.bitrate_budget == 0 ||
            bitstream_.size() <= params.bitrate_budget || quality <= 1)
            break;
        quality = std::max(1, quality - 15);
    }

    // Byte corruption: structured seeded bit flips inside the P bitstream.
    for (uint32_t i = 0; i < params.byte_flips && !bitstream_.empty(); ++i) {
        const uint64_t hf = hash_combine(frame_seed, 0xB17F00Du + i);
        const size_t byte = static_cast<size_t>(
            hf % static_cast<uint64_t>(bitstream_.size()));
        bitstream_[byte] ^= static_cast<uint8_t>(1u << (hash_u64(hf) & 7));
    }

    // ---- decode side: prediction + residual, tolerating desync. Entropy
    // parses serially (reusing the encode coeff buffer); reconstruction —
    // dequant + IDCT + add onto the prediction — runs across threads.
    // Desync mid-stream leaves every later block on bare prediction,
    // exactly like the serial walk (that's the aesthetic, spec §6.3).
    out = pred;
    {
        BitReader br(bitstream_.data(), bitstream_.size());
        int16_t dc_y = 0, dc_u = 0, dc_v = 0;
        int parsed = 0;
        for (int mb = 0; mb < mb_count && parsed == mb * 6; ++mb) {
            const size_t base = static_cast<size_t>(mb) * 6;
            for (int b = 0; b < 6; ++b) {
                int16_t* dc = b < 4 ? &dc_y : (b == 4 ? &dc_u : &dc_v);
                if (!decode_block(br,
                                  p_coeffs_.data() + (base + b) * kBlockCoeffs,
                                  dc))
                    break;
                ++parsed;
            }
        }
        const int done = parsed;
        parallel_blocks(mb_count, true, [&](int begin, int end) {
            int16_t block[kBlockCoeffs];
            for (int mb = begin; mb < end; ++mb) {
                const int mx = mb % mb_w;
                const int my = mb / mb_w;
                const size_t base = static_cast<size_t>(mb) * 6;
                for (int b = 0; b < 6; ++b) {
                    if (static_cast<int>(base) + b >= done) break;
                    const bool luma = b < 4;
                    const int px = luma ? mx * 16 + (b & 1) * 8 : mx * 8;
                    const int py = luma ? my * 16 + (b >> 1) * 8 : my * 8;
                    const int pw =
                        luma ? static_cast<int>(w) : static_cast<int>(cw);
                    const int ph =
                        luma ? static_cast<int>(h) : static_cast<int>(ch);
                    if (px >= pw || py >= ph) continue;   // parsed filler
                    dequantize(p_coeffs_.data() + (base + b) * kBlockCoeffs,
                               luma ? qy : qc, block);
                    idct8x8(block);
                    uint8_t* plane = luma
                        ? out.y.data()
                        : (b == 4 ? out.u.data() : out.v.data());
                    const size_t stride =
                        luma ? out.y_stride : out.uv_stride;
                    uint8_t* dst =
                        plane + static_cast<size_t>(py) * stride + px;
                    const int bw2 = std::min(kBlockSize, pw - px);
                    const int bh2 = std::min(kBlockSize, ph - py);
                    for (int y = 0; y < bh2; ++y) {
                        uint8_t* row = dst + static_cast<size_t>(y) * stride;
                        for (int x = 0; x < bw2; ++x) {
                            const int val = static_cast<int>(row[x]) +
                                            block[y * kBlockSize + x];
                            row[x] = static_cast<uint8_t>(
                                std::clamp(val, 0, 255));
                        }
                    }
                }
            }
        });
    }

    state_ = out;
    has_state_ = true;
}

void MoshCodec::predict_from_state(uint32_t frame_index,
                                   const MoshParams& params, const MvField& mvs,
                                   DecodedFrame& out) const {
    const uint32_t w = state_.width;
    const uint32_t h = state_.height;
    const uint32_t cw = (w + 1) / 2;
    const uint32_t ch = (h + 1) / 2;
    const int mb_w = static_cast<int>((w + 15) / 16);
    const int mb_h = static_cast<int>((h + 15) / 16);
    alloc_frame(out, w, h);

    const float cr = std::cos(params.mv_rotate);
    const float sr = std::sin(params.mv_rotate);
    const uint64_t mv_seed = hash_combine(params.seed, 0x33CC33CCu);

    for (int my = 0; my < mb_h; ++my) {
        for (int mx = 0; mx < mb_w; ++mx) {
            float vx = 0.0f, vy = 0.0f;
            if (mvs.mx && mvs.my && static_cast<uint32_t>(mx) < mvs.blocks_w &&
                static_cast<uint32_t>(my) < mvs.blocks_h) {
                vx = mvs.mx[my * static_cast<int>(mvs.blocks_w) + mx];
                vy = mvs.my[my * static_cast<int>(mvs.blocks_w) + mx];
            }
            // Replace-with-custom-field (spec §6.3): synthetic MV fields
            // swap in for the flow-supplied vectors; the mangling ops
            // below still apply, so rotate steers the pan and scale
            // amplifies the whole field.
            if (params.mv_field != 0) {
                const float px = (mx + 0.5f) / mb_w - 0.5f;
                const float py = (my + 0.5f) / mb_h - 0.5f;
                const float amt = params.mv_field_amount;
                switch (params.mv_field) {
                    case 1:               // pan
                        vx = amt;
                        vy = 0.0f;
                        break;
                    case 2:               // zoom (radial from center)
                        vx = px * 2.0f * amt;
                        vy = py * 2.0f * amt;
                        break;
                    default:              // swirl (perpendicular)
                        vx = -py * 2.0f * amt;
                        vy = px * 2.0f * amt;
                        break;
                }
            }
            // MV mangling: scale, rotate, seeded randomization.
            const float rx = vx * cr - vy * sr;
            const float ry = vx * sr + vy * cr;
            vx = rx * params.mv_scale;
            vy = ry * params.mv_scale;
            if (params.mv_random > 0.0f) {
                const uint64_t hb = hash_combine(
                    hash_combine(mv_seed, frame_index),
                    static_cast<uint64_t>(my) * 4096 + mx);
                vx += (hash_to_float01(hb) - 0.5f) * 2.0f * params.mv_random;
                vy += (hash_to_float01(hash_u64(hb)) - 0.5f) * 2.0f *
                      params.mv_random;
            }
            const int ivx = static_cast<int>(std::lround(vx));
            const int ivy = static_cast<int>(std::lround(vy));

            const int dx = mx * 16;
            const int dy = my * 16;
            const int bw = std::min(16, static_cast<int>(w) - dx);
            const int bh = std::min(16, static_cast<int>(h) - dy);
            if (bw <= 0 || bh <= 0) continue;
            // The block's content came FROM (dx - mv) in the reference.
            copy_block(state_.y.data(), state_.y_stride, static_cast<int>(w),
                       static_cast<int>(h), dx - ivx, dy - ivy, out.y.data(),
                       out.y_stride, dx, dy, bw, bh);
            const int cdx = mx * 8;
            const int cdy = my * 8;
            const int cbw = std::min(8, static_cast<int>(cw) - cdx);
            const int cbh = std::min(8, static_cast<int>(ch) - cdy);
            if (cbw <= 0 || cbh <= 0) continue;
            copy_block(state_.u.data(), state_.uv_stride, static_cast<int>(cw),
                       static_cast<int>(ch), cdx - ivx / 2, cdy - ivy / 2,
                       out.u.data(), out.uv_stride, cdx, cdy, cbw, cbh);
            copy_block(state_.v.data(), state_.uv_stride, static_cast<int>(cw),
                       static_cast<int>(ch), cdx - ivx / 2, cdy - ivy / 2,
                       out.v.data(), out.uv_stride, cdx, cdy, cbw, cbh);
        }
    }
}

}  // namespace looks::codec

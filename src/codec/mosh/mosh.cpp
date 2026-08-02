#include "codec/mosh/mosh.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

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

// Adds a decoded residual block onto dst in place.
void add_residual(const int16_t block[kBlockCoeffs], uint8_t* dst,
                  size_t dst_stride, int avail_w, int avail_h) {
    const int bw = std::min(kBlockSize, avail_w);
    const int bh = std::min(kBlockSize, avail_h);
    for (int y = 0; y < bh; ++y) {
        uint8_t* row = dst + static_cast<size_t>(y) * dst_stride;
        for (int x = 0; x < bw; ++x) {
            const int v = static_cast<int>(row[x]) + block[y * kBlockSize + x];
            row[x] = static_cast<uint8_t>(std::clamp(v, 0, 255));
        }
    }
}

}  // namespace

void MoshCodec::reset() {
    has_state_ = false;
}

void MoshCodec::encode_decode_intra(const FrameView& in, int quality,
                                    DecodedFrame& out) {
    // Entropy-free wire: pixels identical to encode + decode at this
    // quality, no bytes ever written. (in may alias out — the DCT pass
    // consumes it fully before recon writes.)
    intra_dct(in, intra_scratch_, /*parallel=*/true);
    intra_recon(intra_scratch_, std::clamp(quality, 1, 100), out,
                /*parallel=*/true);
}

void MoshCodec::process(const FrameView& in, uint32_t frame_index,
                        const MoshParams& params, const MvField& mvs,
                        DecodedFrame& out) {
    const uint32_t w = in.width;
    const uint32_t h = in.height;
    if (has_state_ && (state_.width != w || state_.height != h))
        has_state_ = false;

    const bool gop_i = params.gop_length > 0 &&
        frame_index % static_cast<uint32_t>(params.gop_length) == 0;

    if (!has_state_ || gop_i) {
        // The virtual encoder emits an I frame. The clean reference always
        // takes it; the moshed chain takes it only when not dropping —
        // a dropped I repeats the stale frame (the freeze before the melt).
        int quality = std::clamp(params.quality, 1, 100);
        // Two-phase: DCT once (parallel), then the rate loop probes exact
        // stream sizes per quality step without writing bits, and the
        // reconstruction skips entropy entirely — nothing downstream reads
        // the bytes, and entropy is lossless, so the pixels are identical.
        intra_dct(in, intra_scratch_, /*parallel=*/true);
        while (params.bitrate_budget > 0 && quality > 1 &&
               intra_entropy_bytes(intra_scratch_, quality) >
                   params.bitrate_budget)
            quality = std::max(1, quality - 15);
        intra_recon(intra_scratch_, quality, clean_state_, /*parallel=*/true);
        // Generation loss: run the wire again N times. The integer pipeline
        // is idempotent at a fixed quality, so alternate the quantizer a
        // notch between passes — like every real dub chain, no two
        // generations quantize identically, and the error accumulates.
        for (int g = 0; g < std::min(params.generations, 12); ++g) {
            const int gq = std::clamp(
                params.quality - ((g & 1) ? 9 : 0), 1, 100);
            encode_decode_intra(clean_state_.view(), gq, clean_state_);
        }
        if (!has_state_ || !params.drop_iframes) state_ = clean_state_;
        out = state_;
        has_state_ = true;
        return;
    }

    // ---- P frame, open loop: the residual is encoded against the CLEAN
    // reference (raw flow MVs — what the virtual encoder believes the
    // decoder holds) and applied to the MOSHED prediction (mangled MVs over
    // the diverged state). Divergence between the chains is therefore never
    // repaired, only repainted by new residual texture — the melt.
    predict(clean_state_, frame_index, params, mvs, /*mangle=*/false,
            clean_pred_);
    predict(state_, frame_index, params, mvs, /*mangle=*/true, pred_);
    // Bloom: re-apply the mangled motion field N extra times.
    for (int r = 0; r < std::min(params.p_repeat, 8); ++r) {
        predict(pred_, frame_index, params, mvs, /*mangle=*/true, pred_tmp_);
        std::swap(pred_, pred_tmp_);
    }

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
            for (int b = 0; b < 4; ++b) {
                const int px = mx * 16 + (b & 1) * 8;
                const int py = my * 16 + (b >> 1) * 8;
                if (px >= static_cast<int>(w) || py >= static_cast<int>(h)) {
                    p_zero_[base + b] = 1;
                    continue;
                }
                residual_dct_block(
                    in.y.data + py * in.y.stride + px, in.y.stride,
                    clean_pred_.y.data() +
                        static_cast<size_t>(py) * clean_pred_.y_stride + px,
                    clean_pred_.y_stride, static_cast<int>(w) - px,
                    static_cast<int>(h) - py,
                    p_coeffs_.data() + (base + b) * kBlockCoeffs);
            }
            const int cx = mx * 8;
            const int cy = my * 8;
            if (cx >= static_cast<int>(cw) || cy >= static_cast<int>(ch)) {
                p_zero_[base + 4] = 1;
                p_zero_[base + 5] = 1;
            } else {
                residual_dct_block(
                    in.u.data + cy * in.u.stride + cx, in.u.stride,
                    clean_pred_.u.data() +
                        static_cast<size_t>(cy) * clean_pred_.uv_stride + cx,
                    clean_pred_.uv_stride, static_cast<int>(cw) - cx,
                    static_cast<int>(ch) - cy,
                    p_coeffs_.data() + (base + 4) * kBlockCoeffs);
                residual_dct_block(
                    in.v.data + cy * in.v.stride + cx, in.v.stride,
                    clean_pred_.v.data() +
                        static_cast<size_t>(cy) * clean_pred_.uv_stride + cx,
                    clean_pred_.uv_stride, static_cast<int>(cw) - cx,
                    static_cast<int>(ch) - cy,
                    p_coeffs_.data() + (base + 5) * kBlockCoeffs);
            }
        }
    });

    // Rate loop: probes exact stream sizes without writing a bit. The
    // quality sequence matches the old encode-and-measure loop exactly.
    while (params.bitrate_budget > 0 && quality > 1 &&
           p_stream_bytes(mb_count, quality) > params.bitrate_budget)
        quality = std::max(1, quality - 15);
    build_quant_table(kQuantBaseLuma, quality, qy);
    build_quant_table(kQuantBaseChroma, quality, qc);

    // ---- moshed parse. Entropy is lossless, so with no byte flips the
    // decoder's quantized coefficients are exactly quantize(p_coeffs_) and
    // no bitstream exists at all. With flips the stream is written once at
    // the final quality, corrupted, and parsed serially tolerating desync:
    // every block after the desync point stays on bare prediction — and
    // open loop means the scar persists.
    int done = mb_count * 6;
    if (params.byte_flips > 0) {
        bitstream_.clear();
        {
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
        }
        flipped_ = bitstream_;
        for (uint32_t i = 0; i < params.byte_flips && !flipped_.empty(); ++i) {
            const uint64_t hf = hash_combine(frame_seed, 0xB17F00Du + i);
            const size_t byte = static_cast<size_t>(
                hf % static_cast<uint64_t>(flipped_.size()));
            flipped_[byte] ^= static_cast<uint8_t>(1u << (hash_u64(hf) & 7));
        }
        p_parsed_.resize(p_coeffs_.size());
        BitReader br(flipped_.data(), flipped_.size());
        int16_t dc_y = 0, dc_u = 0, dc_v = 0;
        int parsed = 0;
        for (int mb = 0; mb < mb_count && parsed == mb * 6; ++mb) {
            const size_t base = static_cast<size_t>(mb) * 6;
            for (int b = 0; b < 6; ++b) {
                int16_t* dc = b < 4 ? &dc_y : (b == 4 ? &dc_u : &dc_v);
                if (!decode_block(br,
                                  p_parsed_.data() + (base + b) * kBlockCoeffs,
                                  dc))
                    break;
                ++parsed;
            }
        }
        done = parsed;
    }

    // ---- reconstruction, both chains in parallel across MBs. The clean
    // chain adds every residual (faithful decode). The moshed chain skips
    // corrupt-rolled MBs, blocks past the desync point, and uses the parsed
    // (possibly garbage) coefficients when the stream was flipped.
    parallel_blocks(mb_count, true, [&](int begin, int end) {
        int16_t quantized[kBlockCoeffs];
        int16_t block[kBlockCoeffs];
        for (int mb = begin; mb < end; ++mb) {
            const int mx = mb % mb_w;
            const int my = mb / mb_w;
            const size_t base = static_cast<size_t>(mb) * 6;
            const bool corrupt =
                params.residual_corrupt > 0.0f &&
                hash_float01(frame_seed,
                             static_cast<uint64_t>(my) * 4096 + mx) <
                    params.residual_corrupt;
            for (int b = 0; b < 6; ++b) {
                if (p_zero_[base + b]) continue;
                const bool luma = b < 4;
                const int px = luma ? mx * 16 + (b & 1) * 8 : mx * 8;
                const int py = luma ? my * 16 + (b >> 1) * 8 : my * 8;
                const int pw = luma ? static_cast<int>(w)
                                    : static_cast<int>(cw);
                const int ph = luma ? static_cast<int>(h)
                                    : static_cast<int>(ch);
                const uint16_t* qtab = luma ? qy : qc;
                quantize(p_coeffs_.data() + (base + b) * kBlockCoeffs, qtab,
                         quantized);
                dequantize(quantized, qtab, block);
                idct8x8(block);
                uint8_t* cplane = luma
                    ? clean_pred_.y.data()
                    : (b == 4 ? clean_pred_.u.data() : clean_pred_.v.data());
                const size_t cstride =
                    luma ? clean_pred_.y_stride : clean_pred_.uv_stride;
                add_residual(block,
                             cplane + static_cast<size_t>(py) * cstride + px,
                             cstride, pw - px, ph - py);
                if (corrupt || static_cast<int>(base) + b >= done) continue;
                if (params.byte_flips > 0) {
                    dequantize(p_parsed_.data() + (base + b) * kBlockCoeffs,
                               qtab, block);
                    idct8x8(block);
                }
                uint8_t* mplane = luma
                    ? pred_.y.data()
                    : (b == 4 ? pred_.u.data() : pred_.v.data());
                const size_t mstride =
                    luma ? pred_.y_stride : pred_.uv_stride;
                add_residual(block,
                             mplane + static_cast<size_t>(py) * mstride + px,
                             mstride, pw - px, ph - py);
            }
        }
    });

    std::swap(clean_state_, clean_pred_);
    std::swap(state_, pred_);
    out = state_;
}

size_t MoshCodec::p_stream_bytes(int mb_count, int quality) {
    uint16_t qy[kBlockCoeffs], qc[kBlockCoeffs];
    build_quant_table(kQuantBaseLuma, quality, qy);
    build_quant_table(kQuantBaseChroma, quality, qc);
    const size_t blocks = static_cast<size_t>(mb_count) * 6;
    p_dc_.resize(blocks);
    p_acbits_.resize(blocks);
    static const int16_t kZeroBlock[kBlockCoeffs] = {};
    const uint32_t zero_ac = ac_bit_count(kZeroBlock);
    parallel_blocks(mb_count, true, [&](int begin, int end) {
        int16_t quantized[kBlockCoeffs];
        for (int mb = begin; mb < end; ++mb) {
            const size_t base = static_cast<size_t>(mb) * 6;
            for (int b = 0; b < 6; ++b) {
                if (p_zero_[base + b]) {
                    // Encoded as a zero block: DC 0 resets the predictor.
                    p_dc_[base + b] = 0;
                    p_acbits_[base + b] = zero_ac;
                    continue;
                }
                quantize(p_coeffs_.data() + (base + b) * kBlockCoeffs,
                         b < 4 ? qy : qc, quantized);
                p_dc_[base + b] = quantized[0];
                p_acbits_[base + b] = ac_bit_count(quantized);
            }
        }
    });
    uint64_t bits = 0;
    int16_t dc_y = 0, dc_u = 0, dc_v = 0;
    for (int mb = 0; mb < mb_count; ++mb) {
        const size_t base = static_cast<size_t>(mb) * 6;
        for (int b = 0; b < 6; ++b) {
            int16_t* dc = b < 4 ? &dc_y : (b == 4 ? &dc_u : &dc_v);
            bits += se_bit_count(p_dc_[base + b] - *dc) +
                    p_acbits_[base + b];
            *dc = p_dc_[base + b];
        }
    }
    return static_cast<size_t>((bits + 7) / 8);
}

void MoshCodec::predict(const DecodedFrame& ref, uint32_t frame_index,
                        const MoshParams& params, const MvField& mvs,
                        bool mangle, DecodedFrame& out) const {
    const uint32_t w = ref.width;
    const uint32_t h = ref.height;
    const uint32_t cw = (w + 1) / 2;
    const uint32_t ch = (h + 1) / 2;
    const int mb_w = static_cast<int>((w + 15) / 16);
    const int mb_h = static_cast<int>((h + 15) / 16);
    alloc_frame(out, w, h);

    const float cr = std::cos(params.mv_rotate);
    const float sr = std::sin(params.mv_rotate);
    const uint64_t mv_seed = hash_combine(params.seed, 0x33CC33CCu);

    // MBs are independent (disjoint output blocks, per-block seeded MVs),
    // so the motion comp fans out across threads deterministically.
    parallel_blocks(mb_w * mb_h, true, [&](int begin, int end) {
        for (int mb = begin; mb < end; ++mb) {
            const int mx = mb % mb_w;
            const int my = mb / mb_w;
            float vx = 0.0f, vy = 0.0f;
            if (mvs.mx && mvs.my && static_cast<uint32_t>(mx) < mvs.blocks_w &&
                static_cast<uint32_t>(my) < mvs.blocks_h) {
                vx = mvs.mx[my * static_cast<int>(mvs.blocks_w) + mx];
                vy = mvs.my[my * static_cast<int>(mvs.blocks_w) + mx];
            }
            if (mangle) {
                // Replace-with-custom-field: synthetic MV fields swap in for
                // the flow-supplied vectors; the mangling ops below still
                // apply, so rotate steers the pan and scale amplifies the
                // whole field.
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
                    vx += (hash_to_float01(hb) - 0.5f) * 2.0f *
                          params.mv_random;
                    vy += (hash_to_float01(hash_u64(hb)) - 0.5f) * 2.0f *
                          params.mv_random;
                }
            }
            const int ivx = static_cast<int>(std::lround(vx));
            const int ivy = static_cast<int>(std::lround(vy));

            const int dx = mx * 16;
            const int dy = my * 16;
            const int bw = std::min(16, static_cast<int>(w) - dx);
            const int bh = std::min(16, static_cast<int>(h) - dy);
            if (bw <= 0 || bh <= 0) continue;
            // The block's content came FROM (dx - mv) in the reference.
            copy_block(ref.y.data(), ref.y_stride, static_cast<int>(w),
                       static_cast<int>(h), dx - ivx, dy - ivy, out.y.data(),
                       out.y_stride, dx, dy, bw, bh);
            const int cdx = mx * 8;
            const int cdy = my * 8;
            const int cbw = std::min(8, static_cast<int>(cw) - cdx);
            const int cbh = std::min(8, static_cast<int>(ch) - cdy);
            if (cbw <= 0 || cbh <= 0) continue;
            copy_block(ref.u.data(), ref.uv_stride, static_cast<int>(cw),
                       static_cast<int>(ch), cdx - ivx / 2, cdy - ivy / 2,
                       out.u.data(), out.uv_stride, cdx, cdy, cbw, cbh);
            copy_block(ref.v.data(), ref.uv_stride, static_cast<int>(cw),
                       static_cast<int>(ch), cdx - ivx / 2, cdy - ivy / 2,
                       out.v.data(), out.uv_stride, cdx, cdy, cbw, cbh);
        }
    });
}

}  // namespace looks::codec

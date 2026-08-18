#include "codec/mez.h"

#include <io.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "codec/core.h"

namespace looks::codec {

namespace {

constexpr uint32_t kMagic = 0x315A454D;   // 'MEZ1' little-endian
constexpr uint32_t kVersion = 1;
constexpr size_t kHeaderSize = 64;

void put_u32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

void put_u64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (i * 8));
}

uint32_t get_u32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t get_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

// Lossless mode: quality 0 skips the DCT entirely — per-plane
// left/above-predicted residuals, signed exp-Golomb. Bit-exact roundtrip,
// ~2:1 on natural footage.
void encode_plane_lossless(BitWriter& bw, const uint8_t* data, size_t stride,
                           int w, int h) {
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = data + static_cast<size_t>(y) * stride;
        const uint8_t* above = row - stride;
        for (int x = 0; x < w; ++x) {
            const int pred = x > 0 ? row[x - 1] : (y > 0 ? above[x] : 128);
            bw.put_se(static_cast<int32_t>(row[x]) - pred);
        }
    }
}

bool decode_plane_lossless(BitReader& br, uint8_t* data, size_t stride,
                           int w, int h) {
    for (int y = 0; y < h; ++y) {
        uint8_t* row = data + static_cast<size_t>(y) * stride;
        const uint8_t* above = row - stride;
        for (int x = 0; x < w; ++x) {
            const int pred = x > 0 ? row[x - 1] : (y > 0 ? above[x] : 128);
            const int v = pred + br.get_se();
            if (!br.ok()) return false;
            row[x] = static_cast<uint8_t>(std::clamp(v, 0, 255));
        }
    }
    return true;
}

}  // namespace

// ------------------------------------------------------------ frame codec

void encode_frame(const FrameView& frame, int quality,
                  std::vector<uint8_t>& out) {
    out.clear();
    if (quality <= 0) {
        out.push_back(0);   // lossless marker
        BitWriter bw(out);
        const int w = static_cast<int>(frame.width);
        const int h = static_cast<int>(frame.height);
        const int cw = (w + 1) / 2;
        const int ch = (h + 1) / 2;
        encode_plane_lossless(bw, frame.y.data, frame.y.stride, w, h);
        encode_plane_lossless(bw, frame.u.data, frame.u.stride, cw, ch);
        encode_plane_lossless(bw, frame.v.data, frame.v.stride, cw, ch);
        bw.finish();
        return;
    }
    out.push_back(static_cast<uint8_t>(quality));

    uint16_t qy[kBlockCoeffs], qc[kBlockCoeffs];
    build_quant_table(kQuantBaseLuma, quality, qy);
    build_quant_table(kQuantBaseChroma, quality, qc);

    BitWriter bw(out);
    const int w = static_cast<int>(frame.width);
    const int h = static_cast<int>(frame.height);
    const int cw = (w + 1) / 2;
    const int ch = (h + 1) / 2;
    const int mb_w = (w + 15) / 16;
    const int mb_h = (h + 15) / 16;

    int16_t dc_y = 0, dc_u = 0, dc_v = 0;
    for (int my = 0; my < mb_h; ++my) {
        for (int mx = 0; mx < mb_w; ++mx) {
            // Four luma blocks.
            for (int by = 0; by < 2; ++by) {
                for (int bx = 0; bx < 2; ++bx) {
                    const int px = mx * 16 + bx * 8;
                    const int py = my * 16 + by * 8;
                    if (px >= w || py >= h) {
                        // Fully outside (odd MB at edge): encode flat block
                        // predicted from DC so cost is ~2 bits.
                        int16_t flat[kBlockCoeffs] = {};
                        flat[0] = dc_y;
                        encode_block(bw, flat, &dc_y);
                        continue;
                    }
                    encode_pixel_block(bw, frame.y.data + py * frame.y.stride + px,
                                       frame.y.stride, w - px, h - py, qy, &dc_y);
                }
            }
            // Chroma 8x8 each (4:2:0: one per MB).
            const int cx = mx * 8;
            const int cy = my * 8;
            if (cx >= cw || cy >= ch) {
                int16_t flat[kBlockCoeffs] = {};
                flat[0] = dc_u;
                encode_block(bw, flat, &dc_u);
                flat[0] = dc_v;
                encode_block(bw, flat, &dc_v);
            } else {
                encode_pixel_block(bw, frame.u.data + cy * frame.u.stride + cx,
                                   frame.u.stride, cw - cx, ch - cy, qc, &dc_u);
                encode_pixel_block(bw, frame.v.data + cy * frame.v.stride + cx,
                                   frame.v.stride, cw - cx, ch - cy, qc, &dc_v);
            }
        }
    }
    bw.finish();
}

// Two-phase intra. Block order MUST mirror encode_frame
// exactly — per MB: four luma (row-major), U, V — so intra_entropy's
// bytes match encode_frame's at the same quality (test-enforced).
void intra_dct(const FrameView& frame, IntraDct& out, bool parallel) {
    const int w = static_cast<int>(frame.width);
    const int h = static_cast<int>(frame.height);
    const int cw = (w + 1) / 2;
    const int ch = (h + 1) / 2;
    const int mb_w = (w + 15) / 16;
    const int mb_h = (h + 15) / 16;
    out.width = frame.width;
    out.height = frame.height;
    const size_t blocks = static_cast<size_t>(mb_w) * mb_h * 6;
    out.coeffs.resize(blocks * kBlockCoeffs);
    out.flat.assign(blocks, 0);

    parallel_blocks(mb_w * mb_h, parallel, [&](int begin, int end) {
        for (int mb = begin; mb < end; ++mb) {
            const int mx = mb % mb_w;
            const int my = mb / mb_w;
            const size_t base = static_cast<size_t>(mb) * 6;
            for (int b = 0; b < 4; ++b) {
                const int bx = b & 1;
                const int by = b >> 1;
                const int px = mx * 16 + bx * 8;
                const int py = my * 16 + by * 8;
                if (px >= w || py >= h) {
                    out.flat[base + b] = 1;
                    continue;
                }
                extract_dct_block(
                    frame.y.data + py * frame.y.stride + px, frame.y.stride,
                    w - px, h - py,
                    out.coeffs.data() + (base + b) * kBlockCoeffs);
            }
            const int cx = mx * 8;
            const int cy = my * 8;
            if (cx >= cw || cy >= ch) {
                out.flat[base + 4] = 1;
                out.flat[base + 5] = 1;
            } else {
                extract_dct_block(
                    frame.u.data + cy * frame.u.stride + cx, frame.u.stride,
                    cw - cx, ch - cy,
                    out.coeffs.data() + (base + 4) * kBlockCoeffs);
                extract_dct_block(
                    frame.v.data + cy * frame.v.stride + cx, frame.v.stride,
                    cw - cx, ch - cy,
                    out.coeffs.data() + (base + 5) * kBlockCoeffs);
            }
        }
    });
}

void intra_entropy(const IntraDct& dct, int quality,
                   std::vector<uint8_t>& out) {
    out.clear();
    out.push_back(static_cast<uint8_t>(quality));
    uint16_t qy[kBlockCoeffs], qc[kBlockCoeffs];
    build_quant_table(kQuantBaseLuma, quality, qy);
    build_quant_table(kQuantBaseChroma, quality, qc);

    BitWriter bw(out);
    const int mb_w = (static_cast<int>(dct.width) + 15) / 16;
    const int mb_h = (static_cast<int>(dct.height) + 15) / 16;
    int16_t dc_y = 0, dc_u = 0, dc_v = 0;
    int16_t quantized[kBlockCoeffs];
    for (int mb = 0; mb < mb_w * mb_h; ++mb) {
        const size_t base = static_cast<size_t>(mb) * 6;
        for (int b = 0; b < 6; ++b) {
            int16_t* dc = b < 4 ? &dc_y : (b == 4 ? &dc_u : &dc_v);
            if (dct.flat[base + b]) {
                int16_t flat[kBlockCoeffs] = {};
                flat[0] = *dc;
                encode_block(bw, flat, dc);
                continue;
            }
            quantize(dct.coeffs.data() + (base + b) * kBlockCoeffs,
                     b < 4 ? qy : qc, quantized);
            encode_block(bw, quantized, dc);
        }
    }
    bw.finish();
}

void intra_recon(const IntraDct& dct, int quality, DecodedFrame& out,
                 bool parallel) {
    uint16_t qy[kBlockCoeffs], qc[kBlockCoeffs];
    build_quant_table(kQuantBaseLuma, quality, qy);
    build_quant_table(kQuantBaseChroma, quality, qc);
    const int w = static_cast<int>(dct.width);
    const int h = static_cast<int>(dct.height);
    const int cw = (w + 1) / 2;
    const int ch = (h + 1) / 2;
    const int mb_w = (w + 15) / 16;
    const int mb_h = (h + 15) / 16;
    out.width = dct.width;
    out.height = dct.height;
    out.y_stride = static_cast<size_t>(w);
    out.uv_stride = static_cast<size_t>(cw);
    out.y.resize(static_cast<size_t>(w) * h);
    out.u.resize(static_cast<size_t>(cw) * ch);
    out.v.resize(static_cast<size_t>(cw) * ch);

    parallel_blocks(mb_w * mb_h, parallel, [&](int begin, int end) {
        int16_t quantized[kBlockCoeffs];
        int16_t block[kBlockCoeffs];
        for (int mb = begin; mb < end; ++mb) {
            const int mx = mb % mb_w;
            const int my = mb / mb_w;
            const size_t base = static_cast<size_t>(mb) * 6;
            for (int b = 0; b < 6; ++b) {
                if (dct.flat[base + b]) continue;   // edge filler
                const bool luma = b < 4;
                const int px = luma ? mx * 16 + (b & 1) * 8 : mx * 8;
                const int py = luma ? my * 16 + (b >> 1) * 8 : my * 8;
                const int pw = luma ? w : cw;
                const int ph = luma ? h : ch;
                quantize(dct.coeffs.data() + (base + b) * kBlockCoeffs,
                         luma ? qy : qc, quantized);
                dequantize(quantized, luma ? qy : qc, block);
                idct8x8(block);
                uint8_t* plane = luma ? out.y.data()
                                      : (b == 4 ? out.u.data() : out.v.data());
                const size_t stride = luma ? out.y_stride : out.uv_stride;
                uint8_t* dst = plane + static_cast<size_t>(py) * stride + px;
                const int copy_w = std::min(kBlockSize, pw - px);
                const int copy_h = std::min(kBlockSize, ph - py);
                for (int y = 0; y < copy_h; ++y)
                    for (int x = 0; x < copy_w; ++x) {
                        const int val = block[y * kBlockSize + x] + 128;
                        dst[static_cast<size_t>(y) * stride + x] =
                            static_cast<uint8_t>(std::clamp(val, 0, 255));
                    }
            }
        }
    });
}

size_t intra_entropy_bytes(const IntraDct& dct, int quality) {
    uint16_t qy[kBlockCoeffs], qc[kBlockCoeffs];
    build_quant_table(kQuantBaseLuma, quality, qy);
    build_quant_table(kQuantBaseChroma, quality, qc);
    const int mb_w = (static_cast<int>(dct.width) + 15) / 16;
    const int mb_h = (static_cast<int>(dct.height) + 15) / 16;
    const int mb_count = mb_w * mb_h;
    const size_t blocks = static_cast<size_t>(mb_count) * 6;

    // Parallel per-block halves; the DC delta chain is the only serial
    // dependency and reduces to one subtraction per block.
    std::vector<int16_t> dcv(blocks);
    std::vector<uint32_t> acbits(blocks);
    static const int16_t kZeroBlock[kBlockCoeffs] = {};
    const uint32_t flat_ac = ac_bit_count(kZeroBlock);
    parallel_blocks(mb_count, true, [&](int begin, int end) {
        int16_t quantized[kBlockCoeffs];
        for (int mb = begin; mb < end; ++mb) {
            const size_t base = static_cast<size_t>(mb) * 6;
            for (int b = 0; b < 6; ++b) {
                if (dct.flat[base + b]) {
                    acbits[base + b] = flat_ac;
                    continue;   // dc repeats the predictor: delta 0
                }
                quantize(dct.coeffs.data() + (base + b) * kBlockCoeffs,
                         b < 4 ? qy : qc, quantized);
                dcv[base + b] = quantized[0];
                acbits[base + b] = ac_bit_count(quantized);
            }
        }
    });

    uint64_t bits = 0;
    int16_t dc_y = 0, dc_u = 0, dc_v = 0;
    for (int mb = 0; mb < mb_count; ++mb) {
        const size_t base = static_cast<size_t>(mb) * 6;
        for (int b = 0; b < 6; ++b) {
            int16_t* dc = b < 4 ? &dc_y : (b == 4 ? &dc_u : &dc_v);
            if (dct.flat[base + b]) {
                bits += se_bit_count(0) + acbits[base + b];
                continue;
            }
            bits += se_bit_count(dcv[base + b] - *dc) + acbits[base + b];
            *dc = dcv[base + b];
        }
    }
    return 1 + static_cast<size_t>((bits + 7) / 8);   // quality byte + pad
}

namespace {

// Parallel reconstruction: serial entropy parse into a quantized-coeff
// buffer, then dequant+IDCT+store across threads. Pixels identical to the
// serial path (same per-block math).
bool decode_frame_parallel(const uint8_t* data, size_t size, uint32_t width,
                           uint32_t height, DecodedFrame& out) {
    const int quality = data[0];
    uint16_t qy[kBlockCoeffs], qc[kBlockCoeffs];
    build_quant_table(kQuantBaseLuma, quality, qy);
    build_quant_table(kQuantBaseChroma, quality, qc);

    const int w = static_cast<int>(width);
    const int h = static_cast<int>(height);
    const int cw = (w + 1) / 2;
    const int ch = (h + 1) / 2;
    const int mb_w = (w + 15) / 16;
    const int mb_h = (h + 15) / 16;

    out.width = width;
    out.height = height;
    out.y_stride = static_cast<size_t>(w);
    out.uv_stride = static_cast<size_t>(cw);
    out.y.resize(static_cast<size_t>(w) * h);
    out.u.resize(static_cast<size_t>(cw) * ch);
    out.v.resize(static_cast<size_t>(cw) * ch);

    const int mb_count = mb_w * mb_h;
    std::vector<int16_t> coeffs(static_cast<size_t>(mb_count) * 6 *
                                kBlockCoeffs);
    BitReader br(data + 1, size - 1);
    int16_t dc_y = 0, dc_u = 0, dc_v = 0;
    for (int mb = 0; mb < mb_count; ++mb) {
        const size_t base = static_cast<size_t>(mb) * 6;
        for (int b = 0; b < 6; ++b) {
            int16_t* dc = b < 4 ? &dc_y : (b == 4 ? &dc_u : &dc_v);
            if (!decode_block(br, coeffs.data() + (base + b) * kBlockCoeffs,
                              dc))
                return false;
        }
    }

    parallel_blocks(mb_count, true, [&](int begin, int end) {
        int16_t block[kBlockCoeffs];
        for (int mb = begin; mb < end; ++mb) {
            const int mx = mb % mb_w;
            const int my = mb / mb_w;
            const size_t base = static_cast<size_t>(mb) * 6;
            for (int b = 0; b < 6; ++b) {
                const bool luma = b < 4;
                const int px = luma ? mx * 16 + (b & 1) * 8 : mx * 8;
                const int py = luma ? my * 16 + (b >> 1) * 8 : my * 8;
                const int pw = luma ? w : cw;
                const int ph = luma ? h : ch;
                if (px >= pw || py >= ph) continue;   // parsed filler
                dequantize(coeffs.data() + (base + b) * kBlockCoeffs,
                           luma ? qy : qc, block);
                idct8x8(block);
                uint8_t* plane = luma ? out.y.data()
                                      : (b == 4 ? out.u.data() : out.v.data());
                const size_t stride = luma ? out.y_stride : out.uv_stride;
                uint8_t* dst = plane + static_cast<size_t>(py) * stride + px;
                const int copy_w = std::min(kBlockSize, pw - px);
                const int copy_h = std::min(kBlockSize, ph - py);
                for (int y = 0; y < copy_h; ++y)
                    for (int x = 0; x < copy_w; ++x) {
                        const int val = block[y * kBlockSize + x] + 128;
                        dst[static_cast<size_t>(y) * stride + x] =
                            static_cast<uint8_t>(std::clamp(val, 0, 255));
                    }
            }
        }
    });
    return true;
}

}  // namespace

bool decode_frame(const uint8_t* data, size_t size, uint32_t width,
                  uint32_t height, DecodedFrame& out, bool parallel) {
    if (size < 1) return false;
    if (data[0] == 0) {
        // Lossless: inherently serial (row prediction), so the
        // parallel flag is ignored.
        const int w = static_cast<int>(width);
        const int h = static_cast<int>(height);
        const int cw = (w + 1) / 2;
        const int ch = (h + 1) / 2;
        out.width = width;
        out.height = height;
        out.y_stride = static_cast<size_t>(w);
        out.uv_stride = static_cast<size_t>(cw);
        out.y.resize(static_cast<size_t>(w) * h);
        out.u.resize(static_cast<size_t>(cw) * ch);
        out.v.resize(static_cast<size_t>(cw) * ch);
        BitReader br(data + 1, size - 1);
        return decode_plane_lossless(br, out.y.data(), out.y_stride, w, h) &&
               decode_plane_lossless(br, out.u.data(), out.uv_stride, cw,
                                     ch) &&
               decode_plane_lossless(br, out.v.data(), out.uv_stride, cw, ch);
    }
    if (parallel) return decode_frame_parallel(data, size, width, height, out);
    const int quality = data[0];

    uint16_t qy[kBlockCoeffs], qc[kBlockCoeffs];
    build_quant_table(kQuantBaseLuma, quality, qy);
    build_quant_table(kQuantBaseChroma, quality, qc);

    const int w = static_cast<int>(width);
    const int h = static_cast<int>(height);
    const int cw = (w + 1) / 2;
    const int ch = (h + 1) / 2;
    const int mb_w = (w + 15) / 16;
    const int mb_h = (h + 15) / 16;

    out.width = width;
    out.height = height;
    out.y_stride = static_cast<size_t>(w);
    out.uv_stride = static_cast<size_t>(cw);
    out.y.resize(static_cast<size_t>(w) * h);
    out.u.resize(static_cast<size_t>(cw) * ch);
    out.v.resize(static_cast<size_t>(cw) * ch);

    BitReader br(data + 1, size - 1);
    int16_t dc_y = 0, dc_u = 0, dc_v = 0;
    int16_t discard[kBlockCoeffs];
    for (int my = 0; my < mb_h; ++my) {
        for (int mx = 0; mx < mb_w; ++mx) {
            for (int by = 0; by < 2; ++by) {
                for (int bx = 0; bx < 2; ++bx) {
                    const int px = mx * 16 + bx * 8;
                    const int py = my * 16 + by * 8;
                    if (px >= w || py >= h) {
                        if (!decode_block(br, discard, &dc_y)) return false;
                        continue;
                    }
                    if (!decode_pixel_block(
                            br, out.y.data() + py * out.y_stride + px,
                            out.y_stride, w - px, h - py, qy, &dc_y))
                        return false;
                }
            }
            const int cx = mx * 8;
            const int cy = my * 8;
            if (cx >= cw || cy >= ch) {
                if (!decode_block(br, discard, &dc_u)) return false;
                if (!decode_block(br, discard, &dc_v)) return false;
            } else {
                if (!decode_pixel_block(br,
                                        out.u.data() + cy * out.uv_stride + cx,
                                        out.uv_stride, cw - cx, ch - cy, qc,
                                        &dc_u))
                    return false;
                if (!decode_pixel_block(br,
                                        out.v.data() + cy * out.uv_stride + cx,
                                        out.uv_stride, cw - cx, ch - cy, qc,
                                        &dc_v))
                    return false;
            }
        }
    }
    return true;
}

// ------------------------------------------------------------ MezWriter

MezWriter::~MezWriter() {
    // An unfinished writer is an ABORTED import (cancel, error,
    // teardown). Never seal it: a partial mez with a patched header
    // reads as a valid SHORTER clip and poisons the bundle cache -
    // close and remove the file so nothing can trust it.
    const bool partial = file_ && !finished_;
    if (file_) std::fclose(static_cast<FILE*>(file_));
    if (partial) {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
}

bool MezWriter::open(const std::filesystem::path& path, uint32_t width,
                     uint32_t height, uint32_t timescale,
                     uint32_t frame_duration, int quality) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    file_ = f;
    path_ = path;
    width_ = width;
    height_ = height;
    quality_ = quality;
    finished_ = false;
    offsets_.clear();

    uint8_t header[kHeaderSize] = {};
    put_u32(header + 0, kMagic);
    put_u32(header + 4, kVersion);
    put_u32(header + 8, width);
    put_u32(header + 12, height);
    put_u32(header + 16, 0);   // frame count (patched)
    put_u32(header + 20, timescale);
    put_u32(header + 24, frame_duration);
    put_u32(header + 28, static_cast<uint32_t>(quality));
    put_u32(header + 32, 0);   // flags
    put_u64(header + 36, 0);   // index offset (patched)
    return std::fwrite(header, 1, kHeaderSize, f) == kHeaderSize;
}

bool MezWriter::add_frame(const FrameView& frame) {
    encode_frame(frame, quality_, scratch_);
    return add_encoded_frame(scratch_);
}

bool MezWriter::add_encoded_frame(const std::vector<uint8_t>& payload) {
    if (!file_ || finished_) return false;
    FILE* f = static_cast<FILE*>(file_);
    offsets_.push_back(static_cast<uint64_t>(_ftelli64(f)));
    uint8_t size_le[4];
    put_u32(size_le, static_cast<uint32_t>(payload.size()));
    if (std::fwrite(size_le, 1, 4, f) != 4) return false;
    return std::fwrite(payload.data(), 1, payload.size(), f) == payload.size();
}

bool MezWriter::add_hold_frames(size_t count) {
    if (!file_ || finished_ || offsets_.empty()) return false;
    offsets_.insert(offsets_.end(), count, offsets_.back());
    return true;
}

bool MezWriter::finish() {
    if (!file_ || finished_) return false;
    FILE* f = static_cast<FILE*>(file_);
    const uint64_t index_offset = static_cast<uint64_t>(_ftelli64(f));
    for (uint64_t offset : offsets_) {
        uint8_t le[8];
        put_u64(le, offset);
        if (std::fwrite(le, 1, 8, f) != 8) return false;
    }
    uint8_t patch[12];
    put_u32(patch, static_cast<uint32_t>(offsets_.size()));
    _fseeki64(f, 16, SEEK_SET);
    if (std::fwrite(patch, 1, 4, f) != 4) return false;
    put_u64(patch, index_offset);
    _fseeki64(f, 36, SEEK_SET);
    if (std::fwrite(patch, 1, 8, f) != 8) return false;
    std::fflush(f);
    finished_ = true;
    return true;
}

bool mez_probe(const std::filesystem::path& path, uint32_t* frames,
               double* fps) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    uint8_t header[kHeaderSize];
    const bool ok = std::fread(header, 1, kHeaderSize, f) == kHeaderSize &&
                    get_u32(header + 0) == kMagic;
    std::fclose(f);
    if (!ok) return false;
    const uint32_t fd = get_u32(header + 24);
    if (frames) *frames = get_u32(header + 16);
    if (fps)
        *fps = fd ? static_cast<double>(get_u32(header + 20)) / fd : 0.0;
    return true;
}

bool mez_set_frame_count(const std::filesystem::path& path, uint32_t count) {
    if (count == 0) return false;
    FILE* f = _wfopen(path.c_str(), L"r+b");
    if (!f) return false;
    bool ok = false;
    uint8_t header[kHeaderSize];
    uint32_t old_count = 0;
    uint64_t index_offset = 0;
    if (std::fread(header, 1, kHeaderSize, f) == kHeaderSize &&
        get_u32(header) == kMagic && get_u32(header + 4) == kVersion) {
        old_count = get_u32(header + 16);
        index_offset = get_u64(header + 36);
    }
    if (old_count > 0 && index_offset >= kHeaderSize) {
        const uint32_t kept = count < old_count ? count : old_count;
        std::vector<uint8_t> raw(static_cast<size_t>(kept) * 8u);
        if (_fseeki64(f, static_cast<int64_t>(index_offset), SEEK_SET) == 0 &&
            std::fread(raw.data(), 1, raw.size(), f) == raw.size()) {
            // Extend by repeating the last kept entry, then patch the count.
            const uint64_t tail_end = index_offset + count * 8ull;
            bool wrote = true;
            if (count > kept) {
                uint8_t le[8];
                std::memcpy(le, raw.data() + (kept - 1) * 8u, 8);
                _fseeki64(f, static_cast<int64_t>(index_offset + kept * 8ull),
                          SEEK_SET);
                for (uint32_t i = kept; i < count && wrote; ++i)
                    wrote = std::fwrite(le, 1, 8, f) == 8;
            }
            if (wrote) {
                uint8_t patch[4];
                put_u32(patch, count);
                _fseeki64(f, 16, SEEK_SET);
                ok = std::fwrite(patch, 1, 4, f) == 4;
                std::fflush(f);
                // Shrink: drop index bytes past the new end (harmless if
                // it fails — the reader consumes exactly `count` entries).
                if (ok && count < old_count)
                    _chsize_s(_fileno(f), static_cast<int64_t>(tail_end));
            }
        }
    }
    std::fclose(f);
    return ok;
}

// ------------------------------------------------------------ MezReader

MezReader::~MezReader() { close(); }

void MezReader::close() {
    if (file_) {
        std::fclose(static_cast<FILE*>(file_));
        file_ = nullptr;
    }
}

bool MezReader::open(const std::filesystem::path& path, std::string* error) {
    close();
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        if (error) *error = "cannot open file";
        return false;
    }
    file_ = f;

    uint8_t header[kHeaderSize];
    if (std::fread(header, 1, kHeaderSize, f) != kHeaderSize ||
        get_u32(header) != kMagic || get_u32(header + 4) != kVersion) {
        if (error) *error = "not a MEZ1 file";
        close();
        return false;
    }
    width_ = get_u32(header + 8);
    height_ = get_u32(header + 12);
    const uint32_t frame_count = get_u32(header + 16);
    timescale_ = get_u32(header + 20);
    frame_duration_ = get_u32(header + 24);
    const uint64_t index_offset = get_u64(header + 36);

    if (frame_count == 0 || index_offset < kHeaderSize) {
        if (error) *error = "unfinished mez file";
        close();
        return false;
    }
    offsets_.resize(frame_count);
    _fseeki64(f, static_cast<int64_t>(index_offset), SEEK_SET);
    std::vector<uint8_t> raw(frame_count * 8u);
    if (std::fread(raw.data(), 1, raw.size(), f) != raw.size()) {
        if (error) *error = "truncated index";
        close();
        return false;
    }
    for (uint32_t i = 0; i < frame_count; ++i)
        offsets_[i] = get_u64(raw.data() + i * 8u);
    return true;
}

bool MezReader::decode(uint32_t frame_index, DecodedFrame& out) {
    if (!file_ || frame_index >= offsets_.size()) return false;
    FILE* f = static_cast<FILE*>(file_);
    if (_fseeki64(f, static_cast<int64_t>(offsets_[frame_index]), SEEK_SET) != 0)
        return false;
    uint8_t size_le[4];
    if (std::fread(size_le, 1, 4, f) != 4) return false;
    const uint32_t payload_size = get_u32(size_le);
    if (payload_size == 0 || payload_size > (1u << 30)) return false;
    scratch_.resize(payload_size);
    if (std::fread(scratch_.data(), 1, payload_size, f) != payload_size)
        return false;
    return decode_frame(scratch_.data(), payload_size, width_, height_, out);
}

}  // namespace looks::codec

#include "codec/mez.h"

#include <io.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "codec/core.h"
#include "util/bytes.h"

namespace looks::codec {

namespace {

constexpr uint32_t kMagic = 0x315A454D;   // 'MEZ1' little-endian
constexpr uint32_t kVersion = 1;
constexpr size_t kHeaderSize = 64;

using bytes::le32;
using bytes::le64;
using bytes::put_le32;
using bytes::put_le64;

// quality 0 = lossless; roundtrip is bit-exact.
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
            for (int by = 0; by < 2; ++by) {
                for (int bx = 0; bx < 2; ++bx) {
                    const int px = mx * 16 + bx * 8;
                    const int py = my * 16 + by * 8;
                    if (px >= w || py >= h) {
                        // The decoder parses a block here too; keep the filler.
                        int16_t flat[kBlockCoeffs] = {};
                        flat[0] = dc_y;
                        encode_block(bw, flat, &dc_y);
                        continue;
                    }
                    encode_pixel_block(bw, frame.y.data + py * frame.y.stride + px,
                                       frame.y.stride, w - px, h - py, qy, &dc_y);
                }
            }
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

// Block order must mirror encode_frame: per MB four luma, then U, then V.
void intra_dct(const FrameView& frame, IntraDct& out) {
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

    parallel_blocks(mb_w * mb_h, true, [&](int begin, int end) {
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

// Both decode paths must share these helpers to stay bit-identical.
struct BlockDst {
    uint8_t* plane;
    size_t stride;
    int px, py, pw, ph;
};

BlockDst block_dst(DecodedFrame& out, int mx, int my, int b, int w, int h,
                   int cw, int ch) {
    const bool luma = b < 4;
    BlockDst d;
    d.px = luma ? mx * 16 + (b & 1) * 8 : mx * 8;
    d.py = luma ? my * 16 + (b >> 1) * 8 : my * 8;
    d.pw = luma ? w : cw;
    d.ph = luma ? h : ch;
    d.plane = luma ? out.y.data() : (b == 4 ? out.u.data() : out.v.data());
    d.stride = luma ? out.y_stride : out.uv_stride;
    return d;
}

void recon_block_write(const BlockDst& d, const int16_t* block) {
    uint8_t* dst =
        d.plane + static_cast<size_t>(d.py) * d.stride + d.px;
    const int copy_w = std::min(kBlockSize, d.pw - d.px);
    const int copy_h = std::min(kBlockSize, d.ph - d.py);
    for (int y = 0; y < copy_h; ++y)
        for (int x = 0; x < copy_w; ++x) {
            const int val = block[y * kBlockSize + x] + 128;
            dst[static_cast<size_t>(y) * d.stride + x] =
                static_cast<uint8_t>(std::clamp(val, 0, 255));
        }
}

void intra_recon(const IntraDct& dct, int quality, DecodedFrame& out) {
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

    parallel_blocks(mb_w * mb_h, true, [&](int begin, int end) {
        int16_t quantized[kBlockCoeffs];
        int16_t block[kBlockCoeffs];
        for (int mb = begin; mb < end; ++mb) {
            const int mx = mb % mb_w;
            const int my = mb / mb_w;
            const size_t base = static_cast<size_t>(mb) * 6;
            for (int b = 0; b < 6; ++b) {
                if (dct.flat[base + b]) continue;   // edge filler
                const bool luma = b < 4;
                quantize(dct.coeffs.data() + (base + b) * kBlockCoeffs,
                         luma ? qy : qc, quantized);
                dequantize(quantized, luma ? qy : qc, block);
                idct8x8(block);
                recon_block_write(block_dst(out, mx, my, b, w, h, cw, ch),
                                  block);
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

// Pixels must match the serial path bit for bit.
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
                const BlockDst d =
                    block_dst(out, mx, my, b, w, h, cw, ch);
                if (d.px >= d.pw || d.py >= d.ph) continue;  // parsed filler
                dequantize(coeffs.data() + (base + b) * kBlockCoeffs,
                           luma ? qy : qc, block);
                idct8x8(block);
                recon_block_write(d, block);
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
        // Lossless is serial; the parallel flag is ignored by intent.
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

MezWriter::~MezWriter() {
    // Never seal a partial file: it reads as a valid shorter clip. Remove it.
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
    put_le32(header + 0, kMagic);
    put_le32(header + 4, kVersion);
    put_le32(header + 8, width);
    put_le32(header + 12, height);
    put_le32(header + 16, 0);   // frame count (patched)
    put_le32(header + 20, timescale);
    put_le32(header + 24, frame_duration);
    put_le32(header + 28, static_cast<uint32_t>(quality));
    put_le32(header + 32, 0);   // flags
    put_le64(header + 36, 0);   // index offset (patched)
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
    put_le32(size_le, static_cast<uint32_t>(payload.size()));
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
        put_le64(le, offset);
        if (std::fwrite(le, 1, 8, f) != 8) return false;
    }
    uint8_t patch[12];
    put_le32(patch, static_cast<uint32_t>(offsets_.size()));
    _fseeki64(f, 16, SEEK_SET);
    if (std::fwrite(patch, 1, 4, f) != 4) return false;
    put_le64(patch, index_offset);
    _fseeki64(f, 36, SEEK_SET);
    if (std::fwrite(patch, 1, 8, f) != 8) return false;
    std::fflush(f);
    finished_ = true;
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
        le32(header) == kMagic && le32(header + 4) == kVersion) {
        old_count = le32(header + 16);
        index_offset = le64(header + 36);
    }
    if (old_count > 0 && index_offset >= kHeaderSize) {
        const uint32_t kept = count < old_count ? count : old_count;
        std::vector<uint8_t> raw(static_cast<size_t>(kept) * 8u);
        if (_fseeki64(f, static_cast<int64_t>(index_offset), SEEK_SET) == 0 &&
            std::fread(raw.data(), 1, raw.size(), f) == raw.size()) {
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
                put_le32(patch, count);
                _fseeki64(f, 16, SEEK_SET);
                ok = std::fwrite(patch, 1, 4, f) == 4;
                std::fflush(f);
                // Truncate failure is harmless: readers use count entries only.
                if (ok && count < old_count)
                    _chsize_s(_fileno(f), static_cast<int64_t>(tail_end));
            }
        }
    }
    std::fclose(f);
    return ok;
}

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
        le32(header) != kMagic || le32(header + 4) != kVersion) {
        if (error) *error = "not a MEZ1 file";
        close();
        return false;
    }
    width_ = le32(header + 8);
    height_ = le32(header + 12);
    const uint32_t frame_count = le32(header + 16);
    timescale_ = le32(header + 20);
    frame_duration_ = le32(header + 24);
    const uint64_t index_offset = le64(header + 36);

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
        offsets_[i] = le64(raw.data() + i * 8u);
    return true;
}

bool MezReader::decode(uint32_t frame_index, DecodedFrame& out) {
    if (!file_ || frame_index >= offsets_.size()) return false;
    FILE* f = static_cast<FILE*>(file_);
    if (_fseeki64(f, static_cast<int64_t>(offsets_[frame_index]), SEEK_SET) != 0)
        return false;
    uint8_t size_le[4];
    if (std::fread(size_le, 1, 4, f) != 4) return false;
    const uint32_t payload_size = le32(size_le);
    if (payload_size == 0 || payload_size > (1u << 30)) return false;
    scratch_.resize(payload_size);
    if (std::fread(scratch_.data(), 1, payload_size, f) != payload_size)
        return false;
    return decode_frame(scratch_.data(), payload_size, width_, height_, out);
}

}  // namespace looks::codec

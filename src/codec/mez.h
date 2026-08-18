// Mezzanine codec: intra-only, MJPEG-class, YCbCr 4:2:0 (I420
// planar), 16x16 macroblocks of four 8x8 luma + one 8x8 per chroma over
// codec_core. Every frame independent -> instant scrub. Decode is CPU-side
// on worker threads; frames reach the GPU as plain uploads.
//
// .mez layout: fixed 64-byte header, then length-prefixed frames, then a
// u64 offset index (header patched with its position on finish). All
// integers little-endian.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace looks::codec {

struct PlaneView {
    const uint8_t* data = nullptr;
    size_t stride = 0;
};

// I420 input: full-res Y, half-res U/V.
struct FrameView {
    PlaneView y, u, v;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct DecodedFrame {
    std::vector<uint8_t> y, u, v;
    uint32_t width = 0;
    uint32_t height = 0;
    size_t y_stride = 0;
    size_t uv_stride = 0;
    // Content identity for upload skipping: producers that keep frames
    // alive across renders (the decode pool) stamp each decode from a
    // process-wide counter, so a consumer re-fed the SAME frame (stills,
    // slowed placements, paused re-renders) can skip re-copying megabytes
    // of identical planes. 0 = unstamped, always treated as fresh.
    uint64_t stamp = 0;

    FrameView view() const {
        return {{y.data(), y_stride}, {u.data(), uv_stride},
                {v.data(), uv_stride}, width, height};
    }
};

// Stateless single-frame codec (also the building block for the mosh
// codec's I-frames). Deterministic: same input + quality => same bytes.
void encode_frame(const FrameView& frame, int quality, std::vector<uint8_t>& out);
// `parallel` splits the pixel reconstruction (dequant+IDCT) across threads
// after the serial entropy parse — identical pixels, used by the Codec-Box
// hot path. The player/import keep the default serial path (they already
// parallelize across frames).
bool decode_frame(const uint8_t* data, size_t size, uint32_t width,
                  uint32_t height, DecodedFrame& out, bool parallel = false);

// Two-phase intra encode (Codec-Box rate loops, ): the DCT is
// quality-independent, so transform once (optionally across threads) and
// re-run only quantize+entropy per quality step. intra_entropy output is
// byte-identical to encode_frame at the same quality. Import stays on
// encode_frame — it is already parallel across frames and the coefficient
// buffer (~12 MB at 1080p) would multiply across its workers.
struct IntraDct {
    uint32_t width = 0, height = 0;
    std::vector<int16_t> coeffs;   // 64 per block, bitstream block order
    std::vector<uint8_t> flat;     // 1 = edge filler block (coeffs unused)
};
void intra_dct(const FrameView& frame, IntraDct& out, bool parallel);
void intra_entropy(const IntraDct& dct, int quality,
                   std::vector<uint8_t>& out);

// Entropy-free reconstruction: quantize + dequantize + IDCT straight from
// the DCT phase. Entropy is lossless, so pixels are bit-identical to
// intra_entropy + decode_frame at the same quality — the wire without the
// bytes, for consumers that never read the stream (the Codec-Box when
// nothing rate-limits or corrupts it).
void intra_recon(const IntraDct& dct, int quality, DecodedFrame& out,
                 bool parallel);

// Exact byte count intra_entropy would produce at this quality, without
// writing it (parallel AC scan + serial DC-delta chain). Rate loops probe
// with this and reconstruct once.
size_t intra_entropy_bytes(const IntraDct& dct, int quality);

class MezWriter {
public:
    ~MezWriter();

    bool open(const std::filesystem::path& path, uint32_t width,
              uint32_t height, uint32_t timescale, uint32_t frame_duration,
              int quality);
    // Frames must arrive in presentation order. Encodes and appends.
    bool add_frame(const FrameView& frame);
    // Appends a pre-encoded frame payload (parallel import encodes on
    // worker threads and serializes through this).
    bool add_encoded_frame(const std::vector<uint8_t>& payload);
    // Still-image media: appends `count` index entries pointing at the
    // LAST written frame's payload — N timeline frames for one frame of
    // storage. The reader can't tell the difference.
    bool add_hold_frames(size_t count);
    bool finish();   // writes the index, patches the header

    uint32_t frame_count() const { return static_cast<uint32_t>(offsets_.size()); }
    int quality() const { return quality_; }

private:
    void* file_ = nullptr;
    std::filesystem::path path_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    int quality_ = 90;
    std::vector<uint64_t> offsets_;
    std::vector<uint8_t> scratch_;
    bool finished_ = false;
};

// Header-only probe: frame count and fps without touching the index
// (44 bytes read). The bundle completeness gate keys on this. False =
// unreadable or not a mez.
bool mez_probe(const std::filesystem::path& path, uint32_t* frames,
               double* fps);

// Rewrites the frame index in place so the media runs `count` frames:
// entries past the old count repeat the last surviving frame's payload
// (the add_hold_frames trick, applied after the fact). Still-image media
// use this to change their timeline duration without re-encoding. The
// file must not be open in a writer; readers holding the old index keep
// working (decode bounds-checks) but see the old length until reopened.
bool mez_set_frame_count(const std::filesystem::path& path, uint32_t count);

class MezReader {
public:
    ~MezReader();

    bool open(const std::filesystem::path& path, std::string* error);
    void close();

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t frame_count() const { return static_cast<uint32_t>(offsets_.size()); }
    uint32_t timescale() const { return timescale_; }
    uint32_t frame_duration() const { return frame_duration_; }
    double fps() const {
        return frame_duration_ ? static_cast<double>(timescale_) / frame_duration_
                               : 0.0;
    }

    // Thread-compatible with itself only under external locking (single
    // FILE*); the player's decode workers each own a reader instance.
    bool decode(uint32_t frame_index, DecodedFrame& out);

    // Where a frame's payload lives: hold-frame entries (stills, cover
    // art) repeat one offset, so a consumer can decode the payload once
    // and alias every frame that points at it. 0 = out of range.
    uint64_t payload_offset(uint32_t frame_index) const {
        return frame_index < offsets_.size() ? offsets_[frame_index] : 0;
    }

private:
    void* file_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t timescale_ = 0;
    uint32_t frame_duration_ = 0;
    std::vector<uint64_t> offsets_;
    std::vector<uint8_t> scratch_;
};

}  // namespace looks::codec

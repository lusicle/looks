// .mez layout: 64-byte header, length-prefixed frames, u64 offset index.
// All integers are little-endian.

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
    std::vector<uint8_t> rgba;
    uint32_t width = 0;
    uint32_t height = 0;
    size_t y_stride = 0;
    size_t uv_stride = 0;
    // Decode identity stamp: same stamp = same planes; 0 = always fresh.
    uint64_t stamp = 0;
    // nv12: y holds Y rows then interleaved CbCr rows at one stride; u/v empty.
    // The codec paths never see nv12 frames.
    bool nv12 = false;

    void alloc_planes(uint32_t w, uint32_t h) {
        rgba.clear();
        nv12 = false;
        width = w;
        height = h;
        y_stride = w;
        uv_stride = (w + 1) / 2;
        y.resize(static_cast<size_t>(w) * h);
        u.resize(uv_stride * ((h + 1) / 2));
        v.resize(uv_stride * ((h + 1) / 2));
    }

    void set_rgba(const uint8_t* pixels, uint32_t w, uint32_t h);

    FrameView view() const {
        if (nv12)
            return {{y.data(), y_stride},
                    {y.data() + y_stride * height, y_stride},
                    {nullptr, 0}, width, height};
        return {{y.data(), y_stride}, {u.data(), uv_stride},
                {v.data(), uv_stride}, width, height};
    }
};

// Same input and quality give the same bytes.
void encode_frame(const FrameView& frame, int quality, std::vector<uint8_t>& out);
// parallel splits reconstruction only; pixels stay identical.
bool decode_frame(const uint8_t* data, size_t size, uint32_t width,
                  uint32_t height, DecodedFrame& out, bool parallel = false);

// intra_entropy output is byte-identical to encode_frame at equal quality.
struct IntraDct {
    uint32_t width = 0, height = 0;
    std::vector<int16_t> coeffs;   // 64 per block, bitstream block order
    std::vector<uint8_t> flat;     // 1 = edge filler block (coeffs unused)
};
void intra_dct(const FrameView& frame, IntraDct& out);
void intra_entropy(const IntraDct& dct, int quality,
                   std::vector<uint8_t>& out);

// Pixels match intra_entropy + decode_frame bit for bit at equal quality.
void intra_recon(const IntraDct& dct, int quality, DecodedFrame& out);

// Must match the byte count intra_entropy writes at this quality.
size_t intra_entropy_bytes(const IntraDct& dct, int quality);

class MezWriter {
public:
    ~MezWriter();

    bool open(const std::filesystem::path& path, uint32_t width,
              uint32_t height, uint32_t timescale, uint32_t frame_duration,
              int quality);
    // Frames must arrive in presentation order.
    bool add_frame(const FrameView& frame);
    bool open_rgba(const std::filesystem::path& path, uint32_t width,
                   uint32_t height, uint32_t timescale, uint32_t frame_duration,
                   bool animated = false);
    bool add_rgba_frame(const uint8_t* pixels);
    // Appends count index entries that point at the last frame's payload.
    bool add_hold_frames(size_t count);
    bool finish();   // the file is incomplete until finish()

    uint32_t frame_count() const { return static_cast<uint32_t>(offsets_.size()); }
    int quality() const { return quality_; }

private:
    bool add_encoded_frame(const std::vector<uint8_t>& payload);

    void* file_ = nullptr;
    std::filesystem::path path_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    int quality_ = 90;
    std::vector<uint64_t> offsets_;
    std::vector<uint8_t> scratch_;
    bool finished_ = false;
};

// The file must not be open in a writer.
// Open readers stay safe but see the old length until reopened.
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
    bool rgba() const { return (flags_ & 1u) != 0; }
    bool animated() const { return (flags_ & 2u) != 0; }
    double fps() const {
        return frame_duration_ ? static_cast<double>(timescale_) / frame_duration_
                               : 0.0;
    }

    // Not thread-safe: serialize calls or use one reader per thread.
    bool decode(uint32_t frame_index, DecodedFrame& out);

    // Hold frames repeat one offset; 0 = out of range.
    uint64_t payload_offset(uint32_t frame_index) const {
        return frame_index < offsets_.size() ? offsets_[frame_index] : 0;
    }

private:
    void* file_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t timescale_ = 0;
    uint32_t frame_duration_ = 0;
    uint32_t flags_ = 0;
    std::vector<uint64_t> offsets_;
    std::vector<uint8_t> scratch_;
};

}  // namespace looks::codec

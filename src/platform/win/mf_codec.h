// Media Foundation MFT glue: raw MFT path — we own the
// containers, so no SourceReader/SinkWriter. OS codecs are touched only at
// import/export edges.
//
// Notes:
// - H.264 ENCODE prefers the hardware path: async MFT unlocked
//   via MF_TRANSFORM_ASYNC_UNLOCK, driven by the METransformNeedInput /
//   HaveOutput event pump, with a D3D11 IMFDXGIDeviceManager attached.
//   Any setup failure logs its stage and falls back to the sync software
//   encoder — import/export are offline, so fallback is only a speed loss.
// - H.264 DECODE keeps the sync inbox MFT but attaches an
//   IMFDXGIDeviceManager when the transform is D3D11-aware: the
//   pixel work then runs on the GPU (DXVA) and samples come back D3D-backed
//   with a GPU pitch, which receive() reads via IMF2DBuffer2::Lock2DSize.
//   Any D3D setup failure silently stays on the pure software path. (There
//   is no vendor async decoder MFT to prefer — hardware decode on Windows
//   is DXVA through the inbox decoder, unlike the encode side.)
// - H.264 input is Annex B: feed() converts the demuxer's length-prefixed
//   AVCC samples and injects SPS/PPS from avcC before keyframes.
// - COM/MF lifetime: construct MfSession once per thread that touches MF.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace looks::platform {

// CoInitializeEx(MTA) + MFStartup, balanced in the destructor. Cheap to
// nest (refcounted by the OS).
class MfSession {
public:
    MfSession();
    ~MfSession();
    bool ok() const { return ok_; }

private:
    bool com_ = false;
    bool mf_ = false;
    bool ok_ = false;
};

struct VideoFrameNV12 {
    std::vector<uint8_t> data;   // Y plane then interleaved UV
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;         // bytes per row (both planes)
    int64_t pts_100ns = 0;
};

class H264Decoder {
public:
    H264Decoder();
    ~H264Decoder();

    // avcc = raw AVCDecoderConfigurationRecord from the demuxer.
    // allow_d3d gates the DXVA path: GPU decode pays a fixed sync-readback
    // stall per frame, so CPU-consuming callers (import) may prefer the
    // multithreaded software decoder.
    bool create(const std::vector<uint8_t>& avcc, uint32_t width,
                uint32_t height, std::string* error, bool allow_d3d = true);

    // One demuxed sample (length-prefixed NALs). Returns false on hard error.
    bool feed(const uint8_t* data, size_t size, int64_t pts_100ns,
              int64_t duration_100ns, bool keyframe);

    // Pulls one decoded frame if available. Returns false when the decoder
    // needs more input (not an error).
    bool receive(VideoFrameNV12& out);

    // Signals end of stream; keep calling receive() until it returns false.
    void drain();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct AudioChunk {
    std::vector<int16_t> samples;   // interleaved
    uint32_t channels = 0;
    uint32_t sample_rate = 0;
    int64_t pts_100ns = 0;
};

class AacDecoder {
public:
    AacDecoder();
    ~AacDecoder();

    // asc = AudioSpecificConfig from esds.
    bool create(const std::vector<uint8_t>& asc, uint32_t channels,
                uint32_t sample_rate, std::string* error);
    bool feed(const uint8_t* data, size_t size, int64_t pts_100ns);
    bool receive(AudioChunk& out);
    void drain();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// MP3 (Layer III) decode through the inbox MFT, fed whole frames by the
// in-repo frame walker (media/mp3.cpp owns the container exactly as the
// BMFF demuxer does for AAC).
class Mp3Decoder {
public:
    Mp3Decoder();
    ~Mp3Decoder();

    bool create(uint32_t channels, uint32_t sample_rate, std::string* error);
    bool feed(const uint8_t* data, size_t size, int64_t pts_100ns);
    bool receive(AudioChunk& out);
    void drain();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---- encoders (export edge; also used by the test-fixture generator)

struct EncodedPacket {
    std::vector<uint8_t> data;   // H.264: Annex B; AAC: raw frame
    int64_t pts_100ns = 0;
    int64_t duration_100ns = 0;
    bool keyframe = false;
};

class H264Encoder {
public:
    H264Encoder();
    ~H264Encoder();

    bool create(uint32_t width, uint32_t height, uint32_t fps_num,
                uint32_t fps_den, uint32_t bitrate_bps, std::string* error);
    // NV12, tightly packed (stride == width). B-frames are disabled at
    // create time so pts == dts and the muxer needs no ctts.
    bool feed_nv12(const uint8_t* data, int64_t pts_100ns,
                   int64_t duration_100ns);
    bool receive(EncodedPacket& out);
    void drain();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class AacEncoder {
public:
    AacEncoder();
    ~AacEncoder();

    bool create(uint32_t channels, uint32_t sample_rate, uint32_t bitrate_bps,
                std::string* error);
    // AudioSpecificConfig for the muxer's esds (valid after create()).
    const std::vector<uint8_t>& audio_specific_config() const;
    bool feed(const int16_t* samples, size_t count, int64_t pts_100ns);
    bool receive(EncodedPacket& out);
    void drain();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace looks::platform

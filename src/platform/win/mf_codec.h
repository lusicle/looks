// Construct an MfSession one time on each thread that uses MF.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace looks::platform {

// This starts COM in MTA mode and MF. The destructor balances both.
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

// The last MFShutdown must not run on a worker thread with live MFTs.
// This class must not start COM. The main thread stays STA for dialogs.
class MfLifetime {
public:
    MfLifetime();
    ~MfLifetime();
    bool ok() const { return mf_; }

private:
    bool mf_ = false;
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

    // avcc is the raw AVCDecoderConfigurationRecord from the demuxer.
    // low_latency caps the output lag. Playback needs it.
    bool create(const std::vector<uint8_t>& avcc, uint32_t width,
                uint32_t height, std::string* error, bool allow_d3d = true,
                bool low_latency = false);

    // The data is one sample of length-prefixed NALs.
    bool feed(const uint8_t* data, size_t size, int64_t pts_100ns,
              int64_t duration_100ns, bool keyframe);

    // Returns false when the decoder needs more input. That is not an error.
    bool receive(VideoFrameNV12& out);

    // After this, call receive() until it returns false.
    void drain();

    // The next feed() must start at a keyframe.
    void flush();

    // After this, create() can run again.
    void destroy();

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

// Feed this decoder whole MP3 frames.
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

    // A gop_frames of 0 keeps the default keyframe interval.
    bool create(uint32_t width, uint32_t height, uint32_t fps_num,
                uint32_t fps_den, uint32_t bitrate_bps, std::string* error,
                uint32_t gop_frames = 0);
    // The input is NV12, tightly packed, with the stride equal to width.
    // B-frames are off, so pts equals dts and the muxer needs no ctts.
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
    // This is valid only after create() succeeds.
    const std::vector<uint8_t>& audio_specific_config() const;
    bool feed(const int16_t* samples, size_t count, int64_t pts_100ns);
    bool receive(EncodedPacket& out);
    void drain();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace looks::platform

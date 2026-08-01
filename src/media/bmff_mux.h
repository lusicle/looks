// BMFF muxer: one H.264 video track + optional AAC audio track,
// avcC/esds, faststart moov.
//
// Sample payloads stream to a temp .mdat sidecar while metadata accumulates
// in memory; finish() builds moov, shifts chunk offsets, and writes the
// final file as ftyp + moov + mdat (faststart — moov first), then deletes
// the temp. One sample per chunk keeps the tables trivial; moov overhead is
// ~20 bytes/sample, irrelevant next to the media.

#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace looks::media {

struct MuxVideoParams {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t timescale = 90000;
    std::vector<uint8_t> avcc;   // AVCDecoderConfigurationRecord
};

struct MuxAudioParams {
    uint32_t channels = 0;
    uint32_t sample_rate = 0;    // also the track timescale
    uint32_t avg_bitrate = 128000;
    std::vector<uint8_t> audio_specific_config;
};

class BmffMuxer {
public:
    ~BmffMuxer();

    bool open(const std::filesystem::path& path, const MuxVideoParams& video,
              const MuxAudioParams* audio /* null = video only */);

    // AVCC-framed payload (length-prefixed NALs, no SPS/PPS in-band).
    // dts/duration/cts in the video timescale. Call in decode (dts) order.
    bool add_video_sample(const uint8_t* data, size_t size, uint64_t dts,
                          uint32_t duration, int32_t cts_offset, bool keyframe);

    // Raw AAC frame; dts/duration in audio timescale (1024 per AAC frame).
    bool add_audio_sample(const uint8_t* data, size_t size, uint64_t dts,
                          uint32_t duration);

    bool finish();

private:
    struct Sample {
        uint64_t mdat_offset;   // relative to mdat payload start
        uint32_t size;
        uint64_t dts;
        uint32_t duration;
        int32_t cts_offset;
        bool keyframe;
    };
    struct Track {
        std::vector<Sample> samples;
        uint64_t total_duration() const {
            return samples.empty()
                ? 0 : samples.back().dts + samples.back().duration;
        }
    };

    bool append_payload(const uint8_t* data, size_t size, uint64_t* offset);

    std::filesystem::path path_;
    std::filesystem::path temp_path_;
    void* temp_file_ = nullptr;
    MuxVideoParams video_;
    MuxAudioParams audio_;
    bool has_audio_ = false;
    Track video_track_;
    Track audio_track_;
    uint64_t mdat_bytes_ = 0;
    bool finished_ = false;
};

}  // namespace looks::media

// This demuxer does not support fragmented MP4 (moof).
// BmffFile reads sample payloads on demand; it never loads mdat whole.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "util/bytes.h"

namespace looks::media {

// esds descriptor tags; the demuxer and the muxer must share these.
inline constexpr uint8_t kEsdsTagES = 0x03;
inline constexpr uint8_t kEsdsTagDecoderConfig = 0x04;
inline constexpr uint8_t kEsdsTagDecoderSpecific = 0x05;
inline constexpr uint8_t kEsdsTagSLConfig = 0x06;

// Reads a descriptor length of up to 4 continuation bytes.
inline uint32_t esds_read_len(bytes::BeReader& r) {
    uint32_t len = 0;
    for (int i = 0; i < 4; ++i) {
        const uint8_t b = r.u8();
        len = (len << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return len;
}

// Writes the minimal varlen form; it refuses lengths above two bytes.
inline bool esds_write_len(std::vector<uint8_t>& out, size_t len) {
    if (len >= (1u << 14)) return false;
    if (len < 128) {
        out.push_back(static_cast<uint8_t>(len));
        return true;
    }
    out.push_back(static_cast<uint8_t>(0x80 | (len >> 7)));
    out.push_back(static_cast<uint8_t>(len & 0x7F));
    return true;
}

struct SampleInfo {
    uint64_t file_offset = 0;
    uint32_t size = 0;
    uint64_t dts = 0;           // track timescale units
    int64_t cts_offset = 0;     // composition offset (signed, ctts v1)
    uint32_t duration = 0;
    bool keyframe = true;
};

struct TrackInfo {
    enum class Kind : uint8_t { Video, Audio, Other };

    Kind kind = Kind::Other;
    uint32_t track_id = 0;
    uint32_t timescale = 0;
    uint64_t duration = 0;      // track timescale units
    char fourcc[5] = {};        // sample entry type: "avc1", "mp4a", ...

    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> avcc;  // raw AVCDecoderConfigurationRecord

    uint32_t channels = 0;
    uint32_t sample_rate = 0;
    std::vector<uint8_t> audio_specific_config;  // from esds

    std::vector<SampleInfo> samples;
};

struct MovieInfo {
    uint32_t timescale = 0;
    uint64_t duration = 0;
    std::vector<TrackInfo> tracks;

    const TrackInfo* first_video() const;
    const TrackInfo* first_audio() const;
};

// The data must contain the moov box header; unknown boxes skip.
bool parse_moov(const uint8_t* data, size_t size, MovieInfo* out,
                std::string* error);

class BmffFile {
public:
    ~BmffFile();

    bool open(const std::filesystem::path& path, std::string* error);
    void close();

    const MovieInfo& movie() const { return movie_; }

    bool read_sample(const SampleInfo& sample, std::vector<uint8_t>& out);

    // file is a FILE* that is open for binary read.
    static bool read_at(void* file, uint64_t offset, uint32_t size,
                        std::vector<uint8_t>& out);

private:
    void* file_ = nullptr;   // FILE*
    MovieInfo movie_;
};

}  // namespace looks::media

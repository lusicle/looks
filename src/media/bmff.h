// BMFF (MP4/MOV) demuxer — hand-rolled. Parses ftyp/moov and
// expands the sample tables (stts/ctts/stsc/stsz/stco/co64/stss) into flat
// per-sample arrays; extracts avcC (H.264 decoder config) and the esds
// AudioSpecificConfig for the MFT glue. No fragmented MP4 (moof) support —
// out of scope by design.
//
// The parser core works on in-memory bytes (moov is small) so tests can
// feed synthetic fixtures; BmffFile wraps a file handle, locates moov, and
// reads sample payloads on demand — mdat is never loaded wholesale.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "util/bytes.h"

namespace looks::media {

// MP4 elementary-stream descriptors (esds): the tag bytes and the 7-bit
// varlen size coding, shared by this demuxer and the muxer so the two
// sides cannot drift.
inline constexpr uint8_t kEsdsTagES = 0x03;
inline constexpr uint8_t kEsdsTagDecoderConfig = 0x04;
inline constexpr uint8_t kEsdsTagDecoderSpecific = 0x05;
inline constexpr uint8_t kEsdsTagSLConfig = 0x06;

// Reads a descriptor length: up to 4 continuation bytes per spec.
inline uint32_t esds_read_len(bytes::BeReader& r) {
    uint32_t len = 0;
    for (int i = 0; i < 4; ++i) {
        const uint8_t b = r.u8();
        len = (len << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return len;
}

// Writes the minimal varlen form. Refuses payloads whose length needs
// more than two bytes - overflowing the continuation byte silently
// emits a corrupt length the reader then mis-parses.
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

    // Video (avc1)
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> avcc;  // raw AVCDecoderConfigurationRecord

    // Audio (mp4a)
    uint32_t channels = 0;
    uint32_t sample_rate = 0;
    uint32_t sample_size_bits = 0;
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

// Parses a complete moov box (header included). Returns false + error on
// malformed input; tolerates unknown boxes by skipping.
bool parse_moov(const uint8_t* data, size_t size, MovieInfo* out,
                std::string* error);

class BmffFile {
public:
    ~BmffFile();

    // Scans top-level boxes, loads + parses moov. `error` gets a reason on
    // failure. Accepts .mp4/.mov (same box structure).
    bool open(const std::filesystem::path& path, std::string* error);
    void close();

    const MovieInfo& movie() const { return movie_; }

    // Reads one sample's payload from mdat.
    bool read_sample(const SampleInfo& sample, std::vector<uint8_t>& out);

    // Reads `size` bytes at `offset` from an open file - the one sample
    // fetch every consumer (this class, the decode pool's session FILE*)
    // shares.
    static bool read_at(void* file, uint64_t offset, uint32_t size,
                        std::vector<uint8_t>& out);

private:
    void* file_ = nullptr;   // FILE*
    MovieInfo movie_;
};

}  // namespace looks::media

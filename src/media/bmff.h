// BMFF (MP4/MOV) demuxer — hand-rolled (spec §3). Parses ftyp/moov and
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

namespace looks::media {

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

    double duration_seconds() const {
        return timescale ? static_cast<double>(duration) / timescale : 0.0;
    }
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

private:
    void* file_ = nullptr;   // FILE*
    MovieInfo movie_;
};

}  // namespace looks::media

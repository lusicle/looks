// PCM sidecar (.pcm): interleaved s16le with a 16-byte header. Written at
// import, mapped by the player (miniaudio pulls straight from it) and by
// the analysis pass. Little-endian throughout.
//
// Layout: 'PCM1'(4) channels(u16) bits(u16=16) sample_rate(u32)
//         frame_count(u32, patched on finish) then samples.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace looks::media {

class PcmWriter {
public:
    ~PcmWriter();
    bool open(const std::filesystem::path& path, uint32_t channels,
              uint32_t sample_rate);
    bool append(const int16_t* samples, size_t count);   // interleaved
    bool finish();

    uint64_t frames_written() const { return frames_; }

private:
    void* file_ = nullptr;
    uint32_t channels_ = 0;
    uint64_t frames_ = 0;
    bool finished_ = false;
};

class PcmReader {
public:
    ~PcmReader();
    bool open(const std::filesystem::path& path, std::string* error);
    void close();

    uint32_t channels() const { return channels_; }
    uint32_t sample_rate() const { return sample_rate_; }
    uint64_t frame_count() const { return frames_; }
    double duration_seconds() const {
        return sample_rate_ ? static_cast<double>(frames_) / sample_rate_ : 0.0;
    }

    // Reads `frames` frames starting at `first_frame`; zero-fills past EOF.
    // Returns frames actually read from the file.
    size_t read(uint64_t first_frame, int16_t* out, size_t frames);

private:
    void* file_ = nullptr;
    uint32_t channels_ = 0;
    uint32_t sample_rate_ = 0;
    uint64_t frames_ = 0;
};

}  // namespace looks::media

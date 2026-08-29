#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace looks::media {

struct WavData {
    uint32_t channels = 0;
    uint32_t sample_rate = 0;
    std::vector<int16_t> samples;   // interleaved s16
    uint64_t frame_count() const {
        return channels ? samples.size() / channels : 0;
    }
};

bool read_wav(const std::filesystem::path& path, WavData* out,
              std::string* error);

bool write_wav(const std::filesystem::path& path, const int16_t* samples,
               uint64_t frame_count, uint32_t channels, uint32_t sample_rate,
               std::string* error);

}  // namespace looks::media

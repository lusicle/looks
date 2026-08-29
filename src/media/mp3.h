#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace looks::media {

struct Mp3Data {
    uint32_t channels = 0;
    uint32_t sample_rate = 0;
    std::vector<int16_t> samples;   // interleaved s16
    uint64_t frame_count() const {
        return channels ? samples.size() / channels : 0;
    }
};

bool read_mp3(const std::filesystem::path& path, Mp3Data* out,
              std::string* error);

bool decode_mp3(const uint8_t* bytes, size_t size, Mp3Data* out,
                std::string* error);

// Copies the first ID3v2 APIC image bytes, not decoded; false if none.
bool mp3_cover_art(const uint8_t* bytes, size_t size,
                   std::vector<uint8_t>* image);

}  // namespace looks::media

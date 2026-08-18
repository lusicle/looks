// MP3 (MPEG-1/2/2.5 Layer III) import decode: the frame walker here
// owns the container - sync scan, header parse, ID3v2/v1 and Xing/Info
// skip - and whole frames feed the inbox decoder MFT, exactly the split
// the mp4 path uses (BMFF demux in-repo, codec at the platform edge).
// Whole-file decode to interleaved s16, mirroring wav.h. Free-format
// streams are refused.

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

// Decode from memory (the file reader above and the tests share it).
bool decode_mp3(const uint8_t* bytes, size_t size, Mp3Data* out,
                std::string* error);

// Embedded cover art: the first ID3v2 APIC frame's image bytes (JPEG or
// PNG, undecoded). False when the stream carries none.
bool mp3_cover_art(const uint8_t* bytes, size_t size,
                   std::vector<uint8_t>* image);

}  // namespace looks::media

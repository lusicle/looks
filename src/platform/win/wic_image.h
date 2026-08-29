#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace looks::platform {

// The output is tightly packed RGBA8.
bool decode_image_rgba(const uint8_t* bytes, size_t size, uint32_t* width,
                       uint32_t* height, std::vector<uint8_t>* rgba,
                       std::string* error);

}  // namespace looks::platform

// WIC decode for embedded artwork (mp3 cover art is almost always
// JPEG, which the in-repo decoders do not cover). System codec at the
// import edge, same standing as the Media Foundation MFTs.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace looks::platform {

// Decodes an in-memory image (JPEG/PNG/BMP/...) to tightly-packed
// RGBA8. Returns false with `error` set when WIC cannot decode it.
bool decode_image_rgba(const uint8_t* bytes, size_t size, uint32_t* width,
                       uint32_t* height, std::vector<uint8_t>* rgba,
                       std::string* error);

}  // namespace looks::platform

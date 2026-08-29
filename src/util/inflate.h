#pragma once

#include <cstdint>
#include <vector>

namespace looks {

bool inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out,
             size_t expected_size = 0);

bool zlib_inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out,
                  size_t expected_size = 0);

uint32_t adler32(const uint8_t* data, size_t size);
uint32_t crc32(const uint8_t* data, size_t size, uint32_t seed = 0);

}  // namespace looks

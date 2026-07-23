// Hand-rolled DEFLATE decompressor (spec §12: own inflate, no zlib). RFC
// 1951 stored/fixed/dynamic blocks + the RFC 1950 zlib wrapper (adler32
// verified). Used by the PNG reader for glyph atlases, LUT strips, and
// dust/leak/screen textures.

#pragma once

#include <cstdint>
#include <vector>

namespace looks {

// Raw DEFLATE stream -> bytes. Returns false on malformed input.
// `expected_size` reserves the output (0 = unknown).
bool inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out,
             size_t expected_size = 0);

// RFC 1950 zlib stream (2-byte header + deflate + adler32).
bool zlib_inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out,
                  size_t expected_size = 0);

uint32_t adler32(const uint8_t* data, size_t size);
uint32_t crc32(const uint8_t* data, size_t size, uint32_t seed = 0);

}  // namespace looks

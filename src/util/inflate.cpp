#include "util/inflate.h"

#include <cstring>

namespace looks {

namespace {

// LSB-first bit reader (DEFLATE bit order — opposite of the codec bitio).
struct Bits {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;        // bit position
    bool ok = true;

    uint32_t get(int count) {
        uint32_t v = 0;
        for (int i = 0; i < count; ++i) {
            const size_t byte = pos >> 3;
            if (byte >= size) {
                ok = false;
                return 0;
            }
            v |= static_cast<uint32_t>((data[byte] >> (pos & 7)) & 1u) << i;
            ++pos;
        }
        return v;
    }

    void align_byte() { pos = (pos + 7) & ~size_t{7}; }
};

// Canonical Huffman decoder from code lengths (RFC 1951 §3.2.2). Decodes
// bit-by-bit against per-length counts — compact and deterministic.
struct Huffman {
    // count[len] = number of codes of that length; symbols sorted by
    // (length, symbol order).
    uint16_t count[16] = {};
    std::vector<uint16_t> symbols;

    bool build(const uint8_t* lengths, size_t n) {
        for (int i = 0; i < 16; ++i) count[i] = 0;
        for (size_t i = 0; i < n; ++i) count[lengths[i]]++;
        count[0] = 0;
        // Over-subscribed check.
        int left = 1;
        for (int len = 1; len < 16; ++len) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return false;
        }
        std::vector<uint16_t> offsets(16, 0);
        for (int len = 1; len < 15; ++len)
            offsets[len + 1] = static_cast<uint16_t>(offsets[len] + count[len]);
        symbols.assign(n, 0);
        for (size_t i = 0; i < n; ++i)
            if (lengths[i]) symbols[offsets[lengths[i]]++] = static_cast<uint16_t>(i);
        return true;
    }

    int decode(Bits& bits) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= static_cast<int>(bits.get(1));
            if (!bits.ok) return -1;
            const int n = count[len];
            if (code - first < n) return symbols[index + (code - first)];
            index += n;
            first = (first + n) << 1;
            code <<= 1;
        }
        return -1;
    }
};

const uint16_t kLengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13,
                                 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2,
                                  2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25,
                                33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                1025, 1537, 2049, 3073, 4097, 6145, 8193,
                                12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12,
                                13, 13};

bool inflate_block(Bits& bits, const Huffman& lit, const Huffman& dist,
                   std::vector<uint8_t>& out) {
    for (;;) {
        const int sym = lit.decode(bits);
        if (sym < 0) return false;
        if (sym < 256) {
            out.push_back(static_cast<uint8_t>(sym));
        } else if (sym == 256) {
            return true;
        } else {
            const int li = sym - 257;
            if (li >= 29) return false;
            const uint32_t length =
                kLengthBase[li] + bits.get(kLengthExtra[li]);
            const int ds = dist.decode(bits);
            if (ds < 0 || ds >= 30) return false;
            const uint32_t distance = kDistBase[ds] + bits.get(kDistExtra[ds]);
            if (!bits.ok || distance > out.size()) return false;
            const size_t start = out.size() - distance;
            for (uint32_t i = 0; i < length; ++i)
                out.push_back(out[start + i]);   // may overlap — by design
        }
    }
}

}  // namespace

bool inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out,
             size_t expected_size) {
    out.clear();
    if (expected_size) out.reserve(expected_size);
    Bits bits{data, size};

    for (;;) {
        const uint32_t final_block = bits.get(1);
        const uint32_t type = bits.get(2);
        if (!bits.ok) return false;

        if (type == 0) {
            // Stored: byte-aligned LEN/NLEN + raw bytes.
            bits.align_byte();
            const size_t byte = bits.pos >> 3;
            if (byte + 4 > size) return false;
            const uint32_t len = data[byte] | (data[byte + 1] << 8);
            const uint32_t nlen = data[byte + 2] | (data[byte + 3] << 8);
            if ((len ^ nlen) != 0xFFFF) return false;
            if (byte + 4 + len > size) return false;
            out.insert(out.end(), data + byte + 4, data + byte + 4 + len);
            bits.pos = (byte + 4 + len) << 3;
        } else if (type == 1) {
            // Fixed trees (RFC 1951 §3.2.6).
            uint8_t lit_lengths[288];
            for (int i = 0; i < 144; ++i) lit_lengths[i] = 8;
            for (int i = 144; i < 256; ++i) lit_lengths[i] = 9;
            for (int i = 256; i < 280; ++i) lit_lengths[i] = 7;
            for (int i = 280; i < 288; ++i) lit_lengths[i] = 8;
            uint8_t dist_lengths[30];
            for (int i = 0; i < 30; ++i) dist_lengths[i] = 5;
            Huffman lit, dist;
            if (!lit.build(lit_lengths, 288) || !dist.build(dist_lengths, 30))
                return false;
            if (!inflate_block(bits, lit, dist, out)) return false;
        } else if (type == 2) {
            // Dynamic trees.
            const uint32_t hlit = bits.get(5) + 257;
            const uint32_t hdist = bits.get(5) + 1;
            const uint32_t hclen = bits.get(4) + 4;
            if (!bits.ok || hlit > 286 || hdist > 30) return false;
            static const uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10,
                                               5, 11, 4, 12, 3, 13, 2, 14, 1,
                                               15};
            uint8_t cl_lengths[19] = {};
            for (uint32_t i = 0; i < hclen; ++i)
                cl_lengths[kOrder[i]] = static_cast<uint8_t>(bits.get(3));
            Huffman cl;
            if (!bits.ok || !cl.build(cl_lengths, 19)) return false;

            uint8_t lengths[286 + 30] = {};
            uint32_t n = 0;
            while (n < hlit + hdist) {
                const int sym = cl.decode(bits);
                if (sym < 0) return false;
                if (sym < 16) {
                    lengths[n++] = static_cast<uint8_t>(sym);
                } else if (sym == 16) {
                    if (n == 0) return false;
                    const uint32_t repeat = 3 + bits.get(2);
                    const uint8_t prev = lengths[n - 1];
                    for (uint32_t i = 0; i < repeat && n < hlit + hdist; ++i)
                        lengths[n++] = prev;
                } else if (sym == 17) {
                    const uint32_t repeat = 3 + bits.get(3);
                    for (uint32_t i = 0; i < repeat && n < hlit + hdist; ++i)
                        lengths[n++] = 0;
                } else {
                    const uint32_t repeat = 11 + bits.get(7);
                    for (uint32_t i = 0; i < repeat && n < hlit + hdist; ++i)
                        lengths[n++] = 0;
                }
                if (!bits.ok) return false;
            }
            Huffman lit, dist;
            if (!lit.build(lengths, hlit) ||
                !dist.build(lengths + hlit, hdist))
                return false;
            if (!inflate_block(bits, lit, dist, out)) return false;
        } else {
            return false;
        }

        if (final_block) return true;
    }
}

uint32_t adler32(const uint8_t* data, size_t size) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < size; ++i) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

uint32_t crc32(const uint8_t* data, size_t size, uint32_t seed) {
    static uint32_t table[256];
    static bool table_built = false;
    if (!table_built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        table_built = true;
    }
    uint32_t crc = seed ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

bool zlib_inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out,
                  size_t expected_size) {
    if (size < 6) return false;
    const uint8_t cmf = data[0];
    const uint8_t flg = data[1];
    if ((cmf & 0x0F) != 8) return false;              // deflate only
    if (((cmf << 8) | flg) % 31 != 0) return false;   // header check
    if (flg & 0x20) return false;                     // FDICT unsupported
    if (!inflate(data + 2, size - 6, out, expected_size)) return false;
    const uint32_t expect = (static_cast<uint32_t>(data[size - 4]) << 24) |
                            (data[size - 3] << 16) | (data[size - 2] << 8) |
                            data[size - 1];
    return adler32(out.data(), out.size()) == expect;
}

}  // namespace looks

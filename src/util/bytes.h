#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace looks::bytes {

// No bounds check. The caller must guarantee the span.

inline uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
inline uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
inline uint64_t be64(const uint8_t* p) {
    return (static_cast<uint64_t>(be32(p)) << 32) | be32(p + 4);
}
inline uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
inline uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t le64(const uint8_t* p) {
    return static_cast<uint64_t>(le32(p)) |
           (static_cast<uint64_t>(le32(p + 4)) << 32);
}

inline void put_be32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}
inline void put_le16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
inline void put_le32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}
inline void put_le64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (i * 8));
}

struct BeReader {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;
    bool ok = true;

    bool has(size_t n) const { return ok && pos + n <= size; }
    void fail() { ok = false; }

    uint8_t u8() {
        if (!has(1)) { fail(); return 0; }
        return data[pos++];
    }
    uint16_t u16() {
        if (!has(2)) { fail(); return 0; }
        const uint16_t v = be16(data + pos);
        pos += 2;
        return v;
    }
    uint32_t u32() {
        if (!has(4)) { fail(); return 0; }
        const uint32_t v = be32(data + pos);
        pos += 4;
        return v;
    }
    uint64_t u64() {
        if (!has(8)) { fail(); return 0; }
        const uint64_t v = be64(data + pos);
        pos += 8;
        return v;
    }
    void skip(size_t n) {
        if (!has(n)) { fail(); return; }
        pos += n;
    }
    void bytes(void* out, size_t n) {
        if (!has(n)) { fail(); std::memset(out, 0, n); return; }
        std::memcpy(out, data + pos, n);
        pos += n;
    }
};

struct LeReader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;

    uint32_t u32() {
        if (end - p < 4) { ok = false; return 0; }
        const uint32_t v = le32(p);
        p += 4;
        return v;
    }
    float f32() {
        const uint32_t bits = u32();
        float v;
        std::memcpy(&v, &bits, 4);
        return v;
    }
    double f64() {
        if (end - p < 8) { ok = false; return 0; }
        double v;
        std::memcpy(&v, p, 8);
        p += 8;
        return v;
    }
};

struct BeWriter {
    std::vector<uint8_t> v;

    void u8(uint32_t x) { v.push_back(static_cast<uint8_t>(x)); }
    void u16(uint32_t x) { u8(x >> 8); u8(x); }
    void u32(uint32_t x) { u16(x >> 16); u16(x); }
    void u64(uint64_t x) {
        u32(static_cast<uint32_t>(x >> 32));
        u32(static_cast<uint32_t>(x));
    }
    void raw(const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        v.insert(v.end(), p, p + n);
    }
    void zeros(size_t n) { v.insert(v.end(), n, 0); }
    void append(const BeWriter& o) {
        v.insert(v.end(), o.v.begin(), o.v.end());
    }
};

inline void app_be16(std::vector<uint8_t>& out, uint16_t x) {
    out.push_back(static_cast<uint8_t>(x >> 8));
    out.push_back(static_cast<uint8_t>(x));
}
inline void app_be32(std::vector<uint8_t>& out, uint32_t x) {
    out.push_back(static_cast<uint8_t>(x >> 24));
    out.push_back(static_cast<uint8_t>(x >> 16));
    out.push_back(static_cast<uint8_t>(x >> 8));
    out.push_back(static_cast<uint8_t>(x));
}
inline void app_utf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xF0 | (cp >> 18));
        s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
}
inline void app_le32(std::vector<uint8_t>& out, uint32_t x) {
    out.push_back(static_cast<uint8_t>(x));
    out.push_back(static_cast<uint8_t>(x >> 8));
    out.push_back(static_cast<uint8_t>(x >> 16));
    out.push_back(static_cast<uint8_t>(x >> 24));
}
inline void app_f32(std::vector<uint8_t>& out, float x) {
    uint32_t bits;
    std::memcpy(&bits, &x, 4);
    app_le32(out, bits);
}

}  // namespace looks::bytes

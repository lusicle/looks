#include "util/image.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "util/bytes.h"
#include "util/file.h"
#include "util/inflate.h"

namespace looks {

namespace {

void set_error(std::string* error, const char* message) {
    if (error) *error = message;
}

using bytes::be32;

int paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a);
    const int pb = std::abs(p - b);
    const int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

}  // namespace

bool decode_png(const uint8_t* data, size_t size, ImageRgba* out,
                std::string* error) {
    static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A,
                                    '\n'};
    if (size < 8 + 25 || std::memcmp(data, kSig, 8) != 0) {
        set_error(error, "not a PNG");
        return false;
    }

    uint32_t width = 0, height = 0;
    uint8_t color_type = 0;
    uint32_t channels = 0;
    std::vector<uint8_t> idat;
    bool saw_ihdr = false, saw_iend = false;

    size_t pos = 8;
    while (pos + 12 <= size && !saw_iend) {
        const uint32_t length = be32(data + pos);
        if (pos + 12 + length > size) {
            set_error(error, "truncated chunk");
            return false;
        }
        const uint8_t* type = data + pos + 4;
        const uint8_t* payload = data + pos + 8;
        const uint32_t crc_expect = be32(payload + length);
        if (crc32(type, 4 + length) != crc_expect) {
            set_error(error, "chunk CRC mismatch");
            return false;
        }

        if (std::memcmp(type, "IHDR", 4) == 0) {
            if (length != 13) {
                set_error(error, "bad IHDR");
                return false;
            }
            width = be32(payload);
            height = be32(payload + 4);
            const uint8_t bit_depth = payload[8];
            color_type = payload[9];
            const uint8_t interlace = payload[12];
            if (width == 0 || height == 0 || width > 16384 || height > 16384) {
                set_error(error, "unsupported dimensions");
                return false;
            }
            if (bit_depth != 8) {
                set_error(error, "only 8-bit PNGs supported");
                return false;
            }
            if (color_type == 0) channels = 1;        // gray
            else if (color_type == 2) channels = 3;   // rgb
            else if (color_type == 6) channels = 4;   // rgba
            else {
                set_error(error, "unsupported color type (use gray/RGB/RGBA)");
                return false;
            }
            if (interlace != 0) {
                set_error(error, "interlaced PNGs unsupported");
                return false;
            }
            saw_ihdr = true;
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            idat.insert(idat.end(), payload, payload + length);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            saw_iend = true;
        } else if (!(type[0] & 0x20)) {
            // Bit 0x20 marks an ancillary chunk. Unknown critical fails.
            set_error(error, "unsupported critical chunk");
            return false;
        }
        pos += 12 + length;
    }

    if (!saw_ihdr || !saw_iend || idat.empty()) {
        set_error(error, "missing IHDR/IDAT/IEND");
        return false;
    }

    const size_t row_bytes = static_cast<size_t>(width) * channels;
    const size_t raw_size = (row_bytes + 1) * height;
    std::vector<uint8_t> raw;
    if (!zlib_inflate(idat.data(), idat.size(), raw, raw_size) ||
        raw.size() != raw_size) {
        set_error(error, "IDAT inflate failed");
        return false;
    }

    out->width = width;
    out->height = height;
    out->pixels.assign(static_cast<size_t>(width) * height * 4, 255);
    std::vector<uint8_t> prev_row(row_bytes, 0);
    std::vector<uint8_t> row(row_bytes, 0);

    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* src = raw.data() + static_cast<size_t>(y) * (row_bytes + 1);
        const uint8_t filter = src[0];
        const uint8_t* line = src + 1;
        const uint32_t bpp = channels;
        for (size_t x = 0; x < row_bytes; ++x) {
            const int a = x >= bpp ? row[x - bpp] : 0;
            const int b = prev_row[x];
            const int c = x >= bpp ? prev_row[x - bpp] : 0;
            int v = line[x];
            switch (filter) {
                case 0: break;
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: v += paeth(a, b, c); break;
                default:
                    set_error(error, "bad filter type");
                    return false;
            }
            row[x] = static_cast<uint8_t>(v);
        }
        uint8_t* dst = out->pixels.data() + static_cast<size_t>(y) * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* px = row.data() + static_cast<size_t>(x) * channels;
            if (channels == 1) {
                dst[x * 4 + 0] = dst[x * 4 + 1] = dst[x * 4 + 2] = px[0];
                dst[x * 4 + 3] = 255;
            } else if (channels == 3) {
                dst[x * 4 + 0] = px[0];
                dst[x * 4 + 1] = px[1];
                dst[x * 4 + 2] = px[2];
                dst[x * 4 + 3] = 255;
            } else {
                std::memcpy(dst + x * 4, px, 4);
            }
        }
        std::swap(prev_row, row);
    }
    return true;
}

bool decode_tga(const uint8_t* data, size_t size, ImageRgba* out,
                std::string* error) {
    if (size < 18) {
        set_error(error, "truncated TGA");
        return false;
    }
    const uint8_t id_length = data[0];
    const uint8_t color_map_type = data[1];
    const uint8_t image_type = data[2];
    const uint32_t width = bytes::le16(data + 12);
    const uint32_t height = bytes::le16(data + 14);
    const uint8_t bpp = data[16];
    const bool top_origin = (data[17] & 0x20) != 0;

    if (color_map_type != 0 || (image_type != 2 && image_type != 10) ||
        (bpp != 24 && bpp != 32) || width == 0 || height == 0) {
        set_error(error, "unsupported TGA (truecolor 24/32 only)");
        return false;
    }

    const uint32_t bytes_pp = bpp / 8;
    const size_t pixel_count = static_cast<size_t>(width) * height;
    std::vector<uint8_t> bgra(pixel_count * bytes_pp);
    size_t pos = 18 + id_length;

    if (image_type == 2) {
        if (pos + bgra.size() > size) {
            set_error(error, "truncated TGA data");
            return false;
        }
        std::memcpy(bgra.data(), data + pos, bgra.size());
    } else {
        size_t written = 0;
        while (written < bgra.size()) {
            if (pos >= size) {
                set_error(error, "truncated TGA RLE");
                return false;
            }
            const uint8_t header = data[pos++];
            const uint32_t count = (header & 0x7F) + 1;
            if (header & 0x80) {
                if (pos + bytes_pp > size) {
                    set_error(error, "truncated TGA RLE");
                    return false;
                }
                for (uint32_t i = 0; i < count && written < bgra.size(); ++i) {
                    std::memcpy(bgra.data() + written, data + pos, bytes_pp);
                    written += bytes_pp;
                }
                pos += bytes_pp;
            } else {
                const size_t bytes = static_cast<size_t>(count) * bytes_pp;
                if (pos + bytes > size) {
                    set_error(error, "truncated TGA RLE");
                    return false;
                }
                const size_t n = std::min(bytes, bgra.size() - written);
                std::memcpy(bgra.data() + written, data + pos, n);
                written += n;
                pos += bytes;
            }
        }
    }

    out->width = width;
    out->height = height;
    out->pixels.resize(pixel_count * 4);
    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t src_y = top_origin ? y : height - 1 - y;
        const uint8_t* src =
            bgra.data() + static_cast<size_t>(src_y) * width * bytes_pp;
        uint8_t* dst = out->pixels.data() + static_cast<size_t>(y) * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            dst[x * 4 + 0] = src[x * bytes_pp + 2];
            dst[x * 4 + 1] = src[x * bytes_pp + 1];
            dst[x * 4 + 2] = src[x * bytes_pp + 0];
            dst[x * 4 + 3] = bytes_pp == 4 ? src[x * bytes_pp + 3] : 255;
        }
    }
    return true;
}

bool load_image(const std::filesystem::path& path, ImageRgba* out,
                std::string* error) {
    auto bytes = read_file_bytes(path);
    if (!bytes) {
        set_error(error, "cannot read file");
        return false;
    }
    const auto ext = path.extension();
    if (ext == ".png" || ext == ".PNG")
        return decode_png(bytes->data(), bytes->size(), out, error);
    if (ext == ".tga" || ext == ".TGA")
        return decode_tga(bytes->data(), bytes->size(), out, error);
    set_error(error, "unsupported image extension");
    return false;
}

namespace {

void put_be32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void put_chunk(std::vector<uint8_t>& out, const char type[4],
               const uint8_t* payload, size_t size) {
    put_be32(out, static_cast<uint32_t>(size));
    const size_t crc_start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload, payload + size);
    put_be32(out, crc32(out.data() + crc_start, 4 + size));
}

}  // namespace

std::vector<uint8_t> encode_png_rgba(const uint8_t* rgba, uint32_t width,
                                     uint32_t height) {
    std::vector<uint8_t> out;
    if (!rgba || !width || !height) return out;
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    out.insert(out.end(), sig, sig + 8);

    uint8_t ihdr[13];
    ihdr[0] = static_cast<uint8_t>(width >> 24);
    ihdr[1] = static_cast<uint8_t>(width >> 16);
    ihdr[2] = static_cast<uint8_t>(width >> 8);
    ihdr[3] = static_cast<uint8_t>(width);
    ihdr[4] = static_cast<uint8_t>(height >> 24);
    ihdr[5] = static_cast<uint8_t>(height >> 16);
    ihdr[6] = static_cast<uint8_t>(height >> 8);
    ihdr[7] = static_cast<uint8_t>(height);
    ihdr[8] = 8;    // bit depth
    ihdr[9] = 6;    // color type RGBA
    ihdr[10] = 0;   // compression
    ihdr[11] = 0;   // filter method
    ihdr[12] = 0;   // no interlace
    put_chunk(out, "IHDR", ihdr, sizeof(ihdr));

    const size_t row = size_t{width} * 4;
    std::vector<uint8_t> raw((row + 1) * height);
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* dst = raw.data() + (row + 1) * y;
        dst[0] = 0;
        std::memcpy(dst + 1, rgba + row * y, row);
    }

    // A zlib header, then stored deflate blocks with no compression.
    std::vector<uint8_t> z;
    z.reserve(raw.size() + raw.size() / 65535 * 5 + 16);
    z.push_back(0x78);
    z.push_back(0x01);
    size_t pos = 0;
    while (pos < raw.size()) {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n & 0xFF));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n & 0xFF));
        z.push_back(static_cast<uint8_t>((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + static_cast<ptrdiff_t>(pos),
                 raw.begin() + static_cast<ptrdiff_t>(pos + n));
        pos += n;
    }
    put_be32(z, adler32(raw.data(), raw.size()));
    put_chunk(out, "IDAT", z.data(), z.size());
    put_chunk(out, "IEND", nullptr, 0);
    return out;
}

bool write_png(const std::filesystem::path& path, const uint8_t* rgba,
               uint32_t width, uint32_t height) {
    const std::vector<uint8_t> bytes = encode_png_rgba(rgba, width, height);
    if (bytes.empty()) return false;
    return write_file_bytes(path, bytes.data(), bytes.size());
}

}  // namespace looks

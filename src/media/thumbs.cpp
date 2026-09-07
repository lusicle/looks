#include "media/thumbs.h"

#include <cstdio>
#include <cstring>

#include "util/bytes.h"
#include "util/file.h"

namespace looks::media {

bool write_thumbs(const std::filesystem::path& path,
                  const ThumbStripData& strip) {
    if (strip.count == 0 || strip.w == 0 || strip.h == 0) return false;
    if (strip.w > 0xFFFF || strip.h > 0xFFFF || strip.count > 0xFFFF)
        return false;
    if (strip.rgb.size() !=
        static_cast<size_t>(strip.w) * strip.h * 3 * strip.count)
        return false;
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    uint8_t header[10];
    if (!strip.alpha.empty() && strip.alpha.size() * 3 != strip.rgb.size()) {
        std::fclose(f);
        return false;
    }
    std::memcpy(header, !strip.alpha.empty() ? "THM3" : (strip.linear_filtered ? "THM2" : "THM1"), 4);
    bytes::put_le16(header + 4, static_cast<uint16_t>(strip.w));
    bytes::put_le16(header + 6, static_cast<uint16_t>(strip.h));
    bytes::put_le16(header + 8, static_cast<uint16_t>(strip.count));
    const bool ok =
        std::fwrite(header, 1, 10, f) == 10 &&
        std::fwrite(strip.rgb.data(), 1, strip.rgb.size(), f) ==
            strip.rgb.size() &&
        (strip.alpha.empty() || std::fwrite(strip.alpha.data(), 1, strip.alpha.size(), f) == strip.alpha.size());
    std::fclose(f);
    return ok;
}

bool read_thumbs(const std::filesystem::path& path, ThumbStripData* out) {
    const auto data = read_file_bytes(path);
    if (!data || data->size() <= 10 ||
        (std::memcmp(data->data(), "THM1", 4) != 0 &&
         std::memcmp(data->data(), "THM2", 4) != 0 &&
         std::memcmp(data->data(), "THM3", 4) != 0))
        return false;
    const uint8_t* p = data->data();
    const uint32_t w = bytes::le16(p + 4);
    const uint32_t h = bytes::le16(p + 6);
    const uint32_t count = bytes::le16(p + 8);
    const size_t need = 10 + static_cast<size_t>(w) * h * 3 * count;
    if (!w || !h || !count || data->size() < need) return false;
    out->w = w;
    out->h = h;
    out->count = count;
    out->linear_filtered = std::memcmp(p, "THM1", 4) != 0;
    out->rgb.assign(p + 10, p + need);
    out->alpha.clear();
    if (std::memcmp(p, "THM3", 4) == 0) {
        const size_t pixels = size_t(w) * h * count;
        if (data->size() < need + pixels) return false;
        out->alpha.assign(p + need, p + need + pixels);
    }
    return true;
}

bool read_thumbs_header(const std::filesystem::path& path,
                        ThumbStripData* out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    uint8_t header[10];
    const bool got = std::fread(header, 1, 10, f) == 10;
    std::fclose(f);
    if (!got || (std::memcmp(header, "THM1", 4) != 0 &&
                 std::memcmp(header, "THM2", 4) != 0 &&
                 std::memcmp(header, "THM3", 4) != 0)) return false;
    out->w = bytes::le16(header + 4);
    out->h = bytes::le16(header + 6);
    out->count = bytes::le16(header + 8);
    out->linear_filtered = std::memcmp(header, "THM1", 4) != 0;
    out->rgb.clear();
    out->alpha.clear();
    return out->w && out->h && out->count;
}

}  // namespace looks::media

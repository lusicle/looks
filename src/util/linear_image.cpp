#include "util/linear_image.h"

#include <cstring>

#include "util/file.h"

namespace looks {

bool load_linear_image(const std::filesystem::path& path, LinearImage& image) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size < 16 || size > (1ull << 30) + 16) return false;
    const auto data = read_file_bytes(path);
    if (!data || data->size() < 16) return false;
    uint32_t header[4];
    std::memcpy(header, data->data(), sizeof(header));
    if (header[0] != 0x4641524c || header[1] != 1 || !header[2] || !header[3]) return false;
    const uint64_t bytes = uint64_t(header[2]) * header[3] * 8;
    if (bytes > (1ull << 30) || bytes + 16 != data->size()) return false;
    image.width = header[2];
    image.height = header[3];
    image.pixels.resize(size_t(bytes / 2));
    std::memcpy(image.pixels.data(), data->data() + 16, size_t(bytes));
    return true;
}

bool write_linear_image(const std::filesystem::path& path, const LinearImage& image) {
    if (!image.width || !image.height ||
        uint64_t(image.width) * image.height * 4 != image.pixels.size() ||
        image.pixels.size() > (1ull << 29)) return false;
    const uint32_t header[] = {0x4641524c, 1, image.width, image.height};
    std::vector<uint8_t> data(16 + image.pixels.size() * 2);
    std::memcpy(data.data(), header, 16);
    std::memcpy(data.data() + 16, image.pixels.data(), image.pixels.size() * 2);
    return write_file_bytes(path, data.data(), data.size());
}

}

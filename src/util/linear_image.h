#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace looks {

struct LinearImage {
    uint32_t width = 0, height = 0;
    std::vector<uint16_t> pixels;
};

bool load_linear_image(const std::filesystem::path& path, LinearImage& image);
bool write_linear_image(const std::filesystem::path& path, const LinearImage& image);

}

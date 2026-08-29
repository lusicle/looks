#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace looks {

std::optional<std::vector<uint8_t>> read_file_bytes(const std::filesystem::path& path);
bool write_file_bytes(const std::filesystem::path& path, const void* data, size_t size);

std::filesystem::path executable_dir();

// These conversions are total. Invalid input becomes U+FFFD.
std::string path_to_u8(const std::filesystem::path& path);
std::filesystem::path u8_to_path(std::string_view utf8);

}  // namespace looks

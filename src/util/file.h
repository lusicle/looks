// Small file helpers used across the app.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace looks {

std::optional<std::vector<uint8_t>> read_file_bytes(const std::filesystem::path& path);
bool write_file_bytes(const std::filesystem::path& path, const void* data, size_t size);

// Directory containing the running executable (shaders/ is staged there).
std::filesystem::path executable_dir();

}  // namespace looks

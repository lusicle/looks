// Small file helpers used across the app.

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

// Directory containing the running executable (shaders/ is staged there).
std::filesystem::path executable_dir();

// UTF-8 <-> filesystem path, hand-rolled and TOTAL. Narrow strings
// that name files are UTF-8 everywhere in this codebase (documents,
// script, drop events, UI text); the system codepage cannot spell
// most of Unicode, and the standard narrowing is lossy or throwing
// for what it cannot spell. Invalid input decodes to U+FFFD - a
// mangled name yields a wrong-but-harmless path, never a crash.
std::string path_to_u8(const std::filesystem::path& path);
std::filesystem::path u8_to_path(std::string_view utf8);

}  // namespace looks

#include "util/file.h"

#include <windows.h>

#include <cstdio>

namespace looks {

std::optional<std::vector<uint8_t>> read_file_bytes(const std::filesystem::path& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return std::nullopt;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(size > 0 ? static_cast<size_t>(size) : 0);
    if (!data.empty() && std::fread(data.data(), 1, data.size(), f) != data.size()) {
        std::fclose(f);
        return std::nullopt;
    }
    std::fclose(f);
    return data;
}

bool write_file_bytes(const std::filesystem::path& path, const void* data, size_t size) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    const bool ok = std::fwrite(data, 1, size, f) == size;
    std::fclose(f);
    return ok;
}

std::filesystem::path executable_dir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::filesystem::path exe(std::wstring(buf, n));
    return exe.parent_path();
}

}  // namespace looks

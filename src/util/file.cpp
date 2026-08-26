#include "util/file.h"

#include <windows.h>

#include <cstdio>

namespace looks {

std::optional<std::vector<uint8_t>> read_file_bytes(const std::filesystem::path& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return std::nullopt;
    _fseeki64(f, 0, SEEK_END);
    const int64_t size = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
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

std::string path_to_u8(const std::filesystem::path& path) {
    const std::wstring& w = path.native();   // UTF-16 on Windows
    std::string out;
    out.reserve(w.size());
    auto put = [&](uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    };
    for (size_t i = 0; i < w.size();) {
        uint32_t cp = static_cast<uint16_t>(w[i++]);
        if (cp >= 0xD800 && cp <= 0xDBFF && i < w.size() &&
            static_cast<uint16_t>(w[i]) >= 0xDC00 &&
            static_cast<uint16_t>(w[i]) <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) +
                 (static_cast<uint16_t>(w[i++]) - 0xDC00);
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;   // unpaired surrogate
        }
        put(cp);
    }
    return out;
}

std::filesystem::path u8_to_path(std::string_view utf8) {
    std::wstring w;
    w.reserve(utf8.size());
    const auto* b = reinterpret_cast<const uint8_t*>(utf8.data());
    const size_t n = utf8.size();
    for (size_t i = 0; i < n;) {
        uint32_t cp = 0xFFFD;
        const uint8_t b0 = b[i];
        auto cont = [&](size_t k) {
            return i + k < n && (b[i + k] & 0xC0) == 0x80;
        };
        if (b0 < 0x80) {
            cp = b0;
            i += 1;
        } else if ((b0 & 0xE0) == 0xC0 && cont(1)) {
            cp = (static_cast<uint32_t>(b0 & 0x1F) << 6) | (b[i + 1] & 0x3F);
            if (cp < 0x80) cp = 0xFFFD;   // overlong
            i += 2;
        } else if ((b0 & 0xF0) == 0xE0 && cont(1) && cont(2)) {
            cp = (static_cast<uint32_t>(b0 & 0x0F) << 12) |
                 (static_cast<uint32_t>(b[i + 1] & 0x3F) << 6) |
                 (b[i + 2] & 0x3F);
            if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
            i += 3;
        } else if ((b0 & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
            cp = (static_cast<uint32_t>(b0 & 0x07) << 18) |
                 (static_cast<uint32_t>(b[i + 1] & 0x3F) << 12) |
                 (static_cast<uint32_t>(b[i + 2] & 0x3F) << 6) |
                 (b[i + 3] & 0x3F);
            if (cp < 0x10000 || cp > 0x10FFFF) cp = 0xFFFD;
            i += 4;
        } else {
            i += 1;   // stray byte
        }
        if (cp >= 0x10000) {
            const uint32_t v = cp - 0x10000;
            w.push_back(static_cast<wchar_t>(0xD800 + (v >> 10)));
            w.push_back(static_cast<wchar_t>(0xDC00 + (v & 0x3FF)));
        } else {
            w.push_back(static_cast<wchar_t>(cp));
        }
    }
    return std::filesystem::path(std::move(w));
}

}  // namespace looks

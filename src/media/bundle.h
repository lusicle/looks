#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace looks::media {

struct AssetBundle {
    uint64_t asset = 0;
    // Set only one path: native reads the source file, mez the mezzanine.
    // Two empty paths mean the asset has no image.
    std::filesystem::path mez;
    std::filesystem::path native;
    std::filesystem::path pcm;   // empty = silent
    uint32_t frames = 0;
    uint32_t width = 0, height = 0;
    double fps = 0.0;
    // Use the ratio for the exact rate; a double cannot hold 30000/1001.
    uint32_t timescale = 0, frame_duration = 0;
};

inline const AssetBundle* find_bundle(const std::vector<AssetBundle>& table,
                                      uint64_t asset) {
    for (const AssetBundle& b : table)
        if (b.asset == asset) return &b;
    return nullptr;
}

// All sidecars keep one stem; readers swap extensions to find siblings.
struct SidecarPaths {
    std::filesystem::path base;
    std::filesystem::path mez;
    std::filesystem::path proxy;
    std::filesystem::path pcm;
    std::filesystem::path analysis;
    std::filesystem::path thumbs;
    std::filesystem::path track;
};

inline SidecarPaths sidecars_for_stem(const std::filesystem::path& dir,
                                      const std::wstring& stem) {
    SidecarPaths s;
    s.base = dir / (stem + L".media");
    s.mez = dir / (stem + L".mez");
    s.proxy = dir / (stem + L".proxy.mez");
    s.pcm = dir / (stem + L".pcm");
    s.analysis = dir / (stem + L".analysis");
    s.thumbs = dir / (stem + L".thumbs");
    s.track = dir / (stem + L".track");
    return s;
}

inline SidecarPaths sidecars_for(const std::filesystem::path& dir,
                                 const std::filesystem::path& source) {
    return sidecars_for_stem(dir, source.stem().wstring());
}

}  // namespace looks::media

// Asset bundles: where an imported asset's media actually lives on this
// machine. doc::Asset stores the SOURCE path the user picked; the .mez /
// .pcm / .proxy files sit in the scratch cache next to the exe, and which
// of them to open depends on the proxy toggle. That resolution is app
// policy (it owns the cache root and the import flow), so the app publishes
// a table of resolved bundles and both consumers - the decode pool and the
// audio mix - read it instead of re-deriving paths.

#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace looks::media {

struct AssetBundle {
    uint64_t asset = 0;
    // Exactly one of the two video paths is set for image-bearing media:
    // `native` points at the SOURCE file itself (mp4/mov, decoded in
    // place by the pool's H.264 sessions); `mez` carries the mezzanine
    // for what still transcodes (stills, audio cover art, direct .mez).
    // Both empty = image-dormant (audio-only) or nothing decodable yet.
    std::filesystem::path mez;
    std::filesystem::path native;
    std::filesystem::path pcm;   // empty = silent
    uint32_t frames = 0;
    uint32_t width = 0, height = 0;
    double fps = 0.0;
    // The bundle's exact rate as a ratio (30000/1001 and friends): the
    // export mux wants integers, and a double cannot spell them.
    uint32_t timescale = 0, frame_duration = 0;
};

inline const AssetBundle* find_bundle(const std::vector<AssetBundle>& table,
                                      uint64_t asset) {
    for (const AssetBundle& b : table)
        if (b.asset == asset) return &b;
    return nullptr;
}

}  // namespace looks::media

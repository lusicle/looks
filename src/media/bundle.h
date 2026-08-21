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

// Sidecar naming for one source, spelled once: the ingest writes these
// files and resolve_bundle rebinds them. `base` is the app's stored
// anchor - swapping its extension reproduces any sibling, which the
// load-time readers rely on; that equivalence holds by construction
// because every field derives from the same stem.
struct SidecarPaths {
    std::filesystem::path base;      // <stem>.media (the anchor)
    std::filesystem::path mez;       // <stem>.mez
    std::filesystem::path proxy;     // <stem>.proxy.mez
    std::filesystem::path pcm;       // <stem>.pcm
    std::filesystem::path analysis;  // <stem>.analysis
    std::filesystem::path thumbs;    // <stem>.thumbs
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
    return s;
}

inline SidecarPaths sidecars_for(const std::filesystem::path& dir,
                                 const std::filesystem::path& source) {
    return sidecars_for_stem(dir, source.stem().wstring());
}

}  // namespace looks::media

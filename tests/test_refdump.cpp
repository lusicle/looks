// Dev harness, not a correctness test: when temp/example.mp4 exists,
// import it once (bundle cached under temp/example_ref/) and dump the
// first second as BMP frames into temp/ref_frames/ so reference footage
// can be examined frame by frame. Passes trivially when the file is
// absent, so CI and other machines never notice it.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "codec/mez.h"
#include "media/import.h"
#include "test_framework.h"

namespace {

// Minimal 24-bit BMP writer (BGR, bottom-up, 4-byte row padding).
bool write_bmp(const std::filesystem::path& path, const uint8_t* rgb,
               uint32_t w, uint32_t h) {
    const uint32_t row = (w * 3 + 3) & ~3u;
    const uint32_t pixel_bytes = row * h;
    const uint32_t off = 14 + 40;
    const uint32_t total = off + pixel_bytes;
    std::vector<uint8_t> out(total, 0);
    uint8_t* p = out.data();
    auto put16 = [&](size_t at, uint16_t v) {
        p[at] = static_cast<uint8_t>(v);
        p[at + 1] = static_cast<uint8_t>(v >> 8);
    };
    auto put32 = [&](size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i)
            p[at + static_cast<size_t>(i)] =
                static_cast<uint8_t>(v >> (8 * i));
    };
    p[0] = 'B';
    p[1] = 'M';
    put32(2, total);
    put32(10, off);
    put32(14, 40);
    put32(18, w);
    put32(22, h);
    put16(26, 1);
    put16(28, 24);
    put32(34, pixel_bytes);
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* dst = p + off + static_cast<size_t>(row) * (h - 1 - y);
        const uint8_t* src = rgb + static_cast<size_t>(w) * 3 * y;
        for (uint32_t x = 0; x < w; ++x) {
            dst[x * 3 + 0] = src[x * 3 + 2];
            dst[x * 3 + 1] = src[x * 3 + 1];
            dst[x * 3 + 2] = src[x * 3 + 0];
        }
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") || !f) return false;
    const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

// BT.709 limited-range I420 -> RGB (matches the engine's fetch shader
// closely enough for eyeballing).
void i420_to_rgb(const looks::codec::FrameView& v, std::vector<uint8_t>& rgb) {
    rgb.resize(static_cast<size_t>(v.width) * v.height * 3);
    for (uint32_t y = 0; y < v.height; ++y) {
        for (uint32_t x = 0; x < v.width; ++x) {
            const float Y = v.y.data[y * v.y.stride + x];
            const float U =
                v.u.data[(y / 2) * v.u.stride + x / 2] - 128.0f;
            const float V =
                v.v.data[(y / 2) * v.v.stride + x / 2] - 128.0f;
            const float c = (Y - 16.0f) * 1.164f;
            const float r = c + 1.793f * V;
            const float g = c - 0.213f * U - 0.533f * V;
            const float b = c + 2.112f * U;
            uint8_t* px = &rgb[(static_cast<size_t>(y) * v.width + x) * 3];
            px[0] = static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f));
            px[1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 255.0f));
            px[2] = static_cast<uint8_t>(std::clamp(b, 0.0f, 255.0f));
        }
    }
}

}  // namespace

// Same dev-harness pattern for long-form footage: when
// temp/long example.mp4 exists, import it once with the app's default
// options (bundle cached under temp/long_example/) so timeline and player
// behavior on long media can be exercised through the smoke workflow.
TEST(long_example_import) {
    namespace fs = std::filesystem;
    const fs::path root(LOOKS_REPO_ROOT);
    const fs::path src = root / "temp" / "long example.mp4";
    if (!fs::exists(src)) return;   // harness inactive on this machine

    const fs::path bundle_dir = root / "temp" / "long_example";
    std::error_code ec;
    fs::create_directories(bundle_dir, ec);
    if (fs::exists(bundle_dir / "long example.mez")) return;   // cached
    const looks::media::ImportResult res =
        looks::media::import_media(src, bundle_dir, {});
    CHECK(res.ok);
}

TEST(mez_decode_throughput_bench) {
    // Dev bench, not an assertion: sequential single-reader decode cost
    // of the cached 4K bundle - the number the preview's per-stream
    // decode budget lives or dies on. Silent when the harness footage
    // is absent.
    namespace fs = std::filesystem;
    const fs::path root(LOOKS_REPO_ROOT);
    const fs::path mez = root / "temp" / "long_example" / "long example.mez";
    if (!fs::exists(mez)) return;
    looks::codec::MezReader reader;
    std::string error;
    CHECK(reader.open(mez, &error));
    if (reader.frame_count() < 2) return;
    looks::codec::DecodedFrame frame;
    reader.decode(0, frame);   // warm the file cache / first-touch
    const uint32_t n = std::min(48u, reader.frame_count());
    const auto t0 = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < n; ++i) CHECK(reader.decode(i, frame));
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    std::fprintf(stderr, "  [bench] mez %ux%u decode %.1f ms/frame\n",
                 reader.width(), reader.height(),
                 ms / static_cast<double>(n));
}

TEST(ref_frames_dump) {
    namespace fs = std::filesystem;
    const fs::path root(LOOKS_REPO_ROOT);
    const fs::path src = root / "temp" / "example.mp4";
    if (!fs::exists(src)) return;   // harness inactive on this machine

    const fs::path bundle_dir = root / "temp" / "example_ref";
    std::error_code ec;
    fs::create_directories(bundle_dir, ec);
    fs::path mez = bundle_dir / "example.mez";
    if (!fs::exists(mez)) {
        looks::media::ImportOptions opt;
        opt.proxy = false;
        opt.thumb_count = 0;
        const looks::media::ImportResult res =
            looks::media::import_media(src, bundle_dir, opt);
        CHECK(res.ok);
        if (!res.ok) return;
        mez = res.mez_path;
    }

    looks::codec::MezReader reader;
    std::string error;
    CHECK(reader.open(mez, &error));
    if (!reader.frame_count()) return;
    const double fps = reader.fps() > 0.0 ? reader.fps() : 30.0;
    const uint32_t second =
        std::min(reader.frame_count(),
                 static_cast<uint32_t>(fps + 0.5));

    // First six consecutive frames (per-frame motion), then a spread
    // across the rest of the first second.
    std::vector<uint32_t> picks;
    for (uint32_t i = 0; i < 6 && i < second; ++i) picks.push_back(i);
    for (uint32_t i = 8; i < second; i += std::max(second / 6u, 1u))
        picks.push_back(i);

    const fs::path out_dir = root / "temp" / "ref_frames";
    fs::create_directories(out_dir, ec);
    looks::codec::DecodedFrame frame;
    std::vector<uint8_t> rgb;
    for (const uint32_t idx : picks) {
        CHECK(reader.decode(idx, frame));
        i420_to_rgb(frame.view(), rgb);
        char name[64];
        std::snprintf(name, sizeof(name), "frame_%03u.bmp", idx);
        CHECK(write_bmp(out_dir / name, rgb.data(), frame.width,
                        frame.height));
    }
}

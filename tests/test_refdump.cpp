// Dev harness, not a correctness test: when reference footage exists in
// temp/, exercise the NATIVE video path on it - ingest sidecars, frame
// dumps, decode/seek benches. Passes trivially when the files are absent,
// so CI and other machines never notice it.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "codec/mez.h"
#include "media/bmff.h"
#include "media/export.h"
#include "media/frame_index.h"
#include "media/import.h"
#include "platform/win/mf_codec.h"
#include "test_framework.h"

namespace {

namespace fs = std::filesystem;

// Minimal 24-bit BMP writer (BGR, bottom-up, 4-byte row padding).
bool write_bmp(const fs::path& path, const uint8_t* rgb, uint32_t w,
               uint32_t h) {
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

// BT.709 limited-range NV12 -> RGB (matches the engine's fetch shader
// closely enough for eyeballing).
void nv12_to_rgb(const looks::platform::VideoFrameNV12& v,
                 std::vector<uint8_t>& rgb) {
    rgb.resize(static_cast<size_t>(v.width) * v.height * 3);
    const size_t stride = v.stride ? v.stride : v.width;
    const uint8_t* uv = v.data.data() + stride * v.height;
    for (uint32_t y = 0; y < v.height; ++y) {
        for (uint32_t x = 0; x < v.width; ++x) {
            const float Y = v.data[y * stride + x];
            const float U = uv[(y / 2) * stride + (x / 2) * 2] - 128.0f;
            const float V = uv[(y / 2) * stride + (x / 2) * 2 + 1] - 128.0f;
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

// A single native decode session, the same shape as the pool's: a demux
// cursor over the frame index plus an H.264 decoder; fetch() rolls from
// the target's keyframe when it cannot continue.
struct Roller {
    looks::media::FrameIndex idx;
    looks::platform::H264Decoder dec;
    FILE* f = nullptr;
    uint32_t next_decode = 0;
    int64_t next_present = -1;
    bool draining = false;

    ~Roller() {
        if (f) std::fclose(f);
    }

    bool open(const fs::path& path, std::string* error) {
        if (!looks::media::load_frame_index(path, &idx, error)) return false;
        // Exactly the pool's session configuration - the benches must
        // measure what playback pays (DXVA when the machine has it,
        // software fallback otherwise).
        if (!dec.create(idx.avcc, idx.width, idx.height, error,
                        /*allow_d3d=*/true, /*low_latency=*/true))
            return false;
        f = _wfopen(path.c_str(), L"rb");
        if (!f && error) *error = "cannot open source";
        return f != nullptr;
    }

    // Decodes presentation frame `target`; `sink(p, nv12)` sees every
    // frame emitted on the way.
    template <typename Sink>
    bool fetch(uint32_t target, Sink&& sink) {
        const uint32_t target_decode = idx.present_to_decode[target];
        const uint32_t key = idx.keyframe_before(target);
        const bool cont = next_present >= 0 && !draining &&
                          next_present <= static_cast<int64_t>(target) &&
                          key < next_decode;
        const uint32_t floor_p =
            cont ? static_cast<uint32_t>(next_present)
                 : idx.decode_to_present[key];
        if (!cont) {
            dec.flush();
            draining = false;
            next_decode = key;
            next_present = idx.decode_to_present[key];
        }
        std::vector<uint8_t> bytes;
        looks::platform::VideoFrameNV12 nv12;
        bool got = false;
        // Arrival-order labeling with a sane-pts override, exactly like
        // the pool: the first post-flush stamp can be garbage.
        int64_t expect = floor_p;
        const int64_t half_dur = idx.timescale
            ? static_cast<int64_t>(5.0e6 * idx.frame_duration /
                                   idx.timescale)
            : 0;
        while (!got) {
            if (next_decode < idx.frame_count() &&
                next_decode <= target_decode + 64) {
                const looks::media::FrameIndex::Sample& sm =
                    idx.samples[next_decode];
                if (_fseeki64(f, static_cast<int64_t>(sm.offset),
                              SEEK_SET) != 0)
                    return false;
                bytes.resize(sm.size);
                if (std::fread(bytes.data(), 1, sm.size, f) != sm.size)
                    return false;
                if (!dec.feed(bytes.data(), bytes.size(), sm.pts_100ns,
                              sm.duration_100ns, sm.keyframe))
                    return false;
                ++next_decode;
            } else if (!draining) {
                dec.drain();
                draining = true;
            } else {
                break;
            }
            while (dec.receive(nv12)) {
                uint32_t p = static_cast<uint32_t>(std::min<int64_t>(
                    expect, idx.frame_count() - 1));
                const int64_t pts = nv12.pts_100ns;
                if (std::llabs(pts - idx.present_pts[p]) > half_dur &&
                    pts + half_dur >= idx.present_pts[floor_p] &&
                    pts <= idx.present_pts.back() + half_dur)
                    p = idx.present_of_pts(pts);
                expect = static_cast<int64_t>(p) + 1;
                next_present = expect;
                if (p < floor_p) continue;
                sink(p, nv12);
                if (p == target) got = true;
            }
        }
        if (draining) next_present = -1;
        return got;
    }
};

}  // namespace

// Long-form footage: run the new ingest once (sidecars cached under
// temp/long_example_ingest/) so timeline and player behavior on long
// media can be exercised through the smoke workflow. The video pass is
// one full decode - a one-time cost per machine, gated by the analysis
// sidecar it produces.
// CONSOLIDATE: a long-GOP fixture transcodes to all-intra with the
// frame count preserved, and the artifact demuxes with EVERY sample a
// keyframe — the property that turns a cold scrub into a one-frame
// decode. The fixture is synthesized in-test through the export path.
TEST(consolidate_all_intra) {
    const fs::path root(LOOKS_REPO_ROOT);
    const fs::path dir = root / "temp" / "consolidate_test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path src = dir / "gop30.mp4";

    looks::media::ExportOptions opt;
    opt.video_bitrate_bps = 1'000'000;
    opt.gop_frames = 30;
    const auto producer = [](uint32_t f, std::vector<uint8_t>& nv12) {
        const uint32_t w = 128, h = 96;
        nv12.assign(static_cast<size_t>(w) * h * 3 / 2, 128);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                nv12[static_cast<size_t>(y) * w + x] =
                    static_cast<uint8_t>(x * 2 + f * 4);
        return true;
    };
    const looks::media::ExportResult made = looks::media::export_movie(
        128, 96, 30, 1, 60, producer, {}, src, opt);
    CHECK(made.ok);

    {
        looks::media::BmffFile f;
        std::string err;
        CHECK(f.open(src, &err));
        const looks::media::TrackInfo* v = f.movie().first_video();
        CHECK(v != nullptr);
        CHECK_EQ(v->samples.size(), size_t{60});
        uint32_t keys = 0;
        for (const looks::media::SampleInfo& s : v->samples)
            keys += s.keyframe ? 1u : 0u;
        CHECK(keys < 10);   // really long-GOP before the transcode
    }

    const looks::media::ImportResult res =
        looks::media::consolidate_video(src, dir);
    CHECK(res.ok);
    CHECK_EQ(res.frame_count, 60u);

    const fs::path intra = dir / "gop30.intra.mp4";
    {
        looks::media::BmffFile f;
        std::string err;
        CHECK(f.open(intra, &err));
        const looks::media::TrackInfo* v = f.movie().first_video();
        CHECK(v != nullptr);
        CHECK_EQ(v->samples.size(), size_t{60});
        for (const looks::media::SampleInfo& s : v->samples)
            CHECK(s.keyframe);
    }
    fs::remove(src, ec);     // test-owned temp fixtures
    fs::remove(intra, ec);
}

TEST(long_example_ingest) {
    const fs::path root(LOOKS_REPO_ROOT);
    const fs::path src = root / "temp" / "long example.mp4";
    if (!fs::exists(src)) return;   // harness inactive on this machine

    const fs::path dest = root / "temp" / "long_example_ingest";
    std::error_code ec;
    fs::create_directories(dest, ec);
    if (fs::exists(dest / "long example.analysis")) return;   // cached
    const looks::media::ImportResult res =
        looks::media::import_media(src, dest, {});
    CHECK(res.ok);
    CHECK(res.mez_path.empty());              // no video transcode
    CHECK(!res.analysis_path.empty());
    CHECK(res.frame_count > 0);
}

TEST(mez_decode_throughput_bench) {
    // Historical baseline, kept while the old fixture bundle exists on a
    // machine: the sequential mez decode cost native playback replaced.
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

TEST(native_decode_throughput_bench) {
    // Sequential native decode rate, one session - the number a stream's
    // playback budget lives on now.
    const fs::path root(LOOKS_REPO_ROOT);
    const fs::path src = root / "temp" / "long example.mp4";
    if (!fs::exists(src)) return;
    looks::platform::MfSession session;
    if (!session.ok()) return;
    Roller roller;
    std::string error;
    CHECK(roller.open(src, &error));
    const uint32_t n = std::min(240u, roller.idx.frame_count());
    if (n < 2) return;
    // The pool RINGS burst emissions, so a sequential consumer only
    // fetches past the last emitted frame - mirror that, or every fetch
    // reseeks the run the burst already decoded.
    int64_t emitted_to = -1;
    roller.fetch(0, [&](uint32_t p, const looks::platform::VideoFrameNV12&) {
        emitted_to = std::max<int64_t>(emitted_to, p);
    });
    const auto t0 = std::chrono::steady_clock::now();
    for (uint32_t i = 1; i < n; ++i) {
        if (static_cast<int64_t>(i) <= emitted_to) continue;
        CHECK(roller.fetch(
            i, [&](uint32_t p, const looks::platform::VideoFrameNV12&) {
                emitted_to = std::max<int64_t>(emitted_to, p);
            }));
    }
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    std::fprintf(stderr,
                 "  [bench] native %ux%u sequential %.1f ms/frame "
                 "(%.1f fps over %u frames)\n",
                 roller.idx.width, roller.idx.height,
                 ms / static_cast<double>(n - 1),
                 1000.0 * (n - 1) / ms, n - 1);
}

TEST(native_seek_latency_bench) {
    // Cold-seek cost: flush + roll from the target's keyframe. Spread
    // deterministic targets across the clip; most force a reseek.
    const fs::path root(LOOKS_REPO_ROOT);
    const fs::path src = root / "temp" / "long example.mp4";
    if (!fs::exists(src)) return;
    looks::platform::MfSession session;
    if (!session.ok()) return;
    Roller roller;
    std::string error;
    CHECK(roller.open(src, &error));
    const uint32_t count = roller.idx.frame_count();
    if (count < 100) return;
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    std::vector<double> ms;
    for (int i = 0; i < 20; ++i) {
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        const uint32_t target =
            static_cast<uint32_t>((rng >> 33) % count);
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = roller.fetch(
            target, [&](uint32_t, const looks::platform::VideoFrameNV12&) {});
        CHECK(ok);
        ms.push_back(std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - t0)
                         .count());
    }
    std::sort(ms.begin(), ms.end());
    std::fprintf(stderr,
                 "  [bench] native seek median %.0f ms, p90 %.0f ms, "
                 "worst %.0f ms (%zu seeks)\n",
                 ms[ms.size() / 2], ms[ms.size() * 9 / 10], ms.back(),
                 ms.size());
}

TEST(ref_frames_dump) {
    const fs::path root(LOOKS_REPO_ROOT);
    const fs::path src = root / "temp" / "example.mp4";
    if (!fs::exists(src)) return;   // harness inactive on this machine

    looks::platform::MfSession session;
    if (!session.ok()) return;
    Roller roller;
    std::string error;
    CHECK(roller.open(src, &error));
    if (!roller.idx.frame_count()) return;
    const double fps = roller.idx.fps() > 0.0 ? roller.idx.fps() : 30.0;
    const uint32_t second = std::min(
        roller.idx.frame_count(), static_cast<uint32_t>(fps + 0.5));

    // First six consecutive frames (per-frame motion), then a spread
    // across the rest of the first second.
    std::vector<uint32_t> picks;
    for (uint32_t i = 0; i < 6 && i < second; ++i) picks.push_back(i);
    for (uint32_t i = 8; i < second; i += std::max(second / 6u, 1u))
        picks.push_back(i);

    const fs::path out_dir = root / "temp" / "ref_frames";
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    std::vector<uint8_t> rgb;
    for (const uint32_t idx : picks) {
        bool wrote = false;
        CHECK(roller.fetch(
            idx, [&](uint32_t p, const looks::platform::VideoFrameNV12& v) {
                if (p != idx) return;
                nv12_to_rgb(v, rgb);
                char name[64];
                std::snprintf(name, sizeof(name), "frame_%03u.bmp", p);
                wrote = write_bmp(out_dir / name, rgb.data(), v.width,
                                  v.height);
            }));
        CHECK(wrote);
    }
}

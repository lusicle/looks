// Determinism harness: same project + seeds => identical frames.
// Renders a deterministic synthetic clip through the full GPU path (upload
// -> YCbCr->linear -> effect chain -> NV12 readback) TWICE with independent
// Engine/readback instances and compares per-frame FNV-1a hashes. With
// --export it additionally drives the offline export pipeline (encoder MFT
// + muxer) and re-demuxes the result with our own parser as a structural
// check. Headless: no window, no swapchain.
//
//   looks_rendertest [frames] [--export out.mp4] [--pcm sidecar.pcm]
//
// Exit codes: 0 ok, 1 mismatch/failure, 77 skipped (no Vulkan device —
// CTest SKIP_RETURN_CODE).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "doc/document.h"
#include "doc/effects.h"
#include "gfx/engine.h"
#include "gfx/readback.h"
#include "gfx/vk_device.h"
#include "media/audio_mix.h"
#include "media/bmff.h"
#include "media/export.h"
#include "util/file.h"
#include "util/hash.h"

namespace {

using namespace looks;

constexpr uint32_t kWidth = 320;
constexpr uint32_t kHeight = 240;

// The harness stands in for the decode pool: one clip source, fed under
// the instance key compile_graph stamps on its Source node (docs/look.md).
gfx::Engine::LayerSourceFrame clip_frame(const doc::Document& doc,
                                         const gfx::SourcePlanes& planes) {
    gfx::Engine::LayerSourceFrame lf;
    // Rendering the look directly: path = look id, and the clip node's
    // asset folds into the Source key.
    lf.key = hash_combine(
        hash_combine(doc.looks[0].id, doc.looks[0].layers[0].id),
        doc.looks[0].layers[0].asset);
    lf.planes = planes;
    return lf;
}

// Deterministic animated I420 source (gradient + moving bar + chroma sweep).
struct SyntheticSource {
    std::vector<uint8_t> y, u, v;

    SyntheticSource() {
        y.resize(kWidth * kHeight);
        u.resize((kWidth / 2) * (kHeight / 2));
        v.resize((kWidth / 2) * (kHeight / 2));
    }

    gfx::SourcePlanes planes(uint32_t frame) {
        for (uint32_t r = 0; r < kHeight; ++r) {
            for (uint32_t c = 0; c < kWidth; ++c) {
                uint32_t value = (c + r + frame * 3) & 0xFF;
                const uint32_t bar = (frame * 5) % kWidth;
                if (c >= bar && c < bar + 24) value = 235;
                y[r * kWidth + c] = static_cast<uint8_t>(16 + value * 219 / 255);
            }
        }
        for (uint32_t r = 0; r < kHeight / 2; ++r) {
            for (uint32_t c = 0; c < kWidth / 2; ++c) {
                u[r * (kWidth / 2) + c] =
                    static_cast<uint8_t>(96 + ((c * 2 + frame) & 63));
                v[r * (kWidth / 2) + c] =
                    static_cast<uint8_t>(160 - ((r + frame) & 63));
            }
        }
        gfx::SourcePlanes p;
        p.y = y.data();
        p.y_stride = kWidth;
        p.u = u.data();
        p.u_stride = kWidth / 2;
        p.v = v.data();
        p.v_stride = kWidth / 2;
        p.width = kWidth;
        p.height = kHeight;
        return p;
    }
};

uint64_t fnv1a(const uint8_t* data, size_t size) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

// The fixed "project": a broad slice of the roster at non-default settings —
// stochastic effects (grain, jitter, quantizer boil), the multi-pass glow,
// and the flow-fed smear all must hash bit-exact across evaluations.
doc::Document make_document() {
    doc::Document doc;
    doc.master_seed = 1234;
    // The clip node binds a synthetic asset id (no Asset entry: unknown
    // length = always on); the harness feeds its planes under the key.
    doc.looks[0].layers[0].asset = doc.next_effect_id++;
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::FlowSmear));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Pixelate));
    doc.looks[0].layers[0].stack[1].params[0] = 9.0f;      // block size
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Glow));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Quantize));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Grain));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Jitter));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::RgbSplit));
    doc.looks[0].layers[0].stack[6].params[0] = 4.5f;      // shift x
    doc.looks[0].layers[0].stack[6].params[1] = 1.5f;      // shift y
    doc.looks[0].layers[0].stack[6].wet = 0.8f;
    doc.looks[0].layers[0].stack[6].opacity = 0.9f;
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Vignette));
    // Codec-Box: exercises the segmented GPU->CPU->GPU roundtrip with
    // persistent decoder state, flow-fed MVs, and seeded corruption.
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Datamosh));
    doc.looks[0].layers[0].stack[8].params[1] = 8.0f;      // gop 8
    doc.looks[0].layers[0].stack[8].params[3] = 4.0f;      // mv random
    doc.looks[0].layers[0].stack[8].params[4] = 0.15f;     // corrupt
    // Stateful one-frame-delay effects (persistent GPU targets).
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Echo));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Feedback));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::FilmStock));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Glyph));
    // Wave 2: seeded slice shuffle, time-quantized curl-noise warp, and a
    // pure-geometry fold.
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::SliceShuffle));
    doc.looks[0].layers[0].stack.back().params[3] = 0.7f;   // probability
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Turbulence));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Kaleido));
    doc.looks[0].layers[0].stack.back().wet = 0.6f;
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Snow));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Composite));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Timestamp));
    // Dither-engine wave: STBN mode reads the build-time LUT; slit-scan
    // exercises the past-frames ring across sequential evaluation.
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Quantize));
    doc.looks[0].layers[0].stack.back().params[0] = 5.0f;   // levels
    doc.looks[0].layers[0].stack.back().params[2] = 6.0f;   // STBN
    doc.looks[0].layers[0].stack.back().wet = 0.5f;
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::SlitScan));
    doc.looks[0].layers[0].stack.back().params[1] = 8.0f;   // depth
    // RD-stipple dither (mode 9): the quantizer's own Gray-Scott
    // state must evolve bit-exact across evaluations.
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Quantize));
    doc.looks[0].layers[0].stack.back().params[2] = 9.0f;
    doc.looks[0].layers[0].stack.back().params[3] = 0.8f;
    doc.looks[0].layers[0].stack.back().wet = 0.5f;
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Voronoi));
    doc.looks[0].layers[0].stack.back().wet = 0.6f;
    // Stateful sim: N Gray-Scott steps per frame, seeded by the picture.
    doc.looks[0].layers[0].stack.push_back(
        doc::make_effect(doc, doc::EffectType::ReactionDiffusion));
    doc.looks[0].layers[0].stack.back().params[2] = 6.0f;   // steps
    doc.looks[0].layers[0].stack.back().wet = 0.7f;
    // CPU serpentine dither with temporal carry (second CPU roundtrip).
    doc.looks[0].layers[0].stack.push_back(
        doc::make_effect(doc, doc::EffectType::ErrorDiffusion));
    doc.looks[0].layers[0].stack.back().params[0] = 3.0f;   // levels
    doc.looks[0].layers[0].stack.back().params[3] = 0.4f;   // carry
    doc.looks[0].layers[0].stack.back().wet = 0.6f;
    // Compositing: a noise generator layer multiplied over the base, with
    // its own mini stack.
    doc::Layer overlay;
    overlay.id = doc.next_effect_id++;
    overlay.name = "noise";
    overlay.source = doc::LayerSourceKind::Noise;
    overlay.blend = doc::BlendMode::Multiply;
    overlay.opacity = 0.6f;
    overlay.gen_scale = 24.0f;
    overlay.stack.push_back(doc::make_effect(doc, doc::EffectType::Pixelate));
    doc.looks[0].layers.push_back(std::move(overlay));
    // Port-1 matte on the pixelate (masks ARE images): a Shape layer wired
    // into the matte port gates the effect through extract + apply. The
    // layer feeds ONLY the gate — no link to the composite.
    doc::Layer matte;
    matte.id = doc.next_effect_id++;
    matte.name = "matte";
    matte.source = doc::LayerSourceKind::Shape;
    matte.gen_scale = 8.0f;
    matte.gen_angle = 0.35f;
    doc.looks[0].layers.push_back(std::move(matte));
    for (const doc::Layer& l : doc.looks[0].layers) {
        if (l.source == doc::LayerSourceKind::Shape) continue;
        uint64_t prev = l.id;
        for (const doc::EffectInstance& fx : l.stack) {
            doc.looks[0].links.push_back({prev, fx.id, 0});
            prev = fx.id;
        }
        doc.looks[0].links.push_back({prev, 0, 0});
    }
    doc.looks[0].links.push_back(
        {doc.looks[0].layers.back().id, doc.looks[0].layers[0].stack[1].id, 1});
    return doc;
}

// History-free slice of the roster for the render-cache coherence check
//: every effect here is a pure function of (document, frame).
doc::Document make_cacheable_document() {
    doc::Document doc;
    doc.master_seed = 555;
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::RgbSplit));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Pixelate));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Glow));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Grain));
    // Level cycling (mode 8) is time-based but pure — must stay cacheable.
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Quantize));
    doc.looks[0].layers[0].stack.back().params[2] = 8.0f;
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Kaleido));
    doc.looks[0].layers[0].stack.back().wet = 0.5f;
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Vignette));
    // Port-1 matte on the pixelate — the wire path must stay cacheable.
    doc::Layer matte;
    matte.id = doc.next_effect_id++;
    matte.name = "matte";
    matte.source = doc::LayerSourceKind::Shape;
    matte.gen_scale = 8.0f;
    matte.gen_angle = 0.35f;
    doc.looks[0].layers.push_back(std::move(matte));
    uint64_t prev = doc.looks[0].layers[0].id;
    for (const doc::EffectInstance& fx : doc.looks[0].layers[0].stack) {
        doc.looks[0].links.push_back({prev, fx.id, 0});
        prev = fx.id;
    }
    doc.looks[0].links.push_back({prev, 0, 0});
    doc.looks[0].links.push_back(
        {doc.looks[0].layers.back().id, doc.looks[0].layers[0].stack[1].id, 1});
    return doc;
}

// Renders the frame range twice through ONE engine with the cache armed:
// pass 1 misses populate it, pass 2 must be served from it bit-exact.
bool cache_coherence_check(gfx::Device& device,
                           const std::filesystem::path& shader_dir,
                           uint32_t frames) {
    const doc::Document doc = make_cacheable_document();
    if (doc::document_uses_history(doc)) {
        std::fprintf(stderr, "cache: test document unexpectedly stateful\n");
        return false;
    }
    auto engine = gfx::Engine::create(device, shader_dir);
    auto readback = gfx::Nv12Readback::create(device, shader_dir);
    if (!engine || !readback) {
        std::fprintf(stderr, "cache: engine/readback init failed\n");
        return false;
    }
    const uint64_t ctx = 0xC0FFEEull | 1u;
    SyntheticSource source;
    std::vector<uint8_t> nv12;
    std::vector<uint64_t> miss_hashes, hit_hashes;
    for (uint32_t f = 0; f < frames; ++f) {
        const auto lf = clip_frame(doc, source.planes(f));
        if (!readback->render(*engine, doc, doc.looks[0].id, f, 30.0, kWidth,
                              kHeight, nv12, ctx, f, &lf, 1))
            return false;
        miss_hashes.push_back(fnv1a(nv12.data(), nv12.size()));
    }
    for (uint32_t f = 0; f < frames; ++f) {
        const auto lf = clip_frame(doc, source.planes(f));
        if (!readback->render(*engine, doc, doc.looks[0].id, f, 30.0, kWidth,
                              kHeight, nv12, ctx, f, &lf, 1))
            return false;
        hit_hashes.push_back(fnv1a(nv12.data(), nv12.size()));
    }
    bool ok = true;
    for (uint32_t f = 0; f < frames; ++f)
        if (miss_hashes[f] != hit_hashes[f]) {
            std::fprintf(stderr, "cache: frame %u hit != miss\n", f);
            ok = false;
        }
    // The readback path fences every call, so each pending readback is
    // harvested on the next render — by pass 2 every frame is resident and
    // every render must hit.
    if (engine->cache().hits() < frames) {
        std::fprintf(stderr, "cache: expected %u hits, saw %llu\n", frames,
                     static_cast<unsigned long long>(engine->cache().hits()));
        ok = false;
    }
    if (engine->cache().count() == 0 ||
        engine->cache().bytes() > engine->cache().budget()) {
        std::fprintf(stderr, "cache: bad fill state\n");
        ok = false;
    }
    if (ok)
        std::printf(
            "render cache: %u frames bit-exact from cache (%zu entries, "
            "%zu KiB)\n",
            frames, engine->cache().count(), engine->cache().bytes() >> 10);
    return ok;
}

bool render_pass(gfx::Device& device, const std::filesystem::path& shader_dir,
                 const doc::Document& doc, uint32_t frames,
                 std::vector<uint64_t>& hashes) {
    auto engine = gfx::Engine::create(device, shader_dir);
    auto readback = gfx::Nv12Readback::create(device, shader_dir);
    if (!engine || !readback) {
        std::fprintf(stderr, "engine/readback init failed\n");
        return false;
    }
    SyntheticSource source;
    std::vector<uint8_t> nv12;
    hashes.clear();
    for (uint32_t f = 0; f < frames; ++f) {
        const auto lf = clip_frame(doc, source.planes(f));
        if (!readback->render(*engine, doc, doc.looks[0].id, f, 30.0, kWidth,
                              kHeight, nv12, 0, f, &lf, 1)) {
            std::fprintf(stderr, "render failed at frame %u\n", f);
            return false;
        }
        hashes.push_back(fnv1a(nv12.data(), nv12.size()));
    }
    return true;
}

// ---- per-effect benchmark (--bench). Renders every effect type in
// isolation at 1080p through the fenced readback path (wall time = upload +
// effect + readback, including CPU roundtrips) and writes a sorted
// ms/frame table to <repo>/temp/bench.txt. Gated on temp/bench_request.txt
// containing '1' so routine ctest runs skip it (exit 77).

struct BenchSource {
    uint32_t w, h;
    std::vector<uint8_t> y, u, v;

    explicit BenchSource(uint32_t width, uint32_t height)
        : w(width), h(height) {
        y.resize(static_cast<size_t>(w) * h);
        u.resize(static_cast<size_t>(w / 2) * (h / 2));
        v.resize(static_cast<size_t>(w / 2) * (h / 2));
    }

    gfx::SourcePlanes planes(uint32_t frame) {
        for (uint32_t r = 0; r < h; ++r)
            for (uint32_t c = 0; c < w; ++c)
                y[static_cast<size_t>(r) * w + c] =
                    static_cast<uint8_t>(16 + ((c + r + frame * 3) & 0xDB));
        for (uint32_t r = 0; r < h / 2; ++r)
            for (uint32_t c = 0; c < w / 2; ++c) {
                u[static_cast<size_t>(r) * (w / 2) + c] =
                    static_cast<uint8_t>(96 + ((c * 2 + frame) & 63));
                v[static_cast<size_t>(r) * (w / 2) + c] =
                    static_cast<uint8_t>(160 - ((r + frame) & 63));
            }
        gfx::SourcePlanes p;
        p.y = y.data();
        p.y_stride = w;
        p.u = u.data();
        p.u_stride = w / 2;
        p.v = v.data();
        p.v_stride = w / 2;
        p.width = w;
        p.height = h;
        return p;
    }
};

int run_bench(gfx::Device& device, const std::filesystem::path& shader_dir) {
    const std::filesystem::path temp_dir =
        std::filesystem::path(LOOKS_REPO_ROOT) / "temp";
    const auto request = read_file_bytes(temp_dir / "bench_request.txt");
    if (!request || request->empty() || (*request)[0] != '1') {
        std::printf("bench: not requested (temp/bench_request.txt != 1)\n");
        return 77;
    }

    constexpr uint32_t kFrames = 16;
    constexpr uint32_t kWarm = 4;
    BenchSource source(1920, 1080);

    struct Row {
        double ms;
        std::string name;
    };
    std::vector<Row> rows;

    // GPU clock warmup: without sustained load first, the sweep's early
    // rows measure idle clocks and the table sorts by enum order instead
    // of kernel cost.
    {
        auto engine = gfx::Engine::create(device, shader_dir);
        auto readback = gfx::Nv12Readback::create(device, shader_dir);
        if (!engine || !readback) {
            std::fprintf(stderr, "bench: engine init failed\n");
            return 1;
        }
        doc::Document doc;
        doc.master_seed = 77;
        doc.canvas_w = source.w;
        doc.canvas_h = source.h;
        std::vector<uint8_t> nv12;
        for (uint32_t f = 0; f < 60; ++f) {
            const auto lf = clip_frame(doc, source.planes(f));
            if (!readback->render(*engine, doc, doc.looks[0].id, f, 30.0,
                                  source.w, source.h, nv12, 0, f, &lf, 1)) {
                std::fprintf(stderr, "bench: warmup render failed\n");
                return 1;
            }
        }
    }

    // Baseline first: upload + convert + readback with an empty stack.
    // One extra virtual row measures Error Diffusion's exact mode (the
    // default is fast; the speculative exact path deserves its own line).
    const int kTypes = static_cast<int>(doc::EffectType::Count);
    for (int t = -1; t <= kTypes; ++t) {
        doc::Document doc;
        doc.master_seed = 77;
        doc.canvas_w = source.w;
        doc.canvas_h = source.h;
        const char* name = "(baseline: no effects)";
        if (t >= 0) {
            const auto type = static_cast<doc::EffectType>(
                t == kTypes ? static_cast<int>(doc::EffectType::ErrorDiffusion)
                            : t);
            doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, type));
            name = t == kTypes ? "Error Diffusion (exact)"
                               : doc::effect_info(type).label;
            if (t == kTypes)
                doc.looks[0].layers[0].stack[0].params[4] = 0.0f;   // speed = exact
        }
        auto engine = gfx::Engine::create(device, shader_dir);
        auto readback = gfx::Nv12Readback::create(device, shader_dir);
        if (!engine || !readback) {
            std::fprintf(stderr, "bench: engine init failed\n");
            return 1;
        }
        std::vector<uint8_t> nv12;
        // Min of the timed frames, not the mean: strips clock-ramp and
        // scheduler spikes, leaving the effect's steady per-frame cost.
        double best = 1.0e9;
        for (uint32_t f = 0; f < kFrames; ++f) {
            // Pattern generation stays OUTSIDE the timed window: it is
            // several ms of single-threaded CPU work that would otherwise
            // flatten every GPU effect onto one harness floor.
            const auto lf = clip_frame(doc, source.planes(f));
            const auto t0 = std::chrono::steady_clock::now();
            if (!readback->render(*engine, doc, doc.looks[0].id, f, 30.0,
                                  source.w, source.h, nv12, 0, f, &lf, 1)) {
                std::fprintf(stderr, "bench: render failed (%s)\n", name);
                return 1;
            }
            if (f >= kWarm)
                best = std::min(
                    best, std::chrono::duration<double>(
                              std::chrono::steady_clock::now() - t0)
                              .count());
        }
        rows.push_back({best * 1000.0, std::string(name)});
        std::printf("bench %-24s %8.3f ms\n", name, rows.back().ms);
    }

    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return a.ms > b.ms; });
    FILE* out = _wfopen((temp_dir / "bench.txt").wstring().c_str(), L"w");
    if (!out) return 1;
    std::fprintf(out, "per-effect ms/frame (min of %u timed), 1920x1080 "
                      "(sorted, includes upload+convert+readback)\n",
                 kFrames - kWarm);
    for (const Row& r : rows)
        std::fprintf(out, "%8.3f ms  %s\n", r.ms, r.name.c_str());
    std::fclose(out);
    std::printf("bench: wrote temp/bench.txt (%zu rows)\n", rows.size());
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    uint32_t frames = 12;
    bool bench = false;
    std::filesystem::path export_path;
    std::filesystem::path pcm_path;
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--export") == 0 && i + 1 < argc) {
            export_path = argv[++i];
        } else if (std::wcscmp(argv[i], L"--pcm") == 0 && i + 1 < argc) {
            pcm_path = argv[++i];
        } else if (std::wcscmp(argv[i], L"--bench") == 0) {
            bench = true;
        } else {
            const int n = _wtoi(argv[i]);
            if (n > 0) frames = static_cast<uint32_t>(n);
        }
    }

    gfx::DeviceDesc desc;   // headless: no hwnd
    auto device = gfx::Device::create(desc);
    if (!device) {
        std::fprintf(stderr, "SKIP: no Vulkan device\n");
        return 77;
    }
    const std::filesystem::path shader_dir = executable_dir() / "shaders";
    if (bench) return run_bench(*device, shader_dir);
    const doc::Document doc = make_document();

    std::vector<uint64_t> pass_a, pass_b;
    if (!render_pass(*device, shader_dir, doc, frames, pass_a)) return 1;
    if (!render_pass(*device, shader_dir, doc, frames, pass_b)) return 1;

    bool ok = true;
    bool any_motion = false;
    for (uint32_t f = 0; f < frames; ++f) {
        const bool match = pass_a[f] == pass_b[f];
        std::printf("frame %2u  %016llx  %s\n", f,
                    static_cast<unsigned long long>(pass_a[f]),
                    match ? "ok" : "MISMATCH");
        if (!match) ok = false;
        if (f > 0 && pass_a[f] != pass_a[f - 1]) any_motion = true;
    }
    if (!any_motion) {
        std::fprintf(stderr, "suspicious: all frames hashed identical\n");
        ok = false;
    }
    if (!ok) {
        std::fprintf(stderr, "DETERMINISM FAILURE\n");
        return 1;
    }
    std::printf("determinism: %u frames bit-exact across two evaluations\n",
                frames);

    if (!cache_coherence_check(*device, shader_dir, frames)) {
        std::fprintf(stderr, "RENDER CACHE FAILURE\n");
        return 1;
    }

    if (!export_path.empty()) {
        auto engine = gfx::Engine::create(*device, shader_dir);
        auto readback = gfx::Nv12Readback::create(*device, shader_dir);
        if (!engine || !readback) return 1;
        SyntheticSource source;
        auto producer = [&](uint32_t f, std::vector<uint8_t>& nv12) {
            const auto lf = clip_frame(doc, source.planes(f));
            return readback->render(*engine, doc, doc.looks[0].id, f, 30.0,
                                    kWidth, kHeight, nv12, 0, f, &lf, 1);
        };
        // The soundtrack goes through the same tree mix the app uses: one
        // source, full span, unity gain.
        media::MixState mix;
        std::vector<float> scratch;
        media::ExportAudio audio;
        if (!pcm_path.empty()) {
            if (auto pcm = media::load_pcm(pcm_path)) {
                mix.fps = 30.0;
                mix.rate = pcm->rate;
                mix.channels = pcm->channels;
                media::MixSource src;
                src.pcm = pcm;
                src.t_out = frames;
                mix.sources.push_back(std::move(src));
                audio.channels = mix.channels;
                audio.rate = mix.rate;
                audio.fill = [&](int64_t first, int16_t* out, uint32_t n) {
                    media::render_mix(mix, first, out, n, scratch);
                };
            }
        }
        media::ExportResult result = media::export_movie(
            kWidth, kHeight, 30, 1, frames, producer, audio, export_path);
        if (!result.ok) {
            std::fprintf(stderr, "export failed: %s\n", result.error.c_str());
            return 1;
        }
        // Structural validation: our own demuxer must accept the output.
        media::BmffFile file;
        std::string error;
        if (!file.open(export_path, &error)) {
            std::fprintf(stderr, "re-demux failed: %s\n", error.c_str());
            return 1;
        }
        const media::TrackInfo* video = file.movie().first_video();
        if (!video || video->width != kWidth || video->height != kHeight ||
            video->samples.size() != frames || video->avcc.empty()) {
            std::fprintf(stderr, "re-demux: unexpected video track\n");
            return 1;
        }
        std::printf("export: %u samples, %ux%u, avcC %zu bytes -> ok\n",
                    static_cast<uint32_t>(video->samples.size()), video->width,
                    video->height, video->avcc.size());
    }
    return 0;
}

// Exit code 77 tells CTest to record a skip.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <crtdbg.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "doc/document.h"
#include "doc/effects.h"
#include "doc/layer_commands.h"
#include "doc/look_commands.h"
#include "app/dev_synth.h"
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

// A new project holds no look, so every fixture seeds its own.
doc::Document doc_with_media_look() {
    doc::Document doc;
    doc::Look seed = doc::make_look(doc, "look 1");
    seed.layers.push_back(doc::make_layer(doc, doc::LayerSourceKind::Media));
    doc.looks.push_back(std::move(seed));
    return doc;
}

// The key must match the Source key that compile_graph stamps.
gfx::Engine::LayerSourceFrame media_frame(const doc::Document& doc,
                                         const gfx::SourcePlanes& planes) {
    gfx::Engine::LayerSourceFrame lf;
    lf.key = hash_combine(
        hash_combine(doc.looks[0].id, doc.looks[0].layers[0].id),
        doc.looks[0].layers[0].asset);
    lf.planes = planes;
    return lf;
}

struct SyntheticSource {
    uint32_t w, h;
    std::vector<uint8_t> y, u, v;

    explicit SyntheticSource(uint32_t width, uint32_t height)
        : w(width), h(height) {
        y.resize(static_cast<size_t>(w) * h);
        u.resize(static_cast<size_t>(w / 2) * (h / 2));
        v.resize(static_cast<size_t>(w / 2) * (h / 2));
    }

    gfx::SourcePlanes planes(uint32_t frame) {
        for (uint32_t r = 0; r < h; ++r)
            for (uint32_t c = 0; c < w; ++c)
                y[static_cast<size_t>(r) * w + c] =
                    devsynth::luma(c, r, frame, w);
        for (uint32_t r = 0; r < h / 2; ++r)
            for (uint32_t c = 0; c < w / 2; ++c) {
                u[static_cast<size_t>(r) * (w / 2) + c] =
                    devsynth::cb(c, frame);
                v[static_cast<size_t>(r) * (w / 2) + c] =
                    devsynth::cr(r, frame);
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

doc::Document make_document() {
    doc::Document doc = doc_with_media_look();
    doc.master_seed = 1234;
    // The asset id has no Asset entry on purpose: unknown length plays always.
    doc.looks[0].layers[0].asset = doc.next_effect_id++;
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::FlowSmear));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Pixelate));
    doc.looks[0].layers[0].stack[1].params[0] = 9.0f;
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Glow));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Quantize));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Grain));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Jitter));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::RgbSplit));
    doc.looks[0].layers[0].stack[6].params[0] = 4.5f;
    doc.looks[0].layers[0].stack[6].params[1] = 1.5f;
    doc.looks[0].layers[0].stack[6].wet = 0.8f;
    doc.looks[0].layers[0].stack[6].opacity = 0.9f;
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Vignette));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Datamosh));
    doc.looks[0].layers[0].stack[8].params[1] = 8.0f;
    doc.looks[0].layers[0].stack[8].params[3] = 4.0f;
    doc.looks[0].layers[0].stack[8].params[4] = 0.15f;
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Echo));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Feedback));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::FilmStock));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Glyph));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::SliceShuffle));
    doc.looks[0].layers[0].stack.back().params[3] = 0.7f;
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Turbulence));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Kaleido));
    doc.looks[0].layers[0].stack.back().wet = 0.6f;
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Snow));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Composite));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Timestamp));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Quantize));
    doc.looks[0].layers[0].stack.back().params[0] = 5.0f;
    doc.looks[0].layers[0].stack.back().params[2] = 6.0f;
    doc.looks[0].layers[0].stack.back().wet = 0.5f;
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::SlitScan));
    doc.looks[0].layers[0].stack.back().params[1] = 8.0f;
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Quantize));
    doc.looks[0].layers[0].stack.back().params[2] = 9.0f;
    doc.looks[0].layers[0].stack.back().params[3] = 0.8f;
    doc.looks[0].layers[0].stack.back().wet = 0.5f;
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Voronoi));
    doc.looks[0].layers[0].stack.back().wet = 0.6f;
    doc.looks[0].layers[0].stack.push_back(
        doc::make_effect(doc, doc::EffectType::ReactionDiffusion));
    doc.looks[0].layers[0].stack.back().params[2] = 6.0f;
    doc.looks[0].layers[0].stack.back().wet = 0.7f;
    doc.looks[0].layers[0].stack.push_back(
        doc::make_effect(doc, doc::EffectType::ErrorDiffusion));
    doc.looks[0].layers[0].stack.back().params[0] = 3.0f;
    doc.looks[0].layers[0].stack.back().params[3] = 0.4f;
    doc.looks[0].layers[0].stack.back().wet = 0.6f;
    doc::Layer overlay;
    overlay.id = doc.next_effect_id++;
    overlay.name = "noise";
    overlay.source = doc::LayerSourceKind::Noise;
    overlay.blend = doc::BlendMode::Multiply;
    overlay.opacity = 0.6f;
    overlay.gen_scale = 24.0f;
    overlay.stack.push_back(doc::make_effect(doc, doc::EffectType::Pixelate));
    doc.looks[0].layers.push_back(std::move(overlay));
    // The Shape layer feeds only the matte port. It has no composite link.
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

// Every effect here must stay a pure function of document and frame.
doc::Document make_cacheable_document() {
    doc::Document doc = doc_with_media_look();
    doc.master_seed = 555;
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::RgbSplit));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Pixelate));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Glow));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Grain));
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Quantize));
    doc.looks[0].layers[0].stack.back().params[2] = 8.0f;
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Kaleido));
    doc.looks[0].layers[0].stack.back().wet = 0.5f;
    doc.looks[0].layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Vignette));
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
    SyntheticSource source(kWidth, kHeight);
    std::vector<uint8_t> nv12;
    std::vector<uint64_t> miss_hashes, hit_hashes;
    for (uint32_t f = 0; f < frames; ++f) {
        const auto lf = media_frame(doc, source.planes(f));
        if (!readback->render(*engine, doc, doc.looks[0].id, f, 30.0, kWidth,
                              kHeight, nv12, ctx, f, &lf, 1))
            return false;
        miss_hashes.push_back(fnv1a(nv12.data(), nv12.size()));
    }
    for (uint32_t f = 0; f < frames; ++f) {
        const auto lf = media_frame(doc, source.planes(f));
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
    // The readback path fences each call, so pass 2 finds every frame resident.
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

// A matte reveals premultiplied zero, so a masked block must not cover the
// canvas in alpha. Opaque ground would measure the full frame instead.
bool matte_alpha_check(gfx::Device& device,
                       const std::filesystem::path& shader_dir) {
    doc::Document doc;
    doc::Look seed = doc::make_look(doc, "masked");
    seed.layers.push_back(doc::make_layer(doc, doc::LayerSourceKind::Solid));
    doc::Layer matte;
    matte.id = doc.next_effect_id++;
    matte.name = "matte";
    matte.source = doc::LayerSourceKind::Shape;
    matte.gen_scale = 6.0f;
    matte.gen_angle = 0.05f;
    seed.links.push_back({seed.layers[0].id, 0, 0});
    seed.links.push_back({matte.id, seed.layers[0].id, 1});
    seed.layers.push_back(std::move(matte));
    doc.looks.push_back(std::move(seed));

    doc::Placement block;
    block.id = doc.next_effect_id++;
    block.target = doc.looks[0].id;
    doc.root().tracks[0].placements.push_back(block);

    auto engine = gfx::Engine::create(device, shader_dir);
    auto readback = gfx::Nv12Readback::create(device, shader_dir);
    if (!engine || !readback) {
        std::fprintf(stderr, "matte: engine/readback init failed\n");
        return false;
    }
    std::vector<uint8_t> nv12;
    if (!readback->render(*engine, doc, doc.root_sequence, 0, 30.0, kWidth,
                          kHeight, nv12, 0, 0, nullptr, 0, block.id))
        return false;
    float rect[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (!engine->read_measure_bounds(rect)) {
        std::fprintf(stderr, "matte: no alpha bounds measured\n");
        return false;
    }
    std::printf("matte alpha bounds: x %.3f y %.3f w %.3f h %.3f\n", rect[0],
                rect[1], rect[2], rect[3]);
    if (rect[2] < 0.1f || rect[3] < 0.1f) {
        std::fprintf(stderr, "matte: the shape gated everything away\n");
        return false;
    }
    if (rect[2] > 0.9f || rect[3] > 0.9f) {
        std::fprintf(stderr,
                     "matte: the reveal is opaque, not premultiplied zero\n");
        return false;
    }
    return true;
}

bool crt_check(gfx::Device& device, const std::filesystem::path& shader_dir) {
    doc::Document doc;
    doc::Look look = doc::make_look(doc, "CRT check");
    look.layers.push_back(doc::make_layer(doc, doc::LayerSourceKind::Solid));
    look.layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::CrtSim));
    doc.looks.push_back(std::move(look));
    auto& layer = doc.looks[0].layers[0];
    auto& fx = layer.stack[0];
    layer.opacity = 1.0f;
    layer.color_a[0] = layer.color_a[1] = layer.color_a[2] = 0.35f;
    fx.params[0] = fx.params[5] = fx.params[7] = fx.params[8] = 0.0f;
    fx.params[1] = 1.0f;
    fx.params[2] = 240.0f;
    auto engine = gfx::Engine::create(device, shader_dir);
    auto readback = gfx::Nv12Readback::create(device, shader_dir);
    if (!engine || !readback) return false;
    constexpr uint32_t width = 960, height = 720;
    std::vector<uint8_t> pixels;
    auto render = [&](uint32_t frame) {
        return readback->render(*engine, doc, doc.looks[0].id, frame, 60.0,
                                width, height, pixels);
    };
    auto mean_light = [&]() {
        double sum = 0.0;
        for (uint32_t y = 60; y < height - 60; ++y)
            for (uint32_t x = 60; x < width - 60; ++x) {
                double v = (pixels[y * width + x] - 16.0) / 219.0;
                sum += v <= 0.04045 ? v / 12.92 :
                    std::pow((v + 0.055) / 1.055, 2.4);
            }
        return sum / ((width - 120) * (height - 120));
    };
    for (float beam : {0.15f, 0.3f, 0.45f}) {
        fx.params[9] = beam;
        for (float bloom : {0.0f, 1.0f}) {
            fx.params[3] = bloom;
            if (!render(0)) return false;
            const double mean = mean_light();
            std::printf("CRT beam %.2f bloom %.1f mean %.5f\n", beam, bloom, mean);
            if (std::abs(mean - 0.10048) > 0.003) return false;
        }
    }
    fx.params[0] = 1.0f;
    fx.params[1] = 0.0f;
    if (!render(0)) return false;
    uint32_t filtered_rows = 0;
    for (uint32_t y = 90; y < height - 90; ++y) {
        for (uint32_t x = 1; x < width / 4; ++x) {
            const uint8_t v = pixels[y * width + x];
            if (v <= 16) continue;
            if (v + 3 < pixels[y * width + x + 2]) ++filtered_rows;
            break;
        }
    }
    std::printf("CRT curved edge: %u rows have partial coverage\n", filtered_rows);
    if (filtered_rows < 200) return false;
    fx.params[0] = 0.0f;
    fx.params[11] = 70.0f;
    if (!render(100)) return false;
    const uint8_t lit = pixels[height / 2 * width + width / 2];
    layer.color_a[0] = layer.color_a[1] = layer.color_a[2] = 0.0f;
    if (!render(101)) return false;
    const uint8_t decay = pixels[height / 2 * width + width / 2];
    const auto repeated = pixels;
    if (!render(101) || pixels != repeated) return false;
    if (!render(102)) return false;
    const uint8_t later = pixels[height / 2 * width + width / 2];
    std::printf("CRT decay: %u -> %u -> %u; paused frame is stable\n", lit, decay, later);
    if (!(lit > decay && decay > later && later > 16)) return false;
    if (!render(120)) return false;
    const auto seek = pixels;
    if (pixels[height / 2 * width + width / 2] != 16 ||
        !render(120) || pixels != seek) return false;
    fx.params[11] = 0.0f;
    fx.params[12] = 1.0f;
    fx.params[1] = 1.0f;
    fx.params[2] = 160.0f;
    layer.color_a[0] = layer.color_a[1] = layer.color_a[2] = 0.35f;
    if (!render(0)) return false;
    const auto even_field = pixels;
    if (!render(1) || pixels == even_field) return false;
    fx.params[12] = 0.0f;
    fx.params[1] = 0.0f;
    fx.params[5] = 1.0f;
    fx.params[6] = 8.0f;
    for (uint32_t divisor : {1u, 2u, 4u}) {
        engine->set_preview_divisor(divisor);
        if (!render(0)) return false;
        const uint32_t pw = width / divisor, ph = height / divisor;
        const uint32_t lag = 24 / divisor;
        double error = 0.0;
        for (uint32_t x = 120 / divisor; x < (width - 144) / divisor; ++x)
            error += std::abs(int(pixels[ph / 2 * pw + x]) -
                              int(pixels[ph / 2 * pw + x + lag]));
        error /= (width - 264) / divisor;
        std::printf("CRT preview /%u: mask period error %.3f\n", divisor, error);
        if (error > 1.0) return false;
    }
    engine->set_preview_divisor(1);
    fx = doc::make_effect(doc, doc::EffectType::CrtSim);
    double total_ms = 0.0;
    for (uint32_t f = 0; f < 12; ++f) {
        const auto start = std::chrono::steady_clock::now();
        if (!readback->render(*engine, doc, doc.looks[0].id, f, 60.0,
                              1920, 1080, pixels)) return false;
        if (f >= 4)
            total_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
    }
    std::printf("CRT 1920x1080: %.3f ms mean, render and readback\n", total_ms / 8.0);
    return true;
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
    SyntheticSource source(kWidth, kHeight);
    std::vector<uint8_t> nv12;
    hashes.clear();
    for (uint32_t f = 0; f < frames; ++f) {
        const auto lf = media_frame(doc, source.planes(f));
        if (!readback->render(*engine, doc, doc.looks[0].id, f, 30.0, kWidth,
                              kHeight, nv12, 0, f, &lf, 1)) {
            std::fprintf(stderr, "render failed at frame %u\n", f);
            return false;
        }
        hashes.push_back(fnv1a(nv12.data(), nv12.size()));
    }
    return true;
}

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
    SyntheticSource source(1920, 1080);

    struct Row {
        double ms;
        std::string name;
    };
    std::vector<Row> rows;

    // Keep this warmup: idle GPU clocks make the first rows measure too slow.
    {
        auto engine = gfx::Engine::create(device, shader_dir);
        auto readback = gfx::Nv12Readback::create(device, shader_dir);
        if (!engine || !readback) {
            std::fprintf(stderr, "bench: engine init failed\n");
            return 1;
        }
        doc::Document doc = doc_with_media_look();
        doc.master_seed = 77;
        doc.canvas_w = source.w;
        doc.canvas_h = source.h;
        std::vector<uint8_t> nv12;
        for (uint32_t f = 0; f < 60; ++f) {
            const auto lf = media_frame(doc, source.planes(f));
            if (!readback->render(*engine, doc, doc.looks[0].id, f, 30.0,
                                  source.w, source.h, nv12, 0, f, &lf, 1)) {
                std::fprintf(stderr, "bench: warmup render failed\n");
                return 1;
            }
        }
    }

    // The range is deliberate: t = -1 is the baseline, t = kTypes is an extra
    // row for the Error Diffusion exact mode.
    const int kTypes = static_cast<int>(doc::EffectType::Count);
    for (int t = -1; t <= kTypes; ++t) {
        doc::Document doc = doc_with_media_look();
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
                doc.looks[0].layers[0].stack[0].params[4] = 0.0f;
        }
        auto engine = gfx::Engine::create(device, shader_dir);
        auto readback = gfx::Nv12Readback::create(device, shader_dir);
        if (!engine || !readback) {
            std::fprintf(stderr, "bench: engine init failed\n");
            return 1;
        }
        std::vector<uint8_t> nv12;
        double best = 1.0e9;
        for (uint32_t f = 0; f < kFrames; ++f) {
            // Keep planes() before t0: its CPU cost would hide the GPU cost.
            const auto lf = media_frame(doc, source.planes(f));
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
    // A crash must report, not raise a modal box that stalls the runner.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
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

    gfx::DeviceDesc desc;   // no hwnd: the device stays headless
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

    if (!matte_alpha_check(*device, shader_dir)) {
        std::fprintf(stderr, "MATTE ALPHA FAILURE\n");
        return 1;
    }
    if (!crt_check(*device, shader_dir)) {
        std::fprintf(stderr, "CRT FAILURE\n");
        return 1;
    }

    if (!export_path.empty()) {
        auto engine = gfx::Engine::create(*device, shader_dir);
        auto readback = gfx::Nv12Readback::create(*device, shader_dir);
        if (!engine || !readback) return 1;
        SyntheticSource source(kWidth, kHeight);
        auto producer = [&](uint32_t f, std::vector<uint8_t>& nv12) {
            const auto lf = media_frame(doc, source.planes(f));
            return readback->render(*engine, doc, doc.looks[0].id, f, 30.0,
                                    kWidth, kHeight, nv12, 0, f, &lf, 1);
        };
        media::MixState mix;
        std::vector<float> scratch;
        media::ExportAudio audio;
        if (!pcm_path.empty()) {
            if (auto pcm = media::load_pcm(pcm_path)) {
                mix.fps = 30.0;
                mix.rate = pcm->rate;
                mix.channels = pcm->channels;
                media::MixNode leaf;
                leaf.pcm = pcm;
                mix.nodes.push_back(std::move(leaf));
                media::MixNode hop;
                hop.windowed = true;
                hop.w1 = frames;
                hop.inputs.push_back(0);
                mix.nodes.push_back(std::move(hop));
                mix.root = 1;
                media::prepare_mix(mix);
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

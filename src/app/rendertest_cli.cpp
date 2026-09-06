// Exit code 77 tells CTest to record a skip.

#include <algorithm>
#include <array>
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
#include "gfx/error_diffusion.h"
#include <vk_mem_alloc.h>
#include "gfx/vk_device.h"
#include "media/audio_mix.h"
#include "media/bmff.h"
#include "media/export.h"
#include "util/file.h"
#include "util/hash.h"
#include "ui/ui_renderer.h"

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

bool alpha_check(gfx::Device& device, const std::filesystem::path& shader_dir) {
    constexpr uint32_t w = 160, h = 120;
    constexpr size_t bytes = size_t(w) * h * 8;
    struct Probe {
        gfx::Device& device;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        void* mapped = nullptr;
        ~Probe() {
            device.wait_idle();
            if (buffer) vmaDestroyBuffer(device.allocator(), buffer, allocation);
            if (fence) vkDestroyFence(device.device(), fence, nullptr);
            if (pool) vkDestroyCommandPool(device.device(), pool, nullptr);
        }
    } probe{device};
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = device.graphics_family();
    if (vkCreateCommandPool(device.device(), &pi, nullptr, &probe.pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ci.commandPool = probe.pool;
    ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ci.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device.device(), &ci, &probe.cmd) != VK_SUCCESS) return false;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(device.device(), &fi, nullptr, &probe.fence) != VK_SUCCESS ||
        !gfx::create_mapped_buffer(device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   &probe.buffer, &probe.allocation, &probe.mapped)) return false;
    auto engine = gfx::Engine::create(device, shader_dir);
    if (!engine) return false;
    doc::Document doc;
    doc.cache_mb = 0;
    doc.canvas_w = w;
    doc.canvas_h = h;
    auto look = doc::make_look(doc, "Alpha checks");
    look.layers.push_back(doc::make_layer(doc, doc::LayerSourceKind::Solid));
    look.layers[0].opacity = 1.0f;
    look.layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Invert));
    doc.looks.push_back(std::move(look));
    auto& layer = doc.looks[0].layers[0];
    auto& fx = layer.stack[0];
    layer.color_a[0] = 0.7f;
    layer.color_a[1] = 0.35f;
    layer.color_a[2] = 0.15f;
    std::vector<float> pixels(w * h * 4);
    auto begin_probe = [&] {
        vkResetCommandPool(device.device(), probe.pool, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        return vkBeginCommandBuffer(probe.cmd, &bi) == VK_SUCCESS;
    };
    auto read_pixels = [&](gfx::GpuImage* result) {
        if (!result) { vkEndCommandBuffer(probe.cmd); return false; }
        result->transition(probe.cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(probe.cmd, result->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               probe.buffer, 1, &copy);
        gfx::memory_barrier(probe.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        if (vkEndCommandBuffer(probe.cmd) != VK_SUCCESS) return false;
        gfx::submit_and_wait(device, device.graphics_queue(), probe.cmd, probe.fence, "alpha readback");
        vmaInvalidateAllocation(device.allocator(), probe.allocation, 0, bytes);
        const auto* halves = static_cast<const uint16_t*>(probe.mapped);
        for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = gfx::half_to_float(halves[i]);
        return true;
    };
    auto render = [&](uint32_t frame, bool edited = true, uint32_t divisor = 1) {
        if (edited) ++doc.revision;
        engine->set_preview_divisor(divisor);
        if (!begin_probe()) return false;
        return read_pixels(engine->render(probe.cmd, 0, doc, doc.looks[0].id,
            frame, 60.0, w * divisor, h * divisor));
    };
    int failures = 0;
    int cases = 0;
    for (int t = 0; t < int(doc::EffectType::Count); ++t) {
        const auto type = static_cast<doc::EffectType>(t);
        const auto& info = doc::effect_info(type);
        fx = doc::make_effect(doc, type);
        const auto defaults = fx.params;
        std::vector<std::pair<int, float>> variants{{-1, 0.0f}};
        for (uint32_t p = 0; p < info.param_count; ++p) {
            const auto& pd = info.params[p];
            if (pd.options) {
                for (float v = pd.min_value; v <= pd.max_value; v += 1.0f)
                    if (v != defaults[p]) variants.emplace_back(int(p), v);
            } else {
                if (pd.max_value != defaults[p]) variants.emplace_back(int(p), pd.max_value);
                if (pd.min_value != defaults[p]) variants.emplace_back(int(p), pd.min_value);
            }
        }
        for (const auto& variant : variants) {
            fx.params = defaults;
            if (variant.first >= 0) fx.params[variant.first] = variant.second;
            for (int input = 0; input < 3; ++input) {
                layer.source = input == 2 ? doc::LayerSourceKind::Shape : doc::LayerSourceKind::Solid;
                layer.gen_scale = 0.35f;
                layer.color_a[3] = input == 0 ? 0.0f : 0.5f;
                if (!render(input == 2 ? 1u : 0u)) return false;
                ++cases;
                for (size_t i = 0; i < pixels.size(); i += 4) {
                    const float a = pixels[i + 3];
                    bool valid = std::isfinite(a) && a >= -0.0001f && a <= 1.0001f;
                    for (int c = 0; c < 3; ++c)
                        valid = valid && std::isfinite(pixels[i + c]) &&
                            (a > 0.0f || std::abs(pixels[i + c]) <= 0.000002f);
                    if (!valid) {
                        std::fprintf(stderr, "Alpha fault %s param %d value %.3f input %d pixel %zu: %.5f %.5f %.5f %.5f\n",
                            info.id, variant.first, variant.second, input, i / 4,
                            pixels[i], pixels[i + 1], pixels[i + 2], a);
                        ++failures;
                        break;
                    }
                }
            }
        }
    }
    layer.source = doc::LayerSourceKind::Solid;
    for (int t = 0; t < int(doc::EffectType::Count); ++t) {
        const auto type = static_cast<doc::EffectType>(t);
        const auto& info = doc::effect_info(type);
        bool preserves = false;
        for (const char* id : {"invert", "levels", "hue_sat", "channel_mix", "split_tone",
             "colorizer", "palette_map", "posterize", "threshold", "solarize", "quantize",
             "bit_plane", "grain", "halftone", "cross_hatch", "photocopy", "risograph",
             "watercolor", "wet_plate", "cel_shade", "dither", "contour", "edge_detect",
             "emulsion", "direct_flash", "oversharpen", "mosquito", "composite_artifacts", "film_stock", "lidar"})
            preserves = preserves || std::strcmp(info.id, id) == 0;
        if (!preserves) continue;
        fx = doc::make_effect(doc, type);
        if (type == doc::EffectType::Photocopy) fx.params[2] = 0.0f;
        for (float wet : {1.0f, 0.37f}) {
            fx.wet = wet;
            layer.color_a[3] = 1.0f;
            if (!render(0)) return false;
            const auto opaque = pixels;
            for (float alpha : {0.5f, 0.0625f, 0.000244140625f, 0.0f}) {
                layer.color_a[3] = alpha;
                if (!render(0)) return false;
                ++cases;
                for (size_t i = 0; i < pixels.size(); ++i) {
                    if (std::abs(pixels[i] - opaque[i] * alpha) > 0.003f * alpha + 0.000002f) {
                        std::fprintf(stderr, "Alpha changes color %s alpha %.4f wet %.2f component %zu: %.6f expected %.6f\n",
                            info.id, alpha, wet, i, pixels[i], opaque[i] * alpha);
                        ++failures;
                        break;
                    }
                }
            }
        }
    }
    for (int t = 0; t < int(doc::EffectType::Count); ++t) {
        const auto type = static_cast<doc::EffectType>(t);
        if (type != doc::EffectType::ErrorDiffusion && !doc::is_codec_box(type)) continue;
        fx = doc::make_effect(doc, type);
        layer.color_a[0] = layer.color_a[1] = layer.color_a[2] = 1.0f;
        for (float alpha : {0.0f, 0.5f, 1.0f, 0.0625f}) {
            layer.color_a[3] = alpha;
            if (!render(0) || !render(0, false)) return false;
            ++cases;
            double light = 0.0;
            for (size_t i = 0; i < pixels.size(); i += 4) light += pixels[i];
            light /= w * h;
            if (std::abs(light - alpha) > 0.03f * alpha + 0.000002f ||
                std::abs(pixels[3] - alpha) > 0.0001f) {
                std::fprintf(stderr, "Alpha paused input %s: %.4f expected %.4f\n",
                    doc::effect_info(type).id, light, alpha);
                ++failures;
            }
        }
    }
    fx.wet = 0.0f;
    layer.color_a[3] = 0.5f;
    layer.blend = doc::BlendMode::Normal;
    if (!render(0)) return false;
    const auto normal = pixels;
    for (int mode = 1; mode <= 4; ++mode) {
        layer.blend = static_cast<doc::BlendMode>(mode);
        if (!render(0)) return false;
        ++cases;
        for (size_t i = 0; i < pixels.size(); ++i) {
            if (std::abs(pixels[i] - normal[i]) > 0.0005f) {
                std::fprintf(stderr, "Alpha blend over empty: mode %d\n", mode);
                ++failures;
                break;
            }
        }
    }
    layer.blend = doc::BlendMode::Normal;
    layer.color_a[3] = 1.0f;
    auto expect_color = [&](const char* name, const float* expected, float tolerance) {
        ++cases;
        for (size_t i = 0; i < pixels.size(); ++i) {
            if (std::abs(pixels[i] - expected[i % 4]) > tolerance) {
                std::fprintf(stderr, "Color %s: %.6f expected %.6f\n", name, pixels[i], expected[i % 4]);
                ++failures;
                break;
            }
        }
    };
    fx = doc::make_effect(doc, doc::EffectType::Watercolor);
    fx.params.assign(fx.params.size(), 0.0f);
    for (float level : {0.02f, 0.2f, 0.5f, 0.9f}) {
        layer.color_a[0] = level;
        layer.color_a[1] = level * 0.5f;
        layer.color_a[2] = level * 0.25f;
        fx.wet = 0.0f;
        if (!render(0)) return false;
        const float expected[4] = {pixels[0], pixels[1], pixels[2], 1.0f};
        fx.wet = 1.0f;
        if (!render(0)) return false;
        expect_color("watercolor neutral", expected, 0.0005f);
    }
    fx = doc::make_effect(doc, doc::EffectType::Risograph);
    fx.params = {1.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f};
    for (float density : {0.25f, 0.5f, 1.0f}) {
        float expected[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        for (int c = 0; c < 3; ++c) {
            expected[c] = std::pow(color::srgb_eotf(c == 0 ? 0.8f : 0.05f), density);
            layer.color_a[c] = color::srgb_oetf(expected[c]);
        }
        if (!render(0)) return false;
        expect_color("ink transmittance", expected, 0.001f);
    }
    fx = doc::make_effect(doc, doc::EffectType::Halftone);
    fx.params[0] = 2.0f;
    for (float level : {0.2f, 0.5f, 0.8f}) {
        layer.color_a[0] = layer.color_a[1] = layer.color_a[2] = level;
        const float linear = color::srgb_eotf(level);
        const float expected[4] = {linear, linear, linear, 1.0f};
        if (!render(0, true, 8)) return false;
        expect_color("unresolved halftone", expected, 0.001f);
    }
    for (auto type : {doc::EffectType::Halftone, doc::EffectType::CrossHatch}) {
        fx = doc::make_effect(doc, type);
        fx.params[0] = 8.0f;
        if (type == doc::EffectType::CrossHatch) fx.params[3] = 0.0f;
        for (float level : {0.2f, 0.5f, 0.7f}) {
            layer.color_a[0] = layer.color_a[1] = layer.color_a[2] = level;
            double reference = 0.0;
            for (uint32_t divisor : {1u, 4u, 8u}) {
                if (!render(0, true, divisor)) return false;
                double mean = 0.0;
                for (size_t i = 0; i < pixels.size(); i += 4) mean += pixels[i];
                mean /= w * h;
                if (divisor == 1) reference = mean;
                ++cases;
                if (std::abs(mean - reference) > 0.01) {
                    std::fprintf(stderr, "Color %s preview /%u mean %.5f expected %.5f\n",
                        doc::effect_info(type).id, divisor, mean, reference);
                    ++failures;
                }
            }
        }
    }
    fx = doc::make_effect(doc, doc::EffectType::Grain);
    fx.params[0] = 0.5f;
    fx.params[2] = 1.0f;
    fx.params[3] = 1.0f;
    double dark_variance = 0.0;
    for (float linear : {0.1f, 0.6f}) {
        layer.color_a[0] = layer.color_a[1] = layer.color_a[2] = color::srgb_oetf(linear);
        if (!render(0)) return false;
        double mean = 0.0, square = 0.0;
        for (size_t i = 0; i < pixels.size(); i += 4) {
            mean += pixels[i];
            square += pixels[i] * pixels[i];
        }
        mean /= w * h;
        const double variance = square / (w * h) - mean * mean;
        ++cases;
        if (std::abs(mean - linear) > 0.003 || variance <= 0.00001) {
            std::fprintf(stderr, "Color sensor noise: mean %.5f variance %.6f\n", mean, variance);
            ++failures;
        }
        if (linear < 0.2f) dark_variance = variance;
        else if (variance < dark_variance * 4.0 || variance > dark_variance * 8.0) {
            std::fprintf(stderr, "Color shot noise does not follow exposure\n");
            ++failures;
        }
    }
    layer.source = doc::LayerSourceKind::Gradient;
    layer.color_a[0] = 0.7f;
    layer.color_a[1] = 0.35f;
    layer.color_a[2] = 0.15f;
    layer.color_b[0] = 0.1f;
    layer.color_b[1] = 0.6f;
    layer.color_b[2] = 0.3f;
    layer.stops.resize(2);
    layer.stops[0].t = 0.0f;
    layer.stops[1].t = 1.0f;
    for (int c = 0; c < 3; ++c) {
        layer.stops[0].color[c] = layer.color_a[c];
        layer.stops[1].color[c] = layer.color_b[c];
    }
    layer.blend = doc::BlendMode::Normal;
    for (int t = 0; t < int(doc::EffectType::Count); ++t) {
        const auto type = static_cast<doc::EffectType>(t);
        const auto& info = doc::effect_info(type);
        bool check = false;
        for (const char* id : {"anaglyph", "rutt_etra", "fm_synth", "engraver", "sharpen", "zoom_crunch", "pixel_sort"})
            check = check || std::strcmp(info.id, id) == 0;
        if (!check) continue;
        fx = doc::make_effect(doc, type);
        layer.color_a[3] = layer.color_b[3] = 1.0f;
        layer.stops[0].color[3] = layer.stops[1].color[3] = 1.0f;
        if (!render(0)) return false;
        const auto opaque = pixels;
        layer.color_a[3] = layer.color_b[3] = 0.5f;
        layer.stops[0].color[3] = layer.stops[1].color[3] = 0.5f;
        if (!render(0)) return false;
        ++cases;
        for (size_t i = 0; i < pixels.size(); ++i) {
            if (std::abs(pixels[i] - opaque[i] * 0.5f) > 0.002f) {
                std::fprintf(stderr, "Alpha changes signal %s\n", info.id);
                ++failures;
                break;
            }
        }
    }
    layer.source = doc::LayerSourceKind::Shape;
    layer.gen_scale = 0.35f;
    for (auto type : {doc::EffectType::Sharpen, doc::EffectType::ZoomCrunch}) {
        fx = doc::make_effect(doc, type);
        fx.params[0] = 1.0f;
        fx.wet = 0.0f;
        layer.blend = doc::BlendMode::Normal;
        if (!render(0)) return false;
        const auto dry = pixels;
        fx.wet = 1.0f;
        if (!render(0)) return false;
        ++cases;
        for (size_t i = 0; i < pixels.size(); ++i) {
            if (std::abs(pixels[i] - dry[i]) > 0.002f) {
                std::fprintf(stderr, "Alpha edge changes color %s\n", doc::effect_info(type).id);
                ++failures;
                break;
            }
        }
    }
    layer.source = doc::LayerSourceKind::Solid;
    layer.stack.clear();
    layer.color_a[0] = 1.0f;
    layer.color_a[1] = layer.color_a[2] = 0.0f;
    layer.color_a[3] = 0.25f;
    auto upper = doc::make_layer(doc, doc::LayerSourceKind::Solid);
    upper.opacity = 1.0f;
    upper.color_a[0] = upper.color_a[1] = 0.0f;
    upper.color_a[2] = 1.0f;
    upper.color_a[3] = 0.5f;
    doc.looks[0].layers.push_back(std::move(upper));
    for (bool node_blend : {false, true}) {
        auto& bottom = doc.looks[0].layers[0];
        auto& top = doc.looks[0].layers[1];
        if (node_blend) {
            bottom.stack.push_back(doc::make_effect(doc, doc::EffectType::BlendNode));
            const uint64_t blend_id = bottom.stack[0].id;
            doc.looks[0].links = {{bottom.id, blend_id, 0}, {top.id, blend_id, 2}, {blend_id, 0, 0}};
            top.blend = doc::BlendMode::Normal;
        }
        for (int mode = 0; mode < (node_blend ? 9 : 5); ++mode) {
            if (node_blend) bottom.stack[0].params[0] = float(mode);
            else top.blend = static_cast<doc::BlendMode>(mode);
            if (!render(0)) return false;
            ++cases;
            float expected[4] = {0.125f, 0.0f, 0.375f, 0.625f};
            if (mode == 0) expected[2] = 0.5f;
            else if (mode == 1 || mode == 3 || mode == 4 || mode == 7) {
                expected[0] += 0.125f;
                expected[2] += 0.125f;
            } else if (mode == 5 || mode == 8) expected[0] += 0.125f;
            for (size_t i = 0; i < pixels.size(); ++i) {
                if (std::abs(pixels[i] - expected[i % 4]) > 0.0005f) {
                    std::fprintf(stderr, "Alpha blend composition node %d mode %d: %.5f expected %.5f\n",
                        int(node_blend), mode, pixels[i], expected[i % 4]);
                    ++failures;
                    break;
                }
            }
        }
    }
    auto& mask = doc.looks[0].layers[0];
    auto& white = doc.looks[0].layers[1];
    mask.stack.clear();
    mask.stack.push_back(doc::make_effect(doc, doc::EffectType::Matte));
    const uint64_t mask_id = mask.stack[0].id;
    mask.opacity = white.opacity = 1.0f;
    for (int c = 0; c < 4; ++c) {
        mask.color_a[c] = 0.5f;
        white.color_a[c] = 1.0f;
    }
    doc.looks[0].links = {{mask.id, mask_id, 0}, {mask_id, 0, 0}};
    mask.stack[0].params[7] = 1.0f;
    if (!render(0)) return false;
    const float cutout_alpha = pixels[3];
    ++cases;
    if (std::abs(cutout_alpha - 0.25f) > 0.0005f) ++failures;
    mask.stack[0].params[7] = 0.0f;
    if (!render(0)) return false;
    ++cases;
    if (std::abs(pixels[0] - cutout_alpha) > 0.0005f) {
        std::fprintf(stderr, "Alpha matte value: %.5f expected %.5f\n", pixels[0], cutout_alpha);
        ++failures;
    }
    doc.looks[0].links = {{mask.id, mask_id, 0}, {mask_id, white.id, 1}, {white.id, 0, 0}};
    if (!render(0)) return false;
    ++cases;
    if (std::abs(pixels[3] - cutout_alpha) > 0.0005f) {
        std::fprintf(stderr, "Alpha wired matte: %.5f expected %.5f\n", pixels[3], cutout_alpha);
        ++failures;
    }
    doc::Document composite_doc;
    composite_doc.cache_mb = 0;
    auto composite_look = doc::make_look(composite_doc, "Composite checks");
    for (int c = 0; c < 3; ++c) {
        auto source = doc::make_layer(composite_doc, doc::LayerSourceKind::Solid);
        source.color_a[0] = source.color_a[1] = source.color_a[2] = 0.0f;
        source.color_a[c] = 1.0f;
        source.opacity = c == 0 ? 0.5f : c == 1 ? 0.25f : 0.75f;
        composite_look.layers.push_back(std::move(source));
    }
    composite_doc.looks.push_back(std::move(composite_look));
    auto& cl = composite_doc.looks[0];
    auto render_composite = [&](uint64_t root = 0, uint64_t preview = 0) {
        ++composite_doc.revision;
        engine->set_preview_divisor(1);
        if (!begin_probe()) return false;
        return read_pixels(engine->render(probe.cmd, 0, composite_doc,
            root ? root : cl.id, 0, 60.0, w, h, 0, 0, nullptr,
            nullptr, 0, preview));
    };
    std::array<int, 3> permutation{0, 1, 2};
    do {
        cl.links.clear();
        float expected[4]{};
        for (int c : permutation) {
            const auto& source = cl.layers[c];
            cl.links.push_back({source.id, 0, 0});
            for (int k = 0; k < 4; ++k) expected[k] *= 1.0f - source.opacity;
            expected[c] += source.opacity;
            expected[3] += source.opacity;
        }
        if (!render_composite()) return false;
        expect_color("ordered three-source composite", expected, 0.001f);
    } while (std::next_permutation(permutation.begin(), permutation.end()));
    const uint64_t red = cl.layers[0].id, green = cl.layers[1].id;
    cl.layers[0].stack.push_back(doc::make_effect(composite_doc, doc::EffectType::Invert));
    const uint64_t invert = cl.layers[0].stack[0].id;
    cl.layers[0].stack[0].opacity = 0.5f;
    cl.links = {{red, invert, 0}, {invert, 0, 0}};
    if (!render_composite()) return false;
    const float half_effect[4] = {0.25f, 0.25f, 0.25f, 0.5f};
    expect_color("source and effect opacity", half_effect, 0.001f);
    if (!render_composite(0, red)) return false;
    const float red_source[4] = {0.5f, 0.0f, 0.0f, 0.5f};
    expect_color("source preview opacity", red_source, 0.001f);
    cl.layers[0].stack[0].opacity = 1.0f;
    cl.layers[0].stack.push_back(doc::make_effect(composite_doc, doc::EffectType::Blur));
    const uint64_t face = cl.layers[0].stack[1].id;
    cl.layers[0].stack[1].bypass = true;
    cl.links = {{red, invert, 0}, {red, face, 0}, {face, invert, 0}, {invert, 0, 0}};
    if (!render_composite()) return false;
    const float diamond[4] = {0.0f, 0.75f, 0.75f, 0.75f};
    expect_color("shared source diamond", diamond, 0.001f);
    std::reverse(cl.layers[0].stack.begin(), cl.layers[0].stack.end());
    if (!render_composite()) return false;
    expect_color("diamond storage order", diamond, 0.001f);
    std::reverse(cl.layers[0].stack.begin(), cl.layers[0].stack.end());
    cl.layers[1].color_a[0] = cl.layers[1].color_a[1] = cl.layers[1].color_a[2] = 1.0f;
    cl.links = {{green, red, 1}, {red, invert, 0}, {invert, 0, 0}};
    if (!render_composite()) return false;
    const float masked[4] = {0.0f, 0.125f, 0.125f, 0.125f};
    expect_color("source opacity and mask", masked, 0.001f);
    doc::Group composite_group;
    composite_group.id = composite_doc.next_effect_id++;
    composite_group.inputs = {composite_doc.next_effect_id++};
    composite_group.face_out = face;
    composite_group.wet = 0.25f;
    cl.layers[0].groups.push_back(composite_group);
    for (auto& effect : cl.layers[0].stack) effect.group_id = composite_group.id;
    cl.links = {{red, composite_group.inputs[0], 0},
        {composite_group.inputs[0], invert, 0}, {invert, face, 0}, {face, 0, 0}};
    if (!render_composite()) return false;
    const float grouped[4] = {0.375f, 0.125f, 0.125f, 0.5f};
    expect_color("group mix with bypassed face", grouped, 0.001f);
    cl.links.push_back({green, composite_group.id, 1});
    if (!render_composite()) return false;
    const float grouped_mask[4] = {0.46875f, 0.03125f, 0.03125f, 0.5f};
    expect_color("group mix before mask", grouped_mask, 0.001f);
    cl.layers[0].groups.clear();
    for (auto& effect : cl.layers[0].stack) effect.group_id = 0;
    cl.links = {{red, invert, 0}, {invert, 0, 0}};
    cl.layers[0].stack[0].blend = doc::BlendMode::Add;
    cl.layers[0].stack[0].opacity = 0.25f;
    if (!render_composite()) return false;
    const float effect_blend[4] = {0.5f, 0.125f, 0.125f, 0.5625f};
    expect_color("effect blend and opacity", effect_blend, 0.001f);
    cl.layers[0].stack[0].blend = doc::BlendMode::Normal;
    cl.links = {{red, 0, 0}};
    doc::Placement placement;
    placement.id = composite_doc.next_effect_id++;
    placement.target = cl.id;
    placement.opacity = 0.5f;
    composite_doc.root().tracks[0].placements.push_back(placement);
    if (!render_composite(composite_doc.root_sequence)) return false;
    const float placed[4] = {0.25f, 0.0f, 0.0f, 0.25f};
    expect_color("source and placement opacity", placed, 0.001f);
    auto ui_renderer = ui::UiRenderer::create(device, VK_FORMAT_R16G16B16A16_SFLOAT, shader_dir);
    auto ui_target = gfx::GpuImage::create(device, VK_FORMAT_R16G16B16A16_SFLOAT, w, h,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!ui_renderer || !ui_target) return false;
    for (const auto& rgba : {std::array<uint8_t, 4>{128, 64, 192, 255},
                             std::array<uint8_t, 4>{128, 128, 128, 128},
                             std::array<uint8_t, 4>{12, 200, 80, 255}}) {
        const auto* texture = ui_renderer->register_image(rgba.data(), 1, 1);
        if (!texture || !begin_probe()) return false;
        ui_target->transition(probe.cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView = ui_target->view();
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, {w, h}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &attachment;
        vkCmdBeginRendering(probe.cmd, &rendering);
        const VkViewport viewport{0.0f, 0.0f, float(w), float(h), 0.0f, 1.0f};
        vkCmdSetViewport(probe.cmd, 0, 1, &viewport);
        ui::Canvas2D canvas;
        canvas.begin_frame(1.0f, {float(w), float(h)});
        canvas.draw_image_quad({0.0f, 0.0f, float(w), float(h)}, texture,
            0.0f, 0.0f, 1.0f, 1.0f, ui::Color::rgba(1.0f, 1.0f, 1.0f));
        ui_renderer->record(probe.cmd, 0, {w, h}, canvas);
        vkCmdEndRendering(probe.cmd);
        if (!read_pixels(ui_target.get())) return false;
        const float alpha = rgba[3] / 255.0f;
        const float expected[4] = {color::srgb_eotf(rgba[0] / 255.0f) * alpha,
            color::srgb_eotf(rgba[1] / 255.0f) * alpha,
            color::srgb_eotf(rgba[2] / 255.0f) * alpha, alpha};
        expect_color("media thumbnail", expected, 0.001f);
    }
    std::printf("Alpha/color: %d cases, %d faults\n", cases, failures);
    return failures == 0;
}

bool simulation_check(gfx::Device& device, const std::filesystem::path& shader_dir) {
    doc::Document doc;
    doc.cache_mb = 0;
    auto look = doc::make_look(doc, "Simulation checks");
    look.layers.push_back(doc::make_layer(doc, doc::LayerSourceKind::Solid));
    look.layers[0].opacity = 1.0f;
    look.layers[0].stack.push_back(doc::make_effect(doc, doc::EffectType::Glyph));
    doc.looks.push_back(std::move(look));
    auto& layer = doc.looks[0].layers[0];
    auto& fx = layer.stack[0];
    auto engine = gfx::Engine::create(device, shader_dir);
    auto readback = gfx::Nv12Readback::create(device, shader_dir);
    if (!engine || !readback) return false;
    uint32_t width = 256, height = 192, divisor = 1;
    std::vector<uint8_t> pixels;
    auto render = [&](uint32_t frame = 0) {
        engine->set_preview_divisor(divisor);
        return readback->render(*engine, doc, doc.looks[0].id, frame, 60.0,
                                width, height, pixels);
    };
    auto mean = [&]() {
        double total = 0.0;
        const uint32_t w = width / divisor, h = height / divisor;
        for (uint32_t y = 8; y < h - 8; ++y)
            for (uint32_t x = 8; x < w - 8; ++x) total += pixels[y * w + x];
        return total / ((w - 16) * (h - 16));
    };
    for (int color = 0; color < 8; ++color) {
        for (int c = 0; c < 3; ++c) layer.color_a[c] = (color & (1 << c)) ? 1.0f : 0.0f;
        fx.wet = 0.0f;
        if (!render()) return false;
        const double expected = mean();
        fx.wet = 1.0f;
        fx.params[1] = 4.0f;
        for (float cell : {2.0f, 7.0f, 32.0f}) {
            fx.params[0] = cell;
            for (uint32_t d : {1u, 2u, 4u}) {
                divisor = d;
                if (!render() || std::abs(mean() - expected) > 1.0) {
                    std::fprintf(stderr, "Glyph color %d cell %.0f /%u mean %.3f expected %.3f\n",
                                 color, cell, d, mean(), expected);
                    return false;
                }
            }
        }
    }
    std::vector<uint8_t> atlas(8 * 8 * 4, 0);
    for (uint32_t y = 0; y < 8; ++y) {
        for (uint32_t x = 0; x < 8; ++x) {
            size_t i = (y * 8 + x) * 4;
            const bool on = (x + y) % 2 == 0;
            atlas[i] = on ? 255 : 0;
            atlas[i + 1] = on ? 0 : 255;
            atlas[i + 3] = on ? 255 : 0;
        }
    }
    if (!engine->set_glyph_atlas_rgba(atlas.data(), 8, 8, 8, 1, 1) ||
        engine->set_glyph_atlas_rgba(atlas.data(), 8, 8, 16, 1, 1)) return false;
    layer.color_a[0] = layer.color_a[1] = layer.color_a[2] = 1.0f;
    fx.params[1] = 2.0f;
    fx.params[0] = 2.0f;
    for (uint32_t d : {1u, 4u}) {
        divisor = d;
        if (!render()) return false;
        const double red = mean();
        if (red < 47.0 || red > 53.0) {
            std::fprintf(stderr, "Glyph custom alpha /%u luma %.3f\n", d, red);
            return false;
        }
        const uint32_t w = width / divisor, h = height / divisor;
        size_t chroma = w * h + (h / 4) * w + w / 2;
        if (pixels[chroma] > 120 || pixels[chroma + 1] < 195) return false;
    }
    std::printf("Glyph: eight colors, cell coverage, custom RGBA and atlas bounds pass\n");
    std::vector<uint8_t> fine_atlas(64 * 64);
    for (uint32_t y = 0; y < 64; ++y)
        for (uint32_t x = 0; x < 64; ++x)
            fine_atlas[y * 64 + x] = (x + y) % 2 == 0 ? 255 : 0;
    if (!engine->set_glyph_atlas(fine_atlas.data(), 64, 64, 64, 1, 1, 2) ||
        !render() || std::abs(mean() - 177.0) > 2.0) return false;
    std::printf("Glyph: fine detail in a 64-pixel atlas keeps mean coverage\n");
    divisor = 1;
    fx = doc::make_effect(doc, doc::EffectType::Glow);
    fx.params[3] = 2.0f;
    fx.params[2] = 0.8f;
    fx.params[0] = 3.0f;
    fx.params[4] = 0.0f;
    layer.color_a[0] = layer.color_a[1] = layer.color_a[2] = 0.5f;
    if (!render()) return false;
    const auto quiet = pixels;
    fx.wet = 0.0f;
    if (!render() || pixels != quiet) return false;
    layer.source = doc::LayerSourceKind::Shape;
    layer.gen_scale = 0.025f;
    layer.gen_angle = 0.0f;
    layer.color_a[0] = layer.color_a[1] = layer.color_a[2] = 1.0f;
    fx.wet = 1.0f;
    fx.params[2] = 0.25f;
    if (!render()) return false;
    const auto bloom = pixels;
    if (bloom[(height / 2 - 8) * width + width / 2] <= 16 ||
        bloom[height / 2 * width + width / 2 + 8] != 16) return false;
    fx.params[4] = 1.0f;
    if (!render() || pixels[height / 4 * width + width / 2] <= bloom[height / 4 * width + width / 2]) return false;
    if (pixels[height / 4 * width + width / 4] != 16) return false;
    std::printf("CCD: quiet field, column smear and empty columns pass\n");
    fx = doc::make_effect(doc, doc::EffectType::Streak);
    fx.params[0] = 256.0f;
    fx.params[2] = 0.99f;
    if (!render()) return false;
    for (uint32_t y : {height / 2 - 12, height / 2 + 12})
        for (uint32_t x = width / 2; x < width; ++x)
            if (pixels[y * width + x] != 16) return false;
    std::printf("Streak: maximum length preserves cross-axis width\n");
    fx = doc::make_effect(doc, doc::EffectType::Jitter);
    layer.source = doc::LayerSourceKind::TestPattern;
    layer.osc_shape = 4;
    for (float mode : {1.0f, 3.0f}) {
        fx.params[2] = mode;
        if (!render(17)) return false;
        const auto target = pixels;
        if (!render(18) || pixels == target || !render(17) || pixels != target) return false;
    }
    std::printf("Jitter: camera motion changes in time and repeats after a seek\n");
    for (const auto type : {doc::EffectType::Glyph, doc::EffectType::Streak,
                           doc::EffectType::Glow, doc::EffectType::Jitter}) {
        fx = doc::make_effect(doc, type);
        if (type == doc::EffectType::Glyph) { fx.params[0] = 2.0f; fx.params[1] = 3.0f; }
        if (type == doc::EffectType::Streak) fx.params[0] = 256.0f;
        if (type == doc::EffectType::Glow) { fx.params[0] = 3.0f; fx.params[3] = 2.0f; }
        if (type == doc::EffectType::Jitter) { fx.params[0] = 48.0f; fx.params[1] = 12.0f; fx.params[2] = 1.0f; }
        for (uint32_t scale : {1u, 2u}) {
            width = 1920 * scale;
            height = 1080 * scale;
            double total = 0.0;
            for (uint32_t f = 0; f < 12; ++f) {
                const auto start = std::chrono::steady_clock::now();
                if (!render(f)) return false;
                if (f >= 4) total += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count();
            }
            std::printf("Simulation limit %ux%u %s: %.3f ms mean, render and readback\n",
                        width, height, doc::effect_info(type).label, total / 8.0);
        }
    }
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
    if (!alpha_check(*device, shader_dir)) return 1;
    if (!simulation_check(*device, shader_dir)) {
        std::fprintf(stderr, "SIMULATION FAILURE\n");
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

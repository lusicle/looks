#include "gfx/engine.h"

#include <vk_mem_alloc.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <vector>

#include "codec/core.h"   // parallel_blocks for the CPU dither passes
#include "doc/effects.h"
#include "gfx/graph.h"
#include "gfx/vk_device.h"
#include "util/file.h"
#include "util/hash.h"
#include "util/image.h"
#include "util/log.h"

namespace looks::gfx {

namespace {

struct FxShaderDesc {
    const char* spv_name;
    uint32_t sampled_inputs;
};

// Indexed by doc::EffectType. Push layout is shared by every effect kernel:
// {uint width, uint height, float wet, float opacity, uint seed, uint frame,
//  float fps, float params[param_count]} — the doc param order matches the
// shader cbuffer member order by construction. Kernels apply the canonical
// composition: final = mix(in, blend(in, mix(in, fx(in), wet)), opacity).
// A null spv marks a multi-pass effect with dedicated pipelines (glow).
constexpr uint32_t kFxPreludeWords = 7;
constexpr FxShaderDesc kFxShaders[] = {
    {"fx_rgb_split.comp.spv", 1},
    {"fx_vignette.comp.spv", 1},
    {"fx_pixelate.comp.spv", 1},
    {"fx_grain.comp.spv", 1},
    {"fx_jitter.comp.spv", 1},
    {"fx_quantize.comp.spv", 4},        // input + LUT + RD + flow (lock)
    {nullptr, 1},                       // Glow: multi-pass
    {"fx_flow_smear.comp.spv", 2},      // input + flow field
    {"fx_motion_extract.comp.spv", 3},  // input + prev/cur luma
    {nullptr, 1},                       // Datamosh (Codec-Box, CPU)
    {nullptr, 1},                       // Generation Loss
    {nullptr, 1},                       // Bitrate Starve
    {"fx_echo.comp.spv", 2},            // input + own previous output
    {"fx_feedback.comp.spv", 2},
    {"fx_glyph.comp.spv", 2},           // input + glyph atlas
    {"fx_film_stock.comp.spv", 1},
    {"fx_kaleido.comp.spv", 1},
    {"fx_polar.comp.spv", 1},
    {"fx_turbulence.comp.spv", 1},
    {"fx_displace.comp.spv", 2},        // input + displacement map
    {"fx_lens_distort.comp.spv", 1},
    {"fx_fringe.comp.spv", 1},
    {"fx_interlace.comp.spv", 1},
    {"fx_slice_shuffle.comp.spv", 1},
    {"fx_pixel_stretch.comp.spv", 1},
    {"fx_composite.comp.spv", 1},
    {"fx_snow.comp.spv", 1},
    {"fx_sync_fail.comp.spv", 1},
    {"fx_timestamp.comp.spv", 1},
    {"fx_oversharpen.comp.spv", 1},
    {"fx_blur.comp.spv", 1},
    {"fx_dust_scratches.comp.spv", 2},   // input + damage plate
    {"fx_light_leak.comp.spv", 1},
    {"fx_anamorphic.comp.spv", 1},
    {"fx_direct_flash.comp.spv", 1},
    {"fx_edge_detect.comp.spv", 1},
    {"fx_kuwahara.comp.spv", 1},
    {"fx_cel_shade.comp.spv", 1},
    {"fx_rutt_etra.comp.spv", 1},
    {"fx_slit_scan.comp.spv", 2},       // input + one history slice per pass
    {"fx_camcorder_hud.comp.spv", 1},
    {"fx_gate_mask.comp.spv", 1},
    {"fx_cue_mark.comp.spv", 1},
    {"fx_screen_texture.comp.spv", 1},
    {"fx_voronoi.comp.spv", 1},
    {"fx_reaction_diffusion.comp.spv", 2},   // input + RD state
    {nullptr, 1},                            // Error Diffusion (CPU)
    {"fx_flicker.comp.spv", 1},
    {"fx_frame_hold.comp.spv", 2},           // input + held frame
    {"fx_stutter.comp.spv", 2},              // input + ring frame
    {"fx_contour.comp.spv", 1},
    {"fx_flow_particles.comp.spv", 3},       // input + own prev + flow
    {"fx_spherize.comp.spv", 1},
    {"fx_soft_upscale.comp.spv", 1},
    {"fx_zoom_crunch.comp.spv", 1},
    {"fx_pixel_sort.comp.spv", 1},
    {"fx_wave_warp.comp.spv", 1},
    {"fx_crt_sim.comp.spv", 1},
    {"fx_halftone.comp.spv", 1},
    {"fx_star_filter.comp.spv", 1},
    {"fx_streak.comp.spv", 1},
    {"fx_split_tone.comp.spv", 1},
    {"fx_corner_soft.comp.spv", 1},
    {"fx_head_switch.comp.spv", 1},
    {"fx_vhs_osd.comp.spv", 1},
    {"fx_cam_auto.comp.spv", 1},
    {"fx_mosquito.comp.spv", 1},
    {"fx_bit_plane.comp.spv", 1},
    {"fx_block_shuffle.comp.spv", 1},
    {"fx_buffer_glitch.comp.spv", 1},
    {"fx_cross_hatch.comp.spv", 1},
    {"fx_splice_bump.comp.spv", 1},
    {"fx_film_slip.comp.spv", 1},
    {"fx_emulsion.comp.spv", 1},
    {"fx_time_displace.comp.spv", 3},   // input + history slice + map
    {"fx_flow_paint.comp.spv", 2},      // input + flow field
    {"fx_fm_synth.comp.spv", 1},
    {"fx_colorizer.comp.spv", 1},
    {"fx_solarize.comp.spv", 1},
    {"fx_invert.comp.spv", 1},
    {"fx_twirl.comp.spv", 1},
    {"fx_tile.comp.spv", 1},
    {"fx_emboss.comp.spv", 1},
    {"fx_lens_flare.comp.spv", 1},
    {"fx_velocity_scan.comp.spv", 3},   // input + canvas + front field
    {"fx_lidar.comp.spv", 2},           // input + own previous output
    {"fx_anaglyph.comp.spv", 1},
    {"fx_photocopy.comp.spv", 1},
    {"fx_risograph.comp.spv", 1},
    {"fx_wet_plate.comp.spv", 1},
    {"fx_reeded_glass.comp.spv", 1},
    {"fx_watercolor.comp.spv", 1},
    {"fx_wire_terrain.comp.spv", 1},
    {"fx_ridgeline.comp.spv", 1},
    {"fx_slow_scan.comp.spv", 2},       // input + own previous output
    {"fx_vector_trace.comp.spv", 2},
    {"fx_scope.comp.spv", 2},
    {"fx_security_mux.comp.spv", 2},    // input + one history slice per pass
    {"fx_audio_scope.comp.spv", 2},     // input + waveform strip
    {"fx_engraver.comp.spv", 2},        // input + FM phase integral
    {"fx_blend_node.comp.spv", 2},      // In + B aux input (graph merge)
    {"fx_matte.comp.spv", 1},           // matte maker
    {"fx_levels.comp.spv", 1},
    {"fx_hue_sat.comp.spv", 1},
    {"fx_channel_mix.comp.spv", 1},
    {"fx_posterize.comp.spv", 1},
    {"fx_threshold.comp.spv", 1},
    {"fx_palette_map.comp.spv", 1},
    {"fx_dither.comp.spv", 3},          // input + LUT + flow (lock)
    {"fx_transform.comp.spv", 1},
    {"fx_frame_delay.comp.spv", 2},     // input + ring slice
    {"fx_text.comp.spv", 2},            // input + string SDF raster
    {"fx_white_balance.comp.spv", 1},
    {"fx_sharpen.comp.spv", 1},
    {"fx_corner_pin.comp.spv", 1},
};
static_assert(sizeof(kFxShaders) / sizeof(kFxShaders[0]) ==
              static_cast<size_t>(doc::EffectType::Count));

float half_to_float(uint16_t h) {
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1F;
    uint32_t man = h & 0x3FF;
    uint32_t bits;
    if (exp == 0) {
        if (man == 0) {
            bits = sign;
        } else {
            exp = 127 - 15 + 1;
            while (!(man & 0x400)) {
                man <<= 1;
                --exp;
            }
            man &= 0x3FF;
            bits = sign | (exp << 23) | (man << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (man << 13);
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (man << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// Maps a Codec-Box effect's params onto the mosh codec (same
// box, different params = the four named effects).
codec::MoshParams mosh_params(const doc::EffectInstance& fx, uint64_t seed) {
    codec::MoshParams mp;
    mp.seed = seed;
    const auto& p = fx.params;
    switch (fx.type) {
        case doc::EffectType::Datamosh:
            mp.quality = static_cast<int>(p[0]);
            mp.gop_length = static_cast<int>(p[1]);
            mp.mv_scale = p[2];
            mp.mv_random = p[3];
            mp.residual_corrupt = p[4];
            mp.p_repeat = static_cast<int>(p[5]);
            mp.mv_rotate = p[6] * 0.01745329252f;
            mp.byte_flips = static_cast<uint32_t>(p[7]);
            mp.mv_field = static_cast<int>(p[8] + 0.5f);
            mp.mv_field_amount = p[9];
            mp.drop_iframes = p[10] > 0.5f;
            break;
        case doc::EffectType::GenerationLoss:
            mp.quality = static_cast<int>(p[0]);
            mp.generations = static_cast<int>(p[1]);
            mp.gop_length = 1;
            break;
        case doc::EffectType::BitrateStarve:
            mp.quality = 90;
            mp.bitrate_budget = static_cast<uint32_t>(p[0]) * 1024;
            mp.gop_length = static_cast<int>(p[1]);
            break;
        default:
            break;
    }
    return mp;
}

}  // namespace

std::unique_ptr<Engine> Engine::create(Device& device,
                                       const std::filesystem::path& shader_dir) {
    auto e = std::unique_ptr<Engine>(new Engine(device));
    if (!e->init(shader_dir)) return nullptr;
    return e;
}

Engine::~Engine() {
    device_.wait_idle();
    VkDevice dev = device_.device();
    if (linear_sampler_) vkDestroySampler(dev, linear_sampler_, nullptr);
    if (codec_io_.fence) vkDestroyFence(dev, codec_io_.fence, nullptr);
    if (codec_io_.pool) vkDestroyCommandPool(dev, codec_io_.pool, nullptr);
    if (codec_io_.readback)
        vmaDestroyBuffer(device_.allocator(), codec_io_.readback,
                         codec_io_.readback_alloc);
    for (CacheIo& io : cache_io_)
        if (io.buf) vmaDestroyBuffer(device_.allocator(), io.buf, io.alloc);
}

bool Engine::ensure_cache_io(CacheIo& io, size_t bytes) {
    if (io.capacity >= bytes) return true;
    // The caller has waited this slot's fence, so its previous submission
    // no longer touches the old buffer.
    if (io.buf) vmaDestroyBuffer(device_.allocator(), io.buf, io.alloc);
    io.buf = VK_NULL_HANDLE;
    io.alloc = nullptr;
    io.mapped = nullptr;
    io.capacity = 0;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = bytes;
    info.usage =
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                       VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo mapped{};
    if (vmaCreateBuffer(device_.allocator(), &info, &alloc_info, &io.buf,
                        &io.alloc, &mapped) != VK_SUCCESS) {
        io.buf = VK_NULL_HANDLE;
        return false;
    }
    io.mapped = mapped.pMappedData;
    io.capacity = bytes;
    return true;
}

bool Engine::init(const std::filesystem::path& shader_dir) {
    for (auto& s : staging_)
        s = std::make_unique<StagingBuffer>(device_);

    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.anisotropyEnable = VK_FALSE;
    sampler_info.maxAnisotropy = 1.0f;
    vk_check(vkCreateSampler(device_.device(), &sampler_info, nullptr,
                             &linear_sampler_),
             "vkCreateSampler(engine)");

    ComputePipelineDesc to_rgb_desc;
    to_rgb_desc.spv_name = "ycbcr_to_rgb.comp.spv";
    to_rgb_desc.sampled_inputs = 3;
    to_rgb_desc.storage_outputs = 1;
    to_rgb_desc.push_bytes = 2 * sizeof(uint32_t);
    to_rgb_ = ComputePipeline::create(device_, shader_dir, to_rgb_desc);
    if (!to_rgb_) return false;

    for (size_t i = 0; i < static_cast<size_t>(doc::EffectType::Count); ++i) {
        if (!kFxShaders[i].spv_name) continue;   // multi-pass, below
        const doc::EffectInfo& info =
            doc::effect_info(static_cast<doc::EffectType>(i));
        ComputePipelineDesc desc;
        desc.spv_name = kFxShaders[i].spv_name;
        desc.sampled_inputs = kFxShaders[i].sampled_inputs;
        desc.storage_outputs = 1;
        // Slit-scan, time-displace and security-mux append one extra push
        // word (the history-pass index), velocity-scan one (front-state
        // width); glyph appends four (atlas grid cols/rows + tile px +
        // color flag, custom glyph sets); text appends four
        // (glyph count + MSDF px range + string width + atlas em px).
        const auto type_i = static_cast<doc::EffectType>(i);
        const uint32_t extra =
            (type_i == doc::EffectType::SlitScan ||
             type_i == doc::EffectType::TimeDisplace ||
             type_i == doc::EffectType::SecurityMux ||
             type_i == doc::EffectType::VelocityScan)
                ? 1u
                : (type_i == doc::EffectType::Glyph ||
                           type_i == doc::EffectType::Text
                       ? 4u
                       : 0u);
        desc.push_bytes = static_cast<uint32_t>(
            (kFxPreludeWords + info.param_count + extra) * sizeof(uint32_t));
        fx_[i] = ComputePipeline::create(device_, shader_dir, desc);
        if (!fx_[i]) return false;
    }

    ComputePipelineDesc flow_desc;
    flow_desc.spv_name = "flow.comp.spv";
    flow_desc.sampled_inputs = 2;
    flow_desc.storage_outputs = 1;
    flow_desc.push_bytes = 3 * sizeof(uint32_t);
    flow_ = ComputePipeline::create(device_, shader_dir, flow_desc);
    if (!flow_) return false;

    // Velocity-scan front stepper (dwell-time rendering): advances the
    // per-line sweep fronts against the current frame's luma.
    ComputePipelineDesc vsf_desc;
    vsf_desc.spv_name = "vs_front.comp.spv";
    vsf_desc.sampled_inputs = 2;   // front state + video frame
    vsf_desc.storage_outputs = 1;
    vsf_desc.push_bytes = 12 * sizeof(uint32_t);
    vs_front_ = ComputePipeline::create(device_, shader_dir, vsf_desc);
    if (!vs_front_) return false;
    if (!flow_) return false;

    // Modulation phase integrator: one thread per lane, marching across
    // the frame accumulating the FM phase (numthreads(8,1,1) — dispatch
    // as (lanes, 1)).
    ComputePipelineDesc mi_desc;
    mi_desc.spv_name = "mod_integrate.comp.spv";
    mi_desc.sampled_inputs = 1;
    mi_desc.storage_outputs = 1;
    mi_desc.push_bytes = 7 * sizeof(uint32_t);
    mod_integrate_ = ComputePipeline::create(device_, shader_dir, mi_desc);
    if (!mod_integrate_) return false;

    // Node-canvas thumbnail tap (docs/flow_canvas.md): one small
    // downsample dispatch per evaluated graph node into a fixed atlas.
    ComputePipelineDesc tt_desc;
    tt_desc.spv_name = "thumb_tap.comp.spv";
    tt_desc.sampled_inputs = 1;
    tt_desc.storage_outputs = 1;
    tt_desc.push_bytes = 2 * sizeof(uint32_t);
    thumb_tap_ = ComputePipeline::create(device_, shader_dir, tt_desc);
    if (!thumb_tap_) return false;

    ComputePipelineDesc rd_desc;
    rd_desc.spv_name = "rd_step.comp.spv";
    rd_desc.sampled_inputs = 2;
    rd_desc.storage_outputs = 1;
    rd_desc.push_bytes = 5 * sizeof(uint32_t);
    rd_step_ = ComputePipeline::create(device_, shader_dir, rd_desc);
    if (!rd_step_) return false;

    // Codec-Box roundtrip: NV12 conversion (shared with export) + generic
    // wet/opacity composite for the CPU-processed result.
    ComputePipelineDesc nv12_desc;
    nv12_desc.spv_name = "export_nv12.comp.spv";
    nv12_desc.sampled_inputs = 1;
    nv12_desc.storage_outputs = 2;
    nv12_desc.push_bytes = 2 * sizeof(uint32_t);
    to_nv12_ = ComputePipeline::create(device_, shader_dir, nv12_desc);

    ComputePipelineDesc mix_desc;
    mix_desc.spv_name = "fx_mix.comp.spv";
    mix_desc.sampled_inputs = 2;
    mix_desc.storage_outputs = 1;
    mix_desc.push_bytes = 4 * sizeof(uint32_t);
    fx_mix_ = ComputePipeline::create(device_, shader_dir, mix_desc);
    if (!to_nv12_ || !fx_mix_) return false;

    // Compositing: layer generators + blend.
    ComputePipelineDesc gen_desc;
    gen_desc.spv_name = "gen.comp.spv";
    gen_desc.sampled_inputs = 0;
    gen_desc.storage_outputs = 1;
    gen_desc.push_bytes = 14 * sizeof(uint32_t);
    generator_ = ComputePipeline::create(device_, shader_dir, gen_desc);

    ComputePipelineDesc blend_desc;
    blend_desc.spv_name = "layer_blend.comp.spv";
    blend_desc.sampled_inputs = 2;
    blend_desc.storage_outputs = 1;
    blend_desc.push_bytes = 12 * sizeof(uint32_t);
    layer_blend_ = ComputePipeline::create(device_, shader_dir, blend_desc);

    ComputePipelineDesc xf_desc;
    xf_desc.spv_name = "layer_transform.comp.spv";
    xf_desc.sampled_inputs = 1;
    xf_desc.storage_outputs = 1;
    xf_desc.push_bytes = 9 * sizeof(uint32_t);
    layer_transform_ = ComputePipeline::create(device_, shader_dir, xf_desc);
    if (!generator_ || !layer_blend_ || !layer_transform_) return false;

    codec_io_.staging = std::make_unique<StagingBuffer>(device_);
    VkCommandPoolCreateInfo cpool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpool.queueFamilyIndex = device_.graphics_family();
    vk_check(vkCreateCommandPool(device_.device(), &cpool, nullptr,
                                 &codec_io_.pool),
             "vkCreateCommandPool(codecbox)");
    VkCommandBufferAllocateInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cb.commandPool = codec_io_.pool;
    cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb.commandBufferCount = 1;
    vk_check(vkAllocateCommandBuffers(device_.device(), &cb, &codec_io_.cmd),
             "vkAllocateCommandBuffers(codecbox)");
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vk_check(vkCreateFence(device_.device(), &fence_info, nullptr,
                           &codec_io_.fence),
             "vkCreateFence(codecbox)");

    // Procedural halftone atlas (dots are glyphs): 96 tiles of
    // 8x8, dot area grows with the tile index.
    {
        constexpr uint32_t kAtlasW = 128, kAtlasH = 48;
        std::vector<uint8_t> halftone(kAtlasW * kAtlasH, 0);
        for (uint32_t tile = 0; tile < 96; ++tile) {
            const uint32_t tx = (tile % 16) * 8;
            const uint32_t ty = (tile / 16) * 8;
            const float coverage = static_cast<float>(tile) / 95.0f;
            const float radius = std::sqrt(coverage) * 5.4f;
            for (uint32_t y = 0; y < 8; ++y) {
                for (uint32_t x = 0; x < 8; ++x) {
                    const float dx = static_cast<float>(x) - 3.5f;
                    const float dy = static_cast<float>(y) - 3.5f;
                    const float d = std::sqrt(dx * dx + dy * dy);
                    const float v =
                        std::clamp(radius - d + 0.5f, 0.0f, 1.0f);
                    halftone[(ty + y) * kAtlasW + tx + x] =
                        static_cast<uint8_t>(v * 255.0f + 0.5f);
                }
            }
        }
        glyph_atlas_[0] = GpuImage::create(
            device_, VK_FORMAT_R8_UNORM, kAtlasW, kAtlasH,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (!glyph_atlas_[0] ||
            !upload_gray_oneshot(*glyph_atlas_[0], halftone.data(), kAtlasW,
                                 kAtlasH))
            return false;
    }

    // Procedural braille atlas: 2x4 dot cells on the same
    // 96-tile grid, lit-dot count rising with the tile index; which dots
    // light is a stable per-tile hash so the ramp reads organically.
    {
        constexpr uint32_t kAtlasW = 128, kAtlasH = 48;
        std::vector<uint8_t> braille(kAtlasW * kAtlasH, 0);
        for (uint32_t tile = 0; tile < 96; ++tile) {
            const uint32_t tx = (tile % 16) * 8;
            const uint32_t ty = (tile / 16) * 8;
            const uint32_t lit =
                (tile * 8 + 47) / 95;   // 0..8 dots, rounded ramp
            // Stable dot pick: knuth-hash the tile, take `lit` of the 8
            // dot slots in a hash-shuffled order.
            uint32_t hash = tile * 2654435761u + 0x9E3779B9u;
            uint8_t order[8] = {0, 1, 2, 3, 4, 5, 6, 7};
            for (int i = 7; i > 0; --i) {
                hash ^= hash << 13;
                hash ^= hash >> 17;
                hash ^= hash << 5;
                const int j = static_cast<int>(hash % (i + 1));
                const uint8_t tmpv = order[i];
                order[i] = order[j];
                order[j] = tmpv;
            }
            for (uint32_t d = 0; d < lit && d < 8; ++d) {
                const uint32_t slot = order[d];
                const uint32_t dx = tx + 2 + (slot & 1u) * 3;   // cols 2/5
                const uint32_t dy = ty + (slot >> 1) * 2;       // rows 0/2/4/6
                // 2x2 dot with a soft corner.
                braille[dy * kAtlasW + dx] = 255;
                braille[dy * kAtlasW + dx + 1] = 255;
                braille[(dy + 1) * kAtlasW + dx] = 255;
                braille[(dy + 1) * kAtlasW + dx + 1] = 200;
            }
        }
        glyph_atlas_[3] = GpuImage::create(
            device_, VK_FORMAT_R8_UNORM, kAtlasW, kAtlasH,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (!glyph_atlas_[3] ||
            !upload_gray_oneshot(*glyph_atlas_[3], braille.data(), kAtlasW,
                                 kAtlasH))
            return false;
    }

    // Procedural teletext atlas: 2x3 block-mosaic sextant
    // cells — filled rectangles, not dots — lit-block count rising with
    // the tile index, hash-shuffled per tile like the braille ramp.
    {
        constexpr uint32_t kAtlasW = 128, kAtlasH = 48;
        std::vector<uint8_t> ttx(kAtlasW * kAtlasH, 0);
        for (uint32_t tile = 0; tile < 96; ++tile) {
            const uint32_t tx = (tile % 16) * 8;
            const uint32_t ty = (tile / 16) * 8;
            const uint32_t lit = (tile * 6 + 47) / 95;   // 0..6 blocks
            uint32_t hash = tile * 2246822519u + 0x9E3779B9u;
            uint8_t order[6] = {0, 1, 2, 3, 4, 5};
            for (int i = 5; i > 0; --i) {
                hash ^= hash << 13;
                hash ^= hash >> 17;
                hash ^= hash << 5;
                const int j = static_cast<int>(hash % (i + 1));
                const uint8_t tmpv = order[i];
                order[i] = order[j];
                order[j] = tmpv;
            }
            // Sextant grid inside the 8x8 tile: cols [0,4)/[4,8),
            // rows [0,3)/[3,6)/[6,8) — full-bleed mosaic blocks.
            for (uint32_t b = 0; b < lit && b < 6; ++b) {
                const uint32_t slot = order[b];
                const uint32_t bx = (slot & 1u) * 4;
                const uint32_t row = slot >> 1;
                const uint32_t by = row * 3;
                const uint32_t bh = row == 2 ? 2 : 3;
                for (uint32_t y = 0; y < bh; ++y)
                    for (uint32_t x = 0; x < 4; ++x)
                        ttx[(ty + by + y) * kAtlasW + tx + bx + x] = 255;
            }
        }
        glyph_atlas_[4] = GpuImage::create(
            device_, VK_FORMAT_R8_UNORM, kAtlasW, kAtlasH,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (!glyph_atlas_[4] ||
            !upload_gray_oneshot(*glyph_atlas_[4], ttx.data(), kAtlasW,
                                 kAtlasH))
            return false;
    }

    // Dust/damage plate (texture-driven dust):
    // a user-droppable assets/textures/dust.png (dark marks = damage);
    // missing file synthesizes a procedural grunge plate so the binding
    // always exists.
    {
        constexpr uint32_t kDustW = 256, kDustH = 256;
        std::vector<uint8_t> plate;
        uint32_t pw = kDustW, ph = kDustH;
        ImageRgba img;
        const std::filesystem::path dust_path =
            shader_dir.parent_path() / "assets" / "textures" / "dust.png";
        if (load_image(dust_path, &img) && img.width && img.height) {
            pw = img.width;
            ph = img.height;
            plate.resize(static_cast<size_t>(pw) * ph);
            for (size_t i = 0; i < plate.size(); ++i) {
                const uint8_t* p = img.pixels.data() + i * 4;
                plate[i] = static_cast<uint8_t>(
                    (p[0] * 54u + p[1] * 183u + p[2] * 19u) >> 8);
            }
        } else {
            // Grunge fallback: mostly-white plate with hashed blotch
            // clusters and a few long fibers.
            plate.assign(static_cast<size_t>(kDustW) * kDustH, 255);
            uint32_t h32 = 0x9E3779B9u;
            auto next = [&h32] {
                h32 ^= h32 << 13;
                h32 ^= h32 >> 17;
                h32 ^= h32 << 5;
                return h32;
            };
            for (int blob = 0; blob < 90; ++blob) {
                const uint32_t bx = next() % kDustW;
                const uint32_t by = next() % kDustH;
                const int r = 1 + static_cast<int>(next() % 5);
                const uint8_t shade =
                    static_cast<uint8_t>(20 + next() % 90);
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx) {
                        if (dx * dx + dy * dy > r * r) continue;
                        const uint32_t x = (bx + dx + kDustW) % kDustW;
                        const uint32_t y = (by + dy + kDustH) % kDustH;
                        plate[y * kDustW + x] =
                            std::min(plate[y * kDustW + x], shade);
                    }
            }
            for (int fiber = 0; fiber < 6; ++fiber) {
                float fx_pos = static_cast<float>(next() % kDustW);
                float fy_pos = static_cast<float>(next() % kDustH);
                float ang = static_cast<float>(next() % 628) * 0.01f;
                for (int s = 0; s < 120; ++s) {
                    ang += (static_cast<float>(next() % 200) - 100.0f) *
                           0.002f;
                    fx_pos += std::cos(ang);
                    fy_pos += std::sin(ang);
                    const uint32_t x =
                        static_cast<uint32_t>(fx_pos + kDustW * 4) % kDustW;
                    const uint32_t y =
                        static_cast<uint32_t>(fy_pos + kDustH * 4) % kDustH;
                    plate[y * kDustW + x] = 30;
                }
            }
        }
        dust_tex_ = GpuImage::create(
            device_, VK_FORMAT_R8_UNORM, pw, ph,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (!dust_tex_ ||
            !upload_gray_oneshot(*dust_tex_, plate.data(), pw, ph))
            return false;
    }

    // Dither LUT: STBN + blue noise generated by the build-time
    // tool, staged under assets/noise next to the exe. Missing file falls
    // back to deterministic hash noise so headless runs never hard-fail.
    {
        constexpr uint32_t kLutW = 512, kLutH = 192;
        std::vector<uint8_t> lut(static_cast<size_t>(kLutW) * kLutH);
        bool loaded = false;
        const std::filesystem::path lut_path =
            shader_dir.parent_path() / "assets" / "noise" / "dither_lut.bin";
        if (auto bytes = read_file_bytes(lut_path)) {
            if (bytes->size() == 12 + lut.size() && (*bytes)[0] == 'D' &&
                (*bytes)[1] == 'L' && (*bytes)[2] == 'T') {
                std::memcpy(lut.data(), bytes->data() + 12, lut.size());
                loaded = true;
            }
        }
        if (!loaded) {
            log_warn("engine: dither LUT missing (%s); using hash fallback",
                     lut_path.string().c_str());
            for (size_t i = 0; i < lut.size(); ++i)
                lut[i] = static_cast<uint8_t>(
                    hash_combine(0xD17E4u, i) & 0xFFu);
        }
        noise_lut_ = GpuImage::create(
            device_, VK_FORMAT_R8_UNORM, kLutW, kLutH,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (!noise_lut_ ||
            !upload_gray_oneshot(*noise_lut_, lut.data(), kLutW, kLutH))
            return false;
    }

    // Text-overlay fonts (docs/flow_canvas.md): every .ttf under
    // assets/fonts, parsed by the in-repo TrueType loader — drop a font
    // next to the shipped ones and it's index N, no bake step. Sorted by
    // lowercased filename so the `font` param stays deterministic;
    // unparseable files (CFF-flavored renames) are skipped. An empty
    // list just leaves the effect dormant.
    {
        const std::filesystem::path fonts_dir =
            shader_dir.parent_path() / "assets" / "fonts";
        std::vector<std::filesystem::path> ttfs;
        std::error_code ec;
        std::filesystem::directory_iterator it(fonts_dir, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            std::string ext = it->path().extension().string();
            for (char& c : ext)
                c = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c)));
            if (ext == ".ttf") ttfs.push_back(it->path());
        }
        std::sort(ttfs.begin(), ttfs.end(),
                  [](const std::filesystem::path& a,
                     const std::filesystem::path& b) {
                      std::string an = a.filename().string();
                      std::string bn = b.filename().string();
                      for (char& c : an)
                          c = static_cast<char>(
                              std::tolower(static_cast<unsigned char>(c)));
                      for (char& c : bn)
                          c = static_cast<char>(
                              std::tolower(static_cast<unsigned char>(c)));
                      return an < bn;
                  });
        for (const std::filesystem::path& p : ttfs) {
            if (fx_fonts_.size() >= 8) break;
            if (auto f = ui::TtfFont::load(p))
                fx_fonts_.push_back(std::move(*f));
        }
    }

    // Glow's four passes share the standard push layout (5 params — the
    // composite pass reads the ccd-smear knob).
    {
        const uint32_t glow_push =
            (kFxPreludeWords + 5) * static_cast<uint32_t>(sizeof(uint32_t));
        const char* names[4] = {"fx_glow_bright.comp.spv",
                                "fx_glow_blur_h.comp.spv",
                                "fx_glow_blur_v.comp.spv",
                                "fx_glow_composite.comp.spv"};
        for (int p = 0; p < 4; ++p) {
            ComputePipelineDesc desc;
            desc.spv_name = names[p];
            desc.sampled_inputs = p == 3 ? 2u : 1u;
            desc.storage_outputs = 1;
            desc.push_bytes = glow_push;
            glow_pass_[p] = ComputePipeline::create(device_, shader_dir, desc);
            if (!glow_pass_[p]) return false;
        }
    }

    // Matte pair: port-1 wires gate through luma extract + apply.
    ComputePipelineDesc extract_desc;
    extract_desc.spv_name = "matte_extract.comp.spv";
    extract_desc.sampled_inputs = 1;
    extract_desc.storage_outputs = 1;
    extract_desc.push_bytes = 2 * sizeof(uint32_t);
    matte_extract_ =
        ComputePipeline::create(device_, shader_dir, extract_desc);

    ComputePipelineDesc apply_desc;
    apply_desc.spv_name = "matte_apply.comp.spv";
    apply_desc.sampled_inputs = 3;
    apply_desc.storage_outputs = 1;
    apply_desc.push_bytes = 2 * sizeof(uint32_t);
    matte_apply_ = ComputePipeline::create(device_, shader_dir, apply_desc);

    return matte_extract_ && matte_apply_;
}

bool Engine::ensure_planes(uint32_t width, uint32_t height) {
    if (plane_y_ && plane_y_->width() == width && plane_y_->height() == height)
        return true;

    // Source size changed (clip switch): the old planes/targets may be in
    // flight — rare enough that a full sync is the simple correct answer.
    device_.wait_idle();
    pool_.clear();
    have_last_frame_ = false;

    const uint32_t cw = (width + 1) / 2;
    const uint32_t ch = (height + 1) / 2;
    const VkImageUsageFlags usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    plane_y_ = GpuImage::create(device_, VK_FORMAT_R8_UNORM, width, height,
                                usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    plane_u_ = GpuImage::create(device_, VK_FORMAT_R8_UNORM, cw, ch, usage);
    plane_v_ = GpuImage::create(device_, VK_FORMAT_R8_UNORM, cw, ch, usage);
    prev_y_ = GpuImage::create(device_, VK_FORMAT_R8_UNORM, width, height,
                               usage);
    return plane_y_ && plane_u_ && plane_v_ && prev_y_;
}

bool Engine::upload_gray_oneshot(GpuImage& dst, const uint8_t* gray,
                                 uint32_t width, uint32_t height) {
    VkCommandBuffer rec = codec_begin_segment();
    codec_io_.staging->reset();
    if (!codec_io_.staging->upload_image(rec, gray,
                                         static_cast<size_t>(width) * height,
                                         width, dst)) {
        vkEndCommandBuffer(rec);
        return false;
    }
    dst.transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    codec_flush_segment();
    return true;
}

bool Engine::upload_rgba_oneshot(GpuImage& dst, const uint8_t* rgba,
                                 uint32_t width, uint32_t height) {
    VkCommandBuffer rec = codec_begin_segment();
    codec_io_.staging->reset();
    if (!codec_io_.staging->upload_image(
            rec, rgba, static_cast<size_t>(width) * height * 4, width,
            dst)) {
        vkEndCommandBuffer(rec);
        return false;
    }
    dst.transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    codec_flush_segment();
    return true;
}

bool Engine::set_glyph_atlas(const uint8_t* gray, uint32_t width,
                             uint32_t height, float tile_px, uint32_t cols,
                             uint32_t rows, int slot) {
    if (slot < 1 || slot > 2 || !cols || !rows || tile_px < 1.0f)
        return false;
    device_.wait_idle();
    glyph_atlas_[slot] = GpuImage::create(
        device_, VK_FORMAT_R8_UNORM, width, height,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!glyph_atlas_[slot]) return false;
    glyph_meta_[slot] = {cols, rows, tile_px};
    glyph_atlas_color_[slot] = false;
    return upload_gray_oneshot(*glyph_atlas_[slot], gray, width, height);
}

bool Engine::set_glyph_atlas_rgba(const uint8_t* rgba, uint32_t width,
                                  uint32_t height, float tile_px,
                                  uint32_t cols, uint32_t rows, int slot) {
    if (slot < 1 || slot > 2 || !cols || !rows || tile_px < 1.0f)
        return false;
    device_.wait_idle();
    glyph_atlas_[slot] = GpuImage::create(
        device_, VK_FORMAT_R8G8B8A8_UNORM, width, height,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!glyph_atlas_[slot]) return false;
    glyph_meta_[slot] = {cols, rows, tile_px};
    glyph_atlas_color_[slot] = true;
    return upload_rgba_oneshot(*glyph_atlas_[slot], rgba, width, height);
}

void Engine::set_scope_audio(std::vector<int16_t> mono,
                             uint32_t sample_rate) {
    scope_audio_ = std::move(mono);
    scope_rate_ = scope_audio_.empty() ? 0 : sample_rate;
}

namespace {

uint16_t float_to_half(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t exp =
        static_cast<int32_t>((x >> 23) & 0xFFu) - 127 + 15;
    const uint32_t man = x & 0x7FFFFFu;
    if (exp <= 0) return static_cast<uint16_t>(sign);            // -> 0
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7BFFu); // clamp
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) |
                                 (man >> 13));
}

float cpu_srgb_oetf(float x) {
    return x <= 0.0031308f ? x * 12.92f
                           : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}

float cpu_srgb_eotf(float x) {
    return x <= 0.04045f ? x / 12.92f
                         : std::pow((x + 0.055f) / 1.055f, 2.4f);
}

}  // namespace

// Hilbert d -> (x, y) on a 2^order square (the standard rotate-and-
// reflect walk). The curve visits 4^order cells; callers skip the ones
// outside the image.
static void hilbert_d2xy(int order, uint64_t d, uint32_t* out_x,
                         uint32_t* out_y) {
    uint32_t x = 0, y = 0;
    for (int s = 0; s < order; ++s) {
        const uint32_t rx = 1u & static_cast<uint32_t>(d >> 1);
        const uint32_t ry = 1u & static_cast<uint32_t>(d ^ rx);
        if (ry == 0) {
            if (rx == 1) {
                x = (1u << s) - 1u - x;
                y = (1u << s) - 1u - y;
            }
            const uint32_t t = x;
            x = y;
            y = t;
        }
        x += rx << s;
        y += ry << s;
        d >>= 2;
    }
    *out_x = x;
    *out_y = y;
}

// Error diffusion in LINEAR light: the error is conserved in the
// domain the display averages in, so dithered fields keep the source
// brightness — encoded-domain diffusion lifts every dark region (Jensen).
// The output palette stays the encoded-uniform levels k/steps (perceptual
// spacing); the level pick is nearest-in-encoded via exact linear
// thresholds, no per-pixel transfer function needed. Temporal carry feeds
// each pixel's quantization error into the next frame's starting values —
// low = smooth, high = ghosting.
// Kernels 0-5 are the fixed-tap serpentine rasters. Kernel 6 is
// Ostromoukhov's variable-coefficient diffusion: three taps whose weights
// come from a per-intensity table (indexed by the pixel's encoded input
// level, mirrored above mid-gray). Kernel 7 is Riemersma dither: the
// pixels are visited along a Hilbert curve and each is nudged by the
// exponentially-decaying sum of the last 16 quantization errors, giving
// a wormy structure no raster kernel produces (serpentine does not apply).
void Engine::run_error_diffusion(const uint16_t* halves, uint32_t width,
                                 uint32_t height,
                                 const doc::EffectInstance& fx, EdSlot& slot) {
    const size_t n = static_cast<size_t>(width) * height;
    const float levels = std::clamp(fx.params[0], 2.0f, 16.0f);
    const int kernel =
        static_cast<int>(std::clamp(fx.params[1], 0.0f, 7.0f) + 0.5f);
    const bool serp = fx.params[2] >= 0.5f;
    const float carry_amt = std::clamp(fx.params[3], 0.0f, 1.0f);
    const float steps = levels - 1.0f;

    // Half is 16-bit, so the input clamp is exactly a table — and the
    // working values stay in the linear domain the planes arrive in.
    static const std::vector<float>& lin_lut = [] {
        static std::vector<float> lut(65536);
        for (uint32_t h16 = 0; h16 < 65536; ++h16)
            lut[h16] = std::clamp(
                half_to_float(static_cast<uint16_t>(h16)), 0.0f, 1.0f);
        return lut;
    }();

    // Reused scratch (no 24 MB alloc+zero per frame); the fill and output
    // conversions are per-pixel independent and run across threads — only
    // the diffusion itself is inherently serial.
    slot.work.resize(n * 3);
    float* const v = slot.work.data();
    codec::parallel_blocks(
        static_cast<int>(height), true, [&](int begin, int end) {
            for (int row = begin; row < end; ++row) {
                const size_t base = static_cast<size_t>(row) * width;
                for (size_t x = 0; x < width; ++x)
                    for (size_t c = 0; c < 3; ++c)
                        v[(base + x) * 3 + c] =
                            lin_lut[halves[(base + x) * 4 + c]];
            }
        });
    if (carry_amt > 0.0f && slot.carry.size() == n * 3)
        for (size_t i = 0; i < n * 3; ++i) v[i] += slot.carry[i] * carry_amt;
    std::vector<float> next_carry;
    if (carry_amt > 0.0f) next_carry.assign(n * 3, 0.0f);

    // Tap tables (order matters: float accumulation order defines the
    // result, and the interior fast path must match the checked path).
    struct Tap {
        int dx, dy;
        float wgt;
    };
    static constexpr Tap kFs[4] = {{1, 0, 7.0f / 16.0f},
                                   {-1, 1, 3.0f / 16.0f},
                                   {0, 1, 5.0f / 16.0f},
                                   {1, 1, 1.0f / 16.0f}};
    static constexpr Tap kAtk[6] = {{1, 0, 1.0f / 8.0f}, {2, 0, 1.0f / 8.0f},
                                    {-1, 1, 1.0f / 8.0f}, {0, 1, 1.0f / 8.0f},
                                    {1, 1, 1.0f / 8.0f}, {0, 2, 1.0f / 8.0f}};
    // The classic wide kernels (family, DitherBoy-league).
    static constexpr Tap kJarvis[12] = {
        {1, 0, 7.0f / 48.0f},  {2, 0, 5.0f / 48.0f},  {-2, 1, 3.0f / 48.0f},
        {-1, 1, 5.0f / 48.0f}, {0, 1, 7.0f / 48.0f},  {1, 1, 5.0f / 48.0f},
        {2, 1, 3.0f / 48.0f},  {-2, 2, 1.0f / 48.0f}, {-1, 2, 3.0f / 48.0f},
        {0, 2, 5.0f / 48.0f},  {1, 2, 3.0f / 48.0f},  {2, 2, 1.0f / 48.0f}};
    static constexpr Tap kStucki[12] = {
        {1, 0, 8.0f / 42.0f},  {2, 0, 4.0f / 42.0f},  {-2, 1, 2.0f / 42.0f},
        {-1, 1, 4.0f / 42.0f}, {0, 1, 8.0f / 42.0f},  {1, 1, 4.0f / 42.0f},
        {2, 1, 2.0f / 42.0f},  {-2, 2, 1.0f / 42.0f}, {-1, 2, 2.0f / 42.0f},
        {0, 2, 4.0f / 42.0f},  {1, 2, 2.0f / 42.0f},  {2, 2, 1.0f / 42.0f}};
    static constexpr Tap kBurkes[7] = {
        {1, 0, 8.0f / 32.0f},  {2, 0, 4.0f / 32.0f}, {-2, 1, 2.0f / 32.0f},
        {-1, 1, 4.0f / 32.0f}, {0, 1, 8.0f / 32.0f}, {1, 1, 4.0f / 32.0f},
        {2, 1, 2.0f / 32.0f}};
    static constexpr Tap kSierra[10] = {
        {1, 0, 5.0f / 32.0f},  {2, 0, 3.0f / 32.0f}, {-2, 1, 2.0f / 32.0f},
        {-1, 1, 4.0f / 32.0f}, {0, 1, 5.0f / 32.0f}, {1, 1, 4.0f / 32.0f},
        {2, 1, 3.0f / 32.0f},  {-1, 2, 2.0f / 32.0f}, {0, 2, 3.0f / 32.0f},
        {1, 2, 2.0f / 32.0f}};
    const Tap* taps = kFs;
    int ntaps = 4;
    int margin = 1;
    switch (kernel) {
        case 1: taps = kAtk; ntaps = 6; margin = 2; break;
        case 2: taps = kJarvis; ntaps = 12; margin = 2; break;
        case 3: taps = kStucki; ntaps = 12; margin = 2; break;
        case 4: taps = kBurkes; ntaps = 7; margin = 2; break;
        case 5: taps = kSierra; ntaps = 10; margin = 2; break;
        default: break;
    }

    // Level tables: the encoded-uniform palette in linear light, plus the
    // encoded midpoints as linear thresholds — `count(thresh < value)` IS
    // nearest-in-encoded, exactly, with ≤15 compares and no transfer
    // function in the hot loop. After the pick, v[] holds the level INDEX.
    const int nlevels_q =
        std::min(17, static_cast<int>(std::lround(std::ceil(steps))) + 1);
    float level_lin[17];
    float thresh_lin[16];
    for (int k = 0; k < nlevels_q; ++k)
        level_lin[k] = cpu_srgb_eotf(
            std::clamp(static_cast<float>(k) / steps, 0.0f, 1.0f));
    for (int k = 0; k + 1 < nlevels_q; ++k)
        thresh_lin[k] = cpu_srgb_eotf(std::clamp(
            (static_cast<float>(k) + 0.5f) / steps, 0.0f, 1.0f));

    const int iw = static_cast<int>(width);
    const int ih = static_cast<int>(height);

    if (kernel == 6) {
        // Ostromoukhov variable-coefficient diffusion: three taps (next
        // in the processing direction, down-behind, down) whose weights
        // come from a per-intensity table. Rows cover [0..127] as
        // integer triples normalized by their sum; levels above mirror
        // (row(i) = row(255 - i)).
        static constexpr int16_t kOstro[128][3] = {
            {13, 0, 5},      {13, 0, 5},      {21, 0, 10},
            {7, 0, 4},       {8, 0, 5},       {47, 3, 28},
            {23, 3, 13},     {15, 3, 8},      {22, 6, 11},
            {43, 15, 20},    {7, 3, 3},       {501, 224, 211},
            {249, 116, 103}, {165, 80, 67},   {123, 62, 49},
            {489, 256, 191}, {81, 44, 31},    {483, 272, 181},
            {60, 35, 22},    {53, 32, 19},    {237, 148, 83},
            {471, 304, 161}, {3, 2, 1},       {481, 314, 185},
            {354, 226, 155}, {1389, 866, 685},{227, 138, 125},
            {267, 158, 163}, {327, 188, 220}, {61, 34, 45},
            {627, 338, 505}, {1227, 638, 1075},{20, 10, 19},
            {1937, 1000, 1767},{977, 520, 855},{657, 360, 551},
            {71, 40, 57},    {2005, 1160, 1539},{337, 200, 247},
            {2039, 1240, 1425},{257, 160, 171},{691, 440, 437},
            {1045, 680, 627},{301, 200, 171}, {177, 120, 95},
            {2141, 1480, 1083},{1079, 760, 513},{725, 520, 323},
            {137, 100, 57},  {2209, 1640, 855},{53, 40, 19},
            {2243, 1720, 741},{565, 440, 171},{759, 600, 209},
            {1147, 920, 285},{2311, 1880, 513},{97, 80, 19},
            {335, 280, 57},  {1181, 1000, 171},{793, 680, 95},
            {599, 520, 57},  {2413, 2120, 171},{405, 360, 19},
            {2447, 2200, 57},{11, 10, 0},     {158, 151, 3},
            {178, 179, 7},   {1030, 1091, 63},{248, 277, 21},
            {318, 375, 35},  {458, 571, 63},  {878, 1159, 147},
            {5, 7, 1},       {172, 181, 37},  {97, 76, 22},
            {72, 41, 17},    {119, 47, 29},   {4, 1, 1},
            {4, 1, 1},       {4, 1, 1},       {4, 1, 1},
            {4, 1, 1},       {4, 1, 1},       {4, 1, 1},
            {4, 1, 1},       {4, 1, 1},       {65, 18, 17},
            {95, 29, 26},    {185, 62, 53},   {30, 11, 9},
            {35, 14, 11},    {85, 37, 28},    {55, 26, 19},
            {80, 41, 29},    {155, 86, 59},   {5, 3, 2},
            {5, 3, 2},       {5, 3, 2},       {5, 3, 2},
            {5, 3, 2},       {5, 3, 2},       {5, 3, 2},
            {5, 3, 2},       {5, 3, 2},       {5, 3, 2},
            {5, 3, 2},       {5, 3, 2},       {5, 3, 2},
            {305, 176, 119}, {155, 86, 59},   {105, 56, 39},
            {80, 41, 29},    {65, 32, 23},    {55, 26, 19},
            {335, 152, 113}, {85, 37, 28},    {115, 48, 37},
            {35, 14, 11},    {355, 136, 109}, {30, 11, 9},
            {365, 128, 107}, {185, 62, 53},   {25, 8, 7},
            {95, 29, 26},    {385, 112, 103}, {65, 18, 17},
            {395, 104, 101}, {4, 1, 1},
        };
        float ostro_w[128][3];
        for (int r = 0; r < 128; ++r) {
            const float m = static_cast<float>(
                kOstro[r][0] + kOstro[r][1] + kOstro[r][2]);
            for (int t = 0; t < 3; ++t)
                ostro_w[r][t] = static_cast<float>(kOstro[r][t]) / m;
        }
        // The table's domain is the encoded input level. Nearest encoded
        // byte of a linear value via midpoint thresholds, folded into a
        // 4096-bin lookup: sub-byte exact except the darkest rows, whose
        // coefficients are near-identical anyway.
        uint8_t row_lut[4096];
        {
            int b = 0;
            for (int j = 0; j < 4096; ++j) {
                const float vj = (static_cast<float>(j) + 0.5f) / 4096.0f;
                while (b < 255 &&
                       vj > cpu_srgb_eotf(
                                (static_cast<float>(b) + 0.5f) / 255.0f))
                    ++b;
                row_lut[j] = static_cast<uint8_t>(b < 128 ? b : 255 - b);
            }
        }
        for (int y = 0; y < ih; ++y) {
            const bool reverse = serp && ((y & 1) != 0);
            const int dir = reverse ? -1 : 1;
            const bool has_down = y + 1 < ih;
            for (int xi = 0; xi < iw; ++xi) {
                const int x = reverse ? (iw - 1 - xi) : xi;
                const size_t i = (static_cast<size_t>(y) * width +
                                  static_cast<size_t>(x)) * 3;
                const int xn = x + dir;
                const int xb = x - dir;
                const bool has_next = xn >= 0 && xn < iw;
                const bool has_back = xb >= 0 && xb < iw;
                for (size_t c = 0; c < 3; ++c) {
                    // Weights index off the ORIGINAL input level (the
                    // domain the table was tuned in), not the
                    // error-shifted working value.
                    const float orig =
                        lin_lut[halves[(static_cast<size_t>(y) * width +
                                        static_cast<size_t>(x)) * 4 + c]];
                    const float* wv = ostro_w[row_lut[std::min(
                        4095, static_cast<int>(orig * 4096.0f))]];
                    const float clamped =
                        std::clamp(v[i + c], 0.0f, 1.0f);
                    int k = 0;
                    while (k + 1 < nlevels_q && clamped > thresh_lin[k])
                        ++k;
                    const float err = clamped - level_lin[k];
                    v[i + c] = static_cast<float>(k);
                    if (has_next)
                        v[(static_cast<size_t>(y) * width +
                           static_cast<size_t>(xn)) * 3 + c] += err * wv[0];
                    if (has_down) {
                        if (has_back)
                            v[(static_cast<size_t>(y + 1) * width +
                               static_cast<size_t>(xb)) * 3 + c] +=
                                err * wv[1];
                        v[(static_cast<size_t>(y + 1) * width +
                           static_cast<size_t>(x)) * 3 + c] += err * wv[2];
                    }
                    if (carry_amt > 0.0f) next_carry[i + c] = err;
                }
            }
        }
    } else if (kernel == 7) {
        // Riemersma dither: pixels are visited along the Hilbert curve
        // covering the next power-of-two square (off-image points are
        // skipped); each pixel is nudged by the decaying weighted sum of
        // the last 16 quantization errors (newest weight 1, oldest 1/16)
        // and its own pure residual joins the list. The serpentine flag
        // has no meaning on a space-filling path.
        int order = 0;
        while ((1u << order) < width || (1u << order) < height) ++order;
        float wts[16];
        const float decay = std::exp(std::log(16.0f) / 15.0f);
        for (int a = 0; a < 16; ++a)
            wts[a] = std::pow(decay, -static_cast<float>(a));
        float ring[16][3] = {};
        int head = 0;
        const uint64_t total = uint64_t{1} << (2 * order);
        for (uint64_t d = 0; d < total; ++d) {
            uint32_t hx = 0, hy = 0;
            hilbert_d2xy(order, d, &hx, &hy);
            if (hx >= width || hy >= height) continue;
            const size_t i = (static_cast<size_t>(hy) * width + hx) * 3;
            float errs[3];
            for (size_t c = 0; c < 3; ++c) {
                float sum = 0.0f;
                for (int a = 0; a < 16; ++a)
                    sum += ring[(head + 15 - a) & 15][c] * wts[a];
                const float clamped = std::clamp(v[i + c], 0.0f, 1.0f);
                const float nudged = clamped + sum;
                int k = 0;
                while (k + 1 < nlevels_q && nudged > thresh_lin[k]) ++k;
                errs[c] = clamped - level_lin[k];
                v[i + c] = static_cast<float>(k);
                if (carry_amt > 0.0f) next_carry[i + c] = errs[c];
            }
            for (size_t c = 0; c < 3; ++c) ring[head][c] = errs[c];
            head = (head + 1) & 15;
        }
    } else {
        for (int y = 0; y < ih; ++y) {
            const bool reverse = serp && ((y & 1) != 0);
            const int dir = reverse ? -1 : 1;
            // Flat index deltas for this row direction (interior fast
            // path — no per-tap bounds checks on ~99% of pixels).
            ptrdiff_t delta[12];
            for (int t = 0; t < ntaps; ++t)
                delta[t] = (static_cast<ptrdiff_t>(taps[t].dy) * iw +
                            taps[t].dx * dir) * 3;
            const bool row_interior = y < ih - margin;
            for (int xi = 0; xi < iw; ++xi) {
                const int x = reverse ? (iw - 1 - xi) : xi;
                const size_t i = (static_cast<size_t>(y) * width +
                                  static_cast<size_t>(x)) * 3;
                const bool interior =
                    row_interior && x >= margin && x < iw - margin;
                for (size_t c = 0; c < 3; ++c) {
                    // Quantize and diffuse the CLAMPED-domain error only
                    // — unclamped error cascades row over row (and
                    // explodes through the temporal carry). Linear-light
                    // pick: count thresholds below the value
                    // (nearest-in-encoded, exact), error against the
                    // level's LINEAR value.
                    const float clamped = std::clamp(v[i + c], 0.0f, 1.0f);
                    int k = 0;
                    while (k + 1 < nlevels_q && clamped > thresh_lin[k])
                        ++k;
                    const float err = clamped - level_lin[k];
                    v[i + c] = static_cast<float>(k);
                    if (interior) {
                        for (int t = 0; t < ntaps; ++t)
                            v[static_cast<size_t>(
                                static_cast<ptrdiff_t>(i + c) +
                                delta[t])] += err * taps[t].wgt;
                    } else {
                        for (int t = 0; t < ntaps; ++t) {
                            const int nx = x + taps[t].dx * dir;
                            const int ny = y + taps[t].dy;
                            if (nx < 0 || ny < 0 || nx >= iw || ny >= ih)
                                continue;
                            v[(static_cast<size_t>(ny) * width +
                               static_cast<size_t>(nx)) * 3 + c] +=
                                err * taps[t].wgt;
                        }
                    }
                    if (carry_amt > 0.0f) next_carry[i + c] = err;
                }
            }
        }
    }
    if (carry_amt > 0.0f) slot.carry = std::move(next_carry);
    else slot.carry.clear();

    // v[] holds level indices now; the outbound linear values are a small
    // exact table shared with the pick above.
    uint16_t out_lut[17];
    for (int k = 0; k < nlevels_q; ++k)
        out_lut[k] = float_to_half(level_lin[k]);
    slot.out.resize(n * 4);
    codec::parallel_blocks(
        static_cast<int>(height), true, [&](int begin, int end) {
            for (int row = begin; row < end; ++row) {
                const size_t base = static_cast<size_t>(row) * width;
                for (size_t x = 0; x < width; ++x) {
                    const size_t i = base + x;
                    for (size_t c = 0; c < 3; ++c) {
                        const int k = std::clamp(
                            static_cast<int>(v[i * 3 + c] + 0.5f), 0,
                            nlevels_q - 1);
                        slot.out[i * 4 + c] = out_lut[k];
                    }
                    slot.out[i * 4 + 3] = 0x3C00;   // alpha = 1
                }
            }
        });
}

bool Engine::ensure_codec_io(uint32_t width, uint32_t height) {
    if (codec_io_.nv_y && codec_io_.nv_y->width() == width &&
        codec_io_.nv_y->height() == height)
        return true;
    device_.wait_idle();
    const VkImageUsageFlags conv =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const VkImageUsageFlags up =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const uint32_t cw = width / 2;
    const uint32_t chh = height / 2;
    codec_io_.nv_y =
        GpuImage::create(device_, VK_FORMAT_R8_UNORM, width, height, conv);
    codec_io_.nv_uv =
        GpuImage::create(device_, VK_FORMAT_R8G8_UNORM, cw, chh, conv);
    codec_io_.up_y =
        GpuImage::create(device_, VK_FORMAT_R8_UNORM, width, height, up);
    codec_io_.up_u = GpuImage::create(device_, VK_FORMAT_R8_UNORM, cw, chh, up);
    codec_io_.up_v = GpuImage::create(device_, VK_FORMAT_R8_UNORM, cw, chh, up);
    codec_io_.up_rgba = GpuImage::create(
        device_, VK_FORMAT_R16G16B16A16_SFLOAT, width, height, up);
    if (!codec_io_.nv_y || !codec_io_.nv_uv || !codec_io_.up_y ||
        !codec_io_.up_u || !codec_io_.up_v || !codec_io_.up_rgba)
        return false;

    const size_t nv_bytes = static_cast<size_t>(width) * height * 3 / 2;
    const size_t flow_offset = (nv_bytes + 7) & ~size_t{7};
    const size_t flow_bytes = static_cast<size_t>((width + 15) / 16) *
                              ((height + 15) / 16) * 8;   // RGBA16F texels
    // The error-diffusion path reads back the full RGBA16F frame instead.
    const size_t rgba_bytes = static_cast<size_t>(width) * height * 8;
    const size_t needed = std::max(flow_offset + flow_bytes, rgba_bytes);
    if (codec_io_.capacity < needed) {
        if (codec_io_.readback)
            vmaDestroyBuffer(device_.allocator(), codec_io_.readback,
                             codec_io_.readback_alloc);
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = needed;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo alloc_info{};
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                           VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo mapped{};
        if (vmaCreateBuffer(device_.allocator(), &info, &alloc_info,
                            &codec_io_.readback, &codec_io_.readback_alloc,
                            &mapped) != VK_SUCCESS) {
            codec_io_.readback = VK_NULL_HANDLE;
            codec_io_.capacity = 0;
            return false;
        }
        codec_io_.mapped = mapped.pMappedData;
        codec_io_.capacity = needed;
    }
    return true;
}

VkCommandBuffer Engine::codec_begin_segment() {
    vk_check(vkResetCommandPool(device_.device(), codec_io_.pool, 0),
             "vkResetCommandPool(codecbox)");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(codec_io_.cmd, &begin),
             "vkBeginCommandBuffer(codecbox)");
    return codec_io_.cmd;
}

void Engine::codec_flush_segment() {
    vk_check(vkEndCommandBuffer(codec_io_.cmd), "vkEndCommandBuffer(codecbox)");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &codec_io_.cmd;
    {
        std::lock_guard<std::mutex> lock(device_.queue_mutex());
        vk_check(vkQueueSubmit(device_.graphics_queue(), 1, &submit,
                               codec_io_.fence),
                 "vkQueueSubmit(codecbox)");
    }
    vk_check(vkWaitForFences(device_.device(), 1, &codec_io_.fence, VK_TRUE,
                             UINT64_MAX),
             "vkWaitForFences(codecbox)");
    vk_check(vkResetFences(device_.device(), 1, &codec_io_.fence),
             "vkResetFences(codecbox)");
}

// Node-canvas thumbnail tap (docs/flow_canvas.md): downsample `src` into
// the next free atlas cell, keyed for the UI's cell map. Silently drops
// taps past the fixed grid — 64 previews bound the cost.
void Engine::record_thumb_tap(VkCommandBuffer rec, uint32_t frame_index,
                              GpuImage* src, uint64_t key) {
    if (!thumb_tap_ || !thumb_atlas_ || !src) return;
    if (thumb_next_cell_ >= kThumbGridCols * kThumbGridRows) return;
    const uint32_t cell = thumb_next_cell_++;
    thumb_cells_[key] = cell;
    src->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    thumb_atlas_->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
    const uint32_t push[2] = {cell % kThumbGridCols, cell / kThumbGridCols};
    const GpuImage* sampled[1] = {src};
    GpuImage* storage[1] = {thumb_atlas_.get()};
    thumb_tap_->dispatch(rec, arena_, frame_index, sampled, 1, storage, 1,
                         push, sizeof(push), kThumbCellW, kThumbCellH,
                         linear_sampler_);
}

GpuImage* Engine::render(VkCommandBuffer cmd, uint32_t frame_index,
                         const SourcePlanes& source, const doc::Document& doc,
                         uint32_t timeline_frame, double fps,
                         uint64_t cache_ctx,
                         GpuImage** out_source,
                         const LayerSourceFrame* layer_sources,
                         size_t layer_source_count,
                         uint64_t preview_node) {
    if (out_source) *out_source = nullptr;
    if (!source.y || !source.u || !source.v || source.width == 0 ||
        source.height == 0)
        return nullptr;
    if (!ensure_planes(source.width, source.height)) return nullptr;

    arena_.reset(frame_index);
    pool_.release_all();
    StagingBuffer& staging = *staging_[frame_index % kFramesInFlight];
    staging.reset();

    // Upload dimensions (full-res planes) vs working dimensions (preview
    // proxy, ). Kernels sample the planes by uv, so the working
    // targets shrink cleanly; even dims keep the codec paths happy.
    const uint32_t src_h = source.height;
    const uint32_t ch = (src_h + 1) / 2;
    const uint32_t w =
        std::max((source.width / preview_divisor_) & ~1u, 2u);
    const uint32_t h =
        std::max((source.height / preview_divisor_) & ~1u, 2u);

    // ---- frame render cache. First harvest the readback this
    // slot recorded kFramesInFlight renders ago — the caller has waited the
    // slot's fence, so the copy is complete and the buffer is ours again.
    CacheIo& cio = cache_io_[frame_index % kFramesInFlight];
    if (cio.pending) {
        cio.pending = false;
        const size_t pending_bytes =
            static_cast<size_t>(cio.pending_w) * cio.pending_h * 8;
        if (cio.mapped && pending_bytes <= cio.capacity) {
            vmaInvalidateAllocation(device_.allocator(), cio.alloc, 0,
                                    VK_WHOLE_SIZE);
            const uint16_t* px = static_cast<const uint16_t*>(cio.mapped);
            cache_.insert(
                cio.pending_ctx, cio.pending_frame, cio.pending_w,
                cio.pending_h,
                std::vector<uint16_t>(px, px + pending_bytes / 2));
        }
    }
    bool arm_readback = false;
    if (cache_ctx != 0) {
        cache_.set_context(cache_ctx);
        const RenderCache::Frame* hit = cache_.find(timeline_frame);
        if (hit && hit->width == w && hit->height == h) {
            const size_t bytes = static_cast<size_t>(w) * h * 8;
            GpuImage* dst = nullptr;
            if (ensure_cache_io(cio, bytes) &&
                (dst = pool_.acquire(w, h)) != nullptr) {
                std::memcpy(cio.mapped, hit->halves.data(), bytes);
                vmaFlushAllocation(device_.allocator(), cio.alloc, 0,
                                   VK_WHOLE_SIZE);
                dst->transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkBufferImageCopy copy{};
                copy.bufferRowLength = w;
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageExtent = {w, h, 1};
                vkCmdCopyBufferToImage(cmd, cio.buf, dst->image(),
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       1, &copy);
                dst->transition(cmd,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                // The GPU planes no longer track the playhead; force a
                // prev-luma re-sync (one frame of zero flow, same as a
                // seek) on the next miss.
                have_last_frame_ = false;
                return dst;
            }
        }
        arm_readback = true;
    }

    const RenderGraph graph = compile_graph(doc, preview_node);
    if (!graph.valid) {
        log_error("engine: render graph invalid (cycle?)");
        return nullptr;
    }

    // Codec-Box nodes need the frame on the CPU mid-graph: evaluate in
    // fenced segments on an internal command buffer instead of `cmd`
    // (correctness-first; the roundtrip stalls preview, accepts).
    bool segmented = false;
    for (const GraphNode& n : graph.nodes) {
        if (n.kind != GraphNode::Kind::Effect) continue;
        const doc::EffectInstance& fx =
            doc.layers[static_cast<size_t>(n.layer_index)]
                .stack[static_cast<size_t>(n.effect_index)];
        if (doc::is_codec_box(fx.type) ||
            fx.type == doc::EffectType::ErrorDiffusion) {
            segmented = true;
            break;
        }
    }
    if (segmented && ((w & 1) || (h & 1) || !ensure_codec_io(w, h))) {
        log_error("engine: codec-box setup failed (odd dims?)");
        return nullptr;
    }
    VkCommandBuffer rec = segmented ? codec_begin_segment() : cmd;

    // Preserve last frame's luma for flow/motion BEFORE the new upload.
    // Only when the timeline advanced — a paused re-render keeps the prior
    // prev frame and validity; a seek gap invalidates for one frame.
    if (have_last_frame_ && timeline_frame != last_timeline_frame_) {
        prev_frame_valid_ = timeline_frame == last_timeline_frame_ + 1;
        plane_y_->transition(rec, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        prev_y_->transition(rec, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {source.width, src_h, 1};
        vkCmdCopyImage(rec, plane_y_->image(),
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, prev_y_->image(),
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        prev_y_->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    } else if (!have_last_frame_) {
        prev_frame_valid_ = false;
    }
    last_timeline_frame_ = timeline_frame;
    have_last_frame_ = true;

    if (!staging.upload_image(rec, source.y, source.y_stride * src_h,
                              source.y_stride, *plane_y_) ||
        !staging.upload_image(rec, source.u, source.u_stride * ch,
                              source.u_stride, *plane_u_) ||
        !staging.upload_image(rec, source.v, source.v_stride * ch,
                              source.v_stride, *plane_v_))
        return nullptr;
    // Per-layer trim sources: per-layer I420 uploads.
    for (size_t i = 0; i < layer_source_count; ++i) {
        const LayerSourceFrame& lf = layer_sources[i];
        if (!lf.planes.y || !lf.planes.u || !lf.planes.v ||
            lf.planes.width == 0 || lf.layer_index < 0)
            continue;
        LayerPlanes& lp = layer_planes_[lf.layer_index];
        if (lp.width != lf.planes.width || lp.height != lf.planes.height) {
            device_.wait_idle();
            const VkImageUsageFlags lu =
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            const uint32_t lcw = (lf.planes.width + 1) / 2;
            const uint32_t lch = (lf.planes.height + 1) / 2;
            lp.y = GpuImage::create(device_, VK_FORMAT_R8_UNORM,
                                    lf.planes.width, lf.planes.height, lu);
            lp.u = GpuImage::create(device_, VK_FORMAT_R8_UNORM, lcw, lch, lu);
            lp.v = GpuImage::create(device_, VK_FORMAT_R8_UNORM, lcw, lch, lu);
            if (!lp.y || !lp.u || !lp.v) return nullptr;
            lp.width = lf.planes.width;
            lp.height = lf.planes.height;
        }
        const uint32_t lch = (lp.height + 1) / 2;
        if (!staging.upload_image(rec, lf.planes.y,
                                  lf.planes.y_stride * lp.height,
                                  lf.planes.y_stride, *lp.y) ||
            !staging.upload_image(rec, lf.planes.u,
                                  lf.planes.u_stride * lch,
                                  lf.planes.u_stride, *lp.u) ||
            !staging.upload_image(rec, lf.planes.v,
                                  lf.planes.v_stride * lch,
                                  lf.planes.v_stride, *lp.v))
            return nullptr;
        lp.y->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        lp.u->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        lp.v->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    plane_y_->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    plane_u_->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    plane_v_->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // Seek gap / first frame: neutral prev = current (zero difference).
    GpuImage* prev_plane = prev_frame_valid_ ? prev_y_.get() : plane_y_.get();

    // Audio Scope strips: one 1-D min/max waveform texture per active
    // instance, sliced from the mono PCM copy for the trailing window
    // ending at this timeline frame (deterministic on frame index).
    {
        auto upload_strip = [&](const doc::EffectInstance& fx) -> bool {
            if (fx.type != doc::EffectType::AudioScope || fx.bypass)
                return true;
            std::unique_ptr<GpuImage>& tex = audio_strip_[fx.id];
            if (!tex) {
                tex = GpuImage::create(
                    device_, VK_FORMAT_R32G32_SFLOAT, kAudioStripBins, 1,
                    VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT);
                if (!tex) return false;
            }
            float bins[kAudioStripBins * 2] = {};
            const float window =
                fx.params.empty()
                    ? 0.5f
                    : std::clamp(fx.params[0], 0.05f, 2.0f);
            if (scope_rate_ > 0 && !scope_audio_.empty() && fps > 0.0) {
                const double t1 = timeline_frame / fps;
                const double t0 = t1 - window;
                const int64_t total =
                    static_cast<int64_t>(scope_audio_.size());
                for (uint32_t i = 0; i < kAudioStripBins; ++i) {
                    const double b0 =
                        t0 + window * i / double(kAudioStripBins);
                    const double b1 =
                        t0 + window * (i + 1) / double(kAudioStripBins);
                    int64_t s0 = static_cast<int64_t>(b0 * scope_rate_);
                    int64_t s1 = static_cast<int64_t>(b1 * scope_rate_);
                    if (s1 <= s0) s1 = s0 + 1;
                    float mn = 0.0f, mx = 0.0f;
                    bool any = false;
                    for (int64_t s = s0; s < s1; ++s) {
                        if (s < 0 || s >= total) continue;
                        const float v =
                            scope_audio_[static_cast<size_t>(s)] /
                            32768.0f;
                        mn = any ? std::min(mn, v) : v;
                        mx = any ? std::max(mx, v) : v;
                        any = true;
                    }
                    bins[i * 2] = mn;
                    bins[i * 2 + 1] = mx;
                }
            }
            if (!staging.upload_image(rec, bins, sizeof(bins),
                                      kAudioStripBins, *tex))
                return false;
            tex->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            return true;
        };
        for (const doc::Layer& layer : doc.layers)
            for (const doc::EffectInstance& fx : layer.stack)
                if (!upload_strip(fx)) return nullptr;
    }

    // Thumbnail atlas + cell map: reset only when the graph actually
    // evaluates (a render-cache hit above kept the previous, still-valid
    // taps on screen).
    if (thumb_tap_ && !thumb_atlas_)
        thumb_atlas_ = GpuImage::create(
            device_, VK_FORMAT_R8G8B8A8_UNORM, kThumbCellW * kThumbGridCols,
            kThumbCellH * kThumbGridRows,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    thumb_cells_.clear();
    thumb_next_cell_ = 0;

    std::vector<GpuImage*> results(graph.nodes.size(), nullptr);
    std::vector<int> remaining_uses(graph.nodes.size(), 0);
    for (const GraphNode& node : graph.nodes)
        for (int input : node.inputs)
            remaining_uses[static_cast<size_t>(input)]++;
    // The viewport blit is the published node's final consumer — the
    // preview tap when set, the output otherwise; the output survives
    // regardless (the Output card thumb reads it at the end).
    remaining_uses[static_cast<size_t>(graph.output)]++;
    if (graph.preview >= 0)
        remaining_uses[static_cast<size_t>(graph.preview)]++;
    // A/B wipe: keep the converted source alive to the end too.
    if (out_source) remaining_uses[0]++;

    auto as_bits = [](float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        return bits;
    };

    for (int index : graph.order) {
        const GraphNode& node = graph.nodes[static_cast<size_t>(index)];
        for (int input : node.inputs)
            results[static_cast<size_t>(input)]->transition(
                rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        // Flow lives at block resolution; everything else at frame size.
        const bool is_flow = node.kind == GraphNode::Kind::Flow;
        GpuImage* dst = is_flow
            ? pool_.acquire((w + 15) / 16, (h + 15) / 16)
            : pool_.acquire(w, h);
        if (!dst) return nullptr;
        dst->transition(rec, VK_IMAGE_LAYOUT_GENERAL);

        auto input_image = [&](size_t i) {
            return results[static_cast<size_t>(node.inputs[i])];
        };

        switch (node.kind) {
            case GraphNode::Kind::Source: {
                // layer_index >= 0: a private per-layer source (trim).
                // Falls back to the playhead planes when the caller
                // supplied no frame for that layer.
                const GpuImage* planes[3] = {plane_y_.get(), plane_u_.get(),
                                             plane_v_.get()};
                if (node.layer_index >= 0) {
                    if (auto it = layer_planes_.find(node.layer_index);
                        it != layer_planes_.end() && it->second.y) {
                        planes[0] = it->second.y.get();
                        planes[1] = it->second.u.get();
                        planes[2] = it->second.v.get();
                    }
                }
                const uint32_t push[2] = {w, h};
                to_rgb_->dispatch(rec, arena_, frame_index, planes, 3, &dst, 1,
                                  push, sizeof(push), w, h, linear_sampler_);
                break;
            }
            case GraphNode::Kind::LayerTransform: {
                const doc::Layer& layer =
                    doc.layers[static_cast<size_t>(node.layer_index)];
                uint32_t push[9] = {};
                push[0] = w;
                push[1] = h;
                push[2] = as_bits(layer.crop_l);
                push[3] = as_bits(layer.crop_r);
                push[4] = as_bits(layer.crop_t);
                push[5] = as_bits(layer.crop_b);
                push[6] = (layer.flip_h ? 1u : 0u) | (layer.flip_v ? 2u : 0u);
                push[7] = as_bits(layer.xf_scale);
                push[8] = as_bits(layer.xf_rotate * 0.01745329252f);
                const GpuImage* sampled[1] = {input_image(0)};
                layer_transform_->dispatch(rec, arena_, frame_index, sampled,
                                           1, &dst, 1, push, sizeof(push), w,
                                           h, linear_sampler_);
                break;
            }
            case GraphNode::Kind::Generator: {
                uint32_t push[14] = {};
                push[0] = w;
                push[1] = h;
                if (node.layer_index >= 0) {
                    const doc::Layer& layer =
                        doc.layers[static_cast<size_t>(node.layer_index)];
                    push[2] = static_cast<uint32_t>(layer.source);
                    push[3] = static_cast<uint32_t>(
                        hash_combine(doc.master_seed, layer.id));
                    push[4] = timeline_frame;
                    for (int c = 0; c < 3; ++c) {
                        push[5 + c] = as_bits(layer.color_a[c]);
                        push[8 + c] = as_bits(layer.color_b[c]);
                    }
                    push[11] = as_bits(layer.gen_scale);
                    push[12] = as_bits(layer.gen_angle);
                    push[13] = layer.osc_shape;
                } else {
                    // Unwired Output: a solid with zeroed colors —
                    // an empty composite renders black, never the source.
                    push[2] = static_cast<uint32_t>(
                        doc::LayerSourceKind::Solid);
                    push[4] = timeline_frame;
                    push[11] = as_bits(24.0f);
                }
                generator_->dispatch(rec, arena_, frame_index, nullptr, 0,
                                     &dst, 1, push, sizeof(push), w, h,
                                     linear_sampler_);
                break;
            }
            case GraphNode::Kind::LayerBlend: {
                const doc::Layer& layer =
                    doc.layers[static_cast<size_t>(node.layer_index)];
                uint32_t push[12] = {};
                push[0] = w;
                push[1] = h;
                push[2] = static_cast<uint32_t>(layer.blend);
                push[3] = as_bits(layer.opacity);
                // Transform gate: cropped / scaled-down regions
                // reveal the composite below instead of stamping black.
                push[4] = doc::layer_has_transform(layer) ? 1u : 0u;
                push[5] = as_bits(layer.crop_l);
                push[6] = as_bits(layer.crop_r);
                push[7] = as_bits(layer.crop_t);
                push[8] = as_bits(layer.crop_b);
                push[9] = (layer.flip_h ? 1u : 0u) | (layer.flip_v ? 2u : 0u);
                push[10] = as_bits(layer.xf_scale);
                push[11] = as_bits(layer.xf_rotate * 0.01745329252f);
                const GpuImage* sampled[2] = {input_image(0), input_image(1)};
                layer_blend_->dispatch(rec, arena_, frame_index, sampled, 2,
                                       &dst, 1, push, sizeof(push), w, h,
                                       linear_sampler_);
                break;
            }
            case GraphNode::Kind::Effect: {
                const doc::EffectInstance& fx =
                    doc.layers[static_cast<size_t>(node.layer_index)]
                        .stack[static_cast<size_t>(node.effect_index)];

                if (doc::is_codec_box(fx.type)) {
                    // --- Codec-Box: GPU->CPU->GPU roundtrip.
                    GpuImage* in = input_image(0);
                    GpuImage* flow_img =
                        fx.type == doc::EffectType::Datamosh &&
                                node.inputs.size() > 1
                            ? input_image(1)
                            : nullptr;

                    // 1. NV12 conversion + readback, then flush the segment.
                    codec_io_.nv_y->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
                    codec_io_.nv_uv->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
                    {
                        const uint32_t nv_push[2] = {w, h};
                        const GpuImage* sampled[1] = {in};
                        GpuImage* storage[2] = {codec_io_.nv_y.get(),
                                                codec_io_.nv_uv.get()};
                        to_nv12_->dispatch(rec, arena_, frame_index, sampled,
                                           1, storage, 2, nv_push,
                                           sizeof(nv_push), w, h,
                                           linear_sampler_);
                    }
                    codec_io_.nv_y->transition(
                        rec, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                    codec_io_.nv_uv->transition(
                        rec, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                    const size_t nv_bytes =
                        static_cast<size_t>(w) * h * 3 / 2;
                    const size_t flow_offset = (nv_bytes + 7) & ~size_t{7};
                    VkBufferImageCopy copies[2]{};
                    copies[0].bufferOffset = 0;
                    copies[0].bufferRowLength = w;
                    copies[0].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,
                                                  0, 0, 1};
                    copies[0].imageExtent = {w, h, 1};
                    vkCmdCopyImageToBuffer(
                        rec, codec_io_.nv_y->image(),
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        codec_io_.readback, 1, &copies[0]);
                    copies[1].bufferOffset = static_cast<size_t>(w) * h;
                    copies[1].bufferRowLength = w / 2;
                    copies[1].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,
                                                  0, 0, 1};
                    copies[1].imageExtent = {w / 2, h / 2, 1};
                    vkCmdCopyImageToBuffer(
                        rec, codec_io_.nv_uv->image(),
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        codec_io_.readback, 1, &copies[1]);
                    if (flow_img) {
                        flow_img->transition(
                            rec, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                        VkBufferImageCopy fcopy{};
                        fcopy.bufferOffset = flow_offset;
                        fcopy.bufferRowLength = flow_img->width();
                        fcopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,
                                                  0, 0, 1};
                        fcopy.imageExtent = {flow_img->width(),
                                             flow_img->height(), 1};
                        vkCmdCopyImageToBuffer(
                            rec, flow_img->image(),
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            codec_io_.readback, 1, &fcopy);
                        flow_img->transition(
                            rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    }
                    VkMemoryBarrier to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                    to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                    vkCmdPipelineBarrier(rec, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                                         &to_host, 0, nullptr, 0, nullptr);
                    codec_flush_segment();

                    // 2. CPU: mosh with persistent per-instance state. A
                    // paused re-render reuses the cached output so the
                    // decoder does not keep stewing (vs export).
                    MoshSlot& slot = mosh_state_[fx.id];
                    if (!slot.valid || slot.last_frame != timeline_frame) {
                        vmaInvalidateAllocation(device_.allocator(),
                                                codec_io_.readback_alloc, 0,
                                                VK_WHOLE_SIZE);
                        const uint8_t* nv =
                            static_cast<const uint8_t*>(codec_io_.mapped);
                        const uint32_t cw2 = w / 2;
                        const uint32_t ch2 = h / 2;
                        std::vector<uint8_t> uplane(
                            static_cast<size_t>(cw2) * ch2);
                        std::vector<uint8_t> vplane(uplane.size());
                        const uint8_t* uv = nv + static_cast<size_t>(w) * h;
                        for (uint32_t r = 0; r < ch2; ++r) {
                            const uint8_t* row =
                                uv + static_cast<size_t>(r) * w;
                            for (uint32_t c = 0; c < cw2; ++c) {
                                uplane[static_cast<size_t>(r) * cw2 + c] =
                                    row[c * 2];
                                vplane[static_cast<size_t>(r) * cw2 + c] =
                                    row[c * 2 + 1];
                            }
                        }
                        codec::FrameView view{{nv, w},
                                              {uplane.data(), cw2},
                                              {vplane.data(), cw2},
                                              w,
                                              h};
                        std::vector<float> mvx, mvy;
                        codec::MvField field;
                        if (flow_img) {
                            const uint32_t bw = flow_img->width();
                            const uint32_t bh = flow_img->height();
                            mvx.resize(static_cast<size_t>(bw) * bh);
                            mvy.resize(mvx.size());
                            const uint16_t* halves =
                                reinterpret_cast<const uint16_t*>(
                                    nv + flow_offset);
                            for (size_t b = 0; b < mvx.size(); ++b) {
                                mvx[b] = half_to_float(halves[b * 4]);
                                mvy[b] = half_to_float(halves[b * 4 + 1]);
                            }
                            field = {mvx.data(), mvy.data(), bw, bh};
                        }
                        const uint64_t box_seed = hash_combine(
                            hash_combine(doc.master_seed, fx.id), fx.seed);
                        slot.codec.process(view, timeline_frame,
                                           mosh_params(fx, box_seed), field,
                                           slot.last_out);
                        slot.last_frame = timeline_frame;
                        slot.valid = true;
                    }
                    const codec::DecodedFrame& mo = slot.last_out;

                    // 3. Re-upload + back to linear RGB + wet/opacity mix.
                    rec = codec_begin_segment();
                    codec_io_.staging->reset();
                    if (!codec_io_.staging->upload_image(
                            rec, mo.y.data(), mo.y_stride * h, mo.y_stride,
                            *codec_io_.up_y) ||
                        !codec_io_.staging->upload_image(
                            rec, mo.u.data(), mo.uv_stride * (h / 2),
                            mo.uv_stride, *codec_io_.up_u) ||
                        !codec_io_.staging->upload_image(
                            rec, mo.v.data(), mo.uv_stride * (h / 2),
                            mo.uv_stride, *codec_io_.up_v))
                        return nullptr;
                    codec_io_.up_y->transition(
                        rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    codec_io_.up_u->transition(
                        rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    codec_io_.up_v->transition(
                        rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    GpuImage* temp = pool_.acquire(w, h);
                    if (!temp) return nullptr;
                    temp->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
                    {
                        const uint32_t rgb_push[2] = {w, h};
                        const GpuImage* planes3[3] = {codec_io_.up_y.get(),
                                                      codec_io_.up_u.get(),
                                                      codec_io_.up_v.get()};
                        to_rgb_->dispatch(rec, arena_, frame_index, planes3,
                                          3, &temp, 1, rgb_push,
                                          sizeof(rgb_push), w, h,
                                          linear_sampler_);
                    }
                    temp->transition(rec,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    {
                        const uint32_t mix_push[4] = {w, h, as_bits(fx.wet),
                                                      as_bits(fx.opacity)};
                        const GpuImage* sampled2[2] = {in, temp};
                        fx_mix_->dispatch(rec, arena_, frame_index, sampled2,
                                          2, &dst, 1, mix_push,
                                          sizeof(mix_push), w, h,
                                          linear_sampler_);
                    }
                    pool_.release(temp);
                    break;
                }

                if (fx.type == doc::EffectType::ErrorDiffusion) {
                    // 1. GPU->CPU: the RGBA16F input, tight-packed.
                    GpuImage* in_img = const_cast<GpuImage*>(input_image(0));
                    in_img->transition(rec,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                    // The input was written by compute this segment; the
                    // read-only->transfer transition alone does not make
                    // those writes visible to the copy.
                    VkMemoryBarrier ed_pre{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                    ed_pre.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    ed_pre.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    vkCmdPipelineBarrier(
                        rec, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &ed_pre, 0,
                        nullptr, 0, nullptr);
                    VkBufferImageCopy ed_copy{};
                    ed_copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                                0, 1};
                    ed_copy.imageExtent = {w, h, 1};
                    vkCmdCopyImageToBuffer(
                        rec, in_img->image(),
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        codec_io_.readback, 1, &ed_copy);
                    in_img->transition(
                        rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    VkMemoryBarrier ed_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                    ed_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    ed_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                    vkCmdPipelineBarrier(rec, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                                         &ed_host, 0, nullptr, 0, nullptr);
                    codec_flush_segment();

                    // 2. CPU: serpentine diffusion (— inherently
                    // serial). Paused re-renders reuse the cached output so
                    // temporal carry does not re-stew.
                    EdSlot& slot = ed_state_[fx.id];
                    const size_t px_count = static_cast<size_t>(w) * h;
                    if (!slot.valid || slot.last_frame != timeline_frame ||
                        slot.out.size() != px_count * 4) {
                        vmaInvalidateAllocation(device_.allocator(),
                                                codec_io_.readback_alloc, 0,
                                                VK_WHOLE_SIZE);
                        run_error_diffusion(
                            static_cast<const uint16_t*>(codec_io_.mapped),
                            w, h, fx, slot);
                        slot.last_frame = timeline_frame;
                        slot.valid = true;
                    }

                    // 3. Re-upload + wet/opacity composite.
                    rec = codec_begin_segment();
                    codec_io_.staging->reset();
                    if (!codec_io_.staging->upload_image(
                            rec, slot.out.data(), px_count * 8, w,
                            *codec_io_.up_rgba))
                        return nullptr;
                    codec_io_.up_rgba->transition(
                        rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    {
                        const uint32_t mix_push[4] = {w, h, as_bits(fx.wet),
                                                      as_bits(fx.opacity)};
                        const GpuImage* sampled2[2] = {
                            in_img, codec_io_.up_rgba.get()};
                        fx_mix_->dispatch(rec, arena_, frame_index, sampled2,
                                          2, &dst, 1, mix_push,
                                          sizeof(mix_push), w, h,
                                          linear_sampler_);
                    }
                    break;
                }

                // Shared push layout: uint2 size, wet, opacity, seed, frame,
                // fps, params (seeded counter-based randomness).
                const uint64_t seed64 = hash_combine(
                    hash_combine(doc.master_seed, fx.id), fx.seed);
                uint32_t push[kFxPreludeWords + 16] = {};
                push[0] = w;
                push[1] = h;
                push[2] = as_bits(fx.wet);
                push[3] = as_bits(fx.opacity);
                push[4] = static_cast<uint32_t>(seed64);
                push[5] = timeline_frame;
                push[6] = as_bits(static_cast<float>(fps));
                const size_t param_count = fx.params.size();
                std::memcpy(&push[kFxPreludeWords], fx.params.data(),
                            param_count * sizeof(float));
                const uint32_t push_bytes = static_cast<uint32_t>(
                    (kFxPreludeWords + param_count) * sizeof(uint32_t));

                if (fx.type == doc::EffectType::Glow) {
                    ComputePipeline& pipe = *glow_pass_[node.pass_index];
                    if (node.pass_index == 3) {
                        const GpuImage* sampled[2] = {input_image(0),
                                                      input_image(1)};
                        pipe.dispatch(rec, arena_, frame_index, sampled, 2,
                                      &dst, 1, push, push_bytes, w, h,
                                      linear_sampler_);
                    } else {
                        const GpuImage* sampled[1] = {input_image(0)};
                        pipe.dispatch(rec, arena_, frame_index, sampled, 1,
                                      &dst, 1, push, push_bytes, w, h,
                                      linear_sampler_);
                    }
                } else if (fx.type == doc::EffectType::FlowSmear ||
                           fx.type == doc::EffectType::FlowPaint) {
                    const GpuImage* sampled[2] = {input_image(0),
                                                  input_image(1)};
                    fx_[static_cast<size_t>(fx.type)]->dispatch(
                        rec, arena_, frame_index, sampled, 2, &dst, 1, push,
                        push_bytes, w, h, linear_sampler_);
                } else if (fx.type == doc::EffectType::MotionExtract) {
                    const GpuImage* sampled[3] = {input_image(0), prev_plane,
                                                  plane_y_.get()};
                    fx_[static_cast<size_t>(fx.type)]->dispatch(
                        rec, arena_, frame_index, sampled, 3, &dst, 1, push,
                        push_bytes, w, h, linear_sampler_);
                } else if (fx.type == doc::EffectType::Glyph) {
                    // set: 0 halftone, 1 ascii, 2 custom (user-
                    // droppable tilesets), 3 braille, 4 teletext; missing
                    // slots fall back down.
                    int which = std::clamp(
                        static_cast<int>(fx.params[1] + 0.5f), 0, 4);
                    if (which == 4 && !glyph_atlas_[4]) which = 1;
                    if (which == 3 && !glyph_atlas_[3]) which = 1;
                    if (which == 2 && !glyph_atlas_[2]) which = 1;
                    if (which == 1 && !glyph_atlas_[1]) which = 0;
                    const GlyphMeta& gm = glyph_meta_[which];
                    push[kFxPreludeWords + param_count] = gm.cols;
                    push[kFxPreludeWords + param_count + 1] = gm.rows;
                    push[kFxPreludeWords + param_count + 2] =
                        as_bits(gm.tile);
                    push[kFxPreludeWords + param_count + 3] =
                        glyph_atlas_color_[which] ? 1u : 0u;
                    const GpuImage* sampled[2] = {input_image(0),
                                                  glyph_atlas_[which].get()};
                    fx_[static_cast<size_t>(fx.type)]->dispatch(
                        rec, arena_, frame_index, sampled, 2, &dst, 1, push,
                        push_bytes + 4 * sizeof(uint32_t), w, h,
                        linear_sampler_);
                } else if (fx.type == doc::EffectType::Quantize) {
                    // RD-stipple dither (mode 9) reuses the RD
                    // sim: a per-instance Gray-Scott state seeded by the
                    // frame becomes the threshold pattern. Other modes
                    // bind the noise LUT in that slot as a dummy.
                    const int dm = fx.params.size() > 2
                        ? static_cast<int>(fx.params[2] + 0.5f)
                        : 0;
                    const GpuImage* stipple = noise_lut_.get();
                    if (dm == 9) {
                        RdSlot& slot = rd_state_[fx.id];
                        if (slot.state[0] &&
                            (slot.state[0]->width() != w ||
                             slot.state[0]->height() != h)) {
                            slot.state[0].reset();
                            slot.state[1].reset();
                            slot.last_frame = 0xFFFFFFFFu;
                        }
                        if (!slot.state[0]) {
                            for (int s = 0; s < 2; ++s) {
                                slot.state[s] = GpuImage::create(
                                    device_, VK_FORMAT_R16G16B16A16_SFLOAT,
                                    w, h,
                                    VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_STORAGE_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT);
                                if (!slot.state[s]) return nullptr;
                                slot.state[s]->transition(
                                    rec, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                                VkClearColorValue init_ab{{1.0f, 0.0f, 0.0f,
                                                           1.0f}};
                                VkImageSubresourceRange range{
                                    VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                                vkCmdClearColorImage(
                                    rec, slot.state[s]->image(),
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    &init_ab, 1, &range);
                            }
                            slot.cur = 0;
                        }
                        if (slot.last_frame != timeline_frame) {
                            // Soliton-regime constants (dots); moderate
                            // frame injection so isolated dots nucleate
                            // instead of saturating along midtone bands.
                            const uint32_t rd_push[5] = {
                                w, h, as_bits(0.030f), as_bits(0.062f),
                                as_bits(0.2f)};
                            for (uint32_t s = 0; s < 8; ++s) {
                                GpuImage* src = slot.state[slot.cur].get();
                                GpuImage* next =
                                    slot.state[1 - slot.cur].get();
                                src->transition(
                                    rec,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                                next->transition(rec,
                                                 VK_IMAGE_LAYOUT_GENERAL);
                                const GpuImage* rd_in[2] = {src,
                                                            input_image(0)};
                                GpuImage* outs = next;
                                rd_step_->dispatch(rec, arena_, frame_index,
                                                   rd_in, 2, &outs, 1,
                                                   rd_push, sizeof(rd_push),
                                                   w, h, linear_sampler_);
                                slot.cur = 1 - slot.cur;
                            }
                            slot.last_frame = timeline_frame;
                        }
                        GpuImage* st = slot.state[slot.cur].get();
                        st->transition(
                            rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        stipple = st;
                    }
                    // Motion lock: the graph wires the flow
                    // node as a second input when lock is on; the LUT
                    // doubles as an unread dummy otherwise.
                    const GpuImage* flow_tex = node.inputs.size() > 1
                                                   ? input_image(1)
                                                   : noise_lut_.get();
                    const GpuImage* sampled[4] = {input_image(0),
                                                  noise_lut_.get(), stipple,
                                                  flow_tex};
                    fx_[static_cast<size_t>(fx.type)]->dispatch(
                        rec, arena_, frame_index, sampled, 4, &dst, 1, push,
                        push_bytes, w, h, linear_sampler_);
                } else if (fx.type == doc::EffectType::Dither) {
                    // Standalone ordered dither: LUT always rides
                    // as input 1; the flow field joins as input 2 only in
                    // motion-locked mode (the LUT doubles as the dummy).
                    const GpuImage* flow_tex = node.inputs.size() > 1
                                                   ? input_image(1)
                                                   : noise_lut_.get();
                    const GpuImage* sampled[3] = {input_image(0),
                                                  noise_lut_.get(), flow_tex};
                    fx_[static_cast<size_t>(fx.type)]->dispatch(
                        rec, arena_, frame_index, sampled, 3, &dst, 1, push,
                        push_bytes, w, h, linear_sampler_);
                } else if (fx.type == doc::EffectType::FrameDelay) {
                    // Plain N-frame delay: the slit-scan ring
                    // machinery, one slice bound as the second input.
                    SlitSlot& slot = slit_state_[fx.id];
                    if (slot.ring[0] &&
                        (slot.ring[0]->width() != w ||
                         slot.ring[0]->height() != h)) {
                        for (auto& img : slot.ring) img.reset();
                        slot.head = slot.count = 0;
                        slot.last_frame = 0xFFFFFFFFu;
                    }
                    const uint32_t delay = static_cast<uint32_t>(std::clamp(
                        fx.params[0], 0.0f,
                        static_cast<float>(kSlitRing - 1)));
                    GpuImage* in_img =
                        const_cast<GpuImage*>(input_image(0));
                    const GpuImage* past = in_img;
                    if (delay > 0 && slot.count > 0) {
                        const uint32_t age = std::min(delay, slot.count);
                        const uint32_t idx =
                            (slot.head + kSlitRing - age) % kSlitRing;
                        if (slot.ring[idx]) past = slot.ring[idx].get();
                    }
                    const GpuImage* sampled[2] = {in_img, past};
                    fx_[static_cast<size_t>(fx.type)]->dispatch(
                        rec, arena_, frame_index, sampled, 2, &dst, 1, push,
                        push_bytes, w, h, linear_sampler_);
                    // Push the current input once per timeline frame.
                    if (slot.last_frame != timeline_frame) {
                        std::unique_ptr<GpuImage>& target =
                            slot.ring[slot.head];
                        if (!target) {
                            target = GpuImage::create(
                                device_, VK_FORMAT_R16G16B16A16_SFLOAT, w, h,
                                VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT);
                            if (!target) return nullptr;
                        }
                        in_img->transition(
                            rec, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                        target->transition(
                            rec, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                        VkImageCopy copy{};
                        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                               0, 1};
                        copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                               0, 1};
                        copy.extent = {w, h, 1};
                        vkCmdCopyImage(rec, in_img->image(),
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       target->image(),
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       1, &copy);
                        target->transition(
                            rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        in_img->transition(
                            rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        slot.head = (slot.head + 1) % kSlitRing;
                        slot.count = std::min(slot.count + 1, kSlitRing);
                        slot.last_frame = timeline_frame;
                    }
                } else if (fx.type == doc::EffectType::Text) {
                    // Runtime-TTF text: pick the size BUCKET
                    // covering the resolved size param, rasterize the
                    // string's SDF once per (text, font, bucket), and
                    // let the kernel scale — a keyframed/modulated size
                    // walks a bounded raster set instead of
                    // re-rasterizing every frame. Bucketing keys on the
                    // PARAM (1080-reference px), so proxy preview and
                    // export pick identical rasters.
                    const int font_n = static_cast<int>(fx_fonts_.size());
                    const int which =
                        font_n > 0
                            ? std::clamp(
                                  !fx.params.empty()
                                      ? static_cast<int>(fx.params[0] + 0.5f)
                                      : 0,
                                  0, font_n - 1)
                            : -1;
                    int bucket = 0;
                    const float size_ref =
                        fx.params.size() > 1 ? fx.params[1] : 90.0f;
                    while (bucket < 5 && kTextBuckets[bucket] < size_ref)
                        ++bucket;
                    TextRaster* ras = nullptr;
                    if (which >= 0 && !fx.text.empty()) {
                        uint64_t thash = hash_combine(
                            0x7E87ull, static_cast<uint64_t>(which));
                        for (char ch : fx.text)
                            thash = hash_combine(
                                thash, static_cast<uint8_t>(ch));
                        TextSlot& slot = text_state_[fx.id];
                        if (slot.hash != thash) {
                            // Text/font changed: the old rasters may be
                            // in flight — settle before dropping them.
                            bool any = false;
                            for (const TextRaster& tb : slot.buckets)
                                any = any || tb.tex != nullptr;
                            if (any) device_.wait_idle();
                            for (TextRaster& tb : slot.buckets) {
                                tb.tex.reset();
                                tb.w = tb.h = 0;
                            }
                            slot.hash = thash;
                        }
                        TextRaster& tb = slot.buckets[bucket];
                        if (!tb.tex && tb.w == 0) {
                            const float bpx = kTextBuckets[bucket];
                            const float spread =
                                std::max(4.0f, bpx * 0.1f);
                            const ui::TtfFont::Sdf s =
                                fx_fonts_[static_cast<size_t>(which)]
                                    .rasterize(fx.text, bpx, spread);
                            if (s.width && s.height) {
                                tb.tex = GpuImage::create(
                                    device_, VK_FORMAT_R8_UNORM, s.width,
                                    s.height,
                                    VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT);
                                if (!tb.tex) return nullptr;
                                if (!staging.upload_image(
                                        rec, s.pixels.data(),
                                        s.pixels.size(), s.width, *tb.tex))
                                    return nullptr;
                                tb.tex->transition(
                                    rec,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                                tb.w = s.width;
                                tb.h = s.height;
                                tb.spread = s.spread_px;
                            } else {
                                // Nothing drawable (spaces): remember,
                                // don't re-rasterize every frame.
                                tb.w = 1;
                            }
                        }
                        if (tb.tex) ras = &tb;
                    }
                    uint32_t* extra = &push[kFxPreludeWords + param_count];
                    extra[0] = as_bits(
                        ras ? static_cast<float>(ras->w) : 0.0f);
                    extra[1] = as_bits(
                        ras ? static_cast<float>(ras->h) : 0.0f);
                    extra[2] = as_bits(ras ? ras->spread : 1.0f);
                    extra[3] = as_bits(kTextBuckets[bucket]);
                    const GpuImage* sdf_tex =
                        ras ? ras->tex.get() : noise_lut_.get();
                    const GpuImage* sampled[2] = {input_image(0), sdf_tex};
                    fx_[static_cast<size_t>(fx.type)]->dispatch(
                        rec, arena_, frame_index, sampled, 2, &dst, 1, push,
                        push_bytes + 4 * sizeof(uint32_t), w, h,
                        linear_sampler_);
                } else if (fx.type == doc::EffectType::Displace) {
                    // Second input: the wired map/matte as the
                    // displacement map when the graph wired one; otherwise
                    // the input doubles as its own map (self-luma).
                    const GpuImage* map = node.inputs.size() > 1
                                              ? input_image(1)
                                              : input_image(0);
                    const GpuImage* sampled[2] = {input_image(0), map};
                    fx_[static_cast<size_t>(fx.type)]->dispatch(
                        rec, arena_, frame_index, sampled, 2, &dst, 1, push,
                        push_bytes, w, h, linear_sampler_);
                } else if (fx.type == doc::EffectType::DustScratches) {
                    // The damage plate rides as a second input (
                    // texture-driven dust); always present (fallback).
                    const GpuImage* sampled[2] = {input_image(0),
                                                  dust_tex_.get()};
                    fx_[static_cast<size_t>(fx.type)]->dispatch(
                        rec, arena_, frame_index, sampled, 2, &dst, 1, push,
                        push_bytes, w, h, linear_sampler_);
                } else if (fx.type == doc::EffectType::SlitScan) {
                    // Ring of past inputs; one masked dispatch per age band
                    // (bands are disjoint pixels, so no barriers between).
                    SlitSlot& slot = slit_state_[fx.id];
                    if (slot.ring[0] &&
                        (slot.ring[0]->width() != w ||
                         slot.ring[0]->height() != h)) {
                        for (auto& img : slot.ring) img.reset();
                        slot.head = slot.count = 0;
                        slot.last_frame = 0xFFFFFFFFu;
                    }
                    const uint32_t depth = static_cast<uint32_t>(std::clamp(
                        fx.params[1], 2.0f, static_cast<float>(kSlitRing)));
                    GpuImage* in_img =
                        const_cast<GpuImage*>(input_image(0));
                    auto history = [&](uint32_t age) -> const GpuImage* {
                        if (age == 0 || age > slot.count) return in_img;
                        const uint32_t idx =
                            (slot.head + kSlitRing - age) % kSlitRing;
                        return slot.ring[idx] ? slot.ring[idx].get() : in_img;
                    };
                    uint32_t* extra = &push[kFxPreludeWords + param_count];
                    const uint32_t slit_bytes =
                        push_bytes + static_cast<uint32_t>(sizeof(uint32_t));
                    for (uint32_t k = 0; k < depth; ++k) {
                        *extra = k;
                        const GpuImage* sampled[2] = {in_img, history(k)};
                        fx_[static_cast<size_t>(fx.type)]->dispatch(
                            rec, arena_, frame_index, sampled, 2, &dst, 1,
                            push, slit_bytes, w, h, linear_sampler_);
                    }
                    // Push the current input once per timeline frame.
                    if (slot.last_frame != timeline_frame) {
                        std::unique_ptr<GpuImage>& target =
                            slot.ring[slot.head];
                        if (!target) {
                            target = GpuImage::create(
                                device_, VK_FORMAT_R16G16B16A16_SFLOAT, w, h,
                                VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT);
                            if (!target) return nullptr;
                        }
                        in_img->transition(
                            rec, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                        target->transition(
                            rec, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                        VkImageCopy copy{};
                        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                               0, 1};
                        copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                               0, 1};
                        copy.extent = {w, h, 1};
                        vkCmdCopyImage(rec, in_img->image(),
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       target->image(),
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       1, &copy);
                        target->transition(
                            rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        in_img->transition(
                            rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        slot.head = (slot.head + 1) % kSlitRing;
                        slot.count = std::min(slot.count + 1, kSlitRing);
                        slot.last_frame = timeline_frame;
                    }
                } else if (fx.type == doc::EffectType::TimeDisplace) {
                    // Same ring mechanism as SlitScan: one dispatch per
                    // age band; the shader masks pixels to its band from
                    // the delay map (self-luma, or a wired map input).
                    SlitSlot& slot = slit_state_[fx.id];
                    if (slot.ring[0] &&
                        (slot.ring[0]->width() != w ||
                         slot.ring[0]->height() != h)) {
                        for (auto& img : slot.ring) img.reset();
                        slot.head = slot.count = 0;
                        slot.last_frame = 0xFFFFFFFFu;
                    }
                    const uint32_t depth = static_cast<uint32_t>(std::clamp(
                        fx.params[0], 2.0f, static_cast<float>(kSlitRing)));
                    GpuImage* in_img =
                        const_cast<GpuImage*>(input_image(0));
                    const GpuImage* map = node.inputs.size() > 1
                                              ? input_image(1)
                                              : in_img;
                    auto history = [&](uint32_t age) -> const GpuImage* {
                        if (age == 0 || age > slot.count) return in_img;
                        const uint32_t idx =
                            (slot.head + kSlitRing - age) % kSlitRing;
                        return slot.ring[idx] ? slot.ring[idx].get() : in_img;
                    };
                    uint32_t* extra = &push[kFxPreludeWords + param_count];
                    const uint32_t td_bytes =
                        push_bytes + static_cast<uint32_t>(sizeof(uint32_t));
                    for (uint32_t k = 0; k < depth; ++k) {
                        *extra = k;
                        const GpuImage* sampled[3] = {in_img, history(k),
                                                      map};
                        fx_[static_cast<size_t>(fx.type)]->dispatch(
                            rec, arena_, frame_index, sampled, 3, &dst, 1,
                            push, td_bytes, w, h, linear_sampler_);
                    }
                    if (slot.last_frame != timeline_frame) {
                        std::unique_ptr<GpuImage>& target =
                            slot.ring[slot.head];
                        if (!target) {
                            target = GpuImage::create(
                                device_, VK_FORMAT_R16G16B16A16_SFLOAT, w, h,
                                VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT);
                            if (!target) return nullptr;
                        }
                        in_img->transition(
                            rec, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                        target->transition(
                            rec, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                        VkImageCopy tc{};
                        tc.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                             0, 1};
                        tc.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                             0, 1};
                        tc.extent = {w, h, 1};
                        vkCmdCopyImage(rec, in_img->image(),
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       target->image(),
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       1, &tc);
                        target->transition(
                            rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        in_img->transition(
                            rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        slot.head = (slot.head + 1) % kSlitRing;
                        slot.count = std::min(slot.count + 1, kSlitRing);
                        slot.last_frame = timeline_frame;
                    }
                } else if (fx.type == doc::EffectType::FrameHold) {
                    HoldSlot& slot = hold_state_[fx.id];
                    if (slot.held && (slot.held->width() != w ||
                                      slot.held->height() != h)) {
                        slot.held.reset();
                        slot.last_tick = 0xFFFFFFFFu;
                    }
                    GpuImage* in_img =
                        const_cast<GpuImage*>(input_image(0));
                    const float hold_fps =
                        std::clamp(fx.params[0], 1.0f, 60.0f);
                    const uint32_t tick =
                        fps > 0.0 ? static_cast<uint32_t>(
                                        timeline_frame * hold_fps / fps)
                                  : timeline_frame;
                    if (!slot.held) {
                        slot.held = GpuImage::create(
                            device_, VK_FORMAT_R16G16B16A16_SFLOAT, w, h,
                            VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT);
                        if (!slot.held) return nullptr;
                        slot.last_tick = 0xFFFFFFFFu;
                    }
                    if (slot.last_tick != tick) {
                        in_img->transition(
                            rec, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                        slot.held->transition(
                            rec, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                        VkImageCopy hc{};
                        hc.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
                                             1};
                        hc.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
                                             1};
                        hc.extent = {w, h, 1};
                        vkCmdCopyImage(rec, in_img->image(),
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       slot.held->image(),
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       1, &hc);
                        in_img->transition(
                            rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        slot.last_tick = tick;
                    }
                    slot.held->transition(
                        rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    const GpuImage* sampled[2] = {in_img, slot.held.get()};
                    fx_[static_cast<size_t>(fx.type)]->dispatch(
                        rec, arena_, frame_index, sampled, 2, &dst, 1, push,
                        push_bytes, w, h, linear_sampler_);
                } else if (fx.type == doc::EffectType::Stutter) {
                    SlitSlot& slot = slit_state_[fx.id];
                    if (slot.ring[0] &&
                        (slot.ring[0]->width() != w ||
                         slot.ring[0]->height() != h)) {
                        for (auto& img : slot.ring) img.reset();
                        slot.head = slot.count = 0;
                        slot.last_frame = 0xFFFFFFFFu;
                    }
                    GpuImage* in_img =
                        const_cast<GpuImage*>(input_image(0));
                    const uint32_t ring_n = static_cast<uint32_t>(std::clamp(
                        fx.params[0], 2.0f, static_cast<float>(kSlitRing)));
                    const bool armed = fx.params[1] >= 0.5f;
                    const GpuImage* chosen = in_img;
                    if (armed && slot.count > 0) {
                        const uint32_t span = std::min(ring_n, slot.count);
                        const uint32_t step = static_cast<uint32_t>(
                            timeline_frame *
                            std::max(fx.params[2], 0.25f));
                        const uint32_t age = 1u + (step % span);
                        const uint32_t idx =
                            (slot.head + kSlitRing - age) % kSlitRing;
                        if (slot.ring[idx]) chosen = slot.ring[idx].get();
                    }
                    const GpuImage* sampled[2] = {in_img, chosen};
                    fx_[static_cast<size_t>(fx.type)]->dispatch(
                        rec, arena_, frame_index, sampled, 2, &dst, 1, push,
                        push_bytes, w, h, linear_sampler_);
                    // Record while unarmed, once per timeline frame; armed
                    // freezes the ring (that IS the repeat).
                    if (!armed && slot.last_frame != timeline_frame) {
                        std::unique_ptr<GpuImage>& target =
                            slot.ring[slot.head];
                        if (!target) {
                            target = GpuImage::create(
                                device_, VK_FORMAT_R16G16B16A16_SFLOAT, w, h,
                                VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT);
                            if (!target) return nullptr;
                        }
                        in_img->transition(
                            rec, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                        target->transition(
                            rec, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                        VkImageCopy sc{};
                        sc.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
                                             1};
                        sc.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
                                             1};
                        sc.extent = {w, h, 1};
                        vkCmdCopyImage(rec, in_img->image(),
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       target->image(),
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       1, &sc);
                        target->transition(
                            rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        in_img->transition(
                            rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        slot.head = (slot.head + 1) % kSlitRing;
                        slot.count = std::min(slot.count + 1, kSlitRing);
                        slot.last_frame = timeline_frame;
                    }
                } else if (fx.type == doc::EffectType::ReactionDiffusion) {
                    RdSlot& slot = rd_state_[fx.id];
                    if (slot.state[0] &&
                        (slot.state[0]->width() != w ||
                         slot.state[0]->height() != h)) {
                        slot.state[0].reset();
                        slot.state[1].reset();
                        slot.last_frame = 0xFFFFFFFFu;
                    }
                    if (!slot.state[0]) {
                        for (int s = 0; s < 2; ++s) {
                            slot.state[s] = GpuImage::create(
                                device_, VK_FORMAT_R16G16B16A16_SFLOAT, w, h,
                                VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_STORAGE_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT);
                            if (!slot.state[s]) return nullptr;
                            slot.state[s]->transition(
                                rec, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                            // A = 1, B = 0 everywhere: the quiescent state.
                            VkClearColorValue init_ab{{1.0f, 0.0f, 0.0f,
                                                       1.0f}};
                            VkImageSubresourceRange range{
                                VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                            vkCmdClearColorImage(
                                rec, slot.state[s]->image(),
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                &init_ab, 1, &range);
                        }
                        slot.cur = 0;
                    }
                    // Advance the sim once per timeline frame.
                    if (slot.last_frame != timeline_frame) {
                        const uint32_t steps = static_cast<uint32_t>(
                            std::clamp(fx.params[2], 1.0f, 24.0f));
                        const uint32_t rd_push[5] = {
                            w, h, as_bits(fx.params[0]),
                            as_bits(fx.params[1]), as_bits(fx.params[3])};
                        for (uint32_t s = 0; s < steps; ++s) {
                            GpuImage* src = slot.state[slot.cur].get();
                            GpuImage* next = slot.state[1 - slot.cur].get();
                            src->transition(
                                rec,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                            next->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
                            const GpuImage* sampled[2] = {src,
                                                          input_image(0)};
                            GpuImage* outs = next;
                            rd_step_->dispatch(rec, arena_, frame_index,
                                               sampled, 2, &outs, 1, rd_push,
                                               sizeof(rd_push), w, h,
                                               linear_sampler_);
                            slot.cur = 1 - slot.cur;
                        }
                        slot.last_frame = timeline_frame;
                    }
                    GpuImage* st = slot.state[slot.cur].get();
                    st->transition(rec,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    const GpuImage* sampled[2] = {input_image(0), st};
                    fx_[static_cast<size_t>(fx.type)]->dispatch(
                        rec, arena_, frame_index, sampled, 2, &dst, 1, push,
                        push_bytes, w, h, linear_sampler_);
                } else if (fx.type == doc::EffectType::VelocityScan) {
                    // Velocity-modulated scanning (dwell-time rendering):
                    // sweep fronts advance at luma-braked speed (ping-pong
                    // state, stepped once per timeline frame); the render
                    // splats the beams over the phosphor canvas — the
                    // effect's own previous output via the feedback slot.
                    VsSlot& vs = vs_state_[fx.id];
                    const uint32_t state_w = std::max(w, h);
                    if (vs.state[0] && vs.state[0]->width() != state_w) {
                        vs.state[0].reset();
                        vs.state[1].reset();
                        vs.last_frame = 0xFFFFFFFFu;
                    }
                    if (!vs.state[0]) {
                        // Full-float state: positions live in PIXELS (up
                        // to frame extent), and half floats step by a
                        // whole pixel past 1024 — slow luma-braked fronts
                        // would freeze, then pop a pixel at a time.
                        for (int s = 0; s < 2; ++s) {
                            vs.state[s] = GpuImage::create(
                                device_, VK_FORMAT_R32G32B32A32_SFLOAT,
                                state_w, kVsSlots,
                                VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_STORAGE_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT);
                            if (!vs.state[s]) return nullptr;
                            vs.state[s]->transition(
                                rec, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                            VkClearColorValue zero{};
                            VkImageSubresourceRange range{
                                VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                            vkCmdClearColorImage(
                                rec, vs.state[s]->image(),
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero,
                                1, &range);
                        }
                        vs.cur = 0;
                    }
                    if (vs.last_frame != timeline_frame) {
                        GpuImage* src = vs.state[vs.cur].get();
                        GpuImage* nxt = vs.state[1 - vs.cur].get();
                        src->transition(
                            rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        nxt->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
                        uint32_t vp[12] = {};
                        vp[0] = state_w;
                        vp[1] = kVsSlots;
                        vp[2] = static_cast<uint32_t>(seed64);
                        vp[3] = timeline_frame;
                        vp[4] = as_bits(static_cast<float>(fps));
                        vp[5] = as_bits(fx.params[1]);   // sweep speed
                        vp[6] = as_bits(fx.params[2]);   // stickiness
                        vp[7] = as_bits(fx.params[4]);   // spawn rate
                        vp[8] = as_bits(fx.params[6]);   // wiggle
                        vp[9] = static_cast<uint32_t>(fx.params[0] + 0.5f);
                        vp[10] = w;
                        vp[11] = h;
                        const GpuImage* vin[2] = {src, input_image(0)};
                        GpuImage* vout = nxt;
                        vs_front_->dispatch(rec, arena_, frame_index, vin, 2,
                                            &vout, 1, vp, sizeof(vp),
                                            state_w, kVsSlots,
                                            linear_sampler_);
                        vs.cur = 1 - vs.cur;
                        vs.last_frame = timeline_frame;
                    }
                    GpuImage* front = vs.state[vs.cur].get();
                    front->transition(
                        rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

                    FeedbackSlot& slot = feedback_state_[fx.id];
                    if (!slot.prev || slot.prev->width() != w ||
                        slot.prev->height() != h) {
                        slot.prev = GpuImage::create(
                            device_, VK_FORMAT_R16G16B16A16_SFLOAT, w, h,
                            VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT);
                        slot.last_frame = 0xFFFFFFFFu;
                        if (!slot.prev) return nullptr;
                        slot.prev->transition(
                            rec, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                        VkClearColorValue black{};
                        VkImageSubresourceRange range{
                            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                        vkCmdClearColorImage(
                            rec, slot.prev->image(),
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                            &range);
                    }
                    slot.prev->transition(
                        rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    push[kFxPreludeWords + param_count] = state_w;
                    const GpuImage* sampled[3] = {input_image(0),
                                                  slot.prev.get(), front};
                    fx_[static_cast<size_t>(fx.type)]->dispatch(
                        rec, arena_, frame_index, sampled, 3, &dst, 1, push,
                        push_bytes +
                            static_cast<uint32_t>(sizeof(uint32_t)),
                        w, h, linear_sampler_);
                    if (slot.last_frame != timeline_frame) {
                        dst->transition(rec,
                                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                        slot.prev->transition(
                            rec, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                        VkImageCopy vc{};
                        vc.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
                                             1};
                        vc.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
                                             1};
                        vc.extent = {w, h, 1};
                        vkCmdCopyImage(rec, dst->image(),
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       slot.prev->image(),
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       1, &vc);
                        slot.prev->transition(
                            rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        dst->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
                        slot.last_frame = timeline_frame;
                    }
                } else if (fx.type == doc::EffectType::FlowParticles) {
                    // Feedback-style persistent field advected by flow:
                    // inputs = {frame, own previous output, flow}.
                    FeedbackSlot& slot = feedback_state_[fx.id];
                    if (!slot.prev || slot.prev->width() != w ||
                        slot.prev->height() != h) {
                        slot.prev = GpuImage::create(
                            device_, VK_FORMAT_R16G16B16A16_SFLOAT, w, h,
                            VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT);
                        slot.last_frame = 0xFFFFFFFFu;
                        if (!slot.prev) return nullptr;
                        slot.prev->transition(
                            rec, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                        VkClearColorValue black{};
                        VkImageSubresourceRange range{
                            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                        vkCmdClearColorImage(
                            rec, slot.prev->image(),
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                            &range);
                    }
                    slot.prev->transition(
                        rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    const GpuImage* sampled[3] = {input_image(0),
                                                  slot.prev.get(),
                                                  input_image(1)};
                    fx_[static_cast<size_t>(fx.type)]->dispatch(
                        rec, arena_, frame_index, sampled, 3, &dst, 1, push,
                        push_bytes, w, h, linear_sampler_);
                    if (slot.last_frame != timeline_frame) {
                        dst->transition(rec,
                                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                        slot.prev->transition(
                            rec, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                        VkImageCopy fp{};
                        fp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
                                             1};
                        fp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
                                             1};
                        fp.extent = {w, h, 1};
                        vkCmdCopyImage(rec, dst->image(),
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       slot.prev->image(),
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       1, &fp);
                        slot.prev->transition(
                            rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        dst->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
                        slot.last_frame = timeline_frame;
                    }
                } else if (fx.type == doc::EffectType::SecurityMux) {
                    // Same ring mechanism as SlitScan at full depth: one
                    // masked dispatch per age band; each tile's seeded
                    // delay picks exactly one band, so the passes tile
                    // the output exactly once.
                    SlitSlot& slot = slit_state_[fx.id];
                    if (slot.ring[0] &&
                        (slot.ring[0]->width() != w ||
                         slot.ring[0]->height() != h)) {
                        for (auto& img : slot.ring) img.reset();
                        slot.head = slot.count = 0;
                        slot.last_frame = 0xFFFFFFFFu;
                    }
                    GpuImage* in_img =
                        const_cast<GpuImage*>(input_image(0));
                    auto history = [&](uint32_t age) -> const GpuImage* {
                        if (age == 0 || age > slot.count) return in_img;
                        const uint32_t idx =
                            (slot.head + kSlitRing - age) % kSlitRing;
                        return slot.ring[idx] ? slot.ring[idx].get() : in_img;
                    };
                    uint32_t* extra = &push[kFxPreludeWords + param_count];
                    const uint32_t mux_bytes =
                        push_bytes + static_cast<uint32_t>(sizeof(uint32_t));
                    for (uint32_t k = 0; k < kSlitRing; ++k) {
                        *extra = k;
                        const GpuImage* sampled[2] = {in_img, history(k)};
                        fx_[static_cast<size_t>(fx.type)]->dispatch(
                            rec, arena_, frame_index, sampled, 2, &dst, 1,
                            push, mux_bytes, w, h, linear_sampler_);
                    }
                    if (slot.last_frame != timeline_frame) {
                        std::unique_ptr<GpuImage>& target =
                            slot.ring[slot.head];
                        if (!target) {
                            target = GpuImage::create(
                                device_, VK_FORMAT_R16G16B16A16_SFLOAT, w, h,
                                VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT);
                            if (!target) return nullptr;
                        }
                        in_img->transition(
                            rec, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                        target->transition(
                            rec, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                        VkImageCopy mc{};
                        mc.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                             0, 1};
                        mc.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                             0, 1};
                        mc.extent = {w, h, 1};
                        vkCmdCopyImage(rec, in_img->image(),
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       target->image(),
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       1, &mc);
                        target->transition(
                            rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        in_img->transition(
                            rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        slot.head = (slot.head + 1) % kSlitRing;
                        slot.count = std::min(slot.count + 1, kSlitRing);
                        slot.last_frame = timeline_frame;
                    }
                } else if (fx.type == doc::EffectType::Engraver) {
                    // FM raster: run the phase integrator over this
                    // node's input, then render iso-phase traces from
                    // the integral (GenerateMe fm.pde model).
                    if (mod_integral_ &&
                        (mod_integral_->width() != w ||
                         mod_integral_->height() != h))
                        mod_integral_.reset();
                    if (!mod_integral_) {
                        mod_integral_ = GpuImage::create(
                            device_, VK_FORMAT_R32G32B32A32_SFLOAT, w, h,
                            VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_STORAGE_BIT);
                        if (!mod_integral_) return nullptr;
                    }
                    mod_integral_->transition(rec,
                                              VK_IMAGE_LAYOUT_GENERAL);
                    uint32_t fm_mode =
                        fx.params.size() > 4 &&
                                fx.params[4] >= 0.5f
                            ? 1u
                            : 0u;
                    // Bit 1: reversed march — the integration always
                    // follows the travel direction (speed sign).
                    if (fx.params.size() > 3 && fx.params[3] < 0.0f)
                        fm_mode |= 2u;
                    const uint32_t lanes = (fm_mode & 1u) ? h : w;
                    uint32_t mp[7] = {};
                    mp[0] = w;
                    mp[1] = h;
                    mp[2] = as_bits(fx.params[0]);   // omega
                    mp[3] = as_bits(fx.params[1]);   // distortion
                    mp[4] = as_bits(fx.params[2]);   // lowpass
                    mp[5] = fm_mode;
                    mp[6] = as_bits(fx.params.size() > 9
                                        ? fx.params[9]
                                        : 0.35f);    // response curve
                    const GpuImage* mi_in[1] = {input_image(0)};
                    GpuImage* mi_out = mod_integral_.get();
                    mod_integrate_->dispatch(rec, arena_, frame_index,
                                             mi_in, 1, &mi_out, 1, mp,
                                             sizeof(mp), lanes, 1,
                                             linear_sampler_);
                    mod_integral_->transition(
                        rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    const GpuImage* sampled[2] = {input_image(0),
                                                  mod_integral_.get()};
                    fx_[static_cast<size_t>(fx.type)]->dispatch(
                        rec, arena_, frame_index, sampled, 2, &dst, 1,
                        push, push_bytes, w, h, linear_sampler_);
                } else if (fx.type == doc::EffectType::BlendNode) {
                    // Graph merge: B rides input 1; unwired B falls
                    // back to In (the blend becomes identity-ish).
                    const GpuImage* b = node.inputs.size() > 1
                        ? input_image(1)
                        : input_image(0);
                    const GpuImage* sampled[2] = {input_image(0), b};
                    fx_[static_cast<size_t>(fx.type)]->dispatch(
                        rec, arena_, frame_index, sampled, 2, &dst, 1, push,
                        push_bytes, w, h, linear_sampler_);
                } else if (fx.type == doc::EffectType::AudioScope) {
                    // The waveform strip was uploaded before graph eval.
                    auto it = audio_strip_.find(fx.id);
                    const GpuImage* strip =
                        it != audio_strip_.end() && it->second
                            ? it->second.get()
                            : input_image(0);
                    const GpuImage* sampled[2] = {input_image(0), strip};
                    fx_[static_cast<size_t>(fx.type)]->dispatch(
                        rec, arena_, frame_index, sampled, 2, &dst, 1, push,
                        push_bytes, w, h, linear_sampler_);
                } else if (doc::is_stateful_feedback(fx.type)) {
                    FeedbackSlot& slot = feedback_state_[fx.id];
                    if (!slot.prev || slot.prev->width() != w ||
                        slot.prev->height() != h) {
                        slot.prev = GpuImage::create(
                            device_, VK_FORMAT_R16G16B16A16_SFLOAT, w, h,
                            VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT);
                        slot.last_frame = 0xFFFFFFFFu;
                        if (!slot.prev) return nullptr;
                        // First use: clear to black.
                        slot.prev->transition(
                            rec, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                        VkClearColorValue black{};
                        VkImageSubresourceRange range{
                            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                        vkCmdClearColorImage(
                            rec, slot.prev->image(),
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                            &range);
                    }
                    slot.prev->transition(
                        rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    const GpuImage* sampled[2] = {input_image(0),
                                                  slot.prev.get()};
                    fx_[static_cast<size_t>(fx.type)]->dispatch(
                        rec, arena_, frame_index, sampled, 2, &dst, 1, push,
                        push_bytes, w, h, linear_sampler_);
                    // Feed the loop: copy this output into the persistent
                    // target — once per timeline frame (paused = stable).
                    if (slot.last_frame != timeline_frame) {
                        dst->transition(rec,
                                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                        slot.prev->transition(
                            rec, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                        VkImageCopy copy{};
                        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                               0, 1};
                        copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                               0, 1};
                        copy.extent = {w, h, 1};
                        vkCmdCopyImage(rec, dst->image(),
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       slot.prev->image(),
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       1, &copy);
                        slot.prev->transition(
                            rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        dst->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
                        slot.last_frame = timeline_frame;
                    }
                } else {
                    const GpuImage* sampled[1] = {input_image(0)};
                    fx_[static_cast<size_t>(fx.type)]->dispatch(
                        rec, arena_, frame_index, sampled, 1, &dst, 1, push,
                        push_bytes, w, h, linear_sampler_);
                }
                break;
            }
            case GraphNode::Kind::Flow: {
                const uint32_t push[3] = {w, h,
                                          prev_frame_valid_ ? 1u : 0u};
                const GpuImage* sampled[2] = {plane_y_.get(), prev_plane};
                flow_->dispatch(rec, arena_, frame_index, sampled, 2, &dst, 1,
                                push, sizeof(push), dst->width(),
                                dst->height(), linear_sampler_);
                break;
            }
            case GraphNode::Kind::MatteExtract: {
                // Image-matte adapter (masks ARE images): the wired
                // image's luma IS the matte.
                const uint32_t push[2] = {w, h};
                const GpuImage* sampled[1] = {input_image(0)};
                matte_extract_->dispatch(rec, arena_, frame_index, sampled,
                                         1, &dst, 1, push, sizeof(push), w,
                                         h, linear_sampler_);
                break;
            }
            case GraphNode::Kind::MatteApply: {
                const uint32_t push[2] = {w, h};
                const GpuImage* sampled[3] = {input_image(0), input_image(1),
                                              input_image(2)};
                matte_apply_->dispatch(rec, arena_, frame_index, sampled, 3,
                                       &dst, 1, push, sizeof(push), w, h,
                                       linear_sampler_);
                break;
            }
        }

        results[static_cast<size_t>(index)] = dst;
        // Node-canvas thumbnail taps (docs/flow_canvas.md): effects key on
        // their id, layer sources on layer.id | bit 62 (id spaces
        // overlap).
        constexpr uint64_t kThumbSourceBit = 1ull << 62;
        if (node.kind == GraphNode::Kind::Effect &&
            node.effect_index >= 0 && node.layer_index >= 0) {
            const doc::EffectInstance& tfx =
                doc.layers[static_cast<size_t>(node.layer_index)]
                    .stack[static_cast<size_t>(node.effect_index)];
            record_thumb_tap(rec, frame_index, dst, tfx.id);
        } else if (node.kind == GraphNode::Kind::Source ||
                   node.kind == GraphNode::Kind::Generator ||
                   node.kind == GraphNode::Kind::LayerTransform) {
            if (node.layer_index >= 0) {
                record_thumb_tap(
                    rec, frame_index, dst,
                    doc.layers[static_cast<size_t>(node.layer_index)].id |
                        kThumbSourceBit);
            } else if (index == 0) {
                // The shared playhead source backs every untrimmed clip
                // layer's card.
                for (const doc::Layer& tl : doc.layers)
                    if (tl.visible &&
                        tl.source == doc::LayerSourceKind::Clip &&
                        !doc::layer_has_trim(tl) &&
                        !doc::layer_has_transform(tl))
                        record_thumb_tap(rec, frame_index, dst,
                                         tl.id | kThumbSourceBit);
            }
        }
        for (int input : node.inputs)
            if (--remaining_uses[static_cast<size_t>(input)] == 0)
                pool_.release(results[static_cast<size_t>(input)]);
    }

    // The published image: the preview tap when set, the OUTPUT
    // otherwise. The output itself always feeds the Output card's
    // thumbnail (cell key 0) — the preview never touches it.
    GpuImage* out = results[static_cast<size_t>(
        graph.preview >= 0 ? graph.preview : graph.output)];
    record_thumb_tap(rec, frame_index,
                     results[static_cast<size_t>(graph.output)], 0);
    out->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (out_source && results[0]) {
        results[0]->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        *out_source = results[0];
    }
    // Segmented (Codec-Box) evaluation ran on internal command buffers; the
    // caller's cmd records nothing — flush the tail segment so the result
    // is complete before the caller's submission (same-queue ordering).
    if (segmented) codec_flush_segment();

    // Cache miss: read the finished frame back into this slot's buffer,
    // harvested when the slot's fence has been waited. (Segmented docs are
    // history-bearing and never reach here with a nonzero ctx — guarded
    // anyway so the copy always records on the caller's cmd.)
    if (arm_readback && !segmented) {
        const size_t bytes = static_cast<size_t>(w) * h * 8;
        if (ensure_cache_io(cio, bytes)) {
            out->transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy copy{};
            copy.bufferRowLength = w;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {w, h, 1};
            vkCmdCopyImageToBuffer(cmd, out->image(),
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   cio.buf, 1, &copy);
            out->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            VkMemoryBarrier to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host,
                                 0, nullptr, 0, nullptr);
            cio.pending = true;
            cio.pending_ctx = cache_ctx;
            cio.pending_frame = timeline_frame;
            cio.pending_w = w;
            cio.pending_h = h;
        }
    }
    return out;
}

}  // namespace looks::gfx

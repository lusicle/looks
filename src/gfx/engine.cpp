#include "gfx/engine.h"

#include <vk_mem_alloc.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <vector>

#include "codec/core.h"   // parallel_blocks for the staging conversions
#include "doc/effects.h"
#include "gfx/error_diffusion.h"
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
    // Land any deferred dither walks before their state unwinds.
    for (auto& [id, slot] : ed_state_)
        if (slot && slot->busy) slot->worker.join();
    device_.wait_idle();
    VkDevice dev = device_.device();
    if (linear_sampler_) vkDestroySampler(dev, linear_sampler_, nullptr);
    if (codec_io_.fence) vkDestroyFence(dev, codec_io_.fence, nullptr);
    if (codec_io_.pool) vkDestroyCommandPool(dev, codec_io_.pool, nullptr);
    if (codec_io_.readback)
        vmaDestroyBuffer(device_.allocator(), codec_io_.readback,
                         codec_io_.readback_alloc);
    if (bounds_buf_)
        vmaDestroyBuffer(device_.allocator(), bounds_buf_, bounds_alloc_);
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
    to_rgb_desc.push_bytes = 6 * sizeof(uint32_t);
    to_rgb_ = ComputePipeline::create(device_, shader_dir, to_rgb_desc);
    if (!to_rgb_) return false;

    ComputePipelineDesc bounds_desc;
    bounds_desc.spv_name = "alpha_bounds.comp.spv";
    bounds_desc.sampled_inputs = 1;
    bounds_desc.storage_outputs = 1;
    bounds_desc.push_bytes = 2 * sizeof(uint32_t);
    alpha_bounds_ = ComputePipeline::create(device_, shader_dir, bounds_desc);
    if (!alpha_bounds_) return false;
    bounds_img_ = GpuImage::create(
        device_, VK_FORMAT_R32_UINT, 4, 1,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!bounds_img_) return false;
    {
        VkBufferCreateInfo binfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        binfo.size = 4 * sizeof(uint32_t);
        binfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo alloc_info{};
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                           VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo mapped{};
        if (vmaCreateBuffer(device_.allocator(), &binfo, &alloc_info,
                            &bounds_buf_, &bounds_alloc_,
                            &mapped) != VK_SUCCESS) {
            bounds_buf_ = VK_NULL_HANDLE;
            return false;
        }
        bounds_mapped_ = mapped.pMappedData;
    }

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

    // Node-canvas thumbnail tap: one small
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

    // GPU mosh: the codec box stays on the GPU whenever no bitstream is
    // rate-limited or corrupted (entropy is lossless, so the wire is just
    // predict + DCT + quant + dequant + IDCT — all data-parallel integer
    // math). The CPU box remains the byte-flips / bitrate path.
    ComputePipelineDesc mpredict_desc;
    mpredict_desc.spv_name = "mosh_predict.comp.spv";
    mpredict_desc.sampled_inputs = 1;
    mpredict_desc.storage_outputs = 6;
    mpredict_desc.push_bytes = 16 * sizeof(uint32_t);
    mosh_predict_ = ComputePipeline::create(device_, shader_dir,
                                            mpredict_desc);
    ComputePipelineDesc mwire_desc;
    mwire_desc.spv_name = "mosh_wire.comp.spv";
    mwire_desc.sampled_inputs = 2;
    mwire_desc.storage_outputs = 6;
    mwire_desc.push_bytes = 12 * sizeof(uint32_t);
    mosh_wire_ = ComputePipeline::create(device_, shader_dir, mwire_desc);
    ComputePipelineDesc mprobe_desc;
    mprobe_desc.spv_name = "mosh_rate_probe.comp.spv";
    mprobe_desc.sampled_inputs = 2;
    mprobe_desc.storage_outputs = 3;
    mprobe_desc.push_bytes = 16 * sizeof(uint32_t);
    mosh_rate_probe_ =
        ComputePipeline::create(device_, shader_dir, mprobe_desc);
    ComputePipelineDesc mreduce_desc;
    mreduce_desc.spv_name = "mosh_rate_reduce.comp.spv";
    mreduce_desc.sampled_inputs = 0;
    mreduce_desc.storage_outputs = 2;
    mreduce_desc.push_bytes = 6 * sizeof(uint32_t);
    mosh_rate_reduce_ =
        ComputePipeline::create(device_, shader_dir, mreduce_desc);
    ComputePipelineDesc mpick_desc;
    mpick_desc.spv_name = "mosh_rate_pick.comp.spv";
    mpick_desc.sampled_inputs = 0;
    mpick_desc.storage_outputs = 2;
    mpick_desc.push_bytes = 12 * sizeof(uint32_t);
    mosh_rate_pick_ =
        ComputePipeline::create(device_, shader_dir, mpick_desc);
    if (!mosh_rate_probe_ || !mosh_rate_reduce_ || !mosh_rate_pick_)
        return false;
    ComputePipelineDesc edx_desc;
    edx_desc.spv_name = "ed_expand.comp.spv";
    edx_desc.sampled_inputs = 0;
    edx_desc.storage_outputs = 2;
    edx_desc.push_bytes = 24 * sizeof(uint32_t);
    ed_expand_ = ComputePipeline::create(device_, shader_dir, edx_desc);
    if (!ed_expand_) return false;
    ComputePipelineDesc munorm_desc;
    munorm_desc.spv_name = "mosh_to_unorm.comp.spv";
    munorm_desc.sampled_inputs = 0;
    munorm_desc.storage_outputs = 6;
    munorm_desc.push_bytes = 4 * sizeof(uint32_t);
    mosh_unorm_ = ComputePipeline::create(device_, shader_dir, munorm_desc);
    if (!mosh_predict_ || !mosh_wire_ || !mosh_unorm_) return false;
    dummy_flow_ = GpuImage::create(device_, VK_FORMAT_R16G16B16A16_SFLOAT,
                                   1, 1, VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!dummy_flow_) return false;

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
    blend_desc.push_bytes = 9 * sizeof(uint32_t);
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

    // Text-overlay fonts: every .ttf under
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

bool Engine::ensure_prev_ref(uint32_t width, uint32_t height) {
    if (prev_y_ && prev_y_->width() == width && prev_y_->height() == height)
        return true;
    // The reference source changed format: the old copy may still be in
    // flight — rare enough that a full sync is the simple correct answer.
    device_.wait_idle();
    prev_y_ = GpuImage::create(device_, VK_FORMAT_R8_UNORM, width, height,
                               VK_IMAGE_USAGE_SAMPLED_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    return prev_y_ != nullptr;
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


bool Engine::mosh_gpu_box(VkCommandBuffer rec, const doc::EffectInstance& fx,
                          uint64_t state_key,
                          const codec::MoshParams& mp, const GpuImage* in,
                          GpuImage* flow_img, uint32_t w, uint32_t h,
                          uint32_t frame_index, uint32_t timeline_frame,
                          GpuImage* dst) {
    const uint32_t cw = w / 2;
    const uint32_t chh = h / 2;
    MoshGpuSlot& g = mosh_gpu_[state_key];
    if (g.w != w || g.h != h) {
        device_.wait_idle();
        const VkImageUsageFlags use = VK_IMAGE_USAGE_STORAGE_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        const auto make3 = [&](std::unique_ptr<GpuImage>* trio) {
            trio[0] = GpuImage::create(device_, VK_FORMAT_R32_UINT, w, h, use);
            trio[1] =
                GpuImage::create(device_, VK_FORMAT_R32_UINT, cw, chh, use);
            trio[2] =
                GpuImage::create(device_, VK_FORMAT_R32_UINT, cw, chh, use);
            return trio[0] && trio[1] && trio[2];
        };
        if (!make3(g.clean) || !make3(g.moshed) || !make3(g.pred_clean) ||
            !make3(g.pred_moshed) || !make3(g.pred_tmp))
            return false;
        g.w = w;
        g.h = h;
        g.has_state = false;
        g.last_frame = 0xFFFFFFFFu;
    }
    // Stateful advance happens once per timeline frame; paused re-renders
    // reuse the resident state (same contract as the CPU box's cache).
    const bool advance = !g.has_state || g.last_frame != timeline_frame;

    const auto barrier = [&] {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(rec, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb,
                             0, nullptr, 0, nullptr);
    };
    const auto transition3 = [&](std::unique_ptr<GpuImage>* trio) {
        for (int i = 0; i < 3; ++i)
            trio[i]->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
    };
    transition3(g.clean);
    transition3(g.moshed);
    transition3(g.pred_clean);
    transition3(g.pred_moshed);
    transition3(g.pred_tmp);
    // Also orders this frame's reads against the previous submission's
    // state writes (GENERAL-to-GENERAL transitions above are no-ops).
    barrier();

    if (advance) {
        // NV12 conversion: the wire's source.
        codec_io_.nv_y->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
        codec_io_.nv_uv->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
        {
            const uint32_t nv_push[2] = {w, h};
            const GpuImage* sampled[1] = {in};
            GpuImage* storage[2] = {codec_io_.nv_y.get(),
                                    codec_io_.nv_uv.get()};
            to_nv12_->dispatch(rec, arena_, frame_index, sampled, 1, storage,
                               2, nv_push, sizeof(nv_push), w, h,
                               linear_sampler_);
        }
        codec_io_.nv_y->transition(rec,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        codec_io_.nv_uv->transition(rec,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        GpuImage* flow = flow_img ? flow_img : dummy_flow_.get();
        flow->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        const uint32_t seed_mv = static_cast<uint32_t>(
            hash_u64(mp.seed ^ 0x33CC33CCull));
        const uint32_t seed_frame = static_cast<uint32_t>(
            hash_u64(hash_combine(mp.seed, timeline_frame)));
        const int quality = std::clamp(mp.quality, 1, 100);
        const bool gop_i =
            mp.gop_length > 0 &&
            timeline_frame % static_cast<uint32_t>(mp.gop_length) == 0;

        struct PlaneDims {
            uint32_t pw, ph, plane, shift;
        };
        const PlaneDims planes[3] = {
            {w, h, 0, 1}, {cw, chh, 1, 0}, {cw, chh, 2, 0}};
        codec_io_.rate_qsel->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
        const auto wire = [&](const PlaneDims& p, uint32_t mode,
                              uint32_t qual, uint32_t wmoshed,
                              uint32_t from_plane, uint32_t from_qsel,
                              GpuImage* src, GpuImage* cpred,
                              GpuImage* mpred, GpuImage* oclean,
                              GpuImage* omoshed) {
            struct {
                uint32_t pw, ph, plane, mode, quality, wm, sfp, cen;
                float cprob;
                uint32_t sframe, shift, qsel;
            } push = {p.pw,
                      p.ph,
                      p.plane,
                      mode,
                      qual,
                      wmoshed,
                      from_plane,
                      mode == 1 && mp.residual_corrupt > 0.0f ? 1u : 0u,
                      mp.residual_corrupt,
                      seed_frame,
                      p.shift,
                      from_qsel};
            const GpuImage* sampled2[2] = {codec_io_.nv_y.get(),
                                           codec_io_.nv_uv.get()};
            GpuImage* storage6[6] = {src,    cpred,   mpred,
                                     oclean, omoshed, codec_io_.rate_qsel.get()};
            mosh_wire_->dispatch(rec, arena_, frame_index, sampled2, 2,
                                 storage6, 6, &push, sizeof(push),
                                 (p.pw + 7) / 8, (p.ph + 7) / 8,
                                 linear_sampler_);
        };

        // Rate control (Bitrate Starve): probe the exact per-rung stream
        // sizes on the GPU (per-block AC bits + pairwise DC deltas — the
        // DC "chain" is a sum of neighbor terms, so it reduces), pick the
        // ladder rung in a one-thread kernel, and let the wire read the
        // choice from qsel. Rate control never touches the CPU.
        const bool rate_on = mp.bitrate_budget > 0;
        uint32_t rungs[8] = {};
        uint32_t nrungs = 0;
        if (rate_on) {
            int q = quality;
            rungs[nrungs++] = static_cast<uint32_t>(q);
            while (q > 1 && nrungs < 7) {
                q = std::max(1, q - 15);
                rungs[nrungs++] = static_cast<uint32_t>(q);
            }
        }
        const auto run_rate = [&](uint32_t mode, uint32_t header) {
            codec_io_.rate_dc->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
            codec_io_.rate_bits->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
            VkClearColorValue zero{};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                          1};
            vkCmdClearColorImage(rec, codec_io_.rate_bits->image(),
                                 VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
            VkMemoryBarrier cb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            cb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            cb.dstAccessMask =
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(rec, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                                 &cb, 0, nullptr, 0, nullptr);
            for (int p = 0; p < 3; ++p) {
                const uint32_t bwp = (planes[p].pw + 7) / 8;
                const uint32_t bhp = (planes[p].ph + 7) / 8;
                struct {
                    uint32_t pw, ph, plane, mode, rung_count, blocks_w;
                    uint32_t pad0, pad1;
                    uint32_t rungs[8];
                } ppush = {planes[p].pw, planes[p].ph, planes[p].plane,
                           mode,         nrungs,       bwp,
                           0,            0,            {}};
                std::memcpy(ppush.rungs, rungs, sizeof(rungs));
                const GpuImage* sampled2[2] = {codec_io_.nv_y.get(),
                                               codec_io_.nv_uv.get()};
                GpuImage* pst[3] = {g.pred_clean[p].get(),
                                    codec_io_.rate_dc.get(),
                                    codec_io_.rate_bits.get()};
                mosh_rate_probe_->dispatch(rec, arena_, frame_index,
                                           sampled2, 2, pst, 3, &ppush,
                                           sizeof(ppush), bwp, bhp,
                                           linear_sampler_);
                barrier();
                struct {
                    uint32_t pw, ph, plane, mode, rung_count, blocks_w;
                } rpush = {planes[p].pw, planes[p].ph, planes[p].plane,
                           mode,         nrungs,       bwp};
                GpuImage* rst[2] = {codec_io_.rate_dc.get(),
                                    codec_io_.rate_bits.get()};
                mosh_rate_reduce_->dispatch(rec, arena_, frame_index,
                                            nullptr, 0, rst, 2, &rpush,
                                            sizeof(rpush), bwp, bhp,
                                            linear_sampler_);
                barrier();
            }
            struct {
                uint32_t rung_count, budget, header, pad;
                uint32_t rungs[8];
            } kpush = {nrungs, mp.bitrate_budget, header, 0, {}};
            std::memcpy(kpush.rungs, rungs, sizeof(rungs));
            GpuImage* kst[2] = {codec_io_.rate_bits.get(),
                                codec_io_.rate_qsel.get()};
            mosh_rate_pick_->dispatch(rec, arena_, frame_index, nullptr, 0,
                                      kst, 2, &kpush, sizeof(kpush), 1, 1,
                                      linear_sampler_);
            barrier();
        };

        if (!g.has_state || gop_i) {
            // I frame into the clean chain (+ generation-loss recycles),
            // then the moshed chain accepts it unless dropping.
            if (rate_on) run_rate(0, 1);
            for (int p = 0; p < 3; ++p)
                wire(planes[p], 0, static_cast<uint32_t>(quality), 0, 0,
                     rate_on ? 1u : 0u, g.clean[p].get(), g.clean[p].get(),
                     g.clean[p].get(), g.clean[p].get(), g.clean[p].get());
            for (int gi = 0; gi < std::min(mp.generations, 12); ++gi) {
                const int gq =
                    std::clamp(mp.quality - ((gi & 1) ? 9 : 0), 1, 100);
                barrier();
                for (int p = 0; p < 3; ++p)
                    wire(planes[p], 0, static_cast<uint32_t>(gq), 0, 1, 0,
                         g.clean[p].get(), g.clean[p].get(),
                         g.clean[p].get(), g.clean[p].get(),
                         g.clean[p].get());
            }
            if (!g.has_state || !mp.drop_iframes) {
                // Accept: the moshed chain takes the clean picture.
                VkMemoryBarrier to_xfer{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                to_xfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                to_xfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(rec,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                                     &to_xfer, 0, nullptr, 0, nullptr);
                for (int p = 0; p < 3; ++p) {
                    VkImageCopy copy{};
                    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
                                           1};
                    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
                                           1};
                    copy.extent = {g.clean[p]->width(),
                                   g.clean[p]->height(), 1};
                    vkCmdCopyImage(rec, g.clean[p]->image(),
                                   VK_IMAGE_LAYOUT_GENERAL,
                                   g.moshed[p]->image(),
                                   VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
                }
                VkMemoryBarrier from_xfer{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                from_xfer.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                from_xfer.dstAccessMask =
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                vkCmdPipelineBarrier(rec, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                     1, &from_xfer, 0, nullptr, 0, nullptr);
            }
        } else {
            const auto predict = [&](uint32_t mangle,
                                     std::unique_ptr<GpuImage>* ref,
                                     std::unique_ptr<GpuImage>* out) {
                struct {
                    uint32_t w, h, cw, ch, bw, bh, mangle, field;
                    float scale, rc, rs, rnd, amt;
                    uint32_t smv, frame, hasflow;
                } push = {w,
                          h,
                          cw,
                          chh,
                          flow_img ? flow_img->width() : 0,
                          flow_img ? flow_img->height() : 0,
                          mangle,
                          static_cast<uint32_t>(mp.mv_field),
                          mp.mv_scale,
                          std::cos(mp.mv_rotate),
                          std::sin(mp.mv_rotate),
                          mp.mv_random,
                          mp.mv_field_amount,
                          seed_mv,
                          timeline_frame,
                          flow_img ? 1u : 0u};
                const GpuImage* sampled1[1] = {flow};
                GpuImage* storage6[6] = {ref[0].get(), ref[1].get(),
                                         ref[2].get(), out[0].get(),
                                         out[1].get(), out[2].get()};
                mosh_predict_->dispatch(rec, arena_, frame_index, sampled1,
                                        1, storage6, 6, &push, sizeof(push),
                                        w, h, linear_sampler_);
            };
            predict(0, g.clean, g.pred_clean);
            predict(1, g.moshed, g.pred_moshed);
            for (int r = 0; r < std::min(mp.p_repeat, 8); ++r) {
                barrier();
                predict(1, g.pred_moshed, g.pred_tmp);
                for (int i = 0; i < 3; ++i)
                    g.pred_moshed[i].swap(g.pred_tmp[i]);
            }
            barrier();
            if (rate_on) run_rate(1, 0);
            for (int p = 0; p < 3; ++p)
                wire(planes[p], 1, static_cast<uint32_t>(quality), 0, 0,
                     rate_on ? 1u : 0u, g.pred_clean[p].get(),
                     g.pred_clean[p].get(), g.pred_moshed[p].get(),
                     g.clean[p].get(), g.moshed[p].get());
        }
        g.has_state = true;
        g.last_frame = timeline_frame;
    }

    // Moshed planes -> unorm upload images -> shared RGB + composite tail.
    barrier();
    codec_io_.up_y->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
    codec_io_.up_u->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
    codec_io_.up_v->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
    {
        const uint32_t un_push[4] = {w, h, cw, chh};
        GpuImage* storage6[6] = {g.moshed[0].get(),     g.moshed[1].get(),
                                 g.moshed[2].get(),     codec_io_.up_y.get(),
                                 codec_io_.up_u.get(),  codec_io_.up_v.get()};
        mosh_unorm_->dispatch(rec, arena_, frame_index, nullptr, 0, storage6,
                              6, un_push, sizeof(un_push), w, h,
                              linear_sampler_);
    }
    codec_io_.up_y->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    codec_io_.up_u->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    codec_io_.up_v->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    GpuImage* temp = pool_.acquire(w, h);
    if (!temp) return false;
    temp->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
    {
        // Moshed planes are already working-size: identity fit.
        struct {
            uint32_t w, h;
            float rx, ry, iw, ih;
        } rgb_push = {w,    h,
                      0.0f, 0.0f,
                      1.0f / static_cast<float>(w),
                      1.0f / static_cast<float>(h)};
        const GpuImage* planes3[3] = {codec_io_.up_y.get(),
                                      codec_io_.up_u.get(),
                                      codec_io_.up_v.get()};
        to_rgb_->dispatch(rec, arena_, frame_index, planes3, 3, &temp, 1,
                          &rgb_push, sizeof(rgb_push), w, h, linear_sampler_);
    }
    temp->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    {
        const auto bits = [](float v) {
            uint32_t b;
            std::memcpy(&b, &v, sizeof(b));
            return b;
        };
        const uint32_t mix_push[4] = {w, h, bits(fx.wet), bits(fx.opacity)};
        const GpuImage* sampled2[2] = {in, temp};
        fx_mix_->dispatch(rec, arena_, frame_index, sampled2, 2, &dst, 1,
                          mix_push, sizeof(mix_push), w, h, linear_sampler_);
    }
    pool_.release(temp);
    return true;
}

bool Engine::composite_ed(VkCommandBuffer rec, const doc::EffectInstance& fx,
                          const GpuImage* in_img, const EdState& ed,
                          uint32_t w, uint32_t h, uint32_t frame_index,
                          GpuImage* dst) {
    // Packed picks (3x5 bits per pixel, 4x smaller than colors) up to the
    // GPU, palette expand, wet/opacity composite.
    const size_t px_count = static_cast<size_t>(w) * h;
    if (!codec_io_.staging->upload_image(rec, ed.out.data(), px_count * 4, w,
                                         *codec_io_.up_idx))
        return false;
    codec_io_.up_idx->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
    GpuImage* temp = pool_.acquire(w, h);
    if (!temp) return false;
    temp->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
    {
        struct {
            uint32_t w, h, nlevels, pad;
            float lv[17];
            float tail_pad[3];
        } xpush = {w, h, 0, 0, {}, {}};
        xpush.nlevels =
            static_cast<uint32_t>(ed_level_table(fx, xpush.lv));
        GpuImage* xst[2] = {codec_io_.up_idx.get(), temp};
        ed_expand_->dispatch(rec, arena_, frame_index, nullptr, 0, xst, 2,
                             &xpush, sizeof(xpush), w, h, linear_sampler_);
    }
    temp->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    {
        const auto bits = [](float v) {
            uint32_t b;
            std::memcpy(&b, &v, sizeof(b));
            return b;
        };
        const uint32_t mix_push[4] = {w, h, bits(fx.wet), bits(fx.opacity)};
        const GpuImage* sampled2[2] = {in_img, temp};
        fx_mix_->dispatch(rec, arena_, frame_index, sampled2, 2, &dst, 1,
                          mix_push, sizeof(mix_push), w, h, linear_sampler_);
    }
    pool_.release(temp);
    return true;
}

bool Engine::ensure_codec_io(uint32_t width, uint32_t height) {
    if (codec_io_.nv_y && codec_io_.nv_y->width() == width &&
        codec_io_.nv_y->height() == height)
        return true;
    device_.wait_idle();
    // SAMPLED: the GPU mosh wire reads the NV12 planes directly.
    const VkImageUsageFlags conv = VK_IMAGE_USAGE_STORAGE_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT;
    // STORAGE: the GPU mosh unorm pass writes the upload planes directly.
    const VkImageUsageFlags up = VK_IMAGE_USAGE_SAMPLED_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                 VK_IMAGE_USAGE_STORAGE_BIT;
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
    codec_io_.up_idx = GpuImage::create(
        device_, VK_FORMAT_R32_UINT, width, height,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    // Rate control scratch: 8 rung columns of per-block DCs (sized for the
    // luma grid, chroma reuses the left portion), the per-rung bit totals
    // (cleared per probe), and the picked quality.
    const uint32_t bw_luma = (width + 7) / 8;
    const uint32_t bh_luma = (height + 7) / 8;
    codec_io_.rate_dc =
        GpuImage::create(device_, VK_FORMAT_R32_UINT, bw_luma * 8, bh_luma,
                         VK_IMAGE_USAGE_STORAGE_BIT);
    codec_io_.rate_bits = GpuImage::create(
        device_, VK_FORMAT_R32_UINT, 8, 1,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    codec_io_.rate_qsel = GpuImage::create(device_, VK_FORMAT_R32_UINT, 1, 1,
                                           VK_IMAGE_USAGE_STORAGE_BIT);
    if (!codec_io_.nv_y || !codec_io_.nv_uv || !codec_io_.up_y ||
        !codec_io_.up_u || !codec_io_.up_v || !codec_io_.up_idx ||
        !codec_io_.rate_dc || !codec_io_.rate_bits || !codec_io_.rate_qsel)
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

// Node-canvas thumbnail tap: downsample `src` into
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

bool Engine::read_measure_bounds(float rect[4]) const {
    if (!bounds_recorded_ || !bounds_mapped_ || !bounds_w_ || !bounds_h_)
        return false;
    vmaInvalidateAllocation(device_.allocator(), bounds_alloc_, 0,
                            VK_WHOLE_SIZE);
    uint32_t v[4];
    std::memcpy(v, bounds_mapped_, sizeof(v));
    // Max cells store the complement (one atomic min serves all four);
    // an untouched clear means fully transparent content.
    if (v[0] == 0xFFFFFFFFu || v[2] == 0xFFFFFFFFu) return false;
    const uint32_t max_x = 0xFFFFFFFFu - v[2];
    const uint32_t max_y = 0xFFFFFFFFu - v[3];
    if (max_x < v[0] || max_y < v[1]) return false;
    const float fw = static_cast<float>(bounds_w_);
    const float fh = static_cast<float>(bounds_h_);
    rect[0] = static_cast<float>(v[0]) / fw;
    rect[1] = static_cast<float>(v[1]) / fh;
    rect[2] = static_cast<float>(max_x + 1 - v[0]) / fw;
    rect[3] = static_cast<float>(max_y + 1 - v[1]) / fh;
    return true;
}

GpuImage* Engine::render(VkCommandBuffer cmd, uint32_t frame_index,
                         const doc::Document& doc,
                         uint64_t root_id, uint32_t root_frame, double fps,
                         uint32_t canvas_w, uint32_t canvas_h,
                         uint64_t cache_ctx, uint32_t cache_frame,
                         GpuImage** out_source,
                         const LayerSourceFrame* layer_sources,
                         size_t layer_source_count,
                         uint64_t preview_node, uint64_t preview_layer,
                         uint64_t measure_placement) {
    bounds_recorded_ = false;
    // The entity being rendered (a sequence or a scoped look) and the
    // frame it plays at. Nodes belonging to NESTED instances read their
    // own look and their own local frame instead (both are shadowed
    // inside the dispatch loop) — only the root's clock drives caching
    // and prev-frame tracking.
    if (out_source) *out_source = nullptr;
    if (canvas_w == 0 || canvas_h == 0) return nullptr;

    arena_.reset(frame_index);
    pool_.release_all();
    StagingBuffer& staging = *staging_[frame_index % kFramesInFlight];
    staging.reset();

    // The CANVAS is the project's, never the clip's: a cut between two
    // source sizes must not resize the graph.
    // Working dimensions shrink under the preview proxy — kernels sample
    // by uv, so everything scales; even dims keep the codec paths happy.
    const uint32_t w = std::max((canvas_w / preview_divisor_) & ~1u, 2u);
    const uint32_t h = std::max((canvas_h / preview_divisor_) & ~1u, 2u);

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
    if (measure_placement == 0) measured_placement_ = 0;
    if (cache_ctx != 0) {
        cache_.set_context(cache_ctx);
        // A newly-selected block needs ONE evaluated graph to measure
        // its bounds; after that, cached frames serve as usual and the
        // last measured box stands (edits miss the cache anyway).
        const bool need_measure =
            measure_placement && measure_placement != measured_placement_;
        const RenderCache::Frame* hit =
            need_measure ? nullptr : cache_.find(cache_frame);
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

    const RenderGraph graph =
        compile_graph(doc, root_id, root_frame, preview_node, preview_layer,
                      measure_placement, out_source != nullptr);
    if (!graph.valid) {
        log_error("engine: render graph invalid (cycle?)");
        return nullptr;
    }
    // A node's subject lives in ITS instance's look.
    auto node_look = [&](const GraphNode& n) -> const doc::Look& {
        return doc.look(
            graph.instances[static_cast<size_t>(n.instance)].look);
    };

    // Codec-Box nodes need the frame on the CPU mid-graph: evaluate in
    // fenced segments on an internal command buffer instead of `cmd`
    // (correctness-first; the roundtrip stalls preview, accepts).
    bool segmented = false;
    for (const GraphNode& n : graph.nodes) {
        if (n.kind != GraphNode::Kind::Effect) continue;
        const doc::EffectInstance& fx =
            node_look(n).layers[static_cast<size_t>(n.layer_index)]
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
    // One staging reset per render: every prior render's segments were
    // fence-waited at its end, so retired buffers and offsets are safe to
    // recycle here (mid-frame resets would clobber same-segment uploads).
    if (segmented) codec_io_.staging->reset();

    // The REFERENCE SOURCE: the first clip source playing in the root
    // instance. A multi-clip look has no single "the source", so the graph
    // names one — the A/B wipe compares against it and the shared motion
    // field is measured on it.
    const uint64_t ref_key =
        graph.source >= 0
            ? graph.nodes[static_cast<size_t>(graph.source)].key
            : 0;

    // Preserve the reference luma for flow/motion BEFORE the new upload
    // overwrites its planes. Only when the timeline advanced (a paused
    // re-render keeps the prior prev frame) and the reference is still the
    // same source — a cut has no motion across it, by construction.
    const auto ref_it = ref_key ? layer_planes_.find(ref_key)
                                : layer_planes_.end();
    const LayerPlanes* ref_last =
        ref_it != layer_planes_.end() ? &ref_it->second : nullptr;
    // A reference that changes size this render has its planes recreated
    // below, so the copy would read a freed image — and there is no motion
    // between two different formats anyway.
    if (ref_last) {
        for (size_t i = 0; i < layer_source_count; ++i)
            if (layer_sources[i].key == ref_key &&
                (layer_sources[i].planes.width != ref_last->width ||
                 layer_sources[i].planes.height != ref_last->height))
                ref_last = nullptr;
    }
    if (have_last_frame_ && root_frame != last_timeline_frame_ &&
        ref_key != 0 && ref_key == last_ref_key_ && ref_last && ref_last->y &&
        ensure_prev_ref(ref_last->width, ref_last->height)) {
        prev_frame_valid_ = root_frame == last_timeline_frame_ + 1;
        ref_last->y->transition(rec, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        prev_y_->transition(rec, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {ref_last->width, ref_last->height, 1};
        vkCmdCopyImage(rec, ref_last->y->image(),
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, prev_y_->image(),
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        prev_y_->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    } else {
        prev_frame_valid_ = false;
    }
    last_timeline_frame_ = root_frame;
    last_ref_key_ = ref_key;
    have_last_frame_ = true;

    // Clip sources: one I420 upload per PLACEMENT, keyed per instance.
    for (size_t i = 0; i < layer_source_count; ++i) {
        const LayerSourceFrame& lf = layer_sources[i];
        if (!lf.planes.y || !lf.planes.u || !lf.planes.v ||
            lf.planes.width == 0 || lf.key == 0)
            continue;
        LayerPlanes& lp = layer_planes_[lf.key];
        if (lp.width != lf.planes.width || lp.height != lf.planes.height) {
            device_.wait_idle();
            const VkImageUsageFlags lu =
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            const uint32_t lcw = (lf.planes.width + 1) / 2;
            const uint32_t lch = (lf.planes.height + 1) / 2;
            // Y also feeds the prev-luma copy when this source is the
            // graph's reference.
            lp.y = GpuImage::create(device_, VK_FORMAT_R8_UNORM,
                                    lf.planes.width, lf.planes.height,
                                    lu | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
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

    // The motion pair the shared Flow field and MotionExtract read. With
    // no clip source in the look there is nothing moving to measure: a
    // cleared 1x1 plane reads as flat black, so motion comes out zero
    // instead of undefined.
    GpuImage* ref_plane = nullptr;
    if (ref_key) {
        if (auto it = layer_planes_.find(ref_key);
            it != layer_planes_.end() && it->second.y)
            ref_plane = it->second.y.get();
    }
    if (!ref_plane) {
        if (!dummy_y_) {
            dummy_y_ = GpuImage::create(
                device_, VK_FORMAT_R8_UNORM, 1, 1,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            if (!dummy_y_) return nullptr;
        }
        dummy_y_->transition(rec, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkClearColorValue black{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                            1};
        vkCmdClearColorImage(rec, dummy_y_->image(),
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                             &range);
        ref_plane = dummy_y_.get();
        prev_frame_valid_ = false;
    }
    ref_plane->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // Seek gap / first frame / cut: neutral prev = current (zero motion).
    GpuImage* prev_plane = prev_frame_valid_ ? prev_y_.get() : ref_plane;

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
                const double t1 = root_frame / fps;
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
        // Every instance that actually renders, so a scope in a nested
        // look still gets its waveform.
        for (const LookInstance& li : graph.instances)
        for (const doc::Layer& layer : doc.look(li.look).layers)
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
    // A/B wipe: keep the effect-stripped BEFORE composite alive to the
    // end (composition attributes intact - only effects differ).
    if (out_source && graph.before >= 0)
        remaining_uses[static_cast<size_t>(graph.before)]++;
    // The measure tap survives to the tail's alpha-bounds reduction.
    if (graph.measure >= 0)
        remaining_uses[static_cast<size_t>(graph.measure)]++;

    auto as_bits = [](float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        return bits;
    };

    for (int index : graph.order) {
        const GraphNode& node = graph.nodes[static_cast<size_t>(index)];
        // Per-instance view: `look` is the look this node
        // came from and `timeline_frame` its LOCAL clock, both shadowing
        // the root's. Everything below addresses its own placement, so a
        // look nested twice runs twice on two different frames. `skey`
        // is the instance-scoped state key — history slots and private
        // source planes hang off it, never off the bare effect id.
        const LookInstance& linst =
            graph.instances[static_cast<size_t>(node.instance)];
        const doc::Look& look = doc.look(linst.look);
        const uint32_t timeline_frame = linst.local_frame;
        const uint64_t skey = node.key;
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
                // The decode pool feeds every clip source under this
                // node's key. A key with no frame (decode failed, or an
                // unbound asset) reads flat black rather than another
                // layer's pixels.
                auto it = layer_planes_.find(skey);
                if (it == layer_planes_.end() || !it->second.y) {
                    dst->transition(rec, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                    const VkClearColorValue clear{};
                    const VkImageSubresourceRange range{
                        VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    vkCmdClearColorImage(rec, dst->image(),
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         &clear, 1, &range);
                    break;
                }
                const GpuImage* planes[3] = {it->second.y.get(),
                                             it->second.u.get(),
                                             it->second.v.get()};
                // Aspect-preserving fit: the clip lands centered at its
                // own shape, transparent outside - never stretched.
                float fit[4];
                source_fit_rect(it->second.y->width(),
                                it->second.y->height(), w, h, fit);
                struct {
                    uint32_t w, h;
                    float rx, ry, iw, ih;
                } push = {w,      h,
                          fit[0], fit[1],
                          1.0f / std::max(fit[2], 1.0f),
                          1.0f / std::max(fit[3], 1.0f)};
                to_rgb_->dispatch(rec, arena_, frame_index, planes, 3, &dst, 1,
                                  &push, sizeof(push), w, h, linear_sampler_);
                break;
            }
            case GraphNode::Kind::LayerTransform: {
                // Look layers only - sequence Motion composites through
                // the lane blend, never through a transform pass.
                const doc::Layer& layer =
                    look.layers[static_cast<size_t>(node.layer_index)];
                uint32_t push[9] = {};
                push[0] = w;
                push[1] = h;
                push[2] = as_bits(layer.crop_l);
                push[3] = as_bits(layer.crop_r);
                push[4] = as_bits(layer.crop_t);
                push[5] = as_bits(layer.crop_b);
                push[6] = (layer.flip_h ? 1u : 0u) |
                          (layer.flip_v ? 2u : 0u);
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
                        look.layers[static_cast<size_t>(node.layer_index)];
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
                // layer_index -1 is a SEQUENCE lane stack: plain
                // alpha-over at the PLACEMENT's opacity, sampling the
                // lane through its canvas affine IN the composite -
                // Motion is an attribute of the arrangement, never an
                // effect pass, and the timeline owns no blend modes.
                // Otherwise the owning look layer's mode applies;
                // premultiplied alpha is the gate either way.
                doc::BlendMode mode = doc::BlendMode::Normal;
                float opacity = node.p_opacity;
                bool moved = false;
                if (node.layer_index >= 0) {
                    const doc::Layer& layer =
                        look.layers[static_cast<size_t>(node.layer_index)];
                    mode = layer.blend;
                    opacity = layer.opacity;
                } else {
                    moved = node.p_scale != 1.0f || node.p_rotate != 0.0f ||
                            node.p_shift_x != 0.0f ||
                            node.p_shift_y != 0.0f;
                }
                uint32_t push[9] = {};
                push[0] = w;
                push[1] = h;
                push[2] = static_cast<uint32_t>(mode);
                push[3] = as_bits(opacity);
                push[4] = moved ? 1u : 0u;
                push[5] = as_bits(node.p_scale);
                push[6] = as_bits(node.p_rotate);
                push[7] = as_bits(node.p_shift_x);
                push[8] = as_bits(node.p_shift_y);
                const GpuImage* sampled[2] = {input_image(0), input_image(1)};
                layer_blend_->dispatch(rec, arena_, frame_index, sampled, 2,
                                       &dst, 1, push, sizeof(push), w, h,
                                       linear_sampler_);
                break;
            }
            case GraphNode::Kind::Effect: {
                const doc::EffectInstance& fx =
                    look.layers[static_cast<size_t>(node.layer_index)]
                        .stack[static_cast<size_t>(node.effect_index)];

                if (doc::is_codec_box(fx.type)) {
                    GpuImage* in = input_image(0);
                    GpuImage* flow_img =
                        fx.type == doc::EffectType::Datamosh &&
                                node.inputs.size() > 1
                            ? input_image(1)
                            : nullptr;

                    // GPU box whenever no real bitstream is needed: the
                    // wire is pure data-parallel integer math then, and
                    // the roundtrip (readback, fence, upload) vanishes.
                    const uint64_t gpu_seed = hash_combine(
                        hash_combine(doc.master_seed, fx.id), fx.seed);
                    const codec::MoshParams gp = mosh_params(fx, gpu_seed);
                    if (gp.byte_flips == 0) {
                        if (!mosh_gpu_box(rec, fx, skey, gp, in, flow_img, w,
                                          h, frame_index, timeline_frame,
                                          dst))
                            return nullptr;
                        break;
                    }

                    // --- CPU Codec-Box: GPU->CPU->GPU roundtrip (the
                    // byte-flips / bitrate-budget paths need real bytes).
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
                    MoshSlot& slot = mosh_state_[skey];
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
                        // Codec round-trip planes are working-size:
                        // identity fit.
                        struct {
                            uint32_t w, h;
                            float rx, ry, iw, ih;
                        } rgb_push = {w,    h,
                                      0.0f, 0.0f,
                                      1.0f / static_cast<float>(w),
                                      1.0f / static_cast<float>(h)};
                        const GpuImage* planes3[3] = {codec_io_.up_y.get(),
                                                      codec_io_.up_u.get(),
                                                      codec_io_.up_v.get()};
                        to_rgb_->dispatch(rec, arena_, frame_index, planes3,
                                          3, &temp, 1, &rgb_push,
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
                    GpuImage* in_img = const_cast<GpuImage*>(input_image(0));
                    auto& slot_ptr = ed_state_[skey];
                    if (!slot_ptr)
                        slot_ptr = std::make_unique<EdSlotAsync>();
                    EdSlotAsync& slot = *slot_ptr;
                    const size_t px_count = static_cast<size_t>(w) * h;

                    // 0. Land the deferred walk (kicked last frame — it
                    // had the whole frame to run off-thread).
                    if (slot.busy) {
                        slot.worker.join();
                        slot.busy = false;
                        slot.has_result = true;
                    }

                    // 1. Composite LAST frame's dither (the one-frame
                    // delay, same legal latency as Feedback's cycle
                    // exemption). First frame / size change passes dry.
                    if (slot.has_result && slot.result_w == w &&
                        slot.result_h == h) {
                        if (!composite_ed(rec, fx, in_img, slot.ed, w, h,
                                          frame_index, dst))
                            return nullptr;
                    } else {
                        const uint32_t mix_push[4] = {
                            w, h, as_bits(fx.wet), as_bits(fx.opacity)};
                        const GpuImage* sampled2[2] = {in_img, in_img};
                        fx_mix_->dispatch(rec, arena_, frame_index, sampled2,
                                          2, &dst, 1, mix_push,
                                          sizeof(mix_push), w, h,
                                          linear_sampler_);
                    }

                    // 2. Capture THIS frame's input and walk it during the
                    // rest of the frame; next frame consumes the result.
                    // Once per timeline frame (paused re-renders reuse).
                    if (slot.captured_frame != timeline_frame) {
                        in_img->transition(
                            rec, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                        VkMemoryBarrier ed_pre{
                            VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                        ed_pre.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                        ed_pre.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                        vkCmdPipelineBarrier(
                            rec, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &ed_pre, 0,
                            nullptr, 0, nullptr);
                        VkBufferImageCopy ed_copy{};
                        ed_copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,
                                                    0, 0, 1};
                        ed_copy.imageExtent = {w, h, 1};
                        vkCmdCopyImageToBuffer(
                            rec, in_img->image(),
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            codec_io_.readback, 1, &ed_copy);
                        in_img->transition(
                            rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        VkMemoryBarrier ed_host{
                            VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                        ed_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                        ed_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                        vkCmdPipelineBarrier(
                            rec, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &ed_host, 0,
                            nullptr, 0, nullptr);
                        codec_flush_segment();
                        rec = codec_begin_segment();
                        vmaInvalidateAllocation(device_.allocator(),
                                                codec_io_.readback_alloc, 0,
                                                VK_WHOLE_SIZE);
                        const uint16_t* src = static_cast<const uint16_t*>(
                            codec_io_.mapped);
                        slot.input.assign(src, src + px_count * 4);
                        slot.pending_fx = fx;
                        slot.result_w = w;
                        slot.result_h = h;
                        slot.captured_frame = timeline_frame;
                        slot.busy = true;
                        EdSlotAsync* s = &slot;
                        const uint32_t cap_w = w, cap_h = h;
                        slot.worker = std::thread([s, cap_w, cap_h] {
                            run_error_diffusion(s->input.data(), cap_w,
                                                cap_h, s->pending_fx, s->ed);
                        });
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
                                                  ref_plane};
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
                        RdSlot& slot = rd_state_[skey];
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
                    SlitSlot& slot = slit_state_[skey];
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
                    SlitSlot& slot = slit_state_[skey];
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
                    SlitSlot& slot = slit_state_[skey];
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
                    HoldSlot& slot = hold_state_[skey];
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
                    SlitSlot& slot = slit_state_[skey];
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
                    RdSlot& slot = rd_state_[skey];
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
                    VsSlot& vs = vs_state_[skey];
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

                    FeedbackSlot& slot = feedback_state_[skey];
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
                    FeedbackSlot& slot = feedback_state_[skey];
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
                    SlitSlot& slot = slit_state_[skey];
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
                    FeedbackSlot& slot = feedback_state_[skey];
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
                const GpuImage* sampled[2] = {ref_plane, prev_plane};
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
        // Node-canvas thumbnail taps: effects key on
        // their id, layer sources on layer.id | bit 62 (id spaces
        // overlap).
        constexpr uint64_t kThumbSourceBit = 1ull << 62;
        if (node.kind == GraphNode::Kind::Effect &&
            node.effect_index >= 0 && node.layer_index >= 0) {
            const doc::EffectInstance& tfx =
                look.layers[static_cast<size_t>(node.layer_index)]
                    .stack[static_cast<size_t>(node.effect_index)];
            record_thumb_tap(rec, frame_index, dst, tfx.id);
        } else if (node.kind == GraphNode::Kind::Source ||
                   node.kind == GraphNode::Kind::Generator ||
                   node.kind == GraphNode::Kind::LayerTransform) {
            if (node.layer_index >= 0)
                record_thumb_tap(
                    rec, frame_index, dst,
                    look.layers[static_cast<size_t>(node.layer_index)].id |
                        kThumbSourceBit);
        }
        for (int input : node.inputs)
            if (--remaining_uses[static_cast<size_t>(input)] == 0)
                pool_.release(results[static_cast<size_t>(input)]);
    }

    // Alpha-bounds reduction on the measure tap: cleared bounds cells,
    // atomic min/max sweep, 16-byte copy-out. Harvested by
    // read_measure_bounds after the caller's fence.
    if (graph.measure >= 0 && results[static_cast<size_t>(graph.measure)]) {
        GpuImage* mimg = results[static_cast<size_t>(graph.measure)];
        mimg->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        bounds_img_->transition(rec, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue cv{};
        for (int i = 0; i < 4; ++i) cv.uint32[i] = 0xFFFFFFFFu;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                                            0, 1};
        vkCmdClearColorImage(rec, bounds_img_->image(),
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1,
                             &range);
        bounds_img_->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
        const uint32_t bpush[2] = {mimg->width(), mimg->height()};
        const GpuImage* msampled[1] = {mimg};
        GpuImage* mstorage[1] = {bounds_img_.get()};
        alpha_bounds_->dispatch(rec, arena_, frame_index, msampled, 1,
                                mstorage, 1, bpush, sizeof(bpush),
                                mimg->width(), mimg->height(),
                                linear_sampler_);
        bounds_img_->transition(rec, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy bcopy{};
        bcopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bcopy.imageExtent = {4, 1, 1};
        vkCmdCopyImageToBuffer(rec, bounds_img_->image(),
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               bounds_buf_, 1, &bcopy);
        bounds_w_ = mimg->width();
        bounds_h_ = mimg->height();
        bounds_recorded_ = true;
        measured_placement_ = measure_placement;
    }

    // The published image: the preview tap when set, the OUTPUT
    // otherwise. The output itself always feeds the Output card's
    // thumbnail (cell key 0) — the preview never touches it.
    GpuImage* out = results[static_cast<size_t>(
        graph.preview >= 0 ? graph.preview : graph.output)];
    record_thumb_tap(rec, frame_index,
                     results[static_cast<size_t>(graph.output)], 0);
    out->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (out_source && graph.before >= 0 &&
        results[static_cast<size_t>(graph.before)]) {
        GpuImage* ref = results[static_cast<size_t>(graph.before)];
        ref->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        *out_source = ref;
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
            cio.pending_frame = cache_frame;
            cio.pending_w = w;
            cio.pending_h = h;
        }
    }
    return out;
}

}  // namespace looks::gfx

#include "gfx/engine.h"

#include <vk_mem_alloc.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "codec/core.h"
#include "doc/effects.h"
#include "gfx/error_diffusion.h"
#include "gfx/graph.h"
#include "gfx/shape_sdf.h"
#include "gfx/vk_device.h"
#include "util/color.h"
#include "util/file.h"
#include "util/hash.h"
#include "util/image.h"
#include "util/log.h"

namespace looks::gfx {

namespace {

// Keep this decoupled from the effect hash so baked assets stay stable.
struct XorShift32 {
    uint32_t s;
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
};

void seeded_shuffle(uint8_t* order, int n, uint32_t seed) {
    XorShift32 rng{seed};
    for (int i = n - 1; i > 0; --i) {
        const int j = static_cast<int>(rng.next() % (i + 1));
        const uint8_t tmpv = order[i];
        order[i] = order[j];
        order[j] = tmpv;
    }
}

// Float params travel in push constants as raw 32-bit words.
uint32_t as_bits(float v) {
    uint32_t b;
    std::memcpy(&b, &v, sizeof(b));
    return b;
}

struct FxShaderDesc {
    const char* spv_name;
    uint32_t sampled_inputs;
};

// Indexed by doc::EffectType; this order must match the enum.
// Push layout: 7-word prelude, then params; a null spv means multi-pass.
constexpr uint32_t kFxPreludeWords = 7;
uint32_t fx_prelude_words(doc::EffectType type) {
    switch (type) {
        case doc::EffectType::Glow:
        case doc::EffectType::CornerSoft:
        case doc::EffectType::Streak:
        case doc::EffectType::Anaglyph:
        case doc::EffectType::SlitScan:
        case doc::EffectType::Jitter:
        case doc::EffectType::Flicker:
        case doc::EffectType::GateMask:
        case doc::EffectType::CueMark:
        case doc::EffectType::FmSynth:
        case doc::EffectType::VelocityScan:
        case doc::EffectType::Feedback:
        case doc::EffectType::Echo:
        case doc::EffectType::Quantize:
        case doc::EffectType::Glyph:
        case doc::EffectType::FlowPaint:
        case doc::EffectType::Kuwahara:
        case doc::EffectType::CelShade:
        case doc::EffectType::Solarize:
        case doc::EffectType::Emboss:
        case doc::EffectType::CamcorderHud:
        case doc::EffectType::VhsOsd:
        case doc::EffectType::Timestamp:
        case doc::EffectType::CamAuto:
        case doc::EffectType::FilmStock:
        case doc::EffectType::Grain:
        case doc::EffectType::Halation:
        case doc::EffectType::Emulsion:
        case doc::EffectType::LightLeak:
        case doc::EffectType::DustScratches:
        case doc::EffectType::FilmSlip:
        case doc::EffectType::SpliceBump:
        case doc::EffectType::Vhs:
        case doc::EffectType::Composite:
        case doc::EffectType::Snow:
        case doc::EffectType::SyncFail:
        case doc::EffectType::HeadSwitch:
        case doc::EffectType::Interlace:
        case doc::EffectType::Aperture:
        case doc::EffectType::Anamorphic:
        case doc::EffectType::RollingShutter:
        case doc::EffectType::LensDistort:
        case doc::EffectType::Fringe:
        case doc::EffectType::LensFlare:
        case doc::EffectType::StarFilter:
        case doc::EffectType::Photocopy:
        case doc::EffectType::Risograph:
        case doc::EffectType::WetPlate:
        case doc::EffectType::Watercolor:
        case doc::EffectType::Halftone:
        case doc::EffectType::CrossHatch:
        case doc::EffectType::Engraver:
        case doc::EffectType::BurnIn:
        case doc::EffectType::ScreenTexture:
        case doc::EffectType::ScopeMonitor:
        case doc::EffectType::AudioScope:
        case doc::EffectType::VectorTrace:
        case doc::EffectType::RuttEtra:
        case doc::EffectType::SlowScan:
        case doc::EffectType::SecurityMux:
        case doc::EffectType::Lidar:
        case doc::EffectType::Mosquito:
        case doc::EffectType::Glass:
        case doc::EffectType::Drip:
        case doc::EffectType::ReactionDiffusion:
            return 10;
        default: return kFxPreludeWords;
    }
}
bool fx_raw_state(doc::EffectType type) {
    return type == doc::EffectType::Drip || type == doc::EffectType::SlowScan ||
           type == doc::EffectType::ScopeMonitor || type == doc::EffectType::BurnIn;
}
bool fx_optical_filter(doc::EffectType type) {
    return type == doc::EffectType::Aperture || type == doc::EffectType::Anamorphic ||
           type == doc::EffectType::Halation || type == doc::EffectType::StarFilter ||
           type == doc::EffectType::LensFlare || type == doc::EffectType::Glow ||
           type == doc::EffectType::CornerSoft;
}
constexpr FxShaderDesc kFxShaders[] = {
    {"fx_rgb_split.comp.spv", 1},
    {"fx_vignette.comp.spv", 1},
    {"fx_pixelate.comp.spv", 1},
    {"fx_grain.comp.spv", 1},
    {"fx_jitter.comp.spv", 1},
    {"fx_quantize.comp.spv", 4},        // input + LUT + RD + flow (lock)
    {"fx_glow_composite.comp.spv", 3},
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
    {"fx_interlace.comp.spv", 2},       // input + previous frame (weave)
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
    {"fx_anamorphic.comp.spv", 2},
    {"fx_direct_flash.comp.spv", 1},
    {"fx_edge_detect.comp.spv", 1},
    {"fx_kuwahara.comp.spv", 1},
    {"fx_cel_shade.comp.spv", 1},
    {"fx_rutt_etra.comp.spv", 1},
    {"fx_slit_scan.comp.spv", 3},
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
    {"fx_crt_sim.comp.spv", 3},
    {"fx_halftone.comp.spv", 1},
    {"fx_star_filter.comp.spv", 2},
    {"fx_streak.comp.spv", 1},
    {"fx_split_tone.comp.spv", 1},
    {"fx_corner_soft.comp.spv", 2},
    {"fx_head_switch.comp.spv", 1},
    {"fx_vhs_osd.comp.spv", 1},
    {"fx_cam_auto.comp.spv", 2},
    {"fx_mosquito.comp.spv", 1},
    {"fx_bit_plane.comp.spv", 1},
    {"fx_block_shuffle.comp.spv", 1},
    {"fx_buffer_glitch.comp.spv", 1},
    {"fx_cross_hatch.comp.spv", 1},
    {"fx_splice_bump.comp.spv", 1},
    {"fx_film_slip.comp.spv", 2},
    {"fx_emulsion.comp.spv", 1},
    {"fx_time_displace.comp.spv", 3},   // input + history slice + map
    {"fx_flow_paint.comp.spv", 2},      // input + flow field
    {"fx_fm_synth.comp.spv", 2},
    {"fx_colorizer.comp.spv", 1},
    {"fx_solarize.comp.spv", 1},
    {"fx_invert.comp.spv", 1},
    {"fx_twirl.comp.spv", 1},
    {"fx_tile.comp.spv", 1},
    {"fx_emboss.comp.spv", 1},
    {"fx_lens_flare.comp.spv", 2},
    {"fx_velocity_scan.comp.spv", 3},   // input + canvas + front field
    {"fx_lidar.comp.spv", 2},           // input + own previous output
    {"fx_anaglyph.comp.spv", 2},
    {"fx_photocopy.comp.spv", 1},
    {"fx_risograph.comp.spv", 1},
    {"fx_wet_plate.comp.spv", 1},
    {"fx_glass.comp.spv", 1},
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
    {"fx_matte.comp.spv", 1},
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
    // Audio modifiers have no kernel; the compiler routes around them.
    {nullptr, 0},
    {nullptr, 0},
    {nullptr, 0},
    {nullptr, 0},
    {nullptr, 0},
    {nullptr, 0},
    // Offset is a time shim; it never dispatches.
    {nullptr, 0},
    {"fx_track_pin.comp.spv", 2},       // input + pinned B
    {"fx_vhs.comp.spv", 1},
    {"fx_morphology.comp.spv", 1},
    {"fx_drip.comp.spv", 2},
    {"fx_burn_in.comp.spv", 2},
    {"fx_aperture.comp.spv", 3},
    {"fx_parallax.comp.spv", 2},
    {"fx_patch_weave.comp.spv", 1},
    {"fx_halation.comp.spv", 2},
    {"fx_rolling_shutter.comp.spv", 2},
    {"fx_normalise.comp.spv", 2},
};
static_assert(sizeof(kFxShaders) / sizeof(kFxShaders[0]) ==
              static_cast<size_t>(doc::EffectType::Count));

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
            mp.mv_rotate = p[6] * doc::kDeg2Rad;
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

// Only gates that withhold the whole parameter response need a signature.
// Gates that withhold state advancement re-read params every render.
uint64_t stateful_param_sig(const doc::EffectInstance& fx, uint64_t seed) {
    return hash_combine(
        seed, fnv1a(fx.params.data(), fx.params.size() * sizeof(float)));
}

}  // namespace

std::unique_ptr<Engine> Engine::create(Device& device,
                                       const std::filesystem::path& shader_dir,
                                       VkQueue submit_queue) {
    auto e = std::unique_ptr<Engine>(new Engine(device));
    e->submit_queue_ =
        submit_queue ? submit_queue : device.graphics_queue();
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
    // The caller waited this slot's fence, so the old buffer is free.
    if (io.buf) vmaDestroyBuffer(device_.allocator(), io.buf, io.alloc);
    io.buf = VK_NULL_HANDLE;
    io.alloc = nullptr;
    io.mapped = nullptr;
    io.capacity = 0;
    if (!create_mapped_buffer(device_, bytes,
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              &io.buf, &io.alloc, &io.mapped))
        return false;
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

    const auto mk = [&](const char* spv, uint32_t inputs, uint32_t outputs,
                        uint32_t push_bytes) {
        ComputePipelineDesc desc;
        desc.spv_name = spv;
        desc.sampled_inputs = inputs;
        desc.storage_outputs = outputs;
        desc.push_bytes = push_bytes;
        return ComputePipeline::create(device_, shader_dir, desc);
    };

    to_rgb_ = mk("ycbcr_to_rgb.comp.spv", 3, 1, 7 * sizeof(uint32_t));
    if (!to_rgb_) return false;

    alpha_bounds_ = mk("alpha_bounds.comp.spv", 1, 1, 2 * sizeof(uint32_t));
    if (!alpha_bounds_) return false;
    bounds_img_ = GpuImage::create(
        device_, VK_FORMAT_R32_UINT, 4, 1,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!bounds_img_) return false;
    if (!create_mapped_buffer(device_, 4 * sizeof(uint32_t),
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT, &bounds_buf_,
                              &bounds_alloc_, &bounds_mapped_))
        return false;

    for (size_t i = 0; i < static_cast<size_t>(doc::EffectType::Count); ++i) {
        if (!kFxShaders[i].spv_name) continue;   // multi-pass, below
        const doc::EffectInfo& info =
            doc::effect_info(static_cast<doc::EffectType>(i));
        ComputePipelineDesc desc;
        desc.spv_name = kFxShaders[i].spv_name;
        desc.sampled_inputs = kFxShaders[i].sampled_inputs;
        desc.storage_outputs = 1;
        // Extra push words: 1 = history-pass index or front-state width,
        // 4 = glyph/text/motion-extract tail, 9 = Track Pin homography.
        const auto type_i = static_cast<doc::EffectType>(i);
        if (fx_raw_state(type_i)) desc.storage_outputs = 2;
        if (type_i == doc::EffectType::ScopeMonitor) desc.storage_outputs = 3;
        const uint32_t extra =
            (type_i == doc::EffectType::SlitScan ||
             type_i == doc::EffectType::TimeDisplace ||
             type_i == doc::EffectType::SecurityMux ||
             type_i == doc::EffectType::VelocityScan)
                ? 1u
                : (type_i == doc::EffectType::Glyph ||
                           type_i == doc::EffectType::Text ||
                           type_i == doc::EffectType::MotionExtract
                       ? 4u
                       : (type_i == doc::EffectType::TrackPin ? 9u :
                          type_i == doc::EffectType::CrtSim ? 3u : 0u));
        desc.push_bytes = static_cast<uint32_t>(
            (fx_prelude_words(type_i) + info.param_count + extra) * sizeof(uint32_t));
        fx_[i] = ComputePipeline::create(device_, shader_dir, desc);
        if (!fx_[i]) return false;
    }

    flow_ = mk("flow.comp.spv", 2, 1, 7 * sizeof(uint32_t));
    if (!flow_) return false;
    const uint32_t crt_push = static_cast<uint32_t>(
        (kFxPreludeWords + doc::effect_info(doc::EffectType::CrtSim).param_count + 3) *
        sizeof(uint32_t));
    crt_prepare_ = mk("fx_crt_prepare.comp.spv", 2, 1, crt_push);
    crt_blur_ = mk("fx_crt_blur.comp.spv", 1, 1, 4 * sizeof(uint32_t));
    if (!crt_prepare_ || !crt_blur_) return false;
    optical_reduce_ = mk("fx_optical_reduce.comp.spv", 1, 1, 5 * sizeof(uint32_t));
    if (!optical_reduce_) return false;
    camera_meter_ = mk("fx_camera_meter.comp.spv", 2, 1, 14 * sizeof(uint32_t));
    if (!camera_meter_) return false;
    normalise_reduce_ = mk("fx_normalise_reduce.comp.spv", 1, 1, 5 * sizeof(uint32_t));
    if (!normalise_reduce_) return false;
    scope_bins_pass_ = mk("fx_scope_bins.comp.spv", 1, 1, 4 * sizeof(uint32_t));
    if (!scope_bins_pass_) return false;

    // Inputs: front state, then the video frame.
    vs_front_ = mk("vs_front.comp.spv", 2, 1, 14 * sizeof(uint32_t));
    if (!vs_front_) return false;

    // numthreads is (8,1,1); dispatch this as (lanes, 1).
    mod_integrate_ = mk("mod_integrate.comp.spv", 1, 1, 7 * sizeof(uint32_t));
    if (!mod_integrate_) return false;

    thumb_tap_ = mk("thumb_tap.comp.spv", 1, 1, 2 * sizeof(uint32_t));
    if (!thumb_tap_) return false;

    gallery_tap_ = mk("gallery_tap.comp.spv", 1, 1, 4 * sizeof(uint32_t));
    if (!gallery_tap_) return false;

    rd_step_ = mk("rd_step.comp.spv", 2, 1, 6 * sizeof(uint32_t));
    if (!rd_step_) return false;

    to_nv12_ = mk("export_nv12.comp.spv", 1, 2, 3 * sizeof(uint32_t));
    fx_mix_ = mk("fx_mix.comp.spv", 2, 1, 4 * sizeof(uint32_t));
    if (!to_nv12_ || !fx_mix_) return false;
    // Full RGBA: wet 1 must return the face exactly.
    group_mix_ = mk("group_mix.comp.spv", 2, 1, 4 * sizeof(uint32_t));
    if (!group_mix_) return false;

    mosh_predict_ = mk("mosh_predict.comp.spv", 1, 6, 16 * sizeof(uint32_t));
    mosh_wire_ = mk("mosh_wire.comp.spv", 2, 6, 12 * sizeof(uint32_t));
    mosh_rate_probe_ =
        mk("mosh_rate_probe.comp.spv", 2, 3, 16 * sizeof(uint32_t));
    mosh_rate_reduce_ =
        mk("mosh_rate_reduce.comp.spv", 0, 2, 6 * sizeof(uint32_t));
    mosh_rate_pick_ =
        mk("mosh_rate_pick.comp.spv", 0, 2, 12 * sizeof(uint32_t));
    if (!mosh_rate_probe_ || !mosh_rate_reduce_ || !mosh_rate_pick_)
        return false;
    ed_expand_ = mk("ed_expand.comp.spv", 0, 2, 24 * sizeof(uint32_t));
    if (!ed_expand_) return false;
    mosh_unorm_ = mk("mosh_to_unorm.comp.spv", 0, 6, 4 * sizeof(uint32_t));
    if (!mosh_predict_ || !mosh_wire_ || !mosh_unorm_) return false;
    dummy_flow_ = GpuImage::create(device_, VK_FORMAT_R16G16B16A16_SFLOAT,
                                   1, 1, VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!dummy_flow_) return false;

    // The generator's one sampled input is the custom-shape SDF.
    generator_ = mk("gen.comp.spv", 2, 1, 24 * sizeof(uint32_t));
    layer_blend_ = mk("layer_blend.comp.spv", 2, 1, 11 * sizeof(uint32_t));
    layer_transform_ =
        mk("layer_transform.comp.spv", 1, 1, 12 * sizeof(uint32_t));
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

    // Atlas layout: 96 tiles of 8x8 on a 16-column R8 grid.
    constexpr uint32_t kAtlasW = 128, kAtlasH = 48;
    const auto build_atlas = [&](int atlas_slot, auto&& fill_tile) -> bool {
        std::vector<uint8_t> pix(kAtlasW * kAtlasH, 0);
        for (uint32_t tile = 0; tile < 96; ++tile) {
            const uint32_t tx = (tile % 16) * 8;
            const uint32_t ty = (tile / 16) * 8;
            fill_tile(pix, tile, tx, ty);
        }
        return set_glyph_atlas_impl(pix.data(), kAtlasW, kAtlasH, 8.0f, 16, 6, atlas_slot, false);
    };

    // Dot area grows with the tile index.
    if (!build_atlas(0, [](std::vector<uint8_t>& pix, uint32_t tile,
                           uint32_t tx, uint32_t ty) {
            const float coverage = static_cast<float>(tile) / 95.0f;
            const float radius = std::sqrt(coverage) * 5.4f;
            for (uint32_t y = 0; y < 8; ++y) {
                for (uint32_t x = 0; x < 8; ++x) {
                    const float dx = static_cast<float>(x) - 3.5f;
                    const float dy = static_cast<float>(y) - 3.5f;
                    const float d = std::sqrt(dx * dx + dy * dy);
                    const float v =
                        std::clamp(radius - d + 0.5f, 0.0f, 1.0f);
                    pix[(ty + y) * kAtlasW + tx + x] =
                        static_cast<uint8_t>(v * 255.0f + 0.5f);
                }
            }
        }))
        return false;


    // In the dust plate, dark marks are the damage.
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
                plate[i] = color::luma709_u8(p[0], p[1], p[2]);
            }
        } else {
            plate.assign(static_cast<size_t>(kDustW) * kDustH, 255);
            XorShift32 rng{0x9E3779B9u};
            auto next = [&rng] { return rng.next(); };
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
            !upload_oneshot(*dust_tex_, plate.data(), 1, pw, ph))
            return false;
    }

    // A missing LUT falls back to deterministic hash noise.
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
            !upload_oneshot(*noise_lut_, lut.data(), 1, kLutW, kLutH))
            return false;
    }

    // Sorted by lowercased filename so the font param index is stable.
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

    glow_columns_ = mk("fx_glow_bright.comp.spv", 1, 2, 5 * sizeof(uint32_t));
    if (!glow_columns_) return false;

    // Matte pair: port-1 wires gate through luma extract + apply.
    matte_extract_ = mk("matte_extract.comp.spv", 1, 1, 2 * sizeof(uint32_t));
    matte_apply_ = mk("matte_apply.comp.spv", 3, 1, 2 * sizeof(uint32_t));

    return matte_extract_ && matte_apply_;
}

bool Engine::ensure_prev_ref(uint32_t width, uint32_t height) {
    if (prev_y_ && prev_y_->width() == width && prev_y_->height() == height)
        return true;
    // The old copy can still be in flight, so do a full sync here.
    device_.wait_idle();
    prev_y_ = GpuImage::create(device_, VK_FORMAT_R8_UNORM, width, height,
                               VK_IMAGE_USAGE_SAMPLED_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    return prev_y_ != nullptr;
}

uint64_t Engine::pin_plane_key(uint64_t asset, float rx, float ry, float rw,
                               float rh) {
    // The quantization must match ensure_plane's match epsilon.
    auto q = [](float v) {
        return static_cast<uint64_t>(
            static_cast<int64_t>(std::lround(v * 1000.0f)) + 100000);
    };
    uint64_t h = hash_combine(0x504C4Eull, asset);
    h = hash_combine(h, q(rx));
    h = hash_combine(h, q(ry));
    h = hash_combine(h, q(rw));
    h = hash_combine(h, q(rh));
    return h;
}

bool Engine::upload_oneshot(GpuImage& dst, const uint8_t* pixels,
                            size_t bytes_per_pixel, uint32_t width,
                            uint32_t height) {
    VkCommandBuffer rec = codec_begin_segment();
    codec_io_.staging->reset();
    if (!codec_io_.staging->upload_image(
            rec, pixels,
            static_cast<size_t>(width) * height * bytes_per_pixel, width,
            dst)) {
        vkEndCommandBuffer(rec);
        return false;
    }
    dst.transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    codec_flush_segment();
    return true;
}

bool Engine::set_glyph_atlas_impl(const uint8_t* pixels, uint32_t width,
                                  uint32_t height, float tile_px,
                                  uint32_t cols, uint32_t rows, int slot,
                                  bool color) {
    if (slot < 0 || slot > 2 || !pixels || !width || !height || !cols || !rows ||
        !std::isfinite(tile_px) || tile_px < 1.0f || std::floor(tile_px) != tile_px ||
        double(cols) * tile_px != width || double(rows) * tile_px != height)
        return false;
    device_.wait_idle();
    const uint32_t tile = static_cast<uint32_t>(tile_px);
    const uint32_t iw = width + cols, ih = height + rows;
    auto uploaded = GpuImage::create(
        device_, VK_FORMAT_R32G32B32A32_SFLOAT, iw, ih,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!uploaded) return false;
    std::vector<float> integral(size_t(iw) * ih * 4, 0.0f);
    for (uint32_t ty = 0; ty < rows; ++ty) {
        for (uint32_t tx = 0; tx < cols; ++tx) {
            for (uint32_t y = 1; y <= tile; ++y) {
                float sum[4] = {};
                for (uint32_t x = 1; x <= tile; ++x) {
                    const size_t source = size_t(ty * tile + y - 1) * width + tx * tile + x - 1;
                    const size_t dest = (size_t(ty * (tile + 1) + y) * iw + tx * (tile + 1) + x) * 4;
                    const float alpha = color ? pixels[source * 4 + 3] / 255.0f : pixels[source] / 255.0f;
                    for (uint32_t c = 0; c < 4; ++c) {
                        sum[c] += color && c < 3 ? color::srgb_eotf(pixels[source * 4 + c] / 255.0f) * alpha : alpha;
                        integral[dest + c] = integral[dest - size_t(iw) * 4 + c] + sum[c];
                    }
                }
            }
        }
    }
    if (!upload_oneshot(*uploaded, reinterpret_cast<const uint8_t*>(integral.data()),
                       sizeof(float) * 4, iw, ih)) return false;
    glyph_atlas_[slot] = std::move(uploaded);
    cache_.clear();
    for (auto& io : cache_io_) io.pending = false;
    glyph_meta_[slot] = {cols, rows, tile_px};
    glyph_atlas_color_[slot] = color;
    return true;
}

bool Engine::set_glyph_atlas(const uint8_t* gray, uint32_t width,
                             uint32_t height, float tile_px, uint32_t cols,
                             uint32_t rows, int slot) {
    return set_glyph_atlas_impl(gray, width, height, tile_px, cols, rows,
                                slot, /*color=*/false);
}

bool Engine::set_glyph_atlas_rgba(const uint8_t* rgba, uint32_t width,
                                  uint32_t height, float tile_px,
                                  uint32_t cols, uint32_t rows, int slot) {
    return set_glyph_atlas_impl(rgba, width, height, tile_px, cols, rows,
                                slot, /*color=*/true);
}

void Engine::set_scope_audio(std::vector<int16_t> mono,
                             uint32_t sample_rate) {
    scope_audio_ = std::move(mono);
    scope_rate_ = scope_audio_.empty() ? 0 : sample_rate;
}

void Engine::codec_planes_to_rgb(VkCommandBuffer rec, uint32_t frame_index,
                                 GpuImage* temp, uint32_t w, uint32_t h) {
    // Push the nv12 word: a short push inherits the last dispatch value.
    struct {
        uint32_t w, h;
        float rx, ry, iw, ih;
        uint32_t nv12;
    } rgb_push = {w,    h,
                  0.0f, 0.0f,
                  1.0f / static_cast<float>(w),
                  1.0f / static_cast<float>(h),
                  0u};
    const GpuImage* planes3[3] = {codec_io_.up_y.get(),
                                  codec_io_.up_u.get(),
                                  codec_io_.up_v.get()};
    to_rgb_->dispatch(rec, arena_, frame_index, planes3, 3, &temp, 1,
                      &rgb_push, sizeof(rgb_push), w, h, linear_sampler_);
}

void Engine::mix_composite_release(VkCommandBuffer rec, uint32_t frame_index,
                                   const GpuImage* in, GpuImage* temp,
                                   GpuImage* dst, float wet, float opacity,
                                   uint32_t w, uint32_t h) {
    temp->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const uint32_t mix_push[4] = {w, h, as_bits(wet), as_bits(opacity)};
    const GpuImage* sampled2[2] = {in, temp};
    fx_mix_->dispatch(rec, arena_, frame_index, sampled2, 2, &dst, 1,
                      mix_push, sizeof(mix_push), w, h, linear_sampler_);
    pool_->release(temp);
}

bool Engine::mosh_gpu_box(VkCommandBuffer rec, const doc::EffectInstance& fx,
                          uint64_t state_key, uint64_t revision,
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
            !make3(g.pred_moshed))
            return false;
        g.w = w;
        g.h = h;
        g.has_state = false;
        g.last_frame = 0xFFFFFFFFu;
    }
    // State advances once per timeline frame; paused re-renders reuse it.
    // A paused param edit re-arms as a discontinuity.
    const uint64_t sig = hash_combine(stateful_param_sig(fx, mp.seed), revision);
    if (g.has_state && g.last_frame == timeline_frame && g.sig != sig)
        g.has_state = false;
    const bool advance = !g.has_state || g.last_frame != timeline_frame;

    const auto barrier = [&] {
        memory_barrier(rec, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    };
    const auto transition3 = [&](std::unique_ptr<GpuImage>* trio) {
        for (int i = 0; i < 3; ++i)
            trio[i]->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
    };
    transition3(g.clean);
    transition3(g.moshed);
    transition3(g.pred_clean);
    transition3(g.pred_moshed);
    // This orders reads against the previous submission's state writes.
    barrier();

    if (advance) {
        dispatch_to_nv12(rec, frame_index, in, w, h);
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

        // The wire reads the picked rung from qsel; rate control stays on GPU.
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
            clear_color(rec, *codec_io_.rate_bits, VK_IMAGE_LAYOUT_GENERAL, {});
            memory_barrier(rec, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT |
                               VK_ACCESS_SHADER_WRITE_BIT);
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
            if (rate_on) run_rate(0, 1);
            for (int p = 0; p < 3; ++p)
                wire(planes[p], 0, static_cast<uint32_t>(quality), 0, 0,
                     rate_on ? 1u : 0u, g.clean[p].get(), g.clean[p].get(),
                     g.clean[p].get(), g.clean[p].get(), g.clean[p].get());
            for (int gi = 1; gi < std::min(mp.generations, 12); ++gi) {
                const int gq = std::clamp(mp.quality, 1, 100);
                barrier();
                for (int p = 0; p < 3; ++p)
                    wire(planes[p], 0, static_cast<uint32_t>(gq), 0, 1, 0,
                         g.clean[p].get(), g.clean[p].get(),
                         g.clean[p].get(), g.clean[p].get(),
                         g.clean[p].get());
            }
            if (!g.has_state || !mp.drop_iframes) {
                memory_barrier(rec, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_ACCESS_SHADER_WRITE_BIT,
                               VK_ACCESS_TRANSFER_READ_BIT);
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
                memory_barrier(rec, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_ACCESS_TRANSFER_WRITE_BIT,
                               VK_ACCESS_SHADER_READ_BIT |
                                   VK_ACCESS_SHADER_WRITE_BIT);
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
            barrier();
            if (rate_on) run_rate(1, 0);
            for (int repeat = 0; repeat <= std::clamp(mp.p_repeat, 0, 8); ++repeat) {
                if (repeat > 0) {
                    barrier();
                    predict(1, g.moshed, g.pred_moshed);
                    barrier();
                }
                for (int p = 0; p < 3; ++p)
                    wire(planes[p], 1, static_cast<uint32_t>(quality), 0, 0,
                         rate_on ? 1u : 0u, g.pred_clean[p].get(),
                         g.pred_clean[p].get(), g.pred_moshed[p].get(),
                         g.clean[p].get(), g.moshed[p].get());
            }
        }
        g.has_state = true;
        g.last_frame = timeline_frame;
        g.sig = sig;
    }

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
    GpuImage* temp = pool_->acquire(w, h);
    if (!temp) return false;
    temp->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
    codec_planes_to_rgb(rec, frame_index, temp, w, h);
    mix_composite_release(rec, frame_index, in, temp, dst, fx.wet,
                          fx.opacity, w, h);
    return true;
}

bool Engine::composite_ed(VkCommandBuffer rec, const doc::EffectInstance& fx,
                          const GpuImage* in_img, const EdState& ed,
                          uint32_t w, uint32_t h, uint32_t frame_index,
                          GpuImage* dst) {
    // The picks upload packed as 3x5 bits per pixel.
    const size_t px_count = static_cast<size_t>(w) * h;
    if (!codec_io_.staging->upload_image(rec, ed.out.data(), px_count * 4, w,
                                         *codec_io_.up_idx))
        return false;
    codec_io_.up_idx->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
    GpuImage* temp = pool_->acquire(w, h);
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
    mix_composite_release(rec, frame_index, in_img, temp, dst, fx.wet,
                          fx.opacity, w, h);
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
    // Rate scratch: 8 rung columns of per-block DCs on the luma grid.
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
        if (!create_mapped_buffer(device_, needed,
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  &codec_io_.readback,
                                  &codec_io_.readback_alloc,
                                  &codec_io_.mapped)) {
            codec_io_.capacity = 0;
            return false;
        }
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
    submit_and_wait(device_, submit_queue_, codec_io_.cmd, codec_io_.fence,
                    "codecbox submit");
}

void Engine::dispatch_to_nv12(VkCommandBuffer rec, uint32_t frame_index,
                              const GpuImage* in, uint32_t w, uint32_t h) {
    codec_io_.nv_y->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
    codec_io_.nv_uv->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
    const uint32_t nv_push[3] = {w, h, 1};
    const GpuImage* sampled[1] = {in};
    GpuImage* storage[2] = {codec_io_.nv_y.get(), codec_io_.nv_uv.get()};
    to_nv12_->dispatch(rec, arena_, frame_index, sampled, 1, storage, 2,
                       nv_push, sizeof(nv_push), w, h, linear_sampler_);
}

// Taps past the fixed grid drop silently.
namespace {

void hsl_of(const float rgb[3], float* h, float* s, float* l) {
    const float mx = std::max(rgb[0], std::max(rgb[1], rgb[2]));
    const float mn = std::min(rgb[0], std::min(rgb[1], rgb[2]));
    const float d = mx - mn;
    *l = (mx + mn) * 0.5f;
    *s = d < 1e-6f ? 0.0f
                   : d / (1.0f - std::fabs(2.0f * *l - 1.0f) + 1e-6f);
    if (d < 1e-6f) {
        *h = 0.0f;
    } else if (mx == rgb[0]) {
        *h = std::fmod((rgb[1] - rgb[2]) / d, 6.0f) / 6.0f;
    } else if (mx == rgb[1]) {
        *h = ((rgb[2] - rgb[0]) / d + 2.0f) / 6.0f;
    } else {
        *h = ((rgb[0] - rgb[1]) / d + 4.0f) / 6.0f;
    }
    if (*h < 0.0f) *h += 1.0f;
}

void oklab_of(const float lin[3], float* out) {
    const float l = 0.4122214708f * lin[0] + 0.5363325363f * lin[1] +
                    0.0514459929f * lin[2];
    const float m = 0.2119034982f * lin[0] + 0.6806995451f * lin[1] +
                    0.1073969566f * lin[2];
    const float s = 0.0883024619f * lin[0] + 0.2817188376f * lin[1] +
                    0.6299787005f * lin[2];
    const float l_ = std::cbrt(l), m_ = std::cbrt(m), s_ = std::cbrt(s);
    out[0] = 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_;
    out[1] = 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_;
    out[2] = 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_;
}

void oklab_to_linear(const float lab[3], float* out) {
    const float l_ = lab[0] + 0.3963377774f * lab[1] + 0.2158037573f * lab[2];
    const float m_ = lab[0] - 0.1055613458f * lab[1] - 0.0638541728f * lab[2];
    const float s_ = lab[0] - 0.0894841775f * lab[1] - 1.2914855480f * lab[2];
    const float l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
    out[0] = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    out[1] = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    out[2] = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
}

float mesh_stop_spacing(const doc::Layer& layer, uint32_t w, uint32_t h) {
    const size_t n = layer.stops.size();
    if (n < 2) return 1.0f;
    const float aspect = static_cast<float>(w) /
                         std::max(1.0f, static_cast<float>(h));
    float total = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        float nearest = 1e30f;
        for (size_t j = 0; j < n; ++j) {
            if (j == i) continue;
            const float dx =
                (layer.stops[i].x - layer.stops[j].x) * aspect;
            const float dy = layer.stops[i].y - layer.stops[j].y;
            nearest = std::min(nearest, dx * dx + dy * dy);
        }
        total += std::sqrt(nearest);
    }
    return std::max(1e-3f, total / static_cast<float>(n));
}

}  // namespace

const GpuImage* Engine::ensure_gradient_ramp(VkCommandBuffer rec,
                                             StagingBuffer& staging,
                                             const doc::Layer& layer) {
    if (layer.stops.empty()) return nullptr;
    uint64_t hash = hash_combine(0x6A0Dull,
                                 static_cast<uint64_t>(layer.gradient_space));
    hash = hash_combine(hash, static_cast<uint64_t>(layer.gradient));
    hash = hash_combine(hash, as_bits(layer.gradient_len));
    hash = hash_combine(hash, as_bits(layer.gen_angle));
    hash = hash_combine(hash, as_bits(layer.gradient_x));
    hash = hash_combine(hash, as_bits(layer.gradient_y));
    for (const doc::GradientStop& s : layer.stops) {
        hash = hash_combine(hash, as_bits(std::clamp(s.t, 0.0f, 1.0f)));
        hash = hash_combine(hash, as_bits(s.x));
        hash = hash_combine(hash, as_bits(s.y));
        for (float c : s.color) hash = hash_combine(hash, as_bits(c));
    }

    RampSlot& slot = ramp_state_[layer.id];
    if (slot.tex && slot.hash == hash) return slot.tex.get();
    // Row 0 is the axial ramp, keyed on where each stop projects onto the
    // axis. Row 1 is the raw stop table that Mesh weights per pixel.
    std::vector<std::pair<float, doc::GradientStop>> stops;
    stops.reserve(layer.stops.size());
    for (const doc::GradientStop& s : layer.stops)
        stops.emplace_back(std::clamp(s.t, 0.0f, 1.0f), s);
    std::sort(stops.begin(), stops.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    if (!slot.tex) {
        slot.tex = GpuImage::create(
            device_, VK_FORMAT_R32G32B32A32_SFLOAT, kRampTexels, 2,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (!slot.tex) return nullptr;
    }

    // Stops hold sRGB, like every other colour the UI writes. Convert to
    // the blend space, interpolate, and leave the ramp linear.
    const doc::GradientSpace space = layer.gradient_space;
    auto encode = [&](const doc::GradientStop& s, float* out) {
        float lin[3];
        for (int i = 0; i < 3; ++i) lin[i] = color::srgb_eotf(s.color[i]);
        if (space == doc::GradientSpace::Hsl)
            hsl_of(s.color, &out[0], &out[1], &out[2]);
        else if (space == doc::GradientSpace::Oklab)
            oklab_of(lin, out);
        else
            for (int i = 0; i < 3; ++i) out[i] = lin[i];
    };
    auto decode = [&](const float* v, float* out) {
        if (space == doc::GradientSpace::Hsl) {
            float srgb[3];
            color::hsl_to_rgb(v[0], v[1], v[2], srgb);
            for (int i = 0; i < 3; ++i) out[i] = color::srgb_eotf(srgb[i]);
        } else if (space == doc::GradientSpace::Oklab) {
            oklab_to_linear(v, out);
        } else {
            for (int i = 0; i < 3; ++i) out[i] = v[i];
        }
    };

    std::vector<float> texels(kRampTexels * 2 * 4);
    for (uint32_t i = 0; i < kRampTexels; ++i) {
        const float t = (static_cast<float>(i) + 0.5f) / kRampTexels;
        size_t hi = 0;
        while (hi < stops.size() && stops[hi].first < t) ++hi;
        float mixed[3];
        float mixed_a;
        if (hi == 0) {
            encode(stops.front().second, mixed);
            mixed_a = stops.front().second.color[3];
        } else if (hi >= stops.size()) {
            encode(stops.back().second, mixed);
            mixed_a = stops.back().second.color[3];
        } else {
            const doc::GradientStop& a = stops[hi - 1].second;
            const doc::GradientStop& b = stops[hi].second;
            const float span = stops[hi].first - stops[hi - 1].first;
            const float f =
                span > 1e-6f ? (t - stops[hi - 1].first) / span : 0.0f;
            float ea[3], eb[3];
            encode(a, ea);
            encode(b, eb);
            mixed_a = a.color[3] + (b.color[3] - a.color[3]) * f;
            if (space == doc::GradientSpace::Hsl) {
                // Take the short way round the hue circle.
                float d = eb[0] - ea[0];
                if (d > 0.5f) d -= 1.0f;
                if (d < -0.5f) d += 1.0f;
                mixed[0] = ea[0] + d * f;
                if (mixed[0] < 0.0f) mixed[0] += 1.0f;
                if (mixed[0] > 1.0f) mixed[0] -= 1.0f;
                mixed[1] = ea[1] + (eb[1] - ea[1]) * f;
                mixed[2] = ea[2] + (eb[2] - ea[2]) * f;
            } else {
                for (int k = 0; k < 3; ++k)
                    mixed[k] = ea[k] + (eb[k] - ea[k]) * f;
            }
        }
        float lin[3];
        decode(mixed, lin);
        for (int k = 0; k < 3; ++k)
            texels[i * 4 + k] = std::max(0.0f, lin[k]);
        texels[i * 4 + 3] = std::clamp(mixed_a, 0.0f, 1.0f);
    }

    // Row 1: two texels per stop, position then blend-space colour.
    const size_t row1 = static_cast<size_t>(kRampTexels) * 4;
    const size_t n_mesh = std::min<size_t>(stops.size(), kRampTexels / 2);
    for (size_t i = 0; i < n_mesh; ++i) {
        const doc::GradientStop& s = stops[i].second;
        texels[row1 + i * 8 + 0] = s.x;
        texels[row1 + i * 8 + 1] = s.y;
        float enc[3];
        encode(s, enc);
        for (int k = 0; k < 3; ++k) texels[row1 + i * 8 + 4 + k] = enc[k];
        texels[row1 + i * 8 + 7] = std::clamp(s.color[3], 0.0f, 1.0f);
    }

    if (!staging.upload_image(rec, texels.data(),
                              texels.size() * sizeof(float), kRampTexels,
                              *slot.tex))
        return nullptr;
    slot.tex->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    slot.hash = hash;
    return slot.tex.get();
}

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

void Engine::record_gallery_tap(VkCommandBuffer rec, uint32_t frame_index,
                                GpuImage* src, uint32_t cell) {
    if (!gallery_tap_ || !src) return;
    if (cell >= kGalleryCols * kGalleryRows) return;
    if (!gallery_atlas_) {
        gallery_atlas_ = GpuImage::create(
            device_, VK_FORMAT_R16G16B16A16_SFLOAT,
            kGalleryCellW * kGalleryCols, kGalleryCellH * kGalleryRows,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        if (!gallery_atlas_) return;
    }
    src->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    gallery_atlas_->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
    const uint32_t push[4] = {cell % kGalleryCols, cell / kGalleryCols,
                              src->width(), src->height()};
    const GpuImage* sampled[1] = {src};
    GpuImage* storage[1] = {gallery_atlas_.get()};
    gallery_tap_->dispatch(rec, arena_, frame_index, sampled, 1, storage,
                           1, push, sizeof(push), kGalleryCellW,
                           kGalleryCellH, linear_sampler_);
}

bool Engine::measure_recorded() const { return bounds_recorded_; }

bool Engine::read_measure_bounds(float rect[4]) const {
    // No bounds_recorded_ gate here: the caller's snapshot owns that check.
    // The untouched-clear sentinel below still rejects garbage.
    if (!bounds_mapped_ || !bounds_w_ || !bounds_h_)
        return false;
    vmaInvalidateAllocation(device_.allocator(), bounds_alloc_, 0,
                            VK_WHOLE_SIZE);
    uint32_t v[4];
    std::memcpy(v, bounds_mapped_, sizeof(v));
    // The max cells hold the complement, so one atomic min serves all four.
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

Engine::FeedbackSlot* Engine::ensure_feedback_prev(VkCommandBuffer rec,
                                                   uint64_t skey, uint32_t w,
                                                   uint32_t h, VkFormat format) {
    FeedbackSlot& slot = feedback_state_[skey];
    if (!slot.prev || !slot.current || slot.prev->width() != w ||
        slot.prev->height() != h) {
        if (slot.prev) retired_images_[render_slot_].push_back(std::move(slot.prev));
        if (slot.current) retired_images_[render_slot_].push_back(std::move(slot.current));
        slot.prev = GpuImage::create(
            device_, format, w, h,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        slot.current = GpuImage::create(
            device_, format, w, h,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        slot.last_frame = 0xFFFFFFFFu;
        if (!slot.prev || !slot.current) return nullptr;
    }
    if (slot.last_frame != 0xFFFFFFFFu && effect_frame_ == slot.last_frame + 1) {
        std::swap(slot.prev, slot.current);
        slot.valid = true;
    } else if (slot.last_frame == 0xFFFFFFFFu || effect_frame_ != slot.last_frame) {
        clear_color(rec, *slot.prev, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, {});
        slot.valid = false;
    }
    slot.last_frame = effect_frame_;
    slot.prev->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return &slot;
}

void Engine::feedback_writeback(VkCommandBuffer rec, FeedbackSlot& slot,
                                GpuImage* dst, uint32_t w, uint32_t h,
                                uint32_t timeline_frame) {
    copy_full(rec, *dst, *slot.current, w, h);
    slot.current->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    dst->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
    slot.last_frame = timeline_frame;
}

Engine::SlitSlot& Engine::slit_ring_slot(uint64_t skey, uint32_t w,
                                         uint32_t h) {
    SlitSlot& slot = slit_state_[skey];
    if (slot.ring[0] &&
        (slot.ring[0]->width() != w || slot.ring[0]->height() != h)) {
        for (auto& img : slot.ring)
            if (img) retired_images_[render_slot_].push_back(std::move(img));
        slot.head = slot.count = 0;
        slot.last_frame = 0xFFFFFFFFu;
        slot.read_frame = 0xFFFFFFFFu;
    }
    if (slot.read_frame != effect_frame_) {
        if (slot.last_frame == 0xFFFFFFFFu || effect_frame_ != slot.last_frame + 1)
            slot.count = 0;
        slot.read_head = slot.head;
        slot.read_count = std::min(slot.count, kSlitRing - 1);
        slot.read_frame = effect_frame_;
    }
    return slot;
}

bool Engine::slit_ring_push(VkCommandBuffer rec, SlitSlot& slot,
                            GpuImage* in_img, uint32_t w, uint32_t h,
                            uint32_t timeline_frame) {
    if (slot.last_frame == timeline_frame) return true;
    std::unique_ptr<GpuImage>& target = slot.ring[slot.head];
    if (!target) {
        target = GpuImage::create(
            device_, VK_FORMAT_R16G16B16A16_SFLOAT, w, h,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (!target) return false;
    }
    copy_full(rec, *in_img, *target, w, h);
    target->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    in_img->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    slot.head = (slot.head + 1) % kSlitRing;
    slot.count = std::min(slot.count + 1, kSlitRing);
    slot.last_frame = timeline_frame;
    return true;
}

const GpuImage* Engine::slit_history(const SlitSlot& slot, GpuImage* in_img,
                                     uint32_t age) {
    if (age == 0 || age > slot.read_count) return in_img;
    const uint32_t idx = (slot.read_head + kSlitRing - age) % kSlitRing;
    return slot.ring[idx] ? slot.ring[idx].get() : in_img;
}

GpuImage* Engine::rd_advance(VkCommandBuffer rec, uint32_t frame_index,
                             uint64_t skey, const GpuImage* in, uint32_t w,
                             uint32_t h, uint32_t timeline_frame,
                             uint32_t steps, float feed, float kill,
                             float inject, float fps) {
    RdSlot& slot = rd_state_[skey];
    if (slot.state[0] &&
        (slot.state[0]->width() != w || slot.state[0]->height() != h)) {
        for (auto& img : slot.state)
            if (img) retired_images_[render_slot_].push_back(std::move(img));
        slot.last_frame = 0xFFFFFFFFu;
    }
    if (!slot.state[0]) {
        for (int s = 0; s < 2; ++s) {
            slot.state[s] = GpuImage::create(
                device_, VK_FORMAT_R16G16B16A16_SFLOAT, w, h,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            if (!slot.state[s]) return nullptr;
            // A = 1, B = 0 everywhere: the quiescent state.
            clear_color(rec, *slot.state[s],
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VkClearColorValue{{1.0f, 0.0f, 0.0f, 1.0f}});
        }
        slot.cur = 0;
    }
    if (slot.last_frame != timeline_frame) {
        if (slot.last_frame != 0xFFFFFFFFu && timeline_frame != slot.last_frame + 1) {
            VkClearColorValue initial{};
            initial.float32[0] = initial.float32[3] = 1.0f;
            for (auto& img : slot.state)
                clear_color(rec, *img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, initial);
        }
        const float duration = static_cast<float>(steps) * 60.0f / std::max(fps, 1.0f);
        const uint32_t substeps = static_cast<uint32_t>(std::ceil(duration));
        const uint32_t rd_push[6] = {w, h, as_bits(feed), as_bits(kill),
            as_bits(inject / static_cast<float>(std::max(steps, 1u))),
            as_bits(duration / static_cast<float>(std::max(substeps, 1u)))};
        for (uint32_t s = 0; s < substeps; ++s) {
            GpuImage* src = slot.state[slot.cur].get();
            GpuImage* next = slot.state[1 - slot.cur].get();
            src->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            next->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
            const GpuImage* sampled[2] = {src, in};
            GpuImage* outs = next;
            rd_step_->dispatch(rec, arena_, frame_index, sampled, 2, &outs,
                               1, rd_push, sizeof(rd_push), w, h,
                               linear_sampler_);
            slot.cur = 1 - slot.cur;
        }
        slot.last_frame = timeline_frame;
    }
    GpuImage* st = slot.state[slot.cur].get();
    st->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return st;
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
                         uint64_t measure_placement, bool cache_store) {
    bounds_recorded_ = false;
    // Only the root instance's clock drives caching and prev-frame tracking.
    if (out_source) *out_source = nullptr;
    if (canvas_w == 0 || canvas_h == 0) return nullptr;

    arena_.reset(frame_index);
    pool_ = &pools_[frame_index % kFramesInFlight];
    pool_->release_all();
    render_slot_ = frame_index % kFramesInFlight;
    retired_images_[render_slot_].clear();
    StagingBuffer& staging = *staging_[frame_index % kFramesInFlight];
    staging.reset();

    // The canvas is the project's, so a cut must not resize the graph.
    // The proxy shrinks the working size; even dims keep the codec paths safe.
    const uint32_t w = even_down(canvas_w, preview_divisor_);
    const uint32_t h = even_down(canvas_h, preview_divisor_);

    // Harvest the readback this slot recorded kFramesInFlight renders ago.
    // The caller waited the slot's fence, so the copy is complete.
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
        // A newly-selected block needs one evaluated graph to measure.
        const bool need_measure =
            measure_placement && measure_placement != measured_placement_;
        const RenderCache::Frame* hit =
            need_measure ? nullptr : cache_.find(cache_frame);
        if (hit && hit->width == w && hit->height == h) {
            const size_t bytes = static_cast<size_t>(w) * h * 8;
            GpuImage* dst = nullptr;
            if (ensure_cache_io(cio, bytes) &&
                (dst = pool_->acquire(w, h)) != nullptr) {
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
                // The GPU planes no longer track the playhead, so re-sync.
                have_last_frame_ = false;
                return dst;
            }
        }
        arm_readback = cache_store;
    }

    const RenderGraph graph =
        compile_graph(doc, root_id, root_frame, preview_node, preview_layer,
                      measure_placement, out_source != nullptr);
    if (!graph.valid) {
        log_error("engine: render graph invalid (cycle?)");
        return nullptr;
    }
    // A node's subject lives in its own instance's look.
    // A sequence instance resolves to the first look, like Document::look.
    std::vector<const doc::Look*> inst_look(graph.instances.size());
    for (size_t i = 0; i < graph.instances.size(); ++i)
        inst_look[i] = &doc.look(graph.instances[i].look);
    auto node_look = [&](const GraphNode& n) -> const doc::Look& {
        return *inst_look[static_cast<size_t>(n.instance)];
    };

    // Codec-Box nodes evaluate in fenced segments on an internal buffer.
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
    // Reset staging once per render only; a mid-frame reset clobbers
    // uploads that the same segment still needs.
    if (segmented) codec_io_.staging->reset();

    // The reference source anchors the motion field and the prev luma.
    const uint64_t ref_key =
        graph.source >= 0
            ? graph.nodes[static_cast<size_t>(graph.source)].key
            : 0;

    // Copy the reference luma before the new upload overwrites its planes.
    // Copy only when the timeline advanced and the source is unchanged.
    const auto ref_it = ref_key ? layer_planes_.find(ref_key)
                                : layer_planes_.end();
    const LayerPlanes* ref_last =
        ref_it != layer_planes_.end() ? &ref_it->second : nullptr;
    // A reference that changes size gets new planes below, so the copy
    // would read a freed image.
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
        copy_full(rec, *ref_last->y, *prev_y_, ref_last->width,
                  ref_last->height);
        prev_y_->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    } else {
        prev_frame_valid_ = false;
    }
    last_timeline_frame_ = root_frame;
    last_ref_key_ = ref_key;
    have_last_frame_ = true;

    // Media sources: one I420 upload per PLACEMENT, keyed per instance.
    for (size_t i = 0; i < layer_source_count; ++i) {
        const LayerSourceFrame& lf = layer_sources[i];
        if (!lf.planes.y || !lf.planes.u ||
            (!lf.planes.nv12 && !lf.planes.v) || lf.planes.width == 0 ||
            lf.key == 0)
            continue;
        LayerPlanes& lp = layer_planes_[lf.key];
        if (lp.width != lf.planes.width || lp.height != lf.planes.height ||
            lp.nv12 != lf.planes.nv12) {
            device_.wait_idle();
            const VkImageUsageFlags lu =
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            const uint32_t lcw = (lf.planes.width + 1) / 2;
            const uint32_t lch = (lf.planes.height + 1) / 2;
            // Y also feeds the prev-luma copy, so it needs TRANSFER_SRC.
            lp.y = GpuImage::create(device_, VK_FORMAT_R8_UNORM,
                                    lf.planes.width, lf.planes.height,
                                    lu | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            if (lf.planes.nv12) {
                lp.u = GpuImage::create(device_, VK_FORMAT_R8G8_UNORM, lcw,
                                        lch, lu);
                lp.v.reset();
                if (!lp.y || !lp.u) return nullptr;
            } else {
                lp.u = GpuImage::create(device_, VK_FORMAT_R8_UNORM, lcw,
                                        lch, lu);
                lp.v = GpuImage::create(device_, VK_FORMAT_R8_UNORM, lcw,
                                        lch, lu);
                if (!lp.y || !lp.u || !lp.v) return nullptr;
            }
            lp.width = lf.planes.width;
            lp.height = lf.planes.height;
            lp.nv12 = lf.planes.nv12;
            lp.stamp = 0;
        }
        // A repeated stamp holds the same pixels, so skip the upload.
        // Layouts still normalize: the copy above can leave Y in TRANSFER_SRC.
        if (lp.stamp != 0 && lp.stamp == lf.content_stamp) {
            lp.y->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            lp.u->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            if (lp.v)
                lp.v->transition(rec,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            continue;
        }
        const uint32_t lch = (lp.height + 1) / 2;
        if (!staging.upload_image(rec, lf.planes.y,
                                  lf.planes.y_stride * lp.height,
                                  lf.planes.y_stride, *lp.y))
            return nullptr;
        if (lp.nv12) {
            // bufferRowLength is in texels, and each is two bytes.
            if (!staging.upload_image(rec, lf.planes.u,
                                      lf.planes.u_stride * lch,
                                      lf.planes.u_stride / 2, *lp.u))
                return nullptr;
        } else if (!staging.upload_image(rec, lf.planes.u,
                                         lf.planes.u_stride * lch,
                                         lf.planes.u_stride, *lp.u) ||
                   !staging.upload_image(rec, lf.planes.v,
                                         lf.planes.v_stride * lch,
                                         lf.planes.v_stride, *lp.v)) {
            return nullptr;
        }
        lp.stamp = lf.content_stamp;
        lp.y->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        lp.u->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (lp.v)
            lp.v->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    // With no media source, a cleared 1x1 plane makes the motion zero.
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
        clear_color(rec, *dummy_y_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, {});
        ref_plane = dummy_y_.get();
        prev_frame_valid_ = false;
    }
    ref_plane->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // On a seek, first frame or cut, prev = current, so motion is zero.
    GpuImage* prev_plane = prev_frame_valid_ ? prev_y_.get() : ref_plane;

    {
        // The strip depends on the params and the root frame only, so
        // every instance of one effect shares it. Upload once per id.
        std::vector<uint64_t> uploaded;
        auto upload_strip = [&](const doc::EffectInstance& fx) -> bool {
            if (fx.type != doc::EffectType::AudioScope || fx.bypass)
                return true;
            for (uint64_t id : uploaded)
                if (id == fx.id) return true;
            uploaded.push_back(fx.id);
            std::unique_ptr<GpuImage>& tex = audio_strip_[fx.id];
            if (!tex) {
                tex = GpuImage::create(
                    device_, VK_FORMAT_R32G32B32A32_SFLOAT, kAudioStripBins, 1,
                    VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT);
                if (!tex) return false;
            }
            float bins[kAudioStripBins * 4] = {};
            const float window =
                fx.params.empty()
                    ? 0.5f
                    : std::clamp(fx.params[0], 0.05f, 2.0f);
            if (scope_rate_ > 0 && !scope_audio_.empty() && fps > 0.0) {
                const double t1 = root_frame / fps;
                double t0 = t1 - window;
                const int64_t total =
                    static_cast<int64_t>(scope_audio_.size());
                if (fx.params.size() > 7 && fx.params[7] > 0.5f) {
                    const int64_t start = std::max<int64_t>(1, static_cast<int64_t>(t0 * scope_rate_));
                    const int64_t end = std::min<int64_t>(total, start + static_cast<int64_t>(window * scope_rate_ * 0.1));
                    for (int64_t s = start; s < end; ++s) {
                        if (scope_audio_[s - 1] <= 0 && scope_audio_[s] > 0) {
                            t0 = static_cast<double>(s) / scope_rate_;
                            break;
                        }
                    }
                }
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
                    bins[i * 4] = mn;
                    bins[i * 4 + 1] = mx;
                    bins[i * 4 + 2] = s0 >= 0 && s0 < total ? scope_audio_[s0] / 32768.0f : 0.0f;
                    bins[i * 4 + 3] = s1 >= 0 && s1 < total ? scope_audio_[s1] / 32768.0f : 0.0f;
                }
            }
            if (!staging.upload_image(rec, bins, sizeof(bins),
                                      kAudioStripBins, *tex))
                return false;
            tex->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            return true;
        };
        for (size_t ii = 0; ii < graph.instances.size(); ++ii)
        for (const doc::Layer& layer : inst_look[ii]->layers)
            for (const doc::EffectInstance& fx : layer.stack)
                if (!upload_strip(fx)) return nullptr;
    }

    // The cell map resets only when the graph evaluates; a hit keeps it.
    if (thumb_tap_ && !thumb_atlas_)
        thumb_atlas_ = GpuImage::create(
            device_, VK_FORMAT_R16G16B16A16_SFLOAT,
            kThumbCellW * kThumbGridCols,
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
    // These extra uses keep the published images alive to the tail.
    remaining_uses[static_cast<size_t>(graph.output)]++;
    if (graph.preview >= 0)
        remaining_uses[static_cast<size_t>(graph.preview)]++;
    if (out_source && graph.before >= 0)
        remaining_uses[static_cast<size_t>(graph.before)]++;
    if (graph.measure >= 0)
        remaining_uses[static_cast<size_t>(graph.measure)]++;
    for (int index : graph.order) {
        const GraphNode& node = graph.nodes[static_cast<size_t>(index)];
        // look and timeline_frame below are per instance and shadow the root.
        // skey is the instance-scoped state key, never the bare effect id.
        const LookInstance& linst =
            graph.instances[static_cast<size_t>(node.instance)];
        const doc::Look& look = *inst_look[static_cast<size_t>(node.instance)];
        const uint32_t timeline_frame = linst.local_frame;
        const uint64_t skey = node.key;
        for (int input : node.inputs)
            results[static_cast<size_t>(input)]->transition(
                rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        // Flow lives at block resolution; everything else at frame size.
        const bool is_flow = node.kind == GraphNode::Kind::Flow;
        GpuImage* dst = is_flow
            ? pool_->acquire((w + 15) / 16, (h + 15) / 16)
            : pool_->acquire(w, h);
        if (!dst) return nullptr;
        dst->transition(rec, VK_IMAGE_LAYOUT_GENERAL);

        auto input_image = [&](size_t i) {
            return results[static_cast<size_t>(node.inputs[i])];
        };

        switch (node.kind) {
            case GraphNode::Kind::Source: {
                // A key with no frame reads black, never another layer.
                auto it = layer_planes_.find(skey);
                if (it == layer_planes_.end() || !it->second.y) {
                    clear_color(rec, *dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                {});
                    break;
                }
                // NV12 binds one chroma texture to both slots; Cr is in .g.
                const GpuImage* planes[3] = {
                    it->second.y.get(), it->second.u.get(),
                    it->second.v ? it->second.v.get()
                                 : it->second.u.get()};
                // The media fits centered and aspect-preserved.
                float fit[4];
                source_fit_rect(it->second.y->width(),
                                it->second.y->height(), w, h, fit,
                                look.layers[node.layer_index].source == doc::LayerSourceKind::Slideshow
                                    ? look.layers[node.layer_index].slide_fit : 0);
                struct {
                    uint32_t w, h;
                    float rx, ry, iw, ih;
                    uint32_t nv12;
                } push = {w,      h,
                          fit[0], fit[1],
                          1.0f / std::max(fit[2], 1.0f),
                          1.0f / std::max(fit[3], 1.0f),
                          it->second.nv12 ? 1u : 0u};
                to_rgb_->dispatch(rec, arena_, frame_index, planes, 3, &dst, 1,
                                  &push, sizeof(push), w, h, linear_sampler_);
                break;
            }
            case GraphNode::Kind::LayerTransform: {
                // Look layers only; sequence Motion runs in the lane blend.
                const doc::Layer& layer =
                    look.layers[static_cast<size_t>(node.layer_index)];
                uint32_t push[12] = {};
                push[0] = w;
                push[1] = h;
                push[2] = as_bits(layer.crop_l);
                push[3] = as_bits(layer.crop_r);
                push[4] = as_bits(layer.crop_t);
                push[5] = as_bits(layer.crop_b);
                push[6] = (layer.flip_h ? 1u : 0u) |
                          (layer.flip_v ? 2u : 0u);
                push[7] = as_bits(layer.xf_scale);
                push[8] = as_bits(layer.xf_rotate * doc::kDeg2Rad);
                push[9] = as_bits(layer.xf_anchor_x);
                push[10] = as_bits(layer.xf_anchor_y);
                push[11] = as_bits(layer.opacity);
                const GpuImage* sampled[1] = {input_image(0)};
                layer_transform_->dispatch(rec, arena_, frame_index, sampled,
                                           1, &dst, 1, push, sizeof(push), w,
                                           h, linear_sampler_);
                break;
            }
            case GraphNode::Kind::Generator: {
                uint32_t push[24] = {};
                push[0] = w;
                push[1] = h;
                // The dummy starts UNDEFINED, so give the sampler a layout.
                dummy_flow_->transition(
                    rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                const GpuImage* sdf_tex = dummy_flow_.get();
                const GpuImage* ramp_tex = dummy_flow_.get();
                if (node.layer_index >= 0) {
                    const doc::Layer& layer =
                        look.layers[static_cast<size_t>(node.layer_index)];
                    push[2] = static_cast<uint32_t>(layer.source);
                    push[3] = static_cast<uint32_t>(
                        hash_combine(doc.master_seed, layer.id));
                    push[4] = timeline_frame;
                    for (int c = 0; c < 4; ++c) {
                        push[5 + c] = as_bits(layer.color_a[c]);
                        push[9 + c] = as_bits(layer.color_b[c]);
                    }
                    push[13] = as_bits(layer.gen_scale);
                    push[14] = as_bits(layer.gen_angle);
                    push[15] = layer.osc_shape;
                    push[16] = as_bits(layer.gen_phase);
                    if (layer.source == doc::LayerSourceKind::Gradient) {
                        push[17] = static_cast<uint32_t>(layer.gradient);
                        push[18] = as_bits(layer.gradient_len);
                        push[19] = as_bits(layer.gradient_x);
                        push[20] = as_bits(layer.gradient_y);
                        push[21] = static_cast<uint32_t>(
                            std::min<size_t>(layer.stops.size(), 32));
                        push[22] = as_bits(layer.gradient_len *
                                           mesh_stop_spacing(layer, w, h));
                        push[23] = static_cast<uint32_t>(
                            layer.gradient_space);
                        if (const GpuImage* r =
                                ensure_gradient_ramp(rec, staging, layer))
                            ramp_tex = r;
                    }
                    if (layer.source == doc::LayerSourceKind::Shape &&
                        layer.osc_shape == 3u && !layer.path.empty()) {
                        // The raster re-runs only on a path or size change.
                        const uint32_t rw =
                            std::clamp(w / 2u, 64u, 1920u);
                        const uint32_t rh =
                            std::clamp(h / 2u, 64u, 1920u);
                        uint64_t phash = hash_combine(
                            0x5DFull,
                            (static_cast<uint64_t>(rw) << 32) | rh);
                        phash = hash_combine(
                            phash, layer.path_closed ? 1ull : 0ull);
                        for (const doc::PathPoint& p : layer.path) {
                            const float f[6] = {p.ax, p.ay, p.in_dx,
                                                p.in_dy, p.out_dx,
                                                p.out_dy};
                            for (float c : f)
                                phash = hash_combine(phash, as_bits(c));
                        }
                        ShapeSlot& slot = shape_state_[layer.id];
                        if (slot.hash != phash || !slot.tex) {
                            if (slot.tex && (slot.w != rw || slot.h != rh)) {
                                // The old raster may be in flight.
                                device_.wait_idle();
                                slot.tex.reset();
                            }
                            const float aspect =
                                static_cast<float>(w) /
                                std::max(1.0f, static_cast<float>(h));
                            std::vector<uint16_t> sdf;
                            shape_sdf_raster(layer.path,
                                             layer.path_closed, aspect, rw,
                                             rh, &sdf);
                            if (!slot.tex) {
                                slot.tex = GpuImage::create(
                                    device_, VK_FORMAT_R16_UNORM, rw, rh,
                                    VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT);
                                if (!slot.tex) return nullptr;
                            }
                            if (!staging.upload_image(
                                    rec, sdf.data(),
                                    sdf.size() * sizeof(uint16_t), rw,
                                    *slot.tex))
                                return nullptr;
                            slot.tex->transition(
                                rec,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                            slot.hash = phash;
                            slot.w = rw;
                            slot.h = rh;
                        }
                        sdf_tex = slot.tex.get();
                    }
                } else {
                    // Premultiplied zero, never opaque black: a ground and
                    // a matte reveal must composite as absent, not as a
                    // hole that hides whatever sits below.
                    clear_color(rec, *dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                {});
                    break;
                }
                const GpuImage* sampled[2] = {sdf_tex, ramp_tex};
                generator_->dispatch(rec, arena_, frame_index, sampled, 2,
                                     &dst, 1, push, sizeof(push), w, h,
                                     linear_sampler_);
                break;
            }
            case GraphNode::Kind::LayerBlend: {
                // layer_index -1 is a sequence lane: plain alpha-over only.
                // A look layer uses its own mode; alpha stays premultiplied.
                doc::BlendMode mode = doc::BlendMode::Normal;
                float opacity = node.p_opacity;
                bool moved = false;
                if (node.layer_index >= 0) {
                    const doc::Layer& layer =
                        look.layers[static_cast<size_t>(node.layer_index)];
                    if (node.effect_index >= 0) {
                        const auto& fx = layer.stack[static_cast<size_t>(node.effect_index)];
                        mode = fx.blend;
                        opacity = fx.opacity;
                    } else mode = layer.blend;
                } else {
                    moved = node.p_scale != 1.0f || node.p_rotate != 0.0f ||
                            node.p_shift_x != 0.0f ||
                            node.p_shift_y != 0.0f;
                }
                uint32_t push[11] = {};
                push[0] = w;
                push[1] = h;
                push[2] = static_cast<uint32_t>(mode);
                push[3] = as_bits(opacity);
                push[4] = moved ? 1u : 0u;
                push[5] = as_bits(node.p_scale);
                push[6] = as_bits(node.p_rotate);
                push[7] = as_bits(node.p_shift_x);
                push[8] = as_bits(node.p_shift_y);
                push[9] = as_bits(node.p_anchor_x);
                push[10] = as_bits(node.p_anchor_y);
                const GpuImage* sampled[2] = {input_image(0), input_image(1)};
                layer_blend_->dispatch(rec, arena_, frame_index, sampled, 2,
                                       &dst, 1, push, sizeof(push), w, h,
                                       linear_sampler_);
                break;
            }
            case GraphNode::Kind::Effect: {
                effect_frame_ = timeline_frame;
                const double effect_fps = doc::entity_fps(doc, linst.look);
                const doc::EffectInstance* fx_ptr =
                    &look.layers[static_cast<size_t>(node.layer_index)]
                        .stack[static_cast<size_t>(node.effect_index)];
                doc::EffectInstance blended_fx;
                if (fx_ptr->blend != doc::BlendMode::Normal) {
                    blended_fx = *fx_ptr;
                    blended_fx.opacity = 1.0f;
                    fx_ptr = &blended_fx;
                }
                const auto& fx = *fx_ptr;

                if ((fx_optical_filter(fx.type) || fx.type == doc::EffectType::Normalise) &&
                    (fx.wet <= 0.0f || fx.opacity <= 0.0f ||
                     ((fx.type == doc::EffectType::Glow || fx.type == doc::EffectType::CornerSoft) &&
                       fx.params[0] <= 0.0f))) {
                    copy_full(rec, *input_image(0), *dst, w, h);
                    input_image(0)->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    dst->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
                    break;
                }

                if (doc::is_codec_box(fx.type)) {
                    GpuImage* in = input_image(0);
                    GpuImage* flow_img =
                        fx.type == doc::EffectType::Datamosh &&
                                node.inputs.size() > 1
                            ? input_image(1)
                            : nullptr;

                    const uint64_t gpu_seed = hash_combine(
                        hash_combine(doc.master_seed, fx.id), fx.seed);
                    const codec::MoshParams gp = mosh_params(fx, gpu_seed);
                    if (gp.byte_flips == 0) {
                        if (!mosh_gpu_box(rec, fx, skey, doc.revision, gp, in, flow_img, w,
                                          h, frame_index, timeline_frame,
                                          dst))
                            return nullptr;
                        break;
                    }

                    // The byte-flip and bitrate paths need real bytes, so
                    // they roundtrip through the CPU in three steps.
                    dispatch_to_nv12(rec, frame_index, in, w, h);
                    copy_nv12_to_buffer(rec, *codec_io_.nv_y, *codec_io_.nv_uv,
                                        codec_io_.readback, w, h);
                    const size_t nv_bytes =
                        static_cast<size_t>(w) * h * 3 / 2;
                    const size_t flow_offset = (nv_bytes + 7) & ~size_t{7};
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
                    memory_barrier(rec, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_HOST_BIT,
                                   VK_ACCESS_TRANSFER_WRITE_BIT,
                                   VK_ACCESS_HOST_READ_BIT);
                    codec_flush_segment();

                    // A paused re-render reuses the cached output.
                    // A paused param edit resets the codec to a fresh I.
                    MoshSlot& slot = mosh_state_[skey];
                    const uint64_t box_seed = hash_combine(
                        hash_combine(doc.master_seed, fx.id), fx.seed);
                    const uint64_t box_sig =
                        hash_combine(stateful_param_sig(fx, box_seed), doc.revision);
                    if (slot.valid && slot.last_frame == timeline_frame &&
                        slot.sig != box_sig) {
                        slot.codec.reset();
                        slot.valid = false;
                    }
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
                        slot.codec.process(view, timeline_frame,
                                           mosh_params(fx, box_seed), field,
                                           slot.last_out);
                        slot.last_frame = timeline_frame;
                        slot.sig = box_sig;
                        slot.valid = true;
                    }
                    const codec::DecodedFrame& mo = slot.last_out;

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
                    GpuImage* temp = pool_->acquire(w, h);
                    if (!temp) return nullptr;
                    temp->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
                    codec_planes_to_rgb(rec, frame_index, temp, w, h);
                    mix_composite_release(rec, frame_index, in, temp, dst,
                                          fx.wet, fx.opacity, w, h);
                    break;
                }

                if (fx.type == doc::EffectType::ErrorDiffusion) {
                    GpuImage* in_img = const_cast<GpuImage*>(input_image(0));
                    auto& slot_ptr = ed_state_[skey];
                    if (!slot_ptr)
                        slot_ptr = std::make_unique<EdSlotAsync>();
                    EdSlotAsync& slot = *slot_ptr;
                    const size_t px_count = static_cast<size_t>(w) * h;

                    // Land the walk kicked last frame before you read it.
                    if (slot.busy) {
                        slot.worker.join();
                        slot.busy = false;
                        slot.has_result = true;
                    }

                    // This composites the last frame's dither, one frame
                    // behind; a first frame or a size change passes dry.
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

                    // This kicks one walk per timeline frame; the next
                    // frame consumes it. A param edit kicks a fresh walk.
                    const uint64_t ed_sig = hash_combine(stateful_param_sig(
                        fx, hash_combine(doc.master_seed, fx.seed)),
                        hash_combine(doc.revision, hash_combine(w, h)));
                    if (slot.captured_frame != timeline_frame ||
                        slot.sig != ed_sig) {
                        in_img->transition(
                            rec, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                        memory_barrier(rec,
                                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VK_ACCESS_SHADER_WRITE_BIT,
                                       VK_ACCESS_TRANSFER_READ_BIT);
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
                        memory_barrier(rec, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VK_PIPELINE_STAGE_HOST_BIT,
                                       VK_ACCESS_TRANSFER_WRITE_BIT,
                                       VK_ACCESS_HOST_READ_BIT);
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
                        slot.sig = ed_sig;
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
                // Push prelude: size, wet, opacity, seed, frame, fps.
                const uint64_t seed64 = hash_combine(
                    hash_combine(doc.master_seed, fx.id), fx.seed);
                // 32 words is the 128-byte push floor; prelude, params and
                // the largest extra tail must all fit inside it.
                uint32_t push[32] = {};
                static_assert(kFxPreludeWords + 16 + 9 <= 32,
                              "push buffer covers params + extras");
                const uint32_t prelude_words = fx_prelude_words(fx.type);
                push[0] = w;
                push[1] = h;
                push[2] = as_bits(fx.wet);
                push[3] = as_bits(fx.opacity);
                push[4] = static_cast<uint32_t>(seed64);
                push[5] = timeline_frame;
                push[6] = as_bits(static_cast<float>(effect_fps));
                if (prelude_words == 10) {
                    push[7] = as_bits(static_cast<float>(canvas_w));
                    push[8] = as_bits(static_cast<float>(canvas_h));
                    push[9] = 0;
                }
                const size_t param_count = fx.params.size();
                std::memcpy(&push[prelude_words], fx.params.data(),
                            param_count * sizeof(float));
                const uint32_t push_bytes = static_cast<uint32_t>(
                    (prelude_words + param_count) * sizeof(uint32_t));
                auto dispatch2 = [&](const GpuImage* a, const GpuImage* b,
                                     uint32_t bytes) {
                    const GpuImage* sampled[2] = {a, b};
                    fx_[static_cast<size_t>(fx.type)]->dispatch(
                        rec, arena_, frame_index, sampled, 2, &dst, 1, push,
                        bytes, w, h, linear_sampler_);
                };

                if (fx.type == doc::EffectType::Photocopy) {
                    const int copies = std::clamp(static_cast<int>(fx.params[1] + 0.5f), 1, 8);
                    const GpuImage* source = input_image(0);
                    GpuImage* current = nullptr;
                    push[2] = as_bits(1.0f);
                    push[3] = as_bits(1.0f);
                    for (int copy = 0; copy < copies; ++copy) {
                        GpuImage* next = pool_->acquire(w, h);
                        if (!next) return nullptr;
                        next->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
                        push[4] = static_cast<uint32_t>(seed64) + static_cast<uint32_t>(copy) * 0x9E3779B9u;
                        const GpuImage* sampled[1] = {source};
                        fx_[static_cast<size_t>(fx.type)]->dispatch(rec, arena_, frame_index,
                            sampled, 1, &next, 1, push, push_bytes, w, h, linear_sampler_);
                        next->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        if (current) pool_->release(current);
                        current = next;
                        source = current;
                    }
                    mix_composite_release(rec, frame_index, input_image(0), current, dst,
                        fx.wet, fx.opacity, w, h);
                } else if (fx.type == doc::EffectType::Normalise) {
                    GpuImage* source = input_image(0);
                    uint32_t sw = w, sh = h;
                    uint32_t grid_w = fx.params[0] < 0.5f ? std::min(w, 128u) : w;
                    uint32_t grid_h = fx.params[0] < 0.5f ? std::min(h, 128u) : h;
                    uint32_t first = 1;
                    do {
                        const uint32_t nw = (grid_w + 7u) / 8u;
                        const uint32_t nh = (grid_h + 7u) / 8u;
                        GpuImage* reduced = pool_->acquire(nw, nh, VK_FORMAT_R32G32B32A32_SFLOAT);
                        if (!reduced) return nullptr;
                        reduced->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
                        const uint32_t rp[5] = {sw, sh, grid_w, grid_h, first};
                        const GpuImage* sampled[1] = {source};
                        normalise_reduce_->dispatch(rec, arena_, frame_index, sampled, 1,
                            &reduced, 1, rp, sizeof(rp), grid_w, grid_h, linear_sampler_);
                        reduced->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        if (!first) pool_->release(source);
                        source = reduced;
                        sw = grid_w = nw;
                        sh = grid_h = nh;
                        first = 0;
                    } while (grid_w > 1 || grid_h > 1);
                    dispatch2(input_image(0), source, push_bytes);
                    pool_->release(source);
                } else if (fx.type == doc::EffectType::CamAuto) {
                    FeedbackSlot* meter = ensure_feedback_prev(rec, skey, 1, 1);
                    if (!meter) return nullptr;
                    push[9] = meter->valid;
                    meter->current->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
                    const GpuImage* sampled[2] = {input_image(0), meter->prev.get()};
                    GpuImage* state = meter->current.get();
                    camera_meter_->dispatch(rec, arena_, frame_index, sampled, 2,
                        &state, 1, push, push_bytes, 1, 1, linear_sampler_);
                    state->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    dispatch2(input_image(0), state, push_bytes);
                } else if (fx.type == doc::EffectType::Glow && fx.params[3] >= 1.5f) {
                    GpuImage* charge = pool_->acquire(w, h);
                    GpuImage* columns = pool_->acquire(w, 1);
                    if (!charge || !columns) return nullptr;
                    charge->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
                    columns->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
                    const uint32_t cp[5] = {w, h, as_bits(fx.params[2]), as_bits(fx.params[0]),
                        as_bits(fx.params[1] * static_cast<float>(h) / canvas_h)};
                    const GpuImage* source[1] = {input_image(0)};
                    GpuImage* targets[2] = {charge, columns};
                    glow_columns_->dispatch(rec, arena_, frame_index, source, 1,
                        targets, 2, cp, sizeof(cp), w, 1, linear_sampler_);
                    charge->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    columns->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    const GpuImage* sampled[3] = {input_image(0), charge, columns};
                    fx_[static_cast<size_t>(fx.type)]->dispatch(rec, arena_, frame_index,
                        sampled, 3, &dst, 1, push, push_bytes, w, h, linear_sampler_);
                    pool_->release(charge);
                    pool_->release(columns);
                } else if (fx_optical_filter(fx.type)) {
                    const float scale = static_cast<float>(w) / canvas_w;
                    float footprint = 2.0f;
                    float threshold = -1.0f;
                    const bool horizontal = fx.type == doc::EffectType::Anamorphic;
                    if (fx.type == doc::EffectType::Glow) {
                        footprint = fx.params[1] * scale * 0.35f;
                        threshold = fx.params[2];
                    }
                    if (fx.type == doc::EffectType::CornerSoft) footprint = fx.params[0] * scale * 4.0f;
                    if (fx.type == doc::EffectType::Aperture) footprint = fx.params[0] * scale * 0.3f;
                    if (fx.type == doc::EffectType::Halation) {
                        footprint = fx.params[1] * scale * 0.35f;
                        threshold = fx.params[0];
                    }
                    if (horizontal) {
                        footprint = w * 0.6f / 20.0f;
                        threshold = fx.params[1] * fx.params[1];
                    }
                    if (fx.type == doc::EffectType::StarFilter) {
                        footprint = 4.0f * scale;
                        threshold = fx.params[2];
                    }
                    if (fx.type == doc::EffectType::LensFlare) {
                        footprint = 8.0f * scale;
                        threshold = fx.params[0];
                    }
                    GpuImage* filtered = input_image(0);
                    uint32_t fw = w, fh = h;
                    float level = 1.0f;
                    do {
                        const uint32_t nw = std::max(1u, fw / 2);
                        const uint32_t nh = horizontal ? fh : std::max(1u, fh / 2);
                        GpuImage* reduced = pool_->acquire(nw, nh);
                        if (!reduced) return nullptr;
                        reduced->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
                        const uint32_t rp[5] = {nw, nh, as_bits(0.5f / fw),
                            as_bits(horizontal ? 0.0f : 0.5f / fh), as_bits(threshold)};
                        const GpuImage* sampled[1] = {filtered};
                        optical_reduce_->dispatch(rec, arena_, frame_index, sampled, 1,
                            &reduced, 1, rp, sizeof(rp), nw, nh, linear_sampler_);
                        reduced->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        if (filtered != input_image(0)) pool_->release(filtered);
                        filtered = reduced;
                        fw = nw; fh = nh; level *= 2.0f; threshold = -1.0f;
                    } while (level < footprint && fw > 1 && fh > 1);
                    for (uint32_t axis = 1u; axis <= (horizontal ? 1u : 2u); ++axis) {
                        GpuImage* smooth = pool_->acquire(fw, fh);
                        if (!smooth) return nullptr;
                        smooth->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
                        const uint32_t bp[4] = {fw, fh, axis, as_bits(0.65f)};
                        const GpuImage* sampled[1] = {filtered};
                        crt_blur_->dispatch(rec, arena_, frame_index, sampled, 1, &smooth, 1,
                            bp, sizeof(bp), fw, fh, linear_sampler_);
                        smooth->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        pool_->release(filtered);
                        filtered = smooth;
                    }
                    const GpuImage* sampled[3] = {input_image(0), filtered, nullptr};
                    uint32_t count = 2;
                    if (fx.type == doc::EffectType::Glow) {
                        sampled[2] = filtered;
                        count = 3;
                    }
                    if (fx.type == doc::EffectType::Aperture) {
                        sampled[1] = node.inputs.size() > 1 ? input_image(1) : input_image(0);
                        sampled[2] = filtered;
                        count = 3;
                    }
                    fx_[static_cast<size_t>(fx.type)]->dispatch(rec, arena_, frame_index,
                        sampled, count, &dst, 1, push, push_bytes, w, h, linear_sampler_);
                    pool_->release(filtered);
                } else if (fx.type == doc::EffectType::CrtSim) {
                    uint32_t* extra = &push[prelude_words + param_count];
                    extra[0] = as_bits(static_cast<float>(canvas_w));
                    extra[1] = as_bits(static_cast<float>(canvas_h));
                    extra[2] = 0;
                    const uint32_t bytes = push_bytes + 3 * sizeof(uint32_t);
                    GpuImage* signal = input_image(0);
                    GpuImage* glow = signal;
                    const bool tube = fx.params[4] < 2.5f;
                    const bool history = tube && fx.params[11] > 0.0f;
                    if (tube) {
                        const uint32_t rows = static_cast<uint32_t>(
                            std::clamp(fx.params[2] + 0.5f, 64.0f, 1080.0f));
                        const GpuImage* previous = input_image(0);
                        if (history) {
                            CrtSlot& slot = crt_state_[skey];
                            if (!slot.current || slot.current->width() != w ||
                                slot.current->height() != rows) {
                                auto& retired = retired_images_[render_slot_];
                                if (slot.previous) retired.push_back(std::move(slot.previous));
                                if (slot.current) retired.push_back(std::move(slot.current));
                                const auto usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                                slot.previous = GpuImage::create(device_,
                                    VK_FORMAT_R16G16B16A16_SFLOAT, w, rows, usage);
                                slot.current = GpuImage::create(device_,
                                    VK_FORMAT_R16G16B16A16_SFLOAT, w, rows, usage);
                                slot.last_frame = 0xFFFFFFFFu;
                            }
                            if (!slot.previous || !slot.current) return nullptr;
                            if (slot.last_frame != 0xFFFFFFFFu &&
                                timeline_frame == slot.last_frame + 1) {
                                std::swap(slot.previous, slot.current);
                                slot.previous_valid = true;
                            } else if (slot.last_frame == 0xFFFFFFFFu ||
                                       timeline_frame != slot.last_frame) {
                                clear_color(rec, *slot.previous,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, {});
                                slot.previous_valid = false;
                            }
                            extra[2] = slot.previous_valid;
                            slot.previous->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                            previous = slot.previous.get();
                            signal = slot.current.get();
                            slot.last_frame = timeline_frame;
                        } else {
                            auto it = crt_state_.find(skey);
                            if (it != crt_state_.end()) it->second.last_frame = 0xFFFFFFFFu;
                            signal = pool_->acquire(w, rows);
                        }
                        if (!signal) return nullptr;
                        signal->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
                        push[1] = rows;
                        const GpuImage* sampled[2] = {input_image(0), previous};
                        crt_prepare_->dispatch(rec, arena_, frame_index,
                            sampled, 2, &signal, 1, push, bytes, w, rows, linear_sampler_);
                        signal->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        push[1] = h;
                        glow = signal;
                        if (fx.params[8] > 0.0f) {
                            auto blur = [&](uint32_t bw, uint32_t bh, uint32_t pass) {
                                GpuImage* target = pool_->acquire(bw, bh);
                                if (!target) return false;
                                target->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
                                const GpuImage* source[1] = {glow};
                                const float span = pass == 1
                                    ? static_cast<float>(bw) * canvas_h / canvas_w
                                    : static_cast<float>(bh);
                                const uint32_t blur_push[4] = {
                                    bw, bh, pass, as_bits(std::max(0.5f, span * 0.012f))};
                                crt_blur_->dispatch(rec, arena_, frame_index,
                                    source, 1, &target, 1, blur_push, sizeof(blur_push),
                                    bw, bh, linear_sampler_);
                                target->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                                if (glow != signal) pool_->release(glow);
                                glow = target;
                                return true;
                            };
                            while (glow->height() > 128 || glow->width() > 512) {
                                if (!blur(std::max(2u, (glow->width() + 1) / 2),
                                          std::max(2u, (glow->height() + 1) / 2), 0))
                                    return nullptr;
                            }
                            if (!blur(glow->width(), glow->height(), 1) ||
                                !blur(glow->width(), glow->height(), 2)) return nullptr;
                        }
                    } else {
                        auto it = crt_state_.find(skey);
                        if (it != crt_state_.end()) it->second.last_frame = 0xFFFFFFFFu;
                    }
                    const GpuImage* sampled[3] = {input_image(0), signal, glow};
                    fx_[static_cast<size_t>(fx.type)]->dispatch(rec, arena_, frame_index,
                        sampled, 3, &dst, 1, push, bytes, w, h, linear_sampler_);
                    if (glow != signal) pool_->release(glow);
                    if (tube && !history) pool_->release(signal);
                } else if (fx.type == doc::EffectType::FlowSmear ||
                           fx.type == doc::EffectType::FlowPaint) {
                    dispatch2(input_image(0), node.inputs.size() > 1 ? input_image(1) : input_image(0), push_bytes);
                } else if (fx.type == doc::EffectType::MotionExtract) {
                    // The luma planes are native, so sample through the fit.
                    float fit[4];
                    source_fit_rect(ref_plane->width(),
                                    ref_plane->height(), w, h, fit);
                    push[prelude_words + param_count] = as_bits(fit[0]);
                    push[prelude_words + param_count + 1] =
                        as_bits(fit[1]);
                    push[prelude_words + param_count + 2] =
                        as_bits(1.0f / std::max(fit[2], 1.0f));
                    push[prelude_words + param_count + 3] =
                        as_bits(1.0f / std::max(fit[3], 1.0f));
                    const GpuImage* sampled[3] = {input_image(0), prev_plane,
                                                  ref_plane};
                    fx_[static_cast<size_t>(fx.type)]->dispatch(
                        rec, arena_, frame_index, sampled, 3, &dst, 1, push,
                        push_bytes + 4 * sizeof(uint32_t), w, h,
                        linear_sampler_);
                } else if (fx.type == doc::EffectType::Glyph) {
                    int which = std::clamp(
                        static_cast<int>(fx.params[1] + 0.5f), 0, 4);
                    if (which >= 3) which = 0;
                    if (which == 2 && !glyph_atlas_[2]) which = 1;
                    if (which == 1 && !glyph_atlas_[1]) which = 0;
                    const GlyphMeta& gm = glyph_meta_[which];
                    push[prelude_words + param_count] = gm.cols;
                    push[prelude_words + param_count + 1] = gm.rows;
                    push[prelude_words + param_count + 2] =
                        as_bits(gm.tile);
                    push[prelude_words + param_count + 3] =
                        glyph_atlas_color_[which] ? 1u : 0u;
                    dispatch2(input_image(0), glyph_atlas_[which].get(),
                              push_bytes + 4 * sizeof(uint32_t));
                } else if (fx.type == doc::EffectType::Quantize) {
                    // Mode 9 puts the RD state in this slot; other modes
                    // bind the noise LUT there as a dummy.
                    const int dm = fx.params.size() > 2
                        ? static_cast<int>(fx.params[2] + 0.5f)
                        : 0;
                    const GpuImage* stipple = noise_lut_.get();
                    if (dm == 9) {
                        // These constants hold the soliton dot regime.
                        GpuImage* st = rd_advance(rec, frame_index, skey,
                                                  input_image(0), w, h,
                                                  timeline_frame, 8, 0.030f,
                                                  0.062f, 0.2f);
                        if (!st) return nullptr;
                        stipple = st;
                    }
                    // The graph wires flow as input 1 under motion lock;
                    // otherwise the LUT rides there as an unread dummy.
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
                    // The LUT is always input 1; flow joins as input 2
                    // only under motion lock, and the LUT is the dummy.
                    const GpuImage* flow_tex = node.inputs.size() > 1
                                                   ? input_image(1)
                                                   : noise_lut_.get();
                    const GpuImage* sampled[3] = {input_image(0),
                                                  noise_lut_.get(), flow_tex};
                    fx_[static_cast<size_t>(fx.type)]->dispatch(
                        rec, arena_, frame_index, sampled, 3, &dst, 1, push,
                        push_bytes, w, h, linear_sampler_);
                } else if (fx.type == doc::EffectType::Interlace ||
                           fx.type == doc::EffectType::FilmSlip) {
                    // The input doubles as the past frame until the ring
                    // fills; the ring advances in every mode.
                    SlitSlot& slot = slit_ring_slot(skey, w, h);
                    GpuImage* in_img =
                        const_cast<GpuImage*>(input_image(0));
                    const GpuImage* past = slit_history(slot, in_img, 1);
                    dispatch2(in_img, past, push_bytes);
                    if (!slit_ring_push(rec, slot, in_img, w, h,
                                        timeline_frame))
                        return nullptr;
                } else if (fx.type == doc::EffectType::FrameDelay) {
                    // One ring slice binds as the second input.
                    SlitSlot& slot = slit_ring_slot(skey, w, h);
                    const uint32_t delay = static_cast<uint32_t>(std::clamp(
                        fx.params[0], 0.0f,
                        static_cast<float>(kSlitRing - 1)));
                    GpuImage* in_img =
                        const_cast<GpuImage*>(input_image(0));
                    const GpuImage* past = slit_history(
                        slot, in_img, std::min(delay, slot.count));
                    dispatch2(in_img, past, push_bytes);
                    if (!slit_ring_push(rec, slot, in_img, w, h,
                                        timeline_frame))
                        return nullptr;
                } else if (fx.type == doc::EffectType::Text) {
                    // The bucket keys on the param in 1080-reference px,
                    // so a proxy preview and export pick the same raster.
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
                            // The old rasters can be in flight; settle first.
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
                                // Nothing drawable; w = 1 stops a re-raster.
                                tb.w = 1;
                            }
                        }
                        if (tb.tex) ras = &tb;
                    }
                    uint32_t* extra = &push[prelude_words + param_count];
                    extra[0] = as_bits(
                        ras ? static_cast<float>(ras->w) : 0.0f);
                    extra[1] = as_bits(
                        ras ? static_cast<float>(ras->h) : 0.0f);
                    extra[2] = as_bits(ras ? ras->spread : 1.0f);
                    extra[3] = as_bits(kTextBuckets[bucket]);
                    const GpuImage* sdf_tex =
                        ras ? ras->tex.get() : noise_lut_.get();
                    dispatch2(input_image(0), sdf_tex,
                              push_bytes + 4 * sizeof(uint32_t));
                } else if (fx.type == doc::EffectType::Displace) {
                    // Input 1 is the wired map, or the input's own luma.
                    const GpuImage* map = node.inputs.size() > 1
                                              ? input_image(1)
                                              : input_image(0);
                    dispatch2(input_image(0), map, push_bytes);
                } else if (fx.type == doc::EffectType::DustScratches) {
                    // The damage plate is input 1 and is always present.
                    dispatch2(input_image(0), dust_tex_.get(), push_bytes);
                } else if (fx.type == doc::EffectType::SlitScan) {
                    // One dispatch per age band; the bands write disjoint
                    // pixels, so they need no barrier between them.
                    SlitSlot& slot = slit_ring_slot(skey, w, h);
                    const uint32_t depth = static_cast<uint32_t>(std::clamp(
                        fx.params[1], 2.0f, static_cast<float>(kSlitRing)));
                    GpuImage* in_img =
                        const_cast<GpuImage*>(input_image(0));
                    uint32_t* extra = &push[prelude_words + param_count];
                    const uint32_t slit_bytes =
                        push_bytes + static_cast<uint32_t>(sizeof(uint32_t));
                    for (uint32_t k = 0; k < depth; ++k) {
                        *extra = k;
                        const bool reverse = fx.params[2] >= 0.5f;
                        const uint32_t a = reverse ? depth - 1 - k : k;
                        const uint32_t b = reverse ? (a > 0 ? a - 1 : 0) : std::min(a + 1, depth - 1);
                        const GpuImage* sampled[3] = {in_img,
                            slit_history(slot, in_img, std::min(a, slot.read_count)),
                            slit_history(slot, in_img, std::min(b, slot.read_count))};
                        const bool rows = fx.params[0] < 0.5f;
                        const uint32_t extent = rows ? h : w;
                        const uint32_t band = extent * (k + 1) / depth - extent * k / depth;
                        if (band == 0) continue;
                        fx_[static_cast<size_t>(fx.type)]->dispatch(rec, arena_, frame_index,
                            sampled, 3, &dst, 1, push, slit_bytes,
                            rows ? w : band, rows ? band : h, linear_sampler_);
                    }
                    if (!slit_ring_push(rec, slot, in_img, w, h,
                                        timeline_frame))
                        return nullptr;
                } else if (fx.type == doc::EffectType::TimeDisplace) {
                    // One dispatch per age band; the shader masks pixels
                    // to its band from the delay map.
                    SlitSlot& slot = slit_ring_slot(skey, w, h);
                    const uint32_t depth = static_cast<uint32_t>(std::clamp(
                        fx.params[0], 2.0f, static_cast<float>(kSlitRing)));
                    GpuImage* in_img =
                        const_cast<GpuImage*>(input_image(0));
                    const GpuImage* map = node.inputs.size() > 1
                                              ? input_image(1)
                                              : in_img;
                    uint32_t* extra = &push[prelude_words + param_count];
                    const uint32_t td_bytes =
                        push_bytes + static_cast<uint32_t>(sizeof(uint32_t));
                    for (uint32_t k = 0; k < depth; ++k) {
                        *extra = k;
                        const GpuImage* sampled[3] = {
                            in_img, slit_history(slot, in_img, k), map};
                        fx_[static_cast<size_t>(fx.type)]->dispatch(
                            rec, arena_, frame_index, sampled, 3, &dst, 1,
                            push, td_bytes, w, h, linear_sampler_);
                    }
                    if (!slit_ring_push(rec, slot, in_img, w, h,
                                        timeline_frame))
                        return nullptr;
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
                        copy_full(rec, *in_img, *slot.held, w, h);
                        in_img->transition(
                            rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        slot.last_tick = tick;
                    }
                    slot.held->transition(
                        rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    dispatch2(in_img, slot.held.get(), push_bytes);
                } else if (fx.type == doc::EffectType::Stutter) {
                    SlitSlot& slot = slit_ring_slot(skey, w, h);
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
                        chosen = slit_history(slot, in_img, 1u + (step % span));
                    }
                    dispatch2(in_img, chosen, push_bytes);
                    // Record once per timeline frame while unarmed; armed
                    // freezes the ring on purpose.
                    if (!armed && !slit_ring_push(rec, slot, in_img, w, h,
                                                  timeline_frame))
                        return nullptr;
                } else if (fx.type == doc::EffectType::ReactionDiffusion) {
                    const uint32_t steps = static_cast<uint32_t>(
                        std::clamp(fx.params[2], 1.0f, 24.0f));
                    const float grid_scale = std::min(1.0f, 512.0f / static_cast<float>(std::max(canvas_w, canvas_h)));
                    const uint32_t grid_w = std::max(8u, static_cast<uint32_t>(canvas_w * grid_scale));
                    const uint32_t grid_h = std::max(8u, static_cast<uint32_t>(canvas_h * grid_scale));
                    GpuImage* st = rd_advance(rec, frame_index, skey,
                                              input_image(0), grid_w, grid_h,
                                              timeline_frame, steps,
                                              fx.params[0], fx.params[1],
                                              fx.params[3], static_cast<float>(effect_fps));
                    if (!st) return nullptr;
                    dispatch2(input_image(0), st, push_bytes);
                } else if (fx.type == doc::EffectType::VelocityScan) {
                    const uint32_t state_w = std::max(w, h);
                    const uint64_t front_key = hash_combine(skey,
                        hash_combine(0x56534652u, hash_combine(hash_combine(canvas_w, canvas_h),
                            hash_combine(as_bits(fx.params[0]), as_bits(static_cast<float>(effect_fps))))));
                    FeedbackSlot* fronts = ensure_feedback_prev(rec, front_key, state_w, kVsSlots,
                                                               VK_FORMAT_R32G32B32A32_SFLOAT);
                    if (!fronts) return nullptr;
                    GpuImage* front = fronts->current.get();
                    front->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
                    const uint32_t vp[14] = {state_w, kVsSlots, static_cast<uint32_t>(seed64),
                        timeline_frame, as_bits(static_cast<float>(effect_fps)), as_bits(fx.params[1]),
                        as_bits(fx.params[2]), as_bits(fx.params[4]), as_bits(fx.params[6]),
                        static_cast<uint32_t>(fx.params[0] + 0.5f), w, h,
                        as_bits(static_cast<float>(canvas_w)), as_bits(static_cast<float>(canvas_h))};
                    const GpuImage* vin[2] = {fronts->prev.get(), input_image(0)};
                    vs_front_->dispatch(rec, arena_, frame_index, vin, 2, &front, 1,
                        vp, sizeof(vp), state_w, kVsSlots, linear_sampler_);
                    front->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    FeedbackSlot* fb = ensure_feedback_prev(rec, skey, w, h);
                    if (!fb) return nullptr;
                    push[prelude_words + param_count] = state_w;
                    const GpuImage* sampled[3] = {input_image(0), fb->prev.get(), front};
                    fx_[static_cast<size_t>(fx.type)]->dispatch(rec, arena_, frame_index,
                        sampled, 3, &dst, 1, push, push_bytes + sizeof(uint32_t),
                        w, h, linear_sampler_);
                    feedback_writeback(rec, *fb, dst, w, h, timeline_frame);
                } else if (fx.type == doc::EffectType::FlowParticles) {
                    // Inputs: frame, own previous output, then flow.
                    FeedbackSlot* fb = ensure_feedback_prev(rec, skey, w, h);
                    if (!fb) return nullptr;
                    const GpuImage* sampled[3] = {input_image(0),
                                                  fb->prev.get(),
                                                  input_image(1)};
                    fx_[static_cast<size_t>(fx.type)]->dispatch(
                        rec, arena_, frame_index, sampled, 3, &dst, 1, push,
                        push_bytes, w, h, linear_sampler_);
                    feedback_writeback(rec, *fb, dst, w, h, timeline_frame);
                } else if (fx.type == doc::EffectType::SecurityMux) {
                    SlitSlot& slot = slit_ring_slot(skey, w, h);
                    GpuImage* in_img =
                        const_cast<GpuImage*>(input_image(0));
                    uint32_t* extra = &push[prelude_words + param_count];
                    const uint32_t mux_bytes =
                        push_bytes + static_cast<uint32_t>(sizeof(uint32_t));
                    const uint32_t cols = static_cast<uint32_t>(std::clamp(fx.params[0], 1.0f, 4.0f) + 0.5f);
                    const uint32_t rows = static_cast<uint32_t>(std::clamp(fx.params[1], 1.0f, 4.0f) + 0.5f);
                    const uint32_t tiles = cols * rows;
                    for (uint32_t tile = 0; tile < tiles; ++tile) {
                        *extra = tile;
                        const uint32_t age = static_cast<uint32_t>(
                            static_cast<float>((timeline_frame + tiles - tile) % tiles) *
                            std::clamp(fx.params[2], 0.0f, 1.0f) + 0.5f);
                        const GpuImage* sampled[2] = {in_img, slit_history(slot, in_img, age)};
                        fx_[static_cast<size_t>(fx.type)]->dispatch(rec, arena_, frame_index,
                            sampled, 2, &dst, 1, push, mux_bytes,
                            (w + cols - 1) / cols, (h + rows - 1) / rows, linear_sampler_);
                    }
                    if (!slit_ring_push(rec, slot, in_img, w, h,
                                        timeline_frame))
                        return nullptr;
                } else if (fx.type == doc::EffectType::Engraver || fx.type == doc::EffectType::FmSynth) {
                    // The phase integrator must run before this dispatch.
                    if (mod_integral_ &&
                        (mod_integral_->width() != w ||
                         mod_integral_->height() != h))
                        retired_images_[render_slot_].push_back(std::move(mod_integral_));
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
                    // Bit 1 reverses the march to follow the speed sign.
                    if (fx.params.size() > 3 && fx.params[3] < 0.0f)
                        fm_mode |= 2u;
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
                    if (fx.type == doc::EffectType::FmSynth) {
                        fm_mode = fx.params[5] >= 0.5f && fx.params[5] < 1.5f ? 1u : 0u;
                        mp[4] = as_bits(0.0f);
                        mp[5] = fm_mode | 4u;
                        mp[6] = as_bits(0.0f);
                    }
                    const GpuImage* mi_in[1] = {input_image(0)};
                    GpuImage* mi_out = mod_integral_.get();
                    mod_integrate_->dispatch(rec, arena_, frame_index,
                                             mi_in, 1, &mi_out, 1, mp,
                                             sizeof(mp), (mp[5] & 1u) ? h : w, 1,
                                             linear_sampler_);
                    mod_integral_->transition(
                        rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    dispatch2(input_image(0), mod_integral_.get(), push_bytes);
                } else if (fx.type == doc::EffectType::Anaglyph) {
                    push[9] = node.inputs.size() > 1 ? 1u : 0u;
                    dispatch2(input_image(0), node.inputs.size() > 1 ? input_image(1) : input_image(0), push_bytes);
                } else if (fx.type == doc::EffectType::TrackPin) {
                    // The CPU composes one 3x3 in double for the kernel.
                    // Rotation stays in metric space to stop a shear.
                    const doc::Layer& own =
                        look.layers[static_cast<size_t>(node.layer_index)];
                    double H[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
                    if (pin_planes_ && own.asset) {
                        const uint64_t key = pin_plane_key(
                            own.asset, fx.params[1], fx.params[2],
                            fx.params[3], fx.params[4]);
                        const auto it = pin_planes_->find(key);
                        if (it != pin_planes_->end() &&
                            !it->second.h.empty()) {
                            const uint32_t media_frame =
                                timeline_frame + own.slip;
                            const uint32_t n = static_cast<uint32_t>(
                                it->second.h.size() / 9);
                            const uint32_t idx =
                                media_frame <= it->second.start
                                    ? 0u
                                    : std::min(media_frame -
                                                   it->second.start,
                                               n - 1);
                            const float* hf =
                                it->second.h.data() +
                                static_cast<size_t>(idx) * 9;
                            for (int k = 0; k < 9; ++k) H[k] = hf[k];
                        }
                    }
                    auto mul3 = [](const double* a, const double* b,
                                   double* o) {
                        for (int r = 0; r < 3; ++r)
                            for (int c = 0; c < 3; ++c)
                                o[r * 3 + c] = a[r * 3] * b[c] +
                                               a[r * 3 + 1] * b[3 + c] +
                                               a[r * 3 + 2] * b[6 + c];
                    };
                    double M[9];
                    const bool stabilize = fx.params[0] >= 0.5f;
                    if (stabilize) {
                        std::memcpy(M, H, sizeof(M));
                    } else {
                        const double a = H[0], b = H[1], c = H[2];
                        const double d = H[3], e = H[4], f = H[5];
                        const double g = H[6], i = H[7], j = H[8];
                        double Hi[9] = {e * j - f * i, c * i - b * j,
                                        b * f - c * e, f * g - d * j,
                                        a * j - c * g, c * d - a * f,
                                        d * i - e * g, b * g - a * i,
                                        a * e - b * d};
                        const double det =
                            a * Hi[0] + b * Hi[3] + c * Hi[6];
                        if (std::fabs(det) > 1.0e-12)
                            for (double& v : Hi) v /= det;
                        // A maps reference uv to B uv.
                        const double aspect =
                            h > 0 ? static_cast<double>(w) / h : 1.0;
                        const double cx = fx.params[1], cy = fx.params[2];
                        const double rw2 = std::max(0.01f, fx.params[3]);
                        const double rh2 = std::max(0.01f, fx.params[4]);
                        const double ox = fx.params[5] * rw2;
                        const double oy = fx.params[6] * rh2;
                        const double s =
                            std::max(0.05f, fx.params[7]);
                        const double ang = fx.params[8];
                        const double ca = std::cos(-ang);
                        const double sa = std::sin(-ang);
                        const double t1[9] = {1, 0, -(cx + ox),
                                              0, 1, -(cy + oy),
                                              0, 0, 1};
                        // Metric rotate, folded into one matrix.
                        const double r2[9] = {ca, -sa / aspect, 0,
                                              sa * aspect, ca, 0,
                                              0, 0, 1};
                        const double s3[9] = {1.0 / (s * rw2), 0, 0.5,
                                              0, 1.0 / (s * rh2), 0.5,
                                              0, 0, 1};
                        double tmp[9], A[9];
                        mul3(r2, t1, tmp);
                        mul3(s3, tmp, A);
                        mul3(A, Hi, M);
                    }
                    uint32_t* extra = &push[prelude_words + param_count];
                    for (int k = 0; k < 9; ++k)
                        extra[k] = as_bits(static_cast<float>(M[k]));
                    const GpuImage* b_img = node.inputs.size() > 1
                                                ? input_image(1)
                                                : input_image(0);
                    dispatch2(input_image(0), b_img,
                              push_bytes +
                                  9u * static_cast<uint32_t>(sizeof(uint32_t)));
                } else if (fx.type == doc::EffectType::BlendNode ||
                           fx.type == doc::EffectType::Aperture ||
                           fx.type == doc::EffectType::Parallax ||
                           fx.type == doc::EffectType::RollingShutter) {
                    // B is input 1; an unwired B falls back to In.
                    const GpuImage* b = node.inputs.size() > 1
                        ? input_image(1)
                        : input_image(0);
                    dispatch2(input_image(0), b, push_bytes);
                } else if (fx.type == doc::EffectType::AudioScope) {
                    // The waveform strip was uploaded before graph eval.
                    auto it = audio_strip_.find(fx.id);
                    const GpuImage* strip =
                        it != audio_strip_.end() && it->second
                            ? it->second.get()
                            : input_image(0);
                    dispatch2(input_image(0), strip, push_bytes);
                } else if (doc::is_stateful_feedback(fx.type)) {
                    FeedbackSlot* fb = ensure_feedback_prev(rec, skey, w, h);
                    if (!fb) return nullptr;
                    if (fx.type == doc::EffectType::BurnIn || fx.type == doc::EffectType::ScopeMonitor) {
                        const uint32_t mode = static_cast<uint32_t>(fx.params[0] + 0.5f);
                        if (fb->mode != mode) {
                            clear_color(rec, *fb->prev, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, {});
                            fb->prev->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                            fb->valid = false;
                            fb->mode = mode;
                        }
                    }
                    if (prelude_words == 10) push[9] = fb->valid;
                    if (fx_raw_state(fx.type)) {
                        fb->current->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
                        const GpuImage* sampled[2] = {input_image(0), fb->prev.get()};
                        GpuImage* outputs[3] = {dst, fb->current.get(), nullptr};
                        uint32_t output_count = 2;
                        if (fx.type == doc::EffectType::ScopeMonitor) {
                            auto& bins = scope_bins_[render_slot_];
                            if (!bins) bins = GpuImage::create(device_, VK_FORMAT_R32_UINT, 1024, 256,
                                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
                            if (!bins) return nullptr;
                            clear_color(rec, *bins, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, {});
                            bins->transition(rec, VK_IMAGE_LAYOUT_GENERAL);
                            const uint32_t bp[4] = {w, h, static_cast<uint32_t>(fx.params[0] + 0.5f), as_bits(fx.params[1])};
                            GpuImage* target = bins.get();
                            scope_bins_pass_->dispatch(rec, arena_, frame_index, sampled, 1, &target, 1,
                                bp, sizeof(bp), w, h, linear_sampler_);
                            memory_barrier(rec, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
                            outputs[2] = bins.get();
                            output_count = 3;
                        }
                        fx_[static_cast<size_t>(fx.type)]->dispatch(
                            rec, arena_, frame_index, sampled, 2, outputs, output_count,
                            push, push_bytes, w, h, linear_sampler_);
                        fb->current->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    } else {
                        dispatch2(input_image(0), fb->prev.get(), push_bytes);
                        feedback_writeback(rec, *fb, dst, w, h, timeline_frame);
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
                effect_frame_ = timeline_frame;
                FeedbackSlot* history = ensure_feedback_prev(rec, skey, w, h);
                if (!history) return nullptr;
                struct {
                    uint32_t w, h, valid;
                    float rx, ry, iw, ih;
                } fpush = {w,
                           h,
                           history->valid ? 1u : 0u,
                           0.0f, 0.0f, 1.0f / w, 1.0f / h};
                const GpuImage* sampled[2] = {input_image(0), history->prev.get()};
                flow_->dispatch(rec, arena_, frame_index, sampled, 2, &dst, 1,
                                &fpush, sizeof(fpush), dst->width(),
                                dst->height(), linear_sampler_);
                feedback_writeback(rec, *history, input_image(0), w, h, timeline_frame);
                break;
            }
            case GraphNode::Kind::MatteExtract: {
                // The wired image's luma is the matte.
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
            case GraphNode::Kind::Crossfade: {
                const uint32_t push[4] = {w, h, as_bits(node.p_opacity), as_bits(1.0f)};
                const GpuImage* sampled[2] = {input_image(0), input_image(1)};
                group_mix_->dispatch(rec, arena_, frame_index, sampled, 2,
                    &dst, 1, push, sizeof(push), w, h, linear_sampler_);
                break;
            }
            case GraphNode::Kind::GroupMix: {
                // The group knobs read from the resolved look, like params.
                const doc::Group& grp =
                    look.layers[static_cast<size_t>(node.layer_index)]
                        .groups[static_cast<size_t>(node.effect_index)];
                const uint32_t push[4] = {w, h, as_bits(grp.wet),
                                          as_bits(grp.opacity)};
                const GpuImage* sampled[2] = {input_image(0),
                                              input_image(1)};
                group_mix_->dispatch(rec, arena_, frame_index, sampled, 2,
                                     &dst, 1, push, sizeof(push), w, h,
                                     linear_sampler_);
                break;
            }
        }

        results[static_cast<size_t>(index)] = dst;
        // A card ends where the compiler says, never where the kind hints:
        // a matte or a group wrap sits past the effect that named the card.
        for (const auto& tap : graph.thumb_taps)
            if (tap.second == index)
                record_thumb_tap(rec, frame_index, dst, tap.first);
        for (int input : node.inputs)
            if (--remaining_uses[static_cast<size_t>(input)] == 0)
                pool_->release(results[static_cast<size_t>(input)]);
        if (remaining_uses[static_cast<size_t>(index)] == 0) pool_->release(dst);
    }

    // read_measure_bounds harvests this after the caller's fence.
    if (graph.measure >= 0 && results[static_cast<size_t>(graph.measure)]) {
        GpuImage* mimg = results[static_cast<size_t>(graph.measure)];
        mimg->transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        VkClearColorValue cv{};
        for (int i = 0; i < 4; ++i) cv.uint32[i] = 0xFFFFFFFFu;
        clear_color(rec, *bounds_img_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    cv);
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

    // Cell key 0 always takes the output, never the preview tap.
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
    // Flush the tail segment so it completes before the caller submits.
    if (segmented) codec_flush_segment();

    // The copy records on the caller's cmd and harvests at its fence.
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
            memory_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT,
                           VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_HOST_READ_BIT);
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

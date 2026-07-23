// Engine (spec §4): per-frame evaluation of the compiled render graph.
// Uploads the decoded I420 frame, converts to the linear RGBA16F working
// space in compute, then walks the graph's topological order dispatching
// one kernel per effect over pooled ping-pong targets. Returns the final
// image ready for the viewport blit (and, next milestone, export readback).

#pragma once

#include <filesystem>
#include <memory>
#include <unordered_map>

#include "codec/mosh/mosh.h"
#include "doc/document.h"
#include "gfx/compute.h"
#include "gfx/render_cache.h"
#include "gfx/texture.h"

namespace looks::gfx {

// CPU-side I420 planes (decoded mezzanine frame or a generator). Strides are
// bytes per row; U/V are half resolution (rounded up).
struct SourcePlanes {
    const uint8_t* y = nullptr;
    size_t y_stride = 0;
    const uint8_t* u = nullptr;
    size_t u_stride = 0;
    const uint8_t* v = nullptr;
    size_t v_stride = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

class Engine {
public:
    static std::unique_ptr<Engine> create(Device& device,
                                          const std::filesystem::path& shader_dir);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Records upload + the full effect graph into `cmd` (must be OUTSIDE any
    // rendering pass). Returns the final target, transitioned to
    // SHADER_READ_ONLY_OPTIMAL — valid until the next render() call for this
    // frame slot. Null on failure (allocation, invalid graph).
    // timeline_frame/fps feed the deterministic per-frame randomness and
    // clocked effects (spec §11: fixed timestep on frame index).
    // overlay_mask_id != 0 shows that mask's grayscale instead (spec §8
    // viewport mask-overlay visualization).
    // External mask-source frame for this render (spec §8): decoded I420
    // planes for the mask with `mask_id`. Callers decode the right frame
    // (locked / free-running) and keep the planes alive through render().
    struct MaskSourceFrame {
        uint64_t mask_id = 0;
        SourcePlanes planes;
    };

    // Per-layer trim (spec §5): a layer whose trim selects a different clip
    // frame than the playhead gets its own decoded planes for this render.
    // Callers decode (trim_in + remapped frame, clamped to the segment) and
    // keep the planes alive through render().
    struct LayerSourceFrame {
        int layer_index = -1;
        SourcePlanes planes;
    };

    // cache_ctx != 0 enables the frame render cache (spec §10) for this
    // render: a hit skips the whole graph and re-uploads the stored frame;
    // a miss arms a deferred readback harvested kFramesInFlight renders
    // later (the caller's fence wait makes the copy safe to read). Callers
    // MUST pass 0 when any history-bearing effect is active
    // (doc::document_uses_history) or determinism is not frame-indexed
    // (live mode) — the engine does not re-check.
    // out_source (spec §9 A/B wipe / bypass-all): when non-null, receives
    // the converted linear-RGB source frame (kept alive alongside the
    // final target, SHADER_READ_ONLY) — or nullptr when unavailable (cache
    // hit). Callers comparing A/B should pass cache_ctx 0.
    GpuImage* render(VkCommandBuffer cmd, uint32_t frame_index,
                     const SourcePlanes& source, const doc::Document& doc,
                     uint32_t timeline_frame, double fps,
                     uint64_t overlay_mask_id = 0,
                     const MaskSourceFrame* mask_sources = nullptr,
                     size_t mask_source_count = 0,
                     uint64_t cache_ctx = 0,
                     GpuImage** out_source = nullptr,
                     const LayerSourceFrame* layer_sources = nullptr,
                     size_t layer_source_count = 0);

    RenderCache& cache() { return cache_; }

    VkSampler linear_sampler() const { return linear_sampler_; }
    DescriptorArena& arena() { return arena_; }

    // Glyph renderer (spec §6.6/§12): installs a tile atlas. Slot 0 is the
    // procedural halftone built at init; slot 1 the ASCII ramp; slot 2 a
    // user-droppable custom set (atlas PNG + JSON grid descriptor). R8,
    // tiles ordered dark -> light. Blocking one-shot upload.
    bool set_glyph_atlas(const uint8_t* gray, uint32_t width, uint32_t height,
                         float tile_px = 8.0f, uint32_t cols = 16,
                         uint32_t rows = 6, int slot = 1);
    // Color tileset (emoji etc.): RGBA8 atlas, coverage from alpha, the
    // tile's own hue rendered. Same slots as the gray variant.
    bool set_glyph_atlas_rgba(const uint8_t* rgba, uint32_t width,
                              uint32_t height, float tile_px = 8.0f,
                              uint32_t cols = 16, uint32_t rows = 6,
                              int slot = 2);

    // Preview proxy (spec §10): 1 = full, 2 = half, 4 = quarter. The source
    // planes stay full-res; the working targets shrink (kernels sample by
    // uv, so everything scales). Export uses its own Engine at 1.
    void set_preview_divisor(uint32_t d) {
        preview_divisor_ = d < 1 ? 1 : (d > 4 ? 4 : d);
    }
    uint32_t preview_divisor() const { return preview_divisor_; }

private:
    explicit Engine(Device& device)
        : device_(device), arena_(device), pool_(device) {}

    bool init(const std::filesystem::path& shader_dir);
    bool ensure_planes(uint32_t width, uint32_t height);
    bool ensure_codec_io(uint32_t width, uint32_t height);
    VkCommandBuffer codec_begin_segment();
    void codec_flush_segment();

    Device& device_;
    DescriptorArena arena_;
    TargetPool pool_;
    std::unique_ptr<StagingBuffer> staging_[kFramesInFlight];
    std::unique_ptr<GpuImage> plane_y_, plane_u_, plane_v_;   // R8 uploads
    // Previous frame's luma for flow/motion (spec §4). Valid only when this
    // render's timeline frame directly follows the last one (sequential
    // playback/export); a seek yields zero flow for one frame.
    std::unique_ptr<GpuImage> prev_y_;
    uint32_t last_timeline_frame_ = 0;
    bool have_last_frame_ = false;
    bool prev_frame_valid_ = false;
    VkSampler linear_sampler_ = VK_NULL_HANDLE;
    std::unique_ptr<ComputePipeline> to_rgb_;
    std::unique_ptr<ComputePipeline> fx_[static_cast<size_t>(doc::EffectType::Count)];
    std::unique_ptr<ComputePipeline> mask_shape_, mask_extract_, mask_blur_,
        mask_apply_, mask_morph_, mask_combine_;
    std::unique_ptr<ComputePipeline> glow_pass_[4];   // bright, H, V, comp
    std::unique_ptr<ComputePipeline> flow_;

    // ---- Codec-Box (spec §6.3): mid-graph CPU roundtrip. The graph is
    // evaluated in fenced SEGMENTS when a codec effect is present: render up
    // to the box, read the frame back, run the mosh codec on the CPU
    // (persistent per-instance decoder state), re-upload, continue.
    struct CodecIo {
        std::unique_ptr<GpuImage> nv_y, nv_uv;        // NV12 conversion
        std::unique_ptr<GpuImage> up_y, up_u, up_v;   // I420 re-upload
        std::unique_ptr<GpuImage> up_rgba;            // error-diffusion path
        VkBuffer readback = VK_NULL_HANDLE;
        VmaAllocation readback_alloc = nullptr;
        void* mapped = nullptr;
        size_t capacity = 0;
        std::unique_ptr<StagingBuffer> staging;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
    };
    CodecIo codec_io_;
    struct MoshSlot {
        codec::MoshCodec codec;
        codec::DecodedFrame last_out;   // paused re-render must not restew
        uint32_t last_frame = 0xFFFFFFFFu;
        bool valid = false;
    };
    std::unordered_map<uint64_t, MoshSlot> mosh_state_;
    std::unique_ptr<ComputePipeline> to_nv12_, fx_mix_;
    std::unique_ptr<ComputePipeline> generator_, layer_blend_;

    // Stateful feedback effects (echo/feedback): persistent per-instance
    // previous-output target (the one-frame-delay rule, spec §4). Updated
    // only when the timeline advances so paused re-renders are stable.
    struct FeedbackSlot {
        std::unique_ptr<GpuImage> prev;
        uint32_t last_frame = 0xFFFFFFFFu;
    };
    std::unordered_map<uint64_t, FeedbackSlot> feedback_state_;

    bool upload_gray_oneshot(GpuImage& dst, const uint8_t* gray, uint32_t width,
                             uint32_t height);
    bool upload_rgba_oneshot(GpuImage& dst, const uint8_t* rgba,
                             uint32_t width, uint32_t height);
    // 0 halftone, 1 ascii, 2 custom (spec §12 droppable glyph sets),
    // 3 braille (procedural 2x4 dot cells). A color-flagged slot holds
    // RGBA tiles (emoji sets): coverage from alpha, hue from the tile.
    std::unique_ptr<GpuImage> glyph_atlas_[4];
    bool glyph_atlas_color_[4] = {};
    struct GlyphMeta {
        uint32_t cols = 16, rows = 6;
        float tile = 8.0f;
    };
    GlyphMeta glyph_meta_[4];

    // Dust/damage plate (spec §12 dust textures): assets/textures/dust.png
    // when present, else a procedural grunge fallback — always non-null.
    std::unique_ptr<GpuImage> dust_tex_;

    // Blue-noise / STBN dither LUT (build-time asset; procedural fallback).
    std::unique_ptr<GpuImage> noise_lut_;

    // Slit-scan (spec §6.1): per-instance ring of past INPUT frames. One
    // slot per effect id; pushed once per timeline frame like feedback.
    static constexpr uint32_t kSlitRing = 16;
    struct SlitSlot {
        std::unique_ptr<GpuImage> ring[kSlitRing];
        uint32_t head = 0;      // next write position
        uint32_t count = 0;     // valid entries
        uint32_t last_frame = 0xFFFFFFFFu;
    };
    std::unordered_map<uint64_t, SlitSlot> slit_state_;   // also Stutter rings

    // Frame-rate sim (spec §6.1): the held frame refreshes when the
    // hold-fps tick advances.
    struct HoldSlot {
        std::unique_ptr<GpuImage> held;
        uint32_t last_tick = 0xFFFFFFFFu;
    };
    std::unordered_map<uint64_t, HoldSlot> hold_state_;
    uint32_t preview_divisor_ = 1;

    // Per-mask external source planes (spec §8), uploaded fresh each render
    // that provides frames for that mask id.
    struct MaskPlanes {
        std::unique_ptr<GpuImage> y, u, v;
        uint32_t width = 0, height = 0;
    };
    std::unordered_map<uint64_t, MaskPlanes> mask_planes_;
    std::unique_ptr<ComputePipeline> mask_source_;

    // Per-layer private source planes (spec §5 trim): same upload shape as
    // mask planes, keyed by layer index.
    std::unordered_map<int, MaskPlanes> layer_planes_;
    std::unique_ptr<ComputePipeline> layer_transform_;

    // Bezier shape masks (spec §8): the closed Catmull-Rom through the
    // control points is flattened CPU-side and cached as an Nx1 RG32F
    // texture, re-uploaded only when the points change.
    struct PathSlot {
        std::unique_ptr<GpuImage> tex;
        uint32_t count = 0;
        uint64_t hash = 0;
    };
    std::unordered_map<uint64_t, PathSlot> path_state_;
    std::unique_ptr<GpuImage> path_dummy_;   // bound when no path (unread)

    // Reaction-diffusion (spec §6.6): persistent Gray-Scott (A, B) state,
    // ping-ponged N steps per timeline frame.
    struct RdSlot {
        std::unique_ptr<GpuImage> state[2];
        int cur = 0;
        uint32_t last_frame = 0xFFFFFFFFu;
    };
    std::unordered_map<uint64_t, RdSlot> rd_state_;
    std::unique_ptr<ComputePipeline> rd_step_;

    // CPU error diffusion (spec §6.2): cached output halves + the residual
    // error plane carried into the next frame (temporal carry).
    struct EdSlot {
        std::vector<uint16_t> out;    // RGBA16F halves, linear
        std::vector<float> carry;     // per-pixel RGB quantization error
        std::vector<float> work;      // sRGB working buffer (reused)
        uint32_t last_frame = 0xFFFFFFFFu;
        bool valid = false;
    };
    std::unordered_map<uint64_t, EdSlot> ed_state_;
    void run_error_diffusion(const uint16_t* halves, uint32_t width,
                             uint32_t height, const doc::EffectInstance& fx,
                             EdSlot& slot);

    // Frame render cache (spec §10): per-slot host-visible transfer buffer,
    // used in both directions — miss records image->buffer (harvested into
    // cache_ when the slot comes around again), hit memcpys the stored
    // frame in and records buffer->image.
    struct CacheIo {
        VkBuffer buf = VK_NULL_HANDLE;
        VmaAllocation alloc = nullptr;
        void* mapped = nullptr;
        size_t capacity = 0;
        bool pending = false;         // a recorded, un-harvested readback
        uint64_t pending_ctx = 0;
        uint32_t pending_frame = 0;
        uint32_t pending_w = 0, pending_h = 0;
    };
    CacheIo cache_io_[kFramesInFlight];
    RenderCache cache_;
    bool ensure_cache_io(CacheIo& io, size_t bytes);
};

}  // namespace looks::gfx

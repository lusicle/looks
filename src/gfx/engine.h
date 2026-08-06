// Engine: per-frame evaluation of the compiled render graph.
// Uploads the decoded I420 frame, converts to the linear RGBA16F working
// space in compute, then walks the graph's topological order dispatching
// one kernel per effect over pooled ping-pong targets. Returns the final
// image ready for the viewport blit (and, next milestone, export readback).

#pragma once

#include <filesystem>
#include <memory>
#include <thread>
#include <unordered_map>

#include "codec/mosh/mosh.h"
#include "doc/document.h"
#include "gfx/compute.h"
#include "gfx/error_diffusion.h"
#include "gfx/render_cache.h"
#include "gfx/texture.h"
#include "ui/truetype.h"   // runtime TTF loader for the Text effect

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
    // clocked effects (fixed timestep on frame index).

    // Clip sources: EVERY placement decodes its own frame (docs/look.md
    // phase 4 — there is no shared playhead frame). The caller's decode
    // pool maps each placement to its source frame and keeps the planes
    // alive through render(); a key with no entry renders black.
    struct LayerSourceFrame {
        // The instance-scoped key of the placement these planes belong
        // to: hash(instance path, layer id), matching GraphNode::key.
        uint64_t key = 0;
        SourcePlanes planes;
    };

    // cache_frame is what the cache keys on: the PLAYHEAD frame, not
    // timeline_frame. Under a speed ramp several playhead frames land on
    // one timeline position with different modulation, so the playhead is
    // the only index that identifies an output frame. Ignored when
    // cache_ctx is 0.
    // cache_ctx != 0 enables the frame render cache for this
    // render: a hit skips the whole graph and re-uploads the stored frame;
    // a miss arms a deferred readback harvested kFramesInFlight renders
    // later (the caller's fence wait makes the copy safe to read). Callers
    // MUST pass 0 when any history-bearing effect is active
    // (doc::document_uses_history) or determinism is not frame-indexed
    // (live mode) — the engine does not re-check.
    // out_source (A/B wipe / bypass-all): when non-null, receives
    // the converted linear-RGB REFERENCE source — the first clip source
    // playing in the rendered look (kept alive alongside the final target,
    // SHADER_READ_ONLY) — or nullptr when unavailable (cache hit, or a
    // look with no clip playing). Callers comparing A/B pass cache_ctx 0.
    // preview_node: publish the named node's output instead of
    // the composite (selection-follows preview). preview_layer: publish
    // the named LAYER's whole contribution (chain end, pre-blend);
    // preview_node outranks it. Callers hashing the render-cache context
    // must include both; export passes 0.
    // root_id names which entity renders: the scoped sequence or look in
    // preview, the exported sequence in export.
    // canvas_w/canvas_h are the PROJECT's working resolution
    // (doc::canvas_size) — the clip under the playhead never decides it.
    GpuImage* render(VkCommandBuffer cmd, uint32_t frame_index,
                     const doc::Document& doc,
                     uint64_t root_id, uint32_t timeline_frame, double fps,
                     uint32_t canvas_w, uint32_t canvas_h,
                     uint64_t cache_ctx = 0, uint32_t cache_frame = 0,
                     GpuImage** out_source = nullptr,
                     const LayerSourceFrame* layer_sources = nullptr,
                     size_t layer_source_count = 0,
                     uint64_t preview_node = 0,
                     uint64_t preview_layer = 0);

    RenderCache& cache() { return cache_; }

    VkSampler linear_sampler() const { return linear_sampler_; }
    DescriptorArena& arena() { return arena_; }

    // Glyph renderer: installs a tile atlas. Slot 0 is the
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

    // Audio Scope: mono soundtrack copy (block-decimated, ~16 kHz) the
    // engine slices into a per-frame min/max waveform strip. Call from the
    // thread that renders (worker / export thread) — no internal locking.
    // Empty vector = no audio (the scope draws a flat line).
    void set_scope_audio(std::vector<int16_t> mono, uint32_t sample_rate);

    // Preview proxy: 1 = full, 2 = half, 4 = quarter. The source
    // planes stay full-res; the working targets shrink (kernels sample by
    // uv, so everything scales). Export uses its own Engine at 1.
    void set_preview_divisor(uint32_t d) {
        preview_divisor_ = d < 1 ? 1 : (d > 4 ? 4 : d);
    }
    uint32_t preview_divisor() const { return preview_divisor_; }

    // Node-canvas thumbnails (docs/flow_canvas.md): a fixed 8x8 grid of
    // 160x90 cells, RGBA8 sRGB-encoded, tapped after each layer-chain
    // effect plus the final composite (cell key 0). The cell map reflects
    // the last EVALUATED graph — stale-but-valid across render-cache hits.
    static constexpr uint32_t kThumbCellW = 160, kThumbCellH = 90;
    static constexpr uint32_t kThumbGridCols = 8, kThumbGridRows = 8;
    GpuImage* thumb_atlas() { return thumb_atlas_.get(); }
    const std::unordered_map<uint64_t, uint32_t>& thumb_cells() const {
        return thumb_cells_;
    }

private:
    explicit Engine(Device& device)
        : device_(device), arena_(device), pool_(device) {}

    bool init(const std::filesystem::path& shader_dir);
    bool ensure_prev_ref(uint32_t width, uint32_t height);
    bool ensure_codec_io(uint32_t width, uint32_t height);
    VkCommandBuffer codec_begin_segment();
    void codec_flush_segment();

    Device& device_;
    DescriptorArena arena_;
    TargetPool pool_;
    std::unique_ptr<StagingBuffer> staging_[kFramesInFlight];
    // Previous frame's REFERENCE luma for flow/motion. Valid only when
    // this render's timeline frame directly follows the last one AND the
    // reference is still the same source (sequential playback/export); a
    // seek or a cut yields zero flow for one frame.
    std::unique_ptr<GpuImage> prev_y_;
    // Flat-black stand-in when the look holds no clip source at all.
    std::unique_ptr<GpuImage> dummy_y_;
    uint64_t last_ref_key_ = 0;
    uint32_t last_timeline_frame_ = 0;
    bool have_last_frame_ = false;
    bool prev_frame_valid_ = false;
    VkSampler linear_sampler_ = VK_NULL_HANDLE;
    std::unique_ptr<ComputePipeline> to_rgb_;
    std::unique_ptr<ComputePipeline> fx_[static_cast<size_t>(doc::EffectType::Count)];
    std::unique_ptr<ComputePipeline> matte_extract_, matte_apply_;
    std::unique_ptr<ComputePipeline> glow_pass_[4];   // bright, H, V, comp
    std::unique_ptr<ComputePipeline> flow_;

    // ---- Codec-Box: mid-graph CPU roundtrip. The graph is
    // evaluated in fenced SEGMENTS when a codec effect is present: render up
    // to the box, read the frame back, run the mosh codec on the CPU
    // (persistent per-instance decoder state), re-upload, continue.
    struct CodecIo {
        std::unique_ptr<GpuImage> nv_y, nv_uv;        // NV12 conversion
        std::unique_ptr<GpuImage> up_y, up_u, up_v;   // I420 re-upload
        std::unique_ptr<GpuImage> up_idx;   // error-diffusion packed picks
        // GPU rate control (Bitrate Starve): per-rung DC scratch, bit
        // totals, and the picked quality the wire reads back.
        std::unique_ptr<GpuImage> rate_dc, rate_bits, rate_qsel;
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
    // GPU mosh pipelines + per-instance chain state. Planes are r32ui
    // storage images holding one byte value per texel; state lives on the
    // GPU across timeline frames (the whole point — no readback). The CPU
    // MoshCodec handles the byte-flips / bitrate-budget params, which are
    // the only paths that still need real bytes.
    std::unique_ptr<ComputePipeline> mosh_predict_, mosh_wire_, mosh_unorm_;
    std::unique_ptr<ComputePipeline> mosh_rate_probe_, mosh_rate_reduce_,
        mosh_rate_pick_;
    std::unique_ptr<ComputePipeline> ed_expand_;
    std::unique_ptr<GpuImage> dummy_flow_;   // bound when no flow is wired
    struct MoshGpuSlot {
        std::unique_ptr<GpuImage> clean[3], moshed[3];       // Y, U, V
        std::unique_ptr<GpuImage> pred_clean[3], pred_moshed[3], pred_tmp[3];
        uint32_t w = 0, h = 0;
        uint32_t last_frame = 0xFFFFFFFFu;
        bool has_state = false;
    };
    std::unordered_map<uint64_t, MoshGpuSlot> mosh_gpu_;
    // Runs one codec-box frame fully on the GPU (no readback, no fence)
    // and composites into dst. Only legal when the params need no real
    // bitstream (no byte flips, no bitrate budget).
    // state_key is the instance-scoped slot id (GraphNode::key).
    bool mosh_gpu_box(VkCommandBuffer rec, const doc::EffectInstance& fx,
                      uint64_t state_key,
                      const codec::MoshParams& mp, const GpuImage* in,
                      GpuImage* flow_img, uint32_t w, uint32_t h,
                      uint32_t frame_index, uint32_t timeline_frame,
                      GpuImage* dst);
    std::unique_ptr<ComputePipeline> generator_, layer_blend_;

    // Stateful feedback effects (echo/feedback): persistent per-instance
    // previous-output target (the one-frame-delay rule, ). Updated
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
    // 0 halftone, 1 ascii, 2 custom (droppable glyph sets),
    // 3 braille (procedural 2x4 dot cells), 4 teletext (procedural 2x3
    // block-mosaic sextants). A color-flagged slot holds RGBA tiles
    // (emoji sets): coverage from alpha, hue from the tile.
    std::unique_ptr<GpuImage> glyph_atlas_[5];
    bool glyph_atlas_color_[5] = {};
    struct GlyphMeta {
        uint32_t cols = 16, rows = 6;
        float tile = 8.0f;
    };
    GlyphMeta glyph_meta_[5];

    // Dust/damage plate (dust textures): assets/textures/dust.png
    // when present, else a procedural grunge fallback — always non-null.
    std::unique_ptr<GpuImage> dust_tex_;

    // Text overlay (docs/flow_canvas.md): runtime TTFs from
    // assets/fonts/*.ttf, sorted by filename — the `font` param indexes
    // the list, no bake step. Per-instance string SDFs (truetype.h) are
    // cached per SIZE BUCKET, so a keyframed/modulated size walks a
    // bounded raster set (the kernel scales between buckets — SDFs
    // magnify cleanly) instead of re-rasterizing every frame. A text or
    // font change drops the whole slot.
    std::vector<ui::TtfFont> fx_fonts_;
    static constexpr float kTextBuckets[6] = {12.0f, 24.0f, 48.0f,
                                              96.0f, 192.0f, 400.0f};
    struct TextRaster {
        std::unique_ptr<GpuImage> tex;
        uint32_t w = 0, h = 0;
        float spread = 0.0f;
    };
    struct TextSlot {
        uint64_t hash = 0;   // (text, font) content hash
        TextRaster buckets[6];
    };
    std::unordered_map<uint64_t, TextSlot> text_state_;

    // Blue-noise / STBN dither LUT (build-time asset; procedural fallback).
    std::unique_ptr<GpuImage> noise_lut_;

    // Slit-scan: per-instance ring of past INPUT frames. One
    // slot per effect id; pushed once per timeline frame like feedback.
    static constexpr uint32_t kSlitRing = 16;
    struct SlitSlot {
        std::unique_ptr<GpuImage> ring[kSlitRing];
        uint32_t head = 0;      // next write position
        uint32_t count = 0;     // valid entries
        uint32_t last_frame = 0xFFFFFFFFu;
    };
    std::unordered_map<uint64_t, SlitSlot> slit_state_;   // also Stutter rings

    // Frame-rate sim: the held frame refreshes when the
    // hold-fps tick advances.
    struct HoldSlot {
        std::unique_ptr<GpuImage> held;
        uint32_t last_tick = 0xFFFFFFFFu;
    };
    std::unordered_map<uint64_t, HoldSlot> hold_state_;
    uint32_t preview_divisor_ = 1;

    // Private source planes for placed layers, uploaded fresh each render
    // that provides frames for them. Keyed by the INSTANCE-scoped node key
    // (docs/look.md), not a layer index: two placements of one look each
    // want their own decoded frame.
    struct LayerPlanes {
        std::unique_ptr<GpuImage> y, u, v;
        uint32_t width = 0, height = 0;
    };
    std::unordered_map<uint64_t, LayerPlanes> layer_planes_;
    std::unique_ptr<ComputePipeline> layer_transform_;

    // Reaction-diffusion: persistent Gray-Scott (A, B) state,
    // ping-ponged N steps per timeline frame.
    struct RdSlot {
        std::unique_ptr<GpuImage> state[2];
        int cur = 0;
        uint32_t last_frame = 0xFFFFFFFFu;
    };
    std::unordered_map<uint64_t, RdSlot> rd_state_;
    std::unique_ptr<ComputePipeline> rd_step_;

    // Velocity scan (dwell-time rendering): per-instance ping-pong front
    // field — one row of sweep-front positions per concurrent line, one
    // column per along-axis texel. The phosphor canvas rides the shared
    // feedback machinery (own previous output).
    static constexpr uint32_t kVsSlots = 24;
    struct VsSlot {
        std::unique_ptr<GpuImage> state[2];
        int cur = 0;
        uint32_t last_frame = 0xFFFFFFFFu;
    };
    std::unordered_map<uint64_t, VsSlot> vs_state_;
    std::unique_ptr<ComputePipeline> vs_front_;

    // Engraver (FM raster): per-frame phase-integral scratch — each
    // lane accumulates omega + distortion * signal across the frame
    // (RGBA32F: R/G/B channel phases + luma phase). Recomputed by
    // mod_integrate_ before every Engraver dispatch, so one shared
    // scratch serves any number of instances (graph eval is sequential).
    std::unique_ptr<GpuImage> mod_integral_;
    std::unique_ptr<ComputePipeline> mod_integrate_;

    // Node-canvas thumbnail atlas (see public accessors). Allocated once,
    // fixed size — no per-frame Vulkan object churn.
    std::unique_ptr<GpuImage> thumb_atlas_;
    std::unique_ptr<ComputePipeline> thumb_tap_;
    std::unordered_map<uint64_t, uint32_t> thumb_cells_;
    uint32_t thumb_next_cell_ = 0;
    void record_thumb_tap(VkCommandBuffer rec, uint32_t frame_index,
                          GpuImage* src, uint64_t key);

    // Audio Scope (sidechain family): per-instance 1-D min/max
    // waveform strip re-uploaded each render from the mono PCM copy.
    static constexpr uint32_t kAudioStripBins = 1024;
    std::unordered_map<uint64_t, std::unique_ptr<GpuImage>> audio_strip_;
    std::vector<int16_t> scope_audio_;
    uint32_t scope_rate_ = 0;

    // CPU error diffusion (gfx/error_diffusion.h) runs ONE FRAME BEHIND —
    // the same legal delay as Feedback's cycle exemption: frame N
    // composites the walk of frame N-1's input while frame N's input is
    // captured and walked on a worker thread during the rest of the
    // frame. The serial walk overlaps GPU time instead of stalling it.
    struct EdSlotAsync {
        EdState ed;
        std::vector<uint16_t> input;   // captured RGBA16F halves
        doc::EffectInstance pending_fx;
        std::thread worker;
        bool busy = false;             // render-thread-owned
        bool has_result = false;
        uint32_t result_w = 0, result_h = 0;
        uint32_t captured_frame = 0xFFFFFFFFu;
    };
    std::unordered_map<uint64_t, std::unique_ptr<EdSlotAsync>> ed_state_;
    bool composite_ed(VkCommandBuffer rec, const doc::EffectInstance& fx,
                      const GpuImage* in_img, const EdState& ed, uint32_t w,
                      uint32_t h, uint32_t frame_index, GpuImage* dst);

public:
    // True while a deferred dither walk is in flight. The render worker
    // uses this to schedule one settle pass instead of idling, so paused
    // frames (and a freshly applied effect on a paused frame) pick up the
    // finished walk without waiting for user interaction.
    bool ed_walk_pending() const {
        for (const auto& [id, slot] : ed_state_)
            if (slot && slot->busy) return true;
        return false;
    }

private:

    // Frame render cache: per-slot host-visible transfer buffer,
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

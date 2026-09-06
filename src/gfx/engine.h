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
#include "ui/truetype.h"

namespace looks::gfx {

// I420 planes; strides are bytes per row, U and V half size rounded up.
struct SourcePlanes {
    const uint8_t* y = nullptr;
    size_t y_stride = 0;
    const uint8_t* u = nullptr;
    size_t u_stride = 0;
    const uint8_t* v = nullptr;
    size_t v_stride = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    // NV12: u is the interleaved CbCr plane and v is unused.
    bool nv12 = false;
};

class Engine {
public:
    // submit_queue takes the engine's internal submissions; null = graphics.
    // A caller that submits on another queue must pass that queue here.
    static std::unique_ptr<Engine> create(
        Device& device, const std::filesystem::path& shader_dir,
        VkQueue submit_queue = VK_NULL_HANDLE);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // cmd must be outside any render pass.
    // The result is SHADER_READ_ONLY and is valid until the next render().

    // The caller keeps the planes alive through render().
    // A key with no entry renders black.
    struct LayerSourceFrame {
        // hash(instance path, layer id); it must match GraphNode::key.
        uint64_t key = 0;
        // The upload is skipped when the stamp repeats; 0 = always upload.
        uint64_t content_stamp = 0;
        SourcePlanes planes;
    };

    // cache_frame is the playhead frame, not timeline_frame.
    // Callers must pass cache_ctx 0 for history effects; no re-check here.
    // out_source gets the reference source, alive with the final target.
    // measure_placement is preview-only and bypasses the cache lookup.
    // cache_store gates only the write side; cache hits stay on either way.
    GpuImage* render(VkCommandBuffer cmd, uint32_t frame_index,
                     const doc::Document& doc,
                     uint64_t root_id, uint32_t timeline_frame, double fps,
                     uint32_t canvas_w, uint32_t canvas_h,
                     uint64_t cache_ctx = 0, uint32_t cache_frame = 0,
                     GpuImage** out_source = nullptr,
                     const LayerSourceFrame* layer_sources = nullptr,
                     size_t layer_source_count = 0,
                     uint64_t preview_node = 0,
                     uint64_t preview_layer = 0,
                     uint64_t measure_placement = 0,
                     bool cache_store = true);

    // Alpha bounds as a canvas-fraction rect {x, y, w, h}.
    // Valid only after the submission that recorded it has fenced.
    bool read_measure_bounds(float rect[4]) const;
    // A pipelined caller snapshots this and reads bounds at that fence.
    bool measure_recorded() const;

    RenderCache& cache() { return cache_; }

    VkSampler linear_sampler() const { return linear_sampler_; }
    DescriptorArena& arena() { return arena_; }

    // Slot 0 = halftone, 1 = ASCII ramp, 2 = custom; R8, dark to light.
    // The upload blocks until it completes.
    bool set_glyph_atlas(const uint8_t* gray, uint32_t width, uint32_t height,
                         float tile_px = 8.0f, uint32_t cols = 16,
                         uint32_t rows = 6, int slot = 1);
    // RGBA8 atlas; alpha gives coverage and the tile keeps its own hue.
    bool set_glyph_atlas_rgba(const uint8_t* rgba, uint32_t width,
                              uint32_t height, float tile_px = 8.0f,
                              uint32_t cols = 16, uint32_t rows = 6,
                              int slot = 2);

    // Call from the render thread only; there is no internal locking.
    void set_scope_audio(std::vector<int16_t> mono, uint32_t sample_rate);

    // Same thread contract as set_scope_audio.
    // A pin with no entry uses the identity homography.
    struct PinPlane {
        uint32_t start = 0;
        std::vector<float> h;   // 9 floats per frame from start
    };
    using PinPlaneMap = std::unordered_map<uint64_t, PinPlane>;
    void set_track_planes(std::shared_ptr<const PinPlaneMap> planes) {
        pin_planes_ = std::move(planes);
    }
    // Both sides must quantize the same way to agree on this key.
    static uint64_t pin_plane_key(uint64_t asset, float rx, float ry,
                                  float rw, float rh);

    // Preview divisor: 1 = full, 2 = half, 4 = quarter.
    // The source planes stay full size; only the working targets shrink.
    void set_preview_divisor(uint32_t d) {
        preview_divisor_ = d < 1 ? 1 : (d > 4 ? 4 : d);
    }
    uint32_t preview_divisor() const { return preview_divisor_; }

    // Thumbnail cells are RGBA8 and sRGB encoded; cell key 0 is the composite.
    // A node absent from the map was not evaluated.
    static constexpr uint32_t kThumbCellW = 160, kThumbCellH = 90;
    static constexpr uint32_t kThumbGridCols = 8, kThumbGridRows = 8;
    GpuImage* thumb_atlas() { return thumb_atlas_.get(); }
    const std::unordered_map<uint64_t, uint32_t>& thumb_cells() const {
        return thumb_cells_;
    }

    // The worker assigns gallery cells and they persist across renders.
    // The tap records into the same command buffer, after render().
    static constexpr uint32_t kGalleryCellW = 160, kGalleryCellH = 90;
    static constexpr uint32_t kGalleryCols = 8, kGalleryRows = 8;
    GpuImage* gallery_atlas() { return gallery_atlas_.get(); }
    void record_gallery_tap(VkCommandBuffer rec, uint32_t frame_index,
                            GpuImage* src, uint32_t cell);

private:
    // Internal submissions land here to order before the caller's submit.
    VkQueue submit_queue_ = VK_NULL_HANDLE;

    explicit Engine(Device& device)
        : device_(device), arena_(device),
          pools_{TargetPool(device), TargetPool(device)} {}

    bool init(const std::filesystem::path& shader_dir);
    bool ensure_prev_ref(uint32_t width, uint32_t height);
    bool ensure_codec_io(uint32_t width, uint32_t height);
    VkCommandBuffer codec_begin_segment();
    void codec_flush_segment();
    void dispatch_to_nv12(VkCommandBuffer rec, uint32_t frame_index,
                          const GpuImage* in, uint32_t w, uint32_t h);

    Device& device_;
    DescriptorArena arena_;
    TargetPool pools_[kFramesInFlight];
    // Per-slot pools stop a GPU data race between frames in flight.
    TargetPool* pool_ = nullptr;
    std::unique_ptr<StagingBuffer> staging_[kFramesInFlight];
    // Previous reference luma; valid only on the next frame, same source.
    std::unique_ptr<GpuImage> prev_y_;
    // Flat-black stand-in when the look holds no media source at all.
    std::unique_ptr<GpuImage> dummy_y_;
    uint64_t last_ref_key_ = 0;
    uint32_t last_timeline_frame_ = 0;
    bool have_last_frame_ = false;
    bool prev_frame_valid_ = false;
    VkSampler linear_sampler_ = VK_NULL_HANDLE;
    std::unique_ptr<ComputePipeline> to_rgb_;
    // Alpha bounds: a 4x1 r32ui atomic min/max target and a mapped readback.
    // The UI harvests it after the worker fence; export never uses it.
    std::unique_ptr<ComputePipeline> alpha_bounds_;
    std::unique_ptr<GpuImage> bounds_img_;
    VkBuffer bounds_buf_ = VK_NULL_HANDLE;
    VmaAllocation bounds_alloc_ = nullptr;
    void* bounds_mapped_ = nullptr;
    bool bounds_recorded_ = false;
    uint32_t bounds_w_ = 0, bounds_h_ = 0;
    // The cache lookup is bypassed only until this selection measures once.
    uint64_t measured_placement_ = 0;
    std::unique_ptr<ComputePipeline> fx_[static_cast<size_t>(doc::EffectType::Count)];
    std::unique_ptr<ComputePipeline> matte_extract_, matte_apply_;
    std::unique_ptr<ComputePipeline> glow_pass_[4];   // bright, H, V, comp
    std::unique_ptr<ComputePipeline> flow_;
    std::unique_ptr<ComputePipeline> crt_prepare_, crt_blur_;
    struct CrtSlot {
        std::unique_ptr<GpuImage> previous, current;
        uint32_t last_frame = 0xFFFFFFFFu;
        bool previous_valid = false;
    };
    std::unordered_map<uint64_t, CrtSlot> crt_state_;
    std::vector<std::unique_ptr<GpuImage>> crt_retired_[kFramesInFlight];

    // A codec effect splits the graph into fenced CPU roundtrip segments.
    struct CodecIo {
        std::unique_ptr<GpuImage> nv_y, nv_uv;        // NV12 conversion
        std::unique_ptr<GpuImage> up_y, up_u, up_v;   // I420 re-upload
        std::unique_ptr<GpuImage> up_idx;   // error-diffusion packed picks
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
        // A paused edit re-arms the gate; advancing frames never reset.
        uint64_t sig = 0;
        bool valid = false;
    };
    std::unordered_map<uint64_t, MoshSlot> mosh_state_;
    std::unique_ptr<ComputePipeline> to_nv12_, fx_mix_;
    std::unique_ptr<ComputePipeline> group_mix_;
    // Mosh planes are r32ui storage images with one byte value per texel.
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
        uint64_t sig = 0;   // paused-edit re-arm, same contract as MoshSlot
        bool has_state = false;
    };
    std::unordered_map<uint64_t, MoshGpuSlot> mosh_gpu_;
    // Legal only when the params need no real bitstream.
    // state_key is the instance-scoped slot id, the same as GraphNode::key.
    bool mosh_gpu_box(VkCommandBuffer rec, const doc::EffectInstance& fx,
                      uint64_t state_key,
                      const codec::MoshParams& mp, const GpuImage* in,
                      GpuImage* flow_img, uint32_t w, uint32_t h,
                      uint32_t frame_index, uint32_t timeline_frame,
                      GpuImage* dst);
    std::unique_ptr<ComputePipeline> generator_, layer_blend_;

    // Updated only when the timeline advances, so paused re-renders match.
    struct FeedbackSlot {
        std::unique_ptr<GpuImage> prev;
        uint32_t last_frame = 0xFFFFFFFFu;
    };
    std::unordered_map<uint64_t, FeedbackSlot> feedback_state_;
    // Recreated on size change and left SHADER_READ_ONLY for the pass.
    FeedbackSlot* ensure_feedback_prev(VkCommandBuffer rec, uint64_t skey,
                                       uint32_t w, uint32_t h);
    void feedback_writeback(VkCommandBuffer rec, FeedbackSlot& slot,
                            GpuImage* dst, uint32_t w, uint32_t h,
                            uint32_t timeline_frame);

    bool upload_oneshot(GpuImage& dst, const uint8_t* pixels,
                        size_t bytes_per_pixel, uint32_t width,
                        uint32_t height);
    bool set_glyph_atlas_impl(const uint8_t* pixels, uint32_t width,
                              uint32_t height, float tile_px, uint32_t cols,
                              uint32_t rows, int slot, bool color);
    // Slots: 0 halftone, 1 ascii, 2 custom, 3 braille, 4 teletext.
    // A color-flagged slot holds RGBA tiles: alpha is coverage.
    std::unique_ptr<GpuImage> glyph_atlas_[5];
    bool glyph_atlas_color_[5] = {};
    struct GlyphMeta {
        uint32_t cols = 16, rows = 6;
        float tile = 8.0f;
    };
    GlyphMeta glyph_meta_[5];

    // Falls back to procedural grunge; this is always non-null.
    std::unique_ptr<GpuImage> dust_tex_;

    // The font param indexes assets/fonts/*.ttf, sorted by filename.
    // SDFs cache per size bucket; a text or font change drops the slot.
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

    // Keyed on (path bytes, closed, raster dims); other edits reuse it.
    struct ShapeSlot {
        uint64_t hash = 0;
        std::unique_ptr<GpuImage> tex;
        uint32_t w = 0, h = 0;
    };
    std::unordered_map<uint64_t, ShapeSlot> shape_state_;
    static constexpr uint32_t kRampTexels = 256;
    struct RampSlot {
        uint64_t hash = 0;
        std::unique_ptr<GpuImage> tex;
    };
    std::unordered_map<uint64_t, RampSlot> ramp_state_;
    const GpuImage* ensure_gradient_ramp(VkCommandBuffer rec,
                                         StagingBuffer& staging,
                                         const doc::Layer& layer);
    std::shared_ptr<const PinPlaneMap> pin_planes_;

    std::unique_ptr<GpuImage> noise_lut_;

    // Ring of past input frames, one slot per effect id, one push per frame.
    static constexpr uint32_t kSlitRing = 16;
    struct SlitSlot {
        std::unique_ptr<GpuImage> ring[kSlitRing];
        uint32_t head = 0;      // next write position
        uint32_t count = 0;     // valid entries
        uint32_t last_frame = 0xFFFFFFFFu;
    };
    std::unordered_map<uint64_t, SlitSlot> slit_state_;
    // A size change drops the ring; the old history has a different size.
    SlitSlot& slit_ring_slot(uint64_t skey, uint32_t w, uint32_t h);
    // Records the input once per timeline frame; false = allocation failure.
    bool slit_ring_push(VkCommandBuffer rec, SlitSlot& slot, GpuImage* in_img,
                        uint32_t w, uint32_t h, uint32_t timeline_frame);
    // Age 0 or past the recorded span reads the live input.
    static const GpuImage* slit_history(const SlitSlot& slot,
                                        GpuImage* in_img, uint32_t age);

    // The held frame refreshes when the hold-fps tick advances.
    struct HoldSlot {
        std::unique_ptr<GpuImage> held;
        uint32_t last_tick = 0xFFFFFFFFu;
    };
    std::unordered_map<uint64_t, HoldSlot> hold_state_;
    uint32_t preview_divisor_ = 1;

    // Keyed by the instance-scoped node key, not by a layer index.
    struct LayerPlanes {
        std::unique_ptr<GpuImage> y, u, v;   // v empty for NV12 media
        uint32_t width = 0, height = 0;
        bool nv12 = false;   // u is the interleaved RG8 CbCr texture
        uint64_t stamp = 0;  // content already uploaded (0 = none)
    };
    std::unordered_map<uint64_t, LayerPlanes> layer_planes_;
    std::unique_ptr<ComputePipeline> layer_transform_;

    // Persistent state, stepped once per timeline frame.
    struct RdSlot {
        std::unique_ptr<GpuImage> state[2];
        int cur = 0;
        uint32_t last_frame = 0xFFFFFFFFu;
    };
    std::unordered_map<uint64_t, RdSlot> rd_state_;
    std::unique_ptr<ComputePipeline> rd_step_;
    // Steps once per timeline frame; the result is SHADER_READ_ONLY.
    GpuImage* rd_advance(VkCommandBuffer rec, uint32_t frame_index,
                         uint64_t skey, const GpuImage* in, uint32_t w,
                         uint32_t h, uint32_t timeline_frame, uint32_t steps,
                         float feed, float kill, float inject);

    // Front field layout: one row per line, one column per along-axis texel.
    static constexpr uint32_t kVsSlots = 24;
    struct VsSlot {
        std::unique_ptr<GpuImage> state[2];
        int cur = 0;
        uint32_t last_frame = 0xFFFFFFFFu;
    };
    std::unordered_map<uint64_t, VsSlot> vs_state_;
    std::unique_ptr<ComputePipeline> vs_front_;

    // RGBA32F scratch: R, G, B channel phases plus the luma phase.
    // mod_integrate_ refills it before every dispatch, so instances share it.
    std::unique_ptr<GpuImage> mod_integral_;
    std::unique_ptr<ComputePipeline> mod_integrate_;

    // Allocated once at a fixed size; no per-frame Vulkan object churn.
    std::unique_ptr<GpuImage> thumb_atlas_;
    std::unique_ptr<ComputePipeline> thumb_tap_;
    std::unordered_map<uint64_t, uint32_t> thumb_cells_;
    uint32_t thumb_next_cell_ = 0;
    void record_thumb_tap(VkCommandBuffer rec, uint32_t frame_index,
                          GpuImage* src, uint64_t key);

    // Allocated lazily at a fixed size.
    std::unique_ptr<GpuImage> gallery_atlas_;
    std::unique_ptr<ComputePipeline> gallery_tap_;

    static constexpr uint32_t kAudioStripBins = 1024;
    std::unordered_map<uint64_t, std::unique_ptr<GpuImage>> audio_strip_;
    std::vector<int16_t> scope_audio_;
    uint32_t scope_rate_ = 0;

    // CPU error diffusion runs one frame behind, on a worker thread.
    // Frame N composites the walk of frame N-1's input.
    struct EdSlotAsync {
        EdState ed;
        std::vector<uint16_t> input;   // captured RGBA16F halves
        doc::EffectInstance pending_fx;
        std::thread worker;
        bool busy = false;             // render-thread-owned
        bool has_result = false;
        uint32_t result_w = 0, result_h = 0;
        uint32_t captured_frame = 0xFFFFFFFFu;
        // A paused edit kicks a fresh walk instead of reusing the old one.
        uint64_t sig = 0;
    };
    std::unordered_map<uint64_t, std::unique_ptr<EdSlotAsync>> ed_state_;
    bool composite_ed(VkCommandBuffer rec, const doc::EffectInstance& fx,
                      const GpuImage* in_img, const EdState& ed, uint32_t w,
                      uint32_t h, uint32_t frame_index, GpuImage* dst);
    // The up_* planes are working-size, so this converts at identity fit.
    void codec_planes_to_rgb(VkCommandBuffer rec, uint32_t frame_index,
                             GpuImage* temp, uint32_t w, uint32_t h);
    // Composites temp over in into dst and returns temp to the pool.
    void mix_composite_release(VkCommandBuffer rec, uint32_t frame_index,
                               const GpuImage* in, GpuImage* temp,
                               GpuImage* dst, float wet, float opacity,
                               uint32_t w, uint32_t h);

public:
    // True while a deferred dither walk is in flight.
    bool ed_walk_pending() const {
        for (const auto& [id, slot] : ed_state_)
            if (slot && slot->busy) return true;
        return false;
    }

private:

    // Host-visible transfer buffer used in both directions.
    // A miss harvests when the same frame slot comes around again.
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

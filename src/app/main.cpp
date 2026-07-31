// looks — milestone 3: engine end-to-end.
//
// Source (imported clip via the player, or an animated test pattern) →
// I420 upload → YCbCr→linear compute → effect chain over pooled RGBA16F
// targets → letterboxed viewport blit, with the UI composited on top.
// Sidebar: transport + effect stack + inspector; every document mutation is
// an undoable command (param drags coalesce into one step).

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "codec/mez.h"
#include "doc/command.h"
#include "doc/document.h"
#include "doc/effects.h"
#include "doc/stack_commands.h"
#include "gfx/engine.h"
#include "gfx/readback.h"
#include "gfx/renderer.h"
#include "gfx/viewport_pass.h"
#include "media/export.h"
#include "media/import.h"
#include "media/pcm.h"
#include "media/player.h"
#include <map>

#include "doc/group_commands.h"
#include "doc/layer_commands.h"
#include "doc/mask_commands.h"
#include "doc/mod_commands.h"
#include "doc/preset.h"
#include "doc/randomize.h"
#include "doc/serialize.h"
#include "util/hash.h"
#include "mod/analysis.h"
#include "mod/eval.h"
#include "mod/param_table.h"
#include "platform/dialog.h"
#include "platform/window.h"
#include "ui/canvas2d.h"
#include "ui/font.h"
#include "ui/layout.h"
#include "ui/text.h"
#include "ui/ui_renderer.h"
#include "ui/widgets.h"
#include "util/file.h"
#include "util/image.h"
#include "util/log.h"

#include "app/flow_canvas.h"

namespace {

using namespace looks;

void fatal_dialog(const wchar_t* message) {
    MessageBoxW(nullptr, message, L"looks", MB_ICONERROR | MB_OK);
}

// ---------------------------------------------------------------- source

// Animated I420 fallback source: 75% color bars + a moving stripe, so the
// proof effects are visible before any clip is imported. Deterministic on
// the frame counter.
struct TestPattern {
    static constexpr uint32_t kWidth = 640;
    static constexpr uint32_t kHeight = 360;

    std::vector<uint8_t> y, u, v;

    TestPattern() {
        y.resize(kWidth * kHeight);
        u.resize((kWidth / 2) * (kHeight / 2));
        v.resize((kWidth / 2) * (kHeight / 2));
    }

    static void rgb_to_ycbcr(float r, float g, float b, uint8_t* out) {
        const float yn = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        const float cb = (b - yn) / 1.8556f;
        const float cr = (r - yn) / 1.5748f;
        out[0] = static_cast<uint8_t>(16.5f + 219.0f * yn);
        out[1] = static_cast<uint8_t>(128.5f + 224.0f * cb);
        out[2] = static_cast<uint8_t>(128.5f + 224.0f * cr);
    }

    void generate(uint64_t t) {
        static constexpr float kBars[8][3] = {
            {0.75f, 0.75f, 0.75f}, {0.75f, 0.75f, 0.0f}, {0.0f, 0.75f, 0.75f},
            {0.0f, 0.75f, 0.0f},   {0.75f, 0.0f, 0.75f}, {0.75f, 0.0f, 0.0f},
            {0.0f, 0.0f, 0.75f},   {0.1f, 0.1f, 0.1f},
        };
        uint8_t bar_ycc[8][3];
        for (int i = 0; i < 8; ++i)
            rgb_to_ycbcr(kBars[i][0], kBars[i][1], kBars[i][2], bar_ycc[i]);

        const uint32_t stripe_x = static_cast<uint32_t>((t * 3) % kWidth);
        for (uint32_t py = 0; py < kHeight; ++py) {
            for (uint32_t px = 0; px < kWidth; ++px) {
                const uint32_t bar = px * 8 / kWidth;
                uint8_t luma = bar_ycc[bar][0];
                const uint32_t dx = (px + kWidth - stripe_x) % kWidth;
                if (dx < 24) luma = 235;
                y[py * kWidth + px] = luma;
            }
        }
        for (uint32_t py = 0; py < kHeight / 2; ++py) {
            for (uint32_t px = 0; px < kWidth / 2; ++px) {
                const uint32_t bar = (px * 2) * 8 / kWidth;
                const uint32_t dx = (px * 2 + kWidth - stripe_x) % kWidth;
                const bool stripe = dx < 24;
                u[py * (kWidth / 2) + px] = stripe ? 128 : bar_ycc[bar][1];
                v[py * (kWidth / 2) + px] = stripe ? 128 : bar_ycc[bar][2];
            }
        }
    }

    gfx::SourcePlanes planes() const {
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

// ---------------------------------------------------------------- import

struct ImportJob {
    std::thread thread;
    media::ImportProgress progress;
    media::ImportResult result;
    std::atomic<bool> done{false};
    std::filesystem::path source;

    ~ImportJob() {
        progress.cancel = true;
        if (thread.joinable()) thread.join();
    }
};

// Import bundles live on the scratch disk (spec §3), never next to the
// user's footage: cache/<source-path-hash>/ beside the exe. Hashing the
// lowercased absolute path keeps one bundle per source on Windows'
// case-insensitive filesystems.
std::filesystem::path bundle_dir_for(const std::filesystem::path& source) {
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(source, ec);
    if (ec) abs = source;
    std::wstring key = abs.native();
    for (wchar_t& c : key) c = static_cast<wchar_t>(towlower(c));
    uint64_t hash = 14695981039346656037ull;
    for (const wchar_t c : key)
        hash = (hash ^ static_cast<uint64_t>(c)) * 1099511628211ull;
    wchar_t hex[17];
    swprintf(hex, 17, L"%016llx", static_cast<unsigned long long>(hash));
    return executable_dir() / "cache" / hex;
}

// Still-image clips (spec §3 import scope: PNG/TGA) get different clip UI:
// a duration entry instead of the time/audio rows, which are meaningless
// when every frame is identical and there is no clip audio.
bool is_still_source(const std::filesystem::path& source) {
    std::wstring ext = source.extension().wstring();
    for (wchar_t& c : ext) c = static_cast<wchar_t>(towlower(c));
    return ext == L".png" || ext == L".tga";
}

std::unique_ptr<ImportJob> start_import(const std::filesystem::path& source,
                                        bool lossless,
                                        const std::filesystem::path& dest) {
    auto job = std::make_unique<ImportJob>();
    job->source = source;
    ImportJob* raw = job.get();
    job->thread = std::thread([raw, source, lossless, dest] {
        media::ImportOptions options;
        if (lossless) options.quality = 0;   // spec §3 lossless mode
        raw->result =
            media::import_media(source, dest, options, &raw->progress);
        raw->done = true;
    });
    return job;
}

struct AppState;
void open_source(AppState& app, const std::filesystem::path& picked);

// ---------------------------------------------------------------- export

struct ExportJob {
    std::thread thread;
    media::ExportProgress progress;
    media::ExportResult result;
    std::atomic<bool> done{false};
    std::filesystem::path out_path;

    ~ExportJob() {
        progress.cancel = true;
        if (thread.joinable()) thread.join();
    }
};

// External mask-source decode state (spec §8), keyed by mask id. One
// instance per consumer (preview loop / export worker) — MezReader is not
// shareable across threads. Map nodes are address-stable, so the returned
// plane pointers stay valid until the next collect().
struct MaskSourceReaders {
    struct Entry {
        codec::MezReader reader;
        std::string path;
        bool ok = false;
        codec::DecodedFrame frame;
        uint32_t last_index = 0xFFFFFFFFu;
    };
    std::map<uint64_t, Entry> entries;

    std::vector<gfx::Engine::MaskSourceFrame> collect(
        const doc::Document& doc, uint32_t timeline_frame) {
        std::vector<gfx::Engine::MaskSourceFrame> out;
        for (const doc::Mask& mask : doc.masks) {
            if (mask.source_path.empty() ||
                mask.type == doc::MaskType::Shape)
                continue;
            Entry& st = entries[mask.id];
            if (st.path != mask.source_path || !st.ok) {
                st.reader.close();
                std::string err;
                st.ok = st.reader.open(mask.source_path, &err);
                st.path = mask.source_path;
                st.last_index = 0xFFFFFFFFu;
                if (!st.ok) continue;
            }
            const uint32_t count = st.reader.frame_count();
            if (count == 0) continue;
            // Locked follows the playhead and holds on the last frame;
            // free-running loops forever (deterministic either way).
            const uint32_t idx = mask.free_run
                                     ? timeline_frame % count
                                     : std::min(timeline_frame, count - 1);
            if (st.last_index != idx) {
                if (!st.reader.decode(idx, st.frame)) continue;
                st.last_index = idx;
            }
            const codec::FrameView view = st.frame.view();
            gfx::Engine::MaskSourceFrame mf;
            mf.mask_id = mask.id;
            mf.planes.y = view.y.data;
            mf.planes.y_stride = view.y.stride;
            mf.planes.u = view.u.data;
            mf.planes.u_stride = view.u.stride;
            mf.planes.v = view.v.data;
            mf.planes.v_stride = view.v.stride;
            mf.planes.width = view.width;
            mf.planes.height = view.height;
            out.push_back(mf);
        }
        return out;
    }
};

// Per-layer trim decode state (spec §5): a clip layer whose trim selects a
// different source frame than the playhead decodes through its own random-
// access reader (intra-only mezzanine makes these seeks free). One instance
// per consumer, same threading rules as MaskSourceReaders.
struct LayerTrimReaders {
    struct Entry {
        codec::MezReader reader;
        std::filesystem::path path;
        bool ok = false;
        codec::DecodedFrame frame;
        uint32_t last_index = 0xFFFFFFFFu;
    };
    Entry entries[doc::kMaxLayers];

    // `playhead_src` is the (already time-remapped) clip frame the shared
    // source planes hold; layers whose trimmed frame matches it just use
    // those. Returned plane pointers stay valid until the next collect().
    std::vector<gfx::Engine::LayerSourceFrame> collect(
        const doc::Document& doc, const std::filesystem::path& mez_path,
        uint32_t playhead_src, uint32_t clip_frames) {
        std::vector<gfx::Engine::LayerSourceFrame> out;
        if (clip_frames == 0 || mez_path.empty()) return out;
        for (size_t li = 0; li < doc.layers.size() && li < doc::kMaxLayers;
             ++li) {
            const doc::Layer& layer = doc.layers[li];
            if (!layer.visible || !doc::layer_has_trim(layer)) continue;
            // Segment [in, out) of the clip; holds the last trimmed frame
            // past the end (a blank layer helps nobody in a looping tool).
            const uint32_t in =
                std::min(layer.trim_in, clip_frames - 1);
            const uint32_t end = layer.trim_out > 0
                                     ? std::min(layer.trim_out, clip_frames)
                                     : clip_frames;
            const uint32_t last = end > in ? end - 1 : in;
            const uint32_t idx =
                std::min<uint64_t>(static_cast<uint64_t>(in) + playhead_src,
                                   last);
            if (idx == playhead_src) continue;   // shared planes are right
            Entry& st = entries[li];
            if (st.path != mez_path || !st.ok) {
                st.reader.close();
                std::string err;
                st.ok = st.reader.open(mez_path, &err);
                st.path = mez_path;
                st.last_index = 0xFFFFFFFFu;
                if (!st.ok) continue;
            }
            if (st.last_index != idx) {
                if (!st.reader.decode(idx, st.frame)) continue;
                st.last_index = idx;
            }
            const codec::FrameView view = st.frame.view();
            gfx::Engine::LayerSourceFrame lf;
            lf.layer_index = static_cast<int>(li);
            lf.planes.y = view.y.data;
            lf.planes.y_stride = view.y.stride;
            lf.planes.u = view.u.data;
            lf.planes.u_stride = view.u.stride;
            lf.planes.v = view.v.data;
            lf.planes.v_stride = view.v.stride;
            lf.planes.width = view.width;
            lf.planes.height = view.height;
            out.push_back(lf);
        }
        return out;
    }
};

// ---------------------------------------------------- preview render thread
//
// Spec §13's fourth thread: the preview graph evaluates OFF the UI thread,
// so a heavy stack — or a fenced Codec-Box roundtrip — slows the viewport,
// never the interface. The worker owns the preview Engine outright. The UI
// posts latest-wins snapshots of the document (copied only when its
// revision moves) and samples the newest published frame; published images
// are triple-buffered and only rewritten once every UI submission that
// sampled them has retired. Player transport calls are thread-safe by
// design (the audio callback is the clock); the UI holds the worker paused
// around player open/close, where MezReader ownership moves.
struct RenderWorker {
    struct Job {
        doc::Document doc;
        uint64_t doc_revision = ~0ull;
        mod::AnalysisCurves analysis;
        bool has_analysis = false;
        uint64_t analysis_stamp = ~0ull;
        std::filesystem::path mez_path;   // full-res (layer-trim readers)
        uint64_t overlay_mask_id = 0;
        // Selection-follows preview (v5.4): node whose output the big
        // preview publishes; 0 = the composite.
        uint64_t preview_node = 0;
        uint32_t preview_div = 1;
        bool live_mode = false;
        double app_seconds = 0.0;
        double env_key_time = -1.0;
        bool want_source = false;         // A/B wipe or bypass-all
        bool have_clip = false;
        bool proxy_active = false;        // cache-context ingredient
        // Custom glyph set hand-off (spec §12): the drop happens on the UI
        // thread, the engine lives here. Gray ramp bytes, or RGBA when
        // `glyph_is_color` (emoji tilesets — coverage from alpha).
        std::vector<uint8_t> glyph_data;
        uint32_t glyph_w = 0, glyph_h = 0, glyph_cols = 16, glyph_rows = 6;
        float glyph_tile = 8.0f;
        bool glyph_is_color = false;
        bool glyph_pending = false;
        // Audio Scope hand-off: mono PCM copy for the engine's waveform
        // strip (empty = silent clip).
        std::vector<int16_t> scope_data;
        uint32_t scope_rate = 0;
        bool scope_pending = false;
    };

    struct View {
        gfx::GpuImage* final_img = nullptr;    // SHADER_READ_ONLY or null
        gfx::GpuImage* source_img = nullptr;   // when want_source was set
        // Node-canvas thumbnails (docs/flow_canvas.md): the published
        // atlas + cell map snapshot consistent with it.
        gfx::GpuImage* thumb_img = nullptr;
        std::unordered_map<uint64_t, uint32_t> thumb_cells;
    };

    RenderWorker(gfx::Device& dev, media::Player& p,
                 std::unique_ptr<gfx::Engine> eng)
        : device(dev), player(p), engine(std::move(eng)) {}
    ~RenderWorker() { stop(); }

    bool start() {
        VkCommandPoolCreateInfo pool_info{
            VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = device.graphics_family();
        if (vkCreateCommandPool(device.device(), &pool_info, nullptr,
                                &pool_) != VK_SUCCESS)
            return false;
        VkCommandBufferAllocateInfo cb{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cb.commandPool = pool_;
        cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cb.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device.device(), &cb, &cmd_) !=
            VK_SUCCESS)
            return false;
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(device.device(), &fence_info, nullptr, &fence_) !=
            VK_SUCCESS)
            return false;
        published_.resize(3);
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(m_);
            quit_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
        device.wait_idle();
        published_.clear();
        graveyard_.clear();
        engine.reset();
        if (fence_) vkDestroyFence(device.device(), fence_, nullptr);
        if (pool_) vkDestroyCommandPool(device.device(), pool_, nullptr);
        fence_ = VK_NULL_HANDLE;
        pool_ = VK_NULL_HANDLE;
    }

    // The UI holds the worker idle across player open/close (MezReader
    // ownership is not thread-safe). Re-entrant via the counter.
    void pause() {
        std::unique_lock<std::mutex> lock(m_);
        ++pause_count_;
        cv_.wait(lock, [&] { return idle_ || quit_; });
    }
    void resume() {
        {
            std::lock_guard<std::mutex> lock(m_);
            --pause_count_;
        }
        cv_.notify_all();
    }

    // Drop published frames (clip changed — stale pixels must not linger).
    void invalidate() {
        std::lock_guard<std::mutex> lock(m_);
        latest_ = -1;
        for (Published& p : published_) p.ready = false;
    }

    // UI side, once per frame: newest ready image, marked as sampled by
    // this UI frame so the worker won't rewrite it until that frame's
    // fence has been waited.
    View acquire(uint64_t ui_frame) {
        std::lock_guard<std::mutex> lock(m_);
        if (latest_ < 0 || !published_[static_cast<size_t>(latest_)].ready)
            return {};
        Published& p = published_[static_cast<size_t>(latest_)];
        p.last_ui_frame = ui_frame;
        View v;
        v.final_img = p.final_img.get();
        v.source_img = p.has_source ? p.source_img.get() : nullptr;
        v.thumb_img = p.has_thumbs ? p.thumb_img.get() : nullptr;
        if (p.has_thumbs) v.thumb_cells = p.thumb_cells;
        return v;
    }

    void set_completed_ui_frame(uint64_t f) {
        completed_ui_frame_.store(f, std::memory_order_relaxed);
    }

    void post_glyph_atlas(std::vector<uint8_t> data, uint32_t w, uint32_t h,
                          float tile, uint32_t cols, uint32_t rows,
                          bool is_color = false) {
        {
            std::lock_guard<std::mutex> lock(m_);
            job_.glyph_data = std::move(data);
            job_.glyph_w = w;
            job_.glyph_h = h;
            job_.glyph_tile = tile;
            job_.glyph_cols = cols;
            job_.glyph_rows = rows;
            job_.glyph_is_color = is_color;
            job_.glyph_pending = true;
            ++job_serial_;
        }
        cv_.notify_all();
    }

    void post_scope_audio(std::vector<int16_t> mono, uint32_t rate) {
        {
            std::lock_guard<std::mutex> lock(m_);
            job_.scope_data = std::move(mono);
            job_.scope_rate = rate;
            job_.scope_pending = true;
            ++job_serial_;
        }
        cv_.notify_all();
    }

    gfx::Device& device;
    media::Player& player;
    std::unique_ptr<gfx::Engine> engine;

    // Shared state (m_): the job snapshot and the publish ring.
    std::mutex m_;
    std::condition_variable cv_;
    Job job_;
    uint64_t job_serial_ = 0;

private:
    struct Published {
        std::unique_ptr<gfx::GpuImage> final_img, source_img;
        // Node-canvas thumbnail atlas copy (fixed size — never retired on
        // resize) + the cell map consistent with its pixels.
        std::unique_ptr<gfx::GpuImage> thumb_img;
        std::unordered_map<uint64_t, uint32_t> thumb_cells;
        uint32_t w = 0, h = 0;
        bool has_source = false;
        bool has_thumbs = false;   // atlas copy recorded at least once
        bool ready = false;
        uint64_t last_ui_frame = 0;
    };

    void run();
    bool ensure_published(Published& p, uint32_t w, uint32_t h,
                          bool want_source, uint64_t completed);

    bool quit_ = false;
    bool idle_ = false;
    int pause_count_ = 0;
    std::vector<Published> published_;
    int latest_ = -1;
    std::atomic<uint64_t> completed_ui_frame_{0};
    // Old-size images wait here until the UI frames that sampled them have
    // retired.
    std::vector<std::pair<uint64_t, std::unique_ptr<gfx::GpuImage>>>
        graveyard_;

    std::thread thread_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
};

bool RenderWorker::ensure_published(Published& p, uint32_t w, uint32_t h,
                                    bool want_source, uint64_t completed) {
    auto retire = [&](std::unique_ptr<gfx::GpuImage>& img) {
        if (img) graveyard_.emplace_back(p.last_ui_frame, std::move(img));
    };
    const VkImageUsageFlags usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (p.w != w || p.h != h) {
        retire(p.final_img);
        retire(p.source_img);
        p.w = 0;
        p.h = 0;
    }
    if (!p.final_img) {
        p.final_img = gfx::GpuImage::create(
            device, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, usage);
        if (!p.final_img) return false;
    }
    if (want_source && !p.source_img) {
        p.source_img = gfx::GpuImage::create(
            device, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, usage);
        if (!p.source_img) return false;
    }
    if (!p.thumb_img) {
        p.thumb_img = gfx::GpuImage::create(
            device, VK_FORMAT_R8G8B8A8_UNORM,
            gfx::Engine::kThumbCellW * gfx::Engine::kThumbGridCols,
            gfx::Engine::kThumbCellH * gfx::Engine::kThumbGridRows, usage);
        if (!p.thumb_img) return false;
    }
    p.w = w;
    p.h = h;
    // Free graveyard entries whose sampling UI frames have retired.
    graveyard_.erase(
        std::remove_if(graveyard_.begin(), graveyard_.end(),
                       [&](const auto& g) { return g.first <= completed; }),
        graveyard_.end());
    return true;
}

void RenderWorker::run() {
    // Worker-local decode/remap state (moved off AppState — these hold
    // FILE handles and are single-thread objects).
    MaskSourceReaders mask_readers;
    LayerTrimReaders trim_readers;
    mod::TimeRemap remap;
    codec::MezReader remap_reader;
    bool remap_reader_ok = false;
    std::filesystem::path remap_reader_path;
    codec::DecodedFrame remap_frame;
    uint32_t remap_last = 0xFFFFFFFFu;

    doc::Document doc;
    uint64_t doc_revision = ~0ull;
    mod::AnalysisCurves analysis;
    bool has_analysis = false;
    uint64_t analysis_stamp = ~0ull;
    uint64_t cache_doc_hash = 0;
    bool cache_doc_history = false;
    uint64_t cache_hash_revision = ~0ull;
    uint64_t last_serial = ~0ull;
    uint32_t last_rendered_frame = 0xFFFFFFFFu;
    bool last_had_frame = false;
    bool pending_render = false;
    uint32_t slot = 0;

    for (;;) {
        // Small-field snapshot; doc/analysis copied only on change.
        uint64_t overlay_mask_id;
        uint64_t preview_node;
        uint32_t preview_div;
        bool live_mode, want_source, have_clip, proxy_active;
        double app_seconds, env_key_time;
        std::filesystem::path mez_path;
        bool doc_changed = false;
        {
            std::unique_lock<std::mutex> lock(m_);
            idle_ = true;
            cv_.notify_all();
            cv_.wait_for(lock, std::chrono::milliseconds(4), [&] {
                return quit_ ||
                       (pause_count_ == 0 && job_serial_ != last_serial);
            });
            if (quit_) return;
            if (pause_count_ > 0) continue;
            idle_ = false;
            last_serial = job_serial_;
            if (job_.doc_revision != doc_revision) {
                doc = job_.doc;
                doc_revision = job_.doc_revision;
                doc_changed = true;
            }
            if (job_.analysis_stamp != analysis_stamp) {
                analysis = job_.analysis;
                has_analysis = job_.has_analysis;
                analysis_stamp = job_.analysis_stamp;
                doc_changed = true;   // curves feed the render too
            }
            if (job_.glyph_pending) {
                job_.glyph_pending = false;
                std::vector<uint8_t> bytes = std::move(job_.glyph_data);
                const uint32_t gw = job_.glyph_w, gh = job_.glyph_h;
                const float gt = job_.glyph_tile;
                const uint32_t gc = job_.glyph_cols, gr = job_.glyph_rows;
                const bool color = job_.glyph_is_color;
                lock.unlock();
                if (color)
                    engine->set_glyph_atlas_rgba(bytes.data(), gw, gh, gt,
                                                 gc, gr, /*slot=*/2);
                else
                    engine->set_glyph_atlas(bytes.data(), gw, gh, gt, gc, gr,
                                            /*slot=*/2);
                lock.lock();
                doc_changed = true;
            }
            if (job_.scope_pending) {
                job_.scope_pending = false;
                std::vector<int16_t> mono = std::move(job_.scope_data);
                const uint32_t rate = job_.scope_rate;
                lock.unlock();
                engine->set_scope_audio(std::move(mono), rate);
                lock.lock();
                doc_changed = true;
            }
            overlay_mask_id = job_.overlay_mask_id;
            preview_node = job_.preview_node;
            preview_div = job_.preview_div;
            live_mode = job_.live_mode;
            app_seconds = job_.app_seconds;
            env_key_time = job_.env_key_time;
            want_source = job_.want_source;
            have_clip = job_.have_clip;
            proxy_active = job_.proxy_active;
            mez_path = job_.mez_path;
        }

        if (!have_clip || !player.is_open()) {
            last_had_frame = false;
            continue;
        }
        // Silent clips (stills, muted sources) have no audio callback; the
        // playhead only moves if this loop ticks the fallback clock — and
        // it must tick even on cycles the idle check below skips.
        player.tick();
        const uint32_t mod_frame = player.current_frame_index();
        // Idle: nothing moved, nothing changed, nothing owed — skip.
        if (!doc_changed && !pending_render && !live_mode &&
            last_had_frame && mod_frame == last_rendered_frame)
            continue;
        // A decided render stays owed until it actually publishes (a
        // busy publish ring or missing decode must retry, not stall).
        pending_render = true;

        std::shared_ptr<const codec::DecodedFrame> decoded =
            player.current_frame();
        if (!decoded) {
            last_had_frame = false;
            continue;
        }

        gfx::SourcePlanes planes;
        const codec::FrameView view = decoded->view();
        planes.y = view.y.data;
        planes.y_stride = view.y.stride;
        planes.u = view.u.data;
        planes.u_stride = view.u.stride;
        planes.v = view.v.data;
        planes.v_stride = view.v.stride;
        planes.width = view.width;
        planes.height = view.height;
        double mod_fps = player.fps() > 0.0 ? player.fps() : 30.0;
        uint32_t playhead_src = mod_frame;

        // Time remap (spec §6.1): only the SOURCE frame remaps; modulation
        // stays fixed-timestep on the playhead (spec §11).
        const uint32_t remap_src = remap.source_frame(
            doc, mod_frame, mod_fps, has_analysis ? &analysis : nullptr,
            player.frame_count(), live_mode ? app_seconds : -1.0);
        if (remap_src != mod_frame) {
            if (!remap_reader_ok || remap_reader_path != mez_path) {
                remap_reader.close();
                std::string err;
                remap_reader_ok = remap_reader.open(mez_path, &err);
                remap_reader_path = mez_path;
                remap_last = 0xFFFFFFFFu;
            }
            if (remap_reader_ok) {
                if (remap_last != remap_src &&
                    remap_reader.decode(remap_src, remap_frame))
                    remap_last = remap_src;
                if (remap_last == remap_src) {
                    const codec::FrameView rv = remap_frame.view();
                    planes.y = rv.y.data;
                    planes.y_stride = rv.y.stride;
                    planes.u = rv.u.data;
                    planes.u_stride = rv.u.stride;
                    planes.v = rv.v.data;
                    planes.v_stride = rv.v.stride;
                    planes.width = rv.width;
                    planes.height = rv.height;
                    playhead_src = remap_src;
                }
            }
        }

        // Video-sampling sources (docs/flow_canvas.md v4) read the exact
        // frame this pass renders — post-remap planes, same as export.
        mod::SourceFrameView sfv;
        sfv.y = planes.y;
        sfv.y_stride = planes.y_stride;
        sfv.u = planes.u;
        sfv.u_stride = planes.u_stride;
        sfv.v = planes.v;
        sfv.v_stride = planes.v_stride;
        sfv.width = static_cast<int>(planes.width);
        sfv.height = static_cast<int>(planes.height);
        const doc::Document resolved = mod::resolve(
            doc, mod_frame, mod_fps, has_analysis ? &analysis : nullptr,
            live_mode ? app_seconds : -1.0, live_mode ? env_key_time : -1.0,
            &sfv);
        engine->set_preview_divisor(preview_div);
        engine->cache().set_budget(static_cast<size_t>(doc.cache_mb) << 20);

        // Frame render cache context (spec §10) — the doc-hash leg is
        // recomputed only when the document changed.
        uint64_t cache_ctx = 0;
        if (!live_mode && !want_source && doc.cache_mb > 0) {
            if (cache_hash_revision != doc_revision) {
                cache_hash_revision = doc_revision;
                const std::string doc_json =
                    json::write(doc::doc_to_json(doc), false);
                uint64_t hash = 14695981039346656037ull;
                for (const char c : doc_json)
                    hash = (hash ^ static_cast<uint8_t>(c)) *
                           1099511628211ull;
                cache_doc_hash = hash;
                cache_doc_history = doc::document_uses_history(doc);
            }
            if (!cache_doc_history) {
                uint64_t ctx = cache_doc_hash;
                ctx = hash_combine(ctx, preview_div);
                ctx = hash_combine(ctx, overlay_mask_id);
                ctx = hash_combine(ctx, preview_node);
                ctx = hash_combine(ctx, has_analysis ? 1u : 0u);
                ctx = hash_combine(ctx, proxy_active ? 2u : 3u);
                for (const wchar_t c : mez_path.native())
                    ctx = hash_combine(ctx, static_cast<uint64_t>(c));
                cache_ctx = ctx | 1u;
            }
        }

        const auto msrc = mask_readers.collect(resolved, mod_frame);
        const auto lsrc = trim_readers.collect(resolved, mez_path,
                                               playhead_src,
                                               player.frame_count());

        // Pick a publish slot the UI is provably done with.
        const uint32_t pw = std::max((planes.width / preview_div) & ~1u, 2u);
        const uint32_t ph =
            std::max((planes.height / preview_div) & ~1u, 2u);
        int target = -1;
        {
            std::lock_guard<std::mutex> lock(m_);
            const uint64_t completed =
                completed_ui_frame_.load(std::memory_order_relaxed);
            for (int i = 0; i < 3; ++i) {
                if (i == latest_) continue;
                if (published_[static_cast<size_t>(i)].last_ui_frame <=
                    completed) {
                    target = i;
                    break;
                }
            }
            if (target >= 0 &&
                !ensure_published(published_[static_cast<size_t>(target)],
                                  pw, ph, want_source, completed))
                target = -1;
        }
        if (target < 0) continue;   // UI briefly holds all slots — retry
        Published& pub = published_[static_cast<size_t>(target)];

        vkResetFences(device.device(), 1, &fence_);
        VkCommandBufferBeginInfo begin{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd_, &begin);

        gfx::GpuImage* source_image = nullptr;
        gfx::GpuImage* final_image = engine->render(
            cmd_, slot, planes, resolved, mod_frame, mod_fps,
            overlay_mask_id, msrc.empty() ? nullptr : msrc.data(),
            msrc.size(), cache_ctx, want_source ? &source_image : nullptr,
            lsrc.empty() ? nullptr : lsrc.data(), lsrc.size(),
            preview_node);
        slot = (slot + 1) % gfx::kFramesInFlight;

        bool published_ok = false;
        if (final_image && final_image->width() == pw &&
            final_image->height() == ph) {
            auto copy_into = [&](gfx::GpuImage& src, gfx::GpuImage& dst) {
                src.transition(cmd_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                dst.transition(cmd_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkImageCopy region{};
                region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.extent = {pw, ph, 1};
                vkCmdCopyImage(cmd_, src.image(),
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               dst.image(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);
                dst.transition(cmd_,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            };
            copy_into(*final_image, *pub.final_img);
            const bool with_source = want_source && source_image;
            if (with_source) copy_into(*source_image, *pub.source_img);
            pub.has_source = with_source;
            // Node-canvas thumbnails: publish the atlas + a cell map
            // snapshot consistent with its pixels.
            if (gfx::GpuImage* atlas = engine->thumb_atlas()) {
                atlas->transition(cmd_,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                pub.thumb_img->transition(
                    cmd_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkImageCopy tregion{};
                tregion.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
                                          1};
                tregion.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
                                          1};
                tregion.extent = {atlas->width(), atlas->height(), 1};
                vkCmdCopyImage(cmd_, atlas->image(),
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               pub.thumb_img->image(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &tregion);
                pub.thumb_img->transition(
                    cmd_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                pub.thumb_cells = engine->thumb_cells();
                pub.has_thumbs = true;
            }
            published_ok = true;
        }
        vkEndCommandBuffer(cmd_);

        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd_;
        {
            std::lock_guard<std::mutex> qlock(device.queue_mutex());
            if (vkQueueSubmit(device.graphics_queue(), 1, &submit, fence_) !=
                VK_SUCCESS)
                continue;
        }
        vkWaitForFences(device.device(), 1, &fence_, VK_TRUE, UINT64_MAX);

        if (published_ok) {
            std::lock_guard<std::mutex> lock(m_);
            pub.ready = true;
            latest_ = target;
        }
        last_rendered_frame = mod_frame;
        last_had_frame = true;
        pending_render = false;
    }
}

// Audio Scope source (spec §7 family): mono copy of the PCM sidecar,
// block-averaged down to ~16 kHz for the engine's per-frame waveform
// strip. Deterministic — preview and export run the same reduction.
std::vector<int16_t> load_scope_audio(const std::filesystem::path& pcm_path,
                                      uint32_t* out_rate) {
    *out_rate = 0;
    if (pcm_path.empty()) return {};
    media::PcmReader pcm;
    std::string error;
    if (!pcm.open(pcm_path, &error)) return {};
    const uint32_t ch = pcm.channels();
    const uint32_t sr = pcm.sample_rate();
    if (!ch || !sr || !pcm.frame_count()) return {};
    const uint32_t decim = std::max(1u, sr / 16000u);
    std::vector<int16_t> mono;
    mono.reserve(static_cast<size_t>(pcm.frame_count() / decim) + 1);
    // Chunk size a multiple of decim so blocks never straddle chunks.
    const size_t chunk = static_cast<size_t>(65536 - (65536 % decim));
    std::vector<int16_t> buf(chunk * ch);
    uint64_t pos = 0;
    while (pos < pcm.frame_count()) {
        const size_t got = pcm.read(pos, buf.data(), chunk);
        if (!got) break;
        for (size_t b = 0; b + decim <= got; b += decim) {
            int32_t acc = 0;
            for (size_t f = 0; f < decim; ++f)
                for (uint32_t c = 0; c < ch; ++c)
                    acc += buf[(b + f) * ch + c];
            mono.push_back(static_cast<int16_t>(
                acc / static_cast<int32_t>(decim * ch)));
        }
        pos += got;
    }
    *out_rate = sr / decim;
    return mono;
}

// Offline export worker: private MezReader + Engine + readback on a copied
// document — the preview loop keeps running; queue submits are serialized
// by the device mutex.
std::unique_ptr<ExportJob> start_export(gfx::Device& device,
                                        const std::filesystem::path& shader_dir,
                                        const std::filesystem::path& mez_path,
                                        const std::filesystem::path& pcm_path,
                                        const doc::Document& doc,
                                        const mod::AnalysisCurves* analysis,
                                        const std::filesystem::path& out_path) {
    auto job = std::make_unique<ExportJob>();
    job->out_path = out_path;
    ExportJob* raw = job.get();
    doc::Document doc_copy = doc;
    mod::AnalysisCurves curves_copy;
    const bool has_analysis = analysis != nullptr;
    if (analysis) curves_copy = *analysis;
    job->thread = std::thread([&device, shader_dir, mez_path, pcm_path,
                               doc_copy = std::move(doc_copy),
                               curves_copy = std::move(curves_copy),
                               has_analysis, raw, out_path] {
        codec::MezReader reader;
        std::string error;
        if (!reader.open(mez_path, &error)) {
            raw->result.error = "mez open failed: " + error;
            raw->done = true;
            return;
        }
        auto engine = gfx::Engine::create(device, shader_dir);
        auto readback = gfx::Nv12Readback::create(device, shader_dir);
        if (!engine || !readback) {
            raw->result.error = "export renderer init failed";
            raw->done = true;
            return;
        }
        // Audio Scope parity with preview (spec §11): same PCM reduction.
        {
            uint32_t scope_rate = 0;
            auto scope_mono = load_scope_audio(pcm_path, &scope_rate);
            engine->set_scope_audio(std::move(scope_mono), scope_rate);
        }
        codec::DecodedFrame decoded;
        MaskSourceReaders mask_readers;
        LayerTrimReaders trim_readers;
        mod::TimeRemap remap;
        const double fps = reader.fps();
        // Clip trim (spec §3/§9): export renders exactly the trim region;
        // modulation still resolves on absolute timeline frames so preview
        // and export stay bit-identical (spec §11).
        const uint32_t total = reader.frame_count();
        const uint32_t t_in =
            std::min(doc_copy.clip_trim_in, total ? total - 1 : 0u);
        const uint32_t t_out = doc_copy.clip_trim_out
                                   ? std::min(doc_copy.clip_trim_out, total)
                                   : total;
        const uint32_t span = t_out > t_in ? t_out - t_in : total;
        auto producer = [&](uint32_t f, std::vector<uint8_t>& nv12) {
            const uint32_t abs_f = t_in + f;
            // Same time-remap math as preview (spec §6.1/§11): export
            // walks frames sequentially, so the prefix sum is incremental.
            const uint32_t src = remap.source_frame(
                doc_copy, abs_f, fps, has_analysis ? &curves_copy : nullptr,
                reader.frame_count());
            if (!reader.decode(src, decoded)) return false;
            const codec::FrameView view = decoded.view();
            gfx::SourcePlanes planes;
            planes.y = view.y.data;
            planes.y_stride = view.y.stride;
            planes.u = view.u.data;
            planes.u_stride = view.u.stride;
            planes.v = view.v.data;
            planes.v_stride = view.v.stride;
            planes.width = view.width;
            planes.height = view.height;
            // Same resolve as preview (spec §11: one code path, fixed
            // timestep on frame index) — including the video-sample view
            // of the identical post-remap decoded frame.
            mod::SourceFrameView sfv;
            sfv.y = planes.y;
            sfv.y_stride = planes.y_stride;
            sfv.u = planes.u;
            sfv.u_stride = planes.u_stride;
            sfv.v = planes.v;
            sfv.v_stride = planes.v_stride;
            sfv.width = static_cast<int>(planes.width);
            sfv.height = static_cast<int>(planes.height);
            const doc::Document resolved = mod::resolve(
                doc_copy, abs_f, fps, has_analysis ? &curves_copy : nullptr,
                -1.0, -1.0, &sfv);
            const auto msrc = mask_readers.collect(resolved, abs_f);
            const auto lsrc = trim_readers.collect(resolved, mez_path, src,
                                                   reader.frame_count());
            return readback->render(*engine, planes, resolved, abs_f, fps,
                                    nv12,
                                    msrc.empty() ? nullptr : msrc.data(),
                                    msrc.size(), 0,
                                    lsrc.empty() ? nullptr : lsrc.data(),
                                    lsrc.size());
        };
        media::ExportOptions options;
        // Trimmed exports keep audio in sync by skipping the same lead-in;
        // the user nudge (spec §7, positive = audio later) subtracts.
        options.audio_offset_seconds =
            (fps > 0.0 ? t_in / fps : 0.0) -
            static_cast<double>(doc_copy.audio_offset_ms) * 0.001;
        raw->result = media::export_movie(
            reader.width(), reader.height(), reader.timescale(),
            reader.frame_duration(), span, producer, pcm_path,
            out_path, options, &raw->progress);
        raw->done = true;
    });
    return job;
}

// ------------------------------------------------------------- app state

// Persistent per-effect widget interaction state, keyed on the effect's
// stable id (survives reorder/remove/undo; the map only grows — effect
// counts are tiny).
struct EffectUiState {
    ui::SliderState wet, opacity;
    ui::SliderState params[16];
    ui::ButtonState bypass_button, up_button, down_button, remove_button;
    ui::ButtonState route_buttons[18], key_buttons[18], expose_buttons[18];
    ui::ButtonState group_button, rnd_button;
    ui::ButtonState solo_button, copy_button;
    ui::DropdownState mask_dd;
    // Selector-param dropdowns + the Text card's string field (v5.6).
    ui::DropdownState param_dd[16];
    ui::ButtonState text_button;
};

struct GroupUiState {
    ui::ButtonState fold_button, ungroup_button, save_button;
    ui::ButtonState bypass_check;
    // Face rows (v5.3): exposed member params as direct aliases.
    ui::SliderState face_sliders[8];
    ui::ButtonState face_remove[8];
    ui::DropdownState face_dd[8];   // selector aliases (v5.6)
};

struct RouteUiState {
    ui::DropdownState source_dd, shape_dd, curve_dd;
    ui::ButtonState remove_button;
    ui::SliderState rate_slider, amount_slider;
};

struct MaskUiState {
    ui::ButtonState remove_button, view_button,
        invert_check, chain_add_button, key_points_button;
    ui::ButtonState freerun_check;
    ui::DropdownState type_dd, extract_dd, fit_dd, source_dd, combine_dd,
        combine_op_dd;
    ui::SliderState sliders[15];
    ui::ButtonState route_buttons[15], key_buttons[15];
    ui::ButtonState chain_remove[4];
    ui::SliderState chain_sliders[4][8];
};

struct LayerUiState {
    ui::ButtonState select_button, visible_check, remove_button;
    ui::ButtonState up_button, down_button;
    ui::DropdownState blend_dd, mask_dd, osc_dd;
    ui::SliderState sliders[9];
    // Transform + trim (spec §5), folded by default.
    bool xf_open = false;
    ui::ButtonState xf_header, flip_h_btn, flip_v_btn;
    ui::SliderState xf_sliders[8];
};

struct LaneUiState {
    int selected = -1;
    bool dragging_key = false;
    bool dragging_in = false;
    bool dragging_out = false;
    // A key added this frame: reselected next frame by exact frame match
    // (nearest-to-mouse picked the WRONG key when keys clustered).
    double pending_add_frame = -1.0;
    ui::ButtonState loop_button;   // loopable region chip (spec §7)
    ui::ButtonState mute_button;   // disable chip
    ui::ButtonState kill_button;   // delete-lane X
};

struct RulerState {
    // 0 idle, 1 scrub, 2 trim-in handle, 3 trim-out handle, 4 loop band.
    int drag_mode = 0;
    double loop_anchor = 0.0;   // frame where the loop drag started
};

// Flow-canvas selection (docs/flow_canvas.md): the inspector shows exactly
// one selected thing. View state — never document state, no undo.
enum class SelKind : uint8_t {
    None, Effect, Group, LayerSource, Mask, ModSource, Output,
    AddEffect,   // add-effect browser targeting a layer (+ optional slot)
    AddLayer,    // add-layer source picker
};
struct Selection {
    SelKind kind = SelKind::None;
    uint64_t id = 0;   // effect / group / layer / mask / route id by kind
};


struct AppState {
    doc::Document document;
    doc::UndoStack undo;

    media::Player player;
    std::unique_ptr<ImportJob> import;
    std::unique_ptr<ExportJob> export_job;
    std::filesystem::path mez_path, pcm_path;   // current clip bundle
    // Audio Scope: which PCM the render engine currently holds (the main
    // loop polls pcm_path and re-posts on change; empty = silent).
    std::filesystem::path scope_pcm_loaded;
    bool scope_pcm_init = false;
    mod::AnalysisCurves analysis;               // merged view (see sidechain)
    bool has_analysis = false;
    // Bumped whenever `analysis` is replaced — the render worker copies
    // the curves only when this moves.
    uint64_t analysis_stamp = 1;
    // Preview render thread (spec §13); owned by wWinMain, pointer here so
    // clip open/close paths can pause it around player mutation.
    RenderWorker* render_worker = nullptr;
    uint64_t ui_frame_counter = 0;
    // Sidechain (spec §7): analysis = clip video curves + (sidechain or
    // clip) audio curves. clip_analysis keeps the clip's own set.
    mod::AnalysisCurves clip_analysis;
    bool has_clip_analysis = false;
    std::string sc_active_path;         // sidechain merged into `analysis`
    std::filesystem::path sc_pcm_path;  // extracted PCM cache (export mux)
    bool sc_ok = false;
    double env_key_time = -1.0;         // live keypress trigger (spec §7)
    // Thumbnail strip (spec §3): RGBA staging until the renderer registers
    // it (textures are long-lived; a new clip just registers another one).
    std::vector<uint8_t> thumbs_rgba;
    uint32_t thumbs_w = 0, thumbs_h = 0, thumbs_count = 0;
    bool thumbs_dirty = false;
    const ui::UiTexture* thumbs_tex = nullptr;
    // Half-res proxy (spec §3): which file the player currently plays.
    bool proxy_active = false;
    // Lossless import (spec §3 mezzanine option), applies to the NEXT
    // import. App preference (ui.json), not project state.
    bool import_lossless = false;
    // Preset browser search (spec §9): plain substring filter. While the
    // field has focus, Char events type into it and letter shortcuts stay
    // inert; Enter/Escape release focus.
    std::string preset_filter;
    bool preset_search_focus = false;
    ui::ButtonState preset_search_btn, preset_import_btn;
    // Still-clip duration entry (seconds): same capture-the-keyboard field
    // pattern as the preset search; Enter commits, Escape cancels.
    std::string duration_edit;
    bool duration_focus = false;
    ui::ButtonState duration_btn;
    std::string clip_name;      // empty = test pattern
    std::string status;         // transient message line
    bool loop = true;

    TestPattern pattern;
    uint64_t pattern_frame = 0;

    uint64_t overlay_mask_id = 0;   // viewport mask overlay (view state)
    size_t selected_layer = 0;      // the stack panel edits this layer

    // Node canvas (docs/flow_canvas.md): selection drives the rail;
    // insert_before anchors splicing (a flow node id; 0 = chain end).
    Selection sel;
    // Multi-selection (canvas node ids): outlines, group move, group
    // delete. `sel` stays the primary/rail selection.
    std::vector<uint64_t> multi_sel;
    // Selected wires (canvas ids + kind; empty = none). Delete cuts all.
    std::vector<flow::Wire> sel_wires;
    // Inline value edit (canvas double-click on a slider row).
    uint64_t value_edit_node = 0;
    int value_edit_row = -1;
    std::string value_edit_buf;
    bool value_commit = false;
    // Find popup (Ctrl+F): the cursor add menu in jump-to-node mode.
    bool find_mode = false;
    // Canvas clipboard (Ctrl+C/X/V): copied node payloads + the links
    // among the copied effects; ids remap on paste.
    struct CanvasClipboard {
        bool valid = false;
        std::vector<doc::EffectInstance> effects;
        std::vector<doc::ModRoute> routes;
        std::vector<doc::Mask> masks;
        std::vector<doc::Document::NodeLink> links;
        float origin_x = 0.0f, origin_y = 0.0f;
    } clipboard;
    // Inline frame rename (canvas): the frame being renamed + edit buffer.
    uint64_t frame_rename_id = 0;
    std::string frame_rename_buf;
    // Inline group-card rename (texed subgraph title rename).
    uint64_t group_rename_id = 0;
    std::string group_rename_buf;
    // Inline Text-card string edit (v5.5): the effect id + edit buffer.
    uint64_t text_edit_id = 0;
    std::string text_edit_buf;
    // Rail multi-selection tools (align/distribute).
    ui::ButtonState align_buttons[4];
    flow::CanvasState canvas_state;
    // Subgraph view (texed): the group the canvas is scoped into; 0 =
    // the main graph. View state, never serialized. Entering fits the
    // view to the members; exiting restores the saved main-graph view
    // (nodes may sit anywhere — never assume the origin).
    uint64_t open_group = 0;
    float saved_pan_x = 0.0f, saved_pan_y = 0.0f, saved_zoom = 1.0f;
    bool saved_view_valid = false;
    // Preset drag-out of the browser: index being dragged (-1 none),
    // press anchor, and whether the drag passed the slop threshold.
    int preset_drag = -1;
    Vec2 preset_press{};
    bool preset_drag_live = false;
    uint64_t insert_before_id = 0;
    // Pending spawn position from a double-click add request.
    float add_gx = 0.0f, add_gy = 0.0f;
    bool add_pos_valid = false;
    // Node thumbnail atlas textures: one external registration per
    // published atlas image (three publish slots → three entries, ever).
    std::unordered_map<VkImageView, const ui::UiTexture*> thumb_registry;
    // Four-region layout seams (user sketch): the node graph dominates
    // top-left, timeline under it, preview + inspector stack right; all
    // three seams drag, fractions persist in ui.json.
    float split_right = 0.30f;      // right column share of the width
    float split_timeline = 0.26f;   // timeline share of the left column
    float split_preview = 0.42f;    // preview share of the right column
    ui::SliderState split_drag[3];
    // Inspector tabs: 0 = node (selection context), 1 = project (clip +
    // project — the old left rail), 2 = presets (the browser).
    int inspector_tab = 0;
    ui::ButtonState tab_buttons[3];
    // Menu bar (file / edit / view over the dropdown popup machinery).
    ui::DropdownState menu_states[3];

    // Project file (spec §10). autosaved_revision tracks what the last
    // autosave captured so quiet frames cost nothing.
    std::filesystem::path project_path;
    uint64_t saved_revision = 0;
    uint64_t autosaved_revision = 0;
    std::chrono::steady_clock::time_point last_autosave =
        std::chrono::steady_clock::now();

    // Preset browser (spec §10): shipped era presets + user-saved ones.
    std::filesystem::path shipped_preset_dir, user_preset_dir;
    // Text-effect font list (v5.5b): '|'-joined stems of assets/fonts/
    // *.ttf, same lowercased-filename order the engine indexes — feeds
    // the font dropdown. Scanned once at startup.
    std::string font_options;
    std::vector<doc::Preset> presets;
    int preset_tag_index = -1;      // -1 = all tags

    // Randomize (spec §10): chaos = intensity; counter advances per gesture
    // so repeated clicks explore, undo walks back one gesture at a time.
    float chaos = 0.5f;
    uint64_t rng_counter = 1;

    // Live mode (spec §9): timeline collapses, transport loops, LFO/drift
    // run on this wall clock (exempt from determinism, spec §11).
    bool live_mode = false;
    double app_seconds = 0.0;

    // Preview proxy divisor (spec §10): 1 full, 2 half, 4 quarter.
    uint32_t preview_div = 1;

    // UI preferences — app-level view state persisted in ui.json next to
    // the exe (deliberately not project state): active theme + sidebar
    // section folds (layers, stack, presets, masks, mod matrix).
    int theme_index = 0;
    bool sec_open[5] = {true, true, true, true, true};
    ui::ButtonState sec_buttons[5];
    ui::DropdownState theme_dd;
    ui::ScrollState timeline_scroll;

    // Add-effect browser (view state): one fold per spec §6.1 category
    // nested inside the stack section.
    bool add_fx_open = false;
    bool fx_cat_open[8] = {};
    ui::ButtonState add_fx_button, fx_cat_buttons[8];
    // Add-node search (docs/flow_canvas.md v3): same capture-the-keyboard
    // field pattern as the preset search; non-empty = flat filtered list.
    std::string fx_filter;
    bool fx_search_focus = false;
    ui::ButtonState fx_search_btn;

    // Viewport A/B wipe + bypass-all (spec §9). View state, not document
    // state — no undo, never exported.
    bool ab_wipe = false;
    float wipe_pos = 0.5f;
    bool bypass_all = false;

    // Bezier mask point editor (spec §8): index of the point being
    // dragged in the viewport, -1 when idle. View state.
    int drag_point = -1;

    // Render queue (spec §9): pending exports, each a full snapshot taken
    // at queue time (document, analysis, clip bundle) so edits made while
    // a job runs don't leak into it. FIFO; the front starts when the
    // active job finishes.
    struct QueuedExport {
        std::filesystem::path out_path;
        doc::Document doc;
        bool has_analysis = false;
        mod::AnalysisCurves analysis;
        std::filesystem::path mez_path, pcm_path;
    };
    std::vector<QueuedExport> export_queue;
    ui::ButtonState queue_remove_buttons[8];

    // (Preview-side decode/remap/cache state lives on the render worker —
    // spec §13 render thread.)

    // Widget state
    std::unordered_map<uint64_t, LayerUiState> layer_ui;
    std::unordered_map<uint64_t, GroupUiState> group_ui;
    std::map<std::string, ui::ButtonState> preset_buttons;
    ui::DropdownState tag_dd;
    ui::ButtonState save_button, open_project_button;
    ui::ButtonState randomize_all_button;
    ui::SliderState chaos_slider;
    ui::SliderState morph_slider;
    ui::ButtonState morph_route_button;
    ui::ButtonState live_button;
    ui::DropdownState proxy_dd;
    ui::SliderState speed_slider;
    ui::DropdownState time_mode_dd;
    ui::DropdownState sc_dd;
    ui::ButtonState sc_mux_check;
    ui::SliderState nudge_slider;
    ui::ButtonState proxy_check, lossless_check;
    ui::ButtonState speed_route_button, speed_key_button;
    ui::ButtonState ab_button, bypass_all_button;
    ui::SliderState wipe_slider;
    ui::ButtonState add_layer_buttons[7];
    std::unordered_map<uint64_t, EffectUiState> fx_ui;
    std::unordered_map<uint64_t, RouteUiState> route_ui;
    std::unordered_map<uint64_t, MaskUiState> mask_ui;
    ui::ButtonState add_mask_button;
    ui::ButtonState add_layer_open_button;
    ui::ButtonState add_frame_button;
    ui::ButtonState open_add_button;
    std::map<std::pair<uint64_t, int>, LaneUiState> lane_ui;
    ui::ButtonState open_button, open_big_button, play_button, undo_button,
        redo_button, export_button;
    ui::ButtonState add_buttons[static_cast<size_t>(doc::EffectType::Count)];
    ui::ButtonState snap_apply[3], snap_store[3];
    ui::SliderState seek_slider;
    ui::ScrubberState scrubber;
    ui::ButtonState loop_check;
    ui::ScrollState sidebar_scroll, right_scroll, preset_scroll;
    RulerState ruler;
};

// ---- flow-canvas selection helpers (docs/flow_canvas.md)

bool find_effect_by_id(const doc::Document& document, uint64_t id,
                       size_t* layer_index, size_t* fx_index) {
    for (size_t li = 0; li < document.layers.size(); ++li)
        for (size_t i = 0; i < document.layers[li].stack.size(); ++i)
            if (document.layers[li].stack[i].id == id) {
                *layer_index = li;
                *fx_index = i;
                return true;
            }
    return false;
}

// A KEYED param displays (and drags against) its lane's value at the
// playhead — the stored base is dead while a lane drives it, so showing
// the base reads as a frozen slider.
float shown_param_value(const AppState& app, const doc::ParamKey& key,
                        float base, float min_v, float max_v) {
    for (const doc::KeyframeLane& lane : app.document.lanes)
        if (lane.target == key && !lane.keys.empty() && !lane.muted) {
            const double ph = app.player.is_open()
                ? app.player.current_frame_index()
                : 0.0;
            return std::clamp(mod::eval_lane(lane, ph), min_v, max_v);
        }
    return base;
}

bool find_group_by_id(const doc::Document& document, uint64_t id,
                      size_t* layer_index) {
    for (size_t li = 0; li < document.layers.size(); ++li)
        for (const doc::Group& g : document.layers[li].groups)
            if (g.id == id) {
                *layer_index = li;
                return true;
            }
    return false;
}

int layer_index_by_id(const doc::Document& document, uint64_t id) {
    for (size_t li = 0; li < document.layers.size(); ++li)
        if (document.layers[li].id == id) return static_cast<int>(li);
    return -1;
}

// Drops a selection whose subject no longer exists (undo, remove, load).
void validate_selection(AppState& app) {
    const doc::Document& d = app.document;
    size_t li = 0, fi = 0;
    switch (app.sel.kind) {
        case SelKind::Effect:
            if (!find_effect_by_id(d, app.sel.id, &li, &fi))
                app.sel = {};
            break;
        case SelKind::Group:
            if (!find_group_by_id(d, app.sel.id, &li)) app.sel = {};
            break;
        case SelKind::LayerSource:
        case SelKind::AddEffect:
            if (layer_index_by_id(d, app.sel.id) < 0) app.sel = {};
            break;
        case SelKind::Mask: {
            bool ok = false;
            for (const doc::Mask& m : d.masks) ok = ok || m.id == app.sel.id;
            if (!ok) app.sel = {};
            break;
        }
        case SelKind::ModSource: {
            bool ok = false;
            for (const doc::ModRoute& r : d.mod_routes)
                ok = ok || r.id == app.sel.id;
            if (!ok) app.sel = {};
            break;
        }
        case SelKind::AddLayer:
            if (d.layers.size() >= doc::kMaxLayers) app.sel = {};
            break;
        default:
            break;
    }
    if (app.sel.kind == SelKind::None) app.insert_before_id = 0;

    // Prune multi-selection entries whose document objects vanished.
    auto canvas_id_live = [&](uint64_t cid) {
        if (cid == flow::kOutNodeId) return true;
        const uint64_t did = cid & 0x00FFFFFFFFFFFFFFull;
        switch (static_cast<flow::NodeKind>((cid >> 56) - 1)) {
            case flow::NodeKind::Source:
                return layer_index_by_id(d, did) >= 0;
            case flow::NodeKind::Effect: {
                size_t li = 0, fi = 0;
                return find_effect_by_id(d, did, &li, &fi);
            }
            case flow::NodeKind::Mask:
                for (const doc::Mask& m : d.masks)
                    if (m.id == did) return true;
                return false;
            case flow::NodeKind::ModSource:
                for (const doc::ModRoute& r : d.mod_routes)
                    if (r.id == did) return true;
                return false;
            case flow::NodeKind::Group: {
                size_t li = 0;
                return find_group_by_id(d, did, &li);
            }
            default:
                return false;
        }
    };
    app.multi_sel.erase(
        std::remove_if(app.multi_sel.begin(), app.multi_sel.end(),
                       [&](uint64_t cid) { return !canvas_id_live(cid); }),
        app.multi_sel.end());
}

void load_clip_analysis(AppState& app) {
    app.has_analysis = false;
    app.analysis = {};
    app.clip_analysis = {};
    app.has_clip_analysis = false;
    ++app.analysis_stamp;
    app.sc_active_path.clear();   // force a sidechain re-merge
    app.sc_pcm_path.clear();
    app.sc_ok = false;
    app.proxy_active = false;     // open_source opened the full-res file
    std::filesystem::path path = app.mez_path;
    path.replace_extension(".analysis");
    mod::AnalysisData data;
    if (mod::load_analysis(path, &data)) {
        app.clip_analysis = data.curves();
        app.has_clip_analysis = true;
        app.analysis = app.clip_analysis;
        app.has_analysis = true;
    }

    // Thumbnail strip (spec §3): RGB thumbs -> one horizontal RGBA strip,
    // registered as a UI texture by the main loop.
    app.thumbs_tex = nullptr;
    app.thumbs_count = 0;
    app.thumbs_dirty = false;
    std::filesystem::path tpath = app.mez_path;
    tpath.replace_extension(".thumbs");
    if (const auto bytes = read_file_bytes(tpath);
        bytes && bytes->size() > 10 &&
        std::memcmp(bytes->data(), "THM1", 4) == 0) {
        const uint8_t* p = bytes->data();
        const uint32_t tw = p[4] | (p[5] << 8);
        const uint32_t th = p[6] | (p[7] << 8);
        const uint32_t count = p[8] | (p[9] << 8);
        const size_t need =
            10 + static_cast<size_t>(tw) * th * 3 * count;
        if (tw && th && count && bytes->size() >= need &&
            tw * count <= 16384) {
            app.thumbs_w = tw;
            app.thumbs_h = th;
            app.thumbs_count = count;
            app.thumbs_rgba.assign(
                static_cast<size_t>(tw) * count * th * 4, 255);
            for (uint32_t t = 0; t < count; ++t) {
                const uint8_t* src =
                    p + 10 + static_cast<size_t>(t) * tw * th * 3;
                for (uint32_t y = 0; y < th; ++y)
                    for (uint32_t x = 0; x < tw; ++x) {
                        const uint8_t* s =
                            src + (static_cast<size_t>(y) * tw + x) * 3;
                        uint8_t* d =
                            app.thumbs_rgba.data() +
                            (static_cast<size_t>(y) * tw * count +
                             t * tw + x) *
                                4;
                        d[0] = s[0];
                        d[1] = s[1];
                        d[2] = s[2];
                    }
            }
            app.thumbs_dirty = true;
        }
    }
}

// Sidechain (spec §7): keep `analysis` = clip video curves + the sidechain
// audio curves. Extraction result is cached next to the clip bundle as
// <stem>.sc.pcm and re-analyzed only when the document path changes.
void sync_sidechain(AppState& app) {
    if (!app.player.is_open()) return;
    const std::string& want = app.document.sidechain_path;
    if (want == app.sc_active_path) return;
    app.sc_active_path = want;
    app.sc_ok = false;
    app.sc_pcm_path.clear();
    app.analysis = app.clip_analysis;
    app.has_analysis = app.has_clip_analysis;
    ++app.analysis_stamp;
    if (want.empty()) return;

    const std::filesystem::path src(want);
    std::error_code ec;
    if (!std::filesystem::exists(src, ec)) {
        app.status = "sidechain missing: " + src.filename().string();
        return;
    }
    std::filesystem::path dest = app.mez_path;
    dest.replace_filename(src.stem().wstring() + L".sc.pcm");
    std::string error;
    const bool have =
        std::filesystem::exists(dest, ec) &&
        std::filesystem::last_write_time(dest, ec) >=
            std::filesystem::last_write_time(src, ec);
    if (!have && !media::extract_audio_pcm(src, dest, &error)) {
        app.status = "sidechain: " + error;
        return;
    }

    media::PcmReader pcm;
    if (!pcm.open(dest, &error)) {
        app.status = "sidechain pcm: " + error;
        return;
    }
    std::vector<int16_t> samples(
        static_cast<size_t>(pcm.frame_count()) * pcm.channels());
    pcm.read(0, samples.data(), static_cast<size_t>(pcm.frame_count()));
    mod::AnalysisData data;
    const double fps = app.player.fps() > 0.0 ? app.player.fps() : 30.0;
    mod::analyze_audio(samples.data(), pcm.frame_count(), pcm.channels(),
                       pcm.sample_rate(), fps, app.player.frame_count(),
                       &data);
    app.analysis.low = std::move(data.low);
    app.analysis.mid = std::move(data.mid);
    app.analysis.high = std::move(data.high);
    app.analysis.onset = std::move(data.onset);
    app.analysis.bpm = data.bpm;
    app.has_analysis = true;
    ++app.analysis_stamp;
    app.sc_pcm_path = dest;
    app.sc_ok = true;
    app.status = "sidechain: " + src.filename().string();
}

// ---- UI preferences (theme + section folds): tiny exe-relative ui.json,
// written on every change. App-level view state — never in the project.
void save_ui_prefs(const AppState& app) {
    json::Value v = json::Value::make_object();
    v.set("theme", ui::theme_name(app.theme_index));
    json::Value secs = json::Value::make_array();
    for (int i = 0; i < 5; ++i) secs.push(app.sec_open[i]);
    v.set("sections", std::move(secs));
    if (app.import_lossless) v.set("lossless_import", true);
    // Four-region layout seams (all draggable, all persisted).
    v.set("split_right", static_cast<double>(app.split_right));
    v.set("split_timeline", static_cast<double>(app.split_timeline));
    v.set("split_preview", static_cast<double>(app.split_preview));
    const std::string text = json::write(v, true);
    write_file_bytes(executable_dir() / "ui.json", text.data(), text.size());
}

void load_ui_prefs(AppState& app) {
    const auto bytes = read_file_bytes(executable_dir() / "ui.json");
    if (!bytes) return;
    const auto parsed = json::parse(std::string_view(
        reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    if (!parsed.value) return;
    for (int i = 0; i < ui::theme_count(); ++i)
        if (parsed.value->get("theme").as_string() == ui::theme_name(i))
            app.theme_index = i;
    const json::Array& secs = parsed.value->get("sections").array();
    for (size_t i = 0; i < secs.size() && i < 5; ++i)
        app.sec_open[i] = secs[i].as_bool(true);
    app.import_lossless =
        parsed.value->get("lossless_import").as_bool(false);
    auto load_frac = [&](const char* key, float* dst, float lo, float hi) {
        const double d = parsed.value->get(key).as_number(
            static_cast<double>(*dst));
        *dst = std::clamp(static_cast<float>(d), lo, hi);
    };
    load_frac("split_right", &app.split_right, 0.15f, 0.5f);
    load_frac("split_timeline", &app.split_timeline, 0.1f, 0.6f);
    load_frac("split_preview", &app.split_preview, 0.15f, 0.7f);
    ui::set_active_theme(app.theme_index);
}

// Open a clip: an existing sidecar bundle (<stem>.mez[/.pcm]) opens
// directly; otherwise a background import produces one first.
bool set_still_frames(AppState& app, uint32_t frames);

void open_source(AppState& app, const std::filesystem::path& picked) {
    // Bind the clip to the document so save/open restores it. Direct write,
    // not a command: the clip binding is environment, not an undoable edit.
    app.document.clip_path = picked.string();
    app.duration_focus = false;
    app.duration_edit.clear();
    // The render worker must not touch the player while its reader moves.
    if (app.render_worker) {
        app.render_worker->pause();
        app.render_worker->invalidate();
    }
    struct ResumeGuard {
        AppState& app;
        ~ResumeGuard() {
            if (app.render_worker) app.render_worker->resume();
        }
    } resume_guard{app};
    // Bundle location (spec §3 scratch disk): a bundle that already sits
    // next to the source (hand-built, or from before the cache move) is
    // honored; otherwise bundles live under cache/<path-hash>/ beside the
    // exe so the app never dumps files into footage folders.
    std::filesystem::path mez = picked;
    if (mez.extension() != ".mez") {
        std::filesystem::path beside = picked;
        beside.replace_extension(".mez");
        std::error_code bec;
        if (std::filesystem::exists(beside, bec))
            mez = beside;
        else
            mez = bundle_dir_for(picked) /
                  (picked.stem().wstring() + L".mez");
    }
    if (std::filesystem::exists(mez)) {
        std::filesystem::path pcm = mez;
        pcm.replace_extension(".pcm");
        if (!std::filesystem::exists(pcm)) pcm.clear();
        std::string error;
        app.player.close();
        if (app.player.open(mez, pcm, &error)) {
            app.clip_name = picked.filename().string();
            app.mez_path = mez;
            app.pcm_path = pcm;
            load_clip_analysis(app);
            app.player.set_looping(app.loop);
            app.player.play();
            app.status.clear();
            // Persisted still duration (project state): reconcile the
            // bundle — fresh, rebuilt, or edited elsewhere — to the
            // document's length.
            if (app.document.still_duration_frames > 0 &&
                is_still_source(picked))
                set_still_frames(app, app.document.still_duration_frames);
        } else {
            app.status = "open failed: " + error;
        }
    } else {
        app.player.close();
        app.clip_name.clear();
        const std::filesystem::path dest = bundle_dir_for(picked);
        std::error_code cec;
        std::filesystem::create_directories(dest, cec);
        app.import = start_import(picked, app.import_lossless,
                                  cec ? picked.parent_path() : dest);
        app.status.clear();
    }
}

// Core of a still-duration change: rewrite the bundle's frame index in
// place (the hold-frame trick after the fact — no re-encode) and reopen
// the player at the new length. Worker readers hold the old index until
// their next reopen; harmless for a still, where every frame is one
// payload. Returns true when the player runs at `frames`.
bool set_still_frames(AppState& app, uint32_t frames) {
    if (!app.player.is_open() || app.mez_path.empty() || frames == 0)
        return false;
    if (frames == app.player.frame_count()) return true;
    if (app.render_worker) {
        app.render_worker->pause();
        app.render_worker->invalidate();
    }
    struct ResumeGuard {
        AppState& app;
        ~ResumeGuard() {
            if (app.render_worker) app.render_worker->resume();
        }
    } resume_guard{app};
    const bool was_playing = app.player.playing();
    const uint32_t at =
        std::min(app.player.current_frame_index(), frames - 1);
    app.player.close();
    bool rewrote = codec::mez_set_frame_count(app.mez_path, frames);
    if (!rewrote) {
        app.status = "duration change failed";
    } else {
        std::filesystem::path proxy = app.mez_path;
        proxy.replace_extension(".proxy.mez");
        std::error_code pec;
        if (std::filesystem::exists(proxy, pec))
            codec::mez_set_frame_count(proxy, frames);
        app.status.clear();
    }
    std::filesystem::path open_path = app.mez_path;
    if (app.proxy_active) open_path.replace_extension(".proxy.mez");
    std::string error;
    if (app.player.open(open_path, app.pcm_path, &error)) {
        app.player.set_looping(app.loop);
        app.player.seek_frame(at);
        if (was_playing) app.player.play();
    } else {
        app.status = "reopen failed: " + error;
        app.clip_name.clear();
        return false;
    }
    return rewrote;
}

// Duration entry commit: apply, then persist in the DOCUMENT — the scratch
// bundle is regenerable (cache clear, another machine), so the project
// file is the durable record and reopening reconciles the bundle to it.
void apply_still_duration(AppState& app, double seconds) {
    if (!app.player.is_open()) return;
    const double fps = app.player.fps() > 0.0 ? app.player.fps() : 30.0;
    const uint32_t frames = std::clamp(
        static_cast<uint32_t>(seconds * fps + 0.5), 1u,
        static_cast<uint32_t>(fps * 3600.0));   // up to an hour
    if (!set_still_frames(app, frames)) return;
    if (app.document.still_duration_frames != frames) {
        // Direct write + revision bump, like the clip binding: not an
        // undoable edit (undo cannot restore the rewritten file), but it
        // must dirty the project and refresh the worker's doc copy.
        app.document.still_duration_frames = frames;
        ++app.document.revision;
    }
}

void rescan_presets(AppState& app) {
    app.presets = doc::scan_presets(app.shipped_preset_dir);
    std::vector<doc::Preset> user = doc::scan_presets(app.user_preset_dir);
    for (doc::Preset& p : user) app.presets.push_back(std::move(p));
}

void save_project(AppState& app, const std::filesystem::path& path) {
    // Rolling project versions (spec §10): <name>.v1.json is the previous
    // save, .v2 the one before, .v3 the oldest kept. Rotated by copy, so
    // nothing is ever deleted — only overwritten by older history.
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        auto version_path = [&](int n) {
            std::filesystem::path v = path;
            v.replace_extension(".v" + std::to_string(n) + ".json");
            return v;
        };
        for (int n = 3; n >= 2; --n) {
            const auto older = version_path(n - 1);
            if (std::filesystem::exists(older, ec))
                std::filesystem::copy_file(
                    older, version_path(n),
                    std::filesystem::copy_options::overwrite_existing, ec);
        }
        std::filesystem::copy_file(
            path, version_path(1),
            std::filesystem::copy_options::overwrite_existing, ec);
    }
    app.document.name = path.stem().string();
    if (doc::save_document(path, app.document)) {
        app.project_path = path;
        app.saved_revision = app.document.revision;
        app.autosaved_revision = app.document.revision;
        app.status = "saved " + path.filename().string();
    } else {
        app.status = "save failed: " + path.string();
    }
}

void open_project(AppState& app, const std::filesystem::path& path) {
    std::string error;
    auto loaded = doc::load_document(path, &error);
    if (!loaded) {
        app.status = "open failed: " + error;
        return;
    }
    app.document = std::move(*loaded);
    app.undo.clear();
    app.selected_layer = 0;
    app.overlay_mask_id = 0;
    // Land on something editable: the base layer's first effect (or its
    // source) so the inspector never opens empty on a real project.
    app.sel = {};
    app.insert_before_id = 0;
    if (!app.document.layers.empty()) {
        const doc::Layer& base = app.document.layers[0];
        app.sel = base.stack.empty()
            ? Selection{SelKind::LayerSource, base.id}
            : Selection{SelKind::Effect, base.stack[0].id};
    }
    app.project_path = path;
    app.saved_revision = app.autosaved_revision = app.document.revision;
    std::string note = "opened " + path.filename().string();
    if (!app.document.clip_path.empty()) {
        const std::filesystem::path clip = app.document.clip_path;
        std::error_code ec;
        if (std::filesystem::exists(clip, ec)) {
            open_source(app, clip);   // may report its own failure
        } else {
            if (app.render_worker) {
                app.render_worker->pause();
                app.render_worker->invalidate();
            }
            app.player.close();
            if (app.render_worker) app.render_worker->resume();
            app.clip_name.clear();
            note += " (clip missing)";
        }
    } else {
        if (app.render_worker) {
            app.render_worker->pause();
            app.render_worker->invalidate();
        }
        app.player.close();
        if (app.render_worker) app.render_worker->resume();
        app.clip_name.clear();
    }
    if (app.status.empty()) app.status = std::move(note);
}

// UI-side per-frame job post (spec §13 render thread): copies only what
// changed — the document rides its revision, the analysis its stamp — and
// bumps the serial so the worker wakes when a re-render is due.
void push_render_job(RenderWorker& w, AppState& app) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(w.m_);
        RenderWorker::Job& j = w.job_;
        if (j.doc_revision != app.document.revision) {
            j.doc = app.document;
            j.doc_revision = app.document.revision;
            changed = true;
        }
        if (j.analysis_stamp != app.analysis_stamp) {
            j.analysis = app.analysis;
            j.has_analysis = app.has_analysis;
            j.analysis_stamp = app.analysis_stamp;
            changed = true;
        }
        const bool have_clip = app.player.is_open();
        const bool want_source = app.ab_wipe || app.bypass_all;
        auto set = [&](auto& dst, const auto& src) {
            if (!(dst == src)) {
                dst = src;
                changed = true;
            }
        };
        set(j.mez_path, app.mez_path);
        set(j.overlay_mask_id, app.overlay_mask_id);
        // Selection-follows preview (v5.4): effects/sources/groups show
        // their own output; masks their matte. Output / value nodes /
        // boundary nodes / no selection = the composite.
        uint64_t preview_key = 0;
        switch (app.sel.kind) {
            case SelKind::Effect:
            case SelKind::LayerSource:
            case SelKind::Group:
                preview_key = app.sel.id;
                break;
            case SelKind::Mask:
                preview_key = app.sel.id | doc::kMaskParamBit;
                break;
            default:
                break;
        }
        set(j.preview_node, preview_key);
        set(j.preview_div, app.preview_div);
        set(j.live_mode, app.live_mode);
        set(j.want_source, want_source);
        set(j.have_clip, have_clip);
        set(j.proxy_active, app.proxy_active);
        // Live clocks update silently — live mode re-renders every cycle
        // anyway, and a paused non-live preview must not.
        j.app_seconds = app.app_seconds;
        j.env_key_time = app.env_key_time;
        if (changed) ++w.job_serial_;
    }
    if (changed) w.cv_.notify_all();
}

// Per-frame staged edits: sliders/checkboxes write into arena floats/bools;
// after run_frame the deltas become commands.
struct ParamStage {
    size_t layer_index;   // the layer the widget was BUILT for (canvas
                          // rows can address any layer, not the selected)
    size_t fx_index;
    int param_index;
    float* staged;
    float original;
    bool* changed;
    bool* released;
};

struct FxRowActions {
    size_t layer_index;
    size_t fx_index;
    bool* up;
    bool* down;
    bool* remove;
    bool* bypass_changed;
    bool* bypass_staged;
    bool* group_toggle;   // "g": group with above / join above's / leave
    bool* randomize;      // "r": mutate this effect at the chaos intensity
    bool* solo_changed = nullptr;    // spec §5 solo toggle
    bool* solo_staged = nullptr;
    bool* duplicate = nullptr;       // spec §5 stack duplicate
};

struct FrameUi {
    std::vector<ParamStage> params;
    std::vector<FxRowActions> rows;
    bool* add_clicked[static_cast<size_t>(doc::EffectType::Count)] = {};
    bool* open_clicked = nullptr;
    bool* play_clicked = nullptr;
    bool* undo_clicked = nullptr;
    bool* redo_clicked = nullptr;
    bool* export_clicked = nullptr;
    bool* loop_clicked = nullptr;
    float* seek_staged = nullptr;
    bool* seek_changed = nullptr;
    ui::LayoutNode* preview = nullptr;

    // Modulation UI staging.
    struct RouteRow {
        uint64_t id;
        int* source_selected;   // dropdown picks; -1 = untouched
        int* shape_selected;
        int* curve_selected;
        bool* remove;
        float* rate_staged;
        bool* rate_changed;
        bool* rate_released;
        float rate_original;
        float* amount_staged;
        bool* amount_changed;
        bool* amount_released;
        float amount_original;
        // Video-sampling geometry rows (v4): px, py, pw, ph. Null when the
        // source type has no sampling window.
        float* pos_staged[4];
        bool* pos_changed[4];
        bool* pos_released[4];
        float pos_original[4];
    };
    std::vector<RouteRow> route_rows;

    struct AddRoute {
        doc::ParamKey key;
        bool* clicked;
    };
    std::vector<AddRoute> add_routes;

    struct KeyToggle {
        doc::ParamKey key;
        float value;    // current base value, keyed at the playhead
        bool* clicked;
        // Canvas rows: the toggle ENABLES/DISABLES keyframing (a lane
        // with keys → remove it); rail rows keep the key-at-playhead
        // toggle for scrub-and-key workflows.
        bool lane_toggle = false;
    };
    std::vector<KeyToggle> key_toggles;

    // Sidebar section fold headers + theme cycle button.
    struct SectionToggle {
        int index;
        bool* clicked;
    };
    std::vector<SectionToggle> section_toggles;
    int* theme_selected = nullptr;   // dropdown pick; -1 = untouched

    // Add-effect browser folds.
    bool* add_fx_toggle = nullptr;
    bool* fx_cat_clicked[8] = {};
    // Multi-selection align/distribute (rail): left, top, spread h/v.
    bool* align_clicked[4] = {};

    // Emitted by the lane widget during the draw pass; applied post-frame.
    struct LaneEdit {
        doc::ParamKey target;
        std::vector<doc::Keyframe> keys;
    };
    std::vector<LaneEdit> lane_edits;
    bool lane_release = false;

    float seek_to = -1.0f;    // ruler scrub target (frames)
    // Timeline region edits (spec §9: trim handles + loop region), from
    // ruler drags. -1 = untouched this frame.
    float trim_in_to = -1.0f;
    float trim_out_to = -1.0f;
    float loop_in_to = -1.0f;
    float loop_out_to = -1.0f;
    bool loop_clear = false;
    bool region_released = false;   // drag ended: break undo coalescing
    struct LaneLoop {
        doc::ParamKey target;
        bool* clicked;
    };
    std::vector<LaneLoop> lane_loops;   // per-lane loop chip (spec §7)
    struct LaneMute {
        doc::ParamKey target;
        bool* clicked;
    };
    std::vector<LaneMute> lane_mutes;   // per-lane disable chip
    struct LaneKill {
        doc::ParamKey target;
        bool* clicked;
    };
    std::vector<LaneKill> lane_kills;   // per-lane delete X
    bool* snap_apply_clicked[3] = {};
    bool* snap_store_clicked[3] = {};

    // Layer panel staging.
    enum class LayerField : int {
        Opacity, ColorAR, ColorAG, ColorAB, ColorBR, ColorBG, ColorBB,
        Scale, Angle,
        // Transform + trim (spec §5).
        CropL, CropR, CropT, CropB, XfScale, Rotate, TrimIn, TrimOut,
    };
    struct LayerStage {
        uint64_t layer_id;
        LayerField field;
        float* staged;
        float original;
        bool* changed;
        bool* released;
    };
    std::vector<LayerStage> layer_stages;
    struct LayerRow {
        size_t index;
        uint64_t id;
        bool* select;
        bool* visible_changed;
        bool* visible_staged;
        int* blend_selected;    // dropdown pick; -1 = untouched
        int* mask_selected;     // [none, masks...]; -1 = untouched
        int* osc_shape_selected = nullptr;   // oscillator waveform pick
        bool* remove;
        bool* up = nullptr;     // swap toward index 0 (bottom of composite)
        bool* down = nullptr;
        bool* xf_toggle = nullptr;   // fold/unfold the transform section
        bool* flip_h = nullptr;      // toggle clicks (spec §5 transform)
        bool* flip_v = nullptr;
    };
    std::vector<LayerRow> layer_rows;
    // solid, gradient, noise, test pattern, oscillator, shape, clip tap
    bool* add_layer_clicked[7] = {};

    // Group + preset staging.
    struct GroupActions {
        uint64_t group_id;
        bool* fold;
        bool* bypass_changed;
        bool* bypass_staged;
        bool* ungroup;
        bool* save;
    };
    std::vector<GroupActions> group_actions;
    // Group face (v5.3): expose/hide one member param (texed expose).
    struct ExposeToggle {
        size_t layer_index;
        uint64_t group_id;
        doc::ParamKey key;
        bool expose;      // desired state when clicked
        bool* clicked;
    };
    std::vector<ExposeToggle> expose_toggles;
    std::vector<std::pair<size_t, bool*>> preset_clicks;  // presets[] index
    // Preset drag-out (texed palette drag): the browser buttons' laid-out
    // nodes, so the post-frame handler can start a drag from a press.
    struct PresetNode {
        size_t index;
        ui::LayoutNode* node;
    };
    std::vector<PresetNode> preset_nodes;
    ui::LayoutNode* canvas_node = nullptr;   // the flow canvas leaf
    // Group card X: deletes the whole subgraph (members + group).
    std::vector<std::pair<uint64_t, bool*>> group_removes;
    int* tag_selected = nullptr;    // preset tag dropdown; -1 = untouched
    bool* save_clicked = nullptr;
    bool* open_project_clicked = nullptr;
    bool* randomize_all = nullptr;
    float* chaos_staged = nullptr;
    bool* chaos_changed = nullptr;
    float* morph_staged = nullptr;
    bool* morph_changed = nullptr;
    bool* morph_released = nullptr;
    bool* live_clicked = nullptr;
    int* proxy_selected = nullptr;  // res dropdown; -1 = untouched
    float* speed_staged = nullptr;
    bool* speed_changed = nullptr;
    int* time_mode_selected = nullptr;   // dropdown pick; -1 = untouched
    // Sidechain + audio nudge (spec §7).
    int* sc_selected = nullptr;          // [clip, <file>, pick]; -1 untouched
    bool* sc_mux_changed = nullptr;
    bool* sc_mux_staged = nullptr;
    float* nudge_staged = nullptr;
    bool* nudge_changed = nullptr;
    bool* nudge_released = nullptr;
    bool* proxy_toggle_changed = nullptr;   // spec §3 half-res proxy
    bool* proxy_toggle_staged = nullptr;
    bool* lossless_changed = nullptr;       // spec §3 lossless import pref
    bool* lossless_staged = nullptr;
    bool* preset_search_clicked = nullptr;  // spec §9 searchable browser
    bool* preset_import_clicked = nullptr;  // spec §9 single-file import
    bool* duration_clicked = nullptr;       // still-clip duration field
    bool* ab_clicked = nullptr;
    float* wipe_staged = nullptr;
    bool* wipe_changed = nullptr;
    bool* bypass_all_clicked = nullptr;

    // Render queue: remove-buttons for pending exports (index, clicked).
    struct QueueRow {
        size_t index;
        bool* remove;
    };
    std::vector<QueueRow> queue_rows;

    // Mask UI staging.
    enum class MaskField : int {
        CenterX, CenterY, RadiusX, RadiusY, Roundness, Feather,
        BlackPoint, WhitePoint, Gamma, KeyCenter, KeyRange, KeyR, KeyG, KeyB,
        BlurPx, GrowPx, GenScale, GenAngle,
    };
    struct MaskStage {
        uint64_t mask_id;
        MaskField field;
        float* staged;
        float original;
        bool* changed;
        bool* released;
    };
    std::vector<MaskStage> mask_stages;
    struct MaskActions {
        uint64_t mask_id;
        int* type_selected;      // dropdown picks; -1 = untouched
        int* extract_selected;
        bool* remove;
        bool* view;
        bool* invert_changed;
        bool* invert_staged;
        bool* chain_add;
        // Source dropdown (spec §8: clip | layers | generators | file |
        // pick). The build stage records the index layout so the handler
        // decodes the pick without re-deriving it.
        int* source_selected = nullptr;
        int src_layer_base = -1;
        int src_layer_count = 0;
        int src_gen_base = -1;
        int src_file_index = -1;
        int src_pick_index = -1;
        int* fit_selected = nullptr;
        bool* freerun_changed = nullptr;
        bool* freerun_staged = nullptr;
        bool* key_points = nullptr;    // whole-shape keyframe at playhead
        int* combine_selected = nullptr;      // [none, other masks...]
        int* combine_op_selected = nullptr;   // add/subtract/intersect
    };
    std::vector<MaskActions> mask_actions;
    struct MaskChainStage {
        uint64_t mask_id;
        size_t chain_index;
        int param_index;
        float* staged;
        float original;
        bool* changed;
        bool* released;
    };
    std::vector<MaskChainStage> mask_chain_stages;
    struct MaskChainRemove {
        uint64_t mask_id;
        size_t chain_index;
        bool* clicked;
    };
    std::vector<MaskChainRemove> mask_chain_removes;
    bool* add_mask_clicked = nullptr;
    bool* add_layer_open = nullptr;   // switch the rail to the layer picker
    bool* fx_search_clicked = nullptr;
    bool* add_frame_clicked = nullptr;
    bool* open_add_clicked = nullptr;   // None-selection "+ add node..."
    struct EffectMaskCycle {
        size_t fx_index;
        int* selected;   // dropdown pick into [none, masks...]; -1 = none
    };
    std::vector<EffectMaskCycle> effect_mask_cycles;
    // Rail selector dropdown (v5.6): a picked option index lands as the
    // param's value through set_param_command.
    struct ParamPick {
        size_t layer_index;
        size_t fx_index;
        int param_index;
        float min_value;
        int* selected;   // -1 = untouched this frame
    };
    std::vector<ParamPick> param_picks;
    // Rail text field (v5.6): clicking opens the shared inline editor.
    struct TextEditOpen {
        uint64_t effect_id;
        bool* clicked;
    };
    std::vector<TextEditOpen> text_edit_opens;
};

// Dynamic-count sibling list (the stack panel) — mirrors make_stack.
ui::LayoutNode* VStackDyn(ui::LayoutArena& arena, const ui::StackOpts& opts,
                          const std::vector<ui::LayoutNode*>& children) {
    ui::LayoutNode* n = ui::make_node(arena, ui::NodeKind::VStack);
    n->gap = opts.gap;
    n->padding = opts.padding;
    n->justify = opts.justify;
    n->cross_align = opts.cross_align;
    n->width = opts.width;
    n->height = opts.height;
    n->children = arena.alloc<ui::LayoutNode*>(children.size());
    n->child_count = static_cast<uint16_t>(children.size());
    for (size_t i = 0; i < children.size(); ++i) n->children[i] = children[i];
    return n;
}

void draw_preview_frame(ui::LayoutNode& node, ui::LayoutFrame& frame) {
    frame.canvas.draw_rect_outline(node.rect, 1.0f, frame.theme.hairline);
}

// ---- menu-bar button (user sketch): a Dropdown variant whose closed
// face is the MENU NAME, opening its item list through the same popup
// machinery (RunPopup handles the overlay + pick).
struct MenuUser {
    const char* label;
    const char* const* items;
    int count;
    ui::DropdownState* state;
    int* out_selected;
};

ui::Rect menu_popup_rect(const MenuUser& u, const ui::Rect& anchor,
                         const ui::LayoutFrame& frame) {
    float widest = 120.0f;
    for (int i = 0; i < u.count; ++i)
        widest = std::max(widest,
                          ui::measure_text(frame.font, u.items[i],
                                           frame.theme.font_size).x +
                              24.0f);
    const float h = static_cast<float>(u.count) * 20.0f + 8.0f;
    return {anchor.x, anchor.bottom() + 2.0f, widest, h};
}

ui::LayoutNode* MenuButton(ui::LayoutArena& arena, const char* label,
                           const char* const* items, int count,
                           ui::DropdownState* state, int* out_selected) {
    auto* u = arena.alloc<MenuUser>();
    u->label = label;
    u->items = items;
    u->count = count;
    u->state = state;
    u->out_selected = out_selected;
    ui::LayoutNode* n = ui::make_node(arena, ui::NodeKind::Leaf);
    n->user = u;
    n->measure_fn = [](ui::LayoutNode& node, const ui::Constraints&,
                       const ui::LayoutFrame& frame) {
        const auto* mu = static_cast<const MenuUser*>(node.user);
        return Vec2{ui::measure_text(frame.font, mu->label,
                                     frame.theme.font_size).x +
                        20.0f,
                    frame.theme.control_height};
    };
    n->hit_fn = [](ui::LayoutNode& node, ui::LayoutFrame& frame) {
        const auto* mu = static_cast<const MenuUser*>(node.user);
        ui::Rect r = node.rect;
        if (!node.clip.empty()) r = r.intersect(node.clip);
        frame.ctx.add_hit(r,
                          frame.ctx.acquire_widget_id(&mu->state->button));
        if (mu->state->open)
            frame.ctx.add_hit(menu_popup_rect(*mu, node.rect, frame),
                              frame.ctx.acquire_widget_id(mu->state),
                              ui::HitLayer::Popup);
    };
    n->draw_fn = [](ui::LayoutNode& node, ui::LayoutFrame& frame) {
        const auto* mu = static_cast<const MenuUser*>(node.user);
        ui::DropdownState& st = *mu->state;
        const ui::Rect& r = node.rect;
        const ui::WidgetId id =
            frame.ctx.acquire_widget_id(&st.button);
        const bool owns = frame.ctx.widget_owns_mouse(id);
        const bool hover = owns && r.contains(frame.input.mouse);
        if (frame.input.left_pressed() && owns && hover) {
            st.open = !st.open;
            if (st.open) frame.ctx.set_popup_owner(&st);
        }
        if (st.open && frame.ctx.popup_owner() != &st) st.open = false;
        const bool over_popup =
            st.open && frame.ctx.widget_owns_mouse(
                           frame.ctx.acquire_widget_id(mu->state));
        if (st.open && !hover && !over_popup &&
            frame.input.left_pressed())
            st.open = false;
        if (hover || st.open)
            frame.canvas.draw_sdf_rect(r, 3.0f,
                                       frame.theme.control_bg_hover);
        ui::draw_text(frame.canvas, frame.font, mu->label,
                      {r.x + 10.0f,
                       r.y + (r.h - frame.font.line_height() *
                                        frame.theme.font_size) *
                                 0.5f},
                      frame.theme.font_size,
                      st.open ? frame.theme.text : frame.theme.text_dim);
        if (st.open) {
            ui::Context::PopupRequest req;
            req.anchor = r;
            req.rect = menu_popup_rect(*mu, r, frame);
            req.items = mu->items;
            req.count = mu->count;
            req.selected = -1;
            req.state = &st;
            req.out_selected = mu->out_selected;
            frame.ctx.set_popup(req);
        }
    };
    n->debug_name = "menu_button";
    return n;
}

// ---- layout seam: a draggable divider that redistributes the fraction
// it owns. axis 0 = vertical bar (drags x), 1 = horizontal bar (drags
// y); dir flips which side grows. Releases persist the fraction.
struct SeamUser {
    AppState* app;
    float* frac;
    ui::SliderState* drag;
    float total;      // the dimension the fraction divides
    int axis;
    float dir;        // +1: fraction grows with +delta, -1: shrinks
    float min_frac, max_frac;
};

ui::LayoutNode* SplitterBar(ui::LayoutArena& arena, AppState& app,
                            float* frac, ui::SliderState* drag,
                            float total, int axis, float dir,
                            float min_frac, float max_frac) {
    auto* u = arena.alloc<SeamUser>();
    *u = {&app, frac, drag, std::max(total, 1.0f), axis, dir, min_frac,
          max_frac};
    ui::LayoutNode* n = ui::make_node(arena, ui::NodeKind::Leaf);
    n->user = u;
    n->width = axis == 0 ? ui::SizeSpec::fixed(6.0f) : ui::SizeSpec::fill();
    n->height = axis == 0 ? ui::SizeSpec::fill() : ui::SizeSpec::fixed(6.0f);
    n->hit_fn = [](ui::LayoutNode& node, ui::LayoutFrame& f) {
        auto* su = static_cast<SeamUser*>(node.user);
        f.ctx.add_hit(node.rect, f.ctx.acquire_widget_id(su->drag));
    };
    n->draw_fn = [](ui::LayoutNode& node, ui::LayoutFrame& f) {
        auto* su = static_cast<SeamUser*>(node.user);
        const ui::WidgetId id = f.ctx.acquire_widget_id(su->drag);
        const bool owns = f.ctx.widget_owns_mouse(id);
        if (f.input.left_pressed() && owns) {
            su->drag->dragging = true;
            f.ctx.set_capture(id);
        }
        if (su->drag->dragging) {
            if (f.input.left_down()) {
                const float d = su->axis == 0 ? f.input.mouse_delta.x
                                              : f.input.mouse_delta.y;
                *su->frac = std::clamp(*su->frac + su->dir * d / su->total,
                                       su->min_frac, su->max_frac);
            } else {
                su->drag->dragging = false;
                f.ctx.clear_capture();
                save_ui_prefs(*su->app);
            }
        }
        const bool hot = owns || su->drag->dragging;
        const ui::Rect& r = node.rect;
        if (su->axis == 0)
            f.canvas.draw_rect({r.x + 2.0f, r.y, 2.0f, r.h},
                               hot ? f.theme.accent_dim : f.theme.hairline);
        else
            f.canvas.draw_rect({r.x, r.y + 2.0f, r.w, 2.0f},
                               hot ? f.theme.accent_dim : f.theme.hairline);
    };
    n->debug_name = "splitter";
    return n;
}

// ASCII ramp atlas for the glyph renderer (spec §6.6): 96 tiles of 8x8,
// printable ASCII sorted by ink coverage so tile index tracks luma.
std::vector<uint8_t> build_ascii_atlas(const ui::Font& font) {
    constexpr uint32_t kW = 128, kH = 48;
    std::vector<uint8_t> atlas(kW * kH, 0);
    const auto& pixels = font.atlas_pixels();
    const uint32_t aw = font.atlas_width();
    const uint32_t ah = font.atlas_height();

    auto glyph_bitmap = [&](uint32_t cp, uint8_t out[64]) {
        std::memset(out, 0, 64);
        const ui::Glyph* g = font.find_glyph(cp);
        if (!g || !g->has_geometry) return;
        const uint32_t x0 = static_cast<uint32_t>(std::lround(g->uv_l * aw));
        const uint32_t y0 = static_cast<uint32_t>(std::lround(g->uv_t * ah));
        const uint32_t gw = std::min(
            8u, static_cast<uint32_t>(std::lround((g->uv_r - g->uv_l) * aw)));
        const uint32_t gh = std::min(
            8u, static_cast<uint32_t>(std::lround((g->uv_b - g->uv_t) * ah)));
        for (uint32_t y = 0; y < gh && y0 + y < ah; ++y)
            for (uint32_t x = 0; x < gw && x0 + x < aw; ++x)
                out[y * 8 + x] = pixels[(y0 + y) * aw + x0 + x];
    };

    struct Entry {
        uint32_t ink;
        uint32_t cp;
    };
    std::vector<Entry> entries;
    for (uint32_t cp = 32; cp < 127; ++cp) {
        uint8_t bm[64];
        glyph_bitmap(cp, bm);
        uint32_t ink = 0;
        for (int i = 0; i < 64; ++i) ink += bm[i];
        entries.push_back({ink, cp});
    }
    std::stable_sort(entries.begin(), entries.end(),
                     [](const Entry& a, const Entry& b) {
                         return a.ink != b.ink ? a.ink < b.ink : a.cp < b.cp;
                     });
    for (uint32_t t = 0; t < 96; ++t) {
        const Entry& e = entries[std::min<size_t>(t, entries.size() - 1)];
        uint8_t bm[64];
        glyph_bitmap(e.cp, bm);
        const uint32_t tx = (t % 16) * 8;
        const uint32_t ty = (t / 16) * 8;
        for (uint32_t y = 0; y < 8; ++y)
            for (uint32_t x = 0; x < 8; ++x)
                atlas[(ty + y) * kW + tx + x] = bm[y * 8 + x];
    }
    return atlas;
}

// ------------------------------------------------------- timeline widgets

struct RulerUser {
    AppState* app;
    FrameUi* out;
    uint32_t frame_count;
    uint32_t playhead;
    double fps;
    uint32_t trim_in;    // resolved (out 0 -> frame_count)
    uint32_t trim_out;
    uint32_t loop_in;
    uint32_t loop_out;   // 0/0 = no loop region
    const ui::UiTexture* thumbs = nullptr;   // filmstrip (spec §3)
    uint32_t thumb_count = 0;
};

void hit_ruler(ui::LayoutNode& node, ui::LayoutFrame& frame) {
    auto* u = static_cast<RulerUser*>(node.user);
    ui::Rect r = node.rect;
    if (!node.clip.empty()) r = r.intersect(node.clip);
    frame.ctx.add_hit(r, frame.ctx.acquire_widget_id(&u->app->ruler));
}

void draw_ruler(ui::LayoutNode& node, ui::LayoutFrame& frame) {
    auto* u = static_cast<RulerUser*>(node.user);
    const ui::Rect& r = node.rect;
    const ui::Theme& theme = frame.theme;
    frame.canvas.draw_sdf_rect(r, 2.0f, theme.control_bg_active);

    if (u->frame_count == 0) return;
    const float per_frame = r.w / static_cast<float>(u->frame_count);
    auto frame_x = [&](double f) {
        return r.x + static_cast<float>(f) * per_frame;
    };

    // Filmstrip (spec §3 thumbnail strip) under everything else.
    if (u->thumbs && u->thumb_count > 0) {
        frame.canvas.draw_image_quad(r, u->thumbs, 0.0f, 0.0f, 1.0f, 1.0f,
                                     ui::Color{1.0f, 1.0f, 1.0f, 0.85f},
                                     2.0f);
    }

    // Trimmed-out zones read as inert.
    ui::Color dim = theme.window_bg;
    dim.a = 0.55f;
    if (u->trim_in > 0)
        frame.canvas.draw_sdf_rect(
            {r.x, r.y, frame_x(u->trim_in) - r.x, r.h}, 2.0f, dim);
    if (u->trim_out < u->frame_count)
        frame.canvas.draw_sdf_rect(
            {frame_x(u->trim_out), r.y,
             r.right() - frame_x(u->trim_out), r.h},
            2.0f, dim);

    // Second ticks.
    if (u->fps > 0.0) {
        for (double f = 0.0; f < u->frame_count; f += u->fps) {
            const float x = frame_x(f);
            frame.canvas.draw_line({x, r.y + r.h * 0.5f}, {x, r.bottom()},
                                   1.0f, theme.hairline);
        }
    }

    // Loop region band (spec §9) along the top edge.
    const float band_h = 4.0f;
    if (u->loop_out > u->loop_in) {
        frame.canvas.draw_sdf_rect(
            {frame_x(u->loop_in), r.y,
             std::max(2.0f, frame_x(u->loop_out) - frame_x(u->loop_in)),
             band_h},
            1.0f, theme.accent_dim);
    }

    // Trim handles: bracket bars at the region edges.
    const float in_x = frame_x(u->trim_in);
    const float out_x = frame_x(u->trim_out);
    frame.canvas.draw_sdf_rect({in_x - 1.0f, r.y, 3.0f, r.h}, 1.0f,
                               theme.text_dim);
    frame.canvas.draw_sdf_rect({out_x - 2.0f, r.y, 3.0f, r.h}, 1.0f,
                               theme.text_dim);

    // Playhead.
    const float px = frame_x(u->playhead + 0.5);
    frame.canvas.draw_line({px, r.y}, {px, r.bottom()}, 2.0f, theme.accent);

    // Interaction: trim handles > loop band (top strip) > scrub.
    RulerState& state = u->app->ruler;
    const ui::WidgetId id = frame.ctx.acquire_widget_id(&state);
    auto mouse_frame = [&] {
        const float t =
            std::clamp((frame.input.mouse.x - r.x) / r.w, 0.0f, 1.0f);
        return static_cast<double>(t) * u->frame_count;
    };
    if (frame.input.left_pressed() && frame.ctx.widget_owns_mouse(id)) {
        const float mx = frame.input.mouse.x;
        const float my = frame.input.mouse.y;
        if (std::abs(mx - in_x) <= 5.0f) state.drag_mode = 2;
        else if (std::abs(mx - out_x) <= 5.0f) state.drag_mode = 3;
        else if (my <= r.y + band_h + 2.0f) {
            state.drag_mode = 4;
            state.loop_anchor = mouse_frame();
        } else {
            state.drag_mode = 1;
        }
        frame.ctx.set_capture(id);
    }
    if (state.drag_mode != 0) {
        const double mf = mouse_frame();
        switch (state.drag_mode) {
            case 1:
                u->out->seek_to = static_cast<float>(
                    std::min(mf, static_cast<double>(u->frame_count - 1)));
                break;
            case 2:
                u->out->trim_in_to = static_cast<float>(
                    std::min(mf, static_cast<double>(u->trim_out) - 1.0));
                break;
            case 3:
                u->out->trim_out_to = static_cast<float>(
                    std::max(mf, static_cast<double>(u->trim_in) + 1.0));
                break;
            case 4: {
                const double a = std::min(state.loop_anchor, mf);
                const double b = std::max(state.loop_anchor, mf);
                if (b - a >= 1.5) {
                    u->out->loop_in_to = static_cast<float>(a);
                    u->out->loop_out_to = static_cast<float>(b);
                }
                break;
            }
        }
        if (frame.input.left_released()) {
            // A click (no real drag) on the loop band clears the region.
            if (state.drag_mode == 4 &&
                std::abs(mouse_frame() - state.loop_anchor) < 1.5)
                u->out->loop_clear = true;
            if (state.drag_mode >= 2) u->out->region_released = true;
            state.drag_mode = 0;
            frame.ctx.clear_capture();
        }
    }
}

struct LaneWidgetUser {
    AppState* app;
    FrameUi* out;
    doc::ParamKey target;
    LaneUiState* state;
    const doc::KeyframeLane* lane;   // stable for this frame
    float min_value, max_value;
    uint32_t frame_count;
    uint32_t playhead;
};

void hit_lane(ui::LayoutNode& node, ui::LayoutFrame& frame) {
    auto* u = static_cast<LaneWidgetUser*>(node.user);
    ui::Rect r = node.rect;
    if (!node.clip.empty()) r = r.intersect(node.clip);
    frame.ctx.add_hit(r, frame.ctx.acquire_widget_id(u->state));
}

void draw_lane(ui::LayoutNode& node, ui::LayoutFrame& frame) {
    auto* u = static_cast<LaneWidgetUser*>(node.user);
    LaneUiState& state = *u->state;
    const ui::Rect& r = node.rect;
    const ui::Theme& theme = frame.theme;
    const float span = std::max(1.0e-6f, u->max_value - u->min_value);
    const uint32_t frames = std::max(1u, u->frame_count);

    auto to_x = [&](double f) {
        return r.x + static_cast<float>(f / frames) * r.w;
    };
    auto to_y = [&](float v) {
        return r.bottom() - (v - u->min_value) / span * r.h;
    };
    auto from_x = [&](float x) {
        return std::clamp(static_cast<double>((x - r.x) / r.w) * frames, 0.0,
                          static_cast<double>(frames - 1));
    };
    auto from_y = [&](float y) {
        return std::clamp(u->min_value + (r.bottom() - y) / r.h * span,
                          u->min_value, u->max_value);
    };

    frame.canvas.draw_sdf_rect(r, 2.0f, theme.control_bg_active);
    frame.canvas.draw_rect_outline(r, 1.0f, theme.hairline);
    // Everything inside the strip clips to it — keys whose values sit
    // outside the shown range must never paint over the panel.
    frame.canvas.push_clip(r);
    // Muted lanes render dimmed (keys kept, param not driven).
    const float lane_alpha = u->lane->muted ? 0.35f : 1.0f;

    const auto& keys = u->lane->keys;
    if (state.selected >= static_cast<int>(keys.size())) state.selected = -1;

    // Sampled curve.
    if (!keys.empty()) {
        const int steps = std::max(2, static_cast<int>(r.w / 3.0f));
        Vec2 prev{};
        for (int i = 0; i <= steps; ++i) {
            const double f = static_cast<double>(i) / steps * frames;
            const float v = std::clamp(mod::eval_lane(*u->lane, f),
                                       u->min_value, u->max_value);
            const Vec2 p{to_x(f), to_y(v)};
            if (i > 0)
                frame.canvas.draw_line(
                    prev, p, 1.0f,
                    theme.accent_dim.with_alpha(lane_alpha));
            prev = p;
        }
    }

    // Playhead.
    const float px = to_x(u->playhead + 0.5);
    frame.canvas.draw_line({px, r.y}, {px, r.bottom()}, 1.0f,
                           theme.accent.with_alpha(0.5f));

    // Keys (+ selected key's bezier handle dots).
    for (size_t i = 0; i < keys.size(); ++i) {
        const Vec2 p{to_x(keys[i].frame), to_y(keys[i].value)};
        const bool selected = static_cast<int>(i) == state.selected;
        frame.canvas.draw_sdf_rect({p.x - 3, p.y - 3, 6, 6}, 1.0f,
                                   (selected ? theme.text : theme.accent)
                                       .with_alpha(lane_alpha));
        if (selected && !keys[i].hold) {
            const Vec2 out_p{to_x(keys[i].frame + keys[i].out_dx),
                                 to_y(keys[i].value + keys[i].out_dy)};
            const Vec2 in_p{to_x(keys[i].frame + keys[i].in_dx),
                                to_y(keys[i].value + keys[i].in_dy)};
            frame.canvas.draw_line(p, out_p, 1.0f, theme.text_dim);
            frame.canvas.draw_line(p, in_p, 1.0f, theme.text_dim);
            frame.canvas.draw_sdf_rect({out_p.x - 2, out_p.y - 2, 4, 4}, 2.0f,
                                       theme.text_dim);
            frame.canvas.draw_sdf_rect({in_p.x - 2, in_p.y - 2, 4, 4}, 2.0f,
                                       theme.text_dim);
        }
    }
    frame.canvas.pop_clip();

    // ---- interaction (queued into FrameUi, applied post-frame).
    const ui::WidgetId id = frame.ctx.acquire_widget_id(&state);
    const bool owns = frame.ctx.widget_owns_mouse(id);
    const Vec2 mouse = frame.input.mouse;
    const bool dragging =
        state.dragging_key || state.dragging_in || state.dragging_out;

    auto emit = [&](std::vector<doc::Keyframe> new_keys) {
        u->out->lane_edits.push_back({u->target, std::move(new_keys)});
    };

    // Right-click a key deletes it (Ctrl+click still works).
    if ((frame.input.buttons_pressed & ui::kMouseRight) && owns &&
        !dragging) {
        int hit = -1;
        float best = 8.0f;
        for (size_t i = 0; i < keys.size(); ++i) {
            const float dx = to_x(keys[i].frame) - mouse.x;
            const float dy = to_y(keys[i].value) - mouse.y;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d < best) {
                best = d;
                hit = static_cast<int>(i);
            }
        }
        if (hit >= 0) {
            std::vector<doc::Keyframe> edited = keys;
            edited.erase(edited.begin() + hit);
            state.selected = -1;
            emit(std::move(edited));
        }
    }

    if (frame.input.left_pressed() && owns && !dragging) {
        // Nearest key within grab range?
        int grabbed = -1;
        float best = 8.0f;
        for (size_t i = 0; i < keys.size(); ++i) {
            const float dx = to_x(keys[i].frame) - mouse.x;
            const float dy = to_y(keys[i].value) - mouse.y;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d < best) {
                best = d;
                grabbed = static_cast<int>(i);
            }
        }
        if (grabbed >= 0 && (frame.input.mods & platform::kModCtrl)) {
            std::vector<doc::Keyframe> edited = keys;
            edited.erase(edited.begin() + grabbed);
            state.selected = -1;
            emit(std::move(edited));
        } else if (grabbed >= 0) {
            state.selected = grabbed;
            state.dragging_key = true;
            frame.ctx.set_capture(id);
        } else {
            // Handle dots of the selected key?
            bool on_handle = false;
            if (state.selected >= 0) {
                const doc::Keyframe& k = keys[state.selected];
                const Vec2 out_p{to_x(k.frame + k.out_dx),
                                     to_y(k.value + k.out_dy)};
                const Vec2 in_p{to_x(k.frame + k.in_dx),
                                    to_y(k.value + k.in_dy)};
                auto near_point = [&](Vec2 p) {
                    const float dx = p.x - mouse.x;
                    const float dy = p.y - mouse.y;
                    return dx * dx + dy * dy < 36.0f;
                };
                if (near_point(out_p)) {
                    state.dragging_out = true;
                    on_handle = true;
                } else if (near_point(in_p)) {
                    state.dragging_in = true;
                    on_handle = true;
                }
                if (on_handle) frame.ctx.set_capture(id);
            }
            if (!on_handle) {
                // Add a key at the click point; remember its frame so
                // the recovery below grabs THIS key, never a neighbour.
                doc::Keyframe k;
                k.frame = std::round(from_x(mouse.x));
                k.value = from_y(mouse.y);
                std::vector<doc::Keyframe> edited = keys;
                edited.push_back(k);
                state.selected = -1;
                state.pending_add_frame = k.frame;
                state.dragging_key = true;   // allow drag-through placement
                frame.ctx.set_capture(id);
                emit(std::move(edited));
            }
        }
    }

    if (dragging && frame.input.left_down() && state.selected >= 0 &&
        state.selected < static_cast<int>(keys.size())) {
        std::vector<doc::Keyframe> edited = keys;
        doc::Keyframe& k = edited[state.selected];
        if (state.dragging_key) {
            double f = std::round(from_x(mouse.x));
            // Stay between neighbors so ordering (and selection) is stable.
            if (state.selected > 0)
                f = std::max(f, edited[state.selected - 1].frame + 1.0);
            if (state.selected + 1 < static_cast<int>(edited.size()))
                f = std::min(f, edited[state.selected + 1].frame - 1.0);
            k.frame = f;
            k.value = from_y(mouse.y);
        } else if (state.dragging_out) {
            k.out_dx = std::max(0.0f,
                                static_cast<float>(from_x(mouse.x) - k.frame));
            k.out_dy = from_y(mouse.y) - k.value;
        } else if (state.dragging_in) {
            k.in_dx = std::min(0.0f,
                               static_cast<float>(from_x(mouse.x) - k.frame));
            k.in_dy = from_y(mouse.y) - k.value;
        }
        emit(std::move(edited));
    }

    // A freshly added key becomes selected once the doc has it (next
    // frame) — matched by the EXACT frame recorded at add time. The old
    // nearest-to-mouse recovery grabbed a neighbouring key when keys
    // clustered, so the drag moved the wrong one.
    if (state.dragging_key && state.selected < 0 && !keys.empty()) {
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i].frame == state.pending_add_frame) {
                state.selected = static_cast<int>(i);
                break;
            }
        if (state.selected < 0) {
            float best = 1.0e9f;
            for (size_t i = 0; i < keys.size(); ++i) {
                const float dx = to_x(keys[i].frame) - mouse.x;
                const float dy = to_y(keys[i].value) - mouse.y;
                if (dx * dx + dy * dy < best) {
                    best = dx * dx + dy * dy;
                    state.selected = static_cast<int>(i);
                }
            }
        }
    }

    if (dragging && frame.input.left_released()) {
        state.dragging_key = state.dragging_in = state.dragging_out = false;
        frame.ctx.clear_capture();
        u->out->lane_release = true;
    }
}

// One parameter row on the design grid: [mod gutter ~ k e][label][slider].
// Rows without mod targets get a blank gutter so every slider in a panel
// starts on the same column; `expose_*` appends the group-face micro.
ui::LayoutNode* param_row(ui::LayoutArena& arena, const char* label,
                          ui::LayoutNode* slider,
                          ui::ButtonState* route_state, bool* route_clicked,
                          ui::ButtonState* key_state, bool* key_clicked,
                          ui::ButtonState* expose_state = nullptr,
                          bool* expose_clicked = nullptr,
                          const char* expose_tip = nullptr) {
    using namespace ui;
    LabelOpts small_dim;
    small_dim.color = active_theme().text_dim;
    small_dim.size = active_theme().font_size_small;
    // The mod gutter is ALWAYS three 18 px slots (wave, key, knob) — absent
    // controls leave blank slots so the label and value columns never shift
    // between grouped/ungrouped/unmodulatable rows.
    std::vector<LayoutNode*> cells;
    if (route_clicked) {
        ButtonOpts micro;
        micro.width = SizeSpec::fixed(18);
        micro.tooltip = "add modulation route";
        cells.push_back(
            IconButton(arena, Icon::Wave, route_state, route_clicked, micro));
        micro.tooltip = "toggle keyframe at playhead";
        cells.push_back(
            IconButton(arena, Icon::Key, key_state, key_clicked, micro));
        if (expose_clicked) {
            micro.tooltip = expose_tip ? expose_tip
                                       : "expose on the group face";
            cells.push_back(IconButton(arena, Icon::Knob, expose_state,
                                       expose_clicked, micro));
        } else {
            cells.push_back(SizedBox(arena, SizeSpec::fixed(18),
                                     SizeSpec::fixed(1), nullptr));
        }
    } else {
        cells.push_back(
            SizedBox(arena, SizeSpec::fixed(58), SizeSpec::fixed(1), nullptr));
    }
    cells.push_back(SizedBox(arena, SizeSpec::fixed(80), SizeSpec::fixed(18),
                             Label(arena, label, small_dim)));
    cells.push_back(slider);
    StackOpts row;
    row.gap = 2.0f;
    row.cross_align = AlignMode::Center;
    LayoutNode* n = VStackDyn(arena, row, cells);
    n->kind = NodeKind::HStack;
    return n;
}

// Label/value row on the same grid: [blank gutter][label][value control].
// Enum-ish values (mask, blend, time mode) all share this shape.
ui::LayoutNode* value_row(ui::LayoutArena& arena, const char* label,
                          ui::LayoutNode* value) {
    using namespace ui;
    LabelOpts small_dim;
    small_dim.color = active_theme().text_dim;
    small_dim.size = active_theme().font_size_small;
    std::vector<LayoutNode*> cells{
        SizedBox(arena, SizeSpec::fixed(58), SizeSpec::fixed(1), nullptr),
        SizedBox(arena, SizeSpec::fixed(80), SizeSpec::fixed(18),
                 Label(arena, label, small_dim)),
        value};
    StackOpts row;
    row.gap = 2.0f;
    row.cross_align = AlignMode::Center;
    LayoutNode* n = VStackDyn(arena, row, cells);
    n->kind = NodeKind::HStack;
    return n;
}

// One effect's inspector panel: header (name, reorder, remove), bypass,
// wet/dry + opacity, then the per-type params.
ui::LayoutNode* build_effect_panel(ui::LayoutArena& arena, AppState& app,
                                   FrameUi& out, size_t fx_index) {
    using namespace ui;
    const doc::EffectInstance& fx = app.document.layers[app.selected_layer].stack[fx_index];
    const doc::EffectInfo& info = doc::effect_info(fx.type);
    EffectUiState& state = app.fx_ui[fx.id];

    LabelOpts small_dim;
    small_dim.color = active_theme().text_dim;
    small_dim.size = active_theme().font_size_small;

    FxRowActions row{};
    row.layer_index = app.selected_layer;
    row.fx_index = fx_index;
    row.up = arena.alloc<bool>();
    row.down = arena.alloc<bool>();
    row.remove = arena.alloc<bool>();
    row.bypass_changed = arena.alloc<bool>();
    row.bypass_staged = arena.alloc<bool>();
    *row.bypass_staged = fx.bypass;
    row.group_toggle = arena.alloc<bool>();
    row.randomize = arena.alloc<bool>();
    row.solo_changed = arena.alloc<bool>();
    row.solo_staged = arena.alloc<bool>();
    *row.solo_staged = !fx.solo;   // "s" click applies this
    row.duplicate = arena.alloc<bool>();

    // Group context: "g" groups with the effect above (or joins its group);
    // grouped effects leave on "g". The knob micro next to a param
    // toggles it on the group FACE (v5.3 exposed params).
    const doc::Group* fx_group = nullptr;
    for (const doc::Group& g :
         app.document.layers[app.selected_layer].groups)
        if (g.id == fx.group_id) fx_group = &g;

    // Header: name, then icon controls in FIXED-width columns that line up
    // across every card. The eye is the bypass toggle, with the rest of the
    // per-effect switches — no separate checkbox row.
    ButtonOpts tiny;
    tiny.width = SizeSpec::fixed(20);
    ButtonOpts eye_opts = tiny;
    eye_opts.tooltip = fx.bypass ? "enable effect" : "bypass effect";
    ButtonOpts solo_opts = tiny;
    solo_opts.tooltip = fx.solo ? "unsolo" : "solo (mute the rest)";
    ButtonOpts copy_opts = tiny;
    copy_opts.tooltip = "duplicate effect";
    ButtonOpts dice_opts = tiny;
    dice_opts.tooltip = "randomize this effect";
    ButtonOpts link_opts = tiny;
    link_opts.disabled = fx.group_id == 0 && fx_index == 0;
    link_opts.tooltip = fx.group_id ? "leave group"
                                    : "group with the effect above";
    ButtonOpts tiny_up = tiny;
    tiny_up.tooltip = "move up the stack";
    tiny_up.disabled = fx_index == 0;
    ButtonOpts tiny_down = tiny;
    tiny_down.tooltip = "move down the stack";
    tiny_down.disabled =
        fx_index + 1 == app.document.layers[app.selected_layer].stack.size();
    ButtonOpts tiny_x = tiny;
    tiny_x.tooltip = "remove effect";

    *row.bypass_staged = !fx.bypass;   // eye click applies this
    std::vector<LayoutNode*> rows;
    rows.push_back(HStack(
        arena, {2.0f},
        {
            Label(arena, info.label),
            Spacer(arena),
            IconButton(arena, fx.bypass ? Icon::EyeOff : Icon::Eye,
                       &state.bypass_button, row.bypass_changed, eye_opts),
            IconButton(arena, fx.solo ? Icon::SoloOn : Icon::Solo,
                       &state.solo_button, row.solo_changed, solo_opts),
            IconButton(arena, Icon::Copy, &state.copy_button, row.duplicate,
                       copy_opts),
            IconButton(arena, Icon::Dice, &state.rnd_button, row.randomize,
                       dice_opts),
            IconButton(arena, Icon::Link, &state.group_button,
                       row.group_toggle, link_opts),
            IconButton(arena, Icon::Up, &state.up_button, row.up, tiny_up),
            IconButton(arena, Icon::Down, &state.down_button, row.down,
                       tiny_down),
            IconButton(arena, Icon::Close, &state.remove_button, row.remove,
                       tiny_x),
        }));

    // Mask assignment: label + dropdown of [none, every mask] on the grid.
    {
        FrameUi::EffectMaskCycle pick{fx_index, arena.alloc<int>()};
        *pick.selected = -1;
        const int mask_count =
            static_cast<int>(std::min<size_t>(app.document.masks.size(), 16));
        const char** items = arena.alloc<const char*>(
            static_cast<size_t>(mask_count) + 1);
        items[0] = "none";
        int current = 0;
        for (int m = 0; m < mask_count; ++m) {
            const doc::Mask& mk = app.document.masks[static_cast<size_t>(m)];
            items[m + 1] = arena.dup(mk.name.c_str(), mk.name.size());
            if (mk.id == fx.mask_id) current = m + 1;
        }
        rows.push_back(value_row(
            arena, "mask",
            Dropdown(arena, items, mask_count + 1, current, &state.mask_dd,
                     pick.selected, SizeSpec::fill(),
                     "gate this effect with a mask")));
        out.effect_mask_cycles.push_back(pick);
    }

    uint32_t ordinal = 0;
    auto stage_slider = [&](int param_index, const char* label, float min_v,
                            float max_v, float value, const char* format,
                            SliderState* slider_state,
                            const char* options = nullptr) {
        value = shown_param_value(app, {fx.id, param_index}, value, min_v,
                                  max_v);
        ParamStage stage{};
        stage.layer_index = app.selected_layer;
        stage.fx_index = fx_index;
        stage.param_index = param_index;
        stage.staged = arena.alloc<float>();
        *stage.staged = value;
        stage.original = value;
        stage.changed = arena.alloc<bool>();
        stage.released = arena.alloc<bool>();
        SliderOpts opts;
        opts.format = format;
        opts.out_changed = stage.changed;
        opts.out_released = stage.released;

        // One grid row: [~ k (m)] mod gutter, label, slider (param_row).
        const doc::ParamKey key{fx.id, param_index};
        FrameUi::AddRoute add_route{key, arena.alloc<bool>()};
        FrameUi::KeyToggle key_toggle{key, value, arena.alloc<bool>()};
        const uint32_t o = ordinal < 18 ? ordinal : 17;
        ++ordinal;
        // Grouped member: the knob micro toggles this param on/off the
        // group FACE (v5.3 exposed params — direct aliases, no macros).
        ui::ButtonState* expose_state = nullptr;
        bool* expose_clicked = nullptr;
        const char* expose_tip = nullptr;
        if (fx_group) {
            const bool on =
                std::find(fx_group->exposed.begin(),
                          fx_group->exposed.end(),
                          key) != fx_group->exposed.end();
            FrameUi::ExposeToggle toggle{app.selected_layer, fx.group_id,
                                         key, !on, arena.alloc<bool>()};
            expose_state = &state.expose_buttons[o];
            expose_clicked = toggle.clicked;
            expose_tip = on ? "hide from the group face"
                            : "expose on the group face";
            out.expose_toggles.push_back(toggle);
        }
        // Selector params render as DROPDOWNS (v5.6): a pick lands as
        // the param value through the same undoable command path.
        LayoutNode* control;
        if (options && param_index >= 0) {
            const int n = doc::param_option_count(options);
            const char** items = arena.alloc<const char*>(
                static_cast<size_t>(n));
            for (int oi = 0; oi < n; ++oi) {
                int olen = 0;
                const char* os = doc::param_option_at(options, oi, &olen);
                items[oi] = arena.dup(os, static_cast<size_t>(olen));
            }
            const int cur = std::clamp(
                static_cast<int>(value - min_v + 0.5f), 0,
                n > 0 ? n - 1 : 0);
            FrameUi::ParamPick pick{app.selected_layer, fx_index,
                                    param_index, min_v, arena.alloc<int>()};
            *pick.selected = -1;
            out.param_picks.push_back(pick);
            control = Dropdown(arena, items, n, cur,
                               &state.param_dd[o < 16 ? o : 15],
                               pick.selected, SizeSpec::fill());
        } else {
            control = SliderF(arena, stage.staged, min_v, max_v,
                              slider_state, opts);
        }
        rows.push_back(param_row(
            arena, label, control,
            &state.route_buttons[o], add_route.clicked,
            &state.key_buttons[o], key_toggle.clicked, expose_state,
            expose_clicked, expose_tip));
        out.add_routes.push_back(add_route);
        out.key_toggles.push_back(key_toggle);
        out.params.push_back(stage);
    };

    stage_slider(doc::kWetParam, "wet/dry", 0.0f, 1.0f, fx.wet, "%.2f",
                 &state.wet);
    stage_slider(doc::kOpacityParam, "opacity", 0.0f, 1.0f, fx.opacity, "%.2f",
                 &state.opacity);
    for (uint32_t p = 0; p < info.param_count && p < 16; ++p) {
        const doc::ParamDesc& desc = info.params[p];
        const char* options =
            fx.type == doc::EffectType::TextOverlay && p == 0 &&
                    !app.font_options.empty()
                ? app.font_options.c_str()
                : desc.options;
        stage_slider(static_cast<int>(p), desc.label, desc.min_value,
                     desc.max_value, fx.params[p], desc.format,
                     &state.params[p], options);
    }
    if (fx.type == doc::EffectType::TextOverlay) {
        // The STRING field (v5.6): click opens the shared inline editor;
        // the buffer (with caret) shows here while editing.
        FrameUi::TextEditOpen open{fx.id, arena.alloc<bool>()};
        const bool editing = app.text_edit_id == fx.id;
        std::string shown = editing ? app.text_edit_buf : fx.text;
        if (editing) shown += '_';
        ButtonOpts bo;
        bo.align_left = true;
        bo.width = SizeSpec::fill();
        bo.tooltip = "edit the text (enter commits, esc cancels)";
        rows.push_back(value_row(
            arena, "text",
            Button(arena, arena.dup(shown.c_str(), shown.size()),
                   &state.text_button, open.clicked, bo)));
        out.text_edit_opens.push_back(open);
    }

    out.rows.push_back(row);

    StackOpts column;
    column.gap = 4.0f;
    column.cross_align = AlignMode::Stretch;
    return Panel(arena, VStackDyn(arena, column, rows),
                 PanelOpts{Edges::all(8), -1.0f, /*outline=*/false});
}

// Group container (spec §5): ONE outlined panel holding the header
// (fold/eye/save/ungroup), the exposed FACE rows, and the member cards
// nested inside with an indent — grouping is containment, not a floating
// header. Folded, the container collapses to header + face.
ui::LayoutNode* build_group_panel(ui::LayoutArena& arena, AppState& app,
                                  FrameUi& out, const doc::Group& group,
                                  const std::vector<size_t>& members,
                                  bool inspector = false) {
    using namespace ui;
    GroupUiState& state = app.group_ui[group.id];
    LabelOpts small_dim;
    small_dim.color = active_theme().text_dim;
    small_dim.size = active_theme().font_size_small;

    FrameUi::GroupActions actions{};
    actions.group_id = group.id;
    actions.fold = arena.alloc<bool>();
    actions.bypass_changed = arena.alloc<bool>();
    actions.bypass_staged = arena.alloc<bool>();
    *actions.bypass_staged = group.bypass;
    actions.ungroup = arena.alloc<bool>();
    actions.save = arena.alloc<bool>();

    // One-line header: chevron fold (the app-wide fold language — no
    // shifting +/- text), then eye + save + ungroup at fixed widths.
    ButtonOpts save_opts;
    save_opts.flat = true;
    save_opts.width = SizeSpec::fixed(28);
    save_opts.tooltip = "save group as preset";
    ButtonOpts ungroup_opts;
    ungroup_opts.width = SizeSpec::fixed(20);
    ungroup_opts.tooltip = "ungroup";

    std::vector<LayoutNode*> rows;
    const char* title = group.name.empty() ? "group" : group.name.c_str();
    *actions.bypass_staged = !group.bypass;   // eye click applies this
    StackOpts hdr;
    hdr.gap = 2.0f;
    hdr.cross_align = AlignMode::Center;
    ButtonOpts group_eye;
    group_eye.width = SizeSpec::fixed(20);
    group_eye.tooltip = group.bypass ? "enable group" : "bypass group";
    std::vector<LayoutNode*> hdr_cells{
        SectionHeader(arena, title, !group.folded, &state.fold_button,
                      actions.fold, /*small=*/true),
        IconButton(arena, group.bypass ? Icon::EyeOff : Icon::Eye,
                   &state.bypass_check, actions.bypass_changed, group_eye),
        Button(arena, "sv", &state.save_button, actions.save, save_opts),
        IconButton(arena, Icon::Close, &state.ungroup_button, actions.ungroup,
                   ungroup_opts)};
    LayoutNode* hdr_stack = VStackDyn(arena, hdr, hdr_cells);
    hdr_stack->kind = NodeKind::HStack;
    rows.push_back(hdr_stack);

    // The FACE (v5.3): exposed member params as DIRECT aliases — same
    // ParamStage path as any effect slider, the x hides from the face.
    size_t face_i = 0;
    for (const doc::ParamKey& fkey : group.exposed) {
        if (face_i >= 8) break;
        size_t fli = 0, ffi = 0;
        if (!find_effect_by_id(app.document, fkey.effect_id, &fli, &ffi))
            continue;
        const doc::EffectInstance& mfx =
            app.document.layers[fli].stack[ffi];
        const doc::EffectInfo& minfo = doc::effect_info(mfx.type);
        float min_v = 0.0f, max_v = 1.0f, cur = 0.0f;
        const char* pname = "?";
        const char* fmt = "%.2f";
        const char* fopts = nullptr;
        if (fkey.param_index == doc::kWetParam) {
            cur = mfx.wet;
            pname = "wet/dry";
        } else if (fkey.param_index == doc::kOpacityParam) {
            cur = mfx.opacity;
            pname = "opacity";
        } else if (fkey.param_index >= 0 &&
                   fkey.param_index <
                       static_cast<int>(minfo.param_count)) {
            const doc::ParamDesc& d = minfo.params[fkey.param_index];
            cur = mfx.params[static_cast<size_t>(fkey.param_index)];
            min_v = d.min_value;
            max_v = d.max_value;
            pname = d.label;
            fmt = d.format;
            fopts = mfx.type == doc::EffectType::TextOverlay &&
                            fkey.param_index == 0 &&
                            !app.font_options.empty()
                        ? app.font_options.c_str()
                        : d.options;
        } else {
            continue;
        }
        ParamStage stage{fli, ffi, fkey.param_index,
                         arena.alloc<float>(), cur, arena.alloc<bool>(),
                         arena.alloc<bool>()};
        *stage.staged = cur;
        SliderOpts opts;
        opts.format = fmt;
        opts.out_changed = stage.changed;
        opts.out_released = stage.released;
        FrameUi::ExposeToggle hide{fli, group.id, fkey, false,
                                   arena.alloc<bool>()};
        out.expose_toggles.push_back(hide);
        char mline[80];
        std::snprintf(mline, sizeof(mline), "%s: %s", minfo.label, pname);
        ButtonOpts mx;
        mx.width = SizeSpec::fixed(20);
        mx.tooltip = "hide from the group face";
        // Selector aliases keep their dropdown (v5.6).
        LayoutNode* fctl;
        if (fopts && fkey.param_index >= 0) {
            const int fn = doc::param_option_count(fopts);
            const char** fitems =
                arena.alloc<const char*>(static_cast<size_t>(fn));
            for (int oi = 0; oi < fn; ++oi) {
                int olen = 0;
                const char* os = doc::param_option_at(fopts, oi, &olen);
                fitems[oi] = arena.dup(os, static_cast<size_t>(olen));
            }
            FrameUi::ParamPick fpick{fli, ffi, fkey.param_index, min_v,
                                     arena.alloc<int>()};
            *fpick.selected = -1;
            out.param_picks.push_back(fpick);
            fctl = Dropdown(arena, fitems, fn,
                            std::clamp(static_cast<int>(cur - min_v + 0.5f),
                                       0, fn > 0 ? fn - 1 : 0),
                            &state.face_dd[face_i], fpick.selected,
                            SizeSpec::fill());
        } else {
            fctl = SliderF(arena, stage.staged, min_v, max_v,
                           &state.face_sliders[face_i], opts);
        }
        StackOpts mrow;
        mrow.gap = 2.0f;
        mrow.cross_align = AlignMode::Center;
        std::vector<LayoutNode*> mcells{
            SizedBox(arena, SizeSpec::fixed(58), SizeSpec::fixed(1), nullptr),
            SizedBox(arena, SizeSpec::fixed(80), SizeSpec::fixed(18),
                     Label(arena, mline, small_dim)),
            fctl,
            IconButton(arena, Icon::Close, &state.face_remove[face_i],
                       out.expose_toggles.back().clicked, mx)};
        LayoutNode* mstack = VStackDyn(arena, mrow, mcells);
        mstack->kind = NodeKind::HStack;
        rows.push_back(mstack);
        out.params.push_back(stage);
        ++face_i;
    }

    // Members live INSIDE the container, indented under the header. The
    // inspector variant shows header + face only — members are selected
    // individually on the flow canvas.
    if (!group.folded && !inspector)
        for (const size_t idx : members)
            rows.push_back(Padding_(arena, Edges{10.0f, 0.0f, 0.0f, 0.0f},
                                    build_effect_panel(arena, app, out, idx)));

    out.group_actions.push_back(actions);
    StackOpts column;
    column.gap = 4.0f;
    column.cross_align = AlignMode::Stretch;
    return Panel(arena, VStackDyn(arena, column, rows),
                 PanelOpts{Edges::all(6), -1.0f, /*outline=*/true});
}

// Popup source entries (docs/flow_canvas.md v3/v4): sources are just
// nodes you add like anything else. "source: clip" is THE input — a tap
// off the project clip (the adjustment type is gone; a clip tap merged
// back through a Blend IS an adjustment). Order here MUST match the pick
// handler's walk.
static const char* kSrcAddLabels[] = {
    "source: clip",  "source: solid", "source: gradient",
    "source: noise", "source: pattern", "source: osc",
    "source: shape"};
static const doc::LayerSourceKind kSrcAddKinds[] = {
    doc::LayerSourceKind::Clip,
    doc::LayerSourceKind::Solid, doc::LayerSourceKind::Gradient,
    doc::LayerSourceKind::Noise, doc::LayerSourceKind::TestPattern,
    doc::LayerSourceKind::Oscillator, doc::LayerSourceKind::Shape};
constexpr int kSrcAddCount =
    static_cast<int>(sizeof(kSrcAddLabels) / sizeof(kSrcAddLabels[0]));

// Popup value-node entries (docs/flow_canvas.md v4): each spawns an
// UNWIRED mod route (target {0, -1} is inert) at the click point — wiring
// happens by dragging its out port onto a param row. Order here MUST
// match the pick handler's walk.
static const char* kValAddLabels[] = {
    "value: lfo",       "value: random",  "value: audio low",
    "value: audio mid", "value: audio high", "value: onset",
    "value: motion",    "value: bright",  "value: sample",
    "value: region"};
static const doc::ModSourceType kValAddTypes[] = {
    doc::ModSourceType::Lfo,        doc::ModSourceType::Drift,
    doc::ModSourceType::AudioLow,   doc::ModSourceType::AudioMid,
    doc::ModSourceType::AudioHigh,  doc::ModSourceType::AudioOnset,
    doc::ModSourceType::VideoMotion, doc::ModSourceType::VideoBrightness,
    doc::ModSourceType::VideoSample, doc::ModSourceType::VideoRegion};
constexpr int kValAddCount =
    static_cast<int>(sizeof(kValAddLabels) / sizeof(kValAddLabels[0]));

// Node-canvas graph (docs/flow_canvas.md v2): the document translated
// into cards + wires each frame. Positions come from the document; nodes
// never dragged flow through a derived auto-layout (chains left→right per
// layer, aux row below) and only commit a position when moved. Param rows
// carry staged pointers so the existing post-frame handlers apply edits.
// One add-menu row resolved at BUILD time: the pick handler indexes this
// array instead of re-walking the filtered lists (category headers made
// order-matching between build and pick too fragile to keep).
struct AddAction {
    enum Kind : uint8_t { Header, Source, Value, Effect, Frame, FindNode };
    uint8_t kind = Header;
    int32_t index = 0;      // kSrcAddKinds / kValAddTypes / EffectType
    uint64_t node = 0;      // FindNode: canvas id to jump to
};

// Context-menu rows (texed openNodeMenu/openFrameMenu), same idea.
enum class CtxAction : uint8_t {
    Bypass, Duplicate, Group, Ungroup, OpenGroup, RenameGroup, SavePreset,
    AlignLeft, AlignTop, SpreadH, SpreadV, Delete, Export, RenameFrame,
    FrameColor, DeleteFrame,
};

struct FlowBuild {
    flow::Graph* graph;
    flow::Output* events;
    const AddAction* add_actions = nullptr;   // parallels graph->add_items
    const CtxAction* ctx_actions = nullptr;   // parallels graph->ctx_items
};

FlowBuild build_flow(ui::LayoutArena& arena, AppState& app, FrameUi& out,
                     const ui::UiTexture* thumb_tex,
                     const std::unordered_map<uint64_t, uint32_t>*
                         thumb_cells) {
    const doc::Document& d = app.document;
    const size_t n_layers = d.layers.size();

    // Subgraph view (texed enterSubgraph): a nonzero open_group scopes
    // the whole canvas to that group — member cards + In/Out boundary
    // nodes + a breadcrumb, nothing else. The document is untouched;
    // this is pure view scoping.
    size_t scope_li = 0;
    const doc::Group* scope_group = nullptr;
    if (app.open_group) {
        if (find_group_by_id(d, app.open_group, &scope_li))
            for (const doc::Group& g : d.layers[scope_li].groups)
                if (g.id == app.open_group) scope_group = &g;
        if (!scope_group) {
            // Deleted under us (ungroup/undo): back to the main graph,
            // restoring the view it was saved with.
            app.open_group = 0;
            if (app.saved_view_valid) {
                app.canvas_state.pan_x = app.saved_pan_x;
                app.canvas_state.pan_y = app.saved_pan_y;
                app.canvas_state.zoom = app.saved_zoom;
                app.saved_view_valid = false;
            } else {
                app.canvas_state.view_inited = false;
            }
        }
    }
    const uint64_t scope = app.open_group;

    // Live previews: map a document key to its atlas cell UVs. The UVs
    // inset HALF A TEXEL — sampling the exact cell bounds let the linear
    // filter blend in the neighbouring cell's edge row (a sliver of an
    // unrelated node's thumb along the border, glaring on black).
    auto set_preview = [&](flow::Node& nd, uint64_t key) {
        if (!thumb_tex || !thumb_cells) return;
        const auto it = thumb_cells->find(key);
        if (it == thumb_cells->end()) return;
        const float cols =
            static_cast<float>(gfx::Engine::kThumbGridCols);
        const float rows_f =
            static_cast<float>(gfx::Engine::kThumbGridRows);
        const float inset_u = 0.5f / (cols * 160.0f);
        const float inset_v = 0.5f / (rows_f * 90.0f);
        nd.preview = thumb_tex;
        nd.pu0 = static_cast<float>(it->second %
                                    gfx::Engine::kThumbGridCols) / cols +
                 inset_u;
        nd.pv0 = static_cast<float>(it->second /
                                    gfx::Engine::kThumbGridCols) / rows_f +
                 inset_v;
        nd.pu1 = nd.pu0 + 1.0f / cols - 2.0f * inset_u;
        nd.pv1 = nd.pv0 + 1.0f / rows_f - 2.0f * inset_v;
    };

    std::vector<flow::Node> nodes;
    std::vector<flow::Wire> wires;
    std::unordered_map<uint64_t, uint64_t> fx_node;   // effect id → node id
    // Folded groups (v4 subgraphs): members collapse into ONE card that
    // shows the exposed face; links crossing the boundary re-anchor there.
    std::unordered_set<uint64_t> emitted_groups;

    static const char* kModNames[] = {"lfo",    "drift",  "a.low",
                                      "a.mid",  "a.high", "onset",
                                      "motion", "bright", "lfo.bpm",
                                      "env",    "cut",    "beat",
                                      "sample", "region"};
    constexpr size_t kModNameCount =
        sizeof(kModNames) / sizeof(kModNames[0]);
    static_assert(kModNameCount ==
                      static_cast<size_t>(doc::ModSourceType::Count),
                  "canvas mod-source names track the enum");

    const float kAutoX0 = 60.0f;
    const float kAutoPitch = flow::node_width() + 70.0f;
    const float kLanePitch = 520.0f;
    // Rightmost derived slot — the Output card's auto position keys on
    // the GRID extent, never on actual card positions (dragging a node
    // must not tow the auto-laid output along).
    float grid_max_x = kAutoX0;

    auto param_modulated = [&](uint64_t eid, int pi) {
        for (const doc::ModRoute& r : d.mod_routes)
            if (r.target.effect_id == eid && r.target.param_index == pi)
                return true;
        return false;
    };
    auto param_keyed = [&](uint64_t eid, int pi) {
        for (const doc::KeyframeLane& l : d.lanes)
            if (l.target.effect_id == eid && l.target.param_index == pi &&
                !l.keys.empty())
                return true;
        return false;
    };

    for (size_t li = 0; li < n_layers; ++li) {
        const doc::Layer& layer = d.layers[li];
        const float auto_y =
            40.0f + static_cast<float>(n_layers - 1 - li) * kLanePitch;
        float auto_x = kAutoX0;

        // Source card: opacity row inline; dot = visibility, X = remove.
        // Hidden in a scoped view — the In boundary node is the input.
        if (!scope) {
            FrameUi::LayerRow lrow{};
            lrow.index = li;
            lrow.id = layer.id;
            lrow.select = arena.alloc<bool>();
            lrow.visible_changed = arena.alloc<bool>();
            lrow.visible_staged = arena.alloc<bool>();
            *lrow.visible_staged = !layer.visible;
            lrow.blend_selected = arena.alloc<int>();
            *lrow.blend_selected = -1;
            lrow.mask_selected = arena.alloc<int>();
            *lrow.mask_selected = -1;
            lrow.remove = arena.alloc<bool>();
            lrow.up = arena.alloc<bool>();
            lrow.down = arena.alloc<bool>();
            out.layer_rows.push_back(lrow);

            flow::ParamRow* rows = arena.alloc<flow::ParamRow>(1);
            FrameUi::LayerStage stage{};
            stage.layer_id = layer.id;
            stage.field = FrameUi::LayerField::Opacity;
            stage.staged = arena.alloc<float>();
            *stage.staged = layer.opacity;
            stage.original = layer.opacity;
            stage.changed = arena.alloc<bool>();
            stage.released = arena.alloc<bool>();
            out.layer_stages.push_back(stage);
            rows[0].label = "opacity";
            rows[0].min_v = 0.0f;
            rows[0].max_v = 1.0f;
            rows[0].format = "%.2f";
            rows[0].staged = stage.staged;
            rows[0].changed = stage.changed;
            rows[0].released = stage.released;

            flow::Node src{};
            src.id = flow::node_id(flow::NodeKind::Source, layer.id);
            src.kind = flow::NodeKind::Source;
            // The card names WHAT the node is — "clip" is the project's
            // source input, generators name their kind. "layer N" was
            // storage-bag residue (v4 flat graph). Legacy Adjustment
            // layers are clip taps.
            static const char* kSrcTitles[] = {"clip",    "solid",
                                               "gradient", "noise",
                                               "pattern",  "osc",
                                               "clip",     "shape"};
            static_assert(sizeof(kSrcTitles) / sizeof(kSrcTitles[0]) ==
                              static_cast<size_t>(
                                  doc::LayerSourceKind::Count),
                          "source card titles track the enum");
            src.title =
                kSrcTitles[static_cast<size_t>(layer.source) %
                           static_cast<size_t>(
                               doc::LayerSourceKind::Count)];
            src.bypassed = !layer.visible;
            src.has_out = true;
            src.has_mask_port = true;
            src.rows = rows;
            src.row_count = 1;
            src.bypass_clicked = lrow.visible_changed;
            src.remove_clicked = lrow.remove;
            set_preview(src, layer.id | (1ull << 62));
            if (layer.node_x != 0.0f || layer.node_y != 0.0f) {
                src.x = layer.node_x;
                src.y = layer.node_y;
            } else {
                src.x = auto_x;
                src.y = auto_y;
                // Materialize the derived slot (docs/flow_canvas.md v2
                // "auto-layout once"): committed the first frame it
                // appears so later deletions never re-slot survivors.
                // Positions are pure UI state the renderer never reads —
                // command-exempt (undo would splice layout writes into
                // gesture coalescing).
                app.document.layers[li].node_x = src.x;
                app.document.layers[li].node_y = src.y;
            }
            // Derived layout is a PURE GRID: the slot advances by pitch
            // regardless of where the card actually sits, so dragging one
            // node never shifts auto-laid neighbours.
            auto_x += kAutoPitch;
            nodes.push_back(src);
            if (layer.mask_id)
                wires.push_back({flow::node_id(flow::NodeKind::Mask,
                                               layer.mask_id),
                                 src.id, 1});
        }

        for (size_t i = 0; i < layer.stack.size(); ++i) {
            const doc::EffectInstance& fx = layer.stack[i];
            const doc::EffectInfo& info = doc::effect_info(fx.type);

            // Scoped view: only the open group's members get cards.
            if (scope && fx.group_id != scope) continue;

            // Grouped member in the MAIN view: no card of its own — the
            // group always renders as ONE card (texed subgraph node;
            // double-click enters it), wires re-anchor on the card.
            const doc::Group* folded = nullptr;
            if (!scope)
                for (const doc::Group& g : layer.groups)
                    if (g.id == fx.group_id) folded = &g;
            if (folded) {
                const uint64_t gid =
                    flow::node_id(flow::NodeKind::Group, folded->id);
                fx_node[fx.id] = gid;
                if (fx.mask_id)
                    wires.push_back({flow::node_id(flow::NodeKind::Mask,
                                                   fx.mask_id),
                                     gid, 1});
                if (!emitted_groups.insert(folded->id).second) continue;

                // Face rows (v5.3): exposed member params as DIRECT
                // aliases — the same ParamStage path as effect cards,
                // keyed/modulated tints included.
                flow::ParamRow* rows = arena.alloc<flow::ParamRow>(6);
                int slot = 0;
                for (const doc::ParamKey& fkey : folded->exposed) {
                    if (slot >= 6) break;
                    size_t ffi = SIZE_MAX;
                    for (size_t s = 0; s < layer.stack.size(); ++s)
                        if (layer.stack[s].id == fkey.effect_id) ffi = s;
                    if (ffi == SIZE_MAX) continue;
                    const doc::EffectInstance& mfx = layer.stack[ffi];
                    const doc::EffectInfo& minfo =
                        doc::effect_info(mfx.type);
                    float min_v = 0.0f, max_v = 1.0f, cur = 0.0f;
                    const char* pname = "?";
                    const char* fmt = "%.2f";
                    const char* fopts = nullptr;
                    if (fkey.param_index == doc::kWetParam) {
                        cur = mfx.wet;
                        pname = "wet/dry";
                    } else if (fkey.param_index == doc::kOpacityParam) {
                        cur = mfx.opacity;
                        pname = "opacity";
                    } else if (fkey.param_index >= 0 &&
                               fkey.param_index <
                                   static_cast<int>(minfo.param_count)) {
                        const doc::ParamDesc& pd =
                            minfo.params[fkey.param_index];
                        cur = mfx.params[static_cast<size_t>(
                            fkey.param_index)];
                        min_v = pd.min_value;
                        max_v = pd.max_value;
                        pname = pd.label;
                        fmt = pd.format;
                        // Selector aliases keep their dropdown (v5.6);
                        // the Text font selector keeps the runtime list.
                        fopts = mfx.type == doc::EffectType::TextOverlay &&
                                        fkey.param_index == 0 &&
                                        !app.font_options.empty()
                                    ? app.font_options.c_str()
                                    : pd.options;
                    } else {
                        continue;
                    }
                    ParamStage stage{li, ffi, fkey.param_index,
                                     arena.alloc<float>(), cur,
                                     arena.alloc<bool>(),
                                     arena.alloc<bool>()};
                    *stage.staged = cur;
                    out.params.push_back(stage);
                    FrameUi::KeyToggle ktog{fkey, cur,
                                            arena.alloc<bool>(), true};
                    out.key_toggles.push_back(ktog);
                    rows[slot].label = arena.dup(pname,
                                                 std::strlen(pname));
                    rows[slot].min_v = min_v;
                    rows[slot].max_v = max_v;
                    rows[slot].format = fmt;
                    if (fopts) {
                        rows[slot].kind = 1;
                        rows[slot].options = fopts;
                    }
                    rows[slot].staged = stage.staged;
                    rows[slot].changed = stage.changed;
                    rows[slot].released = stage.released;
                    rows[slot].key_clicked = ktog.clicked;
                    rows[slot].route_clicked = arena.alloc<bool>();
                    rows[slot].modulated =
                        param_modulated(fkey.effect_id,
                                        fkey.param_index);
                    rows[slot].keyed =
                        param_keyed(fkey.effect_id, fkey.param_index);
                    ++slot;
                }
                flow::Node gn{};
                gn.id = gid;
                gn.kind = flow::NodeKind::Group;
                gn.title = folded->name.empty()
                    ? "group"
                    : arena.dup(folded->name.c_str(),
                                folded->name.size());
                gn.bypassed = folded->bypass;
                gn.has_in = true;
                gn.has_out = true;
                gn.remove_clicked = arena.alloc<bool>();
                out.group_removes.push_back(
                    {folded->id, gn.remove_clicked});
                // Title bypass dot, same GroupActions path as the rail
                // eye (the handler finds the group's own layer).
                FrameUi::GroupActions gact{};
                gact.group_id = folded->id;
                gact.fold = arena.alloc<bool>();
                gact.bypass_changed = arena.alloc<bool>();
                gact.bypass_staged = arena.alloc<bool>();
                *gact.bypass_staged = !folded->bypass;
                gact.ungroup = arena.alloc<bool>();
                gact.save = arena.alloc<bool>();
                out.group_actions.push_back(gact);
                gn.bypass_clicked = gact.bypass_changed;
                gn.rows = rows;
                gn.row_count = slot;
                // Preview: the LAST member's tap ≈ the group's output.
                uint64_t last_member = fx.id;
                for (const doc::EffectInstance& e : layer.stack)
                    if (e.group_id == folded->id) last_member = e.id;
                set_preview(gn, last_member);
                if (folded->node_x != 0.0f || folded->node_y != 0.0f) {
                    gn.x = folded->node_x;
                    gn.y = folded->node_y;
                } else {
                    gn.x = auto_x;
                    gn.y = auto_y;
                    for (doc::Group& mg : app.document.layers[li].groups)
                        if (mg.id == folded->id) {
                            mg.node_x = gn.x;
                            mg.node_y = gn.y;
                        }
                }
                auto_x += kAutoPitch;
                nodes.push_back(gn);
                continue;
            }

            FxRowActions act{};
            act.layer_index = li;
            act.fx_index = i;
            act.up = arena.alloc<bool>();
            act.down = arena.alloc<bool>();
            act.remove = arena.alloc<bool>();
            act.bypass_changed = arena.alloc<bool>();
            act.bypass_staged = arena.alloc<bool>();
            *act.bypass_staged = !fx.bypass;
            act.group_toggle = arena.alloc<bool>();
            act.randomize = arena.alloc<bool>();
            act.solo_changed = arena.alloc<bool>();
            act.solo_staged = arena.alloc<bool>();
            *act.solo_staged = !fx.solo;
            act.duplicate = arena.alloc<bool>();
            out.rows.push_back(act);

            const doc::Group* fx_group = nullptr;
            for (const doc::Group& g : layer.groups)
                if (g.id == fx.group_id) fx_group = &g;

            // Text cards (v5.6) append the STRING row after the params.
            const bool is_text_fx =
                fx.type == doc::EffectType::TextOverlay;
            const uint32_t n_rows = 2 +
                                    std::min<uint32_t>(info.param_count, 16) +
                                    (is_text_fx ? 1 : 0);
            flow::ParamRow* rows = arena.alloc<flow::ParamRow>(n_rows);
            auto stage_row = [&](uint32_t slot, int param_index,
                                 const char* label, float min_v, float max_v,
                                 float value, const char* fmt,
                                 const char* options = nullptr) {
                value = shown_param_value(app, {fx.id, param_index}, value,
                                          min_v, max_v);
                ParamStage stage{};
                stage.layer_index = li;
                stage.fx_index = i;
                stage.param_index = param_index;
                stage.staged = arena.alloc<float>();
                *stage.staged = value;
                stage.original = value;
                stage.changed = arena.alloc<bool>();
                stage.released = arena.alloc<bool>();
                out.params.push_back(stage);
                const doc::ParamKey key{fx.id, param_index};
                FrameUi::AddRoute add_route{key, arena.alloc<bool>()};
                FrameUi::KeyToggle key_toggle{key, value,
                                              arena.alloc<bool>(),
                                              /*lane_toggle=*/true};
                out.add_routes.push_back(add_route);
                out.key_toggles.push_back(key_toggle);
                flow::ParamRow& row = rows[slot];
                row.label = label;
                row.min_v = min_v;
                row.max_v = max_v;
                row.format = fmt;
                row.staged = stage.staged;
                row.changed = stage.changed;
                row.released = stage.released;
                row.route_clicked = add_route.clicked;
                row.key_clicked = key_toggle.clicked;
                // Scoped member rows carry the FACE toggle (v5.3): the
                // e-dot exposes/hides this param on the open group.
                if (scope && fx_group) {
                    const bool on =
                        std::find(fx_group->exposed.begin(),
                                  fx_group->exposed.end(),
                                  key) != fx_group->exposed.end();
                    FrameUi::ExposeToggle toggle{li, fx.group_id, key,
                                                 !on,
                                                 arena.alloc<bool>()};
                    out.expose_toggles.push_back(toggle);
                    row.expose_clicked = toggle.clicked;
                    row.exposed = on;
                }
                row.modulated = param_modulated(fx.id, param_index);
                row.keyed = param_keyed(fx.id, param_index);
                if (options) {
                    row.kind = 1;
                    row.options = options;
                }
            };
            stage_row(0, doc::kWetParam, "wet/dry", 0.0f, 1.0f, fx.wet,
                      "%.2f");
            stage_row(1, doc::kOpacityParam, "opacity", 0.0f, 1.0f,
                      fx.opacity, "%.2f");
            for (uint32_t p = 0; p < info.param_count && p < 16; ++p) {
                const doc::ParamDesc& desc = info.params[p];
                // The Text card's font selector lists the DISCOVERED
                // .ttf files (v5.5b), not a static table.
                const char* options =
                    is_text_fx && p == 0 && !app.font_options.empty()
                        ? app.font_options.c_str()
                        : desc.options;
                stage_row(2 + p, static_cast<int>(p), desc.label,
                          desc.min_value, desc.max_value, fx.params[p],
                          desc.format, options);
            }
            if (is_text_fx) {
                // The STRING row (v5.6): a real field on the card — click
                // to edit inline, same editor the app already runs.
                flow::ParamRow& trow = rows[n_rows - 1];
                trow = {};
                trow.label = "text";
                trow.kind = 2;
                trow.text = fx.text.c_str();
            }

            flow::Node en{};
            en.id = flow::node_id(flow::NodeKind::Effect, fx.id);
            en.kind = flow::NodeKind::Effect;
            en.title = info.label;
            en.tint = static_cast<uint8_t>(info.category);
            en.bypassed = fx.bypass || (fx_group && fx_group->bypass);
            en.solo = fx.solo;
            en.feedback = doc::is_stateful_feedback(fx.type);
            en.has_in = true;
            en.has_out = true;
            en.has_mask_port = true;
            en.has_aux_port = doc::effect_aux_port(fx.type) != nullptr;
            if (en.has_aux_port)
                en.aux_label = doc::effect_aux_port(fx.type);
            en.rows = rows;
            en.row_count = static_cast<int>(n_rows);
            en.bypass_clicked = act.bypass_changed;
            en.remove_clicked = act.remove;
            en.text_edit = fx.type == doc::EffectType::TextOverlay;
            set_preview(en, fx.id);
            if (fx.node_x != 0.0f || fx.node_y != 0.0f) {
                en.x = fx.node_x;
                en.y = fx.node_y;
            } else {
                en.x = auto_x;
                en.y = auto_y;
                app.document.layers[li].stack[i].node_x = en.x;
                app.document.layers[li].stack[i].node_y = en.y;
            }
            auto_x += kAutoPitch;
            nodes.push_back(en);
            fx_node[fx.id] = en.id;
            if (fx.mask_id)
                wires.push_back({flow::node_id(flow::NodeKind::Mask,
                                               fx.mask_id),
                                 en.id, 1});
        }
        grid_max_x = std::max(grid_max_x, auto_x);
    }

    // Scoped view: the In/Out boundary nodes (texed sgin/sgout) flank
    // the member span. Derived chrome — positions recompute each frame,
    // they never move, delete, or persist.
    if (scope) {
        float min_x = 1e9f, max_x = -1e9f, first_y = 40.0f;
        for (const flow::Node& nd : nodes) {
            min_x = std::min(min_x, nd.x);
            max_x = std::max(max_x, nd.x);
            if (nd.x <= min_x) first_y = nd.y;
        }
        if (nodes.empty()) min_x = max_x = kAutoX0 + kAutoPitch;
        // Boundary nodes hold their OWN positions (v5.4) — the member
        // extent only seeds them once, then they materialize like every
        // other card and member drags never tow them.
        flow::Node gin{};
        gin.id = flow::node_id(flow::NodeKind::GroupIn, scope);
        gin.kind = flow::NodeKind::GroupIn;
        gin.title = "in";
        gin.has_out = true;
        if (scope_group->in_x != 0.0f || scope_group->in_y != 0.0f) {
            gin.x = scope_group->in_x;
            gin.y = scope_group->in_y;
        } else {
            gin.x = min_x - kAutoPitch;
            gin.y = first_y;
            for (doc::Group& mg :
                 app.document.layers[scope_li].groups)
                if (mg.id == scope) {
                    mg.in_x = gin.x;
                    mg.in_y = gin.y;
                }
        }
        nodes.push_back(gin);
        flow::Node gout{};
        gout.id = flow::node_id(flow::NodeKind::GroupOut, scope);
        gout.kind = flow::NodeKind::GroupOut;
        gout.title = "out";
        gout.has_in = true;
        if (scope_group->out_x != 0.0f || scope_group->out_y != 0.0f) {
            gout.x = scope_group->out_x;
            gout.y = scope_group->out_y;
        } else {
            gout.x = max_x + kAutoPitch;
            gout.y = first_y;
            for (doc::Group& mg :
                 app.document.layers[scope_li].groups)
                if (mg.id == scope) {
                    mg.out_x = gout.x;
                    mg.out_y = gout.y;
                }
        }
        nodes.push_back(gout);
        // Boundary wires come from the persistent BINDINGS (v5.3
        // intermediaries), never from the outer links — the internal
        // picture holds whether or not anything is connected outside.
        uint64_t first_m = 0, last_m = 0, bind_in = 0, bind_out = 0;
        for (const doc::EffectInstance& e : d.layers[scope_li].stack)
            if (e.group_id == scope) {
                if (!first_m) first_m = e.id;
                last_m = e.id;
                if (scope_group && e.id == scope_group->face_in)
                    bind_in = e.id;
                if (scope_group && e.id == scope_group->face_out)
                    bind_out = e.id;
            }
        const uint64_t in_m = bind_in ? bind_in : first_m;
        const uint64_t out_m = bind_out ? bind_out : last_m;
        if (auto it = fx_node.find(in_m); it != fx_node.end())
            wires.push_back({gin.id, it->second, 0});
        if (auto it = fx_node.find(out_m); it != fx_node.end())
            wires.push_back({it->second, gout.id, 0});
    }

    // Chain + composite wires come from the TRUE-GRAPH link table
    // (docs/flow_canvas.md v3); legacy documents draw the synthesized
    // equivalent. Mask wires were emitted above from the mask_id fields.
    // In a scoped view, links crossing the group boundary re-anchor on
    // the In/Out boundary nodes.
    {
        const std::vector<doc::Document::NodeLink> doc_links =
            d.links.empty() ? doc::synthesize_links(d) : d.links;
        auto canvas_id_of = [&](uint64_t doc_id) -> uint64_t {
            if (doc_id == 0) return flow::kOutNodeId;
            if (layer_index_by_id(d, doc_id) >= 0)
                return flow::node_id(flow::NodeKind::Source, doc_id);
            if (auto it = fx_node.find(doc_id); it != fx_node.end())
                return it->second;
            return 0;
        };
        for (const doc::Document::NodeLink& l : doc_links) {
            if (l.to_port != 0 && l.to_port != 1 && l.to_port != 2)
                continue;
            uint64_t cf = canvas_id_of(l.from);
            uint64_t ct = canvas_id_of(l.to);
            if (scope) {
                const bool from_in = fx_node.count(l.from) != 0;
                const bool to_in = fx_node.count(l.to) != 0;
                if (!from_in && !to_in) continue;
                // Port-0 boundary crossings are represented by the
                // persistent binding wires (drawn above) — link-derived
                // duplicates would vanish when the outer side unplugs.
                if ((!from_in || !to_in) && l.to_port == 0) continue;
                if (!from_in)
                    cf = flow::node_id(flow::NodeKind::GroupIn, scope);
                if (!to_in)
                    ct = flow::node_id(flow::NodeKind::GroupOut, scope);
            }
            // Same-card links are group internals — invisible.
            if (cf && ct && cf != ct)
                wires.push_back(
                    {cf, ct,
                     static_cast<uint8_t>(l.to_port == 2
                                              ? 3
                                              : (l.to_port == 1 ? 1
                                                                : 0))});
        }
    }

    // Scoped view: only masks worn by members (and value nodes driving
    // member params, below) surface alongside the members.
    std::unordered_set<uint64_t> scope_masks;
    if (scope)
        for (const doc::Layer& sl : d.layers)
            for (const doc::EffectInstance& sfx : sl.stack)
                if (sfx.group_id == scope && sfx.mask_id)
                    scope_masks.insert(sfx.mask_id);

    // Aux row: masks, then mod-route sources with inline rate/amount.
    const float aux_y = 40.0f + static_cast<float>(n_layers) * kLanePitch;
    float aux_x = kAutoX0;
    for (size_t mi = 0; mi < d.masks.size(); ++mi) {
        const doc::Mask& m = d.masks[mi];
        if (scope && scope_masks.count(m.id) == 0) continue;
        flow::Node mn{};
        mn.id = flow::node_id(flow::NodeKind::Mask, m.id);
        mn.kind = flow::NodeKind::Mask;
        mn.title = arena.dup(m.name.c_str(), m.name.size());
        // In = the mask's image source (v4: any SOURCE node wires in;
        // the wire mirrors mask.source_layer_id, not the link table).
        mn.has_in = true;
        mn.has_out = true;
        set_preview(mn, m.id | doc::kMaskParamBit);
        if (m.source_layer_id && !scope &&
            layer_index_by_id(d, m.source_layer_id) >= 0)
            wires.push_back({flow::node_id(flow::NodeKind::Source,
                                           m.source_layer_id),
                             mn.id, 0});
        if (m.node_x != 0.0f || m.node_y != 0.0f) {
            mn.x = m.node_x;
            mn.y = m.node_y;
        } else {
            mn.x = aux_x;
            mn.y = aux_y;
            app.document.masks[mi].node_x = mn.x;
            app.document.masks[mi].node_y = mn.y;
        }
        aux_x += kAutoPitch;
        nodes.push_back(mn);
    }
    for (size_t ri = 0; ri < d.mod_routes.size(); ++ri) {
        const doc::ModRoute& r = d.mod_routes[ri];
        if (scope) {
            const uint64_t te = r.target.effect_id;
            const bool member_target =
                (te & doc::kMaskParamBit)
                    ? scope_masks.count(te & ~doc::kMaskParamBit) != 0
                    : fx_node.count(te) != 0;
            if (!member_target) continue;
        }
        FrameUi::RouteRow rr{};
        rr.id = r.id;
        rr.source_selected = arena.alloc<int>();
        *rr.source_selected = -1;
        rr.shape_selected = arena.alloc<int>();
        *rr.shape_selected = -1;
        rr.curve_selected = arena.alloc<int>();
        *rr.curve_selected = -1;
        rr.remove = arena.alloc<bool>();
        rr.rate_staged = arena.alloc<float>();
        rr.rate_changed = arena.alloc<bool>();
        rr.rate_released = arena.alloc<bool>();
        rr.amount_staged = arena.alloc<float>();
        *rr.amount_staged = r.amount;
        rr.amount_original = r.amount;
        rr.amount_changed = arena.alloc<bool>();
        rr.amount_released = arena.alloc<bool>();
        const bool is_lfo = r.source.type == doc::ModSourceType::Lfo ||
                            r.source.type == doc::ModSourceType::LfoBeat;
        const bool is_pulse =
            r.source.type == doc::ModSourceType::Envelope ||
            r.source.type == doc::ModSourceType::Beat;
        const bool has_rate =
            is_lfo || is_pulse ||
            r.source.type == doc::ModSourceType::Drift;
        const bool is_video =
            r.source.type == doc::ModSourceType::VideoSample ||
            r.source.type == doc::ModSourceType::VideoRegion;
        const bool is_region =
            r.source.type == doc::ModSourceType::VideoRegion;
        *rr.rate_staged = is_pulse ? r.source.decay : r.source.rate_hz;
        rr.rate_original = *rr.rate_staged;

        flow::ParamRow* rows = arena.alloc<flow::ParamRow>(6);
        int slot = 0;
        if (has_rate) {
            rows[slot].label = is_pulse ? "decay" : "rate";
            rows[slot].min_v = 0.05f;
            rows[slot].max_v = 8.0f;
            rows[slot].format = is_pulse ? "%.2f s" : "%.2f hz";
            rows[slot].staged = rr.rate_staged;
            rows[slot].changed = rr.rate_changed;
            rows[slot].released = rr.rate_released;
            ++slot;
        }
        if (is_video) {
            // Sampling window rows (v4): point x/y, region w/h.
            static const char* kPosLabels[4] = {"x", "y", "w", "h"};
            const float cur[4] = {r.source.px, r.source.py, r.source.pw,
                                  r.source.ph};
            const int n_pos = is_region ? 4 : 2;
            for (int pi = 0; pi < n_pos; ++pi) {
                rr.pos_staged[pi] = arena.alloc<float>();
                *rr.pos_staged[pi] = cur[pi];
                rr.pos_original[pi] = cur[pi];
                rr.pos_changed[pi] = arena.alloc<bool>();
                rr.pos_released[pi] = arena.alloc<bool>();
                rows[slot].label = kPosLabels[pi];
                rows[slot].min_v = pi >= 2 ? 0.02f : 0.0f;
                rows[slot].max_v = 1.0f;
                rows[slot].format = "%.2f";
                rows[slot].staged = rr.pos_staged[pi];
                rows[slot].changed = rr.pos_changed[pi];
                rows[slot].released = rr.pos_released[pi];
                ++slot;
            }
        }
        rows[slot].label = "amount";
        rows[slot].min_v = -1.0f;
        rows[slot].max_v = 1.0f;
        rows[slot].format = "%+.2f";
        rows[slot].staged = rr.amount_staged;
        rows[slot].changed = rr.amount_changed;
        rows[slot].released = rr.amount_released;
        ++slot;
        out.route_rows.push_back(rr);

        flow::Node rn{};
        rn.id = flow::node_id(flow::NodeKind::ModSource, r.id);
        rn.kind = flow::NodeKind::ModSource;
        rn.title =
            kModNames[static_cast<size_t>(r.source.type) % kModNameCount];
        rn.has_out = true;
        rn.rows = rows;
        rn.row_count = slot;
        rn.remove_clicked = rr.remove;
        if (r.node_x != 0.0f || r.node_y != 0.0f) {
            rn.x = r.node_x;
            rn.y = r.node_y;
        } else {
            rn.x = aux_x;
            rn.y = aux_y;
            app.document.mod_routes[ri].node_x = rn.x;
            app.document.mod_routes[ri].node_y = rn.y;
        }
        aux_x += kAutoPitch;
        nodes.push_back(rn);

        grid_max_x = std::max(grid_max_x, aux_x);

        uint64_t target = 0;
        int to_row = -1;
        if (r.target.effect_id & doc::kMaskParamBit) {
            target = flow::node_id(flow::NodeKind::Mask,
                                   r.target.effect_id & ~doc::kMaskParamBit);
        } else if (auto it = fx_node.find(r.target.effect_id);
                   it != fx_node.end()) {
            target = it->second;
            // Effect cards: row 0 wet, 1 opacity, 2+p params (v4: the mod
            // wire lands on the driven row's gutter). A target hidden in a
            // folded group re-anchors on the group card edge.
            const bool grouped =
                static_cast<flow::NodeKind>((target >> 56) - 1) ==
                flow::NodeKind::Group;
            to_row = grouped ? -1
                : r.target.param_index == doc::kWetParam ? 0
                : r.target.param_index == doc::kOpacityParam ? 1
                : r.target.param_index >= 0 ? 2 + r.target.param_index
                                            : -1;
        }
        if (target) wires.push_back({rn.id, target, 2, to_row});
    }

    // Output card (hidden in a scoped view — GroupOut is the boundary).
    if (!scope) {
        flow::Node on{};
        on.id = flow::kOutNodeId;
        on.kind = flow::NodeKind::Output;
        on.title = "output";
        on.has_in = true;
        set_preview(on, 0);
        if (d.out_node_x != 0.0f || d.out_node_y != 0.0f) {
            on.x = d.out_node_x;
            on.y = d.out_node_y;
        } else {
            on.x = grid_max_x + 20.0f;
            on.y = 40.0f +
                   (n_layers ? static_cast<float>(n_layers - 1) *
                                   kLanePitch * 0.5f
                             : 0.0f);
            app.document.out_node_x = on.x;
            app.document.out_node_y = on.y;
        }
        nodes.push_back(on);
    }

    uint64_t selected = 0;
    switch (app.sel.kind) {
        case SelKind::Effect:
            selected = flow::node_id(flow::NodeKind::Effect, app.sel.id);
            break;
        case SelKind::Group:
            selected = flow::node_id(flow::NodeKind::Group, app.sel.id);
            break;
        case SelKind::LayerSource:
            selected = flow::node_id(flow::NodeKind::Source, app.sel.id);
            break;
        case SelKind::Mask:
            selected = flow::node_id(flow::NodeKind::Mask, app.sel.id);
            break;
        case SelKind::ModSource:
            selected = flow::node_id(flow::NodeKind::ModSource, app.sel.id);
            break;
        case SelKind::Output:
            selected = flow::kOutNodeId;
            break;
        default:
            break;
    }

    // Frames (v3): titled grouping boxes, removable from the canvas.
    // Hidden in a scoped view (they annotate the main graph).
    const size_t n_frames = scope ? 0 : d.frames.size();
    flow::FrameBox* frame_arr = arena.alloc<flow::FrameBox>(
        std::max<size_t>(n_frames, 1));
    for (size_t f = 0; f < n_frames; ++f) {
        frame_arr[f].id = d.frames[f].id;
        frame_arr[f].x = d.frames[f].x;
        frame_arr[f].y = d.frames[f].y;
        frame_arr[f].w = d.frames[f].w;
        frame_arr[f].h = d.frames[f].h;
        frame_arr[f].title = d.frames[f].title.empty()
            ? "frame"
            : arena.dup(d.frames[f].title.c_str(),
                        d.frames[f].title.size());
        frame_arr[f].color = d.frames[f].color;
        frame_arr[f].remove_clicked = arena.alloc<bool>();
        frame_arr[f].color_clicked = arena.alloc<bool>();
    }

    // Folded groups fold many links onto one card edge — identical
    // strokes would over-ink the feathered halo, so dedup exact repeats.
    {
        std::vector<flow::Wire> unique_wires;
        for (const flow::Wire& w : wires) {
            bool dup = false;
            for (const flow::Wire& e : unique_wires)
                dup = dup || (e.from == w.from && e.to == w.to &&
                              e.kind == w.kind && e.to_row == w.to_row);
            if (!dup) unique_wires.push_back(w);
        }
        wires.swap(unique_wires);
    }

    flow::Node* node_arr = arena.alloc<flow::Node>(
        std::max<size_t>(nodes.size(), 1));
    std::copy(nodes.begin(), nodes.end(), node_arr);
    flow::Wire* wire_arr = arena.alloc<flow::Wire>(
        std::max<size_t>(wires.size(), 1));
    std::copy(wires.begin(), wires.end(), wire_arr);

    auto* graph = arena.alloc<flow::Graph>();
    graph->nodes = node_arr;
    graph->node_count = nodes.size();
    graph->wires = wire_arr;
    graph->wire_count = wires.size();
    graph->frames = frame_arr;
    graph->frame_count = n_frames;
    graph->selected = selected;
    if (scope_group) {
        const std::string& gname = scope_group->name.empty()
                                       ? std::string("group")
                                       : scope_group->name;
        graph->crumb = arena.dup(gname.c_str(), gname.size());
    }
    if (!app.sel_wires.empty()) {
        flow::Wire* sw_arr = arena.alloc<flow::Wire>(app.sel_wires.size());
        std::copy(app.sel_wires.begin(), app.sel_wires.end(), sw_arr);
        graph->sel_wires = sw_arr;
        graph->sel_wire_count = app.sel_wires.size();
    }
    graph->rename_frame = app.frame_rename_id;
    graph->rename_node =
        app.group_rename_id
            ? flow::node_id(flow::NodeKind::Group, app.group_rename_id)
            : (app.text_edit_id
                   ? flow::node_id(flow::NodeKind::Effect, app.text_edit_id)
                   : 0);
    // One rename at a time: the buffer belongs to whichever is active.
    const std::string& rename_src =
        app.group_rename_id
            ? app.group_rename_buf
            : (app.text_edit_id ? app.text_edit_buf : app.frame_rename_buf);
    graph->rename_text = arena.dup(rename_src.c_str(), rename_src.size());
    graph->value_edit_node = app.value_edit_node;
    graph->value_edit_row = app.value_edit_row;
    graph->value_edit_text = arena.dup(app.value_edit_buf.c_str(),
                                       app.value_edit_buf.size());
    if (!app.multi_sel.empty()) {
        uint64_t* multi_arr =
            arena.alloc<uint64_t>(app.multi_sel.size());
        std::copy(app.multi_sel.begin(), app.multi_sel.end(), multi_arr);
        graph->multi = multi_arr;
        graph->multi_count = app.multi_sel.size();
    }
    const AddAction* add_actions = nullptr;
    {
        // Cursor add menu content, resolved to ACTIONS at build time —
        // grouped under category headers (texed _fillAddMenu); the
        // filter matches the label OR its category (texed).
        auto lower = [](std::string s) {
            for (char& c : s)
                c = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        const std::string needle = lower(app.fx_filter);
        auto matches = [&](const char* label, const char* cat) {
            if (needle.empty()) return true;
            if (lower(label).find(needle) != std::string::npos)
                return true;
            return cat && lower(cat).find(needle) != std::string::npos;
        };
        std::vector<const char*> items;
        std::vector<uint8_t> headers;
        std::vector<AddAction> actions;
        auto push = [&](const char* label, bool header, AddAction act) {
            items.push_back(label);
            headers.push_back(header ? 1 : 0);
            actions.push_back(act);
        };
        if (app.find_mode) {
            // Ctrl+F: the popup lists NODES; picking jumps to one.
            for (const flow::Node& nd : nodes) {
                if (!matches(nd.title, nullptr)) continue;
                push(nd.title, false,
                     {AddAction::FindNode, 0, nd.id});
            }
        } else {
            // Scoped view adds EFFECTS only (into the open group) —
            // sources/values/frames belong to the main graph.
            if (!scope && matches("frame", "layout")) {
                push("layout", true, {});
                push("frame", false, {AddAction::Frame, 0, 0});
            }
            bool head = false;
            for (int s = 0; !scope && s < kSrcAddCount; ++s) {
                if (!matches(kSrcAddLabels[s], "sources")) continue;
                if (!head) push("sources", true, {}), head = true;
                push(kSrcAddLabels[s], false, {AddAction::Source, s, 0});
            }
            head = false;
            for (int s = 0; !scope && s < kValAddCount; ++s) {
                if (!matches(kValAddLabels[s], "values")) continue;
                if (!head) push("values", true, {}), head = true;
                push(kValAddLabels[s], false, {AddAction::Value, s, 0});
            }
            for (size_t c = 0;
                 c < static_cast<size_t>(doc::FxCategory::Count); ++c) {
                const char* cat = doc::fx_category_label(
                    static_cast<doc::FxCategory>(c));
                head = false;
                for (size_t t = 0;
                     t < static_cast<size_t>(doc::EffectType::Count);
                     ++t) {
                    const doc::EffectInfo& info = doc::effect_info(
                        static_cast<doc::EffectType>(t));
                    if (static_cast<size_t>(info.category) != c) continue;
                    if (!matches(info.label, cat)) continue;
                    if (!head) push(cat, true, {}), head = true;
                    push(info.label, false,
                         {AddAction::Effect, static_cast<int32_t>(t), 0});
                }
            }
        }
        const size_t cap = std::max<size_t>(items.size(), 1);
        const char** item_arr = arena.alloc<const char*>(cap);
        uint8_t* header_arr = arena.alloc<uint8_t>(cap);
        AddAction* action_arr = arena.alloc<AddAction>(cap);
        std::copy(items.begin(), items.end(), item_arr);
        std::copy(headers.begin(), headers.end(), header_arr);
        std::copy(actions.begin(), actions.end(), action_arr);
        graph->add_items = item_arr;
        graph->add_headers = header_arr;
        graph->add_count = items.size();
        graph->add_filter =
            arena.dup(app.fx_filter.c_str(), app.fx_filter.size());
        add_actions = action_arr;
    }
    // Context-menu items for the open menu's target (texed node/frame
    // menus) — labels + actions in lockstep.
    const CtxAction* ctx_actions = nullptr;
    if (app.canvas_state.ctx_open) {
        const uint64_t target = app.canvas_state.ctx_target;
        std::vector<const char*> items;
        std::vector<CtxAction> actions;
        auto push = [&](const char* label, CtxAction act) {
            items.push_back(label);
            actions.push_back(act);
        };
        const flow::NodeKind kind = static_cast<flow::NodeKind>(
            (target >> 56) - 1);
        const uint64_t doc_id = target & 0x00FFFFFFFFFFFFFFull;
        if (target == flow::kOutNodeId) {
            push("export...", CtxAction::Export);
        } else if (kind == flow::NodeKind::Frame) {
            push("rename", CtxAction::RenameFrame);
            push("cycle colour", CtxAction::FrameColor);
            push("delete frame (keeps nodes)", CtxAction::DeleteFrame);
        } else {
            if (kind == flow::NodeKind::Group) {
                push("open (double-click)", CtxAction::OpenGroup);
                push("rename", CtxAction::RenameGroup);
                push("ungroup (ctrl+shift+g)", CtxAction::Ungroup);
                push("save as preset...", CtxAction::SavePreset);
            }
            if (kind == flow::NodeKind::Effect ||
                kind == flow::NodeKind::Group) {
                bool bypassed = false;
                if (kind == flow::NodeKind::Effect) {
                    size_t li = 0, fi = 0;
                    if (find_effect_by_id(app.document, doc_id, &li, &fi))
                        bypassed = app.document.layers[li]
                                       .stack[fi].bypass;
                } else {
                    for (const doc::Layer& gl : app.document.layers)
                        for (const doc::Group& gr : gl.groups)
                            if (gr.id == doc_id) bypassed = gr.bypass;
                }
                push(bypassed ? "enable (b)" : "bypass (b)",
                     CtxAction::Bypass);
            }
            if (kind == flow::NodeKind::Effect ||
                kind == flow::NodeKind::ModSource ||
                kind == flow::NodeKind::Mask)
                push("duplicate (ctrl+d)", CtxAction::Duplicate);
            if (kind == flow::NodeKind::Effect)
                push("group selection (ctrl+g)", CtxAction::Group);
            if (app.multi_sel.size() >= 2) {
                push("align left", CtxAction::AlignLeft);
                push("align top", CtxAction::AlignTop);
            }
            if (app.multi_sel.size() >= 3) {
                push("spread h", CtxAction::SpreadH);
                push("spread v", CtxAction::SpreadV);
            }
            push("delete (del)", CtxAction::Delete);
        }
        const size_t cap = std::max<size_t>(items.size(), 1);
        const char** item_arr = arena.alloc<const char*>(cap);
        CtxAction* action_arr = arena.alloc<CtxAction>(cap);
        std::copy(items.begin(), items.end(), item_arr);
        std::copy(actions.begin(), actions.end(), action_arr);
        graph->ctx_items = item_arr;
        graph->ctx_count = items.size();
        ctx_actions = action_arr;
    }
    auto* events = arena.alloc<flow::Output>();
    // Arena allocation memsets to zero — it does NOT run default member
    // initializers. Restore every nonzero sentinel by hand or a zeroed
    // field reads as a live event (add_pick 0 = "picked item 0" spawned
    // an RGB Split every frame).
    events->add_pick = -1;
    events->route_drop_row = -1;
    events->ctx_pick = -1;
    return {graph, events, add_actions, ctx_actions};
}

// Two side panels from one pass (spec §9): LEFT = project, layers, masks,
// presets. RIGHT = the selected layer's stack + modulation — always visible
// beside the viewport, so picking a layer and editing its stack never
// scrolls. Blocks that belong to the right panel shadow `rows` with
// `right_rows` so their internals stay identical.
void build_side_panels(ui::LayoutArena& arena, AppState& app, FrameUi& out,
                       float fps, ui::LayoutNode** left_out,
                       ui::LayoutNode** right_out,
                       ui::LayoutNode** preset_out) {
    using namespace ui;
    LabelOpts dim;
    dim.color = active_theme().text_dim;
    LabelOpts small_dim = dim;
    small_dim.size = active_theme().font_size_small;

    std::vector<LayoutNode*> rows;
    std::vector<LayoutNode*> right_rows;
    // Title row: the app name with the open action beside it.
    {
        out.open_clicked = arena.alloc<bool>();
        ButtonOpts open_opts;
        open_opts.width = SizeSpec::fixed(90);
        open_opts.tooltip = "open an mp4/mov clip";
        StackOpts hdr;
        hdr.gap = kSpaceTight;
        hdr.cross_align = AlignMode::Center;
        std::vector<LayoutNode*> hdr_cells{
            Heading(arena, "looks"), Spacer(arena),
            Button(arena, "open clip...", &app.open_button, out.open_clicked,
                   open_opts)};
        LayoutNode* hdr_stack = VStackDyn(arena, hdr, hdr_cells);
        hdr_stack->kind = NodeKind::HStack;
        rows.push_back(hdr_stack);
    }

    // ---- clip status
    const bool has_clip = app.player.is_open();
    char line[96];
    if (app.import) {
        const uint32_t total = app.import->progress.frames_total.load();
        const uint32_t done = app.import->progress.frames_done.load();
        std::snprintf(line, sizeof(line), "importing %u%%",
                      total ? done * 100 / total : 0);
        rows.push_back(Label(arena, line, dim));
    } else if (has_clip) {
        rows.push_back(Label(arena, app.clip_name.c_str(), small_dim));
    }

    if (has_clip) {
        const bool is_still = is_still_source(app.document.clip_path);
        // Still clips: a duration field replaces the time/audio rows —
        // speed/direction/sidechain/nudge do nothing when every frame is
        // identical and there is no clip audio.
        if (is_still) {
            out.duration_clicked = arena.alloc<bool>();
            std::string label;
            if (app.duration_focus) {
                label = app.duration_edit + "_";
            } else {
                char buf[32];
                const double secs = app.player.fps() > 0.0
                    ? app.player.frame_count() / app.player.fps()
                    : 0.0;
                std::snprintf(buf, sizeof(buf), "%.1f s", secs);
                label = buf;
            }
            ButtonOpts dur_opts;
            dur_opts.width = SizeSpec::fill();
            dur_opts.flat = true;
            dur_opts.align_left = true;
            dur_opts.tooltip = "still length in seconds: click, type, enter";
            rows.push_back(value_row(
                arena, "duration",
                Button(arena, arena.dup(label.c_str(), label.size()),
                       &app.duration_btn, out.duration_clicked, dur_opts)));
        }
        // Time remap (spec §6.1) — a DOCUMENT parameter, so it lives here
        // rather than in the transport bar. "~" routes a mod source onto
        // speed, "k" drops a keyframe (lanes make it a true speed ramp).
        if (!is_still) {
        out.speed_staged = arena.alloc<float>();
        *out.speed_staged = app.document.speed;
        out.speed_changed = arena.alloc<bool>();
        SliderOpts spd_opts;
        spd_opts.format = "%.2fx";
        spd_opts.out_changed = out.speed_changed;
        FrameUi::AddRoute speed_route{{0, 1}, arena.alloc<bool>()};
        FrameUi::KeyToggle speed_key{{0, 1}, app.document.speed,
                                     arena.alloc<bool>()};
        rows.push_back(param_row(
            arena, "speed",
            SliderF(arena, out.speed_staged, 0.0f, doc::kMaxSpeed,
                    &app.speed_slider, spd_opts),
            &app.speed_route_button, speed_route.clicked,
            &app.speed_key_button, speed_key.clicked));
        out.add_routes.push_back(speed_route);
        out.key_toggles.push_back(speed_key);
        out.time_mode_selected = arena.alloc<int>();
        *out.time_mode_selected = -1;
        static const char* kTimeModes[] = {"forward", "reverse", "ping-pong"};
        rows.push_back(value_row(
            arena, "time",
            Dropdown(arena, kTimeModes, 3,
                     static_cast<int>(app.document.time_mode),
                     &app.time_mode_dd, out.time_mode_selected,
                     SizeSpec::fill(), "playback direction")));
        // Sidechain + audio nudge (spec §7).
        {
            out.sc_selected = arena.alloc<int>();
            *out.sc_selected = -1;
            const bool has_sc = !app.document.sidechain_path.empty();
            const char** items = arena.alloc<const char*>(3);
            int n = 0;
            int current = 0;
            items[n++] = "clip audio";
            if (has_sc) {
                const std::string file =
                    std::filesystem::path(app.document.sidechain_path)
                        .filename()
                        .string();
                items[n] = arena.dup(file.c_str(), file.size());
                current = n;
                ++n;
            }
            items[n++] = "pick audio...";
            rows.push_back(value_row(
                arena, "sidechain",
                Dropdown(arena, items, n, current, &app.sc_dd,
                         out.sc_selected, SizeSpec::fill(),
                         "audio-reactive source: clip or external wav/mp4")));
            if (has_sc) {
                out.sc_mux_changed = arena.alloc<bool>();
                out.sc_mux_staged = arena.alloc<bool>();
                *out.sc_mux_staged = app.document.sidechain_mux;
                rows.push_back(Checkbox(arena, "export sidechain audio",
                                        out.sc_mux_staged, &app.sc_mux_check,
                                        out.sc_mux_changed));
            }
            out.nudge_staged = arena.alloc<float>();
            *out.nudge_staged = app.document.audio_offset_ms;
            out.nudge_changed = arena.alloc<bool>();
            out.nudge_released = arena.alloc<bool>();
            SliderOpts nopts;
            nopts.format = "%+.0f ms";
            nopts.out_changed = out.nudge_changed;
            nopts.out_released = out.nudge_released;
            rows.push_back(value_row(
                arena, "a. nudge",
                SliderF(arena, out.nudge_staged, -1000.0f, 1000.0f,
                        &app.nudge_slider, nopts)));
        }
        }   // !is_still
        // Half-res proxy toggle (spec §3/§10), shown when the import
        // produced one.
        {
            std::filesystem::path proxy = app.mez_path;
            proxy.replace_extension(".proxy.mez");
            std::error_code pec;
            if (std::filesystem::exists(proxy, pec)) {
                out.proxy_toggle_changed = arena.alloc<bool>();
                out.proxy_toggle_staged = arena.alloc<bool>();
                *out.proxy_toggle_staged = app.document.use_proxy;
                rows.push_back(Checkbox(arena, "half-res proxy",
                                        out.proxy_toggle_staged,
                                        &app.proxy_check,
                                        out.proxy_toggle_changed));
            }
        }
    } else if (!app.import) {
        rows.push_back(Label(arena, "test pattern", small_dim));
    }
    // Lossless import (spec §3): applies to the next import.
    {
        out.lossless_changed = arena.alloc<bool>();
        out.lossless_staged = arena.alloc<bool>();
        *out.lossless_staged = app.import_lossless;
        rows.push_back(Checkbox(arena, "lossless import",
                                out.lossless_staged, &app.lossless_check,
                                out.lossless_changed));
    }
    // ---- project (spec §10): save / open, dirty star, Ctrl+S / Ctrl+O.
    {
        // Project name (status) + its two actions on ONE row.
        out.save_clicked = arena.alloc<bool>();
        out.open_project_clicked = arena.alloc<bool>();
        const bool dirty = app.document.revision != app.saved_revision;
        std::string proj = app.project_path.empty()
            ? std::string("untitled")
            : app.project_path.filename().string();
        if (dirty) proj += " *";
        ButtonOpts small_act;
        small_act.width = SizeSpec::fixed(56);
        StackOpts prow_opts;
        prow_opts.gap = 4.0f;
        prow_opts.cross_align = AlignMode::Center;
        std::vector<LayoutNode*> prow{
            Label(arena, proj.c_str(), small_dim), Spacer(arena),
            Button(arena, "save", &app.save_button, out.save_clicked,
                   small_act),
            Button(arena, "proj...", &app.open_project_button,
                   out.open_project_clicked, small_act)};
        LayoutNode* pstack = VStackDyn(arena, prow_opts, prow);
        pstack->kind = NodeKind::HStack;
        rows.push_back(pstack);
    }
    if (!app.status.empty())
        rows.push_back(Label(arena, app.status.c_str(), small_dim));
    rows.push_back(Separator(arena));

    // ---- layer inspector (docs/flow_canvas.md): the selected layer's
    // props (canvas source node) or the add-layer source picker. Lives on
    // the right panel; the canvas lanes replaced the layer list.
    static const char* kBlendNames[] = {"normal", "add", "mult", "screen",
                                        "diff"};
    if (app.sel.kind == SelKind::LayerSource ||
        app.sel.kind == SelKind::AddLayer) {
    std::vector<LayoutNode*>& rows = right_rows;   // inspector content
    rows.push_back(Heading(arena, app.sel.kind == SelKind::AddLayer
                                      ? "new layer"
                                      : "layer"));
    if (app.sel.kind == SelKind::AddLayer) {
        // Masks and frames stay addable from here too — an empty graph
        // must offer every node kind (docs/flow_canvas.md v3).
        out.add_mask_clicked = arena.alloc<bool>();
        out.add_frame_clicked = arena.alloc<bool>();
        ButtonOpts half;
        half.width = SizeSpec::fill();
        rows.push_back(HStack(
            arena, {4.0f},
            {Button(arena, "+ mask", &app.add_mask_button,
                    out.add_mask_clicked, half),
             Button(arena, "+ frame", &app.add_frame_button,
                    out.add_frame_clicked, half)}));
    }
    for (size_t li = 0; li < app.document.layers.size(); ++li) {
        const doc::Layer& layer = app.document.layers[li];
        if (app.sel.kind != SelKind::LayerSource || layer.id != app.sel.id)
            continue;
        LayerUiState& ls = app.layer_ui[layer.id];
        FrameUi::LayerRow lrow{};
        lrow.index = li;
        lrow.id = layer.id;
        lrow.select = arena.alloc<bool>();
        lrow.visible_changed = arena.alloc<bool>();
        lrow.visible_staged = arena.alloc<bool>();
        *lrow.visible_staged = !layer.visible;   // eye click applies this
        lrow.blend_selected = arena.alloc<int>();
        *lrow.blend_selected = -1;
        lrow.mask_selected = arena.alloc<int>();
        *lrow.mask_selected = -1;
        lrow.remove = arena.alloc<bool>();
        lrow.up = arena.alloc<bool>();
        lrow.down = arena.alloc<bool>();

        const bool selected = li == app.selected_layer;
        const std::string& select_label = layer.name;
        std::vector<LayoutNode*> layer_rows_ui;
        ButtonOpts name_opts;
        name_opts.width = SizeSpec::fill();
        name_opts.flat = true;
        name_opts.align_left = true;
        name_opts.tooltip = "select layer (the stack panel edits it)";
        // v/^ swap with the neighbour: layers composite bottom-up, so
        // "down" in the list is later in the composite.
        ButtonOpts up_opts;
        up_opts.width = SizeSpec::fixed(20);
        up_opts.disabled = li == 0;
        up_opts.tooltip = "move layer up";
        ButtonOpts down_opts = up_opts;
        down_opts.disabled = li + 1 >= app.document.layers.size();
        down_opts.tooltip = "move layer down";
        ButtonOpts x_opts;
        x_opts.width = SizeSpec::fixed(20);
        x_opts.tooltip = "remove layer";
        ButtonOpts eye_opts;
        eye_opts.width = SizeSpec::fixed(20);
        eye_opts.tooltip = layer.visible ? "hide layer" : "show layer";
        layer_rows_ui.push_back(HStack(
            arena, {2.0f},
            {
                Button(arena, select_label.c_str(), &ls.select_button,
                       lrow.select, name_opts),
                IconButton(arena, layer.visible ? Icon::Eye : Icon::EyeOff,
                           &ls.visible_check, lrow.visible_changed, eye_opts),
                IconButton(arena, Icon::Up, &ls.up_button, lrow.up, up_opts),
                IconButton(arena, Icon::Down, &ls.down_button, lrow.down,
                           down_opts),
                IconButton(arena, Icon::Close, &ls.remove_button, lrow.remove,
                           x_opts),
            }));
        // Blend + layer mask on the value grid, dropdowns like every enum.
        layer_rows_ui.push_back(value_row(
            arena, "blend",
            Dropdown(arena, kBlendNames, 5,
                     static_cast<int>(layer.blend), &ls.blend_dd,
                     lrow.blend_selected, SizeSpec::fill(),
                     "blend mode over the composite below")));
        {
            const int mask_count = static_cast<int>(
                std::min<size_t>(app.document.masks.size(), 16));
            const char** items = arena.alloc<const char*>(
                static_cast<size_t>(mask_count) + 1);
            items[0] = "none";
            int current = 0;
            for (int m = 0; m < mask_count; ++m) {
                const doc::Mask& mk =
                    app.document.masks[static_cast<size_t>(m)];
                items[m + 1] = arena.dup(mk.name.c_str(), mk.name.size());
                if (mk.id == layer.mask_id) current = m + 1;
            }
            layer_rows_ui.push_back(value_row(
                arena, "mask",
                Dropdown(arena, items, mask_count + 1, current, &ls.mask_dd,
                         lrow.mask_selected, SizeSpec::fill(),
                         "gate this layer's contribution with a mask")));
        }

        int lslider = 0;
        auto layer_slider = [&](FrameUi::LayerField field, const char* label,
                                float min_v, float max_v, float value,
                                const char* format) {
            if (lslider >= 9) return;
            FrameUi::LayerStage stage{};
            stage.layer_id = layer.id;
            stage.field = field;
            stage.staged = arena.alloc<float>();
            *stage.staged = value;
            stage.original = value;
            stage.changed = arena.alloc<bool>();
            stage.released = arena.alloc<bool>();
            SliderOpts opts;
            opts.format = format;
            opts.out_changed = stage.changed;
            opts.out_released = stage.released;
            layer_rows_ui.push_back(param_row(
                arena, label,
                SliderF(arena, stage.staged, min_v, max_v,
                        &ls.sliders[lslider], opts),
                nullptr, nullptr, nullptr, nullptr));
            out.layer_stages.push_back(stage);
            ++lslider;
        };
        using LF = FrameUi::LayerField;
        layer_slider(LF::Opacity, "opacity", 0.0f, 1.0f, layer.opacity,
                     "%.2f");
        if (layer.source == doc::LayerSourceKind::Solid ||
            layer.source == doc::LayerSourceKind::Gradient ||
            layer.source == doc::LayerSourceKind::Noise ||
            layer.source == doc::LayerSourceKind::Oscillator) {
            layer_slider(LF::ColorAR, "color a r", 0.0f, 1.0f,
                         layer.color_a[0], "%.2f");
            layer_slider(LF::ColorAG, "color a g", 0.0f, 1.0f,
                         layer.color_a[1], "%.2f");
            layer_slider(LF::ColorAB, "color a b", 0.0f, 1.0f,
                         layer.color_a[2], "%.2f");
        }
        if (layer.source == doc::LayerSourceKind::Gradient ||
            layer.source == doc::LayerSourceKind::Noise ||
            layer.source == doc::LayerSourceKind::Oscillator) {
            layer_slider(LF::ColorBR, "color b r", 0.0f, 1.0f,
                         layer.color_b[0], "%.2f");
            layer_slider(LF::ColorBG, "color b g", 0.0f, 1.0f,
                         layer.color_b[1], "%.2f");
            layer_slider(LF::ColorBB, "color b b", 0.0f, 1.0f,
                         layer.color_b[2], "%.2f");
        }
        if (layer.source == doc::LayerSourceKind::Gradient ||
            layer.source == doc::LayerSourceKind::Oscillator)
            layer_slider(LF::Angle, "angle", -3.1416f, 3.1416f,
                         layer.gen_angle, "%.2f");
        if (layer.source == doc::LayerSourceKind::Noise)
            layer_slider(LF::Scale, "scale", 2.0f, 128.0f, layer.gen_scale,
                         "%.0f px");
        if (layer.source == doc::LayerSourceKind::Oscillator) {
            layer_slider(LF::Scale, "frequency", 0.5f, 32.0f,
                         layer.gen_scale, "%.1f cyc");
            // Waveform dropdown (spec §5 generators): the oscillator is a
            // patchable periodic source, shape picks its geometry.
            static const char* kOscShapes[] = {"sine bars", "rings",
                                               "plasma", "lissajous"};
            lrow.osc_shape_selected = arena.alloc<int>();
            *lrow.osc_shape_selected = -1;
            layer_rows_ui.push_back(value_row(
                arena, "wave",
                Dropdown(arena, kOscShapes, 4,
                         static_cast<int>(layer.osc_shape), &ls.osc_dd,
                         lrow.osc_shape_selected, SizeSpec::fill(),
                         "oscillator waveform")));
        }
        if (layer.source == doc::LayerSourceKind::Shape) {
            // The matte maker (v5.2): size + feather + geometry; place
            // and rotate it with the layer transform, texture it with
            // the effect chain it feeds.
            layer_slider(LF::Scale, "size", 0.5f, 30.0f, layer.gen_scale,
                         "%.1f");
            layer_slider(LF::Angle, "feather", 0.0f, 3.1416f,
                         layer.gen_angle, "%.2f");
            static const char* kShapeKinds[] = {"circle", "box",
                                                "diamond"};
            lrow.osc_shape_selected = arena.alloc<int>();
            *lrow.osc_shape_selected = -1;
            layer_rows_ui.push_back(value_row(
                arena, "shape",
                Dropdown(arena, kShapeKinds, 3,
                         static_cast<int>(layer.osc_shape) % 3, &ls.osc_dd,
                         lrow.osc_shape_selected, SizeSpec::fill(),
                         "matte geometry")));
        }

        // Transform + trim (spec §5: crop/flip/scale/rotate + the clip
        // segment live on the layer). Folded per layer; the header marks
        // itself when the transform is active so a folded card still tells.
        lrow.xf_toggle = arena.alloc<bool>();
        layer_rows_ui.push_back(SectionHeader(
            arena,
            doc::layer_has_transform(layer) || doc::layer_has_trim(layer)
                ? "transform *"
                : "transform",
            ls.xf_open, &ls.xf_header, lrow.xf_toggle, /*small=*/true));
        if (ls.xf_open) {
            int xslider = 0;
            auto xf_slider = [&](FrameUi::LayerField field, const char* label,
                                 float min_v, float max_v, float value,
                                 const char* format) {
                if (xslider >= 8) return;
                FrameUi::LayerStage stage{};
                stage.layer_id = layer.id;
                stage.field = field;
                stage.staged = arena.alloc<float>();
                *stage.staged = value;
                stage.original = value;
                stage.changed = arena.alloc<bool>();
                stage.released = arena.alloc<bool>();
                SliderOpts opts;
                opts.format = format;
                opts.out_changed = stage.changed;
                opts.out_released = stage.released;
                layer_rows_ui.push_back(param_row(
                    arena, label,
                    SliderF(arena, stage.staged, min_v, max_v,
                            &ls.xf_sliders[xslider], opts),
                    nullptr, nullptr, nullptr, nullptr));
                out.layer_stages.push_back(stage);
                ++xslider;
            };
            xf_slider(LF::CropL, "crop l", 0.0f, 0.45f, layer.crop_l,
                      "%.2f");
            xf_slider(LF::CropR, "crop r", 0.0f, 0.45f, layer.crop_r,
                      "%.2f");
            xf_slider(LF::CropT, "crop t", 0.0f, 0.45f, layer.crop_t,
                      "%.2f");
            xf_slider(LF::CropB, "crop b", 0.0f, 0.45f, layer.crop_b,
                      "%.2f");
            xf_slider(LF::XfScale, "scale", 0.25f, 4.0f, layer.xf_scale,
                      "%.2f x");
            xf_slider(LF::Rotate, "rotate", -180.0f, 180.0f,
                      layer.xf_rotate, "%.0f deg");
            lrow.flip_h = arena.alloc<bool>();
            lrow.flip_v = arena.alloc<bool>();
            layer_rows_ui.push_back(value_row(
                arena, "flip",
                HStack(arena, {6.0f},
                       {Chip(arena, "horizontal", layer.flip_h,
                             &ls.flip_h_btn, lrow.flip_h, "mirror left-right"),
                        Chip(arena, "vertical", layer.flip_v, &ls.flip_v_btn,
                             lrow.flip_v, "mirror top-bottom")})));
            if (layer.source == doc::LayerSourceKind::Clip &&
                app.player.is_open() && app.player.frame_count() > 1) {
                const float fmax = static_cast<float>(
                    app.player.frame_count());
                xf_slider(LF::TrimIn, "trim in", 0.0f, fmax - 1.0f,
                          static_cast<float>(layer.trim_in), "%.0f f");
                xf_slider(LF::TrimOut, "trim out", 0.0f, fmax,
                          static_cast<float>(layer.trim_out), "%.0f f");
            }
        }

        StackOpts layer_col;
        layer_col.gap = 4.0f;
        layer_col.cross_align = AlignMode::Stretch;
        // Selection reads from the accent edge, not a text prefix.
        rows.push_back(Panel(arena, VStackDyn(arena, layer_col, layer_rows_ui),
                             PanelOpts{Edges::all(6), -1.0f,
                                       /*outline=*/false,
                                       /*accent_edge=*/selected}));
        out.layer_rows.push_back(lrow);
    }
    if (app.sel.kind == SelKind::AddLayer &&
        app.document.layers.size() < doc::kMaxLayers) {
        // Paired rows: full labels never fit one 300 px row. clip = a
        // tap off THE source clip (the adjustment type is gone — a clip
        // tap wired back through a Blend IS an adjustment).
        static const char* kAddLayer[] = {"+ solid",   "+ gradient",
                                          "+ noise",   "+ pattern",
                                          "+ osc",     "+ shape",
                                          "+ clip"};
        for (int t = 0; t < 7; ++t)
            out.add_layer_clicked[t] = arena.alloc<bool>();
        ButtonOpts half;
        half.width = SizeSpec::fill();
        for (int r = 0; r < 3; ++r) {
            LayoutNode* pair = HStack(
                arena, {4.0f},
                {Button(arena, kAddLayer[r * 2],
                        &app.add_layer_buttons[r * 2],
                        out.add_layer_clicked[r * 2], half),
                 Button(arena, kAddLayer[r * 2 + 1],
                        &app.add_layer_buttons[r * 2 + 1],
                        out.add_layer_clicked[r * 2 + 1], half)});
            rows.push_back(pair);
        }
        rows.push_back(Button(arena, kAddLayer[6],
                              &app.add_layer_buttons[6],
                              out.add_layer_clicked[6], half));
    }
    rows.push_back(Separator(arena));
    }   // end layer inspector

    // ---- RIGHT PANEL: stack + inspector. Group headers render above
    // their first member; folded groups hide the member panels. The header
    // names the layer the stack belongs to — the panel edits the SELECTED
    // layer.
    {
    std::vector<LayoutNode*>& rows = right_rows;   // right panel from here
    // Inspector title (docs/flow_canvas.md): the selection names the
    // content; the flow canvas below is the selector.
    const bool stack_sel = app.sel.kind == SelKind::Effect ||
                           app.sel.kind == SelKind::Group ||
                           app.sel.kind == SelKind::AddEffect ||
                           app.sel.kind == SelKind::None;
    if (stack_sel) {
        const char* insp_title =
            app.sel.kind == SelKind::Effect        ? "effect"
            : app.sel.kind == SelKind::Group       ? "group"
            : app.sel.kind == SelKind::AddEffect   ? "add effect"
                                                   : "inspector";
        rows.push_back(Heading(arena, insp_title));
    }
    if (app.sel.kind == SelKind::None) {
        rows.push_back(Label(arena,
                             "select a node below - double-click the "
                             "canvas to add",
                             small_dim));
        out.open_add_clicked = arena.alloc<bool>();
        rows.push_back(Button(arena, "+ add node...", &app.open_add_button,
                              out.open_add_clicked));
    }
    if ((app.sel.kind == SelKind::None || app.sel.kind == SelKind::Effect ||
         app.sel.kind == SelKind::Group) &&
        !app.document.layers.empty()) {
        // Randomize (spec §10): chaos slider + whole-stack button; each
        // effect row also carries its own dice.
        out.randomize_all = arena.alloc<bool>();
        out.chaos_staged = arena.alloc<float>();
        *out.chaos_staged = app.chaos;
        out.chaos_changed = arena.alloc<bool>();
        SliderOpts chaos_opts;
        chaos_opts.format = "%.2f";
        chaos_opts.out_changed = out.chaos_changed;
        ButtonOpts rnd_opts;
        rnd_opts.width = SizeSpec::fixed(110);
        std::vector<LayoutNode*> rnd_row{
            Button(arena, "randomize", &app.randomize_all_button,
                   out.randomize_all, rnd_opts),
            SliderF(arena, out.chaos_staged, 0.0f, 1.0f, &app.chaos_slider,
                    chaos_opts)};
        LayoutNode* rnd_stack = VStackDyn(arena, {}, rnd_row);
        rnd_stack->kind = NodeKind::HStack;
        rnd_stack->gap = 6.0f;
        rows.push_back(rnd_stack);
    }
    if (app.sel.kind == SelKind::Effect) {
        size_t li = 0, fi = 0;
        if (find_effect_by_id(app.document, app.sel.id, &li, &fi)) {
            app.selected_layer = li;   // stack staging keys on this layer
            rows.push_back(Label(arena,
                                 app.document.layers[li].name.c_str(),
                                 small_dim));
            rows.push_back(build_effect_panel(arena, app, out, fi));
            // A grouped effect brings its group's face along — groups
            // have no card of their own on the node canvas.
            const doc::Layer& sel_layer = app.document.layers[li];
            const uint64_t gid = sel_layer.stack[fi].group_id;
            if (gid) {
                const doc::Group* group = nullptr;
                for (const doc::Group& g : sel_layer.groups)
                    if (g.id == gid) group = &g;
                std::vector<size_t> members;
                for (size_t j = 0; j < sel_layer.stack.size(); ++j)
                    if (sel_layer.stack[j].group_id == gid)
                        members.push_back(j);
                if (group)
                    rows.push_back(build_group_panel(arena, app, out,
                                                     *group, members,
                                                     /*inspector=*/true));
            }
        }
    } else if (app.sel.kind == SelKind::Group) {
        size_t li = 0;
        if (find_group_by_id(app.document, app.sel.id, &li)) {
            app.selected_layer = li;
            const doc::Layer& sel_layer = app.document.layers[li];
            const doc::Group* group = nullptr;
            for (const doc::Group& g : sel_layer.groups)
                if (g.id == app.sel.id) group = &g;
            std::vector<size_t> members;
            for (size_t j = 0; j < sel_layer.stack.size(); ++j)
                if (sel_layer.stack[j].group_id == app.sel.id)
                    members.push_back(j);
            if (group)
                rows.push_back(build_group_panel(arena, app, out, *group,
                                                 members,
                                                 /*inspector=*/true));
        }
    }

    // Add-node browser: spec §6.1 category folds, shown after a
    // double-click on the canvas (or an explicit add request). Masks and
    // layers add from the same place.
    if (app.sel.kind == SelKind::AddEffect &&
        layer_index_by_id(app.document, app.sel.id) >= 0) {
        app.selected_layer = static_cast<size_t>(
            layer_index_by_id(app.document, app.sel.id));
        rows.push_back(Label(arena,
                             app.insert_before_id != 0
                                 ? "into the clicked slot"
                                 : "at the end of the chain",
                             small_dim));
        {
            out.add_mask_clicked = arena.alloc<bool>();
            out.add_layer_open = arena.alloc<bool>();
            ButtonOpts half;
            half.width = SizeSpec::fill();
            out.add_frame_clicked = arena.alloc<bool>();
            rows.push_back(HStack(
                arena, {4.0f},
                {Button(arena, "+ mask", &app.add_mask_button,
                        out.add_mask_clicked, half),
                 Button(arena, "+ layer...", &app.add_layer_open_button,
                        out.add_layer_open, half),
                 Button(arena, "+ frame", &app.add_frame_button,
                        out.add_frame_clicked, half)}));
            // Search (v3): type-to-filter across every category.
            out.fx_search_clicked = arena.alloc<bool>();
            std::string label = app.fx_filter;
            if (app.fx_search_focus) label += "_";
            else if (label.empty()) label = "search...";
            ButtonOpts search_opts;
            search_opts.width = SizeSpec::fill();
            search_opts.flat = true;
            search_opts.align_left = true;
            search_opts.tooltip = "type to filter effects by name";
            rows.push_back(value_row(
                arena, "find",
                Button(arena, arena.dup(label.c_str(), label.size()),
                       &app.fx_search_btn, out.fx_search_clicked,
                       search_opts)));
        }
        if (!app.fx_filter.empty()) {
            // Flat filtered list, two per row, categories ignored.
            auto lower = [](std::string s) {
                for (char& c : s)
                    c = static_cast<char>(
                        std::tolower(static_cast<unsigned char>(c)));
                return s;
            };
            const std::string needle = lower(app.fx_filter);
            std::vector<LayoutNode*> browser;
            ButtonOpts half;
            half.width = SizeSpec::fill();
            LayoutNode* pending_btn = nullptr;
            for (size_t t = 0;
                 t < static_cast<size_t>(doc::EffectType::Count); ++t) {
                const doc::EffectInfo& info =
                    doc::effect_info(static_cast<doc::EffectType>(t));
                if (lower(info.label).find(needle) == std::string::npos)
                    continue;
                out.add_clicked[t] = arena.alloc<bool>();
                LayoutNode* b =
                    Button(arena, info.label, &app.add_buttons[t],
                           out.add_clicked[t], half);
                if (!pending_btn) {
                    pending_btn = b;
                } else {
                    browser.push_back(HStack(arena, {4.0f},
                                             {pending_btn, b}));
                    pending_btn = nullptr;
                }
            }
            if (pending_btn)
                browser.push_back(
                    HStack(arena, {4.0f}, {pending_btn, Spacer(arena)}));
            if (browser.empty())
                browser.push_back(
                    Label(arena, "no effects match", small_dim));
            StackOpts browser_col;
            browser_col.gap = 4.0f;
            browser_col.cross_align = AlignMode::Stretch;
            rows.push_back(Panel(arena,
                                 VStackDyn(arena, browser_col, browser),
                                 PanelOpts{Edges::all(8), -1.0f,
                                           /*outline=*/true}));
        } else {
            // The open browser reads as one contained dropdown panel —
            // an outlined body under the header, not loose rows drifting
            // in the stack list (same containment treatment as groups).
            std::vector<LayoutNode*> browser;
            for (int c = 0; c < 8; ++c) {
                const auto cat = static_cast<doc::FxCategory>(c);
                out.fx_cat_clicked[c] = arena.alloc<bool>();
                browser.push_back(SectionHeader(arena,
                                                doc::fx_category_label(cat),
                                                app.fx_cat_open[c],
                                                &app.fx_cat_buttons[c],
                                                out.fx_cat_clicked[c],
                                                /*small=*/true));
                if (!app.fx_cat_open[c]) continue;
                ButtonOpts half;
                half.width = SizeSpec::fill();
                LayoutNode* pending = nullptr;
                for (size_t t = 0;
                     t < static_cast<size_t>(doc::EffectType::Count); ++t) {
                    const doc::EffectInfo& info =
                        doc::effect_info(static_cast<doc::EffectType>(t));
                    if (info.category != cat) continue;
                    out.add_clicked[t] = arena.alloc<bool>();
                    LayoutNode* b =
                        Button(arena, info.label, &app.add_buttons[t],
                               out.add_clicked[t], half);
                    if (!pending) {
                        pending = b;
                    } else {
                        browser.push_back(
                            HStack(arena, {4.0f}, {pending, b}));
                        pending = nullptr;
                    }
                }
                if (pending)
                    browser.push_back(HStack(arena, {4.0f},
                                             {pending, Spacer(arena)}));
            }
            StackOpts browser_col;
            browser_col.gap = 4.0f;
            browser_col.cross_align = AlignMode::Stretch;
            rows.push_back(Panel(arena,
                                 VStackDyn(arena, browser_col, browser),
                                 PanelOpts{Edges::all(8), -1.0f,
                                           /*outline=*/true}));
        }
    }
    rows.push_back(Separator(arena));
    }   // end right panel (inspector)

    // ---- preset browser (spec §10): click = drop the group onto the
    // selected layer's stack. Its OWN inspector tab (it was buried
    // behind a fold in the project tab — "where are the presets?").
    std::vector<LayoutNode*> preset_rows;
    {
        std::vector<LayoutNode*>& rows = preset_rows;
        rows.push_back(Heading(arena, "presets"));
        std::vector<std::string> tags;
        for (const doc::Preset& p : app.presets)
            for (const std::string& t : p.tags)
                if (std::find(tags.begin(), tags.end(), t) == tags.end())
                    tags.push_back(t);
        std::sort(tags.begin(), tags.end());
        if (app.preset_tag_index >= static_cast<int>(tags.size()))
            app.preset_tag_index = -1;
        out.tag_selected = arena.alloc<int>();
        *out.tag_selected = -1;
        const int tag_count =
            static_cast<int>(std::min<size_t>(tags.size(), 16));
        const char** tag_items =
            arena.alloc<const char*>(static_cast<size_t>(tag_count) + 1);
        tag_items[0] = "all";
        for (int t = 0; t < tag_count; ++t)
            tag_items[t + 1] = arena.dup(tags[static_cast<size_t>(t)].c_str(),
                                         tags[static_cast<size_t>(t)].size());
        rows.push_back(value_row(
            arena, "tag",
            Dropdown(arena, tag_items, tag_count + 1,
                     app.preset_tag_index + 1, &app.tag_dd, out.tag_selected,
                     SizeSpec::fill(), "filter presets by tag")));
        // Text search (spec §9: browser is searchable). A flat field that
        // captures the keyboard while focused; enter/escape release it.
        {
            out.preset_search_clicked = arena.alloc<bool>();
            std::string label = app.preset_filter;
            if (app.preset_search_focus) label += "_";
            else if (label.empty()) label = "search...";
            ButtonOpts search_opts;
            search_opts.width = SizeSpec::fill();
            search_opts.flat = true;
            search_opts.align_left = true;
            search_opts.tooltip = "type to filter presets by name";
            rows.push_back(value_row(
                arena, "find",
                Button(arena, arena.dup(label.c_str(), label.size()),
                       &app.preset_search_btn, out.preset_search_clicked,
                       search_opts)));
        }
        auto matches_filter = [&](const doc::Preset& p) {
            if (app.preset_filter.empty()) return true;
            auto lower = [](std::string s) {
                for (char& c : s)
                    c = static_cast<char>(std::tolower(
                        static_cast<unsigned char>(c)));
                return s;
            };
            return lower(p.name).find(lower(app.preset_filter)) !=
                   std::string::npos;
        };
        for (size_t pi = 0; pi < app.presets.size(); ++pi) {
            const doc::Preset& p = app.presets[pi];
            if (app.preset_tag_index >= 0 &&
                std::find(p.tags.begin(), p.tags.end(),
                          tags[static_cast<size_t>(app.preset_tag_index)]) ==
                    p.tags.end())
                continue;
            if (!matches_filter(p)) continue;
            bool* clicked = arena.alloc<bool>();
            LayoutNode* pb = Button(arena, p.name.c_str(),
                                    &app.preset_buttons[p.path.string()],
                                    clicked);
            rows.push_back(pb);
            out.preset_clicks.push_back({pi, clicked});
            out.preset_nodes.push_back({pi, pb});
        }
        if (app.presets.empty())
            rows.push_back(Label(arena, "no presets found", small_dim));
        // Single-file preset import (spec §9); drag-and-drop works too.
        out.preset_import_clicked = arena.alloc<bool>();
        rows.push_back(Button(arena, "import preset...",
                              &app.preset_import_btn,
                              out.preset_import_clicked));
    }

    // ---- mask inspector (spec §8, docs/flow_canvas.md): the selected
    // canvas mask node's full block — right panel; the canvas aux lane
    // replaced the mask list.
    if (app.sel.kind == SelKind::Mask) {
    std::vector<LayoutNode*>& rows = right_rows;   // inspector content
    rows.push_back(Heading(arena, "mask"));
    static const char* kMaskTypeNames[] = {"shape", "luma", "luma key",
                                           "chroma key", "motion"};
    static const char* kExtractNames[] = {"luma", "red", "green", "blue",
                                          "alpha"};
    for (const doc::Mask& mask : app.document.masks) {
        if (mask.id != app.sel.id) continue;
        MaskUiState& ms = app.mask_ui[mask.id];
        FrameUi::MaskActions actions{};
        actions.mask_id = mask.id;
        actions.type_selected = arena.alloc<int>();
        *actions.type_selected = -1;
        actions.extract_selected = arena.alloc<int>();
        *actions.extract_selected = -1;
        actions.remove = arena.alloc<bool>();
        actions.view = arena.alloc<bool>();
        actions.invert_changed = arena.alloc<bool>();
        actions.invert_staged = arena.alloc<bool>();
        *actions.invert_staged = mask.invert;
        actions.chain_add = arena.alloc<bool>();

        std::vector<LayoutNode*> mask_rows;
        ButtonOpts tiny;
        tiny.width = SizeSpec::fixed(20);
        tiny.tooltip = "remove mask";
        const bool viewing = app.overlay_mask_id == mask.id;
        ButtonOpts view_opts;
        view_opts.width = SizeSpec::fixed(20);
        view_opts.tooltip = viewing ? "hide mask overlay"
                                    : "view mask in the viewport";
        mask_rows.push_back(HStack(
            arena, {2.0f},
            {
                Label(arena, mask.name.c_str()),
                Spacer(arena),
                IconButton(arena, viewing ? Icon::EyeOff : Icon::Eye,
                           &ms.view_button, actions.view, view_opts),
                IconButton(arena, Icon::Close, &ms.remove_button,
                           actions.remove, tiny),
            }));
        mask_rows.push_back(value_row(
            arena, "type",
            Dropdown(arena, kMaskTypeNames, 5,
                     static_cast<int>(mask.type), &ms.type_dd,
                     actions.type_selected, SizeSpec::fill(), nullptr)));
        if (mask.type == doc::MaskType::Luma)
            mask_rows.push_back(value_row(
                arena, "extract",
                Dropdown(arena, kExtractNames, 5,
                         static_cast<int>(mask.extract), &ms.extract_dd,
                         actions.extract_selected, SizeSpec::fill(),
                         nullptr)));
        mask_rows.push_back(Checkbox(arena, "invert", actions.invert_staged,
                                     &ms.invert_check,
                                     actions.invert_changed));
        if (mask.type == doc::MaskType::Shape && mask.points.size() >= 6) {
            // Whole-shape keyframe at the playhead (spec §8 "keyframable
            // points"): keys every point coordinate; pressed again on a
            // keyed frame it removes the shape key (mirrors [k]).
            actions.key_points = arena.alloc<bool>();
            ButtonOpts kp;
            kp.width = SizeSpec::fixed(84);
            mask_rows.push_back(Button(arena, "key points",
                                       &ms.key_points_button,
                                       actions.key_points, kp));
        }

        int slider_i = 0;
        auto mask_slider = [&](FrameUi::MaskField field, const char* label,
                               float min_v, float max_v, float value,
                               const char* format) {
            if (slider_i >= 15) return;
            FrameUi::MaskStage stage{};
            stage.mask_id = mask.id;
            stage.field = field;
            stage.staged = arena.alloc<float>();
            *stage.staged = value;
            stage.original = value;
            stage.changed = arena.alloc<bool>();
            stage.released = arena.alloc<bool>();
            SliderOpts opts;
            opts.format = format;
            opts.out_changed = stage.changed;
            opts.out_released = stage.released;
            // Mask params are mod targets (spec §8): [~] route, [k] key,
            // keyed under kMaskParamBit. Fields outside the mod set
            // (roundness, key RGB) keep a plain label.
            int mod_index = -1;
            using MFF = FrameUi::MaskField;
            switch (field) {
                case MFF::Feather: mod_index = 0; break;
                case MFF::CenterX: mod_index = 1; break;
                case MFF::CenterY: mod_index = 2; break;
                case MFF::RadiusX: mod_index = 3; break;
                case MFF::RadiusY: mod_index = 4; break;
                case MFF::BlurPx: mod_index = 5; break;
                case MFF::BlackPoint: mod_index = 6; break;
                case MFF::WhitePoint: mod_index = 7; break;
                case MFF::Gamma: mod_index = 8; break;
                case MFF::KeyCenter: mod_index = 9; break;
                case MFF::KeyRange: mod_index = 10; break;
                default: break;
            }
            LayoutNode* slider = SliderF(arena, stage.staged, min_v, max_v,
                                         &ms.sliders[slider_i], opts);
            if (mod_index >= 0) {
                const doc::ParamKey mkey{mask.id | doc::kMaskParamBit,
                                         mod_index};
                FrameUi::AddRoute mroute{mkey, arena.alloc<bool>()};
                FrameUi::KeyToggle mkeyt{mkey, value, arena.alloc<bool>()};
                mask_rows.push_back(param_row(
                    arena, label, slider, &ms.route_buttons[slider_i],
                    mroute.clicked, &ms.key_buttons[slider_i],
                    mkeyt.clicked));
                out.add_routes.push_back(mroute);
                out.key_toggles.push_back(mkeyt);
            } else {
                mask_rows.push_back(param_row(arena, label, slider, nullptr,
                                              nullptr, nullptr, nullptr));
            }
            out.mask_stages.push_back(stage);
            ++slider_i;
        };
        using MF = FrameUi::MaskField;
        if (mask.type == doc::MaskType::Shape) {
            mask_slider(MF::CenterX, "center x", 0.0f, 1.0f, mask.center_x, "%.2f");
            mask_slider(MF::CenterY, "center y", 0.0f, 1.0f, mask.center_y, "%.2f");
            mask_slider(MF::RadiusX, "radius x", 0.01f, 1.0f, mask.radius_x, "%.2f");
            mask_slider(MF::RadiusY, "radius y", 0.01f, 1.0f, mask.radius_y, "%.2f");
            mask_slider(MF::Roundness, "roundness", 0.0f, 1.0f, mask.roundness, "%.2f");
            mask_slider(MF::Feather, "feather", 0.0f, 0.5f, mask.feather, "%.3f");
        } else {
            if (mask.type == doc::MaskType::LumaKey) {
                mask_slider(MF::KeyCenter, "key center", 0.0f, 1.0f,
                            mask.key_center, "%.2f");
                mask_slider(MF::KeyRange, "key range", 0.01f, 1.0f,
                            mask.key_range, "%.2f");
            } else if (mask.type == doc::MaskType::ChromaKey) {
                mask_slider(MF::KeyR, "key r", 0.0f, 1.0f, mask.key_r, "%.2f");
                mask_slider(MF::KeyG, "key g", 0.0f, 1.0f, mask.key_g, "%.2f");
                mask_slider(MF::KeyB, "key b", 0.0f, 1.0f, mask.key_b, "%.2f");
                mask_slider(MF::KeyRange, "key range", 0.01f, 2.0f,
                            mask.key_range, "%.2f");
            }
            // Mask source (spec §8: "source can be anything"): the shared
            // clip, another layer's source, a built-in generator, or an
            // external video + fit + time-sync. Motion masks read the flow
            // field and need no source.
            const bool motion = mask.type == doc::MaskType::Motion;
            const bool file_active = mask.source_gen == 0 &&
                                     mask.source_layer_id == 0 &&
                                     !mask.source_path.empty();
            if (!motion) {
                actions.source_selected = arena.alloc<int>();
                *actions.source_selected = -1;
                actions.freerun_changed = arena.alloc<bool>();
                actions.freerun_staged = arena.alloc<bool>();
                *actions.freerun_staged = mask.free_run;
                const char** src_items = arena.alloc<const char*>(
                    doc::kMaxLayers + 6);
                int n = 0;
                int current = 0;
                src_items[n++] = "clip";
                actions.src_layer_base = n;
                for (const doc::Layer& sl : app.document.layers) {
                    if (n >= static_cast<int>(doc::kMaxLayers) + 1) break;
                    src_items[n] = arena.dup(sl.name.c_str(),
                                             sl.name.size());
                    if (mask.source_gen == 0 &&
                        mask.source_layer_id == sl.id)
                        current = n;
                    ++n;
                }
                actions.src_layer_count = n - actions.src_layer_base;
                actions.src_gen_base = n;
                src_items[n++] = "gen: noise";
                src_items[n++] = "gen: gradient";
                src_items[n++] = "gen: bars";
                if (mask.source_gen ==
                    static_cast<uint32_t>(doc::LayerSourceKind::Noise))
                    current = actions.src_gen_base;
                else if (mask.source_gen ==
                         static_cast<uint32_t>(
                             doc::LayerSourceKind::Gradient))
                    current = actions.src_gen_base + 1;
                else if (mask.source_gen ==
                         static_cast<uint32_t>(
                             doc::LayerSourceKind::TestPattern))
                    current = actions.src_gen_base + 2;
                if (!mask.source_path.empty()) {
                    const std::string file =
                        std::filesystem::path(mask.source_path)
                            .filename()
                            .string();
                    actions.src_file_index = n;
                    src_items[n] = arena.dup(file.c_str(), file.size());
                    if (file_active) current = n;
                    ++n;
                }
                actions.src_pick_index = n;
                src_items[n++] = "pick video...";
                mask_rows.push_back(value_row(
                    arena, "source",
                    Dropdown(arena, src_items, n, current, &ms.source_dd,
                             actions.source_selected, SizeSpec::fill(),
                             "grayscale source for this mask")));
                if (mask.source_gen != 0) {
                    mask_slider(MF::GenScale, "gen scale", 2.0f, 256.0f,
                                mask.gen_scale, "%.0f px");
                    if (mask.source_gen ==
                        static_cast<uint32_t>(
                            doc::LayerSourceKind::Gradient))
                        mask_slider(MF::GenAngle, "gen angle", -3.1416f,
                                    3.1416f, mask.gen_angle, "%.2f");
                }
                if (file_active) {
                    static const char* kFitNames[] = {"stretch", "fill",
                                                      "tile"};
                    actions.fit_selected = arena.alloc<int>();
                    *actions.fit_selected = -1;
                    mask_rows.push_back(value_row(
                        arena, "fit",
                        Dropdown(arena, kFitNames, 3,
                                 static_cast<int>(mask.fit % 3), &ms.fit_dd,
                                 actions.fit_selected, SizeSpec::fill(),
                                 "resolution-mismatch handling")));
                    mask_rows.push_back(Checkbox(
                        arena, "free run", actions.freerun_staged,
                        &ms.freerun_check, actions.freerun_changed));
                }
            }

            mask_slider(MF::BlackPoint, "black", 0.0f, 1.0f,
                        mask.black_point, "%.2f");
            mask_slider(MF::WhitePoint, "white", 0.0f, 1.0f,
                        mask.white_point, "%.2f");
            mask_slider(MF::Gamma, "gamma", 0.1f, 4.0f, mask.gamma, "%.2f");
            mask_slider(MF::GrowPx, "grow", -64.0f, 64.0f, mask.grow_px,
                        "%.0f px");
            mask_slider(MF::BlurPx, "feather px", 0.0f, 64.0f, mask.blur_px,
                        "%.0f");

            // Mini effect chain (spec §8: mask sources own a chain).
            // Motion masks read the flow field — no chain.
            for (size_t c = 0; !motion && c < mask.chain.size() && c < 4;
                 ++c) {
                const doc::EffectInstance& cfx = mask.chain[c];
                const doc::EffectInfo& cinfo = doc::effect_info(cfx.type);
                FrameUi::MaskChainRemove remove{mask.id, c,
                                                arena.alloc<bool>()};
                ButtonOpts chain_x;
                chain_x.width = SizeSpec::fixed(20);
                chain_x.tooltip = "remove chain effect";
                mask_rows.push_back(HStack(
                    arena, {2.0f},
                    {
                        Label(arena, cinfo.label, small_dim),
                        Spacer(arena),
                        IconButton(arena, Icon::Close, &ms.chain_remove[c],
                                   remove.clicked, chain_x),
                    }));
                out.mask_chain_removes.push_back(remove);
                for (uint32_t p = 0; p < cinfo.param_count && p < 8; ++p) {
                    FrameUi::MaskChainStage stage{};
                    stage.mask_id = mask.id;
                    stage.chain_index = c;
                    stage.param_index = static_cast<int>(p);
                    stage.staged = arena.alloc<float>();
                    *stage.staged = cfx.params[p];
                    stage.original = cfx.params[p];
                    stage.changed = arena.alloc<bool>();
                    stage.released = arena.alloc<bool>();
                    SliderOpts copts;
                    copts.format = cinfo.params[p].format;
                    copts.out_changed = stage.changed;
                    copts.out_released = stage.released;
                    mask_rows.push_back(Label(arena, cinfo.params[p].label,
                                              small_dim));
                    mask_rows.push_back(
                        SliderF(arena, stage.staged,
                                cinfo.params[p].min_value,
                                cinfo.params[p].max_value,
                                &ms.chain_sliders[c][p], copts));
                    out.mask_chain_stages.push_back(stage);
                }
            }
            if (!motion && mask.chain.size() < 4)
                mask_rows.push_back(Button(arena, "+ chain pixelate",
                                           &ms.chain_add_button,
                                           actions.chain_add));
        }

        // Combine (spec §8): fold another mask in — add/subtract/intersect.
        if (app.document.masks.size() > 1) {
            actions.combine_selected = arena.alloc<int>();
            *actions.combine_selected = -1;
            const char** comb_items = arena.alloc<const char*>(17);
            int n = 0;
            int current = 0;
            comb_items[n++] = "none";
            for (const doc::Mask& other : app.document.masks) {
                if (other.id == mask.id || n >= 17) continue;
                comb_items[n] = arena.dup(other.name.c_str(),
                                          other.name.size());
                if (other.id == mask.combine_id) current = n;
                ++n;
            }
            mask_rows.push_back(value_row(
                arena, "combine",
                Dropdown(arena, comb_items, n, current, &ms.combine_dd,
                         actions.combine_selected, SizeSpec::fill(),
                         "fold another mask into this one")));
            if (mask.combine_id != 0) {
                static const char* kOpNames[] = {"add", "subtract",
                                                 "intersect"};
                actions.combine_op_selected = arena.alloc<int>();
                *actions.combine_op_selected = -1;
                mask_rows.push_back(value_row(
                    arena, "op",
                    Dropdown(arena, kOpNames, 3,
                             static_cast<int>(mask.combine_op) % 3,
                             &ms.combine_op_dd,
                             actions.combine_op_selected, SizeSpec::fill(),
                             nullptr)));
            }
        }

        StackOpts mask_col;
        mask_col.gap = 4.0f;
        mask_col.cross_align = AlignMode::Stretch;
        rows.push_back(Panel(arena, VStackDyn(arena, mask_col, mask_rows),
                             PanelOpts{Edges::all(6), -1.0f}));
        out.mask_actions.push_back(actions);
    }
    rows.push_back(Separator(arena));
    }   // end mask inspector ("+ mask" lives on the canvas aux lane)

    // ---- RIGHT PANEL: modulation routes (selection-filtered) + the
    // Output inspector (snapshots/morph) + the persistent undo/export tail.
    {
    std::vector<LayoutNode*>& rows = right_rows;   // right panel from here
    // Multi-selection tools (texed align/distribute, node context menu):
    // surfaced on the rail — looks has no per-node context menu.
    if (app.multi_sel.size() >= 2) {
        rows.push_back(Label(arena, "selection", small_dim));
        static const char* kAlignLabels[4] = {
            "align left", "align top", "spread h", "spread v"};
        static const char* kAlignTips[4] = {
            "align selected nodes to the leftmost",
            "align selected nodes to the topmost",
            "space selected nodes evenly left to right",
            "space selected nodes evenly top to bottom"};
        std::vector<LayoutNode*> cells;
        for (int a = 0; a < 4; ++a) {
            if (a >= 2 && app.multi_sel.size() < 3) break;
            out.align_clicked[a] = arena.alloc<bool>();
            ButtonOpts bo;
            bo.tooltip = kAlignTips[a];
            cells.push_back(Button(arena, kAlignLabels[a],
                                   &app.align_buttons[a],
                                   out.align_clicked[a], bo));
        }
        StackOpts arow;
        arow.gap = 4.0f;
        arow.cross_align = AlignMode::Center;
        LayoutNode* astack = VStackDyn(arena, arow, cells);
        astack->kind = NodeKind::HStack;
        rows.push_back(astack);
        rows.push_back(Separator(arena));
    }
    const bool show_matrix = app.sel.kind == SelKind::Effect ||
                             app.sel.kind == SelKind::Mask ||
                             app.sel.kind == SelKind::ModSource ||
                             app.sel.kind == SelKind::Output;
    if (app.sel.kind == SelKind::Output)
        rows.push_back(Heading(arena, "project & output"));
    else if (app.sel.kind == SelKind::ModSource)
        rows.push_back(Heading(arena, "mod route"));
    else if (show_matrix)
        rows.push_back(Label(arena, "modulation", small_dim));
    size_t routes_shown = 0;
    if (show_matrix) {
    const auto table = mod::build_param_table(app.document);
    auto path_of = [&](const doc::ParamKey& key) -> std::string {
        for (const auto& e : table)
            if (e.key == key) return e.path;
        return "(deleted)";
    };
    static const char* kSourceNames[] = {"lfo",    "drift", "a.low",
                                         "a.mid",  "a.high", "onset",
                                         "motion", "bright", "lfo.bpm",
                                         "env",    "cut",    "beat",
                                         "sample", "region"};
    static_assert(sizeof(kSourceNames) / sizeof(kSourceNames[0]) ==
                      static_cast<size_t>(doc::ModSourceType::Count),
                  "route-panel source names track the enum");
    static const char* kShapeNames[] = {"sin", "tri", "sqr", "s&h"};
    static const char* kCurveNames[] = {"lin", "exp", "s", "inv"};
    for (const doc::ModRoute& route : app.document.mod_routes) {
        // Selection filter (docs/flow_canvas.md): an effect shows its own
        // routes, a mask its mask-param routes, a mod node just itself.
        const bool match =
            app.sel.kind == SelKind::Output ? true
            : app.sel.kind == SelKind::ModSource ? route.id == app.sel.id
            : app.sel.kind == SelKind::Effect
                ? route.target.effect_id == app.sel.id
                : route.target.effect_id ==
                      (app.sel.id | doc::kMaskParamBit);
        if (!match) continue;
        ++routes_shown;
        RouteUiState& rs = app.route_ui[route.id];
        FrameUi::RouteRow row{};
        row.id = route.id;
        row.source_selected = arena.alloc<int>();
        *row.source_selected = -1;
        row.shape_selected = arena.alloc<int>();
        *row.shape_selected = -1;
        row.curve_selected = arena.alloc<int>();
        *row.curve_selected = -1;
        row.remove = arena.alloc<bool>();
        row.rate_staged = arena.alloc<float>();
        *row.rate_staged = route.source.rate_hz;
        row.rate_original = route.source.rate_hz;
        row.rate_changed = arena.alloc<bool>();
        row.rate_released = arena.alloc<bool>();
        row.amount_staged = arena.alloc<float>();
        *row.amount_staged = route.amount;
        row.amount_original = route.amount;
        row.amount_changed = arena.alloc<bool>();
        row.amount_released = arena.alloc<bool>();

        // The shape dropdown doubles as the envelope's trigger selector;
        // the rate slider doubles as decay for envelope AND beat (spec §7).
        const bool is_lfo = route.source.type == doc::ModSourceType::Lfo ||
                            route.source.type == doc::ModSourceType::LfoBeat;
        const bool is_env =
            route.source.type == doc::ModSourceType::Envelope;
        const bool is_beat =
            route.source.type == doc::ModSourceType::LfoBeat;
        const bool is_pulse =
            is_env || route.source.type == doc::ModSourceType::Beat;
        const bool has_rate = is_lfo || is_pulse ||
                              route.source.type == doc::ModSourceType::Drift;
        const bool is_video =
            route.source.type == doc::ModSourceType::VideoSample ||
            route.source.type == doc::ModSourceType::VideoRegion;
        if (is_pulse) {
            *row.rate_staged = route.source.decay;
            row.rate_original = route.source.decay;
        }

        // Unwired value node (v4): target {0, -1} is inert until its out
        // port is dropped on a param row.
        const bool unwired = route.target.effect_id == 0 &&
                             route.target.param_index < 0;
        std::vector<LayoutNode*> route_rows_ui;
        ButtonOpts route_x;
        route_x.width = SizeSpec::fixed(20);
        route_x.tooltip = "remove route";
        route_rows_ui.push_back(HStack(
            arena, {2.0f},
            {Label(arena,
                   unwired ? "(unwired - drag onto a param)"
                           : path_of(route.target).c_str(),
                   small_dim),
             Spacer(arena),
             IconButton(arena, Icon::Close, &rs.remove_button, row.remove,
                        route_x)}));
        static const char* kTriggerNames[] = {"onset", "cut", "beat", "key"};
        std::vector<LayoutNode*> pick_cells{
            Dropdown(arena, kSourceNames, 14,
                     static_cast<int>(route.source.type), &rs.source_dd,
                     row.source_selected, SizeSpec::fill(),
                     "modulation source")};
        if (is_lfo)
            pick_cells.push_back(Dropdown(
                arena, kShapeNames, 4,
                static_cast<int>(route.source.shape), &rs.shape_dd,
                row.shape_selected, SizeSpec::fill(), "LFO shape"));
        else if (is_env)
            pick_cells.push_back(Dropdown(
                arena, kTriggerNames, 4,
                static_cast<int>(route.source.trigger % 4), &rs.shape_dd,
                row.shape_selected, SizeSpec::fill(),
                "envelope trigger (key = press g in live mode)"));
        else if (is_video) {
            static const char* kChanNames[] = {"luma", "red", "green",
                                               "blue"};
            pick_cells.push_back(Dropdown(
                arena, kChanNames, 4,
                static_cast<int>(route.source.channel % 4), &rs.shape_dd,
                row.shape_selected, SizeSpec::fill(),
                "sampled channel"));
        }
        pick_cells.push_back(Dropdown(
            arena, kCurveNames, 4, static_cast<int>(route.curve),
            &rs.curve_dd, row.curve_selected, SizeSpec::fill(),
            "response curve"));
        StackOpts pick_row;
        pick_row.gap = 4.0f;
        pick_row.cross_align = AlignMode::Center;
        LayoutNode* pick_stack = VStackDyn(arena, pick_row, pick_cells);
        pick_stack->kind = NodeKind::HStack;
        route_rows_ui.push_back(pick_stack);
        if (has_rate) {
            SliderOpts rate_opts;
            rate_opts.format =
                is_pulse ? "%.2f s" : (is_beat ? "%.2f beats" : "%.2f hz");
            rate_opts.out_changed = row.rate_changed;
            rate_opts.out_released = row.rate_released;
            route_rows_ui.push_back(SliderF(arena, row.rate_staged, 0.05f,
                                            8.0f, &rs.rate_slider, rate_opts));
        }
        SliderOpts amount_opts;
        amount_opts.format = "%+.2f";
        amount_opts.out_changed = row.amount_changed;
        amount_opts.out_released = row.amount_released;
        route_rows_ui.push_back(SliderF(arena, row.amount_staged, -1.0f, 1.0f,
                                        &rs.amount_slider, amount_opts));

        StackOpts route_col;
        route_col.gap = 4.0f;
        route_col.cross_align = AlignMode::Stretch;
        rows.push_back(Panel(arena, VStackDyn(arena, route_col, route_rows_ui),
                             PanelOpts{Edges::all(6), -1.0f,
                                       /*outline=*/false}));
        out.route_rows.push_back(row);
    }
    if (routes_shown == 0)
        rows.push_back(Label(arena, "press ~ next to a param", small_dim));
    rows.push_back(Separator(arena));
    }   // end show_matrix

    // ---- snapshots + morph: the Output inspector (docs/flow_canvas.md)
    if (app.sel.kind == SelKind::Output) {
        ButtonOpts slot;
        slot.width = SizeSpec::fixed(36);
        bool* apply[3];
        bool* store[3];
        std::vector<LayoutNode*> apply_row{Label(arena, "snap", small_dim)};
        std::vector<LayoutNode*> store_row{Label(arena, "set ", small_dim)};
        static const char* kSlots[] = {"A", "B", "C"};
        for (int s = 0; s < 3; ++s) {
            apply[s] = arena.alloc<bool>();
            store[s] = arena.alloc<bool>();
            ButtonOpts apply_opts = slot;
            apply_opts.disabled = !app.document.snapshots[s].valid;
            apply_row.push_back(Button(arena, kSlots[s], &app.snap_apply[s],
                                       apply[s], apply_opts));
            store_row.push_back(Button(arena, kSlots[s], &app.snap_store[s],
                                       store[s], slot));
        }
        LayoutNode* apply_stack = VStackDyn(arena, {}, apply_row);
        apply_stack->kind = NodeKind::HStack;
        apply_stack->gap = 6.0f;
        apply_stack->cross_align = AlignMode::Center;
        LayoutNode* store_stack = VStackDyn(arena, {}, store_row);
        store_stack->kind = NodeKind::HStack;
        store_stack->gap = 6.0f;
        store_stack->cross_align = AlignMode::Center;
        rows.push_back(apply_stack);
        rows.push_back(store_stack);
        out.snap_apply_clicked[0] = apply[0];
        out.snap_apply_clicked[1] = apply[1];
        out.snap_apply_clicked[2] = apply[2];
        out.snap_store_clicked[0] = store[0];
        out.snap_store_clicked[1] = store[1];
        out.snap_store_clicked[2] = store[2];

        // Morph A>B (spec §7): live once both slots are stored; "~" routes
        // a mod source onto the morph position (ParamKey {0, 0}).
        out.morph_staged = arena.alloc<float>();
        *out.morph_staged = app.document.morph_pos;
        out.morph_changed = arena.alloc<bool>();
        out.morph_released = arena.alloc<bool>();
        SliderOpts mopts;
        mopts.format = "%.2f";
        mopts.out_changed = out.morph_changed;
        mopts.out_released = out.morph_released;
        FrameUi::AddRoute morph_route{{0, 0}, arena.alloc<bool>()};
        ButtonOpts micro2;
        micro2.width = SizeSpec::fixed(22);
        const bool morph_ready = app.document.snapshots[0].valid &&
                                 app.document.snapshots[1].valid;
        std::vector<LayoutNode*> mrow{
            Label(arena,
                  morph_ready ? "morph A>B" : "morph A>B (store A+B)",
                  small_dim),
            Spacer(arena),
            Button(arena, "~", &app.morph_route_button, morph_route.clicked,
                   micro2)};
        LayoutNode* mstack = VStackDyn(arena, {}, mrow);
        mstack->kind = NodeKind::HStack;
        mstack->gap = 4.0f;
        rows.push_back(mstack);
        rows.push_back(SliderF(arena, out.morph_staged, 0.0f, 1.0f,
                               &app.morph_slider, mopts));
        out.add_routes.push_back(morph_route);
        rows.push_back(Separator(arena));
    }

    // ---- undo/redo
    out.undo_clicked = arena.alloc<bool>();
    out.redo_clicked = arena.alloc<bool>();
    ButtonOpts undo_opts;
    undo_opts.disabled = !app.undo.can_undo();
    ButtonOpts redo_opts;
    redo_opts.disabled = !app.undo.can_redo();
    rows.push_back(HStack(arena, {8.0f},
                          {
                              Button(arena, "undo", &app.undo_button,
                                     out.undo_clicked, undo_opts),
                              Button(arena, "redo", &app.redo_button,
                                     out.redo_clicked, redo_opts),
                          }));
    const std::string undo_name = app.undo.undo_name();
    if (!undo_name.empty()) {
        std::snprintf(line, sizeof(line), "undo: %s", undo_name.c_str());
        rows.push_back(Label(arena, line, small_dim));
    }

    // ---- export + render queue (spec §9). The button stays live while a
    // job runs — further exports snapshot the current state and queue up.
    if (app.export_job) {
        const uint32_t total = app.export_job->progress.frames_total.load();
        const uint32_t done = app.export_job->progress.frames_done.load();
        std::snprintf(line, sizeof(line), "exporting %s %u%%",
                      app.export_job->out_path.filename().string().c_str(),
                      total ? done * 100 / total : 0);
        rows.push_back(Label(arena, line, dim));
    }
    if (has_clip) {
        out.export_clicked = arena.alloc<bool>();
        rows.push_back(Button(arena,
                              app.export_job ? "export (queue)..."
                                             : "export...",
                              &app.export_button, out.export_clicked));
    }
    for (size_t q = 0; q < app.export_queue.size(); ++q) {
        FrameUi::QueueRow qrow{q, arena.alloc<bool>()};
        std::snprintf(line, sizeof(line), "%zu. %s", q + 1,
                      app.export_queue[q].out_path.filename().string().c_str());
        ButtonOpts tiny_x;
        tiny_x.width = SizeSpec::fixed(24);
        std::vector<LayoutNode*> qr{
            Label(arena, line, small_dim), Spacer(arena),
            Button(arena, "x",
                   &app.queue_remove_buttons[q < 8 ? q : 7], qrow.remove,
                   tiny_x)};
        LayoutNode* qstack = VStackDyn(arena, {}, qr);
        qstack->kind = NodeKind::HStack;
        qstack->gap = 4.0f;
        rows.push_back(qstack);
        out.queue_rows.push_back(qrow);
    }
    }   // end right panel (matrix / snapshots / undo / export)

    // Theme picker (also cycles on the 't' key) + fps readout.
    out.theme_selected = arena.alloc<int>();
    *out.theme_selected = -1;
    static const char* kThemeItems[] = {"graphite", "night", "ember",
                                        "paper"};
    rows.push_back(value_row(
        arena, "theme",
        Dropdown(arena, kThemeItems, 4, app.theme_index, &app.theme_dd,
                 out.theme_selected, SizeSpec::fill(), "ui theme")));
    std::snprintf(line, sizeof(line), "%.1f fps", fps);
    rows.push_back(Label(arena, line, small_dim));

    StackOpts column;
    column.gap = 8.0f;
    column.cross_align = AlignMode::Stretch;
    // No right padding: the scroll area's own gutter IS the right margin,
    // so the scrollbar hugs the panel edge instead of floating mid-margin.
    const Edges panel_pad{12.0f, 12.0f, 2.0f, 12.0f};
    *left_out = Panel(
        arena,
        ScrollAreaV(arena, &app.sidebar_scroll, VStackDyn(arena, column, rows)),
        PanelOpts{panel_pad, -1.0f});
    *right_out = Panel(
        arena,
        ScrollAreaV(arena, &app.right_scroll,
                    VStackDyn(arena, column, right_rows)),
        PanelOpts{panel_pad, -1.0f});
    *preset_out = Panel(
        arena,
        ScrollAreaV(arena, &app.preset_scroll,
                    VStackDyn(arena, column, preset_rows)),
        PanelOpts{panel_pad, -1.0f});
}

// Bottom timeline: ruler + one row per keyframe lane (spec §9). Lanes are
// created with the [k] button next to any param.
// Transport bar under the viewport (spec §9): playback + monitoring only —
// play, scrubber, time readout, then the view chips (loop, live, preview
// res, a/b wipe, fx bypass). Document parameters (speed, time mode) stay
// in the sidebar.
ui::LayoutNode* build_transport(ui::LayoutArena& arena, AppState& app,
                                FrameUi& out) {
    using namespace ui;
    LabelOpts small_dim;
    small_dim.color = active_theme().text_dim;
    small_dim.size = active_theme().font_size_small;

    std::vector<LayoutNode*> items;
    out.play_clicked = arena.alloc<bool>();
    ButtonOpts play_opts;
    play_opts.tooltip = "play / pause (space)";
    items.push_back(IconButton(arena,
                               app.player.playing() ? Icon::Pause : Icon::Play,
                               &app.play_button, out.play_clicked, play_opts));

    const uint32_t frames = app.player.frame_count();
    out.seek_staged = arena.alloc<float>();
    *out.seek_staged = static_cast<float>(app.player.current_frame_index());
    out.seek_changed = arena.alloc<bool>();
    items.push_back(Scrubber(arena, out.seek_staged,
                             static_cast<float>(frames), &app.scrubber,
                             out.seek_changed));

    char line[64];
    std::snprintf(line, sizeof(line), "%u / %u  %.2fs",
                  app.player.current_frame_index(), frames,
                  app.player.position_seconds());
    items.push_back(SizedBox(arena, SizeSpec::fixed(104), SizeSpec::fixed(18),
                             Label(arena, line, small_dim)));

    out.loop_clicked = arena.alloc<bool>();
    items.push_back(Chip(arena, "loop", app.loop, &app.loop_check,
                         out.loop_clicked, "loop playback"));
    out.live_clicked = arena.alloc<bool>();
    items.push_back(Chip(arena, "live", app.live_mode, &app.live_button,
                         out.live_clicked,
                         "live mode: realtime mod sources, timeline hidden"));
    // Proxy indicator (spec §9): plain text, only when the player is on
    // the half-res file.
    if (app.proxy_active) {
        LabelOpts pxy;
        pxy.color = active_theme().accent_dim;
        pxy.size = active_theme().font_size_small;
        items.push_back(Label(arena, "proxy", pxy));
    }
    out.proxy_selected = arena.alloc<int>();
    *out.proxy_selected = -1;
    static const char* kResItems[] = {"full", "half", "quarter"};
    items.push_back(SizedBox(
        arena, SizeSpec::fixed(84), SizeSpec::fixed(22),
        Dropdown(arena, kResItems, 3,
                 app.preview_div == 1 ? 0 : (app.preview_div == 2 ? 1 : 2),
                 &app.proxy_dd, out.proxy_selected, SizeSpec::fill(),
                 "preview resolution")));
    out.ab_clicked = arena.alloc<bool>();
    items.push_back(Chip(arena, "a/b", app.ab_wipe, &app.ab_button,
                         out.ab_clicked, "before/after wipe (a)"));
    out.bypass_all_clicked = arena.alloc<bool>();
    items.push_back(Chip(arena, "fx", !app.bypass_all,
                         &app.bypass_all_button, out.bypass_all_clicked,
                         "bypass all effects (b)"));
    if (app.ab_wipe) {
        out.wipe_staged = arena.alloc<float>();
        *out.wipe_staged = app.wipe_pos;
        out.wipe_changed = arena.alloc<bool>();
        SliderOpts wipe_opts;
        wipe_opts.format = nullptr;
        wipe_opts.out_changed = out.wipe_changed;
        items.push_back(SizedBox(arena, SizeSpec::fixed(90),
                                 SizeSpec::fixed(22),
                                 SliderF(arena, out.wipe_staged, 0.0f, 1.0f,
                                         &app.wipe_slider, wipe_opts)));
    }

    StackOpts bar;
    bar.gap = kSpaceUnit;
    bar.cross_align = AlignMode::Center;
    LayoutNode* row = VStackDyn(arena, bar, items);
    row->kind = NodeKind::HStack;
    return Panel(arena, row, PanelOpts{Edges::xy(8.0f, 4.0f), -1.0f});
}

ui::LayoutNode* build_timeline(ui::LayoutArena& arena, AppState& app,
                               FrameUi& out) {
    using namespace ui;
    LabelOpts small_dim;
    small_dim.color = active_theme().text_dim;
    small_dim.size = active_theme().font_size_small;

    const uint32_t frame_count = app.player.frame_count();
    const uint32_t playhead = app.player.current_frame_index();
    const auto table = mod::build_param_table(app.document);

    std::vector<LayoutNode*> rows;

    auto* ruler_user = arena.alloc<RulerUser>();
    ruler_user->app = &app;
    ruler_user->out = &out;
    ruler_user->frame_count = frame_count;
    ruler_user->playhead = playhead;
    ruler_user->fps = app.player.fps();
    ruler_user->trim_in =
        std::min(app.document.clip_trim_in, frame_count ? frame_count - 1 : 0u);
    ruler_user->trim_out = app.document.clip_trim_out
                               ? std::min(app.document.clip_trim_out,
                                          frame_count)
                               : frame_count;
    ruler_user->loop_in = app.document.loop_in;
    ruler_user->loop_out = app.document.loop_out;
    ruler_user->thumbs = app.thumbs_tex;
    ruler_user->thumb_count = app.thumbs_count;
    // With a filmstrip the ruler earns more height (spec §3 thumbnails).
    const float ruler_h = app.thumbs_tex ? 34.0f : 20.0f;
    LayoutNode* ruler = make_node(arena, NodeKind::Leaf);
    ruler->width = SizeSpec::fill();
    ruler->height = SizeSpec::fixed(ruler_h);
    ruler->user = ruler_user;
    ruler->draw_fn = draw_ruler;
    ruler->hit_fn = hit_ruler;
    rows.push_back(HStack(arena, {6.0f},
                          {SizedBox(arena, SizeSpec::fixed(150),
                                    SizeSpec::fixed(ruler_h),
                                    Label(arena, "timeline", small_dim)),
                           ruler}));

    // Lanes shown: only LIVE targets (a deleted node's lanes keep their
    // keys for undo but must not render — the 0..1 fallback range flung
    // their keys outside the strip), filtered to the canvas selection
    // when one exists (globals always pass).
    auto lane_matches_selection = [&](const doc::ParamKey& target) {
        if (app.multi_sel.empty()) return true;
        if (target.effect_id == 0) return true;
        for (const uint64_t cid : app.multi_sel) {
            const uint64_t did = cid & 0x00FFFFFFFFFFFFFFull;
            switch (static_cast<flow::NodeKind>((cid >> 56) - 1)) {
                case flow::NodeKind::Effect:
                    if (!(target.effect_id & doc::kMaskParamBit) &&
                        target.effect_id == did)
                        return true;
                    break;
                case flow::NodeKind::Mask:
                    if ((target.effect_id & doc::kMaskParamBit) &&
                        (target.effect_id & ~doc::kMaskParamBit) == did)
                        return true;
                    break;
                case flow::NodeKind::Group: {
                    size_t gli = 0;
                    if (find_group_by_id(app.document, did, &gli))
                        for (const doc::EffectInstance& fx :
                             app.document.layers[gli].stack)
                            if (fx.group_id == did &&
                                fx.id == target.effect_id)
                                return true;
                    break;
                }
                default:
                    break;
            }
        }
        return false;
    };
    std::vector<LayoutNode*> lane_rows;
    for (const doc::KeyframeLane& lane : app.document.lanes) {
        std::string path;
        float min_v = 0.0f, max_v = 1.0f;
        for (const auto& e : table) {
            if (e.key == lane.target) {
                path = e.path;
                min_v = e.min_value;
                max_v = e.max_value;
                break;
            }
        }
        if (path.empty()) continue;   // dangling target: hidden
        if (!lane_matches_selection(lane.target)) continue;
        LaneUiState& lane_state =
            app.lane_ui[{lane.target.effect_id, lane.target.param_index}];

        auto* user = arena.alloc<LaneWidgetUser>();
        user->app = &app;
        user->out = &out;
        user->target = lane.target;
        user->state = &lane_state;
        user->lane = &lane;
        user->min_value = min_v;
        user->max_value = max_v;
        user->frame_count = frame_count;
        user->playhead = playhead;
        LayoutNode* widget = make_node(arena, NodeKind::Leaf);
        widget->width = SizeSpec::fill();
        widget->height = SizeSpec::fixed(42);
        widget->user = user;
        widget->draw_fn = draw_lane;
        widget->hit_fn = hit_lane;

        const char* label = arena.dup(path.c_str(), path.size());
        LabelOpts lane_label = small_dim;
        // Name column: label + loop/mute chips + the lane's X.
        FrameUi::LaneLoop ll{lane.target, arena.alloc<bool>()};
        out.lane_loops.push_back(ll);
        FrameUi::LaneMute lm{lane.target, arena.alloc<bool>()};
        out.lane_mutes.push_back(lm);
        FrameUi::LaneKill lk{lane.target, arena.alloc<bool>()};
        out.lane_kills.push_back(lk);
        ButtonOpts lane_x;
        lane_x.width = SizeSpec::fixed(18);
        lane_x.tooltip = "delete this lane (all its keyframes)";
        StackOpts chip_row;
        chip_row.gap = 2.0f;
        chip_row.cross_align = AlignMode::Center;
        std::vector<LayoutNode*> chip_cells{
            Chip(arena, "loop", lane.loop, &lane_state.loop_button,
                 ll.clicked, "wrap playback through the key span"),
            Chip(arena, "mute", lane.muted, &lane_state.mute_button,
                 lm.clicked, "keep the keys, stop driving the param"),
            IconButton(arena, Icon::Close, &lane_state.kill_button,
                       lk.clicked, lane_x)};
        LayoutNode* chips = VStackDyn(arena, chip_row, chip_cells);
        chips->kind = NodeKind::HStack;
        StackOpts name_col;
        name_col.gap = 2.0f;
        name_col.cross_align = AlignMode::Start;
        lane_rows.push_back(HStack(
            arena, {6.0f},
            {SizedBox(arena, SizeSpec::fixed(150), SizeSpec::fixed(42),
                      VStack(arena, name_col,
                             {Label(arena, label, lane_label), chips})),
             widget}));
    }
    if (!lane_rows.empty()) {
        // Lanes scroll inside whatever height the timeline seam grants —
        // the drag bar owns the region size now, not a fixed cap.
        StackOpts lane_col;
        lane_col.gap = 4.0f;
        lane_col.cross_align = AlignMode::Stretch;
        rows.push_back(
            ScrollAreaV(arena, &app.timeline_scroll,
                        VStackDyn(arena, lane_col, lane_rows)));
    }
    if (lane_rows.empty())
        rows.push_back(Label(
            arena,
            app.document.lanes.empty()
                ? "no keyframe lanes - press k next to a param"
                : "no lanes for this selection",
            small_dim));

    StackOpts column;
    column.gap = 4.0f;
    column.cross_align = AlignMode::Stretch;
    column.width = SizeSpec::fill();
    column.height = SizeSpec::fill();
    return Panel(arena, VStackDyn(arena, column, rows),
                 PanelOpts{Edges::all(8), -1.0f});
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmdline, int) {
    platform::init();

    platform::WindowDesc window_desc;
    window_desc.title = "looks";
    auto window = platform::create_window(window_desc);

#if defined(_DEBUG) || !defined(NDEBUG)
    constexpr bool kValidation = true;
#else
    constexpr bool kValidation = false;
#endif

    auto renderer = gfx::Renderer::create(window->native_window(),
                                          window->native_instance(),
                                          window->width(), window->height(),
                                          kValidation);
    if (!renderer) {
        fatal_dialog(L"Vulkan initialization failed. A Vulkan 1.2+ driver with "
                     L"dynamic rendering support is required.");
        return 1;
    }

    const std::filesystem::path shader_dir = executable_dir() / "shaders";
    auto ui_renderer = ui::UiRenderer::create(
        renderer->device(), renderer->swapchain_format(), shader_dir);
    auto engine = gfx::Engine::create(renderer->device(), shader_dir);
    auto viewport_pass = gfx::ViewportPass::create(
        renderer->device(), renderer->swapchain_format(), shader_dir);
    if (!ui_renderer || !engine || !viewport_pass) {
        fatal_dialog(L"Renderer initialization failed (missing shaders?).");
        return 1;
    }

    // Baked MSDF fonts (spec §12 fontbake) when the build staged them:
    // Outfit (sans) for body text, Cormorant (serif) for headers. The
    // compiled-in debug font covers machines without the bake tool.
    const std::filesystem::path fonts_dir = executable_dir() / "assets/fonts";
    ui::Font font = [&] {
        if (auto baked = ui::Font::load_msdf(fonts_dir / "ui_font.png",
                                             fonts_dir / "ui_font.json"))
            return std::move(*baked);
        return ui::Font::create_debug();
    }();
    if (!ui_renderer->register_font(font)) {
        fatal_dialog(L"Font atlas upload failed.");
        return 1;
    }
    std::optional<ui::Font> header_font =
        ui::Font::load_msdf(fonts_dir / "header_font.png",
                            fonts_dir / "header_font.json");
    if (header_font && !ui_renderer->register_font(*header_font))
        header_font.reset();
    {
        // The fx glyph ASCII atlas stays on the compiled-in debug font —
        // its 128x48 A8 layout is what the glyph renderer expects, and the
        // pixel-font look is the aesthetic.
        ui::Font debug_font = ui::Font::create_debug();
        const std::vector<uint8_t> ascii_atlas = build_ascii_atlas(debug_font);
        engine->set_glyph_atlas(ascii_atlas.data(), 128, 48);
    }

    ui::Canvas2D canvas;
    ui::Context ctx;
    ui::LayoutArena arena;
    ui::UiInput input;
    AppState app;

    // Preset browser: shipped era presets next to the exe, user saves in
    // ./presets (created on first save).
    app.shipped_preset_dir = executable_dir() / "assets" / "presets";
    {
        // Font list for the Text effect's dropdown — must mirror the
        // engine's scan (lowercased-filename sort) so indices agree.
        std::vector<std::string> stems;
        std::error_code fec;
        std::filesystem::directory_iterator fit(
            executable_dir() / "assets" / "fonts", fec), fend;
        for (; !fec && fit != fend; fit.increment(fec)) {
            std::string ext = fit->path().extension().string();
            for (char& c : ext)
                c = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c)));
            if (ext == ".ttf")
                stems.push_back(fit->path().stem().string());
        }
        std::sort(stems.begin(), stems.end(),
                  [](std::string a, std::string b) {
                      for (char& c : a)
                          c = static_cast<char>(
                              std::tolower(static_cast<unsigned char>(c)));
                      for (char& c : b)
                          c = static_cast<char>(
                              std::tolower(static_cast<unsigned char>(c)));
                      return a < b;
                  });
        for (size_t s = 0; s < stems.size() && s < 8; ++s) {
            if (s) app.font_options += '|';
            app.font_options += stems[s];
        }
    }
    app.user_preset_dir = executable_dir() / "presets";
    load_ui_prefs(app);
    rescan_presets(app);

    // Preview render thread (spec §13): takes ownership of the preview
    // engine — from here on the UI thread never touches it directly.
    RenderWorker render_worker(renderer->device(), app.player,
                               std::move(engine));
    if (!render_worker.start()) {
        fatal_dialog(L"Render thread startup failed.");
        return 1;
    }
    app.render_worker = &render_worker;

    // Viewport sampling state for the UI thread (the engine's descriptor
    // arena and sampler moved to the worker with it).
    gfx::DescriptorArena ui_view_arena(renderer->device());
    VkSampler ui_view_sampler = VK_NULL_HANDLE;
    {
        VkSamplerCreateInfo sampler_info{
            VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler_info.magFilter = VK_FILTER_LINEAR;
        sampler_info.minFilter = VK_FILTER_LINEAR;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(renderer->device().device(), &sampler_info,
                            nullptr, &ui_view_sampler) != VK_SUCCESS) {
            fatal_dialog(L"Viewport sampler creation failed.");
            return 1;
        }
    }

    // A fresh document starts MINIMAL (v5.4, user demand — the demo
    // stack got deleted every launch): one clip source wired to the
    // Output, nothing else. Opening a clip shows immediately; presets
    // and the add menu build from there.

    // `looks.exe <clip|project.json>` opens it at startup (projects load
    // their bound clip themselves).
    if (cmdline && cmdline[0]) {
        std::wstring arg = cmdline;
        // Launchers pad the command line (Start-Process appends a trailing
        // space); Win32 file lookups tolerate it but string compares don't.
        const auto trim = [](std::wstring& s) {
            while (!s.empty() && (s.back() == L' ' || s.back() == L'\t'))
                s.pop_back();
            while (!s.empty() && (s.front() == L' ' || s.front() == L'\t'))
                s.erase(s.begin());
        };
        trim(arg);
        if (arg.size() >= 2 && arg.front() == L'"' && arg.back() == L'"')
            arg = arg.substr(1, arg.size() - 2);
        trim(arg);
        std::filesystem::path source(arg);
        if (std::filesystem::exists(source)) {
            if (source.extension() == L".json") open_project(app, source);
            else open_source(app, source);
        }
    }

    std::vector<platform::Event> events;
    auto last_time = std::chrono::steady_clock::now();
    float smoothed_dt = 1.0f / 60.0f;

    bool running = true;
    while (running) {
        events.clear();
        if (!window->pump_events(events)) break;
        bool toggle_play = false;
        bool do_undo = false, do_redo = false;
        bool do_save = false, do_save_as = false, do_open_project = false;
        bool do_delete_sel = false;
        bool do_add_first = false;   // Enter in the cursor add menu
        bool do_duplicate = false;   // Ctrl+D (texed duplicateSelection)
        bool do_group = false;       // Ctrl+G: fold selection into a group
        bool do_ungroup = false;     // Ctrl+Shift+G: dissolve it
        bool do_select_all = false;  // Ctrl+A (texed selectAll)
        bool do_copy = false;        // Ctrl+C / first half of Ctrl+X
        bool do_cut = false;         // Ctrl+X: delete after the copy
        bool do_paste = false;       // Ctrl+V at the canvas cursor
        float nudge_dx = 0.0f, nudge_dy = 0.0f;   // arrow-key node nudge
        std::string dropped_file;
        for (const platform::Event& e : events) {
            switch (e.type) {
                case platform::Event::Type::CloseRequested:
                    running = false;
                    break;
                case platform::Event::Type::Char:
                    // Still duration typing: digits and one decimal point.
                    if (app.duration_focus) {
                        if ((e.codepoint >= '0' && e.codepoint <= '9') ||
                            (e.codepoint == '.' &&
                             app.duration_edit.find('.') ==
                                 std::string::npos))
                            app.duration_edit.push_back(
                                static_cast<char>(e.codepoint));
                        break;
                    }
                    // Inline value typing (canvas double-click).
                    if (app.value_edit_node) {
                        if (((e.codepoint >= '0' && e.codepoint <= '9') ||
                             e.codepoint == '.' || e.codepoint == '-') &&
                            app.value_edit_buf.size() < 16)
                            app.value_edit_buf.push_back(
                                static_cast<char>(e.codepoint));
                        break;
                    }
                    // Frame rename typing (canvas inline edit).
                    if (app.frame_rename_id) {
                        if (e.codepoint >= 32 && e.codepoint < 127 &&
                            app.frame_rename_buf.size() < 64)
                            app.frame_rename_buf.push_back(
                                static_cast<char>(e.codepoint));
                        break;
                    }
                    // Group card rename typing (texed subgraph rename).
                    if (app.group_rename_id) {
                        if (e.codepoint >= 32 && e.codepoint < 127 &&
                            app.group_rename_buf.size() < 60)
                            app.group_rename_buf.push_back(
                                static_cast<char>(e.codepoint));
                        break;
                    }
                    // Text card string typing (v5.5 inline edit).
                    if (app.text_edit_id) {
                        if (e.codepoint >= 32 && e.codepoint < 127 &&
                            app.text_edit_buf.size() < 64)
                            app.text_edit_buf.push_back(
                                static_cast<char>(e.codepoint));
                        break;
                    }
                    // Preset search typing (spec §9): printable ASCII only.
                    if (app.preset_search_focus && e.codepoint >= 32 &&
                        e.codepoint < 127)
                        app.preset_filter.push_back(
                            static_cast<char>(e.codepoint));
                    // Add-node search (docs/flow_canvas.md v3).
                    if (app.fx_search_focus && e.codepoint >= 32 &&
                        e.codepoint < 127)
                        app.fx_filter.push_back(
                            static_cast<char>(e.codepoint));
                    break;
                case platform::Event::Type::KeyDown:
                    if (app.duration_focus) {
                        // Same swallow-the-keyboard contract as the preset
                        // search: backspace edits, Enter commits, Escape
                        // abandons.
                        if (e.key == platform::Key::Backspace &&
                            !app.duration_edit.empty()) {
                            app.duration_edit.pop_back();
                        } else if (e.key == platform::Key::Enter) {
                            if (!app.duration_edit.empty())
                                apply_still_duration(
                                    app, std::atof(app.duration_edit.c_str()));
                            app.duration_focus = false;
                            app.duration_edit.clear();
                        } else if (e.key == platform::Key::Escape) {
                            app.duration_focus = false;
                            app.duration_edit.clear();
                        }
                        break;
                    }
                    if (app.value_edit_node) {
                        // Enter commits (applied post-frame where the
                        // row's staged pointer exists), Escape abandons.
                        if (e.key == platform::Key::Backspace &&
                            !app.value_edit_buf.empty()) {
                            app.value_edit_buf.pop_back();
                        } else if (e.key == platform::Key::Enter) {
                            app.value_commit = true;
                        } else if (e.key == platform::Key::Escape) {
                            app.value_edit_node = 0;
                            app.value_edit_row = -1;
                            app.value_edit_buf.clear();
                        }
                        break;
                    }
                    if (app.frame_rename_id) {
                        // Same swallow contract as the searches: Enter
                        // commits the title, Escape abandons.
                        if (e.key == platform::Key::Backspace &&
                            !app.frame_rename_buf.empty()) {
                            app.frame_rename_buf.pop_back();
                        } else if (e.key == platform::Key::Enter) {
                            app.undo.execute(
                                app.document,
                                doc::set_frame_title_command(
                                    app.frame_rename_id,
                                    app.frame_rename_buf));
                            app.frame_rename_id = 0;
                            app.frame_rename_buf.clear();
                        } else if (e.key == platform::Key::Escape) {
                            app.frame_rename_id = 0;
                            app.frame_rename_buf.clear();
                        }
                        break;
                    }
                    if (app.group_rename_id) {
                        // Same swallow contract; commit rewrites the
                        // group's name through set_group_props.
                        if (e.key == platform::Key::Backspace &&
                            !app.group_rename_buf.empty()) {
                            app.group_rename_buf.pop_back();
                        } else if (e.key == platform::Key::Enter) {
                            size_t gli = 0;
                            if (find_group_by_id(app.document,
                                                 app.group_rename_id,
                                                 &gli)) {
                                for (const doc::Group& gr :
                                     app.document.layers[gli].groups)
                                    if (gr.id == app.group_rename_id) {
                                        doc::Group edited = gr;
                                        edited.name = app.group_rename_buf;
                                        app.undo.execute(
                                            app.document,
                                            doc::set_group_props_command(
                                                gli, edited));
                                        break;
                                    }
                            }
                            app.group_rename_id = 0;
                            app.group_rename_buf.clear();
                        } else if (e.key == platform::Key::Escape) {
                            app.group_rename_id = 0;
                            app.group_rename_buf.clear();
                        }
                        break;
                    }
                    if (app.text_edit_id) {
                        // Same swallow contract; Enter commits the string
                        // through the undoable text command.
                        if (e.key == platform::Key::Backspace &&
                            !app.text_edit_buf.empty()) {
                            app.text_edit_buf.pop_back();
                        } else if (e.key == platform::Key::Enter) {
                            size_t li = 0, fi = 0;
                            if (find_effect_by_id(app.document,
                                                  app.text_edit_id, &li,
                                                  &fi))
                                app.undo.execute(
                                    app.document,
                                    doc::set_effect_text_command(
                                        li, fi, app.text_edit_buf));
                            app.text_edit_id = 0;
                            app.text_edit_buf.clear();
                        } else if (e.key == platform::Key::Escape) {
                            app.text_edit_id = 0;
                            app.text_edit_buf.clear();
                        }
                        break;
                    }
                    if (app.preset_search_focus) {
                        // The search field swallows the keyboard: only
                        // backspace/enter/escape mean anything here.
                        if (e.key == platform::Key::Backspace &&
                            !app.preset_filter.empty())
                            app.preset_filter.pop_back();
                        else if (e.key == platform::Key::Enter ||
                                 e.key == platform::Key::Escape)
                            app.preset_search_focus = false;
                        break;
                    }
                    if (app.fx_search_focus) {
                        if (e.key == platform::Key::Backspace &&
                            !app.fx_filter.empty()) {
                            app.fx_filter.pop_back();
                        } else if (e.key == platform::Key::Enter) {
                            if (app.canvas_state.add_open)
                                do_add_first = true;
                            app.fx_search_focus = false;
                        } else if (e.key == platform::Key::Escape) {
                            app.fx_search_focus = false;
                            app.canvas_state.add_open = false;
                            app.find_mode = false;
                        }
                        break;
                    }
                    // Escape deselects first (docs/flow_canvas.md); a bare
                    // Escape with nothing selected keeps the old quit.
                    if (e.key == platform::Key::Escape) {
                        if (app.canvas_state.dd_open) {
                            app.canvas_state.dd_open = false;
                        } else if (app.canvas_state.ctx_open) {
                            app.canvas_state.ctx_open = false;
                        } else if (app.open_group &&
                                   app.sel.kind == SelKind::None) {
                            app.open_group = 0;   // exit the subgraph
                            if (app.saved_view_valid) {
                                app.canvas_state.pan_x = app.saved_pan_x;
                                app.canvas_state.pan_y = app.saved_pan_y;
                                app.canvas_state.zoom = app.saved_zoom;
                                app.saved_view_valid = false;
                            } else {
                                app.canvas_state.view_inited = false;
                            }
                        } else if (app.sel.kind != SelKind::None) {
                            app.sel = {};
                            app.insert_before_id = 0;
                        } else {
                            running = false;
                        }
                    } else if ((e.key == platform::Key::Delete ||
                                e.key == platform::Key::Backspace) &&
                               !e.repeat) {
                        do_delete_sel = true;   // texed: both keys delete
                    }
                    else if (e.key == platform::Key::Space && !e.repeat)
                        toggle_play = true;
                    else if (e.key == platform::Key::Z &&
                             (e.mods & platform::kModCtrl)) {
                        if (e.mods & platform::kModShift) do_redo = true;
                        else do_undo = true;
                    } else if (e.key == platform::Key::Y &&
                               (e.mods & platform::kModCtrl)) {
                        do_redo = true;
                    } else if (e.key == platform::Key::G && !e.repeat &&
                               (e.mods & platform::kModCtrl)) {
                        // texed Ctrl+G / Ctrl+Shift+G (subgraphs).
                        if (e.mods & platform::kModShift) do_ungroup = true;
                        else do_group = true;
                    } else if (e.key == platform::Key::D && !e.repeat &&
                               (e.mods & platform::kModCtrl)) {
                        do_duplicate = true;   // texed Ctrl+D
                    } else if (e.key == platform::Key::Left ||
                               e.key == platform::Key::Right ||
                               e.key == platform::Key::Up ||
                               e.key == platform::Key::Down) {
                        // Arrow nudge (texed): selection moves 1 graph
                        // unit, shift = 10; repeats coalesce via the
                        // position command's merge.
                        const float step =
                            (e.mods & platform::kModShift) ? 10.0f : 1.0f;
                        if (e.key == platform::Key::Left) nudge_dx -= step;
                        if (e.key == platform::Key::Right) nudge_dx += step;
                        if (e.key == platform::Key::Up) nudge_dy -= step;
                        if (e.key == platform::Key::Down) nudge_dy += step;
                    } else if (e.key == platform::Key::S &&
                               (e.mods & platform::kModCtrl)) {
                        if (e.mods & platform::kModShift) do_save_as = true;
                        else do_save = true;
                    } else if (e.key == platform::Key::O &&
                               (e.mods & platform::kModCtrl)) {
                        do_open_project = true;
                    } else if (e.key == platform::Key::A && !e.repeat &&
                               (e.mods & platform::kModCtrl)) {
                        do_select_all = true;   // texed Ctrl+A
                    } else if (e.key == platform::Key::F && !e.repeat &&
                               (e.mods & platform::kModCtrl)) {
                        // texed Ctrl+F: find / jump-to-node popup (the
                        // cursor add menu in find mode).
                        app.find_mode = true;
                        app.fx_filter.clear();
                        app.fx_search_focus = true;
                        app.canvas_state.add_open = true;
                        app.canvas_state.add_anchor =
                            app.canvas_state.last_mouse;
                        app.canvas_state.add_scroll = 0.0f;
                        app.canvas_state.splice_from = 0;
                        app.canvas_state.splice_to = 0;
                        app.canvas_state.splice_port = 0;
                    } else if (e.key == platform::Key::F && !e.repeat &&
                               (e.mods & (platform::kModCtrl |
                                          platform::kModAlt)) == 0) {
                        // Fit view (texed F): re-trigger the first-frame
                        // content fit.
                        app.canvas_state.view_inited = false;
                    } else if (e.key == platform::Key::C && !e.repeat &&
                               (e.mods & platform::kModCtrl)) {
                        do_copy = true;    // texed Ctrl+C
                    } else if (e.key == platform::Key::X && !e.repeat &&
                               (e.mods & platform::kModCtrl)) {
                        do_copy = true;    // texed Ctrl+X = copy + delete
                        do_cut = true;
                    } else if (e.key == platform::Key::V && !e.repeat &&
                               (e.mods & platform::kModCtrl)) {
                        do_paste = true;   // texed Ctrl+V
                    } else if (e.key == platform::Key::A && !e.repeat &&
                               (e.mods & (platform::kModCtrl |
                                          platform::kModAlt)) == 0) {
                        app.ab_wipe = !app.ab_wipe;   // spec §9 A/B wipe
                    } else if (e.key == platform::Key::B && !e.repeat &&
                               (e.mods & (platform::kModCtrl |
                                          platform::kModAlt)) == 0) {
                        // texed B: bypass the SELECTION when one exists;
                        // a bare B keeps the app-wide fx toggle.
                        bool any = false;
                        for (const uint64_t cid : app.multi_sel) {
                            const uint64_t did =
                                cid & 0x00FFFFFFFFFFFFFFull;
                            const auto kind =
                                static_cast<flow::NodeKind>(
                                    (cid >> 56) - 1);
                            size_t li = 0, fi = 0;
                            if (kind == flow::NodeKind::Effect &&
                                find_effect_by_id(app.document, did, &li,
                                                  &fi)) {
                                if (!any)
                                    app.undo.begin_group("Bypass");
                                any = true;
                                app.undo.execute(
                                    app.document,
                                    doc::set_bypass_command(
                                        li, fi,
                                        !app.document.layers[li]
                                             .stack[fi].bypass));
                            } else if (kind == flow::NodeKind::Group &&
                                       find_group_by_id(app.document, did,
                                                        &li)) {
                                for (const doc::Group& gr :
                                     app.document.layers[li].groups)
                                    if (gr.id == did) {
                                        if (!any)
                                            app.undo.begin_group(
                                                "Bypass");
                                        any = true;
                                        doc::Group edited = gr;
                                        edited.bypass = !edited.bypass;
                                        app.undo.execute(
                                            app.document,
                                            doc::set_group_props_command(
                                                li, edited));
                                        break;
                                    }
                            }
                        }
                        if (any) app.undo.end_group();
                        else app.bypass_all = !app.bypass_all;
                    } else if (e.key == platform::Key::M && !e.repeat &&
                               (e.mods & (platform::kModCtrl |
                                          platform::kModAlt)) == 0) {
                        // Cycle the viewport mask overlay (spec §8/§9):
                        // off -> mask 1 -> mask 2 -> ... -> off.
                        const auto& masks = app.document.masks;
                        size_t cur = masks.size();
                        for (size_t i = 0; i < masks.size(); ++i)
                            if (masks[i].id == app.overlay_mask_id) cur = i;
                        const size_t next = cur + 1;
                        app.overlay_mask_id =
                            next < masks.size() ? masks[next].id
                            : (cur == masks.size() && !masks.empty()
                                   ? masks[0].id
                                   : 0);
                    } else if (e.key == platform::Key::T && !e.repeat &&
                               (e.mods & (platform::kModCtrl |
                                          platform::kModAlt)) == 0) {
                        // Cycle the UI theme (also the sidebar button).
                        app.theme_index =
                            (app.theme_index + 1) % ui::theme_count();
                        ui::set_active_theme(app.theme_index);
                        save_ui_prefs(app);
                    } else if (e.key == platform::Key::G && !e.repeat &&
                               (e.mods & (platform::kModCtrl |
                                          platform::kModAlt)) == 0) {
                        // Envelope keypress trigger (spec §7): live-mode
                        // only — wall-clock triggers are exempt from
                        // determinism (spec §11) there and only there.
                        if (app.live_mode) app.env_key_time = app.app_seconds;
                    }
                    break;
                case platform::Event::Type::FileDrop:
                    dropped_file = e.drop_path;
                    break;
                case platform::Event::Type::Resize:
                    renderer->notify_resize(static_cast<uint32_t>(e.width),
                                            static_cast<uint32_t>(e.height));
                    break;
                default:
                    break;
            }
        }
        if (!running) {
            window->request_close();
            break;
        }
        if (window->minimized()) {
            Sleep(16);
            continue;
        }

        // Finished import → open the player (worker held off the reader).
        if (app.import && app.import->done.load()) {
            auto job = std::move(app.import);
            if (job->thread.joinable()) job->thread.join();
            if (job->result.ok) {
                std::string error;
                render_worker.pause();
                render_worker.invalidate();
                struct ResumeGuard {
                    RenderWorker& w;
                    ~ResumeGuard() { w.resume(); }
                } import_resume{render_worker};
                if (app.player.open(job->result.mez_path, job->result.pcm_path,
                                    &error)) {
                    app.clip_name = job->source.filename().string();
                    app.mez_path = job->result.mez_path;
                    app.pcm_path = job->result.pcm_path;
                    load_clip_analysis(app);
                    app.player.set_looping(app.loop);
                    app.player.play();
                    app.status.clear();
                    // A rebuilt still bundle (cache cleared, new machine)
                    // comes back at the import default; the document's
                    // persisted duration wins.
                    if (app.document.still_duration_frames > 0 &&
                        is_still_source(job->source))
                        set_still_frames(app,
                                         app.document.still_duration_frames);
                } else {
                    app.status = "open failed: " + error;
                }
            } else {
                app.status = "import failed: " + job->result.error;
            }
        }

        // Audio Scope: keep the engine's mono PCM copy in sync with the
        // current clip — every open path funnels through app.pcm_path, so
        // one poll covers them all (empty path = silent, flat line).
        if (!app.scope_pcm_init || app.scope_pcm_loaded != app.pcm_path) {
            app.scope_pcm_init = true;
            app.scope_pcm_loaded = app.pcm_path;
            uint32_t scope_rate = 0;
            auto scope_mono = load_scope_audio(app.pcm_path, &scope_rate);
            render_worker.post_scope_audio(std::move(scope_mono),
                                           scope_rate);
        }

        // Finished export → report, then start the next queued job.
        if (app.export_job && app.export_job->done.load()) {
            auto job = std::move(app.export_job);
            if (job->thread.joinable()) job->thread.join();
            app.status = job->result.ok
                ? "exported " + job->out_path.filename().string()
                : "export failed: " + job->result.error;
            if (!app.export_queue.empty()) {
                AppState::QueuedExport next =
                    std::move(app.export_queue.front());
                app.export_queue.erase(app.export_queue.begin());
                app.export_job = start_export(
                    renderer->device(), shader_dir, next.mez_path,
                    next.pcm_path, next.doc,
                    next.has_analysis ? &next.analysis : nullptr,
                    next.out_path);
            }
        }

        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - last_time).count();
        last_time = now;
        smoothed_dt += (dt - smoothed_dt) * 0.05f;
        app.app_seconds += dt;   // live-mode mod clock

        gfx::FrameContext frame;
        if (!renderer->begin_frame(frame)) continue;
        // Render-thread bookkeeping (spec §13): begin_frame waited this
        // slot's fence, so UI submissions kFramesInFlight back have
        // retired — the worker may rewrite publish images they sampled.
        ++app.ui_frame_counter;
        render_worker.set_completed_ui_frame(
            app.ui_frame_counter > gfx::kFramesInFlight
                ? app.ui_frame_counter - gfx::kFramesInFlight
                : 0);

        const float scale = window->dpi_scale();
        const ui::Rect viewport{0, 0,
                                static_cast<float>(frame.extent.width) / scale,
                                static_cast<float>(frame.extent.height) / scale};

        input.begin_frame(events, scale);
        canvas.begin_frame(scale, {viewport.w, viewport.h});
        arena.reset();

        FrameUi frame_ui;

        ui::LayoutNode* preview = ui::make_node(arena, ui::NodeKind::Leaf);
        preview->width = ui::SizeSpec::fill();
        preview->height = ui::SizeSpec::fill();
        preview->draw_fn = draw_preview_frame;
        frame_ui.preview = preview;

        // Selection stays in range across undo/redo of layer edits.
        if (!app.document.layers.empty() &&
            app.selected_layer >= app.document.layers.size())
            app.selected_layer = app.document.layers.size() - 1;
        validate_selection(app);
        // Node thumbnails: the newest published atlas + its cell map.
        // acquire() also marks the slot for this UI frame — idempotent
        // with the viewport's own acquire later this frame.
        RenderWorker::View thumb_view =
            render_worker.acquire(app.ui_frame_counter);
        const ui::UiTexture* thumb_tex = nullptr;
        if (thumb_view.thumb_img) {
            auto reg = app.thumb_registry.find(thumb_view.thumb_img->view());
            if (reg == app.thumb_registry.end())
                reg = app.thumb_registry
                          .emplace(thumb_view.thumb_img->view(),
                                   ui_renderer->register_external(
                                       thumb_view.thumb_img->view(),
                                       thumb_view.thumb_img->width(),
                                       thumb_view.thumb_img->height()))
                          .first;
            thumb_tex = reg->second;
        }
        // Node canvas (docs/flow_canvas.md): graph + events built before
        // the panels so the rail and the canvas share one selection.
        FlowBuild flow_ui = build_flow(arena, app, frame_ui, thumb_tex,
                                       thumb_view.thumb_img
                                           ? &thumb_view.thumb_cells
                                           : nullptr);
        // ---- four-region layout (user sketch): menu bar on top; the
        // node graph DOMINATES top-left with the timeline under it; the
        // preview + transport and the tabbed inspector stack right. All
        // three seams drag (SplitterBar) and persist in ui.json. Both old
        // rails still build every frame — their staged pointers feed the
        // post-frame handlers — but only the active inspector tab joins
        // the tree.
        ui::LayoutNode* left_panel = nullptr;
        ui::LayoutNode* right_panel = nullptr;
        ui::LayoutNode* preset_panel = nullptr;
        build_side_panels(arena, app, frame_ui, 1.0f / smoothed_dt,
                          &left_panel, &right_panel, &preset_panel);

        // Menu bar: every file/edit/view action stays reachable without
        // the old rail buttons; picks dispatch after RunPopup below.
        static const char* kFileItems[] = {
            "open clip...",           "open project...  (ctrl+o)",
            "save project  (ctrl+s)", "save project as...",
            "import preset...",       "export..."};
        static const char* kEditItems[] = {
            "undo  (ctrl+z)",      "redo  (ctrl+y)",
            "duplicate  (ctrl+d)", "group  (ctrl+g)",
            "ungroup  (ctrl+shift+g)", "select all  (ctrl+a)"};
        static const char* kViewItems[] = {
            "fit graph  (f)",   "find node...  (ctrl+f)",
            "cycle theme  (t)", "mask overlay  (m)",
            "a/b wipe  (a)",    "bypass fx  (b)"};
        int* menu_picks = arena.alloc<int>(3);
        for (int m = 0; m < 3; ++m) menu_picks[m] = -1;
        char fps_buf[32];
        std::snprintf(fps_buf, sizeof(fps_buf), "%.0f fps",
                      1.0f / std::max(smoothed_dt, 1e-4f));
        ui::LabelOpts bar_dim;
        bar_dim.color = ui::active_theme().text_dim;
        ui::StackOpts bar;
        bar.gap = 2.0f;
        bar.cross_align = ui::AlignMode::Center;
        bar.width = ui::SizeSpec::fill();
        ui::LayoutNode* menu_bar = ui::HStack(
            arena, bar,
            {MenuButton(arena, "file", kFileItems, 6,
                        &app.menu_states[0], &menu_picks[0]),
             MenuButton(arena, "edit", kEditItems, 6,
                        &app.menu_states[1], &menu_picks[1]),
             MenuButton(arena, "view", kViewItems, 6,
                        &app.menu_states[2], &menu_picks[2]),
             ui::Spacer(arena),
             ui::Label(arena, app.status.c_str(), bar_dim),
             ui::SizedBox(arena, ui::SizeSpec::fixed(12.0f),
                          ui::SizeSpec::fixed(1.0f), nullptr),
             ui::Label(arena, fps_buf, bar_dim)});

        // LEFT column: the node canvas over the timeline (timeline
        // collapses in Live mode / with nothing open).
        ui::LayoutNode* canvas_band = flow::FlowCanvas(
            arena, flow_ui.graph, &app.canvas_state, flow_ui.events);
        frame_ui.canvas_node = canvas_band;
        ui::LayoutNode* left_col = canvas_band;
        if (app.player.is_open() && !app.live_mode) {
            const float tl_h = std::clamp(
                app.split_timeline * viewport.h, 96.0f,
                viewport.h * 0.6f);
            ui::LayoutNode* timeline =
                build_timeline(arena, app, frame_ui);
            ui::LayoutNode* tl_box =
                ui::SizedBox(arena, ui::SizeSpec::fill(),
                             ui::SizeSpec::fixed(tl_h), timeline);
            ui::StackOpts lc;
            lc.cross_align = ui::AlignMode::Stretch;
            lc.width = ui::SizeSpec::fill();
            lc.height = ui::SizeSpec::fill();
            left_col = ui::VStack(
                arena, lc,
                {canvas_band,
                 SplitterBar(arena, app, &app.split_timeline,
                             &app.split_drag[1], viewport.h, 1, -1.0f,
                             0.1f, 0.6f),
                 tl_box});
        }

        // RIGHT column: preview (+ transport / empty state) over the
        // tabbed inspector.
        ui::LayoutNode* preview_cell = preview;
        if (app.player.is_open()) {
            ui::StackOpts rc;
            rc.gap = 6.0f;
            rc.cross_align = ui::AlignMode::Stretch;
            rc.width = ui::SizeSpec::fill();
            rc.height = ui::SizeSpec::fill();
            preview_cell = ui::VStack(
                arena, rc, {preview, build_transport(arena, app, frame_ui)});
        } else if (!app.import) {
            ui::LabelOpts hint;
            hint.color = ui::active_theme().text_dim;
            hint.size = ui::active_theme().font_size + 2.0f;
            ui::ButtonOpts big;
            big.width = ui::SizeSpec::fixed(150);
            ui::StackOpts center;
            center.justify = ui::Justify::Center;
            center.cross_align = ui::AlignMode::Center;
            center.width = ui::SizeSpec::fill();
            center.height = ui::SizeSpec::fill();
            center.gap = 10.0f;
            ui::LayoutNode* empty_ui = ui::VStack(
                arena, center,
                {ui::Label(arena, "drag a clip here", hint),
                 ui::Button(arena, "open clip...", &app.open_big_button,
                            frame_ui.open_clicked, big)});
            ui::LayoutNode* z = ui::ZStack(arena, {preview, empty_ui});
            z->width = ui::SizeSpec::fill();
            z->height = ui::SizeSpec::fill();
            preview_cell = z;
        }
        const float pv_h = std::clamp(app.split_preview * viewport.h,
                                      140.0f, viewport.h * 0.7f);
        ui::LayoutNode* pv_box =
            ui::SizedBox(arena, ui::SizeSpec::fill(),
                         ui::SizeSpec::fixed(pv_h), preview_cell);
        bool* tab_node = arena.alloc<bool>();
        bool* tab_project = arena.alloc<bool>();
        bool* tab_presets = arena.alloc<bool>();
        ui::StackOpts tabs_row;
        tabs_row.gap = 4.0f;
        tabs_row.cross_align = ui::AlignMode::Center;
        tabs_row.width = ui::SizeSpec::fill();
        ui::LayoutNode* tabs = ui::HStack(
            arena, tabs_row,
            {ui::Chip(arena, "node", app.inspector_tab == 0,
                      &app.tab_buttons[0], tab_node,
                      "selection inspector"),
             ui::Chip(arena, "project", app.inspector_tab == 1,
                      &app.tab_buttons[1], tab_project,
                      "clip & project settings"),
             ui::Chip(arena, "presets", app.inspector_tab == 2,
                      &app.tab_buttons[2], tab_presets,
                      "preset browser")});
        ui::StackOpts insp_col;
        insp_col.gap = 4.0f;
        insp_col.cross_align = ui::AlignMode::Stretch;
        insp_col.width = ui::SizeSpec::fill();
        insp_col.height = ui::SizeSpec::fill();
        ui::LayoutNode* inspector = ui::VStack(
            arena, insp_col,
            {tabs,
             ui::SizedBox(arena, ui::SizeSpec::fill(),
                          ui::SizeSpec::fill(),
                          app.inspector_tab == 2
                              ? preset_panel
                              : (app.inspector_tab == 1 ? left_panel
                                                        : right_panel))});
        const float right_w = std::clamp(app.split_right * viewport.w,
                                         280.0f, 560.0f);
        ui::StackOpts rc2;
        rc2.gap = 0.0f;
        rc2.cross_align = ui::AlignMode::Stretch;
        rc2.width = ui::SizeSpec::fill();
        rc2.height = ui::SizeSpec::fill();
        ui::LayoutNode* right_col_inner = ui::VStack(
            arena, rc2,
            {pv_box,
             SplitterBar(arena, app, &app.split_preview,
                         &app.split_drag[2], viewport.h, 1, 1.0f, 0.15f,
                         0.7f),
             inspector});
        ui::LayoutNode* right_col =
            ui::SizedBox(arena, ui::SizeSpec::fixed(right_w),
                         ui::SizeSpec::fill(), right_col_inner);

        ui::StackOpts body_opts;
        body_opts.cross_align = ui::AlignMode::Stretch;
        body_opts.width = ui::SizeSpec::fill();
        body_opts.height = ui::SizeSpec::fill();
        ui::LayoutNode* body = ui::HStack(
            arena, body_opts,
            {left_col,
             SplitterBar(arena, app, &app.split_right, &app.split_drag[0],
                         viewport.w, 0, -1.0f, 0.15f, 0.5f),
             right_col});

        ui::StackOpts root_opts;
        root_opts.gap = 4.0f;
        root_opts.padding = ui::Edges::all(8);
        ui::LayoutNode* root = ui::VStack(arena, root_opts,
                                          {menu_bar, body});

        ui::LayoutFrame layout_frame{canvas, input, ctx, font,
                                     ui::active_theme(), dt,
                                     header_font ? &*header_font : nullptr};
        ui::run_frame(root, viewport, layout_frame);
        // Dropdown overlay: interacts NOW — before the edit handlers below
        // — so a selection made this frame is applied this frame. Drawn
        // here it also overlays every widget (recorded after them).
        ui::RunPopup(canvas, font, ui::active_theme(), ctx, input);

        // Menu-bar picks: each item routes through the SAME flag or frame
        // local its button/hotkey twin uses — one code path per action.
        if (menu_picks[0] >= 0) switch (menu_picks[0]) {
            case 0:
                if (frame_ui.open_clicked) *frame_ui.open_clicked = true;
                break;
            case 1: do_open_project = true; break;
            case 2: do_save = true; break;
            case 3: do_save_as = true; break;
            case 4:
                if (frame_ui.preset_import_clicked)
                    *frame_ui.preset_import_clicked = true;
                break;
            case 5:
                if (frame_ui.export_clicked)
                    *frame_ui.export_clicked = true;
                break;
        }
        if (menu_picks[1] >= 0) switch (menu_picks[1]) {
            case 0:
                if (frame_ui.undo_clicked) *frame_ui.undo_clicked = true;
                break;
            case 1:
                if (frame_ui.redo_clicked) *frame_ui.redo_clicked = true;
                break;
            case 2: do_duplicate = true; break;
            case 3: do_group = true; break;
            case 4: do_ungroup = true; break;
            case 5: do_select_all = true; break;
        }
        if (menu_picks[2] >= 0) switch (menu_picks[2]) {
            case 0: app.canvas_state.view_inited = false; break;
            case 1:
                // Find/jump popup — the Ctrl+F path.
                app.find_mode = true;
                app.fx_filter.clear();
                app.fx_search_focus = true;
                app.canvas_state.add_open = true;
                app.canvas_state.add_anchor = app.canvas_state.last_mouse;
                app.canvas_state.add_scroll = 0.0f;
                app.canvas_state.splice_from = 0;
                app.canvas_state.splice_to = 0;
                app.canvas_state.splice_port = 0;
                break;
            case 2:
                app.theme_index =
                    (app.theme_index + 1) % ui::theme_count();
                ui::set_active_theme(app.theme_index);
                save_ui_prefs(app);
                break;
            case 3: {
                // Cycle the viewport mask overlay — the M path.
                const auto& masks = app.document.masks;
                size_t cur = masks.size();
                for (size_t i = 0; i < masks.size(); ++i)
                    if (masks[i].id == app.overlay_mask_id) cur = i;
                const size_t next = cur + 1;
                app.overlay_mask_id =
                    next < masks.size()
                        ? masks[next].id
                        : (cur == masks.size() && !masks.empty()
                               ? masks[0].id
                               : 0);
                break;
            }
            case 4: app.ab_wipe = !app.ab_wipe; break;
            case 5: app.bypass_all = !app.bypass_all; break;
        }
        // Inspector tab switch.
        if (*tab_node) app.inspector_tab = 0;
        if (*tab_project) app.inspector_tab = 1;
        if (*tab_presets) app.inspector_tab = 2;

        // ---- apply staged UI edits as commands (before rendering, so the
        // frame reflects this frame's slider positions). Stack edits target
        // the layer the panel was BUILT for, not post-click selection.
        // Zero layers is valid: ui_layer is only consumed by handlers whose
        // widgets exist, but keep it in range regardless.
        const size_t ui_layer = app.document.layers.empty()
            ? 0
            : std::min(app.selected_layer, app.document.layers.size() - 1);
        bool did_break = false;

        // ---- bezier mask point editor (spec §8): live while the mask
        // overlay shows a shape mask. Left-drag moves the nearest point,
        // left-click on empty canvas appends one, right-click removes.
        // Handles + control polygon draw on the UI canvas over the video.
        if (app.overlay_mask_id != 0 && app.player.is_open() &&
            frame_ui.preview) {
            const doc::Mask* om =
                doc::find_mask(app.document, app.overlay_mask_id);
            auto decoded_now = app.player.current_frame();
            if (om && om->type == doc::MaskType::Shape && decoded_now) {
                const codec::FrameView v = decoded_now->view();
                const ui::Rect pr = frame_ui.preview->rect.inset(1.0f);
                const float aspect = static_cast<float>(v.width) /
                                     static_cast<float>(v.height);
                float fit_w = pr.w, fit_h = fit_w / aspect;
                if (fit_h > pr.h) {
                    fit_h = pr.h;
                    fit_w = fit_h * aspect;
                }
                const float left = pr.x + (pr.w - fit_w) * 0.5f;
                const float top = pr.y + (pr.h - fit_h) * 0.5f;
                const float ux = (input.mouse.x - left) / fit_w;
                const float uy = (input.mouse.y - top) / fit_h;
                const bool inside =
                    ux >= 0.0f && ux <= 1.0f && uy >= 0.0f && uy <= 1.0f;

                const size_t np = om->points.size() / 2;
                // Per-point keyframes (spec §8 "keyframable points"):
                // handles track the lane-resolved positions at the
                // playhead, and dragging a keyed point writes keyframes
                // there instead of moving the base shape.
                const double playhead = app.player.current_frame_index();
                const uint64_t okey = om->id | doc::kMaskParamBit;
                std::vector<float> disp = om->points;
                std::vector<bool> keyed(np, false);
                for (const doc::KeyframeLane& lane : app.document.lanes) {
                    if (lane.target.effect_id != okey || lane.keys.empty() ||
                        lane.target.param_index < doc::kMaskPointParamBase)
                        continue;
                    const size_t s = static_cast<size_t>(
                        lane.target.param_index - doc::kMaskPointParamBase);
                    if (s >= disp.size()) continue;
                    disp[s] = std::clamp(mod::eval_lane(lane, playhead),
                                         0.0f, 1.0f);
                    keyed[s / 2] = true;
                }
                auto nearest = [&]() {
                    int best = -1;
                    float best_d = 0.035f;
                    for (size_t i = 0; i < np; ++i) {
                        const float dx = disp[i * 2] - ux;
                        const float dy = disp[i * 2 + 1] - uy;
                        const float d = std::sqrt(dx * dx + dy * dy);
                        if (d < best_d) {
                            best_d = d;
                            best = static_cast<int>(i);
                        }
                    }
                    return best;
                };

                if (input.left_pressed() && !input.consumed && inside) {
                    const int idx = nearest();
                    if (idx >= 0) {
                        app.drag_point = idx;
                    } else {
                        doc::Mask m = *om;
                        m.points.push_back(ux);
                        m.points.push_back(uy);
                        app.undo.execute(app.document,
                                         doc::set_mask_params_command(m));
                        app.drag_point = static_cast<int>(np);
                    }
                }
                if (app.drag_point >= 0 && input.left_down() &&
                    static_cast<size_t>(app.drag_point) * 2 + 1 <
                        om->points.size()) {
                    const float nx = std::clamp(ux, 0.0f, 1.0f);
                    const float ny = std::clamp(uy, 0.0f, 1.0f);
                    const size_t pi = static_cast<size_t>(app.drag_point);
                    // keyed/disp predate a same-frame append; a fresh
                    // point is never keyed.
                    if (pi < keyed.size() && keyed[pi]) {
                        // Upsert x + y keys at the playhead atomically so
                        // the drag coalesces into one undo step.
                        if (disp[pi * 2] != nx || disp[pi * 2 + 1] != ny) {
                            std::vector<doc::KeyframeLane> lanes(2);
                            const float vals[2] = {nx, ny};
                            for (int a = 0; a < 2; ++a) {
                                doc::KeyframeLane& lane =
                                    lanes[static_cast<size_t>(a)];
                                lane.target = {
                                    okey, doc::kMaskPointParamBase +
                                              static_cast<int>(pi) * 2 + a};
                                for (const doc::KeyframeLane& l :
                                     app.document.lanes)
                                    if (l.target == lane.target)
                                        lane.keys = l.keys;
                                bool updated = false;
                                for (doc::Keyframe& k : lane.keys) {
                                    if (std::fabs(k.frame - playhead) < 0.5) {
                                        k.value = vals[a];
                                        updated = true;
                                        break;
                                    }
                                }
                                if (!updated) {
                                    doc::Keyframe k;
                                    k.frame = playhead;
                                    k.value = vals[a];
                                    lane.keys.push_back(k);
                                }
                            }
                            app.undo.execute(
                                app.document,
                                doc::set_lanes_command(std::move(lanes)),
                                /*coalesce=*/true);
                        }
                    } else if (om->points[pi * 2] != nx ||
                               om->points[pi * 2 + 1] != ny) {
                        doc::Mask m = *om;
                        m.points[pi * 2] = nx;
                        m.points[pi * 2 + 1] = ny;
                        app.undo.execute(app.document,
                                         doc::set_mask_params_command(m),
                                         /*coalesce=*/true);
                    }
                }
                if (input.left_released() && app.drag_point >= 0) {
                    app.drag_point = -1;
                    if (!did_break) {
                        app.undo.break_coalescing();
                        did_break = true;
                    }
                }
                if ((input.buttons_pressed & ui::kMouseRight) && inside) {
                    const int idx = nearest();
                    if (idx >= 0) {
                        app.undo.execute(
                            app.document,
                            doc::remove_mask_point_command(
                                om->id, static_cast<size_t>(idx)));
                        app.drag_point = -1;
                    }
                }

                // Handles: control polygon + point squares (amber = the
                // point has keyframe lanes).
                const ui::Color line_col{0.3f, 0.8f, 1.0f, 0.55f};
                const ui::Color pt_col{1.0f, 1.0f, 1.0f, 0.9f};
                const ui::Color key_col{1.0f, 0.72f, 0.25f, 0.95f};
                const size_t nn = disp.size() / 2;
                for (size_t i = 0; i < nn; ++i) {
                    const float x = left + disp[i * 2] * fit_w;
                    const float y = top + disp[i * 2 + 1] * fit_h;
                    if (nn >= 2) {
                        const size_t j = (i + 1) % nn;
                        const float x2 = left + disp[j * 2] * fit_w;
                        const float y2 = top + disp[j * 2 + 1] * fit_h;
                        canvas.draw_line({x, y}, {x2, y2}, 1.0f, line_col);
                    }
                    canvas.draw_rect({x - 3.0f, y - 3.0f, 6.0f, 6.0f},
                                     i < keyed.size() && keyed[i] ? key_col
                                                                  : pt_col);
                }
            }
        }
        for (const ParamStage& stage : frame_ui.params) {
            if (*stage.changed && *stage.staged != stage.original &&
                stage.layer_index < app.document.layers.size() &&
                stage.fx_index <
                    app.document.layers[stage.layer_index].stack.size()) {
                // KEYED params: the lane sets the base every frame, so a
                // slider drag must move the key at the playhead (auto-
                // key) — writing the base reads as a dead slider.
                const doc::ParamKey pk{
                    app.document.layers[stage.layer_index]
                        .stack[stage.fx_index]
                        .id,
                    stage.param_index};
                const doc::KeyframeLane* keyed_lane = nullptr;
                for (const doc::KeyframeLane& lane : app.document.lanes)
                    if (lane.target == pk && !lane.keys.empty())
                        keyed_lane = &lane;
                if (keyed_lane) {
                    const double ph = app.player.is_open()
                        ? app.player.current_frame_index()
                        : 0.0;
                    std::vector<doc::Keyframe> keys2 = keyed_lane->keys;
                    for (size_t k = 0; k < keys2.size(); ++k)
                        if (std::fabs(keys2[k].frame - ph) < 0.5) {
                            keys2.erase(keys2.begin() + k);
                            break;
                        }
                    doc::Keyframe nk;
                    nk.frame = ph;
                    nk.value = *stage.staged;
                    keys2.push_back(nk);
                    app.undo.execute(app.document,
                                     doc::set_lane_command(
                                         pk, std::move(keys2)),
                                     /*coalesce=*/true);
                } else {
                    app.undo.execute(
                        app.document,
                        doc::set_param_command(stage.layer_index,
                                               stage.fx_index,
                                               stage.param_index,
                                               *stage.staged),
                        /*coalesce=*/true);
                }
            }
            if (*stage.released && !did_break) {
                app.undo.break_coalescing();
                did_break = true;
            }
        }

        // Structural edits: at most one per frame keeps indices coherent.
        bool structure_done = false;
        for (const FxRowActions& row : frame_ui.rows) {
            if (structure_done) break;
            const size_t row_layer = row.layer_index;
            if (row_layer >= app.document.layers.size() ||
                row.fx_index >= app.document.layers[row_layer].stack.size())
                continue;
            if (*row.bypass_changed) {
                app.undo.execute(app.document,
                                 doc::set_bypass_command(row_layer,
                                                         row.fx_index,
                                                         *row.bypass_staged));
                structure_done = true;
            } else if (row.solo_changed && *row.solo_changed) {
                app.undo.execute(app.document,
                                 doc::set_solo_command(row_layer,
                                                       row.fx_index,
                                                       *row.solo_staged));
                structure_done = true;
            } else if (row.duplicate && *row.duplicate) {
                // Duplicate (spec §5): identical clone right below, with a
                // fresh id (and seed offset so "same settings" doesn't mean
                // "identical noise").
                doc::EffectInstance copy =
                    app.document.layers[row_layer].stack[row.fx_index];
                copy.id = app.document.next_effect_id++;
                copy.seed = copy.id;
                app.undo.execute(app.document,
                                 doc::add_effect_command(row_layer,
                                                         std::move(copy),
                                                         row.fx_index + 1));
                structure_done = true;
            } else if (*row.remove) {
                app.undo.execute(
                    app.document,
                    doc::remove_effect_command(row_layer, row.fx_index));
                structure_done = true;
            } else if (*row.up && row.fx_index > 0) {
                app.undo.execute(app.document,
                                 doc::move_effect_command(row_layer,
                                                          row.fx_index,
                                                          row.fx_index - 1));
                structure_done = true;
            } else if (*row.down &&
                       row.fx_index + 1 <
                           app.document.layers[row_layer].stack.size()) {
                app.undo.execute(app.document,
                                 doc::move_effect_command(row_layer,
                                                          row.fx_index,
                                                          row.fx_index + 1));
                structure_done = true;
            } else if (*row.group_toggle) {
                // "g": leave the group (dissolving it if now empty), join
                // the group above, or start a new group with the one above.
                auto& stack = app.document.layers[row_layer].stack;
                const doc::EffectInstance& fx = stack[row.fx_index];
                if (fx.group_id != 0) {
                    const uint64_t gid = fx.group_id;
                    app.undo.begin_group("Ungroup Effect");
                    app.undo.execute(app.document,
                                     doc::set_effect_group_command(
                                         row_layer, row.fx_index, 0));
                    bool any = false;
                    for (const doc::EffectInstance& e : stack)
                        any = any || e.group_id == gid;
                    if (!any)
                        app.undo.execute(app.document,
                                         doc::ungroup_command(row_layer,
                                                              gid));
                    app.undo.end_group();
                } else if (row.fx_index > 0) {
                    const uint64_t above = stack[row.fx_index - 1].group_id;
                    if (above != 0) {
                        app.undo.execute(app.document,
                                         doc::set_effect_group_command(
                                             row_layer, row.fx_index,
                                             above));
                    } else {
                        doc::Group g = doc::make_group(app.document, "group");
                        app.undo.execute(app.document,
                                         doc::group_effects_command(
                                             row_layer, std::move(g),
                                             row.fx_index - 1,
                                             row.fx_index));
                    }
                }
                structure_done = true;
            }
        }
        // ---- node canvas events (docs/flow_canvas.md): selection is view
        // state; moves stream through one coalesced command per gesture.
        {
            const flow::Output& fe = *flow_ui.events;
            auto tag_kind = [](uint64_t id) {
                return static_cast<flow::NodeKind>((id >> 56) - 1);
            };
            auto tag_doc = [](uint64_t id) {
                return id & 0x00FFFFFFFFFFFFFFull;
            };
            // Shared canvas-id helpers for the gesture handlers (move,
            // nudge, align, duplicate).
            auto node_ref_of = [&](uint64_t cid, doc::NodeRef* ref,
                                   uint64_t* rid) {
                if (cid == flow::kOutNodeId) {
                    *ref = doc::NodeRef::Output;
                    *rid = 0;
                    return true;
                }
                *rid = tag_doc(cid);
                switch (tag_kind(cid)) {
                    case flow::NodeKind::Source:
                        *ref = doc::NodeRef::Layer;
                        return true;
                    case flow::NodeKind::Effect:
                        *ref = doc::NodeRef::Effect;
                        return true;
                    case flow::NodeKind::Mask:
                        *ref = doc::NodeRef::Mask;
                        return true;
                    case flow::NodeKind::ModSource:
                        *ref = doc::NodeRef::Route;
                        return true;
                    case flow::NodeKind::Frame:
                        *ref = doc::NodeRef::Frame;
                        return true;
                    case flow::NodeKind::Group:
                        *ref = doc::NodeRef::Group;
                        return true;
                    case flow::NodeKind::GroupIn:
                        *ref = doc::NodeRef::GroupIn;
                        return true;
                    case flow::NodeKind::GroupOut:
                        *ref = doc::NodeRef::GroupOut;
                        return true;
                    default:
                        return false;
                }
            };
            auto node_pos_of = [&](uint64_t cid, float* x, float* y) {
                for (size_t i = 0; i < flow_ui.graph->node_count; ++i)
                    if (flow_ui.graph->nodes[i].id == cid) {
                        *x = flow_ui.graph->nodes[i].x;
                        *y = flow_ui.graph->nodes[i].y;
                        return true;
                    }
                return false;
            };

            // Ctrl+C / Ctrl+X (texed clipboard): copy the selection's
            // payloads plus the links AMONG copied effects; runs before
            // the delete handlers so cut copies first.
            if (do_copy && !app.multi_sel.empty()) {
                auto& cb = app.clipboard;
                cb = {};
                std::unordered_set<uint64_t> cb_fx;
                float ox = 1e9f, oy = 1e9f;
                for (const uint64_t cid : app.multi_sel) {
                    float nx = 0.0f, ny = 0.0f;
                    if (!node_pos_of(cid, &nx, &ny)) continue;
                    const uint64_t did = tag_doc(cid);
                    size_t li = 0, fi = 0;
                    switch (tag_kind(cid)) {
                        case flow::NodeKind::Effect:
                            if (find_effect_by_id(app.document, did, &li,
                                                  &fi)) {
                                doc::EffectInstance copy =
                                    app.document.layers[li].stack[fi];
                                copy.node_x = nx;
                                copy.node_y = ny;
                                cb.effects.push_back(std::move(copy));
                                cb_fx.insert(did);
                            }
                            break;
                        case flow::NodeKind::ModSource:
                            for (const doc::ModRoute& r :
                                 app.document.mod_routes)
                                if (r.id == did) {
                                    doc::ModRoute copy = r;
                                    copy.node_x = nx;
                                    copy.node_y = ny;
                                    cb.routes.push_back(copy);
                                }
                            break;
                        case flow::NodeKind::Mask:
                            for (const doc::Mask& m : app.document.masks)
                                if (m.id == did) {
                                    doc::Mask copy = m;
                                    copy.node_x = nx;
                                    copy.node_y = ny;
                                    cb.masks.push_back(std::move(copy));
                                }
                            break;
                        default:
                            break;
                    }
                    ox = std::min(ox, nx);
                    oy = std::min(oy, ny);
                }
                if (!cb.effects.empty() || !cb.routes.empty() ||
                    !cb.masks.empty()) {
                    const std::vector<doc::Document::NodeLink> all_links =
                        app.document.links.empty()
                            ? doc::synthesize_links(app.document)
                            : app.document.links;
                    for (const doc::Document::NodeLink& l : all_links)
                        if ((l.to_port == 0 || l.to_port == 2) &&
                            cb_fx.count(l.from) && cb_fx.count(l.to))
                            cb.links.push_back(l);
                    cb.origin_x = ox < 1e9f ? ox : 0.0f;
                    cb.origin_y = oy < 1e9f ? oy : 0.0f;
                    cb.valid = true;
                    app.status = do_cut ? "cut to clipboard"
                                        : "copied to clipboard";
                    if (do_cut) do_delete_sel = true;
                } else {
                    app.status = "nothing copyable selected";
                }
            }
            if (fe.clicked == flow::kOutNodeId) {
                app.sel = {SelKind::Output, 0};
            } else if (fe.clicked) {
                const uint64_t fdoc = tag_doc(fe.clicked);
                size_t li = 0, fi = 0;
                switch (tag_kind(fe.clicked)) {
                    case flow::NodeKind::Source: {
                        const int idx =
                            layer_index_by_id(app.document, fdoc);
                        if (idx >= 0) {
                            app.sel = {SelKind::LayerSource, fdoc};
                            app.selected_layer = static_cast<size_t>(idx);
                        }
                        break;
                    }
                    case flow::NodeKind::Effect:
                        if (find_effect_by_id(app.document, fdoc, &li,
                                              &fi)) {
                            app.sel = {SelKind::Effect, fdoc};
                            app.selected_layer = li;
                        }
                        break;
                    case flow::NodeKind::Mask:
                        app.sel = {SelKind::Mask, fdoc};
                        break;
                    case flow::NodeKind::ModSource:
                        app.sel = {SelKind::ModSource, fdoc};
                        break;
                    case flow::NodeKind::Group:
                        if (find_group_by_id(app.document, fdoc, &li)) {
                            app.sel = {SelKind::Group, fdoc};
                            app.selected_layer = li;
                        }
                        break;
                    case flow::NodeKind::GroupIn:
                    case flow::NodeKind::GroupOut:
                        // Boundary nodes have no output of their own —
                        // deselect so the big preview shows the
                        // composite (v5.4).
                        app.sel = {};
                        break;
                    default:
                        break;
                }
                if (app.sel.kind != SelKind::AddEffect)
                    app.insert_before_id = 0;
                // Multi set: shift toggles membership; a plain click on a
                // node ALREADY in the selection keeps the group (texed —
                // that's what makes grab-and-drag-the-selection work), on
                // anything else it replaces the set.
                if (fe.clicked_shift) {
                    auto it = std::find(app.multi_sel.begin(),
                                        app.multi_sel.end(), fe.clicked);
                    if (it != app.multi_sel.end())
                        app.multi_sel.erase(it);
                    else
                        app.multi_sel.push_back(fe.clicked);
                } else if (std::find(app.multi_sel.begin(),
                                     app.multi_sel.end(),
                                     fe.clicked) == app.multi_sel.end()) {
                    app.multi_sel.assign(1, fe.clicked);
                }
            }
            if (fe.clicked) {
                app.sel_wires.clear();
                // A canvas selection pulls the inspector to the node tab
                // so the click's context is what the panel shows.
                app.inspector_tab = 0;
            }
            if (fe.clicked_empty) {
                app.sel = {};
                app.insert_before_id = 0;
                app.multi_sel.clear();
                app.sel_wires.clear();
            }
            if (fe.wire_clicked) {
                // Wire selection (texed sel.links): exclusive with node
                // selection; Delete cuts the connection(s). Shift-click
                // TOGGLES the wire in the set (texed toggleLinkSelect).
                const flow::Wire w{fe.wire_from, fe.wire_to,
                                   fe.wire_kind};
                if (fe.wire_clicked_shift) {
                    auto it = std::find_if(
                        app.sel_wires.begin(), app.sel_wires.end(),
                        [&](const flow::Wire& s) {
                            return s.from == w.from && s.to == w.to &&
                                   s.kind == w.kind;
                        });
                    if (it != app.sel_wires.end())
                        app.sel_wires.erase(it);
                    else
                        app.sel_wires.push_back(w);
                } else {
                    app.sel_wires.assign(1, w);
                }
                app.sel = {};
                app.multi_sel.clear();
            }
            // Context-menu pick (texed node/frame menu dispatch): routes
            // through the same staged flags and frame locals the buttons
            // and hotkeys already use, so every item shares one code
            // path with its non-menu twin.
            uint64_t ctx_open_group = 0, ctx_frame_rename = 0;
            if (fe.ctx_pick >= 0 && flow_ui.ctx_actions &&
                fe.ctx_pick <
                    static_cast<int>(flow_ui.graph->ctx_count)) {
                const CtxAction act = flow_ui.ctx_actions[fe.ctx_pick];
                const uint64_t target = fe.ctx_node;
                const uint64_t did = tag_doc(target);
                switch (act) {
                    case CtxAction::Bypass:
                        for (size_t i = 0;
                             i < flow_ui.graph->node_count; ++i)
                            if (flow_ui.graph->nodes[i].id == target &&
                                flow_ui.graph->nodes[i].bypass_clicked)
                                *flow_ui.graph->nodes[i].bypass_clicked =
                                    true;
                        break;
                    case CtxAction::Duplicate:
                        do_duplicate = true;
                        break;
                    case CtxAction::Group:
                        do_group = true;
                        break;
                    case CtxAction::Ungroup:
                        do_ungroup = true;
                        break;
                    case CtxAction::OpenGroup:
                        ctx_open_group = target;
                        break;
                    case CtxAction::RenameGroup: {
                        app.group_rename_id = did;
                        app.group_rename_buf.clear();
                        size_t gli = 0;
                        if (find_group_by_id(app.document, did, &gli))
                            for (const doc::Group& gr :
                                 app.document.layers[gli].groups)
                                if (gr.id == did)
                                    app.group_rename_buf = gr.name;
                        break;
                    }
                    case CtxAction::SavePreset: {
                        size_t gli = 0;
                        if (!find_group_by_id(app.document, did, &gli))
                            break;
                        const doc::Group* group = nullptr;
                        for (const doc::Group& gr :
                             app.document.layers[gli].groups)
                            if (gr.id == did) group = &gr;
                        if (!group) break;
                        auto out_path = platform::show_save_dialog(
                            window.get(), {{"looks preset", "*.json"}},
                            (group->name.empty() ? std::string("preset")
                                                 : group->name) +
                                ".json");
                        if (!out_path) break;
                        if (out_path->extension() != ".json")
                            out_path->replace_extension(".json");
                        doc::Preset preset = doc::make_preset_from_group(
                            app.document, gli, did);
                        preset.name = out_path->stem().string();
                        if (preset.tags.empty())
                            preset.tags.push_back("user");
                        std::error_code ec;
                        std::filesystem::create_directories(
                            out_path->parent_path(), ec);
                        if (doc::save_preset(*out_path, preset)) {
                            app.status = "saved preset " +
                                         out_path->filename().string();
                            rescan_presets(app);
                        } else {
                            app.status = "preset save failed";
                        }
                        break;
                    }
                    case CtxAction::AlignLeft:
                    case CtxAction::AlignTop:
                    case CtxAction::SpreadH:
                    case CtxAction::SpreadV: {
                        const int a =
                            static_cast<int>(act) -
                            static_cast<int>(CtxAction::AlignLeft);
                        if (frame_ui.align_clicked[a])
                            *frame_ui.align_clicked[a] = true;
                        break;
                    }
                    case CtxAction::Delete:
                        do_delete_sel = true;
                        break;
                    case CtxAction::Export:
                        if (frame_ui.export_clicked)
                            *frame_ui.export_clicked = true;
                        break;
                    case CtxAction::RenameFrame:
                        ctx_frame_rename = did;
                        break;
                    case CtxAction::FrameColor:
                    case CtxAction::DeleteFrame:
                        for (size_t f = 0;
                             f < flow_ui.graph->frame_count; ++f)
                            if (flow_ui.graph->frames[f].id == did) {
                                bool* flag =
                                    act == CtxAction::FrameColor
                                        ? flow_ui.graph->frames[f]
                                              .color_clicked
                                        : flow_ui.graph->frames[f]
                                              .remove_clicked;
                                if (flag) *flag = true;
                            }
                        break;
                }
            }
            if (fe.marquee_done) {
                // Every card intersecting the marquee joins the set, plus
                // every wire the canvas sampled crossing the rect.
                app.multi_sel.clear();
                for (size_t i = 0; i < flow_ui.graph->node_count; ++i) {
                    const flow::Node& nd = flow_ui.graph->nodes[i];
                    const float nw = flow::node_width();
                    const float nh = flow::node_height_of(nd);
                    if (nd.x < fe.mq_x1 && nd.x + nw > fe.mq_x0 &&
                        nd.y < fe.mq_y1 && nd.y + nh > fe.mq_y0)
                        app.multi_sel.push_back(nd.id);
                }
                app.sel_wires.assign(fe.mq_wires,
                                     fe.mq_wires + fe.mq_wire_count);
            }
            if (fe.moved) {
                auto ref_of = [&](uint64_t cid, doc::NodeRef* ref,
                                  uint64_t* rid) {
                    if (cid == flow::kOutNodeId) {
                        *ref = doc::NodeRef::Output;
                        *rid = 0;
                        return true;
                    }
                    *rid = tag_doc(cid);
                    switch (tag_kind(cid)) {
                        case flow::NodeKind::Source:
                            *ref = doc::NodeRef::Layer;
                            return true;
                        case flow::NodeKind::Effect:
                            *ref = doc::NodeRef::Effect;
                            return true;
                        case flow::NodeKind::Mask:
                            *ref = doc::NodeRef::Mask;
                            return true;
                        case flow::NodeKind::ModSource:
                            *ref = doc::NodeRef::Route;
                            return true;
                        case flow::NodeKind::Frame:
                            *ref = doc::NodeRef::Frame;
                            return true;
                        case flow::NodeKind::Group:
                            *ref = doc::NodeRef::Group;
                            return true;
                        case flow::NodeKind::GroupIn:
                            *ref = doc::NodeRef::GroupIn;
                            return true;
                        case flow::NodeKind::GroupOut:
                            *ref = doc::NodeRef::GroupOut;
                            return true;
                        default:
                            return false;
                    }
                };
                auto graph_pos = [&](uint64_t cid, float* x, float* y) {
                    for (size_t i = 0; i < flow_ui.graph->node_count; ++i)
                        if (flow_ui.graph->nodes[i].id == cid) {
                            *x = flow_ui.graph->nodes[i].x;
                            *y = flow_ui.graph->nodes[i].y;
                            return true;
                        }
                    return false;
                };
                // Dropped positions land on whole graph units (texed
                // rounds world px) so hand-laid graphs stay crisp.
                const float mvx = std::round(fe.moved_x);
                const float mvy = std::round(fe.moved_y);
                // Frame drags carry their contents (texed): every card
                // whose center sits inside the frame moves with it —
                // unless alt is held (frame moves alone).
                if (fe.moved != flow::kOutNodeId &&
                    tag_kind(fe.moved) == flow::NodeKind::Frame) {
                    const uint64_t fid = tag_doc(fe.moved);
                    const doc::Document::Frame* fr = nullptr;
                    for (const doc::Document::Frame& f :
                         app.document.frames)
                        if (f.id == fid) fr = &f;
                    if (fr) {
                        const float dx = mvx - fr->x;
                        const float dy = mvy - fr->y;
                        for (size_t i = 0;
                             !fe.moved_alt &&
                             i < flow_ui.graph->node_count; ++i) {
                            const flow::Node& nd =
                                flow_ui.graph->nodes[i];
                            const float ncx =
                                nd.x + flow::node_width() * 0.5f;
                            const float ncy =
                                nd.y +
                                flow::node_height_of(nd) * 0.5f;
                            if (ncx < fr->x || ncx > fr->x + fr->w ||
                                ncy < fr->y || ncy > fr->y + fr->h)
                                continue;
                            doc::NodeRef nref;
                            uint64_t nrid = 0;
                            if (!ref_of(nd.id, &nref, &nrid)) continue;
                            app.undo.execute(
                                app.document,
                                doc::set_node_pos_command(
                                    nref, nrid, nd.x + dx, nd.y + dy),
                                /*coalesce=*/true);
                        }
                        app.undo.execute(app.document,
                                         doc::set_node_pos_command(
                                             doc::NodeRef::Frame, fid,
                                             mvx, mvy),
                                         /*coalesce=*/true);
                    }
                } else {
                doc::NodeRef ref;
                uint64_t rid = 0;
                float px = 0.0f, py = 0.0f;
                const bool grouped_move =
                    app.multi_sel.size() > 1 &&
                    std::find(app.multi_sel.begin(), app.multi_sel.end(),
                              fe.moved) != app.multi_sel.end() &&
                    graph_pos(fe.moved, &px, &py);
                if (grouped_move) {
                    // Group move (texed): the drag delta applies to every
                    // selected node — auto-laid nodes commit where they
                    // sat, one coalesced command per node per gesture.
                    const float dx = mvx - px;
                    const float dy = mvy - py;
                    for (const uint64_t cid : app.multi_sel) {
                        float mx = 0.0f, my = 0.0f;
                        if (!ref_of(cid, &ref, &rid)) continue;
                        if (!graph_pos(cid, &mx, &my)) continue;
                        app.undo.execute(app.document,
                                         doc::set_node_pos_command(
                                             ref, rid, mx + dx, my + dy),
                                         /*coalesce=*/true);
                    }
                } else if (ref_of(fe.moved, &ref, &rid)) {
                    app.undo.execute(app.document,
                                     doc::set_node_pos_command(
                                         ref, rid, mvx, mvy),
                                     /*coalesce=*/true);
                }
                }
            }
            if (fe.move_released && !did_break) {
                app.undo.break_coalescing();
                did_break = true;
            }
            // Wire edits (docs/flow_canvas.md v3): port 0 goes through the
            // link commands with the cycle guard; port 1 (mask) maps onto
            // the mask_id fields — their existing commands and UI stay the
            // single source of truth.
            if ((fe.connect_requested || fe.disconnect_requested) &&
                !structure_done) {
                auto doc_id_of = [&](uint64_t cid) {
                    return cid == flow::kOutNodeId ? 0ull : tag_doc(cid);
                };
                // Group cards proxy their BOUNDARY members (texed: the
                // subgraph node IS its boundary): the card's In is the
                // first member's In, its Out the last member's out. The
                // scoped view's In/Out boundary nodes rewire the
                // boundary links themselves (handled below).
                auto is_kind = [&](uint64_t cid, flow::NodeKind k) {
                    return cid != 0 && cid != flow::kOutNodeId &&
                           tag_kind(cid) == k;
                };
                auto group_member = [&](uint64_t cid,
                                        bool last) -> uint64_t {
                    const uint64_t gid = tag_doc(cid);
                    size_t gli = 0;
                    if (!find_group_by_id(app.document, gid, &gli))
                        return 0;
                    uint64_t first = 0, last_id = 0, bind = 0;
                    const doc::Group* gr2 = nullptr;
                    for (const doc::Group& g :
                         app.document.layers[gli].groups)
                        if (g.id == gid) gr2 = &g;
                    for (const doc::EffectInstance& e :
                         app.document.layers[gli].stack)
                        if (e.group_id == gid) {
                            if (!first) first = e.id;
                            last_id = e.id;
                            // The boundary BINDINGS win when they name a
                            // live member (v5.3 intermediaries).
                            if (gr2 &&
                                e.id == (last ? gr2->face_out
                                              : gr2->face_in))
                                bind = e.id;
                        }
                    return bind ? bind : (last ? last_id : first);
                };
                auto resolve_src = [&](uint64_t cid) -> uint64_t {
                    return is_kind(cid, flow::NodeKind::Group)
                               ? group_member(cid, /*last=*/true)
                               : doc_id_of(cid);
                };
                auto resolve_dst = [&](uint64_t cid) -> uint64_t {
                    return is_kind(cid, flow::NodeKind::Group)
                               ? group_member(cid, /*last=*/false)
                               : doc_id_of(cid);
                };
                // Members of the OPEN group, for boundary-link lookups.
                std::unordered_set<uint64_t> scope_members;
                if (app.open_group)
                    for (const doc::Layer& sl : app.document.layers)
                        for (const doc::EffectInstance& e : sl.stack)
                            if (e.group_id == app.open_group)
                                scope_members.insert(e.id);
                const auto boundary_links = [&]() {
                    return app.document.links.empty()
                               ? doc::synthesize_links(app.document)
                               : app.document.links;
                };
                const bool boundary_edit =
                    is_kind(fe.connect_from, flow::NodeKind::GroupIn) ||
                    is_kind(fe.connect_to, flow::NodeKind::GroupOut) ||
                    is_kind(fe.disconnect_from,
                            flow::NodeKind::GroupIn) ||
                    is_kind(fe.disconnect_to, flow::NodeKind::GroupOut);
                if (boundary_edit) {
                    // Boundary edits from inside the scoped view (v5.3
                    // intermediaries): rewiring In/Out RETARGETS the
                    // persistent binding; any outer links follow it.
                    // Unplugging a boundary wire cuts only the OUTER
                    // link — the internal picture never changes from
                    // outside edits, and vice versa.
                    size_t bgli = 0;
                    const doc::Group* bgroup = nullptr;
                    if (find_group_by_id(app.document, app.open_group,
                                         &bgli))
                        for (const doc::Group& g :
                             app.document.layers[bgli].groups)
                            if (g.id == app.open_group) bgroup = &g;
                    app.undo.begin_group("Rewire Boundary");
                    if (is_kind(fe.disconnect_from,
                                flow::NodeKind::GroupIn) &&
                        !fe.connect_requested) {
                        for (const doc::Document::NodeLink& l :
                             boundary_links())
                            if (l.to_port == 0 &&
                                scope_members.count(l.to) &&
                                !scope_members.count(l.from)) {
                                app.undo.execute(
                                    app.document,
                                    doc::disconnect_command(l));
                                break;
                            }
                    } else if (is_kind(fe.disconnect_to,
                                       flow::NodeKind::GroupOut) &&
                               !fe.connect_requested) {
                        for (const doc::Document::NodeLink& l :
                             boundary_links())
                            if (l.to_port == 0 &&
                                scope_members.count(l.from) &&
                                !scope_members.count(l.to)) {
                                app.undo.execute(
                                    app.document,
                                    doc::disconnect_command(l));
                                break;
                            }
                    }
                    if (bgroup &&
                        is_kind(fe.connect_from,
                                flow::NodeKind::GroupIn) &&
                        fe.connect_port == 0) {
                        const uint64_t to2 = resolve_dst(fe.connect_to);
                        if (to2 && scope_members.count(to2)) {
                            doc::Group edited = *bgroup;
                            edited.face_in = to2;
                            app.undo.execute(
                                app.document,
                                doc::set_group_props_command(bgli,
                                                             edited));
                            // The outer producer's link follows.
                            for (const doc::Document::NodeLink& l :
                                 boundary_links())
                                if (l.to_port == 0 &&
                                    scope_members.count(l.to) &&
                                    !scope_members.count(l.from)) {
                                    app.undo.execute(
                                        app.document,
                                        doc::disconnect_command(l));
                                    app.undo.execute(
                                        app.document,
                                        doc::connect_command(
                                            {l.from, to2, 0}));
                                    break;
                                }
                        }
                    } else if (bgroup &&
                               is_kind(fe.connect_to,
                                       flow::NodeKind::GroupOut) &&
                               fe.connect_port == 0) {
                        const uint64_t from2 =
                            resolve_src(fe.connect_from);
                        if (from2 && scope_members.count(from2)) {
                            doc::Group edited = *bgroup;
                            edited.face_out = from2;
                            app.undo.execute(
                                app.document,
                                doc::set_group_props_command(bgli,
                                                             edited));
                            // Every outer consumer's link follows (the
                            // composite link included).
                            for (const doc::Document::NodeLink& l :
                                 boundary_links())
                                if (l.to_port == 0 &&
                                    scope_members.count(l.from) &&
                                    !scope_members.count(l.to)) {
                                    app.undo.execute(
                                        app.document,
                                        doc::disconnect_command(l));
                                    app.undo.execute(
                                        app.document,
                                        doc::connect_command(
                                            {from2, l.to, 0}));
                                }
                        }
                    }
                    app.undo.end_group();
                    structure_done = true;
                } else {
                const bool grouped =
                    fe.connect_requested && fe.disconnect_requested;
                if (grouped) app.undo.begin_group("Rewire");
                if (fe.disconnect_requested) {
                    const uint64_t tdoc = doc_id_of(fe.disconnect_to);
                    size_t li = 0, fi = 0;
                    if (fe.disconnect_port == 1 &&
                        tag_kind(fe.disconnect_from) !=
                            flow::NodeKind::Mask) {
                        // Image matte (v5.2): a plain port-1 link cut.
                        app.undo.execute(
                            app.document,
                            doc::disconnect_command(
                                {resolve_src(fe.disconnect_from), tdoc,
                                 1}));
                    } else if (fe.disconnect_port == 1) {
                        if (tag_kind(fe.disconnect_to) ==
                                flow::NodeKind::Effect &&
                            find_effect_by_id(app.document, tdoc, &li,
                                              &fi)) {
                            app.undo.execute(app.document,
                                             doc::set_effect_mask_command(
                                                 li, fi, 0));
                        } else if (tag_kind(fe.disconnect_to) ==
                                   flow::NodeKind::Source) {
                            const int idx =
                                layer_index_by_id(app.document, tdoc);
                            if (idx >= 0) {
                                doc::Layer edited = app.document.layers
                                    [static_cast<size_t>(idx)];
                                edited.mask_id = 0;
                                app.undo.execute(
                                    app.document,
                                    doc::set_layer_props_command(
                                        std::move(edited)));
                            }
                        }
                    } else if (fe.disconnect_to != flow::kOutNodeId &&
                               tag_kind(fe.disconnect_to) ==
                                   flow::NodeKind::Mask) {
                        // In port of a mask card = its image source
                        // (mask.source_layer_id, not the link table).
                        if (const doc::Mask* m =
                                doc::find_mask(app.document, tdoc)) {
                            doc::Mask edited = *m;
                            edited.source_layer_id = 0;
                            app.undo.execute(
                                app.document,
                                doc::set_mask_params_command(
                                    std::move(edited)));
                        }
                    } else {
                        app.undo.execute(
                            app.document,
                            doc::disconnect_command(
                                {resolve_src(fe.disconnect_from),
                                 resolve_dst(fe.disconnect_to),
                                 fe.disconnect_port}));
                    }
                }
                if (fe.connect_requested) {
                    const uint64_t fdoc2 = doc_id_of(fe.connect_from);
                    const uint64_t tdoc2 = doc_id_of(fe.connect_to);
                    size_t li = 0, fi = 0;
                    if (fe.connect_port == 1) {
                        if (tag_kind(fe.connect_from) ==
                            flow::NodeKind::Mask) {
                            if (tag_kind(fe.connect_to) ==
                                    flow::NodeKind::Effect &&
                                find_effect_by_id(app.document, tdoc2, &li,
                                                  &fi)) {
                                app.undo.execute(
                                    app.document,
                                    doc::set_effect_mask_command(li, fi,
                                                                 fdoc2));
                            } else if (tag_kind(fe.connect_to) ==
                                       flow::NodeKind::Source) {
                                const int idx = layer_index_by_id(
                                    app.document, tdoc2);
                                if (idx >= 0) {
                                    doc::Layer edited = app.document.layers
                                        [static_cast<size_t>(idx)];
                                    edited.mask_id = fdoc2;
                                    app.undo.execute(
                                        app.document,
                                        doc::set_layer_props_command(
                                            std::move(edited)));
                                }
                            }
                        } else if (tag_kind(fe.connect_from) ==
                                       flow::NodeKind::Source ||
                                   tag_kind(fe.connect_from) ==
                                       flow::NodeKind::Effect ||
                                   tag_kind(fe.connect_from) ==
                                       flow::NodeKind::Group) {
                            // v5.2 masks ARE images: the matte anchor is
                            // a plain port-1 image link — the engine
                            // reads the wired image's luma as the gate.
                            // The legacy mask_id entity clears so one
                            // matte rules the consumer.
                            const uint64_t rf =
                                resolve_src(fe.connect_from);
                            if (!rf) {
                                app.status = "that group has no members";
                            } else if (doc::link_would_cycle(
                                           app.document, rf, tdoc2)) {
                                app.status =
                                    "refused: that matte would loop";
                            } else {
                                const bool own_group = !grouped;
                                if (own_group)
                                    app.undo.begin_group("Wire Matte");
                                app.undo.execute(
                                    app.document,
                                    doc::connect_command({rf, tdoc2, 1}));
                                if (tag_kind(fe.connect_to) ==
                                        flow::NodeKind::Effect &&
                                    find_effect_by_id(app.document, tdoc2,
                                                      &li, &fi) &&
                                    app.document.layers[li]
                                            .stack[fi].mask_id != 0) {
                                    app.undo.execute(
                                        app.document,
                                        doc::set_effect_mask_command(
                                            li, fi, 0));
                                } else if (tag_kind(fe.connect_to) ==
                                           flow::NodeKind::Source) {
                                    const int idx = layer_index_by_id(
                                        app.document, tdoc2);
                                    if (idx >= 0 &&
                                        app.document
                                            .layers[static_cast<size_t>(
                                                idx)]
                                            .mask_id != 0) {
                                        doc::Layer edited =
                                            app.document.layers
                                                [static_cast<size_t>(
                                                    idx)];
                                        edited.mask_id = 0;
                                        app.undo.execute(
                                            app.document,
                                            doc::set_layer_props_command(
                                                std::move(edited)));
                                    }
                                }
                                if (own_group) app.undo.end_group();
                            }
                        }
                    } else if (fe.connect_to != flow::kOutNodeId &&
                               tag_kind(fe.connect_to) ==
                                   flow::NodeKind::Mask) {
                        // In port of a mask card: retarget its image
                        // source. Pre-stack sources only (spec §8 — post-
                        // stack would allow cycles).
                        if (tag_kind(fe.connect_from) ==
                            flow::NodeKind::Source) {
                            if (const doc::Mask* m = doc::find_mask(
                                    app.document, tdoc2)) {
                                doc::Mask edited = *m;
                                edited.source_layer_id = fdoc2;
                                edited.source_gen = 0;
                                edited.source_path.clear();
                                app.undo.execute(
                                    app.document,
                                    doc::set_mask_params_command(
                                        std::move(edited)));
                            }
                        } else {
                            app.status = "masks sample SOURCE nodes "
                                         "(pre-stack, no cycles)";
                        }
                    } else if (tag_kind(fe.connect_from) !=
                                   flow::NodeKind::Source &&
                               tag_kind(fe.connect_from) !=
                                   flow::NodeKind::Effect &&
                               tag_kind(fe.connect_from) !=
                                   flow::NodeKind::Group) {
                        app.status = "only image nodes feed In ports";
                    } else {
                        // Group cards resolve to their boundary members
                        // before the link edit + cycle guard.
                        const uint64_t rf = resolve_src(fe.connect_from);
                        const uint64_t rt = resolve_dst(fe.connect_to);
                        if (!rf || (fe.connect_to != flow::kOutNodeId &&
                                    !rt &&
                                    is_kind(fe.connect_to,
                                            flow::NodeKind::Group))) {
                            app.status = "that group has no members";
                        } else if (doc::link_would_cycle(app.document,
                                                         rf, rt)) {
                            app.status =
                                "refused: that connection would loop";
                        } else {
                            app.undo.execute(app.document,
                                             doc::connect_command(
                                                 {rf, rt,
                                                  fe.connect_port}));
                        }
                    }
                }
                if (grouped) app.undo.end_group();
                structure_done = true;
                }
            }

            // Splice-on-drop (texed): a single unfed effect card released
            // over a wire splices into it, cycle-guarded on both new
            // connections.
            if (fe.node_splice_requested && !structure_done &&
                tag_kind(fe.splice_node) == flow::NodeKind::Effect) {
                const uint64_t nid = tag_doc(fe.splice_node);
                // Group wire ends resolve to their boundary members —
                // the link table stores members, not the card.
                auto splice_end = [&](uint64_t cid,
                                      bool is_from) -> uint64_t {
                    if (cid == flow::kOutNodeId) return 0;
                    if (tag_kind(cid) != flow::NodeKind::Group)
                        return tag_doc(cid);
                    const uint64_t gid = tag_doc(cid);
                    size_t gli = 0;
                    if (!find_group_by_id(app.document, gid, &gli))
                        return 0;
                    uint64_t first = 0, last_id = 0, bind = 0;
                    const doc::Group* gr2 = nullptr;
                    for (const doc::Group& g :
                         app.document.layers[gli].groups)
                        if (g.id == gid) gr2 = &g;
                    for (const doc::EffectInstance& e :
                         app.document.layers[gli].stack)
                        if (e.group_id == gid) {
                            if (!first) first = e.id;
                            last_id = e.id;
                            if (gr2 &&
                                e.id == (is_from ? gr2->face_out
                                                 : gr2->face_in))
                                bind = e.id;
                        }
                    return bind ? bind : (is_from ? last_id : first);
                };
                auto bkind = [&](uint64_t cid) {
                    return cid != flow::kOutNodeId &&
                           (tag_kind(cid) == flow::NodeKind::GroupIn ||
                            tag_kind(cid) == flow::NodeKind::GroupOut);
                };
                const uint64_t wf =
                    splice_end(fe.splice_wire_from, true);
                const uint64_t wt = splice_end(fe.splice_wire_to, false);
                if (bkind(fe.splice_wire_from) ||
                    bkind(fe.splice_wire_to)) {
                    app.status =
                        "boundary wires re-route via the in/out nodes";
                } else if (!wf) {
                    app.status = "that group has no members";
                } else if (doc::link_would_cycle(app.document, wf, nid) ||
                           doc::link_would_cycle(app.document, nid, wt)) {
                    app.status = "refused: that splice would loop";
                } else {
                    app.undo.begin_group("Splice Node");
                    if (app.document.links.empty())
                        app.undo.execute(
                            app.document,
                            doc::disconnect_command({0, 0, 9999}));
                    app.undo.execute(app.document,
                                     doc::disconnect_command(
                                         {wf, wt, fe.splice_wire_port}));
                    app.undo.execute(app.document,
                                     doc::connect_command({wf, nid, 0}));
                    app.undo.execute(
                        app.document,
                        doc::connect_command(
                            {nid, wt, fe.splice_wire_port}));
                    app.undo.end_group();
                    structure_done = true;
                }
            }

            // Value-node wiring (v4): a ModSource out wire dropped on an
            // effect card's param row retargets that route. Row 0 = wet,
            // 1 = opacity, 2+p = params — the card build order. Rows past
            // the param span (the Text card's string row, v5.6) are not
            // mod targets.
            if (fe.route_drop_requested && !structure_done &&
                tag_kind(fe.route_drop_from) == flow::NodeKind::ModSource &&
                fe.route_drop_row >= 0) {
                const uint64_t rid = tag_doc(fe.route_drop_from);
                if (tag_kind(fe.route_drop_to) == flow::NodeKind::Effect) {
                    const int pi = fe.route_drop_row == 0
                        ? doc::kWetParam
                        : fe.route_drop_row == 1
                            ? doc::kOpacityParam
                            : fe.route_drop_row - 2;
                    size_t rli = 0, rfi = 0;
                    const bool param_ok =
                        pi < 0 ||
                        (find_effect_by_id(app.document,
                                           tag_doc(fe.route_drop_to), &rli,
                                           &rfi) &&
                         pi < static_cast<int>(
                                  doc::effect_info(
                                      app.document.layers[rli]
                                          .stack[rfi]
                                          .type)
                                      .param_count));
                    if (param_ok) {
                        app.undo.execute(
                            app.document,
                            doc::set_route_target_command(
                                rid, {tag_doc(fe.route_drop_to), pi}));
                        structure_done = true;
                    }
                } else if (tag_kind(fe.route_drop_to) ==
                           flow::NodeKind::Group) {
                    // Face rows are member-param ALIASES (v5.3): the
                    // drop retargets onto the row'th valid exposed key.
                    const uint64_t gid = tag_doc(fe.route_drop_to);
                    size_t gli = 0;
                    if (find_group_by_id(app.document, gid, &gli)) {
                        const doc::Layer& gl = app.document.layers[gli];
                        for (const doc::Group& gr : gl.groups) {
                            if (gr.id != gid) continue;
                            int vrow = -1;
                            for (const doc::ParamKey& k : gr.exposed) {
                                bool member = false;
                                for (const doc::EffectInstance& e :
                                     gl.stack)
                                    member = member || e.id == k.effect_id;
                                if (!member) continue;
                                if (++vrow == fe.route_drop_row) {
                                    app.undo.execute(
                                        app.document,
                                        doc::set_route_target_command(
                                            rid, k));
                                    structure_done = true;
                                    break;
                                }
                            }
                            break;
                        }
                    }
                }
            }

            // Frame corner resize: streamed as one coalesced command per
            // gesture; rename opens the inline title edit.
            if (fe.frame_resized)
                app.undo.execute(app.document,
                                 doc::set_frame_bounds_command(
                                     fe.frame_resized, fe.frame_w,
                                     fe.frame_h),
                                 /*coalesce=*/true);
            if (fe.frame_resize_released && !did_break) {
                app.undo.break_coalescing();
                did_break = true;
            }
            const uint64_t frame_rename_req =
                fe.frame_rename ? fe.frame_rename : ctx_frame_rename;
            if (frame_rename_req) {
                app.frame_rename_id = frame_rename_req;
                app.frame_rename_buf.clear();
                for (const doc::Document::Frame& f : app.document.frames)
                    if (f.id == frame_rename_req)
                        app.frame_rename_buf = f.title;
            }
            // Group card title double-click: inline rename (texed
            // subgraph rename), prefilled with the current name.
            if (fe.group_rename) {
                const uint64_t gid = tag_doc(fe.group_rename);
                app.group_rename_id = gid;
                app.group_rename_buf.clear();
                size_t gli = 0;
                if (find_group_by_id(app.document, gid, &gli))
                    for (const doc::Group& gr :
                         app.document.layers[gli].groups)
                        if (gr.id == gid) app.group_rename_buf = gr.name;
            }
            // Text card title double-click: edit its string through the
            // shared inline editor (v5.5).
            if (fe.text_edit) {
                const uint64_t tid = tag_doc(fe.text_edit);
                app.text_edit_id = tid;
                app.text_edit_buf.clear();
                size_t tli = 0, tfi = 0;
                if (find_effect_by_id(app.document, tid, &tli, &tfi))
                    app.text_edit_buf =
                        app.document.layers[tli].stack[tfi].text;
            }
            // Frame colour dot: cycles none → palette hues → none.
            for (size_t f = 0; f < flow_ui.graph->frame_count; ++f) {
                const flow::FrameBox& fb = flow_ui.graph->frames[f];
                if (fb.color_clicked && *fb.color_clicked) {
                    app.undo.execute(app.document,
                                     doc::set_frame_color_command(
                                         fb.id, (fb.color + 1) % 9));
                    break;
                }
            }

            // Inline value edit: double-click opened it — prefill with
            // the current value; Enter commits into the row's staged
            // slot so the existing appliers do the rest.
            if (fe.value_edit_node) {
                app.value_edit_node = fe.value_edit_node;
                app.value_edit_row = fe.value_edit_row;
                app.value_edit_buf.clear();
                for (size_t i = 0; i < flow_ui.graph->node_count; ++i) {
                    const flow::Node& nd = flow_ui.graph->nodes[i];
                    if (nd.id != fe.value_edit_node) continue;
                    if (fe.value_edit_row >= 0 &&
                        fe.value_edit_row < nd.row_count &&
                        nd.rows[fe.value_edit_row].staged) {
                        char vb[32];
                        std::snprintf(
                            vb, sizeof(vb), "%.4g",
                            static_cast<double>(
                                *nd.rows[fe.value_edit_row].staged));
                        app.value_edit_buf = vb;
                    }
                    break;
                }
            }
            // Click-away commits the open value editor (texed blur
            // commit) — any left press that didn't OPEN it this frame.
            if (app.value_edit_node && !fe.value_edit_node &&
                input.left_pressed())
                app.value_commit = true;
            if (app.value_commit) {
                app.value_commit = false;
                for (size_t i = 0; i < flow_ui.graph->node_count; ++i) {
                    const flow::Node& nd = flow_ui.graph->nodes[i];
                    if (nd.id != app.value_edit_node) continue;
                    if (app.value_edit_row >= 0 &&
                        app.value_edit_row < nd.row_count &&
                        nd.rows[app.value_edit_row].staged &&
                        !app.value_edit_buf.empty()) {
                        const flow::ParamRow& row =
                            nd.rows[app.value_edit_row];
                        const float v = std::clamp(
                            static_cast<float>(
                                std::atof(app.value_edit_buf.c_str())),
                            row.min_v, row.max_v);
                        // Effect rows commit DIRECTLY: the generic staged
                        // path is applied before this handler runs, so a
                        // staged write here evaporated on the next frame
                        // (typed values "reverted"). Keyed params move
                        // the key at the playhead; the rest set the
                        // param. Other card kinds' appliers run later —
                        // their staged writes still land.
                        bool applied = false;
                        // Group face rows are member-param ALIASES
                        // (v5.3): resolve the row to its exposed key so
                        // typed values commit through the same direct
                        // path as effect rows.
                        doc::ParamKey face_key{0, 0};
                        bool face = false;
                        if (tag_kind(nd.id) == flow::NodeKind::Group) {
                            const uint64_t gid = tag_doc(nd.id);
                            size_t gli = 0;
                            if (find_group_by_id(app.document, gid,
                                                 &gli)) {
                                const doc::Layer& gl =
                                    app.document.layers[gli];
                                for (const doc::Group& gr : gl.groups) {
                                    if (gr.id != gid) continue;
                                    int vrow = -1;
                                    for (const doc::ParamKey& k :
                                         gr.exposed) {
                                        bool member = false;
                                        for (const doc::EffectInstance&
                                                 e : gl.stack)
                                            member = member ||
                                                     e.id == k.effect_id;
                                        if (!member) continue;
                                        if (++vrow ==
                                            app.value_edit_row) {
                                            face_key = k;
                                            face = true;
                                            break;
                                        }
                                    }
                                    break;
                                }
                            }
                        }
                        if (tag_kind(nd.id) == flow::NodeKind::Effect ||
                            face) {
                            const int pi = face ? face_key.param_index
                                : app.value_edit_row == 0
                                ? doc::kWetParam
                                : app.value_edit_row == 1
                                    ? doc::kOpacityParam
                                    : app.value_edit_row - 2;
                            const doc::ParamKey pk =
                                face ? face_key
                                     : doc::ParamKey{tag_doc(nd.id), pi};
                            std::vector<doc::Keyframe> keys2;
                            bool keyed = false;
                            for (const doc::KeyframeLane& lane :
                                 app.document.lanes)
                                if (lane.target == pk &&
                                    !lane.keys.empty()) {
                                    keyed = true;
                                    keys2 = lane.keys;
                                }
                            size_t li = 0, fi = 0;
                            if (keyed) {
                                const double ph = app.player.is_open()
                                    ? app.player.current_frame_index()
                                    : 0.0;
                                for (size_t k = 0; k < keys2.size(); ++k)
                                    if (std::fabs(keys2[k].frame - ph) <
                                        0.5) {
                                        keys2.erase(keys2.begin() + k);
                                        break;
                                    }
                                doc::Keyframe nk;
                                nk.frame = ph;
                                nk.value = v;
                                keys2.push_back(nk);
                                app.undo.execute(
                                    app.document,
                                    doc::set_lane_command(
                                        pk, std::move(keys2)));
                                applied = true;
                            } else if (find_effect_by_id(app.document,
                                                         pk.effect_id,
                                                         &li, &fi)) {
                                app.undo.execute(
                                    app.document,
                                    doc::set_param_command(li, fi, pi,
                                                           v));
                                applied = true;
                            }
                        }
                        if (!applied) {
                            *row.staged = v;
                            if (row.changed) *row.changed = true;
                            if (row.released) *row.released = true;
                        }
                    }
                    break;
                }
                app.value_edit_node = 0;
                app.value_edit_row = -1;
                app.value_edit_buf.clear();
            }

            // Frame removal (v3): X on a frame's title strip.
            for (size_t f = 0;
                 f < flow_ui.graph->frame_count && !structure_done; ++f) {
                if (flow_ui.graph->frames[f].remove_clicked &&
                    *flow_ui.graph->frames[f].remove_clicked) {
                    app.undo.execute(app.document,
                                     doc::remove_frame_command(
                                         flow_ui.graph->frames[f].id));
                    structure_done = true;
                }
            }

            // Cursor add menu (texed): opening focuses the filter; a pick
            // (or Enter = first match) spawns at the recorded click point
            // and splices into the right-clicked wire when one was hit.
            if (fe.add_menu_opened) {
                app.fx_filter.clear();
                app.fx_search_focus = true;
                app.preset_search_focus = false;
                app.duration_focus = false;
                app.find_mode = false;   // right-click = the ADD menu
            }
            {
                // Belt and braces: a pick is only meaningful while the
                // menu is actually open.
                const int pick =
                    !app.canvas_state.add_open ? -1
                    : fe.add_pick >= 0         ? fe.add_pick
                    : do_add_first             ? 0
                                               : -1;
                if (pick >= 0 && !structure_done) {
                    // Resolve through the BUILD-TIME action array (one
                    // truth with the drawn list — category headers made
                    // positional re-walks too fragile). Enter-on-filter
                    // advances past leading headers to the first real
                    // item.
                    int ridx = pick;
                    while (do_add_first && flow_ui.graph->add_headers &&
                           ridx <
                               static_cast<int>(
                                   flow_ui.graph->add_count) &&
                           flow_ui.graph->add_headers[ridx])
                        ++ridx;
                    AddAction act{};
                    bool act_ok = false;
                    if (flow_ui.add_actions && ridx >= 0 &&
                        ridx <
                            static_cast<int>(flow_ui.graph->add_count)) {
                        act = flow_ui.add_actions[ridx];
                        act_ok = act.kind != AddAction::Header;
                    }
                    // Find mode (Ctrl+F): the pick JUMPS to a node
                    // instead of adding one.
                    bool find_handled = false;
                    if (app.find_mode) {
                        find_handled = true;
                        for (size_t i = 0;
                             act_ok && act.kind == AddAction::FindNode &&
                             i < flow_ui.graph->node_count;
                             ++i) {
                            const flow::Node& nd =
                                flow_ui.graph->nodes[i];
                            if (nd.id != act.node) continue;
                            app.multi_sel.assign(1, nd.id);
                            app.canvas_state.center_on = nd.id;
                            const uint64_t fdoc3 = tag_doc(nd.id);
                            size_t fli = 0, ffi = 0;
                            if (nd.id == flow::kOutNodeId) {
                                app.sel = {SelKind::Output, 0};
                            } else {
                                switch (tag_kind(nd.id)) {
                                    case flow::NodeKind::Source:
                                        app.sel = {SelKind::LayerSource,
                                                   fdoc3};
                                        break;
                                    case flow::NodeKind::Effect:
                                        if (find_effect_by_id(
                                                app.document, fdoc3,
                                                &fli, &ffi)) {
                                            app.sel = {SelKind::Effect,
                                                       fdoc3};
                                            app.selected_layer = fli;
                                        }
                                        break;
                                    case flow::NodeKind::Mask:
                                        app.sel = {SelKind::Mask, fdoc3};
                                        break;
                                    case flow::NodeKind::ModSource:
                                        app.sel = {SelKind::ModSource,
                                                   fdoc3};
                                        break;
                                    case flow::NodeKind::Group:
                                        if (find_group_by_id(
                                                app.document, fdoc3,
                                                &fli)) {
                                            app.sel = {SelKind::Group,
                                                       fdoc3};
                                            app.selected_layer = fli;
                                        }
                                        break;
                                    default:
                                        break;
                                }
                            }
                            break;
                        }
                        app.find_mode = false;
                    }
                    const int src_kind =
                        !find_handled && act_ok &&
                                act.kind == AddAction::Source
                            ? act.index
                            : -1;
                    const int val_kind =
                        !find_handled && act_ok &&
                                act.kind == AddAction::Value
                            ? act.index
                            : -1;
                    const doc::EffectType chosen =
                        !find_handled && act_ok &&
                                act.kind == AddAction::Effect
                            ? static_cast<doc::EffectType>(act.index)
                            : doc::EffectType::Count;
                    if (!find_handled && act_ok &&
                        act.kind == AddAction::Frame) {
                        // "frame" under the Layout header (texed): a
                        // grouping box at the click point.
                        doc::Document::Frame fr;
                        fr.id = app.document.next_effect_id++;
                        fr.title = "frame " + std::to_string(fr.id);
                        fr.x = app.canvas_state.add_gx;
                        fr.y = app.canvas_state.add_gy;
                        app.undo.execute(
                            app.document,
                            doc::add_frame_command(std::move(fr)));
                        structure_done = true;
                    }
                    if (src_kind >= 0 &&
                        app.document.layers.size() < doc::kMaxLayers) {
                        // Source node at the click point (v3).
                        doc::Layer nl = doc::make_layer(
                            app.document, kSrcAddKinds[src_kind]);
                        nl.node_x = app.canvas_state.add_gx;
                        nl.node_y = app.canvas_state.add_gy;
                        const uint64_t lid = nl.id;
                        app.undo.execute(
                            app.document,
                            doc::add_layer_command(
                                std::move(nl),
                                app.document.layers.size()));
                        app.sel = {SelKind::LayerSource, lid};
                        app.multi_sel.assign(
                            1, flow::node_id(flow::NodeKind::Source, lid));
                        structure_done = true;
                    }
                    if (val_kind >= 0 && !structure_done) {
                        // Value node (v4): an unwired route at the click
                        // point — drag its out port onto a param row to
                        // drive something.
                        doc::ModRoute route;
                        route.id = app.document.next_route_id++;
                        route.source.type = kValAddTypes[val_kind];
                        route.target = {0, -1};   // inert until wired
                        route.amount = 0.25f;
                        route.node_x = app.canvas_state.add_gx;
                        route.node_y = app.canvas_state.add_gy;
                        const uint64_t rid = route.id;
                        app.undo.execute(app.document,
                                         doc::add_route_command(route));
                        app.sel = {SelKind::ModSource, rid};
                        app.multi_sel.assign(
                            1, flow::node_id(flow::NodeKind::ModSource,
                                             rid));
                        structure_done = true;
                    }
                    if (chosen != doc::EffectType::Count) {
                        app.undo.begin_group("Add Node");
                        if (app.document.layers.empty()) {
                            // v3: effects need a storage bag, never a
                            // user-facing precondition — conjure the
                            // clip host silently.
                            doc::Layer host = doc::make_layer(
                                app.document,
                                doc::LayerSourceKind::Clip);
                            app.undo.execute(app.document,
                                             doc::add_layer_command(
                                                 std::move(host), 0));
                        }
                        size_t li = std::min(
                            app.selected_layer,
                            app.document.layers.size() - 1);
                        auto fx = doc::make_effect(app.document, chosen);
                        fx.node_x = app.canvas_state.add_gx;
                        fx.node_y = app.canvas_state.add_gy;
                        // Scoped view: the new effect joins the open
                        // group at the end of its member span.
                        size_t insert_at = SIZE_MAX;   // SIZE_MAX = end
                        if (app.open_group) {
                            size_t gli2 = 0;
                            if (find_group_by_id(app.document,
                                                 app.open_group, &gli2)) {
                                li = gli2;
                                fx.group_id = app.open_group;
                                const auto& stk =
                                    app.document.layers[li].stack;
                                for (size_t s = 0; s < stk.size(); ++s)
                                    if (stk[s].group_id ==
                                        app.open_group)
                                        insert_at = s + 1;
                            }
                        }
                        const uint64_t new_id = fx.id;
                        auto is_bnode = [](uint64_t cid) {
                            const uint64_t t = cid >> 56;
                            return t == static_cast<uint64_t>(
                                            flow::NodeKind::GroupIn) +
                                            1 ||
                                   t == static_cast<uint64_t>(
                                            flow::NodeKind::GroupOut) +
                                            1;
                        };
                        const bool splice =
                            (app.canvas_state.splice_to != 0 ||
                             app.canvas_state.splice_from != 0) &&
                            !is_bnode(app.canvas_state.splice_from) &&
                            !is_bnode(app.canvas_state.splice_to);
                        auto splice_doc = [&](uint64_t cid) {
                            return cid == flow::kOutNodeId
                                ? 0ull
                                : (cid & 0x00FFFFFFFFFFFFFFull);
                        };
                        if (app.document.links.empty()) {
                            // Materialize the legacy links FIRST so the
                            // new node spawns unwired instead of being
                            // chained in by stack-order synthesis (this
                            // disconnect matches nothing by design).
                            app.undo.execute(
                                app.document,
                                doc::disconnect_command({0, 0, 9999}));
                        }
                        app.undo.execute(
                            app.document,
                            doc::add_effect_command(
                                li, std::move(fx),
                                insert_at == SIZE_MAX
                                    ? app.document.layers[li]
                                          .stack.size()
                                    : insert_at));
                        if (splice) {
                            app.undo.execute(
                                app.document,
                                doc::disconnect_command(
                                    {splice_doc(
                                         app.canvas_state.splice_from),
                                     splice_doc(
                                         app.canvas_state.splice_to),
                                     app.canvas_state.splice_port}));
                            app.undo.execute(
                                app.document,
                                doc::connect_command(
                                    {splice_doc(
                                         app.canvas_state.splice_from),
                                     new_id, 0}));
                            app.undo.execute(
                                app.document,
                                doc::connect_command(
                                    {new_id,
                                     splice_doc(
                                         app.canvas_state.splice_to),
                                     app.canvas_state.splice_port}));
                        }
                        app.undo.end_group();
                        app.sel = {SelKind::Effect, new_id};
                        app.multi_sel.assign(
                            1, flow::node_id(flow::NodeKind::Effect,
                                             new_id));
                        structure_done = true;
                    }
                    app.canvas_state.add_open = false;
                    app.fx_search_focus = false;
                }
            }

            if (fe.add_requested) {
                // Double-click on empty canvas ALWAYS opens an add
                // browser: the effect browser when a layer exists, the
                // layer picker when the graph is empty — an empty
                // document must never dead-end (no path to add = bug).
                if (app.document.layers.empty()) {
                    app.sel = {SelKind::AddLayer, 0};
                } else {
                    const size_t li =
                        std::min(app.selected_layer,
                                 app.document.layers.size() - 1);
                    app.sel = {SelKind::AddEffect,
                               app.document.layers[li].id};
                }
                app.insert_before_id = 0;
                app.add_gx = fe.add_x;
                app.add_gy = fe.add_y;
                app.add_pos_valid = true;
            }

            // Duplicate (texed Ctrl+D): the primary selection, offset
            // +26/+26, params copied, the copy selected. Sources stay
            // single (layer semantics are explicit).
            if (do_duplicate && !structure_done) {
                float px = 0.0f, py = 0.0f;
                if (app.sel.kind == SelKind::Effect) {
                    size_t li = 0, fi = 0;
                    if (find_effect_by_id(app.document, app.sel.id, &li,
                                          &fi)) {
                        doc::EffectInstance copy =
                            app.document.layers[li].stack[fi];
                        copy.id = app.document.next_effect_id++;
                        // Scoped view: the copy joins the open group,
                        // right after its original (span stays whole).
                        copy.group_id = app.open_group;
                        node_pos_of(flow::node_id(flow::NodeKind::Effect,
                                                  app.sel.id),
                                    &px, &py);
                        copy.node_x = px + 26.0f;
                        copy.node_y = py + 26.0f;
                        const uint64_t nid = copy.id;
                        app.undo.begin_group("Duplicate");
                        if (app.document.links.empty())
                            // Materialize first so the copy spawns
                            // unwired (matches the popup add).
                            app.undo.execute(
                                app.document,
                                doc::disconnect_command({0, 0, 9999}));
                        app.undo.execute(
                            app.document,
                            doc::add_effect_command(
                                li, std::move(copy),
                                app.open_group
                                    ? fi + 1
                                    : app.document.layers[li]
                                          .stack.size()));
                        app.undo.end_group();
                        app.sel = {SelKind::Effect, nid};
                        app.multi_sel.assign(
                            1, flow::node_id(flow::NodeKind::Effect, nid));
                        structure_done = true;
                    }
                } else if (app.sel.kind == SelKind::ModSource) {
                    for (const doc::ModRoute& r : app.document.mod_routes)
                        if (r.id == app.sel.id) {
                            doc::ModRoute copy = r;
                            copy.id = app.document.next_route_id++;
                            node_pos_of(
                                flow::node_id(flow::NodeKind::ModSource,
                                              app.sel.id),
                                &px, &py);
                            copy.node_x = px + 26.0f;
                            copy.node_y = py + 26.0f;
                            const uint64_t nid = copy.id;
                            app.undo.execute(app.document,
                                             doc::add_route_command(copy));
                            app.sel = {SelKind::ModSource, nid};
                            app.multi_sel.assign(
                                1, flow::node_id(
                                       flow::NodeKind::ModSource, nid));
                            structure_done = true;
                            break;
                        }
                } else if (app.sel.kind == SelKind::Mask) {
                    for (const doc::Mask& m : app.document.masks)
                        if (m.id == app.sel.id) {
                            doc::Mask copy = m;
                            copy.id = app.document.next_mask_id++;
                            copy.name = m.name + " copy";
                            node_pos_of(
                                flow::node_id(flow::NodeKind::Mask,
                                              app.sel.id),
                                &px, &py);
                            copy.node_x = px + 26.0f;
                            copy.node_y = py + 26.0f;
                            const uint64_t nid = copy.id;
                            app.undo.execute(
                                app.document,
                                doc::add_mask_command(std::move(copy)));
                            app.sel = {SelKind::Mask, nid};
                            app.multi_sel.assign(
                                1, flow::node_id(flow::NodeKind::Mask,
                                                 nid));
                            structure_done = true;
                            break;
                        }
                } else if (app.sel.kind != SelKind::None) {
                    app.status = "duplicate: select an effect, value, or "
                                 "mask node";
                }
            }

            // Ctrl+A (texed selectAll): every card joins the set.
            if (do_select_all) {
                app.multi_sel.clear();
                for (size_t i = 0; i < flow_ui.graph->node_count; ++i)
                    app.multi_sel.push_back(flow_ui.graph->nodes[i].id);
            }

            // Enter a group (double-click its card / context menu): the
            // canvas scopes to its members + In/Out boundary nodes
            // (texed enterSubgraph); the breadcrumb's "main" exits.
            // Pure view state — no document mutation.
            if (fe.group_open || ctx_open_group) {
                const uint64_t gid =
                    tag_doc(fe.group_open ? fe.group_open
                                          : ctx_open_group);
                size_t gli = 0;
                if (find_group_by_id(app.document, gid, &gli)) {
                    // Save the main-graph view, then fit the members —
                    // they can sit anywhere, never assume the origin.
                    app.saved_pan_x = app.canvas_state.pan_x;
                    app.saved_pan_y = app.canvas_state.pan_y;
                    app.saved_zoom = app.canvas_state.zoom;
                    app.saved_view_valid = true;
                    app.canvas_state.view_inited = false;
                    app.open_group = gid;
                    app.sel = {SelKind::Group, gid};
                    app.selected_layer = gli;
                    app.multi_sel.clear();
                    app.sel_wires.clear();
                }
            }
            if (fe.crumb_clicked) {
                app.open_group = 0;
                app.multi_sel.clear();
                app.sel_wires.clear();
                if (app.saved_view_valid) {
                    app.canvas_state.pan_x = app.saved_pan_x;
                    app.canvas_state.pan_y = app.saved_pan_y;
                    app.canvas_state.zoom = app.saved_zoom;
                    app.saved_view_valid = false;
                } else {
                    app.canvas_state.view_inited = false;
                }
            }

            // Ctrl+G (texed groupSelection → subgraph): wrap the span of
            // selected effects in one layer into a FOLDED group — it
            // lands as a single card with its exposed face as knobs.
            if (do_group && app.open_group) {
                app.status = "groups don't nest - exit to the main graph "
                             "first";
                do_group = false;
            }
            if (do_group && !structure_done) {
                size_t gli = SIZE_MAX, lo = SIZE_MAX, hi = 0;
                int count = 0;
                for (const uint64_t cid : app.multi_sel) {
                    if (tag_kind(cid) != flow::NodeKind::Effect) continue;
                    size_t li = 0, fi = 0;
                    if (!find_effect_by_id(app.document, tag_doc(cid), &li,
                                           &fi))
                        continue;
                    if (gli == SIZE_MAX) gli = li;
                    if (li != gli) continue;
                    lo = std::min(lo, fi);
                    hi = std::max(hi, fi);
                    ++count;
                }
                if (count >= 1 && gli != SIZE_MAX) {
                    doc::Group g = doc::make_group(app.document, "group");
                    g.folded = true;
                    const uint64_t gid = g.id;
                    app.status =
                        "grouped " + std::to_string(hi - lo + 1) +
                        " effect(s)";
                    if (static_cast<size_t>(count) < hi - lo + 1)
                        app.status =
                            "grouped the whole stack span between the "
                            "selected effects";
                    app.undo.execute(app.document,
                                     doc::group_effects_command(
                                         gli, std::move(g), lo, hi));
                    app.sel = {SelKind::Group, gid};
                    app.selected_layer = gli;
                    app.multi_sel.assign(
                        1, flow::node_id(flow::NodeKind::Group, gid));
                    structure_done = true;
                } else {
                    app.status = "group: select effect nodes first";
                }
            }
            // Ctrl+Shift+G: dissolve the selected group (or the group of
            // the selected effect); members keep their cards.
            if (do_ungroup && !structure_done) {
                uint64_t gid = 0;
                if (app.sel.kind == SelKind::Group) {
                    gid = app.sel.id;
                } else if (app.sel.kind == SelKind::Effect) {
                    size_t li = 0, fi = 0;
                    if (find_effect_by_id(app.document, app.sel.id, &li,
                                          &fi))
                        gid = app.document.layers[li].stack[fi].group_id;
                }
                size_t gli = 0;
                if (gid && find_group_by_id(app.document, gid, &gli)) {
                    app.undo.execute(app.document,
                                     doc::ungroup_command(gli, gid));
                    if (app.sel.kind == SelKind::Group) app.sel = {};
                    app.multi_sel.clear();
                    app.status = "ungrouped";
                    structure_done = true;
                }
            }

            // Ctrl+V (texed paste): remap ids, land at the canvas cursor,
            // recreate the internal links — one undo group.
            if (do_paste && app.open_group) {
                app.status = "exit the group (breadcrumb) to paste";
                do_paste = false;
            }
            if (do_paste && app.clipboard.valid && !structure_done) {
                const auto& cb = app.clipboard;
                const float px0 = app.canvas_state.last_gx;
                const float py0 = app.canvas_state.last_gy;
                std::unordered_map<uint64_t, uint64_t> fx_remap;
                std::unordered_map<uint64_t, uint64_t> mask_remap;
                std::vector<uint64_t> pasted;
                app.undo.begin_group("Paste");
                if (!cb.effects.empty() && app.document.layers.empty()) {
                    doc::Layer host = doc::make_layer(
                        app.document, doc::LayerSourceKind::Clip);
                    app.undo.execute(app.document,
                                     doc::add_layer_command(
                                         std::move(host), 0));
                }
                if (!cb.effects.empty() && app.document.links.empty())
                    // Materialize so pasted effects spawn wired only to
                    // each other (this disconnect matches nothing).
                    app.undo.execute(app.document,
                                     doc::disconnect_command({0, 0,
                                                              9999}));
                for (const doc::Mask& m0 : cb.masks) {
                    doc::Mask nm = m0;
                    nm.id = app.document.next_mask_id++;
                    nm.node_x = m0.node_x - cb.origin_x + px0;
                    nm.node_y = m0.node_y - cb.origin_y + py0;
                    mask_remap[m0.id] = nm.id;
                    pasted.push_back(
                        flow::node_id(flow::NodeKind::Mask, nm.id));
                    app.undo.execute(app.document,
                                     doc::add_mask_command(std::move(nm)));
                }
                uint64_t first_fx = 0;
                if (!cb.effects.empty()) {
                    const size_t li =
                        std::min(app.selected_layer,
                                 app.document.layers.size() - 1);
                    for (const doc::EffectInstance& f0 : cb.effects) {
                        doc::EffectInstance fx = f0;
                        fx.id = app.document.next_effect_id++;
                        fx.group_id = 0;
                        if (auto itm = mask_remap.find(fx.mask_id);
                            itm != mask_remap.end())
                            fx.mask_id = itm->second;
                        fx.node_x = f0.node_x - cb.origin_x + px0;
                        fx.node_y = f0.node_y - cb.origin_y + py0;
                        fx_remap[f0.id] = fx.id;
                        if (!first_fx) first_fx = fx.id;
                        pasted.push_back(flow::node_id(
                            flow::NodeKind::Effect, fx.id));
                        app.undo.execute(
                            app.document,
                            doc::add_effect_command(
                                li, std::move(fx),
                                app.document.layers[li].stack.size()));
                    }
                    for (const doc::Document::NodeLink& l : cb.links)
                        app.undo.execute(app.document,
                                         doc::connect_command(
                                             {fx_remap[l.from],
                                              fx_remap[l.to],
                                              l.to_port}));
                }
                for (const doc::ModRoute& r0 : cb.routes) {
                    doc::ModRoute r = r0;
                    r.id = app.document.next_route_id++;
                    if (r.target.effect_id & doc::kMaskParamBit) {
                        const uint64_t mid =
                            r.target.effect_id & ~doc::kMaskParamBit;
                        if (auto itm = mask_remap.find(mid);
                            itm != mask_remap.end())
                            r.target.effect_id =
                                itm->second | doc::kMaskParamBit;
                    } else if (auto itf =
                                   fx_remap.find(r.target.effect_id);
                               itf != fx_remap.end()) {
                        r.target.effect_id = itf->second;
                    }
                    r.node_x = r0.node_x - cb.origin_x + px0;
                    r.node_y = r0.node_y - cb.origin_y + py0;
                    pasted.push_back(flow::node_id(
                        flow::NodeKind::ModSource, r.id));
                    app.undo.execute(app.document,
                                     doc::add_route_command(r));
                }
                app.undo.end_group();
                if (!pasted.empty()) {
                    app.multi_sel = pasted;
                    if (first_fx)
                        app.sel = {SelKind::Effect, first_fx};
                    app.status = "pasted " +
                                 std::to_string(pasted.size()) +
                                 " node(s)";
                    structure_done = true;
                }
            }

            // Arrow-key nudge (texed): the whole selection shifts; the
            // per-node position command's merge coalesces held keys.
            if ((nudge_dx != 0.0f || nudge_dy != 0.0f) &&
                !app.multi_sel.empty()) {
                for (const uint64_t cid : app.multi_sel) {
                    doc::NodeRef ref;
                    uint64_t rid = 0;
                    float nx = 0.0f, ny = 0.0f;
                    if (!node_ref_of(cid, &ref, &rid)) continue;
                    if (!node_pos_of(cid, &nx, &ny)) continue;
                    app.undo.execute(app.document,
                                     doc::set_node_pos_command(
                                         ref, rid, nx + nudge_dx,
                                         ny + nudge_dy),
                                     /*coalesce=*/true);
                }
            }

            // Align / distribute (texed alignSelection): left/top snap to
            // the selection minimum; spread spaces evenly between the two
            // outermost. One undo group per click.
            for (int a = 0; a < 4; ++a) {
                if (!frame_ui.align_clicked[a] ||
                    !*frame_ui.align_clicked[a])
                    continue;
                struct AlignNode {
                    uint64_t cid;
                    float x, y;
                };
                std::vector<AlignNode> list;
                for (const uint64_t cid : app.multi_sel) {
                    float nx = 0.0f, ny = 0.0f;
                    if (node_pos_of(cid, &nx, &ny))
                        list.push_back({cid, nx, ny});
                }
                if (list.size() < 2) break;
                if (a == 0 || a == 1) {
                    float lo = 1e9f;
                    for (const AlignNode& an : list)
                        lo = std::min(lo, a == 0 ? an.x : an.y);
                    for (AlignNode& an : list)
                        (a == 0 ? an.x : an.y) = lo;
                } else {
                    const bool horiz = a == 2;
                    std::sort(list.begin(), list.end(),
                              [&](const AlignNode& p, const AlignNode& q) {
                                  return (horiz ? p.x : p.y) <
                                         (horiz ? q.x : q.y);
                              });
                    const float lo = horiz ? list.front().x
                                           : list.front().y;
                    const float hi = horiz ? list.back().x : list.back().y;
                    for (size_t i = 1; i + 1 < list.size(); ++i) {
                        const float v =
                            lo + (hi - lo) * static_cast<float>(i) /
                                     static_cast<float>(list.size() - 1);
                        (horiz ? list[i].x : list[i].y) = v;
                    }
                }
                app.undo.begin_group("Align Nodes");
                for (const AlignNode& an : list) {
                    doc::NodeRef ref;
                    uint64_t rid = 0;
                    if (!node_ref_of(an.cid, &ref, &rid)) continue;
                    app.undo.execute(app.document,
                                     doc::set_node_pos_command(ref, rid,
                                                               an.x, an.y));
                }
                app.undo.end_group();
                break;
            }

            // Delete: ONE pass over the whole selection — wires AND
            // nodes in a single undo group (a wire-only cut used to
            // consume the frame and starve node deletion). Undeletable
            // cards (Output, group cards) are SKIPPED, never block the
            // rest. Delete and each card's X run the same commands.
            // Group deletion (texed: a subgraph node deletes whole):
            // members go first, then the empty group dissolves. Shares
            // one path across Delete, the context menu, and the card X.
            auto delete_group = [&](uint64_t gid, bool own_undo_group) {
                size_t gli = 0;
                if (!find_group_by_id(app.document, gid, &gli))
                    return false;
                if (own_undo_group) app.undo.begin_group("Delete Group");
                bool removing = true;
                while (removing) {
                    removing = false;
                    const auto& stk = app.document.layers[gli].stack;
                    for (size_t s = 0; s < stk.size(); ++s)
                        if (stk[s].group_id == gid) {
                            app.undo.execute(
                                app.document,
                                doc::remove_effect_command(gli, s));
                            removing = true;
                            break;
                        }
                }
                app.undo.execute(app.document,
                                 doc::ungroup_command(gli, gid));
                if (own_undo_group) app.undo.end_group();
                if (app.open_group == gid) app.open_group = 0;
                return true;
            };
            if (do_delete_sel && !structure_done &&
                (!app.sel_wires.empty() || app.multi_sel.size() > 1)) {
                auto is_cid_kind = [&](uint64_t cid, flow::NodeKind k) {
                    return cid != 0 && cid != flow::kOutNodeId &&
                           tag_kind(cid) == k;
                };
                // Group wire ends resolve to their boundary members —
                // the link table stores members, never the card.
                auto boundary_of = [&](uint64_t cid,
                                       bool from_side) -> uint64_t {
                    const uint64_t gid = tag_doc(cid);
                    size_t gli = 0;
                    if (!find_group_by_id(app.document, gid, &gli))
                        return 0;
                    uint64_t first = 0, last_id = 0, bind = 0;
                    const doc::Group* gr2 = nullptr;
                    for (const doc::Group& g :
                         app.document.layers[gli].groups)
                        if (g.id == gid) gr2 = &g;
                    for (const doc::EffectInstance& e :
                         app.document.layers[gli].stack)
                        if (e.group_id == gid) {
                            if (!first) first = e.id;
                            last_id = e.id;
                            if (gr2 &&
                                e.id == (from_side ? gr2->face_out
                                                   : gr2->face_in))
                                bind = e.id;
                        }
                    return bind ? bind : (from_side ? last_id : first);
                };
                auto wire_live = [&](const flow::Wire& sw) {
                    for (size_t w = 0; w < flow_ui.graph->wire_count; ++w)
                        if (flow_ui.graph->wires[w].from == sw.from &&
                            flow_ui.graph->wires[w].to == sw.to &&
                            flow_ui.graph->wires[w].kind == sw.kind)
                            return true;
                    return false;
                };
                // Members of the open group, for boundary-wire cuts.
                std::unordered_set<uint64_t> del_members;
                if (app.open_group)
                    for (const doc::Layer& sl : app.document.layers)
                        for (const doc::EffectInstance& e : sl.stack)
                            if (e.group_id == app.open_group)
                                del_members.insert(e.id);
                app.undo.begin_group("Delete Selection");
                for (const flow::Wire& sw : app.sel_wires) {
                    if (!wire_live(sw)) continue;
                    // Boundary wires in the scoped view cut the REAL
                    // outer link they visualize.
                    if (is_cid_kind(sw.from, flow::NodeKind::GroupIn) ||
                        is_cid_kind(sw.to, flow::NodeKind::GroupOut)) {
                        const auto links =
                            app.document.links.empty()
                                ? doc::synthesize_links(app.document)
                                : app.document.links;
                        for (const doc::Document::NodeLink& l : links) {
                            if (l.to_port != 0) continue;
                            const bool in_cut =
                                is_cid_kind(sw.from,
                                            flow::NodeKind::GroupIn) &&
                                !del_members.count(l.from) &&
                                del_members.count(l.to) &&
                                l.to == tag_doc(sw.to);
                            const bool out_cut =
                                is_cid_kind(sw.to,
                                            flow::NodeKind::GroupOut) &&
                                del_members.count(l.from) &&
                                !del_members.count(l.to) &&
                                l.from == tag_doc(sw.from);
                            if (in_cut || out_cut) {
                                app.undo.execute(
                                    app.document,
                                    doc::disconnect_command(l));
                                break;
                            }
                        }
                        continue;
                    }
                    const uint64_t wf =
                        is_cid_kind(sw.from, flow::NodeKind::Group)
                            ? boundary_of(sw.from, true)
                            : tag_doc(sw.from);
                    uint64_t wt = sw.to == flow::kOutNodeId
                        ? 0ull
                        : (is_cid_kind(sw.to, flow::NodeKind::Group)
                               ? boundary_of(sw.to, false)
                               : tag_doc(sw.to));
                    if (sw.kind == 1 &&
                        is_cid_kind(sw.to, flow::NodeKind::Group)) {
                        // Mask wire re-anchored on a group card: clear
                        // the MEMBER that actually wears this mask.
                        const uint64_t gid = tag_doc(sw.to);
                        size_t gli = 0;
                        if (find_group_by_id(app.document, gid, &gli)) {
                            const auto& stk =
                                app.document.layers[gli].stack;
                            for (size_t s = 0; s < stk.size(); ++s)
                                if (stk[s].group_id == gid &&
                                    stk[s].mask_id == tag_doc(sw.from))
                                    app.undo.execute(
                                        app.document,
                                        doc::set_effect_mask_command(
                                            gli, s, 0));
                        }
                        continue;
                    }
                    if (sw.kind == 0 || sw.kind == 3) {
                        if (sw.to != flow::kOutNodeId &&
                            tag_kind(sw.to) == flow::NodeKind::Mask) {
                            if (const doc::Mask* m =
                                    doc::find_mask(app.document, wt)) {
                                doc::Mask edited = *m;
                                edited.source_layer_id = 0;
                                app.undo.execute(
                                    app.document,
                                    doc::set_mask_params_command(
                                        std::move(edited)));
                            }
                        } else {
                            app.undo.execute(
                                app.document,
                                doc::disconnect_command(
                                    {wf, wt,
                                     sw.kind == 3 ? 2u : 0u}));
                        }
                    } else if (sw.kind == 1 &&
                               !is_cid_kind(sw.from,
                                            flow::NodeKind::Mask)) {
                        // Image matte (v5.2): a plain port-1 link cut.
                        app.undo.execute(
                            app.document,
                            doc::disconnect_command({wf, wt, 1}));
                    } else if (sw.kind == 1) {
                        size_t li = 0, fi = 0;
                        if (tag_kind(sw.to) == flow::NodeKind::Effect &&
                            find_effect_by_id(app.document, wt, &li,
                                              &fi)) {
                            app.undo.execute(
                                app.document,
                                doc::set_effect_mask_command(li, fi, 0));
                        } else if (tag_kind(sw.to) ==
                                   flow::NodeKind::Source) {
                            const int idx =
                                layer_index_by_id(app.document, wt);
                            if (idx >= 0) {
                                doc::Layer edited = app.document.layers
                                    [static_cast<size_t>(idx)];
                                edited.mask_id = 0;
                                app.undo.execute(
                                    app.document,
                                    doc::set_layer_props_command(
                                        std::move(edited)));
                            }
                        }
                    } else if (sw.kind == 2) {
                        app.undo.execute(
                            app.document,
                            doc::set_route_target_command(wf, {0, -1}));
                    }
                }
                for (const uint64_t cid : app.multi_sel) {
                    const uint64_t did = tag_doc(cid);
                    size_t li = 0, fi = 0;
                    switch (tag_kind(cid)) {
                        case flow::NodeKind::Effect:
                            if (find_effect_by_id(app.document, did, &li,
                                                  &fi))
                                app.undo.execute(
                                    app.document,
                                    doc::remove_effect_command(li, fi));
                            break;
                        case flow::NodeKind::Source: {
                            // Same path as the card's X.
                            const int idx =
                                layer_index_by_id(app.document, did);
                            if (idx >= 0)
                                app.undo.execute(
                                    app.document,
                                    doc::remove_layer_command(
                                        static_cast<size_t>(idx)));
                            break;
                        }
                        case flow::NodeKind::Mask:
                            if (app.overlay_mask_id == did)
                                app.overlay_mask_id = 0;
                            app.undo.execute(
                                app.document,
                                doc::remove_mask_command(did));
                            break;
                        case flow::NodeKind::ModSource:
                            app.undo.execute(
                                app.document,
                                doc::remove_route_command(did));
                            break;
                        case flow::NodeKind::Group:
                            // Whole subgraph goes (texed): members +
                            // the group entity.
                            delete_group(did, /*own_undo_group=*/false);
                            break;
                        default:
                            break;   // Output / boundary nodes: skipped
                    }
                }
                app.undo.end_group();
                app.sel_wires.clear();
                app.multi_sel.clear();
                app.sel = {};
                if (!app.document.layers.empty() &&
                    app.selected_layer >= app.document.layers.size())
                    app.selected_layer = app.document.layers.size() - 1;
                structure_done = true;
            }
            // Delete removes the selected effect or mask (layer removal
            // stays behind its inspector button — docs/flow_canvas.md).
            if (do_delete_sel && !structure_done) {
                if (app.sel.kind == SelKind::Effect) {
                    size_t li = 0, fi = 0;
                    if (find_effect_by_id(app.document, app.sel.id, &li,
                                          &fi)) {
                        app.undo.execute(
                            app.document,
                            doc::remove_effect_command(li, fi));
                        // Land on the neighbour that takes the slot.
                        const auto& stack = app.document.layers[li].stack;
                        app.sel = stack.empty()
                            ? Selection{SelKind::LayerSource,
                                        app.document.layers[li].id}
                            : Selection{SelKind::Effect,
                                        stack[std::min(fi,
                                                       stack.size() - 1)]
                                            .id};
                        structure_done = true;
                    }
                } else if (app.sel.kind == SelKind::Mask) {
                    if (app.overlay_mask_id == app.sel.id)
                        app.overlay_mask_id = 0;
                    app.undo.execute(app.document,
                                     doc::remove_mask_command(app.sel.id));
                    app.sel = {};
                    structure_done = true;
                } else if (app.sel.kind == SelKind::ModSource) {
                    app.undo.execute(app.document,
                                     doc::remove_route_command(app.sel.id));
                    app.sel = {};
                    structure_done = true;
                } else if (app.sel.kind == SelKind::Group) {
                    if (delete_group(app.sel.id,
                                     /*own_undo_group=*/true)) {
                        app.sel = {};
                        app.multi_sel.clear();
                        structure_done = true;
                    }
                } else if (app.sel.kind == SelKind::LayerSource) {
                    // Delete == the card's X: same remove_layer path.
                    const int idx =
                        layer_index_by_id(app.document, app.sel.id);
                    if (idx >= 0) {
                        app.undo.execute(
                            app.document,
                            doc::remove_layer_command(
                                static_cast<size_t>(idx)));
                        if (!app.document.layers.empty() &&
                            app.selected_layer >=
                                app.document.layers.size())
                            app.selected_layer =
                                app.document.layers.size() - 1;
                        app.sel = {};
                        structure_done = true;
                    }
                }
            }
            // Group card X: same whole-subgraph delete as the key.
            for (const auto& gr2 : frame_ui.group_removes) {
                if (structure_done || !*gr2.second) continue;
                if (delete_group(gr2.first, /*own_undo_group=*/true)) {
                    if (app.sel.kind == SelKind::Group &&
                        app.sel.id == gr2.first)
                        app.sel = {};
                    app.multi_sel.clear();
                    structure_done = true;
                }
            }
        }

        for (size_t t = 0;
             !structure_done && t < static_cast<size_t>(doc::EffectType::Count);
             ++t) {
            if (frame_ui.add_clicked[t] && *frame_ui.add_clicked[t]) {
                auto fx = doc::make_effect(app.document,
                                           static_cast<doc::EffectType>(t));
                // Splice where the canvas asked (docs/flow_canvas.md):
                // insert_before anchors a slot, else the chain end; a
                // double-click add spawns the card at the click.
                const auto& stack = app.document.layers[ui_layer].stack;
                size_t insert_at = stack.size();
                if (app.insert_before_id != 0) {
                    const uint64_t bdoc =
                        app.insert_before_id & 0x00FFFFFFFFFFFFFFull;
                    for (size_t i = 0; i < stack.size(); ++i)
                        if (stack[i].id == bdoc) {
                            insert_at = i;
                            break;
                        }
                }
                if (app.add_pos_valid) {
                    fx.node_x = app.add_gx;
                    fx.node_y = app.add_gy;
                    app.add_pos_valid = false;
                }
                const uint64_t new_id = fx.id;
                app.undo.execute(app.document,
                                 doc::add_effect_command(
                                     ui_layer, std::move(fx), insert_at));
                // Select the newborn so its params appear immediately.
                app.sel = {SelKind::Effect, new_id};
                app.insert_before_id = 0;
                structure_done = true;
            }
        }

        // ---- morph position (coalesced slider drag)
        if (frame_ui.morph_changed && *frame_ui.morph_changed &&
            frame_ui.morph_staged &&
            *frame_ui.morph_staged != app.document.morph_pos) {
            app.undo.execute(
                app.document,
                doc::set_morph_command(app.document.morph_from,
                                       app.document.morph_to,
                                       *frame_ui.morph_staged),
                /*coalesce=*/true);
        }

        // ---- time remap (spec §6.1): speed drag + mode cycle
        if (frame_ui.speed_changed && *frame_ui.speed_changed &&
            frame_ui.speed_staged &&
            *frame_ui.speed_staged != app.document.speed) {
            app.undo.execute(app.document,
                             doc::set_time_remap_command(
                                 *frame_ui.speed_staged,
                                 app.document.time_mode),
                             /*coalesce=*/true);
        }
        if (frame_ui.time_mode_selected && *frame_ui.time_mode_selected >= 0 &&
            static_cast<uint32_t>(*frame_ui.time_mode_selected) !=
                app.document.time_mode) {
            app.undo.execute(
                app.document,
                doc::set_time_remap_command(
                    app.document.speed,
                    static_cast<uint32_t>(*frame_ui.time_mode_selected)));
        }

        // ---- sidechain + audio nudge (spec §7)
        if (frame_ui.sc_selected && *frame_ui.sc_selected >= 0) {
            const int sel = *frame_ui.sc_selected;
            const bool has_sc = !app.document.sidechain_path.empty();
            const int pick_index = has_sc ? 2 : 1;
            if (sel == 0 && has_sc) {
                app.undo.execute(app.document,
                                 doc::set_audio_config_command(
                                     "", false,
                                     app.document.audio_offset_ms));
            } else if (sel == pick_index) {
                auto picked = platform::show_open_dialog(
                    window.get(),
                    {{"audio (wav / mp4 / mov)", "*.wav;*.mp4;*.mov"},
                     {"all files", "*.*"}});
                if (picked)
                    app.undo.execute(app.document,
                                     doc::set_audio_config_command(
                                         picked->string(),
                                         app.document.sidechain_mux,
                                         app.document.audio_offset_ms));
            }
        }
        if (frame_ui.sc_mux_changed && *frame_ui.sc_mux_changed) {
            app.undo.execute(app.document,
                             doc::set_audio_config_command(
                                 app.document.sidechain_path,
                                 *frame_ui.sc_mux_staged,
                                 app.document.audio_offset_ms));
        }
        if (frame_ui.nudge_changed && *frame_ui.nudge_changed &&
            frame_ui.nudge_staged &&
            *frame_ui.nudge_staged != app.document.audio_offset_ms) {
            app.undo.execute(app.document,
                             doc::set_audio_config_command(
                                 app.document.sidechain_path,
                                 app.document.sidechain_mux,
                                 *frame_ui.nudge_staged),
                             /*coalesce=*/true);
        }
        if (frame_ui.nudge_released && *frame_ui.nudge_released &&
            !did_break) {
            app.undo.break_coalescing();
            did_break = true;
        }
        if (frame_ui.proxy_toggle_changed && *frame_ui.proxy_toggle_changed)
            app.undo.execute(app.document,
                             doc::set_use_proxy_command(
                                 *frame_ui.proxy_toggle_staged));
        if (frame_ui.lossless_changed && *frame_ui.lossless_changed) {
            app.import_lossless = *frame_ui.lossless_staged;
            save_ui_prefs(app);
        }
        if (frame_ui.morph_released && *frame_ui.morph_released &&
            !did_break) {
            app.undo.break_coalescing();
            did_break = true;
        }

        // ---- randomize (spec §10): one gesture = one undo step
        if (frame_ui.chaos_changed && *frame_ui.chaos_changed &&
            frame_ui.chaos_staged)
            app.chaos = *frame_ui.chaos_staged;
        if (frame_ui.randomize_all && *frame_ui.randomize_all &&
            !app.document.layers.empty() &&
            !app.document.layers[ui_layer].stack.empty()) {
            doc::randomize_stack(
                app.document, app.undo, ui_layer, app.chaos,
                hash_combine(app.document.master_seed, app.rng_counter++));
        }
        for (const FxRowActions& row : frame_ui.rows) {
            if (app.document.layers.empty()) break;
            if (row.randomize && *row.randomize &&
                row.fx_index < app.document.layers[ui_layer].stack.size()) {
                doc::randomize_effect(
                    app.document, app.undo, ui_layer, row.fx_index, app.chaos,
                    hash_combine(app.document.master_seed,
                                 app.rng_counter++));
                break;
            }
        }

        // ---- group edits (spec §5, v5.3: groups + the exposed face)
        for (const FrameUi::ExposeToggle& et : frame_ui.expose_toggles) {
            if (!*et.clicked) continue;
            if (et.layer_index >= app.document.layers.size()) continue;
            app.undo.execute(app.document,
                             doc::set_group_exposed_command(
                                 et.layer_index, et.group_id, et.key,
                                 et.expose));
            break;
        }
        for (const FrameUi::GroupActions& ga : frame_ui.group_actions) {
            // The group's OWN layer, not the selected one — group cards
            // stage actions from any layer on the canvas.
            size_t ga_layer = 0;
            if (!find_group_by_id(app.document, ga.group_id, &ga_layer))
                continue;
            const doc::Group* group = nullptr;
            for (const doc::Group& g :
                 app.document.layers[ga_layer].groups)
                if (g.id == ga.group_id) group = &g;
            if (!group) continue;
            if (*ga.fold) {
                doc::Group edited = *group;
                edited.folded = !edited.folded;
                app.undo.execute(app.document,
                                 doc::set_group_props_command(ga_layer,
                                                              edited));
            } else if (*ga.bypass_changed) {
                doc::Group edited = *group;
                edited.bypass = *ga.bypass_staged;
                app.undo.execute(app.document,
                                 doc::set_group_props_command(ga_layer,
                                                              edited));
            } else if (*ga.save) {
                auto out_path = platform::show_save_dialog(
                    window.get(), {{"looks preset", "*.json"}},
                    (group->name.empty() ? std::string("preset")
                                         : group->name) + ".json");
                if (out_path) {
                    if (out_path->extension() != ".json")
                        out_path->replace_extension(".json");
                    doc::Preset preset = doc::make_preset_from_group(
                        app.document, ga_layer, ga.group_id);
                    preset.name = out_path->stem().string();
                    if (preset.tags.empty()) preset.tags.push_back("user");
                    std::error_code ec;
                    std::filesystem::create_directories(
                        out_path->parent_path(), ec);
                    if (doc::save_preset(*out_path, preset)) {
                        app.status =
                            "saved preset " + out_path->filename().string();
                        rescan_presets(app);
                    } else {
                        app.status = "preset save failed";
                    }
                }
            } else if (*ga.ungroup && !structure_done) {
                app.undo.execute(app.document,
                                 doc::ungroup_command(ga_layer,
                                                      ga.group_id));
                structure_done = true;
            }
        }
        // Preset spawn, shared by click and drag-out. The card lands at
        // an explicit CANVAS position — click centers it in the visible
        // viewport (nodes may live far from the origin), a drag-out
        // drops it under the cursor.
        auto canvas_graph_pos = [&](Vec2 screen, float* gx, float* gy) {
            if (!frame_ui.canvas_node) return false;
            const ui::Rect& cr = frame_ui.canvas_node->rect;
            if (cr.w <= 0.0f || cr.h <= 0.0f) return false;
            const float z = std::max(app.canvas_state.zoom, 1e-3f);
            *gx = (screen.x - cr.x - app.canvas_state.pan_x) / z;
            *gy = (screen.y - cr.y - app.canvas_state.pan_y) / z;
            return true;
        };
        auto spawn_preset = [&](size_t pi, bool at_cursor) {
            if (structure_done || pi >= app.presets.size()) return;
            if (app.open_group) {
                app.status = "exit the group (breadcrumb) to add presets";
                return;
            }
            app.undo.begin_group("Add Preset");
            if (app.document.layers.empty()) {
                // Same silent host conjure as the effect popup — a
                // storage bag is never a user-facing precondition.
                doc::Layer host = doc::make_layer(
                    app.document, doc::LayerSourceKind::Clip);
                app.undo.execute(app.document,
                                 doc::add_layer_command(std::move(host),
                                                        0));
            }
            const size_t target_layer =
                std::min(ui_layer, app.document.layers.size() - 1);
            doc::Group group;
            std::vector<doc::EffectInstance> effects;
            doc::instantiate_preset(app.document, app.presets[pi],
                                    &group, &effects);
            float gx = 0.0f, gy = 0.0f;
            const ui::Rect cr = frame_ui.canvas_node
                                    ? frame_ui.canvas_node->rect
                                    : ui::Rect{};
            const Vec2 anchor = at_cursor
                ? input.mouse
                : Vec2{cr.x + cr.w * 0.5f, cr.y + cr.h * 0.5f};
            if (canvas_graph_pos(anchor, &gx, &gy)) {
                group.node_x = gx - flow::node_width() * 0.5f;
                group.node_y = gy - 60.0f;
            }
            const uint64_t new_gid = group.id;
            app.undo.execute(app.document,
                             doc::insert_group_command(
                                 target_layer, std::move(group),
                                 std::move(effects)));
            app.undo.end_group();
            // Select the dropped group: its face lands in the inspector
            // and the canvas highlights the new card.
            app.sel = {SelKind::Group, new_gid};
            structure_done = true;
        };
        // Drag a preset out of the browser onto the canvas (texed
        // palette drag): press arms, slop starts the drag, release over
        // the canvas drops at the cursor. A still click on the button
        // keeps firing preset_clicks below (centered spawn).
        if (input.left_pressed() && app.preset_drag < 0) {
            for (const FrameUi::PresetNode& pn : frame_ui.preset_nodes) {
                ui::Rect r = pn.node->rect;
                if (!pn.node->clip.empty()) r = r.intersect(pn.node->clip);
                if (r.w > 0.0f && r.contains(input.mouse)) {
                    app.preset_drag = static_cast<int>(pn.index);
                    app.preset_press = input.mouse;
                    app.preset_drag_live = false;
                }
            }
        }
        if (app.preset_drag >= 0 && input.left_down()) {
            const float dd =
                std::fabs(input.mouse.x - app.preset_press.x) +
                std::fabs(input.mouse.y - app.preset_press.y);
            if (dd > 6.0f) app.preset_drag_live = true;
            if (app.preset_drag_live &&
                static_cast<size_t>(app.preset_drag) <
                    app.presets.size()) {
                // Ghost label under the cursor while the drag is live.
                const char* pname =
                    app.presets[static_cast<size_t>(app.preset_drag)]
                        .name.c_str();
                const ui::Theme& th = ui::active_theme();
                const Vec2 ts = ui::measure_text(font, pname, 11.0f);
                const ui::Rect gr{input.mouse.x + 12.0f,
                                  input.mouse.y + 10.0f, ts.x + 14.0f,
                                  20.0f};
                canvas.draw_sdf_rect(gr, 4.0f, th.control_bg);
                canvas.draw_sdf_rect_outline(gr, 4.0f, 1.0f, th.accent_dim);
                ui::draw_text(canvas, font, pname,
                              {gr.x + 7.0f, gr.y + 4.5f}, 11.0f, th.text);
            }
        }
        if (app.preset_drag >= 0 && !input.left_down()) {
            const bool live = app.preset_drag_live;
            const size_t pi = static_cast<size_t>(app.preset_drag);
            app.preset_drag = -1;
            app.preset_drag_live = false;
            if (live && frame_ui.canvas_node &&
                frame_ui.canvas_node->rect.contains(input.mouse))
                spawn_preset(pi, /*at_cursor=*/true);
        }
        for (const auto& pc : frame_ui.preset_clicks) {
            if (!*pc.second || structure_done) continue;
            spawn_preset(pc.first, /*at_cursor=*/false);
            break;
        }
        if (frame_ui.tag_selected && *frame_ui.tag_selected >= 0)
            app.preset_tag_index = *frame_ui.tag_selected - 1;   // 0 = all
        if (frame_ui.preset_search_clicked && *frame_ui.preset_search_clicked) {
            app.preset_search_focus = !app.preset_search_focus;
            app.duration_focus = false;
            app.fx_search_focus = false;
        }
        if (frame_ui.fx_search_clicked && *frame_ui.fx_search_clicked) {
            app.fx_search_focus = !app.fx_search_focus;
            app.preset_search_focus = false;
            app.duration_focus = false;
        }
        if (frame_ui.duration_clicked && *frame_ui.duration_clicked) {
            app.duration_focus = !app.duration_focus;
            app.duration_edit.clear();
            if (app.duration_focus) app.preset_search_focus = false;
        }
        if (frame_ui.preset_import_clicked &&
            *frame_ui.preset_import_clicked) {
            auto picked = platform::show_open_dialog(
                window.get(),
                {{"looks preset", "*.json"}, {"all files", "*.*"}});
            if (picked) {
                bool is_preset = false;
                if (const auto bytes = read_file_bytes(*picked)) {
                    const auto parsed = json::parse(std::string_view(
                        reinterpret_cast<const char*>(bytes->data()),
                        bytes->size()));
                    is_preset = parsed.value &&
                                parsed.value->get("looks_preset").as_int(0) >=
                                    1;
                }
                if (is_preset) {
                    std::error_code ec;
                    std::filesystem::create_directories(app.user_preset_dir,
                                                        ec);
                    std::filesystem::copy_file(
                        *picked, app.user_preset_dir / picked->filename(),
                        std::filesystem::copy_options::overwrite_existing,
                        ec);
                    if (!ec) {
                        rescan_presets(app);
                        app.status = "imported preset " +
                                     picked->filename().string();
                    } else {
                        app.status = "preset import failed";
                    }
                } else {
                    app.status = "not a looks preset file";
                }
            }
        }

        // ---- modulation edits
        for (const FrameUi::RouteRow& row : frame_ui.route_rows) {
            const doc::ModRoute* route = nullptr;
            for (const doc::ModRoute& r : app.document.mod_routes)
                if (r.id == row.id) route = &r;
            if (!route) continue;
            if (*row.source_selected >= 0 &&
                *row.source_selected !=
                    static_cast<int>(route->source.type)) {
                doc::ModSource s = route->source;
                s.type =
                    static_cast<doc::ModSourceType>(*row.source_selected);
                app.undo.execute(app.document,
                                 doc::set_route_source_command(row.id, s));
            } else if (*row.shape_selected >= 0 &&
                       (route->source.type == doc::ModSourceType::Lfo ||
                        route->source.type ==
                            doc::ModSourceType::LfoBeat)) {
                doc::ModSource s = route->source;
                s.shape = static_cast<doc::LfoShape>(*row.shape_selected);
                app.undo.execute(app.document,
                                 doc::set_route_source_command(row.id, s));
            } else if (*row.shape_selected >= 0 &&
                       route->source.type ==
                           doc::ModSourceType::Envelope) {
                doc::ModSource s = route->source;
                s.trigger =
                    static_cast<uint32_t>(*row.shape_selected % 4);
                app.undo.execute(app.document,
                                 doc::set_route_source_command(row.id, s));
            } else if (*row.shape_selected >= 0 &&
                       (route->source.type ==
                            doc::ModSourceType::VideoSample ||
                        route->source.type ==
                            doc::ModSourceType::VideoRegion)) {
                doc::ModSource s = route->source;
                s.channel =
                    static_cast<uint32_t>(*row.shape_selected % 4);
                app.undo.execute(app.document,
                                 doc::set_route_source_command(row.id, s));
            } else if (*row.curve_selected >= 0) {
                app.undo.execute(
                    app.document,
                    doc::set_route_curve_command(
                        row.id, static_cast<doc::ResponseCurve>(
                                    *row.curve_selected)));
            }
            if (*row.rate_changed && *row.rate_staged != row.rate_original) {
                doc::ModSource s = route->source;
                if (s.type == doc::ModSourceType::Envelope ||
                    s.type == doc::ModSourceType::Beat)
                    s.decay = *row.rate_staged;   // slider = decay seconds
                else
                    s.rate_hz = *row.rate_staged;
                app.undo.execute(app.document,
                                 doc::set_route_source_command(row.id, s),
                                 /*coalesce=*/true);
            }
            if (*row.amount_changed &&
                *row.amount_staged != row.amount_original) {
                app.undo.execute(
                    app.document,
                    doc::set_route_amount_command(row.id, *row.amount_staged),
                    /*coalesce=*/true);
            }
            // Sampling-window drags (v4): px/py/pw/ph, one coalesced
            // source edit per changed knob.
            bool pos_released = false;
            for (int pi = 0; pi < 4; ++pi) {
                if (row.pos_released[pi] && *row.pos_released[pi])
                    pos_released = true;
                if (!row.pos_changed[pi] || !*row.pos_changed[pi] ||
                    *row.pos_staged[pi] == row.pos_original[pi])
                    continue;
                doc::ModSource s = route->source;
                float* f = pi == 0 ? &s.px
                    : pi == 1 ? &s.py
                    : pi == 2 ? &s.pw
                              : &s.ph;
                *f = *row.pos_staged[pi];
                app.undo.execute(app.document,
                                 doc::set_route_source_command(row.id, s),
                                 /*coalesce=*/true);
            }
            if ((*row.rate_released || *row.amount_released ||
                 pos_released) &&
                !did_break) {
                app.undo.break_coalescing();
                did_break = true;
            }
            if (*row.remove) {
                app.undo.execute(app.document,
                                 doc::remove_route_command(row.id));
                break;   // indices into mod_routes shifted; one per frame
            }
        }
        for (const FrameUi::AddRoute& add : frame_ui.add_routes) {
            if (!*add.clicked) continue;
            doc::ModRoute route;
            route.id = app.document.next_route_id++;
            route.target = add.key;
            route.amount = 0.25f;
            app.undo.execute(app.document, doc::add_route_command(route));
            break;
        }
        for (const FrameUi::KeyToggle& toggle : frame_ui.key_toggles) {
            if (!*toggle.clicked) continue;
            const double playhead = app.player.is_open()
                ? app.player.current_frame_index() : 0.0;
            std::vector<doc::Keyframe> keys;
            for (const doc::KeyframeLane& lane : app.document.lanes)
                if (lane.target == toggle.key) keys = lane.keys;
            if (toggle.lane_toggle && !keys.empty()) {
                // Canvas keyframe toggle OFF: the whole lane goes (the
                // timeline below is where individual keys are edited).
                app.undo.execute(app.document,
                                 doc::set_lane_command(toggle.key, {}));
                break;
            }
            bool removed = false;
            for (size_t i = 0; i < keys.size(); ++i) {
                if (std::fabs(keys[i].frame - playhead) < 0.5) {
                    keys.erase(keys.begin() + i);
                    removed = true;
                    break;
                }
            }
            if (!removed) {
                doc::Keyframe k;
                k.frame = playhead;
                k.value = toggle.value;
                keys.push_back(k);
            }
            app.undo.execute(app.document,
                             doc::set_lane_command(toggle.key, std::move(keys)));
            // Jump-to-lane (docs/flow_canvas.md v4): k on a card also
            // scrolls the timeline so the param's lane editor is in view
            // (rows are 42 px + 4 gap).
            for (size_t i = 0; i < app.document.lanes.size(); ++i)
                if (app.document.lanes[i].target == toggle.key)
                    app.timeline_scroll.offset =
                        static_cast<float>(i) * 46.0f;
            break;
        }
        for (FrameUi::LaneEdit& edit : frame_ui.lane_edits) {
            app.undo.execute(
                app.document,
                doc::set_lane_command(edit.target, std::move(edit.keys)),
                /*coalesce=*/true);
        }
        if (frame_ui.lane_release) app.undo.break_coalescing();

        // ---- mask edits
        if (frame_ui.add_mask_clicked && *frame_ui.add_mask_clicked) {
            doc::Mask mask;
            mask.id = app.document.next_mask_id++;
            mask.name = "mask " + std::to_string(mask.id);
            if (app.add_pos_valid) {
                mask.node_x = app.add_gx;
                mask.node_y = app.add_gy;
                app.add_pos_valid = false;
            }
            const uint64_t mid = mask.id;
            app.undo.execute(app.document,
                             doc::add_mask_command(std::move(mask)));
            app.sel = {SelKind::Mask, mid};
        }
        if (frame_ui.add_layer_open && *frame_ui.add_layer_open)
            app.sel = {SelKind::AddLayer, 0};
        if (frame_ui.open_add_clicked && *frame_ui.open_add_clicked) {
            if (app.document.layers.empty()) {
                app.sel = {SelKind::AddLayer, 0};
            } else {
                const size_t li = std::min(
                    app.selected_layer, app.document.layers.size() - 1);
                app.sel = {SelKind::AddEffect,
                           app.document.layers[li].id};
                app.insert_before_id = 0;
            }
        }
        if (frame_ui.add_frame_clicked && *frame_ui.add_frame_clicked) {
            doc::Document::Frame fr;
            fr.id = app.document.next_effect_id++;
            fr.title = "frame " + std::to_string(fr.id);
            if (app.add_pos_valid) {
                fr.x = app.add_gx;
                fr.y = app.add_gy;
                app.add_pos_valid = false;
            } else {
                fr.x = 80.0f;
                fr.y = 80.0f;
            }
            app.undo.execute(app.document,
                             doc::add_frame_command(std::move(fr)));
        }
        for (const FrameUi::EffectMaskCycle& pick :
             frame_ui.effect_mask_cycles) {
            if (*pick.selected < 0 || app.document.layers.empty() ||
                pick.fx_index >= app.document.layers[ui_layer].stack.size())
                continue;
            const int sel = *pick.selected;   // 0 = none, 1.. = mask index
            const uint64_t next =
                sel == 0 || static_cast<size_t>(sel) >
                                app.document.masks.size()
                    ? 0
                    : app.document.masks[static_cast<size_t>(sel - 1)].id;
            const uint64_t current =
                app.document.layers[ui_layer].stack[pick.fx_index].mask_id;
            if (next != current)
                app.undo.execute(app.document,
                                 doc::set_effect_mask_command(
                                     ui_layer, pick.fx_index, next));
            break;
        }
        // Rail selector dropdowns (v5.6): a pick becomes the param value.
        for (const FrameUi::ParamPick& pick : frame_ui.param_picks) {
            if (*pick.selected < 0 ||
                pick.layer_index >= app.document.layers.size() ||
                pick.fx_index >=
                    app.document.layers[pick.layer_index].stack.size())
                continue;
            const float next =
                pick.min_value + static_cast<float>(*pick.selected);
            app.undo.execute(app.document,
                             doc::set_param_command(pick.layer_index,
                                                    pick.fx_index,
                                                    pick.param_index, next));
        }
        // Rail text field (v5.6): open the shared inline editor.
        for (const FrameUi::TextEditOpen& open : frame_ui.text_edit_opens) {
            if (!*open.clicked) continue;
            app.text_edit_id = open.effect_id;
            app.text_edit_buf.clear();
            size_t tli = 0, tfi = 0;
            if (find_effect_by_id(app.document, open.effect_id, &tli, &tfi))
                app.text_edit_buf =
                    app.document.layers[tli].stack[tfi].text;
        }

        // ---- layer edits
        for (const FrameUi::LayerRow& lrow : frame_ui.layer_rows) {
            if (*lrow.select) app.selected_layer = lrow.index;
            const doc::Layer* layer = nullptr;
            for (const doc::Layer& l : app.document.layers)
                if (l.id == lrow.id) layer = &l;
            if (!layer) continue;
            if (*lrow.visible_changed) {
                doc::Layer edited = *layer;
                edited.visible = *lrow.visible_staged;
                app.undo.execute(app.document,
                                 doc::set_layer_props_command(edited));
            } else if (*lrow.blend_selected >= 0 &&
                       *lrow.blend_selected !=
                           static_cast<int>(layer->blend)) {
                doc::Layer edited = *layer;
                edited.blend =
                    static_cast<doc::BlendMode>(*lrow.blend_selected);
                app.undo.execute(app.document,
                                 doc::set_layer_props_command(edited));
            } else if (lrow.osc_shape_selected &&
                       *lrow.osc_shape_selected >= 0 &&
                       static_cast<uint32_t>(*lrow.osc_shape_selected) !=
                           layer->osc_shape) {
                doc::Layer edited = *layer;
                edited.osc_shape =
                    static_cast<uint32_t>(*lrow.osc_shape_selected);
                app.undo.execute(app.document,
                                 doc::set_layer_props_command(edited));
            } else if (lrow.mask_selected && *lrow.mask_selected >= 0) {
                const int sel = *lrow.mask_selected;   // 0 = none
                const uint64_t next =
                    sel == 0 || static_cast<size_t>(sel) >
                                    app.document.masks.size()
                        ? 0
                        : app.document.masks[static_cast<size_t>(sel - 1)].id;
                if (next != layer->mask_id) {
                    doc::Layer edited = *layer;
                    edited.mask_id = next;
                    app.undo.execute(app.document,
                                     doc::set_layer_props_command(edited));
                }
            } else if (lrow.xf_toggle && *lrow.xf_toggle) {
                // View state, no undo (like section folds).
                app.layer_ui[lrow.id].xf_open =
                    !app.layer_ui[lrow.id].xf_open;
            } else if (lrow.flip_h && *lrow.flip_h) {
                doc::Layer edited = *layer;
                edited.flip_h = !edited.flip_h;
                app.undo.execute(app.document,
                                 doc::set_layer_props_command(edited));
            } else if (lrow.flip_v && *lrow.flip_v) {
                doc::Layer edited = *layer;
                edited.flip_v = !edited.flip_v;
                app.undo.execute(app.document,
                                 doc::set_layer_props_command(edited));
            } else if (*lrow.remove) {
                // Zero layers is a valid document — the viewport shows the
                // raw source and the layers panel just offers the adders.
                app.undo.execute(app.document,
                                 doc::remove_layer_command(lrow.index));
                if (!app.document.layers.empty() &&
                    app.selected_layer >= app.document.layers.size())
                    app.selected_layer = app.document.layers.size() - 1;
                break;
            } else if (lrow.up && *lrow.up && lrow.index > 0) {
                app.undo.execute(app.document,
                                 doc::move_layer_command(lrow.index, -1));
                if (app.selected_layer == lrow.index)
                    app.selected_layer = lrow.index - 1;
                else if (app.selected_layer == lrow.index - 1)
                    app.selected_layer = lrow.index;
                break;
            } else if (lrow.down && *lrow.down &&
                       lrow.index + 1 < app.document.layers.size()) {
                app.undo.execute(app.document,
                                 doc::move_layer_command(lrow.index, 1));
                if (app.selected_layer == lrow.index)
                    app.selected_layer = lrow.index + 1;
                else if (app.selected_layer == lrow.index + 1)
                    app.selected_layer = lrow.index;
                break;
            }
        }
        for (const FrameUi::LayerStage& stage : frame_ui.layer_stages) {
            if (*stage.changed && *stage.staged != stage.original) {
                doc::Layer* layer = nullptr;
                for (doc::Layer& l : app.document.layers)
                    if (l.id == stage.layer_id) layer = &l;
                if (!layer) continue;
                doc::Layer edited = *layer;
                const float v = *stage.staged;
                using LF = FrameUi::LayerField;
                switch (stage.field) {
                    case LF::Opacity: edited.opacity = v; break;
                    case LF::ColorAR: edited.color_a[0] = v; break;
                    case LF::ColorAG: edited.color_a[1] = v; break;
                    case LF::ColorAB: edited.color_a[2] = v; break;
                    case LF::ColorBR: edited.color_b[0] = v; break;
                    case LF::ColorBG: edited.color_b[1] = v; break;
                    case LF::ColorBB: edited.color_b[2] = v; break;
                    case LF::Scale: edited.gen_scale = v; break;
                    case LF::Angle: edited.gen_angle = v; break;
                    case LF::CropL: edited.crop_l = v; break;
                    case LF::CropR: edited.crop_r = v; break;
                    case LF::CropT: edited.crop_t = v; break;
                    case LF::CropB: edited.crop_b = v; break;
                    case LF::XfScale: edited.xf_scale = v; break;
                    case LF::Rotate: edited.xf_rotate = v; break;
                    case LF::TrimIn:
                        edited.trim_in =
                            static_cast<uint32_t>(v + 0.5f);
                        break;
                    case LF::TrimOut:
                        edited.trim_out =
                            static_cast<uint32_t>(v + 0.5f);
                        break;
                }
                app.undo.execute(app.document,
                                 doc::set_layer_props_command(std::move(edited)),
                                 /*coalesce=*/true);
            }
            if (*stage.released && !did_break) {
                app.undo.break_coalescing();
                did_break = true;
            }
        }
        {
            static const doc::LayerSourceKind kAddKinds[7] = {
                doc::LayerSourceKind::Solid, doc::LayerSourceKind::Gradient,
                doc::LayerSourceKind::Noise, doc::LayerSourceKind::TestPattern,
                doc::LayerSourceKind::Oscillator,
                doc::LayerSourceKind::Shape,
                doc::LayerSourceKind::Clip};
            for (int t = 0; t < 7; ++t) {
                if (frame_ui.add_layer_clicked[t] &&
                    *frame_ui.add_layer_clicked[t] &&
                    app.document.layers.size() < doc::kMaxLayers) {
                    doc::Layer layer =
                        doc::make_layer(app.document, kAddKinds[t]);
                    app.undo.execute(app.document,
                                     doc::add_layer_command(
                                         std::move(layer),
                                         app.document.layers.size()));
                    app.selected_layer = app.document.layers.size() - 1;
                    break;
                }
            }
        }
        // ---- UI prefs (view state, no undo): section folds + theme.
        for (const FrameUi::SectionToggle& toggle :
             frame_ui.section_toggles) {
            if (!*toggle.clicked) continue;
            app.sec_open[toggle.index] = !app.sec_open[toggle.index];
            save_ui_prefs(app);
        }
        if (frame_ui.theme_selected && *frame_ui.theme_selected >= 0 &&
            *frame_ui.theme_selected != app.theme_index) {
            app.theme_index = *frame_ui.theme_selected;
            ui::set_active_theme(app.theme_index);
            save_ui_prefs(app);
        }
        if (frame_ui.add_fx_toggle && *frame_ui.add_fx_toggle)
            app.add_fx_open = !app.add_fx_open;
        for (int c = 0; c < 8; ++c)
            if (frame_ui.fx_cat_clicked[c] && *frame_ui.fx_cat_clicked[c])
                app.fx_cat_open[c] = !app.fx_cat_open[c];
        for (const FrameUi::MaskActions& actions : frame_ui.mask_actions) {
            const doc::Mask* mask =
                doc::find_mask(app.document, actions.mask_id);
            if (!mask) continue;
            if (*actions.view) {
                app.overlay_mask_id = app.overlay_mask_id == actions.mask_id
                    ? 0 : actions.mask_id;
            }
            if (*actions.type_selected >= 0 &&
                *actions.type_selected != static_cast<int>(mask->type)) {
                doc::Mask edited = *mask;
                edited.type =
                    static_cast<doc::MaskType>(*actions.type_selected);
                app.undo.execute(app.document,
                                 doc::set_mask_params_command(edited));
            } else if (*actions.extract_selected >= 0 &&
                       mask->type == doc::MaskType::Luma &&
                       *actions.extract_selected !=
                           static_cast<int>(mask->extract)) {
                doc::Mask edited = *mask;
                edited.extract = static_cast<doc::MaskExtract>(
                    *actions.extract_selected);
                app.undo.execute(app.document,
                                 doc::set_mask_params_command(edited));
            } else if (*actions.invert_changed) {
                doc::Mask edited = *mask;
                edited.invert = *actions.invert_staged;
                app.undo.execute(app.document,
                                 doc::set_mask_params_command(edited));
            } else if (*actions.chain_add) {
                app.undo.execute(
                    app.document,
                    doc::mask_chain_add_command(
                        actions.mask_id,
                        doc::make_effect(app.document,
                                         doc::EffectType::Pixelate)));
            } else if (actions.source_selected &&
                       *actions.source_selected >= 0) {
                // Decode the pick with the index layout the build recorded
                // (clip | layers | generators | file | pick video).
                const int sel = *actions.source_selected;
                doc::Mask edited = *mask;
                bool apply = false;
                if (sel == 0) {
                    apply = edited.source_gen != 0 ||
                            edited.source_layer_id != 0 ||
                            !edited.source_path.empty();
                    edited.source_gen = 0;
                    edited.source_layer_id = 0;
                    edited.source_path.clear();
                } else if (sel >= actions.src_layer_base &&
                           sel < actions.src_layer_base +
                                     actions.src_layer_count) {
                    const size_t li = static_cast<size_t>(
                        sel - actions.src_layer_base);
                    if (li < app.document.layers.size()) {
                        const uint64_t id = app.document.layers[li].id;
                        apply = edited.source_gen != 0 ||
                                edited.source_layer_id != id;
                        edited.source_gen = 0;
                        edited.source_layer_id = id;
                    }
                } else if (sel >= actions.src_gen_base &&
                           sel < actions.src_gen_base + 3) {
                    static const doc::LayerSourceKind kGens[3] = {
                        doc::LayerSourceKind::Noise,
                        doc::LayerSourceKind::Gradient,
                        doc::LayerSourceKind::TestPattern};
                    const uint32_t gen = static_cast<uint32_t>(
                        kGens[sel - actions.src_gen_base]);
                    apply = edited.source_gen != gen;
                    edited.source_gen = gen;
                    edited.source_layer_id = 0;
                } else if (sel == actions.src_file_index) {
                    apply = mask->source_gen != 0 ||
                            mask->source_layer_id != 0;
                    edited.source_gen = 0;
                    edited.source_layer_id = 0;
                } else if (sel == actions.src_pick_index) {
                    auto picked = platform::show_open_dialog(
                        window.get(),
                        {{"mezzanine video", "*.mez"}, {"all files", "*.*"}});
                    if (picked) {
                        edited.source_path = picked->string();
                        edited.source_gen = 0;
                        edited.source_layer_id = 0;
                        apply = true;
                    }
                }
                if (apply)
                    app.undo.execute(app.document,
                                     doc::set_mask_params_command(edited));
            } else if (actions.combine_selected &&
                       *actions.combine_selected >= 0) {
                // [none, every other mask] in document order (self skipped).
                const int sel = *actions.combine_selected;
                uint64_t next = 0;
                if (sel > 0) {
                    int i = 0;
                    for (const doc::Mask& other : app.document.masks) {
                        if (other.id == mask->id) continue;
                        if (++i == sel) {
                            next = other.id;
                            break;
                        }
                    }
                }
                if (next != mask->combine_id) {
                    doc::Mask edited = *mask;
                    edited.combine_id = next;
                    app.undo.execute(app.document,
                                     doc::set_mask_params_command(edited));
                }
            } else if (actions.combine_op_selected &&
                       *actions.combine_op_selected >= 0 &&
                       *actions.combine_op_selected !=
                           static_cast<int>(mask->combine_op)) {
                doc::Mask edited = *mask;
                edited.combine_op = static_cast<doc::MaskCombineOp>(
                    *actions.combine_op_selected);
                app.undo.execute(app.document,
                                 doc::set_mask_params_command(edited));
            } else if (actions.fit_selected && *actions.fit_selected >= 0 &&
                       *actions.fit_selected !=
                           static_cast<int>(mask->fit % 3)) {
                doc::Mask edited = *mask;
                edited.fit = static_cast<uint32_t>(*actions.fit_selected);
                app.undo.execute(app.document,
                                 doc::set_mask_params_command(edited));
            } else if (actions.freerun_changed && *actions.freerun_changed) {
                doc::Mask edited = *mask;
                edited.free_run = *actions.freerun_staged;
                app.undo.execute(app.document,
                                 doc::set_mask_params_command(edited));
            } else if (actions.key_points && *actions.key_points) {
                // Whole-shape key at the playhead: every point coordinate
                // gets a key at its lane-resolved value; if the shape is
                // already fully keyed on this frame, remove that key.
                const double playhead = app.player.is_open()
                    ? app.player.current_frame_index() : 0.0;
                const uint64_t key_id = actions.mask_id | doc::kMaskParamBit;
                std::vector<doc::KeyframeLane> lanes;
                bool all_keyed = true;
                for (size_t s = 0; s < mask->points.size(); ++s) {
                    doc::KeyframeLane lane;
                    lane.target = {key_id, doc::kMaskPointParamBase +
                                               static_cast<int>(s)};
                    for (const doc::KeyframeLane& l : app.document.lanes)
                        if (l.target == lane.target) lane.keys = l.keys;
                    bool keyed = false;
                    for (const doc::Keyframe& k : lane.keys)
                        if (std::fabs(k.frame - playhead) < 0.5) keyed = true;
                    all_keyed = all_keyed && keyed;
                    lanes.push_back(std::move(lane));
                }
                for (size_t s = 0; s < lanes.size(); ++s) {
                    auto& keys = lanes[s].keys;
                    if (all_keyed) {
                        for (size_t i = 0; i < keys.size(); ++i) {
                            if (std::fabs(keys[i].frame - playhead) < 0.5) {
                                keys.erase(keys.begin() +
                                           static_cast<ptrdiff_t>(i));
                                break;
                            }
                        }
                        continue;
                    }
                    const float value = keys.empty()
                        ? mask->points[s]
                        : std::clamp(mod::eval_lane(lanes[s], playhead),
                                     0.0f, 1.0f);
                    bool updated = false;
                    for (doc::Keyframe& k : keys) {
                        if (std::fabs(k.frame - playhead) < 0.5) {
                            k.value = value;
                            updated = true;
                            break;
                        }
                    }
                    if (!updated) {
                        doc::Keyframe k;
                        k.frame = playhead;
                        k.value = value;
                        keys.push_back(k);
                    }
                }
                app.undo.execute(app.document,
                                 doc::set_lanes_command(std::move(lanes)));
            } else if (*actions.remove) {
                if (app.overlay_mask_id == actions.mask_id)
                    app.overlay_mask_id = 0;
                app.undo.execute(app.document,
                                 doc::remove_mask_command(actions.mask_id));
                break;
            }
        }
        for (const FrameUi::MaskStage& stage : frame_ui.mask_stages) {
            if (!*stage.changed || *stage.staged == stage.original) {
                if (*stage.released && !did_break) {
                    app.undo.break_coalescing();
                    did_break = true;
                }
                continue;
            }
            const doc::Mask* mask = doc::find_mask(app.document, stage.mask_id);
            if (!mask) continue;
            doc::Mask edited = *mask;
            const float v = *stage.staged;
            using MF = FrameUi::MaskField;
            switch (stage.field) {
                case MF::CenterX: edited.center_x = v; break;
                case MF::CenterY: edited.center_y = v; break;
                case MF::RadiusX: edited.radius_x = v; break;
                case MF::RadiusY: edited.radius_y = v; break;
                case MF::Roundness: edited.roundness = v; break;
                case MF::Feather: edited.feather = v; break;
                case MF::BlackPoint: edited.black_point = v; break;
                case MF::WhitePoint: edited.white_point = v; break;
                case MF::Gamma: edited.gamma = v; break;
                case MF::KeyCenter: edited.key_center = v; break;
                case MF::KeyRange: edited.key_range = v; break;
                case MF::KeyR: edited.key_r = v; break;
                case MF::KeyG: edited.key_g = v; break;
                case MF::KeyB: edited.key_b = v; break;
                case MF::BlurPx: edited.blur_px = v; break;
                case MF::GrowPx: edited.grow_px = v; break;
                case MF::GenScale: edited.gen_scale = v; break;
                case MF::GenAngle: edited.gen_angle = v; break;
            }
            app.undo.execute(app.document,
                             doc::set_mask_params_command(std::move(edited)),
                             /*coalesce=*/true);
            if (*stage.released && !did_break) {
                app.undo.break_coalescing();
                did_break = true;
            }
        }
        for (const FrameUi::MaskChainStage& stage :
             frame_ui.mask_chain_stages) {
            if (*stage.changed && *stage.staged != stage.original) {
                app.undo.execute(app.document,
                                 doc::mask_chain_set_param_command(
                                     stage.mask_id, stage.chain_index,
                                     stage.param_index, *stage.staged),
                                 /*coalesce=*/true);
            }
            if (*stage.released && !did_break) {
                app.undo.break_coalescing();
                did_break = true;
            }
        }
        for (const FrameUi::MaskChainRemove& remove :
             frame_ui.mask_chain_removes) {
            if (*remove.clicked) {
                app.undo.execute(app.document,
                                 doc::mask_chain_remove_command(
                                     remove.mask_id, remove.chain_index));
                break;
            }
        }
        for (int s = 0; s < 3; ++s) {
            if (frame_ui.snap_store_clicked[s] &&
                *frame_ui.snap_store_clicked[s])
                app.undo.execute(app.document, doc::store_snapshot_command(s));
            if (frame_ui.snap_apply_clicked[s] &&
                *frame_ui.snap_apply_clicked[s] &&
                app.document.snapshots[s].valid)
                app.undo.execute(app.document, doc::apply_snapshot_command(s));
        }

        if ((frame_ui.undo_clicked && *frame_ui.undo_clicked) || do_undo)
            app.undo.undo(app.document);
        if ((frame_ui.redo_clicked && *frame_ui.redo_clicked) || do_redo)
            app.undo.redo(app.document);

        // ---- project save / open (spec §10)
        if ((frame_ui.save_clicked && *frame_ui.save_clicked) || do_save ||
            do_save_as) {
            std::filesystem::path path = app.project_path;
            if (path.empty() || do_save_as) {
                auto picked = platform::show_save_dialog(
                    window.get(), {{"looks project", "*.json"}},
                    app.document.name + ".json");
                if (picked) {
                    if (picked->extension() != ".json")
                        picked->replace_extension(".json");
                    path = *picked;
                } else {
                    path.clear();
                }
            }
            if (!path.empty()) save_project(app, path);
        }
        if ((frame_ui.open_project_clicked &&
             *frame_ui.open_project_clicked) ||
            do_open_project) {
            auto picked = platform::show_open_dialog(
                window.get(),
                {{"looks project", "*.json"}, {"all files", "*.*"}});
            if (picked) open_project(app, *picked);
        }
        // Autosave: once a project has a path, dirty documents snapshot to
        // <name>.autosave.json at most once a minute.
        if (!app.project_path.empty() &&
            app.document.revision != app.autosaved_revision &&
            now - app.last_autosave > std::chrono::seconds(60)) {
            std::filesystem::path auto_path = app.project_path;
            auto_path.replace_extension(".autosave.json");
            if (doc::save_document(auto_path, app.document))
                app.autosaved_revision = app.document.revision;
            app.last_autosave = now;
        }

        // ---- transport
        if (frame_ui.open_clicked && *frame_ui.open_clicked && !app.import) {
            auto picked = platform::show_open_dialog(
                window.get(),
                {{"video / image", "*.mp4;*.mov;*.mez;*.png;*.tga"},
                 {"all files", "*.*"}});
            if (picked) open_source(app, *picked);
        }
        // Drag-and-drop: video files open like the dialog would; preset
        // files import into the browser; other .json loads as a project;
        // a PNG installs a custom glyph set (spec §12).
        if (!dropped_file.empty() && !app.import) {
            const std::filesystem::path p(dropped_file);
            const auto ext = p.extension();
            if (ext == ".json") {
                bool is_preset = false;
                if (const auto bytes = read_file_bytes(p)) {
                    const auto parsed = json::parse(std::string_view(
                        reinterpret_cast<const char*>(bytes->data()),
                        bytes->size()));
                    is_preset = parsed.value &&
                                parsed.value->get("looks_preset").as_int(0) >=
                                    1;
                }
                if (is_preset) {
                    std::error_code ec;
                    std::filesystem::create_directories(app.user_preset_dir,
                                                        ec);
                    std::filesystem::copy_file(
                        p, app.user_preset_dir / p.filename(),
                        std::filesystem::copy_options::overwrite_existing,
                        ec);
                    if (!ec) {
                        rescan_presets(app);
                        app.status =
                            "imported preset " + p.filename().string();
                    } else {
                        app.status = "preset import failed";
                    }
                } else {
                    open_project(app, p);
                }
            } else if (ext == ".mp4" || ext == ".mov" || ext == ".mez" ||
                       ext == ".tga") {
                open_source(app, p);
            } else if (ext == ".png") {
                // A PNG with a sibling .json grid descriptor ({"tile": 8,
                // "cols": 16, "rows": 6}) installs as a custom glyph set
                // (spec §12); a bare PNG opens as a still clip.
                std::filesystem::path desc = p;
                desc.replace_extension(".json");
                std::error_code dec;
                ImageRgba img;
                if (!std::filesystem::exists(desc, dec)) {
                    open_source(app, p);
                } else if (load_image(p, &img) && img.width > 0) {
                    float tile = 8.0f;
                    uint32_t cols = 16;
                    if (const auto dbytes = read_file_bytes(desc)) {
                        const auto parsed =
                            json::parse(std::string_view(
                                reinterpret_cast<const char*>(
                                    dbytes->data()),
                                dbytes->size()));
                        if (parsed.value) {
                            tile = static_cast<float>(
                                parsed.value->get("tile").as_number(8.0));
                            cols = static_cast<uint32_t>(
                                parsed.value->get("cols").as_int(16));
                        }
                    }
                    tile = std::max(1.0f, tile);
                    cols = std::max(
                        1u, std::min(cols, img.width /
                                               static_cast<uint32_t>(tile)));
                    const uint32_t rows = std::max(
                        1u, img.height / static_cast<uint32_t>(tile));
                    // Color tilesets (emoji, spec §6.6): any real chroma
                    // in the PNG keeps it RGBA — coverage comes from
                    // alpha, tiles keep their own hue. Monochrome art
                    // collapses to the gray ramp as before.
                    bool is_color = false;
                    for (size_t i = 0; i < static_cast<size_t>(img.width) *
                                               img.height && !is_color;
                         ++i) {
                        const uint8_t* px = img.pixels.data() + i * 4;
                        const int r = px[0], g = px[1], b = px[2];
                        if (std::abs(r - g) > 8 || std::abs(g - b) > 8)
                            is_color = true;
                    }
                    // The worker owns the engine — hand the atlas over;
                    // it installs it before the next render.
                    if (is_color) {
                        render_worker.post_glyph_atlas(
                            std::move(img.pixels), img.width, img.height,
                            tile, cols, rows, /*is_color=*/true);
                    } else {
                        std::vector<uint8_t> gray(
                            static_cast<size_t>(img.width) * img.height);
                        for (size_t i = 0; i < gray.size(); ++i) {
                            const uint8_t* px = img.pixels.data() + i * 4;
                            gray[i] = static_cast<uint8_t>(
                                (54u * px[0] + 183u * px[1] +
                                 19u * px[2]) >> 8);
                        }
                        render_worker.post_glyph_atlas(std::move(gray),
                                                       img.width,
                                                       img.height, tile,
                                                       cols, rows);
                    }
                    app.status = "custom glyph set: " +
                                 p.filename().string() +
                                 (is_color ? " (set 2, color)"
                                           : " (set 2)");
                } else {
                    app.status = "png decode failed";
                }
            }
        }
        if (((frame_ui.play_clicked && *frame_ui.play_clicked) ||
             toggle_play) &&
            app.player.is_open()) {
            if (app.player.playing()) app.player.pause();
            else app.player.play();
        }
        if (frame_ui.seek_changed && *frame_ui.seek_changed &&
            frame_ui.seek_staged) {
            app.player.seek_frame(
                static_cast<uint32_t>(*frame_ui.seek_staged + 0.5f));
        }
        if (frame_ui.seek_to >= 0.0f && app.player.is_open())
            app.player.seek_frame(
                static_cast<uint32_t>(frame_ui.seek_to + 0.5f));
        // Timeline region edits (spec §9): ruler trim handles + loop band.
        if (frame_ui.trim_in_to >= 0.0f || frame_ui.trim_out_to >= 0.0f ||
            frame_ui.loop_in_to >= 0.0f || frame_ui.loop_clear) {
            uint32_t t_in = app.document.clip_trim_in;
            uint32_t t_out = app.document.clip_trim_out;
            uint32_t l_in = app.document.loop_in;
            uint32_t l_out = app.document.loop_out;
            if (frame_ui.trim_in_to >= 0.0f)
                t_in = static_cast<uint32_t>(frame_ui.trim_in_to + 0.5f);
            if (frame_ui.trim_out_to >= 0.0f)
                t_out = static_cast<uint32_t>(frame_ui.trim_out_to + 0.5f);
            if (frame_ui.loop_in_to >= 0.0f) {
                l_in = static_cast<uint32_t>(frame_ui.loop_in_to + 0.5f);
                l_out = static_cast<uint32_t>(frame_ui.loop_out_to + 0.5f);
            }
            if (frame_ui.loop_clear) {
                l_in = 0;
                l_out = 0;
            }
            // Dragging the out handle back to the clip end stores 0
            // ("full") so untouched projects stay byte-stable.
            if (app.player.is_open() && t_out >= app.player.frame_count())
                t_out = 0;
            app.undo.execute(app.document,
                             doc::set_timeline_region_command(t_in, t_out,
                                                              l_in, l_out),
                             /*coalesce=*/true);
        }
        if (frame_ui.region_released) app.undo.break_coalescing();
        for (const FrameUi::LaneLoop& ll : frame_ui.lane_loops) {
            if (!*ll.clicked) continue;
            for (const doc::KeyframeLane& lane : app.document.lanes) {
                if (!(lane.target == ll.target)) continue;
                app.undo.execute(app.document,
                                 doc::set_lane_loop_command(ll.target,
                                                            !lane.loop));
                break;
            }
        }
        for (const FrameUi::LaneMute& lm : frame_ui.lane_mutes) {
            if (!*lm.clicked) continue;
            for (const doc::KeyframeLane& lane : app.document.lanes) {
                if (!(lane.target == lm.target)) continue;
                app.undo.execute(app.document,
                                 doc::set_lane_mute_command(lm.target,
                                                            !lane.muted));
                break;
            }
        }
        for (const FrameUi::LaneKill& lk : frame_ui.lane_kills) {
            if (!*lk.clicked) continue;
            // Empty keys = remove the lane (set_lane_command semantics).
            app.undo.execute(app.document,
                             doc::set_lane_command(lk.target, {}));
            break;
        }
        // Keep the player inside the document's region (cheap when
        // unchanged; set_trim also snaps the cursor into range).
        if (app.player.is_open()) {
            const uint32_t fcount = app.player.frame_count();
            const uint32_t t_in = std::min(app.document.clip_trim_in,
                                           fcount ? fcount - 1 : 0u);
            const uint32_t t_out =
                app.document.clip_trim_out
                    ? std::min(app.document.clip_trim_out, fcount)
                    : fcount;
            if (t_in != app.player.trim_in() ||
                t_out != app.player.trim_out())
                app.player.set_trim(t_in, t_out);
            app.player.set_loop_region(app.document.loop_in,
                                       app.document.loop_out);
            app.player.set_audio_offset(
                static_cast<double>(app.document.audio_offset_ms) * 0.001);
        }
        sync_sidechain(app);
        // Half-res proxy (spec §3): swap the player between the full and
        // proxy mezzanine when the toggle disagrees with reality.
        if (app.player.is_open()) {
            std::filesystem::path proxy = app.mez_path;
            proxy.replace_extension(".proxy.mez");
            std::error_code pec;
            const bool want = app.document.use_proxy &&
                              std::filesystem::exists(proxy, pec);
            if (want != app.proxy_active) {
                const uint32_t at = app.player.current_frame_index();
                const bool was_playing = app.player.playing();
                std::string err;
                render_worker.pause();
                if (app.player.open(want ? proxy : app.mez_path,
                                    app.pcm_path, &err)) {
                    app.proxy_active = want;
                    app.player.set_looping(app.loop);
                    app.player.seek_frame(at);
                    if (was_playing) app.player.play();
                } else {
                    app.status = "proxy open failed: " + err;
                }
                render_worker.resume();
            }
        }
        // Thumbnail strip (spec §3): register the staged RGBA once.
        if (app.thumbs_dirty) {
            app.thumbs_dirty = false;
            if (app.thumbs_count > 0)
                app.thumbs_tex = ui_renderer->register_image(
                    app.thumbs_rgba.data(), app.thumbs_w * app.thumbs_count,
                    app.thumbs_h);
            app.thumbs_rgba.clear();
            app.thumbs_rgba.shrink_to_fit();
        }
        if (frame_ui.loop_clicked && *frame_ui.loop_clicked) {
            app.loop = !app.loop;
            app.player.set_looping(app.loop);
        }
        if (frame_ui.proxy_selected && *frame_ui.proxy_selected >= 0)
            app.preview_div = *frame_ui.proxy_selected == 0
                                  ? 1u
                                  : (*frame_ui.proxy_selected == 1 ? 2u : 4u);
        if (frame_ui.ab_clicked && *frame_ui.ab_clicked)
            app.ab_wipe = !app.ab_wipe;
        if (frame_ui.bypass_all_clicked && *frame_ui.bypass_all_clicked)
            app.bypass_all = !app.bypass_all;
        if (frame_ui.wipe_changed && *frame_ui.wipe_changed &&
            frame_ui.wipe_staged)
            app.wipe_pos = *frame_ui.wipe_staged;
        if (frame_ui.live_clicked && *frame_ui.live_clicked) {
            app.live_mode = !app.live_mode;
            if (app.live_mode) {
                app.loop = true;
                app.player.set_looping(true);
                if (!app.player.playing()) app.player.play();
            }
        }
        if (frame_ui.export_clicked && *frame_ui.export_clicked &&
            app.player.is_open()) {
            std::filesystem::path default_name(app.clip_name);
            default_name.replace_extension("");
            auto out = platform::show_save_dialog(
                window.get(), {{"MP4 video", "*.mp4"}},
                default_name.string() + "_look.mp4");
            if (out) {
                if (out->extension() != ".mp4") out->replace_extension(".mp4");
                // Sidechain mux (spec §7): export the sidechain's audio
                // instead of the clip's when asked (and available).
                const std::filesystem::path export_pcm =
                    app.document.sidechain_mux && app.sc_ok
                        ? app.sc_pcm_path
                        : app.pcm_path;
                if (!app.export_job) {
                    app.export_job = start_export(
                        renderer->device(), shader_dir, app.mez_path,
                        export_pcm, app.document,
                        app.has_analysis ? &app.analysis : nullptr, *out);
                } else {
                    // Render queue (spec §9): snapshot now, render later.
                    AppState::QueuedExport q;
                    q.out_path = *out;
                    q.doc = app.document;
                    q.has_analysis = app.has_analysis;
                    if (app.has_analysis) q.analysis = app.analysis;
                    q.mez_path = app.mez_path;
                    q.pcm_path = export_pcm;
                    app.export_queue.push_back(std::move(q));
                }
            }
        }
        for (const FrameUi::QueueRow& qrow : frame_ui.queue_rows) {
            if (!*qrow.remove) continue;
            if (qrow.index < app.export_queue.size())
                app.export_queue.erase(app.export_queue.begin() +
                                       static_cast<ptrdiff_t>(qrow.index));
            break;   // indices shifted; one removal per frame
        }

        // ---- preview (spec §13 render thread): post the latest snapshot
        // and sample the newest published frame. Decode, time remap,
        // modulation resolve, and the whole graph evaluation happen on the
        // worker — a heavy stack never stalls this loop.
        push_render_job(render_worker, app);
        const RenderWorker::View rview =
            render_worker.acquire(app.ui_frame_counter);
        gfx::GpuImage* final_image = rview.final_img;
        gfx::GpuImage* source_image = rview.source_img;

        // Window clear = the active theme's background (stored linear).
        const ui::Color wbg = ui::active_theme().window_bg;
        VkClearColorValue clear{{wbg.r, wbg.g, wbg.b, 1.0f}};
        renderer->begin_present_pass(frame, clear);

        if (final_image) {
            ui_view_arena.reset(frame.frame_index);
            const ui::Rect r = frame_ui.preview->rect.inset(1.0f);
            const float px = r.x * scale, py = r.y * scale;
            const float pw = r.w * scale, ph = r.h * scale;
            if (app.bypass_all && source_image) {
                viewport_pass->draw(frame.cmd, ui_view_arena,
                                    frame.frame_index, *source_image,
                                    ui_view_sampler, frame.extent,
                                    px, py, pw, ph);
            } else if (app.ab_wipe && source_image) {
                // Before left of the split, after right of it (spec §9).
                viewport_pass->draw(frame.cmd, ui_view_arena,
                                    frame.frame_index, *source_image,
                                    ui_view_sampler, frame.extent,
                                    px, py, pw, ph, 0.0f, app.wipe_pos);
                viewport_pass->draw(frame.cmd, ui_view_arena,
                                    frame.frame_index, *final_image,
                                    ui_view_sampler, frame.extent,
                                    px, py, pw, ph, app.wipe_pos, 1.0f);
            } else {
                viewport_pass->draw(frame.cmd, ui_view_arena,
                                    frame.frame_index, *final_image,
                                    ui_view_sampler, frame.extent,
                                    px, py, pw, ph);
            }
        }

        // Deferred tooltip: drawn last so it overlays every widget.
        if (const char* tip = ctx.tooltip()) {
            const ui::Theme& th = ui::active_theme();
            const Vec2 ts = ui::measure_text(font, tip, th.font_size_small);
            const float pad = 6.0f;
            Vec2 pos = ctx.tooltip_pos();
            const float w = ts.x + pad * 2.0f;
            const float h = font.line_height() * th.font_size_small +
                            pad * 2.0f - 4.0f;
            pos.x = std::min(pos.x, viewport.w - w - 4.0f);
            pos.y = std::min(pos.y, viewport.h - h - 4.0f);
            const ui::Rect bg{pos.x, pos.y, w, h};
            canvas.draw_sdf_rect(bg, th.corner_radius, th.control_bg);
            canvas.draw_sdf_rect_outline(bg, th.corner_radius,
                                         th.stroke_width, th.hairline);
            ui::draw_text(canvas, font, tip,
                          {pos.x + pad, pos.y + pad - 2.0f},
                          th.font_size_small, th.text);
            ctx.clear_tooltip();
        }

        ui_renderer->record(frame.cmd, frame.frame_index, frame.extent, canvas);
        renderer->end_frame(frame);
    }

    // Stop the render thread before its device resources unwind.
    app.render_worker = nullptr;
    render_worker.stop();
    vkDestroySampler(renderer->device().device(), ui_view_sampler, nullptr);
    return 0;
}

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
    };

    struct View {
        gfx::GpuImage* final_img = nullptr;    // SHADER_READ_ONLY or null
        gfx::GpuImage* source_img = nullptr;   // when want_source was set
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
        uint32_t w = 0, h = 0;
        bool has_source = false;
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
            overlay_mask_id = job_.overlay_mask_id;
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

        const doc::Document resolved = mod::resolve(
            doc, mod_frame, mod_fps, has_analysis ? &analysis : nullptr,
            live_mode ? app_seconds : -1.0, live_mode ? env_key_time : -1.0);
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
            lsrc.empty() ? nullptr : lsrc.data(), lsrc.size());
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
            // timestep on frame index).
            const doc::Document resolved = mod::resolve(
                doc_copy, abs_f, fps, has_analysis ? &curves_copy : nullptr);
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
    ui::ButtonState route_buttons[18], key_buttons[18], macro_buttons[18];
    ui::ButtonState group_button, rnd_button;
    ui::ButtonState solo_button, copy_button;
    ui::DropdownState mask_dd;
};

struct GroupUiState {
    ui::ButtonState fold_button, ungroup_button, save_button, add_macro_button;
    ui::ButtonState bypass_check;
    ui::SliderState macro_sliders[4];
    ui::ButtonState macro_remove[4];
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
    ui::DropdownState blend_dd, mask_dd;
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
    ui::ButtonState loop_button;   // loopable region chip (spec §7)
};

struct RulerState {
    // 0 idle, 1 scrub, 2 trim-in handle, 3 trim-out handle, 4 loop band.
    int drag_mode = 0;
    double loop_anchor = 0.0;   // frame where the loop drag started
};

struct AppState {
    doc::Document document;
    doc::UndoStack undo;

    media::Player player;
    std::unique_ptr<ImportJob> import;
    std::unique_ptr<ExportJob> export_job;
    std::filesystem::path mez_path, pcm_path;   // current clip bundle
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

    // Project file (spec §10). autosaved_revision tracks what the last
    // autosave captured so quiet frames cost nothing.
    std::filesystem::path project_path;
    uint64_t saved_revision = 0;
    uint64_t autosaved_revision = 0;
    std::chrono::steady_clock::time_point last_autosave =
        std::chrono::steady_clock::now();

    // Preset browser (spec §10): shipped era presets + user-saved ones.
    std::filesystem::path shipped_preset_dir, user_preset_dir;
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
    ui::ButtonState add_layer_buttons[5];
    std::unordered_map<uint64_t, EffectUiState> fx_ui;
    std::unordered_map<uint64_t, RouteUiState> route_ui;
    std::unordered_map<uint64_t, MaskUiState> mask_ui;
    ui::ButtonState add_mask_button;
    std::map<std::pair<uint64_t, int>, LaneUiState> lane_ui;
    ui::ButtonState open_button, open_big_button, play_button, undo_button,
        redo_button, export_button;
    ui::ButtonState add_buttons[static_cast<size_t>(doc::EffectType::Count)];
    ui::ButtonState snap_apply[3], snap_store[3];
    ui::SliderState seek_slider;
    ui::ScrubberState scrubber;
    ui::ButtonState loop_check;
    ui::ScrollState sidebar_scroll, right_scroll;
    RulerState ruler;
};

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
    size_t fx_index;
    int param_index;
    float* staged;
    float original;
    bool* changed;
    bool* released;
};

struct FxRowActions {
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
        bool* remove;
        bool* up = nullptr;     // swap toward index 0 (bottom of composite)
        bool* down = nullptr;
        bool* xf_toggle = nullptr;   // fold/unfold the transform section
        bool* flip_h = nullptr;      // toggle clicks (spec §5 transform)
        bool* flip_v = nullptr;
    };
    std::vector<LayerRow> layer_rows;
    // solid, gradient, noise, test pattern, adjustment
    bool* add_layer_clicked[5] = {};

    // Group + preset staging.
    struct GroupStage {
        uint64_t group_id;
        size_t macro_index;
        float* staged;
        float original;
        bool* changed;
        bool* released;
    };
    std::vector<GroupStage> group_stages;
    struct GroupActions {
        uint64_t group_id;
        bool* fold;
        bool* bypass_changed;
        bool* bypass_staged;
        bool* ungroup;
        bool* save;
        bool* add_macro;
        bool* macro_remove[4] = {};
    };
    std::vector<GroupActions> group_actions;
    struct AddMacroTarget {
        uint64_t fx_id;
        int param_index;
        bool* clicked;
    };
    std::vector<AddMacroTarget> add_macro_targets;
    std::vector<std::pair<size_t, bool*>> preset_clicks;  // presets[] index
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
    struct EffectMaskCycle {
        size_t fx_index;
        int* selected;   // dropdown pick into [none, masks...]; -1 = none
    };
    std::vector<EffectMaskCycle> effect_mask_cycles;
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
                frame.canvas.draw_line(prev, p, 1.0f, theme.accent_dim);
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
                                   selected ? theme.text : theme.accent);
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

    // ---- interaction (queued into FrameUi, applied post-frame).
    const ui::WidgetId id = frame.ctx.acquire_widget_id(&state);
    const bool owns = frame.ctx.widget_owns_mouse(id);
    const Vec2 mouse = frame.input.mouse;
    const bool dragging =
        state.dragging_key || state.dragging_in || state.dragging_out;

    auto emit = [&](std::vector<doc::Keyframe> new_keys) {
        u->out->lane_edits.push_back({u->target, std::move(new_keys)});
    };

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
                // Add a key at the click point.
                doc::Keyframe k;
                k.frame = std::round(from_x(mouse.x));
                k.value = from_y(mouse.y);
                std::vector<doc::Keyframe> edited = keys;
                edited.push_back(k);
                state.selected = -1;
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

    // A freshly added key becomes selected once the doc has it (next frame);
    // meanwhile dragging with selected == -1 finds the nearest key.
    if (state.dragging_key && state.selected < 0 && !keys.empty()) {
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

    if (dragging && frame.input.left_released()) {
        state.dragging_key = state.dragging_in = state.dragging_out = false;
        frame.ctx.clear_capture();
        u->out->lane_release = true;
    }
}

// One parameter row on the design grid: [mod gutter ~ k][label][slider].
// Rows without mod targets get a blank gutter so every slider in a panel
// starts on the same column; `macro_*` appends the group-macro micro.
ui::LayoutNode* param_row(ui::LayoutArena& arena, const char* label,
                          ui::LayoutNode* slider,
                          ui::ButtonState* route_state, bool* route_clicked,
                          ui::ButtonState* key_state, bool* key_clicked,
                          ui::ButtonState* macro_state = nullptr,
                          bool* macro_clicked = nullptr) {
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
        if (macro_clicked) {
            micro.tooltip = "add to the group's macro knob";
            cells.push_back(IconButton(arena, Icon::Knob, macro_state,
                                       macro_clicked, micro));
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
    // grouped effects leave on "g". "m" next to a param adds it to the
    // group's newest macro knob.
    const doc::Group* fx_group = nullptr;
    for (const doc::Group& g :
         app.document.layers[app.selected_layer].groups)
        if (g.id == fx.group_id) fx_group = &g;
    const bool can_macro = fx_group && !fx_group->macros.empty();

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
                            SliderState* slider_state) {
        ParamStage stage{};
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
        ui::ButtonState* macro_state = nullptr;
        bool* macro_clicked = nullptr;
        if (can_macro) {
            FrameUi::AddMacroTarget target{fx.id, param_index,
                                           arena.alloc<bool>()};
            macro_state = &state.macro_buttons[o];
            macro_clicked = target.clicked;
            out.add_macro_targets.push_back(target);
        }
        rows.push_back(param_row(
            arena, label,
            SliderF(arena, stage.staged, min_v, max_v, slider_state, opts),
            &state.route_buttons[o], add_route.clicked,
            &state.key_buttons[o], key_toggle.clicked, macro_state,
            macro_clicked));
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
        stage_slider(static_cast<int>(p), desc.label, desc.min_value,
                     desc.max_value, fx.params[p], desc.format,
                     &state.params[p]);
    }

    out.rows.push_back(row);

    StackOpts column;
    column.gap = 4.0f;
    column.cross_align = AlignMode::Stretch;
    return Panel(arena, VStackDyn(arena, column, rows),
                 PanelOpts{Edges::all(8), -1.0f, /*outline=*/false});
}

// Group container (spec §5): ONE outlined panel holding the header
// (fold/eye/save/ungroup), the macro knobs, and the member effect cards
// nested inside with an indent — grouping is containment, not a floating
// header. Folded, the container collapses to header + macros.
ui::LayoutNode* build_group_panel(ui::LayoutArena& arena, AppState& app,
                                  FrameUi& out, const doc::Group& group,
                                  const std::vector<size_t>& members) {
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
    actions.add_macro = arena.alloc<bool>();

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

    // Macros: one grid row each — [knob icon-ish gutter][name][slider][x].
    for (size_t m = 0; m < group.macros.size() && m < 4; ++m) {
        const doc::MacroKnob& knob = group.macros[m];
        FrameUi::GroupStage stage{};
        stage.group_id = group.id;
        stage.macro_index = m;
        stage.staged = arena.alloc<float>();
        *stage.staged = knob.value;
        stage.original = knob.value;
        stage.changed = arena.alloc<bool>();
        stage.released = arena.alloc<bool>();
        SliderOpts opts;
        opts.format = "%.2f";
        opts.out_changed = stage.changed;
        opts.out_released = stage.released;
        actions.macro_remove[m] = arena.alloc<bool>();
        char mline[80];
        std::snprintf(mline, sizeof(mline), "%s (%zu)", knob.name.c_str(),
                      knob.targets.size());
        ButtonOpts mx;
        mx.width = SizeSpec::fixed(20);
        mx.tooltip = "remove macro knob";
        StackOpts mrow;
        mrow.gap = 2.0f;
        mrow.cross_align = AlignMode::Center;
        std::vector<LayoutNode*> mcells{
            SizedBox(arena, SizeSpec::fixed(58), SizeSpec::fixed(1), nullptr),
            SizedBox(arena, SizeSpec::fixed(80), SizeSpec::fixed(18),
                     Label(arena, mline, small_dim)),
            SliderF(arena, stage.staged, 0.0f, 1.0f,
                    &state.macro_sliders[m], opts),
            IconButton(arena, Icon::Close, &state.macro_remove[m],
                       actions.macro_remove[m], mx)};
        LayoutNode* mstack = VStackDyn(arena, mrow, mcells);
        mstack->kind = NodeKind::HStack;
        rows.push_back(mstack);
        out.group_stages.push_back(stage);
    }
    // "+ macro" is an editing affordance — open state only. Folded shows
    // just the header + the macro knobs (spec §5).
    if (!group.folded && group.macros.size() < 4)
        rows.push_back(Button(arena, "+ macro", &state.add_macro_button,
                              actions.add_macro));

    // Members live INSIDE the container, indented under the header.
    if (!group.folded)
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

// Two side panels from one pass (spec §9): LEFT = project, layers, masks,
// presets. RIGHT = the selected layer's stack + modulation — always visible
// beside the viewport, so picking a layer and editing its stack never
// scrolls. Blocks that belong to the right panel shadow `rows` with
// `right_rows` so their internals stay identical.
void build_side_panels(ui::LayoutArena& arena, AppState& app, FrameUi& out,
                       float fps, ui::LayoutNode** left_out,
                       ui::LayoutNode** right_out) {
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

    // Section fold headers (chevron + serif label). A folded section still
    // builds its body, then drops the rows — unbuilt widgets never draw, so
    // their out-flags stay false and the post-frame handlers no-op.
    auto section_header = [&](std::vector<LayoutNode*>& target, int idx,
                              const char* name) {
        FrameUi::SectionToggle t{idx, arena.alloc<bool>()};
        target.push_back(SectionHeader(arena, name, app.sec_open[idx],
                                       &app.sec_buttons[idx], t.clicked));
        out.section_toggles.push_back(t);
        return target.size();   // fold marker: the body starts here
    };

    // ---- layers (spec §5: 3 max, bottom-up)
    size_t sec_begin = section_header(rows, 0, "layers");
    static const char* kBlendNames[] = {"normal", "add", "mult", "screen",
                                        "diff"};
    for (size_t li = 0; li < app.document.layers.size(); ++li) {
        const doc::Layer& layer = app.document.layers[li];
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
            layer.source == doc::LayerSourceKind::Noise) {
            layer_slider(LF::ColorAR, "color a r", 0.0f, 1.0f,
                         layer.color_a[0], "%.2f");
            layer_slider(LF::ColorAG, "color a g", 0.0f, 1.0f,
                         layer.color_a[1], "%.2f");
            layer_slider(LF::ColorAB, "color a b", 0.0f, 1.0f,
                         layer.color_a[2], "%.2f");
        }
        if (layer.source == doc::LayerSourceKind::Gradient ||
            layer.source == doc::LayerSourceKind::Noise) {
            layer_slider(LF::ColorBR, "color b r", 0.0f, 1.0f,
                         layer.color_b[0], "%.2f");
            layer_slider(LF::ColorBG, "color b g", 0.0f, 1.0f,
                         layer.color_b[1], "%.2f");
            layer_slider(LF::ColorBB, "color b b", 0.0f, 1.0f,
                         layer.color_b[2], "%.2f");
        }
        if (layer.source == doc::LayerSourceKind::Gradient)
            layer_slider(LF::Angle, "angle", -3.1416f, 3.1416f,
                         layer.gen_angle, "%.2f");
        if (layer.source == doc::LayerSourceKind::Noise)
            layer_slider(LF::Scale, "scale", 2.0f, 128.0f, layer.gen_scale,
                         "%.0f px");

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
    if (app.document.layers.size() < doc::kMaxLayers) {
        // Paired rows: five full labels never fit one 300 px row.
        static const char* kAddLayer[] = {"+ solid", "+ gradient", "+ noise",
                                          "+ pattern", "+ adjust"};
        for (int t = 0; t < 5; ++t)
            out.add_layer_clicked[t] = arena.alloc<bool>();
        ButtonOpts half;
        half.width = SizeSpec::fill();
        for (int r = 0; r < 2; ++r) {
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
        rows.push_back(Button(arena, kAddLayer[4], &app.add_layer_buttons[4],
                              out.add_layer_clicked[4], half));
    }
    if (!app.sec_open[0]) rows.resize(sec_begin);
    rows.push_back(Separator(arena));

    // ---- RIGHT PANEL: stack + inspector. Group headers render above
    // their first member; folded groups hide the member panels. The header
    // names the layer the stack belongs to — the panel edits the SELECTED
    // layer.
    {
    std::vector<LayoutNode*>& rows = right_rows;   // right panel from here
    const std::string stack_title = app.document.layers.empty()
        ? std::string("stack")
        : "stack: " + app.document.layers[app.selected_layer].name;
    sec_begin = section_header(rows, 1, stack_title.c_str());
    if (app.document.layers.empty()) {
        rows.push_back(Label(arena, "no layers - add one in the layers panel",
                             small_dim));
    }
    if (!app.document.layers.empty()) {
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
    if (!app.document.layers.empty()) {
        const doc::Layer& sel_layer = app.document.layers[app.selected_layer];
        std::vector<uint64_t> emitted_groups;
        auto group_of = [&](uint64_t id) -> const doc::Group* {
            for (const doc::Group& g : sel_layer.groups)
                if (g.id == id) return &g;
            return nullptr;
        };
        for (size_t i = 0; i < sel_layer.stack.size(); ++i) {
            const uint64_t gid = sel_layer.stack[i].group_id;
            const doc::Group* group = gid ? group_of(gid) : nullptr;
            if (!group) {
                rows.push_back(build_effect_panel(arena, app, out, i));
                continue;
            }
            if (std::find(emitted_groups.begin(), emitted_groups.end(),
                          gid) != emitted_groups.end())
                continue;   // rendered inside its container already
            emitted_groups.push_back(gid);
            std::vector<size_t> members;
            for (size_t j = 0; j < sel_layer.stack.size(); ++j)
                if (sel_layer.stack[j].group_id == gid) members.push_back(j);
            rows.push_back(
                build_group_panel(arena, app, out, *group, members));
        }
        if (sel_layer.stack.empty())
            rows.push_back(Label(arena,
                                 "empty - add an effect below",
                                 small_dim));
    }

    // Add-effect browser: spec §6.1 category folds instead of a 54-button
    // wall. Effects lay out two per row inside an open category.
    if (!app.document.layers.empty()) {
        // A fold, not an action: chevron language like every other
        // disclosure ("+" stays reserved for buttons that add on click).
        out.add_fx_toggle = arena.alloc<bool>();
        rows.push_back(SectionHeader(arena, "add effect", app.add_fx_open,
                                     &app.add_fx_button, out.add_fx_toggle,
                                     /*small=*/true));
        if (app.add_fx_open) {
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
    if (!app.sec_open[1]) rows.resize(sec_begin);
    rows.push_back(Separator(arena));
    }   // end right panel (stack)

    // ---- preset browser (spec §10): click = drop the group onto the
    // selected layer's stack. Tag button filters; "sv" on a group saves.
    sec_begin = section_header(rows, 2, "presets");
    {
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
            rows.push_back(Button(arena, p.name.c_str(),
                                  &app.preset_buttons[p.path.string()],
                                  clicked));
            out.preset_clicks.push_back({pi, clicked});
        }
        if (app.presets.empty())
            rows.push_back(Label(arena, "no presets found", small_dim));
        // Single-file preset import (spec §9); drag-and-drop works too.
        out.preset_import_clicked = arena.alloc<bool>();
        rows.push_back(Button(arena, "import preset...",
                              &app.preset_import_btn,
                              out.preset_import_clicked));
    }
    if (!app.sec_open[2]) rows.resize(sec_begin);
    rows.push_back(Separator(arena));

    // ---- masks (spec §8)
    sec_begin = section_header(rows, 3, "masks");
    static const char* kMaskTypeNames[] = {"shape", "luma", "luma key",
                                           "chroma key", "motion"};
    static const char* kExtractNames[] = {"luma", "red", "green", "blue",
                                          "alpha"};
    for (const doc::Mask& mask : app.document.masks) {
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
    out.add_mask_clicked = arena.alloc<bool>();
    rows.push_back(Button(arena, "+ mask", &app.add_mask_button,
                          out.add_mask_clicked));
    if (!app.sec_open[3]) rows.resize(sec_begin);
    rows.push_back(Separator(arena));

    // ---- RIGHT PANEL: mod matrix + snapshots + undo + export.
    {
    std::vector<LayoutNode*>& rows = right_rows;   // right panel from here
    sec_begin = section_header(rows, 4, "mod matrix");
    const auto table = mod::build_param_table(app.document);
    auto path_of = [&](const doc::ParamKey& key) -> std::string {
        for (const auto& e : table)
            if (e.key == key) return e.path;
        return "(deleted)";
    };
    static const char* kSourceNames[] = {"lfo",    "drift", "a.low",
                                         "a.mid",  "a.high", "onset",
                                         "motion", "bright", "lfo.bpm",
                                         "env",    "cut",    "beat"};
    static const char* kShapeNames[] = {"sin", "tri", "sqr", "s&h"};
    static const char* kCurveNames[] = {"lin", "exp", "s", "inv"};
    for (const doc::ModRoute& route : app.document.mod_routes) {
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
        if (is_pulse) {
            *row.rate_staged = route.source.decay;
            row.rate_original = route.source.decay;
        }

        std::vector<LayoutNode*> route_rows_ui;
        ButtonOpts route_x;
        route_x.width = SizeSpec::fixed(20);
        route_x.tooltip = "remove route";
        route_rows_ui.push_back(HStack(
            arena, {2.0f},
            {Label(arena, path_of(route.target).c_str(), small_dim),
             Spacer(arena),
             IconButton(arena, Icon::Close, &rs.remove_button, row.remove,
                        route_x)}));
        static const char* kTriggerNames[] = {"onset", "cut", "beat", "key"};
        std::vector<LayoutNode*> pick_cells{
            Dropdown(arena, kSourceNames, 12,
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
    if (app.document.mod_routes.empty())
        rows.push_back(Label(arena, "press ~ next to a param", small_dim));
    rows.push_back(Separator(arena));

    // ---- snapshots
    {
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
    }
    if (!app.sec_open[4]) rows.resize(sec_begin);
    rows.push_back(Separator(arena));

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

    std::vector<LayoutNode*> lane_rows;
    for (const doc::KeyframeLane& lane : app.document.lanes) {
        std::string path = "(deleted)";
        float min_v = 0.0f, max_v = 1.0f;
        for (const auto& e : table) {
            if (e.key == lane.target) {
                path = e.path;
                min_v = e.min_value;
                max_v = e.max_value;
                break;
            }
        }
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
        // Label + the loopable-region chip (spec §7) share the name column.
        FrameUi::LaneLoop ll{lane.target, arena.alloc<bool>()};
        out.lane_loops.push_back(ll);
        StackOpts name_col;
        name_col.gap = 2.0f;
        name_col.cross_align = AlignMode::Start;
        lane_rows.push_back(HStack(
            arena, {6.0f},
            {SizedBox(arena, SizeSpec::fixed(150), SizeSpec::fixed(42),
                      VStack(arena, name_col,
                             {Label(arena, label, lane_label),
                              Chip(arena, "loop", lane.loop,
                                   &lane_state.loop_button, ll.clicked,
                                   "wrap playback through the key span")})),
             widget}));
    }
    if (lane_rows.size() > 4) {
        // Many lanes: cap the timeline height and scroll inside it so the
        // viewport keeps its space.
        StackOpts lane_col;
        lane_col.gap = 4.0f;
        lane_col.cross_align = AlignMode::Stretch;
        rows.push_back(SizedBox(
            arena, SizeSpec::fill(), SizeSpec::fixed(184.0f),
            ScrollAreaV(arena, &app.timeline_scroll,
                        VStackDyn(arena, lane_col, lane_rows))));
    } else {
        for (LayoutNode* r : lane_rows) rows.push_back(r);
    }
    if (app.document.lanes.empty())
        rows.push_back(Label(arena,
                             "no keyframe lanes - press k next to a param",
                             small_dim));

    StackOpts column;
    column.gap = 4.0f;
    column.cross_align = AlignMode::Stretch;
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

    // First-launch demo stack (direct construction, not commands: this is
    // the document baseline, not a user mutation). Spec §5 default order
    // Time → Mosaic → Optics → Color → Texture → Codec, exercising the
    // wave-1 roster: jitter, masked pixelate, halation glow, quantizer,
    // grain, LFO-driven RGB split, keyframed vignette.
    {
        app.document.layers[app.selected_layer].stack.reserve(12);   // references below stay valid
        auto add = [&](doc::EffectType type) -> doc::EffectInstance& {
            app.document.layers[app.selected_layer].stack.push_back(doc::make_effect(app.document, type));
            return app.document.layers[app.selected_layer].stack.back();
        };
        doc::EffectInstance& jitter = add(doc::EffectType::Jitter);
        jitter.params[0] = 3.0f;                        // subtle gate weave
        doc::EffectInstance& stock = add(doc::EffectType::ColorScience);
        stock.params[0] = 1.0f;                         // kodachrome
        stock.params[3] = 0.25f;                        // faded
        doc::EffectInstance& smear = add(doc::EffectType::FlowSmear);
        smear.params[0] = 3.0f;                         // motion streaks
        doc::EffectInstance& mosh = add(doc::EffectType::Datamosh);
        mosh.params[1] = 15.0f;                         // gop 15
        mosh.params[3] = 6.0f;                          // mv random
        mosh.params[4] = 0.2f;                          // corruption
        doc::EffectInstance& pixelate = add(doc::EffectType::Pixelate);
        doc::EffectInstance& glow = add(doc::EffectType::Glow);
        glow.params[0] = 1.2f;
        glow.params[3] = 1.0f;                          // halation
        doc::EffectInstance& quantize = add(doc::EffectType::Quantize);
        quantize.params[0] = 6.0f;                      // rgb levels + bayer8
        doc::EffectInstance& grain = add(doc::EffectType::Grain);
        grain.params[0] = 0.5f;
        doc::EffectInstance& echo = add(doc::EffectType::Echo);
        echo.params[0] = 0.8f;                          // ghost trails
        doc::EffectInstance& split = add(doc::EffectType::RgbSplit);
        doc::EffectInstance& glyph = add(doc::EffectType::Glyph);
        glyph.params[0] = 6.0f;                         // ascii cells
        glyph.bypass = true;                            // opt-in finale
        doc::EffectInstance& vignette = add(doc::EffectType::Vignette);

        doc::ModRoute route;
        route.id = app.document.next_route_id++;
        route.target = {split.id, 0};                   // shift x
        route.amount = 0.2f;
        route.source.type = doc::ModSourceType::Lfo;
        route.source.shape = doc::LfoShape::Sine;
        route.source.rate_hz = 0.5f;
        app.document.mod_routes.push_back(route);

        doc::KeyframeLane lane;
        lane.target = {vignette.id, 0};                 // amount
        lane.keys.push_back({0.0, 0.0f, 20.0f, 0.0f, 0.0f, 0.0f, false});
        lane.keys.push_back({45.0, 0.9f});
        lane.keys.push_back({89.0, 0.1f});
        app.document.lanes.push_back(lane);

        // Group the film-base pair behind a "fade" macro (spec §5: a group
        // collapses a sub-stack and exposes macro knobs).
        doc::Group film = doc::make_group(app.document, "film base");
        doc::MacroKnob fade;
        fade.name = "fade";
        fade.value = 0.3f;
        fade.targets.push_back(
            {stock.id, 3, 0.0f, 0.5f, doc::ResponseCurve::Linear});
        fade.targets.push_back(
            {grain.id, 0, 0.0f, 0.3f, doc::ResponseCurve::Linear});
        film.macros.push_back(fade);
        jitter.group_id = film.id;
        stock.group_id = film.id;
        app.document.layers[0].groups.push_back(film);

        doc::Mask mask;                                 // pixelate only
        mask.id = app.document.next_mask_id++;          // inside an ellipse
        mask.name = "mask " + std::to_string(mask.id);
        mask.type = doc::MaskType::Shape;
        mask.radius_x = 0.3f;
        mask.radius_y = 0.4f;
        mask.feather = 0.12f;
        app.document.masks.push_back(mask);
        pixelate.mask_id = mask.id;

        // Second layer: coarse noise screened over the composite (dust/
        // texture-plate feel; spec §5 compositing demo).
        doc::Layer texture = doc::make_layer(app.document,
                                             doc::LayerSourceKind::Noise);
        texture.blend = doc::BlendMode::Screen;
        texture.opacity = 0.25f;
        texture.gen_scale = 3.0f;
        texture.color_a[0] = 0.9f;
        texture.color_a[1] = 0.85f;
        texture.color_a[2] = 0.8f;
        texture.color_b[0] = texture.color_b[1] = texture.color_b[2] = 0.0f;
        app.document.layers.push_back(std::move(texture));
    }

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
                    // Preset search typing (spec §9): printable ASCII only.
                    if (app.preset_search_focus && e.codepoint >= 32 &&
                        e.codepoint < 127)
                        app.preset_filter.push_back(
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
                    if (e.key == platform::Key::Escape) running = false;
                    else if (e.key == platform::Key::Space && !e.repeat)
                        toggle_play = true;
                    else if (e.key == platform::Key::Z &&
                             (e.mods & platform::kModCtrl)) {
                        if (e.mods & platform::kModShift) do_redo = true;
                        else do_undo = true;
                    } else if (e.key == platform::Key::Y &&
                               (e.mods & platform::kModCtrl)) {
                        do_redo = true;
                    } else if (e.key == platform::Key::S &&
                               (e.mods & platform::kModCtrl)) {
                        if (e.mods & platform::kModShift) do_save_as = true;
                        else do_save = true;
                    } else if (e.key == platform::Key::O &&
                               (e.mods & platform::kModCtrl)) {
                        do_open_project = true;
                    } else if (e.key == platform::Key::A && !e.repeat &&
                               (e.mods & (platform::kModCtrl |
                                          platform::kModAlt)) == 0) {
                        app.ab_wipe = !app.ab_wipe;   // spec §9 A/B wipe
                    } else if (e.key == platform::Key::B && !e.repeat &&
                               (e.mods & (platform::kModCtrl |
                                          platform::kModAlt)) == 0) {
                        app.bypass_all = !app.bypass_all;   // bypass-all
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
        ui::StackOpts main_opts;
        main_opts.gap = 12.0f;
        // Three-panel layout (spec §9): layers/masks/presets left, viewport
        // + transport center, the selected layer's stack + modulation right
        // — layer-to-stack editing never scrolls.
        ui::LayoutNode* left_panel = nullptr;
        ui::LayoutNode* right_panel = nullptr;
        build_side_panels(arena, app, frame_ui, 1.0f / smoothed_dt,
                          &left_panel, &right_panel);
        // Equal widths: both panels share the same param grid, so equal
        // width means identical slider columns left and right.
        constexpr float kPanelW = 320.0f;
        ui::LayoutNode* left_box = ui::SizedBox(
            arena, ui::SizeSpec::fixed(kPanelW), ui::SizeSpec::fill(),
            left_panel);
        ui::LayoutNode* right_box = ui::SizedBox(
            arena, ui::SizeSpec::fixed(kPanelW), ui::SizeSpec::fill(),
            right_panel);
        // Center column: viewport + the transport bar underneath (spec §9).
        // With nothing open the viewport is a quiet empty state — dark,
        // centered open action, and drag-and-drop lands anywhere.
        ui::LayoutNode* center_col = preview;
        if (app.player.is_open()) {
            ui::StackOpts rc;
            rc.gap = 8.0f;
            rc.cross_align = ui::AlignMode::Stretch;
            rc.width = ui::SizeSpec::fill();
            rc.height = ui::SizeSpec::fill();
            center_col = ui::VStack(
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
            center_col = z;
        }
        ui::LayoutNode* main_row =
            ui::HStack(arena, main_opts, {left_box, center_col, right_box});
        main_row->height = ui::SizeSpec::fill();

        ui::StackOpts root_opts;
        root_opts.gap = 12.0f;
        root_opts.padding = ui::Edges::all(12);
        ui::LayoutNode* root;
        if (app.player.is_open() && !app.live_mode) {
            ui::LayoutNode* timeline = build_timeline(arena, app, frame_ui);
            root = ui::VStack(arena, root_opts, {main_row, timeline});
        } else {
            root = ui::VStack(arena, root_opts, {main_row});
        }

        ui::LayoutFrame layout_frame{canvas, input, ctx, font,
                                     ui::active_theme(), dt,
                                     header_font ? &*header_font : nullptr};
        ui::run_frame(root, viewport, layout_frame);
        // Dropdown overlay: interacts NOW — before the edit handlers below
        // — so a selection made this frame is applied this frame. Drawn
        // here it also overlays every widget (recorded after them).
        ui::RunPopup(canvas, font, ui::active_theme(), ctx, input);

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
            if (*stage.changed && *stage.staged != stage.original) {
                app.undo.execute(app.document,
                                 doc::set_param_command(ui_layer,
                                                        stage.fx_index,
                                                        stage.param_index,
                                                        *stage.staged),
                                 /*coalesce=*/true);
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
            if (*row.bypass_changed) {
                app.undo.execute(app.document,
                                 doc::set_bypass_command(ui_layer,
                                                         row.fx_index,
                                                         *row.bypass_staged));
                structure_done = true;
            } else if (row.solo_changed && *row.solo_changed) {
                app.undo.execute(app.document,
                                 doc::set_solo_command(ui_layer, row.fx_index,
                                                       *row.solo_staged));
                structure_done = true;
            } else if (row.duplicate && *row.duplicate) {
                // Duplicate (spec §5): identical clone right below, with a
                // fresh id (and seed offset so "same settings" doesn't mean
                // "identical noise").
                doc::EffectInstance copy =
                    app.document.layers[ui_layer].stack[row.fx_index];
                copy.id = app.document.next_effect_id++;
                copy.seed = copy.id;
                app.undo.execute(app.document,
                                 doc::add_effect_command(ui_layer,
                                                         std::move(copy),
                                                         row.fx_index + 1));
                structure_done = true;
            } else if (*row.remove) {
                app.undo.execute(
                    app.document,
                    doc::remove_effect_command(ui_layer, row.fx_index));
                structure_done = true;
            } else if (*row.up && row.fx_index > 0) {
                app.undo.execute(app.document,
                                 doc::move_effect_command(ui_layer,
                                                          row.fx_index,
                                                          row.fx_index - 1));
                structure_done = true;
            } else if (*row.down &&
                       row.fx_index + 1 <
                           app.document.layers[ui_layer].stack.size()) {
                app.undo.execute(app.document,
                                 doc::move_effect_command(ui_layer,
                                                          row.fx_index,
                                                          row.fx_index + 1));
                structure_done = true;
            } else if (*row.group_toggle) {
                // "g": leave the group (dissolving it if now empty), join
                // the group above, or start a new group with the one above.
                auto& stack = app.document.layers[ui_layer].stack;
                const doc::EffectInstance& fx = stack[row.fx_index];
                if (fx.group_id != 0) {
                    const uint64_t gid = fx.group_id;
                    app.undo.begin_group("Ungroup Effect");
                    app.undo.execute(app.document,
                                     doc::set_effect_group_command(
                                         ui_layer, row.fx_index, 0));
                    bool any = false;
                    for (const doc::EffectInstance& e : stack)
                        any = any || e.group_id == gid;
                    if (!any)
                        app.undo.execute(app.document,
                                         doc::ungroup_command(ui_layer, gid));
                    app.undo.end_group();
                } else if (row.fx_index > 0) {
                    const uint64_t above = stack[row.fx_index - 1].group_id;
                    if (above != 0) {
                        app.undo.execute(app.document,
                                         doc::set_effect_group_command(
                                             ui_layer, row.fx_index, above));
                    } else {
                        doc::Group g = doc::make_group(app.document, "group");
                        app.undo.execute(app.document,
                                         doc::group_effects_command(
                                             ui_layer, std::move(g),
                                             row.fx_index - 1, row.fx_index));
                    }
                }
                structure_done = true;
            }
        }
        for (size_t t = 0;
             !structure_done && t < static_cast<size_t>(doc::EffectType::Count);
             ++t) {
            if (frame_ui.add_clicked[t] && *frame_ui.add_clicked[t]) {
                auto fx = doc::make_effect(app.document,
                                           static_cast<doc::EffectType>(t));
                // Default insert position (spec §5): keep the stack in
                // Time → Mosaic → Optics → Color → Texture → Overlay →
                // Codec order — overlay BEFORE codec so timestamps/HUDs
                // get JPEG-crushed. Manual reordering stays free.
                auto cat_rank = [](doc::EffectType type) {
                    switch (doc::effect_info(type).category) {
                        case doc::FxCategory::Time: return 0;
                        case doc::FxCategory::Warp: return 1;
                        case doc::FxCategory::Mosaic: return 2;
                        case doc::FxCategory::Optics: return 3;
                        case doc::FxCategory::Color: return 4;
                        case doc::FxCategory::Texture: return 5;
                        case doc::FxCategory::Overlay: return 6;
                        default: return 7;   // signal & codec last
                    }
                };
                const auto& stack = app.document.layers[ui_layer].stack;
                const int rank = cat_rank(fx.type);
                size_t insert_at = stack.size();
                for (size_t i = 0; i < stack.size(); ++i)
                    if (cat_rank(stack[i].type) > rank) {
                        insert_at = i;
                        break;
                    }
                // Never split a group's contiguous run.
                while (insert_at > 0 && insert_at < stack.size() &&
                       stack[insert_at - 1].group_id != 0 &&
                       stack[insert_at - 1].group_id ==
                           stack[insert_at].group_id)
                    ++insert_at;
                app.undo.execute(app.document,
                                 doc::add_effect_command(
                                     ui_layer, std::move(fx), insert_at));
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

        // ---- group edits (spec §5: groups + macro knobs)
        for (const FrameUi::GroupStage& gs : frame_ui.group_stages) {
            if (*gs.changed && *gs.staged != gs.original) {
                const doc::Group* group = nullptr;
                for (const doc::Group& g : app.document.layers[ui_layer].groups)
                    if (g.id == gs.group_id) group = &g;
                if (group && gs.macro_index < group->macros.size()) {
                    doc::Group edited = *group;
                    edited.macros[gs.macro_index].value = *gs.staged;
                    app.undo.execute(
                        app.document,
                        doc::set_group_props_command(ui_layer, edited),
                        /*coalesce=*/true);
                }
            }
            if (*gs.released && !did_break) {
                app.undo.break_coalescing();
                did_break = true;
            }
        }
        for (const FrameUi::GroupActions& ga : frame_ui.group_actions) {
            const doc::Group* group = nullptr;
            for (const doc::Group& g : app.document.layers[ui_layer].groups)
                if (g.id == ga.group_id) group = &g;
            if (!group) continue;
            if (*ga.fold) {
                doc::Group edited = *group;
                edited.folded = !edited.folded;
                app.undo.execute(app.document,
                                 doc::set_group_props_command(ui_layer,
                                                              edited));
            } else if (*ga.bypass_changed) {
                doc::Group edited = *group;
                edited.bypass = *ga.bypass_staged;
                app.undo.execute(app.document,
                                 doc::set_group_props_command(ui_layer,
                                                              edited));
            } else if (*ga.add_macro) {
                doc::MacroKnob knob;
                knob.name = "macro " + std::to_string(group->macros.size() + 1);
                app.undo.execute(app.document,
                                 doc::add_macro_command(ui_layer, ga.group_id,
                                                        std::move(knob)));
            } else if (*ga.save) {
                auto out_path = platform::show_save_dialog(
                    window.get(), {{"looks preset", "*.json"}},
                    (group->name.empty() ? std::string("preset")
                                         : group->name) + ".json");
                if (out_path) {
                    if (out_path->extension() != ".json")
                        out_path->replace_extension(".json");
                    doc::Preset preset = doc::make_preset_from_group(
                        app.document, ui_layer, ga.group_id);
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
                                 doc::ungroup_command(ui_layer, ga.group_id));
                structure_done = true;
            } else {
                for (size_t m = 0; m < 4; ++m) {
                    if (ga.macro_remove[m] && *ga.macro_remove[m]) {
                        app.undo.execute(
                            app.document,
                            doc::remove_macro_command(ui_layer, ga.group_id,
                                                      m));
                        break;
                    }
                }
            }
        }
        for (const FrameUi::AddMacroTarget& amt : frame_ui.add_macro_targets) {
            if (!*amt.clicked) continue;
            uint64_t gid = 0;
            for (const doc::EffectInstance& fx :
                 app.document.layers[ui_layer].stack)
                if (fx.id == amt.fx_id) gid = fx.group_id;
            const doc::Group* group = nullptr;
            for (const doc::Group& g : app.document.layers[ui_layer].groups)
                if (g.id == gid) group = &g;
            if (!group || group->macros.empty()) continue;
            doc::Group edited = *group;
            doc::MacroTarget target;
            target.effect_id = amt.fx_id;
            target.param_index = amt.param_index;
            edited.macros.back().targets.push_back(target);
            app.undo.execute(app.document,
                             doc::set_group_props_command(ui_layer, edited));
            break;
        }
        for (const auto& pc : frame_ui.preset_clicks) {
            if (!*pc.second || structure_done) continue;
            if (pc.first >= app.presets.size()) continue;
            if (app.document.layers.empty()) break;   // nowhere to drop
            doc::Group group;
            std::vector<doc::EffectInstance> effects;
            doc::instantiate_preset(app.document, app.presets[pc.first],
                                    &group, &effects);
            app.undo.execute(app.document,
                             doc::insert_group_command(ui_layer,
                                                       std::move(group),
                                                       std::move(effects)));
            structure_done = true;
            break;
        }
        if (frame_ui.tag_selected && *frame_ui.tag_selected >= 0)
            app.preset_tag_index = *frame_ui.tag_selected - 1;   // 0 = all
        if (frame_ui.preset_search_clicked && *frame_ui.preset_search_clicked) {
            app.preset_search_focus = !app.preset_search_focus;
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
            if ((*row.rate_released || *row.amount_released) && !did_break) {
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
            app.undo.execute(app.document,
                             doc::add_mask_command(std::move(mask)));
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
            static const doc::LayerSourceKind kAddKinds[5] = {
                doc::LayerSourceKind::Solid, doc::LayerSourceKind::Gradient,
                doc::LayerSourceKind::Noise, doc::LayerSourceKind::TestPattern,
                doc::LayerSourceKind::Adjustment};
            for (int t = 0; t < 5; ++t) {
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

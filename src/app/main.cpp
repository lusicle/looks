// looks — the app shell.
//
// The timeline is the clock (docs/look.md phase 4): the transport runs the
// scoped look's local time, the decode pool decodes one frame per clip
// PLACEMENT in the instance tree, and the engine composites them into the
// project canvas → letterboxed viewport blit, with the UI on top.
// Sidebar: transport + effect stack + inspector; every document mutation is
// an undoable command (param drags coalesce into one step).

#include <windows.h>

#include <shellapi.h>

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
#include "doc/instances.h"
#include "media/audio_mix.h"
#include "media/bundle.h"
#include "media/decode_pool.h"
#include "media/export.h"
#include "media/import.h"
#include "media/pcm.h"
#include "media/player.h"
#include <map>

#include "doc/group_commands.h"
#include "doc/layer_commands.h"
#include "doc/look_commands.h"
#include "doc/mod_commands.h"
#include "doc/placement_commands.h"
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

// ---------------------------------------------------------------- import

struct ImportJob {
    std::thread thread;
    media::ImportProgress progress;
    media::ImportResult result;
    std::atomic<bool> done{false};
    std::filesystem::path source;
    // Generic import: the asset joins the browser on completion and
    // NOTHING is placed or played ("open clip" stays the fast lane).
    // bind_layer additionally points a clip NODE at the new asset.
    bool import_only = false;
    uint64_t bind_look = 0;
    uint64_t bind_layer = 0;

    ~ImportJob() {
        progress.cancel = true;
        if (thread.joinable()) thread.join();
    }
};

// Import bundles live on the scratch disk, never next to the
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

// A bundle only counts when it is no older than its source — footage
// overwritten at the same path must re-import instead of silently serving
// stale frames. Unreadable timestamps serve what exists.
bool bundle_is_fresh(const std::filesystem::path& bundle,
                     const std::filesystem::path& source) {
    std::error_code e1, e2;
    if (!std::filesystem::exists(bundle, e1)) return false;
    const auto bundle_t = std::filesystem::last_write_time(bundle, e1);
    const auto source_t = std::filesystem::last_write_time(source, e2);
    return e1 || e2 || bundle_t >= source_t;
}

// Where an asset's media lives: a hand-built bundle beside the source, else
// the scratch cache. `ready` is false when nothing usable exists yet (the
// source still needs importing).
struct BundlePaths {
    std::filesystem::path mez, pcm;
    bool ready = false;
};

BundlePaths resolve_bundle(const std::filesystem::path& source) {
    BundlePaths out;
    std::filesystem::path mez = source;
    if (mez.extension() != ".mez") {
        std::filesystem::path beside = source;
        beside.replace_extension(".mez");
        if (bundle_is_fresh(beside, source))
            mez = beside;
        else
            mez = bundle_dir_for(source) /
                  (source.stem().wstring() + L".mez");
    }
    const bool have = mez == source ? std::filesystem::exists(mez)
                                    : bundle_is_fresh(mez, source);
    if (!have) return out;
    out.mez = mez;
    out.pcm = mez;
    out.pcm.replace_extension(".pcm");
    std::error_code ec;
    if (!std::filesystem::exists(out.pcm, ec)) out.pcm.clear();
    out.ready = true;
    return out;
}

// Still-image clips (import scope: PNG/TGA) get different clip UI:
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
        if (lossless) options.quality = 0;   // lossless mode
        raw->result =
            media::import_media(source, dest, options, &raw->progress);
        raw->done = true;
    });
    return job;
}

struct AppState;
void open_source(AppState& app, const std::filesystem::path& picked);

// ---- look scope + the project's clip (docs/look.md)

// The clip a single-asset project plays: assets[0] until the decode pool
// lands (phase 4). Empty when nothing is bound.
inline std::string primary_clip_path(const doc::Document& doc) {
    const doc::Asset* a = doc.primary_asset();
    return a ? a->path : std::string();
}

// Binds `path` as the project's clip and points every unbound clip NODE
// at it. Environment, not an undoable edit — the same policy the single
// clip_path field carried.
inline void bind_primary_clip(doc::Document& doc, const std::string& path) {
    if (doc.assets.empty()) {
        doc::Asset a;
        a.id = doc.next_effect_id++;
        doc.assets.push_back(std::move(a));
    }
    doc::Asset& asset = doc.assets.front();
    asset.path = path;
    asset.name = std::filesystem::path(path).filename().string();
    for (doc::Look& look : doc.looks)
        for (doc::Layer& l : look.layers)
            if (doc::layer_is_clip(l) && !l.asset) l.asset = asset.id;
}

// Still-image length lives on the asset (it describes the media, not the
// project); 0 = as imported.
inline uint32_t primary_still_duration(const doc::Document& doc) {
    const doc::Asset* a = doc.primary_asset();
    return a ? a->still_duration_frames : 0;
}

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

// The decode pool hands back shared decoded frames; the engine wants plane
// pointers. One loop, both consumers (preview worker and export worker).
inline std::vector<gfx::Engine::LayerSourceFrame> to_layer_sources(
    const std::vector<media::SourceFrame>& frames) {
    std::vector<gfx::Engine::LayerSourceFrame> out;
    out.reserve(frames.size());
    for (const media::SourceFrame& sf : frames) {
        if (!sf.frame) continue;
        const codec::FrameView view = sf.frame->view();
        gfx::Engine::LayerSourceFrame lf;
        lf.key = sf.key;
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

// An empty look still needs a timeline: it is the surface you put the
// first block ONTO, so the transport runs a default span until placements
// define a real one.
inline constexpr double kEmptyTimelineSeconds = 10.0;

// Slack past the last block, so a block's end can be dragged OUT and the
// next one has somewhere to land. Two seconds: enough to grab, small
// enough that the ruler does not read as padded. Extending into it grows
// the content, which puts a fresh buffer beyond - so a long drag is a few
// short ones, not a wall.
inline uint32_t timeline_buffer_frames(double fps) {
    return static_cast<uint32_t>(std::max(60.0, (fps > 0.0 ? fps : 30.0) * 2.0));
}

// The project's frame rate: its own setting, else the first asset that
// knows one. One clock for every look, so nested local times stay
// commensurable.
inline double project_fps(const doc::Document& doc,
                          const std::vector<media::AssetBundle>& bundles) {
    if (doc.fps > 0.0) return doc.fps;
    for (const media::AssetBundle& b : bundles)
        if (b.fps > 0.0) return b.fps;
    return 30.0;
}

// The same rate as an exact ratio for the mux. A bundle running at the
// project rate lends its own timescale (30000/1001 cannot be spelled as a
// double); otherwise a millirate is close enough to be honest.
inline void frame_rate_ratio(double fps,
                             const std::vector<media::AssetBundle>& bundles,
                             uint32_t* num, uint32_t* den) {
    for (const media::AssetBundle& b : bundles)
        if (b.frame_duration && b.timescale && std::abs(b.fps - fps) < 1e-6) {
            *num = b.timescale;
            *den = b.frame_duration;
            return;
        }
    *num = static_cast<uint32_t>(fps * 1000.0 + 0.5);
    *den = 1000;
}

// Loaded PCM sidecars, shared by every placement of an asset and by every
// consumer of the mix (monitor + export).
using PcmCache =
    std::unordered_map<uint64_t, std::shared_ptr<const media::PcmBuffer>>;

// The audio half of the instance tree: the same flattened placements the
// decode pool decodes, carrying PCM instead of pixels.
inline media::MixState build_mix(const doc::Document& doc, uint64_t look_id,
                                 const PcmCache& pcm, double fps,
                                 uint32_t rate, uint32_t channels) {
    media::MixState mix;
    mix.fps = fps > 0.0 ? fps : 30.0;
    mix.rate = rate;
    mix.channels = channels;
    // An unbounded placement (a source whose length nothing knows) would
    // run to the end of time; the scoped entity's own length is as far
    // as anything can play.
    uint32_t span = 0;
    if (const doc::Look* l = doc.find_look(look_id))
        span = doc::look_duration(doc, *l);
    else if (const doc::Sequence* s = doc.find_sequence(look_id))
        span = doc::sequence_duration(doc, *s);
    const double horizon =
        static_cast<double>(std::max<uint32_t>(span, 1));
    // Sound rides AUDIO tracks only: a video block with no linked audio
    // partner genuinely has no audio; a scoped look sounds like its
    // clips in lockstep.
    for (const doc::ClipInstance& c : doc::flatten_audio_sources(doc, look_id)) {
        if (c.gain <= 0.0f) continue;
        const auto it = pcm.find(c.asset);
        if (it == pcm.end() || !it->second) continue;
        media::MixSource src;
        src.pcm = it->second;
        src.t_in = c.t_in;
        src.t_out = std::min(c.t_out, horizon);
        src.source_in = c.source_in;
        src.speed = c.speed;
        src.gain = c.gain;
        if (src.t_out <= src.t_in) continue;
        mix.sources.push_back(std::move(src));
    }
    return mix;
}

// ---------------------------------------------------- preview render thread
//
// The preview graph evaluates OFF the UI thread,
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
        // Where every asset's media lives (media/bundle.h): the decode
        // pool resolves placements through this, not through the document.
        std::vector<media::AssetBundle> bundles;
        uint64_t bundle_stamp = ~0ull;
        // The look being previewed (docs/look.md): the editing scope.
        uint64_t look_id = 0;
        // Selection-follows preview: node whose output the big
        // preview publishes; 0 = the composite. preview_layer taps a
        // LAYER's whole contribution instead (node outranks layer).
        uint64_t preview_node = 0;
        uint64_t preview_layer = 0;
        uint32_t preview_div = 1;
        bool live_mode = false;
        double app_seconds = 0.0;
        double env_key_time = -1.0;
        bool want_source = false;         // A/B wipe or bypass-all
        bool proxy_active = false;        // cache-context ingredient
        // Custom glyph set hand-off: the drop happens on the UI
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
    media::DecodePool pool;
    mod::TimeRemap remap;

    doc::Document doc;
    uint64_t doc_revision = ~0ull;
    mod::AnalysisCurves analysis;
    bool has_analysis = false;
    uint64_t analysis_stamp = ~0ull;
    uint64_t cache_doc_hash = 0;
    bool cache_doc_history = false;
    uint64_t cache_hash_revision = ~0ull;
    std::vector<media::AssetBundle> bundles;
    uint64_t bundle_stamp = ~0ull;
    uint64_t last_serial = ~0ull;
    uint32_t last_rendered_frame = 0xFFFFFFFFu;
    bool last_had_frame = false;
    bool pending_render = false;
    uint32_t slot = 0;

    for (;;) {
        // Small-field snapshot; doc/analysis copied only on change.
        uint64_t preview_node, preview_layer, look_id;
        uint32_t preview_div;
        bool live_mode, want_source, proxy_active;
        double app_seconds, env_key_time;
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
            if (job_.bundle_stamp != bundle_stamp) {
                bundles = job_.bundles;
                bundle_stamp = job_.bundle_stamp;
                doc_changed = true;   // different media, same document
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
            preview_node = job_.preview_node;
            preview_layer = job_.preview_layer;
            look_id = job_.look_id;
            preview_div = job_.preview_div;
            live_mode = job_.live_mode;
            app_seconds = job_.app_seconds;
            env_key_time = job_.env_key_time;
            want_source = job_.want_source;
            proxy_active = job_.proxy_active;
        }

        // The transport runs whether or not any media is loaded — a look
        // of generators is a perfectly good thing to render. Without an
        // audio device the playhead only moves if this loop ticks the
        // fallback clock, and it must tick even on cycles it skips.
        player.tick();
        const uint32_t mod_frame = player.current_frame_index();
        // Idle: nothing moved, nothing changed, nothing owed — skip. A
        // pending dither walk counts as owed: one settle pass joins it so
        // paused frames show the finished result without interaction.
        if (!doc_changed && !pending_render && !live_mode &&
            last_had_frame && mod_frame == last_rendered_frame &&
            !engine->ed_walk_pending())
            continue;
        // A decided render stays owed until it actually publishes (a
        // busy publish ring must retry, not stall).
        pending_render = true;

        const double mod_fps = player.fps() > 0.0 ? player.fps() : 30.0;
        uint32_t canvas_w = 0, canvas_h = 0;
        doc::canvas_size(doc, &canvas_w, &canvas_h);

        // Time remap retimes the TIMELINE (a root-only feature): the
        // graph, the decode pool and every instance clock run on the
        // remapped position, while modulation stays fixed-timestep on the
        // raw playhead so lanes and LFOs do not ramp with it.
        uint32_t play_frame = mod_frame;
        if (look_id == doc.root_sequence) {
            const uint32_t span =
                doc::sequence_duration(doc, doc.sequence(look_id));
            play_frame = remap.source_frame(
                doc, mod_frame, mod_fps, has_analysis ? &analysis : nullptr,
                span, live_mode ? app_seconds : -1.0);
        }

        // Clip pixels: one decoded frame per PLACEMENT in the instance
        // tree, keyed exactly as the compiler keys its Source nodes.
        pool.set_document(doc, look_id, bundles, doc_revision);
        const std::vector<media::SourceFrame>& decoded =
            pool.collect(play_frame);
        const auto lsrc = to_layer_sources(decoded);

        // Video-sampling sources (docs/flow_canvas.md) read the exact
        // frame this pass renders: the reference source, which is what
        // the A/B wipe and the motion field read too.
        mod::SourceFrameView sfv;
        const gfx::Engine::LayerSourceFrame* ref =
            lsrc.empty() ? nullptr : &lsrc.front();
        if (ref) {
            sfv.y = ref->planes.y;
            sfv.y_stride = ref->planes.y_stride;
            sfv.u = ref->planes.u;
            sfv.u_stride = ref->planes.u_stride;
            sfv.v = ref->planes.v;
            sfv.v_stride = ref->planes.v_stride;
            sfv.width = static_cast<int>(ref->planes.width);
            sfv.height = static_cast<int>(ref->planes.height);
        }
        const doc::Document resolved = mod::resolve(
            doc, mod_frame, mod_fps, has_analysis ? &analysis : nullptr,
            live_mode ? app_seconds : -1.0, live_mode ? env_key_time : -1.0,
            &sfv);
        engine->set_preview_divisor(preview_div);
        engine->cache().set_budget(static_cast<size_t>(doc.cache_mb) << 20);

        // Frame render cache context — the doc-hash leg is
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
                ctx = hash_combine(ctx, preview_node);
                ctx = hash_combine(ctx, preview_layer);
                // WHICH look renders is part of the context; the doc hash
                // already covers every look's content, and instance paths
                // and local frames derive from it plus the frame index.
                ctx = hash_combine(ctx, look_id);
                ctx = hash_combine(ctx, has_analysis ? 1u : 0u);
                ctx = hash_combine(ctx, proxy_active ? 2u : 3u);
                // Which media is bound, and where it lives.
                ctx = hash_combine(ctx, bundle_stamp);
                cache_ctx = ctx | 1u;
            }
        }

        // Pick a publish slot the UI is provably done with.
        const uint32_t pw = std::max((canvas_w / preview_div) & ~1u, 2u);
        const uint32_t ph = std::max((canvas_h / preview_div) & ~1u, 2u);
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
            cmd_, slot, resolved, look_id, play_frame, mod_fps, canvas_w,
            canvas_h, cache_ctx, mod_frame,
            want_source ? &source_image : nullptr,
            lsrc.empty() ? nullptr : lsrc.data(), lsrc.size(),
            preview_node, preview_layer);
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

// Audio Scope source (family): mono copy of the PCM sidecar,
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
// by the device mutex. export_look_id names what renders (the project's
// root look, or one look on its own — docs/look.md).
std::unique_ptr<ExportJob> start_export(
    gfx::Device& device, const std::filesystem::path& shader_dir,
    const std::vector<media::AssetBundle>& bundles, const PcmCache& pcm,
    const std::filesystem::path& scope_pcm, const doc::Document& doc,
    uint64_t export_look_id, const mod::AnalysisCurves* analysis,
    const std::filesystem::path& out_path) {
    auto job = std::make_unique<ExportJob>();
    job->out_path = out_path;
    ExportJob* raw = job.get();
    doc::Document doc_copy = doc;
    mod::AnalysisCurves curves_copy;
    const bool has_analysis = analysis != nullptr;
    if (analysis) curves_copy = *analysis;
    std::vector<media::AssetBundle> bundle_copy = bundles;
    PcmCache pcm_copy = pcm;   // shared buffers, not a second load
    job->thread = std::thread([&device, shader_dir, scope_pcm,
                               bundle_copy = std::move(bundle_copy),
                               pcm_copy = std::move(pcm_copy),
                               doc_copy = std::move(doc_copy),
                               curves_copy = std::move(curves_copy),
                               has_analysis, raw, out_path,
                               export_look_id] {
        auto engine = gfx::Engine::create(device, shader_dir);
        auto readback = gfx::Nv12Readback::create(device, shader_dir);
        if (!engine || !readback) {
            raw->result.error = "export renderer init failed";
            raw->done = true;
            return;
        }
        // Output scale: the export engine rides the same proxy
        // divisor preview uses — kernels sample by uv, so working targets
        // and the NV12 readback shrink cleanly together.
        engine->set_preview_divisor(doc_copy.export_scale);
        uint32_t canvas_w = 0, canvas_h = 0;
        doc::canvas_size(doc_copy, &canvas_w, &canvas_h);
        const uint32_t out_w =
            std::max((canvas_w / doc_copy.export_scale) & ~1u, 2u);
        const uint32_t out_h =
            std::max((canvas_h / doc_copy.export_scale) & ~1u, 2u);
        // Audio Scope parity with preview: same PCM reduction.
        {
            uint32_t scope_rate = 0;
            auto scope_mono = load_scope_audio(scope_pcm, &scope_rate);
            engine->set_scope_audio(std::move(scope_mono), scope_rate);
        }
        // Its own pool: MezReaders are single-thread objects, and the
        // preview worker is still running its own.
        media::DecodePool pool;
        pool.set_document(doc_copy, export_look_id, bundle_copy, 1);
        mod::TimeRemap remap;
        const double fps = project_fps(doc_copy, bundle_copy);
        // Look trim: export renders exactly the trim region; modulation
        // still resolves on absolute timeline frames so preview and export
        // stay bit-identical.
        const doc::Sequence& export_seq = doc_copy.sequence(export_look_id);
        const uint32_t total = doc::sequence_duration(doc_copy, export_seq);
        if (total == 0) {
            raw->result.error = "nothing to export (empty sequence)";
            raw->done = true;
            return;
        }
        const uint32_t t_in = std::min(export_seq.trim_in, total - 1);
        const uint32_t t_out = export_seq.trim_out
                                   ? std::min(export_seq.trim_out, total)
                                   : total;
        const uint32_t span = t_out > t_in ? t_out - t_in : total;
        auto producer = [&](uint32_t f, std::vector<uint8_t>& nv12) {
            const uint32_t abs_f = t_in + f;
            // Same time-remap math as preview: export walks frames
            // sequentially, so the prefix sum is incremental.
            uint32_t play_frame = abs_f;
            if (export_look_id == doc_copy.root_sequence)
                play_frame = remap.source_frame(
                    doc_copy, abs_f, fps,
                    has_analysis ? &curves_copy : nullptr, total);
            const std::vector<media::SourceFrame>& decoded =
                pool.collect(play_frame);
            const auto lsrc = to_layer_sources(decoded);
            // Same resolve as preview (one code path, fixed timestep on
            // frame index) — including the video-sample view of the
            // identical reference source.
            mod::SourceFrameView sfv;
            if (!lsrc.empty()) {
                const gfx::SourcePlanes& p = lsrc.front().planes;
                sfv.y = p.y;
                sfv.y_stride = p.y_stride;
                sfv.u = p.u;
                sfv.u_stride = p.u_stride;
                sfv.v = p.v;
                sfv.v_stride = p.v_stride;
                sfv.width = static_cast<int>(p.width);
                sfv.height = static_cast<int>(p.height);
            }
            const doc::Document resolved = mod::resolve(
                doc_copy, abs_f, fps, has_analysis ? &curves_copy : nullptr,
                -1.0, -1.0, &sfv);
            return readback->render(*engine, resolved, export_look_id,
                                    play_frame, fps, canvas_w, canvas_h, nv12,
                                    0, abs_f,
                                    lsrc.empty() ? nullptr : lsrc.data(),
                                    lsrc.size());
        };
        media::ExportOptions options;
        options.video_bitrate_bps = static_cast<uint32_t>(
            std::clamp(doc_copy.export_bitrate_mbps, 1.0f, 60.0f) *
            1'000'000.0f);
        // Trimmed exports keep audio in sync by skipping the same lead-in;
        // the user nudge (positive = audio later) subtracts.
        options.audio_offset_seconds =
            (fps > 0.0 ? t_in / fps : 0.0) -
            static_cast<double>(doc_copy.audio_offset_ms) * 0.001;
        // The soundtrack is the project mix, pulled exactly as the monitor
        // pulls it: what was heard is what is written.
        media::ExportAudio audio;
        if (doc_copy.export_audio) {
            auto mix = std::make_shared<media::MixState>(
                build_mix(doc_copy, export_look_id, pcm_copy, fps, 48000, 2));
            if (!mix->sources.empty()) {
                audio.channels = mix->channels;
                audio.rate = mix->rate;
                auto scratch = std::make_shared<std::vector<float>>();
                audio.fill = [mix, scratch](int64_t first, int16_t* out,
                                            uint32_t frames) {
                    media::render_mix(*mix, first, out, frames, *scratch);
                };
            }
        }
        uint32_t fps_num = 0, fps_den = 0;
        frame_rate_ratio(fps, bundle_copy, &fps_num, &fps_den);
        raw->result =
            media::export_movie(out_w, out_h, fps_num, fps_den, span,
                                producer, audio, out_path, options,
                                &raw->progress);
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
    ui::ButtonState value_edit_button;   // rail type-in field
    // Selector-param dropdowns + the Text card's string field.
    ui::DropdownState param_dd[16];
    ui::ButtonState text_button;
};

struct GroupUiState {
    ui::ButtonState fold_button, ungroup_button, save_button;
    ui::ButtonState bypass_check;
    // Face rows: exposed member params as direct aliases.
    ui::SliderState face_sliders[8];
    ui::ButtonState face_remove[8];
    ui::DropdownState face_dd[8];   // selector aliases
};

// Per-WIRE inspector state: curve + remove. Source params live on the
// value node's card; the wire replaces the base, so it has no depth.
struct RouteUiState {
    ui::DropdownState curve_dd;
    ui::ButtonState remove_button;
};

struct LayerUiState {
    ui::ButtonState select_button, visible_check, remove_button;
    ui::ButtonState up_button, down_button, unique_button;
    ui::DropdownState blend_dd, osc_dd, clip_dd;
    ui::ButtonState clip_browse;
    ui::SwatchState swatch_a, swatch_b;
    ui::SliderState sliders[9];
    ui::ButtonState value_edit_button;   // rail type-in field
    // Layer params are mod targets — route/key micros per row.
    ui::ButtonState route_buttons[9], key_buttons[9];
    // Tree audio mix: this source's level and mute.
    ui::SliderState audio_slider;
    ui::ButtonState audio_mute_btn;
    // Transform + trim, folded by default.
    bool xf_open = false;
    ui::ButtonState xf_header, flip_h_btn, flip_v_btn;
    ui::SliderState xf_sliders[8];
    ui::ButtonState xf_route_buttons[8], xf_key_buttons[8];
};

struct LaneUiState {
    int selected = -1;
    bool dragging_key = false;
    bool dragging_in = false;
    bool dragging_out = false;
    // A key added this frame: reselected next frame by exact frame match
    // (nearest-to-mouse picked the WRONG key when keys clustered).
    double pending_add_frame = -1.0;
    // Multi-select: identity by key FRAME so the set survives the
    // sort the lane command applies. `selected` stays the primary key
    // (handles, readout). Box-select drags a marquee on empty strip.
    std::vector<double> sel_frames;
    bool box_select = false;
    Vec2 box_anchor{};
    // Group drag transforms a snapshot from the press — re-deriving from
    // the live keys every motion would accumulate rounding.
    bool group_drag = false;
    double drag_anchor_frame = 0.0;
    float drag_anchor_value = 0.0f;
    std::vector<doc::Keyframe> drag_orig;
    std::vector<double> drag_sel;   // selected frames at press time
    ui::ButtonState loop_button;   // loopable region chip
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
    None, Effect, Group, LayerSource, ModSource, Output,
    AddEffect,   // add-effect browser targeting a layer (+ optional slot)
    AddLayer,    // add-layer source picker
};
struct Selection {
    SelKind kind = SelKind::None;
    uint64_t id = 0;   // effect / group / layer / route id by kind
};

// In-app modal confirm — replaces the native MessageBox guards (silent,
// theme-matched, no system chrome). One dialog at a time; while open it
// owns the keyboard and the pointer, and everything under the scrim gets
// dead input. The guarded flow is stored as a continuation and runs on
// resolution, so the callers are asynchronous across frames instead of
// blocking inside the event loop.
struct ConfirmDialog {
    enum class Kind : uint8_t { None, SaveDiscard, YesNo };
    // The continuation an affirmed dialog runs.
    enum class Action : uint8_t {
        None,
        CloseApp,                  // exit guard
        OpenProjectDialog,         // file picker, then open
        OpenProjectPath,           // open `path`
        RestoreProjectAutosave,    // yes: load `path` (autosave) as `path2`
        RestoreUntitledAutosave,   // yes: load `path`; retires either way
    };
    Kind kind = Kind::None;
    Action action = Action::None;
    std::string title;
    std::string text;
    std::string primary;           // affirmative label ("save" / "restore")
    std::string secondary;         // negative label ("discard")
    std::filesystem::path path, path2;
    ui::ButtonState buttons[3];    // primary / secondary / cancel
    int hovered = -1;              // interaction pass -> draw pass
    bool open() const { return kind != Kind::None; }
};

struct AppState {
    doc::Document document;
    doc::UndoStack undo;
    // The ENTITY being edited: a sequence (the timeline surfaces) or a
    // look (the graph surfaces). One state - every editing surface,
    // every command scope, the player span and the preview read this.
    // 0 or stale resolves to the root sequence.
    uint64_t scope_look = 0;

    bool scope_is_look() const {
        return document.find_look(scope_look) != nullptr;
    }
    // The scoped look. Graph/effect surfaces build only at look scope;
    // the fallback keeps a stale id harmless mid-frame.
    doc::Look& look() {
        if (doc::Look* l = document.find_look(scope_look)) return *l;
        return document.looks.front();
    }
    const doc::Look& look() const {
        const doc::Look* l = document.find_look(scope_look);
        return l ? *l : document.looks.front();
    }
    // The scoped sequence - the timeline the block surfaces bind to;
    // the root when a look is scoped (or the id went stale).
    doc::Sequence& sequence() {
        if (doc::Sequence* s = document.find_sequence(scope_look))
            return *s;
        return document.root();
    }
    const doc::Sequence& sequence() const {
        const doc::Sequence* s = document.find_sequence(scope_look);
        return s ? *s : document.root();
    }
    // The scoped entity's length: what the player spans.
    uint32_t scope_duration() const {
        if (scope_is_look()) return doc::look_duration(document, look());
        return doc::sequence_duration(document, sequence());
    }

    // Two different questions the UI used to ask the player. A TIMELINE
    // exists whenever the scoped look has length - generators alone are
    // enough. MEDIA is about the primary clip bundle: its name, its
    // analysis, its still duration, the proxy row.
    bool has_timeline() const { return player.frame_count() > 0; }
    bool has_media() const { return !mez_path.empty(); }

    media::Player player;
    std::unique_ptr<ImportJob> import;
    std::unique_ptr<ExportJob> export_job;
    std::filesystem::path mez_path, pcm_path;   // PRIMARY clip bundle
    // Every asset's resolved media, plus its PCM in RAM. Rebuilt when the
    // asset list, the proxy toggle or a bundle on disk changes; the stamp
    // is what the render worker and the mix watch.
    std::vector<media::AssetBundle> bundles;
    PcmCache pcm_cache;
    uint64_t bundle_stamp = 1;
    // The mix published to the transport, rebuilt when the document or the
    // bundles move (gain, mute, placement and nesting all feed it).
    uint64_t mix_revision = ~0ull;
    uint64_t mix_stamp = ~0ull;
    uint64_t mix_look = 0;
    // Audio Scope: which PCM the render engine currently holds (the main
    // loop polls pcm_path and re-posts on change; empty = silent).
    std::filesystem::path scope_pcm_loaded;
    bool scope_pcm_init = false;
    mod::AnalysisCurves analysis;               // merged view (see sidechain)
    bool has_analysis = false;
    // Bumped whenever `analysis` is replaced — the render worker copies
    // the curves only when this moves.
    uint64_t analysis_stamp = 1;
    // Preview render thread; owned by wWinMain, pointer here so
    // clip open/close paths can pause it around player mutation.
    RenderWorker* render_worker = nullptr;
    uint64_t ui_frame_counter = 0;
    // Sidechain: analysis = clip video curves + (sidechain or
    // clip) audio curves. clip_analysis keeps the clip's own set.
    mod::AnalysisCurves clip_analysis;
    bool has_clip_analysis = false;
    std::string sc_active_path;         // sidechain merged into `analysis`
    std::filesystem::path sc_pcm_path;  // extracted PCM cache (export mux)
    bool sc_ok = false;
    double env_key_time = -1.0;         // live keypress trigger
    // Thumbnail strip: RGBA staging until the renderer registers
    // it (textures are long-lived; a new clip just registers another one).
    std::vector<uint8_t> thumbs_rgba;
    uint32_t thumbs_w = 0, thumbs_h = 0, thumbs_count = 0;
    bool thumbs_dirty = false;
    const ui::UiTexture* thumbs_tex = nullptr;
    // Half-res proxy: which file the player currently plays.
    bool proxy_active = false;
    // Lossless import (mezzanine option), applies to the NEXT
    // import. App preference (ui.json), not project state.
    bool import_lossless = false;
    // Preset browser search: plain substring filter. While the
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
    ConfirmDialog confirm;      // in-app modal guard (unsaved / restore)
    // Timeline audio-strip acceleration: per-frame combined amplitude plus
    // 64-frame block maxima, rebuilt when the analysis stamp moves. The
    // strip's per-column max scan otherwise touches every frame in the
    // visible span each UI frame - milliseconds per frame zoomed out on
    // long clips.
    struct StripAccel {
        uint64_t stamp = 0;
        std::vector<float> amp_frame;         // max(low, mid, high) per frame
        std::vector<float> amp, onset, cut;   // per-block maxima
    } strip_accel;
    bool loop = true;

    size_t selected_layer = 0;      // the stack panel edits this layer
    // Three selection states drive the monitor: a NODE picked in the
    // graph previews its own output; else a LAYER picked in the
    // timeline/panel previews its whole contribution; else the film.
    // selected_layer stays the rail target either way; this flag says
    // the layer itself is picked. Canvas clicks never clear it -
    // working the graph is working inside the layer.
    bool layer_sel = false;

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
        std::vector<doc::ValueNode> value_nodes;
        std::vector<doc::NodeLink> links;
        float origin_x = 0.0f, origin_y = 0.0f;
    } clipboard;
    // Inline frame rename (canvas): the frame being renamed + edit buffer.
    uint64_t frame_rename_id = 0;
    std::string frame_rename_buf;
    // Inline group-card rename (texed subgraph title rename).
    uint64_t group_rename_id = 0;
    std::string group_rename_buf;
    // Inline Text-card string edit: the effect id + edit buffer.
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

    // Project file. autosaved_revision tracks what the last
    // autosave captured so quiet frames cost nothing.
    std::filesystem::path project_path;
    uint64_t saved_revision = 0;
    uint64_t autosaved_revision = 0;
    std::chrono::steady_clock::time_point last_autosave =
        std::chrono::steady_clock::now();

    // Preset browser: shipped era presets + user-saved ones.
    std::filesystem::path shipped_preset_dir, user_preset_dir;
    // Text-effect font list: '|'-joined stems of assets/fonts/
    // *.ttf, same lowercased-filename order the engine indexes — feeds
    // the font dropdown. Scanned once at startup.
    std::string font_options;
    std::vector<doc::Preset> presets;
    int preset_tag_index = -1;      // -1 = all tags

    // Randomize: chaos = intensity; counter advances per gesture
    // so repeated clicks explore, undo walks back one gesture at a time.
    float chaos = 0.5f;
    uint64_t rng_counter = 1;

    // Live mode: timeline collapses, transport loops, LFO/drift
    // run on this wall clock (exempt from determinism, ).
    bool live_mode = false;
    double app_seconds = 0.0;

    // Preview proxy divisor: 1 full, 2 half, 4 quarter.
    uint32_t preview_div = 1;

    // UI preferences — app-level view state persisted in ui.json next to
    // the exe (deliberately not project state): active theme + sidebar
    // section folds (layers, stack, presets, mod matrix).
    int theme_index = 0;
    bool sec_open[5] = {true, true, true, true, true};
    ui::ButtonState sec_buttons[5];
    ui::DropdownState theme_dd;
    ui::ScrollState timeline_scroll;

    // Timeline view: the visible frame range shared by the ruler,
    // audio strip, and every lane so they stay column-aligned. v1 <= v0
    // reads as "whole clip". Wheel zooms around the cursor, shift+wheel
    // pans; zooming fully out restores the whole-clip view.
    double tl_v0 = 0.0, tl_v1 = 0.0;
    // Timeline region rect: strips union into _accum during draw, the
    // frame loop swaps it in — the wheel pre-router and the keyboard
    // router (Delete / Ctrl+C / Ctrl+V go to keys when hovered) read the
    // one-frame-stale copy.
    ui::Rect tl_rect{}, tl_rect_accum{};
    float tl_strip_x = 0.0f, tl_strip_w = 0.0f;   // ruler column x span
    // Block lanes (docs/look.md phase 5): one drag at a time. Mode 0
    // idle, 1 move, 2 trim-in, 3 trim-out; anchor is the frame under the
    // press. Widget-id anchors per lane row (stable addresses).
    // The picked BLOCK (view state, 0 = none): Delete removes it with
    // its link group; razor narrows to its layer; it wears the outline.
    uint64_t sel_placement = 0;
    // Double-click detection on blocks: a second press on the same
    // placement inside the window opens its target for editing.
    uint64_t last_block_pick = 0;
    double last_block_pick_time = -1.0e9;
    // Drag snapping (S toggles): candidates rebuilt each frame from
    // every lane's edges plus the static marks (playhead, trim, loop,
    // markers, zero). Threshold is in PIXELS, so zoom decides reach;
    // the engaged target draws a line across the lanes.
    bool tl_snap = true;
    double tl_snap_frame = -1.0;
    struct TlSnapEdge {
        double frame;
        uint64_t pid;    // owning placement, 0 = a static mark
        uint64_t link;   // its link group (partners move together)
    };
    std::vector<TlSnapEdge> tl_snap_edges;
    uint64_t blk_drag_layer = 0;
    uint64_t blk_drag_placement = 0;
    int blk_drag_mode = 0;
    double blk_drag_anchor = 0.0;
    doc::Placement blk_drag_orig;
    float frame_dt = 1.0f / 60.0f;   // seconds, for rate-based drags
    // One anchor per lane row: video lanes then audio lanes.
    char tl_lane_ids[doc::kMaxLayers * 2 + 1] = {};
    // Per-asset loudness silhouettes at the asset's frame rate, from the
    // PCM cache. Display only; rebuilt when the bundle table moves. An
    // asset with no PCM has no entry, which is what gates the timeline's
    // audio display (a silent look must not read as having audio).
    struct AssetAmp {
        uint64_t stamp = ~0ull;
        std::unordered_map<uint64_t, std::vector<float>> amp;
    } asset_amp;
    // Key clipboard: copies the selected keys of one lane,
    // normalized to the first key; paste lands at the playhead in the
    // source lane.
    std::vector<doc::Keyframe> key_clipboard;
    doc::ParamKey key_clip_target{};
    // Inline key readout editor: click the selected key's value
    // readout to type it (shift+click types the frame); identity by frame
    // so the doc round-trip cannot lose the key.
    int key_edit_mode = 0;   // 0 closed, 1 value, 2 frame
    doc::ParamKey key_edit_target{};
    double key_edit_frame = 0.0;
    std::string key_edit_buf;
    // Rail inline value editor: click a slider's value text to
    // type it; the commit flows through the row's normal staged path on
    // the next build. effect_id 0 = closed.
    doc::ParamKey rail_edit_key{};
    float rail_edit_scale = 1.0f;   // the row's display multiplier (deg)
    std::string rail_edit_buf;
    bool rail_edit_commit = false;
    // Param clipboard (ctx menu): whole param set of one effect,
    // pasteable onto any same-type instance.
    bool param_clip_valid = false;
    doc::EffectType param_clip_type = doc::EffectType::RgbSplit;
    std::vector<float> param_clip_values;
    float param_clip_wet = 1.0f, param_clip_opacity = 1.0f;

    // Add-effect browser (view state): one fold per category
    // nested inside the stack section.
    bool add_fx_open = false;
    bool fx_cat_open[static_cast<size_t>(doc::FxCategory::Count)] = {};
    ui::ButtonState add_fx_button,
        fx_cat_buttons[static_cast<size_t>(doc::FxCategory::Count)];
    // Add-node search (docs/flow_canvas.md): same capture-the-keyboard
    // field pattern as the preset search; non-empty = flat filtered list.
    std::string fx_filter;
    bool fx_search_focus = false;
    ui::ButtonState fx_search_btn;

    // Viewport A/B wipe + bypass-all + alpha checker. View state, not
    // document state — no undo, never exported.
    bool ab_wipe = false;
    float wipe_pos = 0.5f;
    bool bypass_all = false;
    bool alpha_checker = false;

    // Render queue: pending exports, each a full snapshot taken
    // at queue time (document, analysis, clip bundle) so edits made while
    // a job runs don't leak into it. FIFO; the front starts when the
    // active job finishes.
    struct QueuedExport {
        std::filesystem::path out_path;
        doc::Document doc;
        // Which look this job renders, captured with the snapshot.
        uint64_t look_id = 0;
        bool has_analysis = false;
        mod::AnalysisCurves analysis;
        std::vector<media::AssetBundle> bundles;
        PcmCache pcm;
        std::filesystem::path scope_pcm;
    };
    std::vector<QueuedExport> export_queue;
    ui::ButtonState queue_remove_buttons[8];

    // (Preview-side decode/remap/cache state lives on the render worker —
    // render thread.)

    // Widget state
    std::unordered_map<uint64_t, LayerUiState> layer_ui;
    // Audio lane label controls (docs/look.md): per-track gain + mute -
    // the per-track half of the mixer.
    struct AudioLaneUi {
        ui::SliderState gain;
        ui::ButtonState mute;
    };
    std::unordered_map<uint64_t, AudioLaneUi> audio_ui;
    // Browser rows (sequences + assets) on the project tab.
    struct BrowserRowUi {
        ui::ButtonState open, place;
    };
    std::unordered_map<uint64_t, BrowserRowUi> browser_ui;
    ui::ButtonState new_seq_button;
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
    ui::ButtonState ab_button, bypass_all_button;
    ui::SliderState wipe_slider;
    ui::ButtonState add_layer_buttons[7];
    std::unordered_map<uint64_t, EffectUiState> fx_ui;
    std::unordered_map<uint64_t, RouteUiState> route_ui;
    ui::ButtonState add_layer_open_button;
    ui::ButtonState add_frame_button;
    ui::ButtonState open_add_button;
    std::map<std::pair<uint64_t, int>, LaneUiState> lane_ui;
    ui::ButtonState open_button, open_big_button, play_button, undo_button,
        redo_button, export_button, new_look_button;
    ui::ButtonState export_cancel_button, export_audio_check;
    ui::SliderState export_bitrate_slider;
    ui::DropdownState export_scale_dd;
    // Monitor volume: app-level prefs, persisted in ui.json.
    bool audio_muted = false;
    float audio_gain = 1.0f;
    ui::ButtonState mute_button;
    ui::SliderState volume_slider;
    // Recent projects: newest first, capped, persisted in ui.json;
    // listed on the project tab.
    std::vector<std::string> recent_projects;
    ui::ButtonState recent_buttons[6];
    // Cache management: size scanned at startup and after edits.
    uint64_t cache_bytes = 0;
    ui::ButtonState cache_open_button, cache_clear_button;
    // Status history: every distinct status line, newest last —
    // errors stop vanishing when the next status overwrites the strip.
    std::vector<std::string> status_log;
    std::string status_log_last;
    ui::ButtonState add_buttons[static_cast<size_t>(doc::EffectType::Count)];
    ui::ButtonState snap_apply[3], snap_store[3];
    ui::SliderState seek_slider;
    ui::ScrubberState scrubber;
    ui::ButtonState loop_check;
    ui::ScrollState sidebar_scroll, right_scroll, preset_scroll;
    RulerState ruler;
};

// ---- flow-canvas selection helpers (docs/flow_canvas.md)

bool find_effect_by_id(const doc::Look& look, uint64_t id,
                       size_t* layer_index, size_t* fx_index) {
    for (size_t li = 0; li < look.layers.size(); ++li)
        for (size_t i = 0; i < look.layers[li].stack.size(); ++i)
            if (look.layers[li].stack[i].id == id) {
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
    for (const doc::KeyframeLane& lane : app.look().lanes)
        if (lane.target == key && !lane.keys.empty() && !lane.muted) {
            const double ph = app.has_timeline()
                ? app.player.current_frame_index()
                : 0.0;
            return std::clamp(mod::eval_lane(lane, ph), min_v, max_v);
        }
    return base;
}

bool find_group_by_id(const doc::Look& look, uint64_t id,
                      size_t* layer_index) {
    for (size_t li = 0; li < look.layers.size(); ++li)
        for (const doc::Group& g : look.layers[li].groups)
            if (g.id == id) {
                *layer_index = li;
                return true;
            }
    return false;
}

int layer_index_by_id(const doc::Look& look, uint64_t id) {
    for (size_t li = 0; li < look.layers.size(); ++li)
        if (look.layers[li].id == id) return static_cast<int>(li);
    return -1;
}

// Drops a selection whose subject no longer exists (undo, remove, load).
void validate_selection(AppState& app) {
    const doc::Look& d = app.look();
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
        case SelKind::ModSource:
            if (!doc::find_value_node(d, app.sel.id)) app.sel = {};
            break;
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
            case flow::NodeKind::ModSource:
                return doc::find_value_node(d, did) != nullptr;
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

    // Thumbnail strip: RGB thumbs -> one horizontal RGBA strip,
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

// Sidechain: keep `analysis` = clip video curves + the sidechain
// audio curves. Extraction result is cached next to the clip bundle as
// <stem>.sc.pcm and re-analyzed only when the document path changes.
void sync_sidechain(AppState& app) {
    if (!app.has_media()) return;   // the .sc.pcm cache sits by the bundle
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
    // Monitor volume + recent projects.
    v.set("volume", static_cast<double>(app.audio_gain));
    if (app.audio_muted) v.set("muted", true);
    if (!app.recent_projects.empty()) {
        json::Value recents = json::Value::make_array();
        for (const std::string& r : app.recent_projects) recents.push(r);
        v.set("recent", std::move(recents));
    }
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
    app.audio_gain = std::clamp(
        static_cast<float>(parsed.value->get("volume").as_number(1.0)),
        0.0f, 1.5f);
    app.audio_muted = parsed.value->get("muted").as_bool(false);
    for (const json::Value& rv : parsed.value->get("recent").array()) {
        if (app.recent_projects.size() >= 6) break;
        std::string p = rv.as_string();
        if (!p.empty()) app.recent_projects.push_back(std::move(p));
    }
    ui::set_active_theme(app.theme_index);
}

// Open a clip: an existing sidecar bundle (<stem>.mez[/.pcm]) opens
// directly; otherwise a background import produces one first.
bool set_still_frames(AppState& app, uint32_t frames);

// Resolves every asset's media, refreshes what the document caches about
// it (length, rate, size — the timeline's clock and canvas derive from
// these), and loads its PCM. Cheap enough to call whenever the asset list,
// the proxy toggle or a bundle on disk may have moved; the stamp it bumps
// is what the render worker and the mix watch.
void refresh_bundles(AppState& app) {
    std::vector<media::AssetBundle> table;
    bool doc_changed = false;
    for (doc::Asset& asset : app.document.assets) {
        media::AssetBundle bundle;
        bundle.asset = asset.id;
        if (!asset.path.empty()) {
            const BundlePaths paths =
                resolve_bundle(std::filesystem::path(asset.path));
            if (paths.ready) {
                // Preview may run the half-res proxy; export never does.
                std::filesystem::path open_path = paths.mez;
                if (app.document.use_proxy) {
                    std::filesystem::path proxy = paths.mez;
                    proxy.replace_extension(".proxy.mez");
                    std::error_code ec;
                    if (std::filesystem::exists(proxy, ec)) open_path = proxy;
                }
                codec::MezReader reader;
                std::string error;
                if (reader.open(open_path, &error)) {
                    bundle.mez = open_path;
                    bundle.pcm = paths.pcm;
                    bundle.frames = reader.frame_count();
                    bundle.width = reader.width();
                    bundle.height = reader.height();
                    bundle.fps = reader.fps();
                    bundle.timescale = reader.timescale();
                    bundle.frame_duration = reader.frame_duration();
                }
            }
        }
        // The document caches what looks need before any decode opens.
        // Direct write, like the clip binding: media facts, not edits.
        if (asset.frame_count != bundle.frames || asset.fps != bundle.fps ||
            asset.width != bundle.width || asset.height != bundle.height) {
            asset.frame_count = bundle.frames;
            asset.fps = bundle.fps;
            asset.width = bundle.width;
            asset.height = bundle.height;
            doc_changed = true;
        }
        if (!bundle.pcm.empty() && !app.pcm_cache.count(asset.id))
            app.pcm_cache[asset.id] = media::load_pcm(bundle.pcm);
        table.push_back(std::move(bundle));
    }
    // Assets that went away must not keep their PCM alive.
    for (auto it = app.pcm_cache.begin(); it != app.pcm_cache.end();) {
        if (media::find_bundle(table, it->first))
            ++it;
        else
            it = app.pcm_cache.erase(it);
    }
    app.bundles = std::move(table);
    ++app.bundle_stamp;
    app.mix_stamp = ~0ull;   // the mix is stale by construction
    if (doc_changed) ++app.document.revision;
}

// Republishes the monitor mix when the document, the scope or the media
// moved. Everything the mix depends on — placement, nesting, gain, mute,
// which asset is bound — lives in one of those three.
void refresh_mix(AppState& app) {
    if (app.mix_revision == app.document.revision &&
        app.mix_stamp == app.bundle_stamp && app.mix_look == app.scope_look)
        return;
    app.mix_revision = app.document.revision;
    app.mix_stamp = app.bundle_stamp;
    app.mix_look = app.scope_look;
    auto mix = std::make_shared<media::MixState>(build_mix(
        app.document, app.scope_look, app.pcm_cache,
        project_fps(app.document, app.bundles), app.player.audio_sample_rate(),
        app.player.audio_channels()));
    app.player.set_mix(std::move(mix));
}

// Rebuilds the per-asset loudness silhouettes the timeline blocks draw:
// one float per ASSET frame, max |sample| over the frame's span. Only
// assets whose PCM actually loaded get an entry — presence in this map IS
// the "block has audio" test.
void refresh_asset_amp(AppState& app) {
    if (app.asset_amp.stamp == app.bundle_stamp) return;
    app.asset_amp.stamp = app.bundle_stamp;
    app.asset_amp.amp.clear();
    for (const auto& [asset_id, pcm] : app.pcm_cache) {
        if (!pcm || pcm->rate == 0 || pcm->channels == 0 ||
            pcm->samples.empty())
            continue;
        const media::AssetBundle* b = media::find_bundle(app.bundles, asset_id);
        const double fps = b && b->fps > 0.0
            ? b->fps
            : project_fps(app.document, app.bundles);
        const uint64_t pcm_frames = pcm->frames();
        const uint32_t frames = static_cast<uint32_t>(
            pcm_frames * fps / pcm->rate) + 1;
        std::vector<float> amp(frames, 0.0f);
        const uint32_t ch = pcm->channels;
        for (uint64_t s = 0; s < pcm_frames; ++s) {
            int32_t peak = 0;
            for (uint32_t c = 0; c < ch; ++c)
                peak = std::max(peak, std::abs(static_cast<int32_t>(
                                    pcm->samples[s * ch + c])));
            const uint32_t f = static_cast<uint32_t>(
                s * fps / pcm->rate);
            if (f < frames)
                amp[f] = std::max(amp[f],
                                  static_cast<float>(peak) / 32768.0f);
        }
        app.asset_amp.amp.emplace(asset_id, std::move(amp));
    }
}

// The look wrapping an asset: a clip is the simplest look, so raw media
// never sits on a timeline. Reused per asset - dropping the same file
// twice places the same treated clip twice (templates by design).
uint64_t find_wrapper_look(const doc::Document& doc, uint64_t asset_id) {
    for (const doc::Look& l : doc.looks)
        if (l.layers.size() == 1 && doc::layer_is_clip(l.layers[0]) &&
            l.layers[0].asset == asset_id)
            return l.id;
    return 0;
}

// Adds `picked` to the project as an ASSET and places it at `at_frame`:
// at sequence scope the clip arrives WRAPPED IN A LOOK and lands as a
// block on the first lane (with its linked audio pair when the media has
// sound); scoped inside a look it lands as a clip NODE in the graph -
// looks are timeless, so a drop into one carries no when. Reuses an
// asset already bound to the same path so dropping twice does not import
// twice. Returns false when the media has no usable bundle yet (the
// caller imports first); everything it does lands in ONE undo step.
bool place_clip_block(AppState& app, const std::filesystem::path& picked,
                      uint32_t at_frame) {
    const BundlePaths paths = resolve_bundle(picked);
    if (!paths.ready) return false;

    uint64_t asset_id = 0;
    for (const doc::Asset& a : app.document.assets)
        if (a.path == picked.string()) asset_id = a.id;

    if (app.scope_is_look()) {
        // Graph drop: a clip node, in lockstep like every source.
        if (app.look().layers.size() >= doc::kMaxLayers) {
            app.status = "layer limit reached";
            return true;
        }
        app.undo.begin_group("Add Clip");
        if (!asset_id) {
            doc::Asset asset = doc::make_asset(app.document,
                                               picked.filename().string(),
                                               picked.string());
            asset_id = asset.id;
            app.undo.execute(app.document,
                             doc::add_asset_command(std::move(asset)));
        }
        app.undo.execute(app.document,
                         doc::materialize_links_command(app.look().id));
        doc::Layer layer =
            doc::make_layer(app.document, doc::LayerSourceKind::Clip);
        layer.name = picked.stem().string();
        layer.asset = asset_id;
        const uint64_t layer_id = layer.id;
        app.undo.execute(app.document,
                         doc::add_layer_command(app.look().id,
                                                std::move(layer),
                                                app.look().layers.size()));
        app.undo.execute(
            app.document,
            doc::connect_command(app.look().id, {layer_id, 0, 0}));
        app.undo.end_group();
        refresh_bundles(app);
        app.selected_layer = app.look().layers.size() - 1;
        app.layer_sel = false;
        app.status = "added " + picked.filename().string();
        return true;
    }

    doc::Sequence& seq = app.sequence();
    if (seq.tracks.empty() ||
        seq.tracks.front().placements.size() >=
            doc::kMaxPlacementsPerTrack) {
        app.status = "lane is full";
        return true;
    }
    app.undo.begin_group("Add Clip");
    if (!asset_id) {
        doc::Asset asset = doc::make_asset(app.document,
                                           picked.filename().string(),
                                           picked.string());
        asset_id = asset.id;
        app.undo.execute(app.document,
                         doc::add_asset_command(std::move(asset)));
    }
    uint64_t wrapper = find_wrapper_look(app.document, asset_id);
    if (!wrapper) {
        doc::Look look = doc::make_look(app.document, picked.stem().string());
        doc::Layer clip =
            doc::make_layer(app.document, doc::LayerSourceKind::Clip);
        clip.name = picked.stem().string();
        clip.asset = asset_id;
        look.layers.push_back(std::move(clip));
        wrapper = look.id;
        app.undo.execute(app.document,
                         doc::add_look_command(std::move(look)));
    }
    doc::Placement block;
    block.id = app.document.next_effect_id++;
    block.target = wrapper;
    block.t_in = at_frame;
    block.t_out = 0;   // runs as long as the media does
    const uint64_t video_place_id = block.id;
    app.undo.execute(app.document,
                     doc::add_placement_command(
                         seq.id, seq.tracks.front().id, block));
    // A clip WITH sound lays a linked audio placement beside the video
    // one: audio presence is a placement that exists, never an inference
    // from the asset. Silent media lays none.
    if (!paths.pcm.empty()) {
        doc::Placement ap;
        ap.target = wrapper;
        ap.t_in = at_frame;
        ap.t_out = 0;
        const uint64_t audio_track =
            seq.audio.empty() ? 0 : seq.audio.front().id;
        app.undo.execute(app.document,
                         doc::add_audio_placement_command(
                             app.document, seq.id, audio_track, ap,
                             video_place_id));
    }
    app.undo.end_group();

    // The asset's length and size are media facts, not edits: probe them
    // so the block gets a real span and the canvas a real size.
    refresh_bundles(app);
    // Arranging picks nothing - no node, no layer: the monitor keeps
    // the film. The rail still lands on the placed lane.
    app.selected_layer = 0;
    app.layer_sel = false;
    app.sel_placement = 0;
    app.status = "placed " + picked.filename().string();
    return true;
}

// Places entity `target_id` (a look or a sequence) as a block at
// `at_frame` - the browser's "+". At look scope it lands as a lockstep
// ref NODE instead. Lays the linked audio placement when the target has
// any sound under it; one undo step; cycle-guarded both ways.
bool place_look_block(AppState& app, uint64_t target_id, uint32_t at_frame) {
    const doc::Look* tl = app.document.find_look(target_id);
    const doc::Sequence* ts =
        tl ? nullptr : app.document.find_sequence(target_id);
    if (!tl && !ts) return false;
    const std::string tname = tl ? tl->name : ts->name;

    if (app.scope_is_look()) {
        if (doc::nest_reaches(app.document, target_id, app.look().id)) {
            app.status = "that would loop - it cannot reach itself";
            return false;
        }
        if (app.look().layers.size() >= doc::kMaxLayers) {
            app.status = "layer limit reached";
            return false;
        }
        app.undo.begin_group("Place Ref");
        app.undo.execute(app.document,
                         doc::materialize_links_command(app.look().id));
        doc::Layer layer = doc::make_layer(
            app.document, tl ? doc::LayerSourceKind::LookRef
                             : doc::LayerSourceKind::SequenceRef);
        layer.name = tname.empty() ? "ref" : tname;
        layer.target = target_id;
        const uint64_t layer_id = layer.id;
        app.undo.execute(app.document,
                         doc::add_layer_command(app.look().id,
                                                std::move(layer),
                                                app.look().layers.size()));
        app.undo.execute(
            app.document,
            doc::connect_command(app.look().id, {layer_id, 0, 0}));
        app.undo.end_group();
        app.selected_layer = app.look().layers.size() - 1;
        app.layer_sel = false;
        app.status = "placed " + (tname.empty() ? "ref" : tname);
        return true;
    }

    doc::Sequence& seq = app.sequence();
    if (doc::nest_reaches(app.document, target_id, seq.id)) {
        app.status = "that would loop - it cannot reach itself";
        return false;
    }
    if (seq.tracks.empty() ||
        seq.tracks.front().placements.size() >=
            doc::kMaxPlacementsPerTrack) {
        app.status = "lane is full";
        return false;
    }
    app.undo.begin_group("Place Block");
    doc::Placement block;
    block.id = app.document.next_effect_id++;
    block.target = target_id;
    block.t_in = at_frame;
    const uint64_t video_place_id = block.id;
    app.undo.execute(app.document,
                     doc::add_placement_command(
                         seq.id, seq.tracks.front().id, block));
    // The pair rule: a target WITH sound arrives with its submix linked;
    // a silent one lays no audio placement at all.
    if (!doc::flatten_audio_sources(app.document, target_id).empty()) {
        doc::Placement ap;
        ap.target = target_id;
        ap.t_in = at_frame;
        const uint64_t audio_track =
            seq.audio.empty() ? 0 : seq.audio.front().id;
        app.undo.execute(app.document,
                         doc::add_audio_placement_command(
                             app.document, seq.id, audio_track, ap,
                             video_place_id));
    }
    app.undo.end_group();
    // Arranging picks nothing - no node, no layer: the monitor keeps
    // the film.
    app.selected_layer = 0;
    app.layer_sel = false;
    app.sel_placement = 0;
    app.status = "placed " + (tname.empty() ? std::string("block") : tname);
    return true;
}

// "Open clip" and a finished import ENSURE the clip is on the timeline:
// one block of its wrapper look (with the linked audio pair), laid
// through the SAME undoable path a drop uses - there is exactly one way
// anything enters the arrangement, and Delete makes it stay gone.
// Already-placed clips place nothing, so reopening never stacks blocks;
// at look scope the clip lands as a graph node instead (place_clip_block
// branches there).
void ensure_clip_placed(AppState& app,
                        const std::filesystem::path& picked) {
    if (!app.scope_is_look()) {
        uint64_t asset_id = 0;
        for (const doc::Asset& a : app.document.assets)
            if (a.path == picked.string()) asset_id = a.id;
        if (asset_id) {
            const uint64_t wrapper =
                find_wrapper_look(app.document, asset_id);
            if (wrapper)
                for (const doc::SeqTrack& t : app.sequence().tracks)
                    for (const doc::Placement& p : t.placements)
                        if (p.target == wrapper) return;
        }
    }
    place_clip_block(app, picked, app.player.current_frame_index());
}

// Generic IMPORT: the asset joins the browser and NOTHING is placed or
// played - placing is a separate, deliberate act (drop, browser "+").
// Media without a bundle runs the import job flagged import_only.
void import_media(AppState& app, const std::filesystem::path& picked) {
    for (const doc::Asset& a : app.document.assets)
        if (a.path == picked.string()) {
            app.status =
                "already imported: " + picked.filename().string();
            return;
        }
    const BundlePaths paths = resolve_bundle(picked);
    if (paths.ready) {
        doc::Asset asset = doc::make_asset(
            app.document, picked.filename().string(), picked.string());
        app.undo.execute(app.document,
                         doc::add_asset_command(std::move(asset)));
        refresh_bundles(app);
        app.status = "imported " + picked.filename().string();
        return;
    }
    if (app.import) {
        app.status = "an import is already running";
        return;
    }
    const std::filesystem::path dest = bundle_dir_for(picked);
    std::error_code cec;
    std::filesystem::create_directories(dest, cec);
    app.import = start_import(picked, app.import_lossless,
                              cec ? picked.parent_path() : dest);
    app.import->import_only = true;
}

// Browse-and-bind for a clip NODE (card row and inspector button share
// this): existing or ready media binds now in one undo group; fresh
// media runs the import job carrying the bind target.
void browse_and_bind_clip(AppState& app, platform::Window* window,
                          uint64_t look_id, uint64_t layer_id) {
    if (app.import) {
        app.status = "an import is already running";
        return;
    }
    auto picked = platform::show_open_dialog(
        window, {{"video / image", "*.mp4;*.mov;*.mez;*.png;*.tga"},
                 {"all files", "*.*"}});
    if (!picked) return;
    uint64_t asset_id = 0;
    for (const doc::Asset& a : app.document.assets)
        if (a.path == picked->string()) asset_id = a.id;
    const BundlePaths paths = resolve_bundle(*picked);
    if (asset_id || paths.ready) {
        doc::Look* look = app.document.find_look(look_id);
        if (!look) return;
        doc::Layer* layer = nullptr;
        for (doc::Layer& l : look->layers)
            if (l.id == layer_id) layer = &l;
        if (!layer || !doc::layer_is_clip(*layer)) return;
        app.undo.begin_group("Bind Clip");
        if (!asset_id) {
            doc::Asset asset = doc::make_asset(
                app.document, picked->filename().string(),
                picked->string());
            asset_id = asset.id;
            app.undo.execute(app.document,
                             doc::add_asset_command(std::move(asset)));
        }
        doc::Layer edited = *layer;
        edited.asset = asset_id;
        app.undo.execute(
            app.document,
            doc::set_layer_props_command(look_id, std::move(edited)));
        app.undo.end_group();
        refresh_bundles(app);
        app.status = "bound " + picked->filename().string();
        return;
    }
    const std::filesystem::path dest = bundle_dir_for(*picked);
    std::error_code cec;
    std::filesystem::create_directories(dest, cec);
    app.import = start_import(*picked, app.import_lossless,
                              cec ? picked->parent_path() : dest);
    app.import->import_only = true;
    app.import->bind_look = look_id;
    app.import->bind_layer = layer_id;
}

void open_source(AppState& app, const std::filesystem::path& picked) {
    // Bind the clip to the document so save/open restores it. Direct write,
    // not a command: the clip binding is environment, not an undoable edit.
    bind_primary_clip(app.document, picked.string());
    app.duration_focus = false;
    app.duration_edit.clear();
    // The render worker must not touch the decode pool's files while the
    // bundle table moves under it.
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
    // Bundle location (scratch disk): a bundle that already sits next to
    // the source (hand-built, or from before the cache move) is honored;
    // otherwise bundles live under cache/<path-hash>/ beside the exe so the
    // app never dumps files into footage folders.
    const BundlePaths paths = resolve_bundle(picked);
    if (paths.ready) {
        app.clip_name = picked.filename().string();
        app.mez_path = paths.mez;
        app.pcm_path = paths.pcm;
        refresh_bundles(app);
        ensure_clip_placed(app, picked);
        load_clip_analysis(app);
        app.player.set_looping(app.loop);
        app.player.play();
        app.status.clear();
        // Persisted still duration (project state): reconcile the bundle —
        // fresh, rebuilt, or edited elsewhere — to the document's length.
        if (primary_still_duration(app.document) > 0 &&
            is_still_source(picked))
            set_still_frames(app, primary_still_duration(app.document));
    } else {
        app.clip_name.clear();
        app.mez_path.clear();
        app.pcm_path.clear();
        refresh_bundles(app);
        const std::filesystem::path dest = bundle_dir_for(picked);
        std::error_code cec;
        std::filesystem::create_directories(dest, cec);
        app.import = start_import(picked, app.import_lossless,
                                  cec ? picked.parent_path() : dest);
        app.status.clear();
    }
}

// Core of a still-duration change: rewrite the bundle's frame index in
// place (the hold-frame trick after the fact — no re-encode) and re-read
// the bundle at the new length. The pool's readers hold the old index until
// their next open; harmless for a still, where every frame is one payload.
// Returns true when the asset runs at `frames`.
bool set_still_frames(AppState& app, uint32_t frames) {
    if (app.mez_path.empty() || frames == 0) return false;
    const doc::Asset* primary = app.document.primary_asset();
    if (primary && primary->frame_count == frames) return true;
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
    const bool rewrote = codec::mez_set_frame_count(app.mez_path, frames);
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
    refresh_bundles(app);
    return rewrote;
}

// Duration entry commit: apply, then persist in the DOCUMENT — the scratch
// bundle is regenerable (cache clear, another machine), so the project
// file is the durable record and reopening reconciles the bundle to it.
void apply_still_duration(AppState& app, double seconds) {
    if (app.mez_path.empty()) return;
    const double fps = app.player.fps() > 0.0 ? app.player.fps() : 30.0;
    const uint32_t frames = std::clamp(
        static_cast<uint32_t>(seconds * fps + 0.5), 1u,
        static_cast<uint32_t>(fps * 3600.0));   // up to an hour
    if (!set_still_frames(app, frames)) return;
    doc::Asset* asset =
        app.document.assets.empty() ? nullptr : &app.document.assets.front();
    if (asset && asset->still_duration_frames != frames) {
        // Direct write + revision bump, like the clip binding: not an
        // undoable edit (undo cannot restore the rewritten file), but it
        // must dirty the project and refresh the worker's doc copy.
        asset->still_duration_frames = frames;
        ++app.document.revision;
    }
}

void rescan_presets(AppState& app) {
    int failed = 0;
    app.presets = doc::scan_presets(app.shipped_preset_dir, &failed);
    std::vector<doc::Preset> user =
        doc::scan_presets(app.user_preset_dir, &failed);
    for (doc::Preset& p : user) app.presets.push_back(std::move(p));
    // A corrupt preset must not vanish silently.
    if (failed > 0)
        app.status = std::to_string(failed) +
                     " preset file(s) failed to load";
}

// Cache footprint: summed on demand — startup and after clears —
// never per frame.
uint64_t scan_cache_bytes() {
    uint64_t total = 0;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(
             executable_dir() / "cache", ec);
         !ec && it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
        if (it->is_regular_file(ec)) total += it->file_size(ec);
    }
    return total;
}

// Recent-projects list: newest first, deduped, capped; persisted
// with the ui prefs and listed on the project tab.
void remember_recent_project(AppState& app,
                             const std::filesystem::path& path) {
    const std::string s = path.string();
    auto& recents = app.recent_projects;
    recents.erase(std::remove(recents.begin(), recents.end(), s),
                  recents.end());
    recents.insert(recents.begin(), s);
    if (recents.size() > 6) recents.resize(6);
    save_ui_prefs(app);
}

// Autosave target: titled projects snapshot beside their file,
// untitled sessions under cache/ — the highest-risk case (new work never
// saved) is exactly the one that must be covered.
std::filesystem::path autosave_path_for(const AppState& app) {
    if (app.project_path.empty())
        return executable_dir() / "cache" / "untitled.autosave.json";
    std::filesystem::path p = app.project_path;
    p.replace_extension(".autosave.json");
    return p;
}

void save_project(AppState& app, const std::filesystem::path& path) {
    // Rolling project versions: <name>.v1.json is the previous
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
        const bool was_untitled = app.project_path.empty();
        app.project_path = path;
        app.saved_revision = app.document.revision;
        app.autosaved_revision = app.document.revision;
        app.status = "saved " + path.filename().string();
        remember_recent_project(app, path);
        // The work now lives in a real file — retire the autosaves that
        // covered it.
        std::filesystem::remove(autosave_path_for(app), ec);
        if (was_untitled)
            std::filesystem::remove(
                executable_dir() / "cache" / "untitled.autosave.json", ec);
    } else {
        app.status = "save failed: " + path.string();
    }
}

// The load itself, after any autosave-restore choice has been made:
// `load_from` is the file read (project or its autosave), `path` the
// project identity it loads as.
void open_project_load(AppState& app, const std::filesystem::path& load_from,
                       const std::filesystem::path& path, bool restored) {
    std::string error;
    auto loaded = doc::load_document(load_from, &error);
    if (!loaded && restored) loaded = doc::load_document(path, &error);
    if (!loaded) {
        app.status = "open failed: " + error;
        return;
    }
    app.document = std::move(*loaded);
    app.undo.clear();
    // The scope belongs to the OLD document. Ids restart low in every
    // project, so a stale one does not merely dangle - it resolves to an
    // unrelated look in the new one, and AppState::look() only
    // self-repairs when the id is gone entirely.
    app.scope_look = app.document.root_sequence;
    app.open_group = 0;
    app.tl_v0 = app.tl_v1 = 0.0;   // re-resolve the view to the new span
    app.selected_layer = 0;
    app.layer_sel = false;
    app.sel_placement = 0;
    // A project opens on its timeline with nothing selected: the film.
    app.sel = {};
    app.insert_before_id = 0;
    app.project_path = path;
    app.saved_revision = app.autosaved_revision = app.document.revision;
    // A restored autosave is unsaved work — keep the dirty star lit so
    // the exit guard covers it until a real save.
    if (restored) app.saved_revision = app.document.revision - 1;
    remember_recent_project(app, path);
    std::string note = "opened " + path.filename().string();
    // Media is environment, not document: whatever the project names gets
    // resolved fresh, and a look of generators opens perfectly well
    // without any.
    auto drop_media = [&] {
        if (app.render_worker) {
            app.render_worker->pause();
            app.render_worker->invalidate();
        }
        app.clip_name.clear();
        app.mez_path.clear();
        app.pcm_path.clear();
        app.pcm_cache.clear();
        refresh_bundles(app);
        if (app.render_worker) app.render_worker->resume();
    };
    const std::filesystem::path clip = primary_clip_path(app.document);
    std::error_code ec;
    if (!clip.empty() && std::filesystem::exists(clip, ec)) {
        open_source(app, clip);   // may report its own failure
    } else {
        drop_media();
        if (!clip.empty()) note += " (clip missing)";
    }
    if (app.status.empty()) app.status = std::move(note);
}

void open_project(AppState& app, const std::filesystem::path& path,
                  platform::Window* window) {
    // Crash recovery: a newer .autosave.json beside the project
    // holds work the last session never saved — offer it before loading.
    std::filesystem::path auto_path = path;
    auto_path.replace_extension(".autosave.json");
    std::error_code e1, e2;
    if (window && std::filesystem::exists(auto_path, e1) &&
        std::filesystem::last_write_time(auto_path, e1) >
            std::filesystem::last_write_time(path, e2) &&
        !e1 && !e2) {
        ConfirmDialog d;
        d.kind = ConfirmDialog::Kind::YesNo;
        d.action = ConfirmDialog::Action::RestoreProjectAutosave;
        d.title = "crash recovery";
        d.text = "a newer autosave of " + path.filename().string() +
                 " exists - restore it?";
        d.primary = "restore";
        d.secondary = "discard";
        d.path = auto_path;
        d.path2 = path;
        app.confirm = std::move(d);
        return;
    }
    open_project_load(app, path, path, false);
}

void open_project_via_dialog(AppState& app, platform::Window* window) {
    auto picked = platform::show_open_dialog(
        window, {{"looks project", "*.json"}, {"all files", "*.*"}});
    if (picked) open_project(app, *picked, window);
}

// Unsaved-changes guard: called before anything that would drop the
// document (close, open-over). True = clean, proceed now; false = the
// in-app confirm opened (or already owns the frame) and `action` runs on
// resolution instead. Untitled documents route through Save-As.
bool guard_unsaved_changes(AppState& app, ConfirmDialog::Action action,
                           std::filesystem::path payload = {}) {
    if (app.document.revision == app.saved_revision) return true;
    if (app.confirm.open()) return false;
    const std::string name = app.project_path.empty()
        ? std::string("untitled")
        : app.project_path.filename().string();
    ConfirmDialog d;
    d.kind = ConfirmDialog::Kind::SaveDiscard;
    d.action = action;
    d.title = "unsaved changes";
    d.text = "save changes to " + name + "?";
    d.primary = "save";
    d.secondary = "discard";
    d.path = std::move(payload);
    app.confirm = std::move(d);
    return false;
}

void run_confirm_action(AppState& app, const ConfirmDialog& d,
                        platform::Window* window, bool* running) {
    switch (d.action) {
        case ConfirmDialog::Action::CloseApp:
            *running = false;
            break;
        case ConfirmDialog::Action::OpenProjectDialog:
            open_project_via_dialog(app, window);
            break;
        case ConfirmDialog::Action::OpenProjectPath:
            open_project(app, d.path, window);
            break;
        default:
            break;
    }
}

// Dialog resolution. pick: 1 = primary, 2 = secondary, 3 = cancel.
void resolve_confirm(AppState& app, int pick, platform::Window* window,
                     bool* running) {
    // Take the dialog down before running anything: a continuation may
    // open the NEXT dialog (open-over-dirty chains into autosave-restore).
    ConfirmDialog d = std::move(app.confirm);
    app.confirm = {};
    if (d.kind == ConfirmDialog::Kind::SaveDiscard) {
        if (pick == 3) return;
        if (pick == 1) {
            std::filesystem::path path = app.project_path;
            if (path.empty()) {
                auto picked = platform::show_save_dialog(
                    window, {{"looks project", "*.json"}},
                    app.document.name + ".json");
                if (!picked) return;   // save-as declined = action aborted
                if (picked->extension() != ".json")
                    picked->replace_extension(".json");
                path = *picked;
            }
            save_project(app, path);
            if (app.document.revision != app.saved_revision)
                return;   // save failed — never drop the document
        }
        run_confirm_action(app, d, window, running);
        return;
    }
    std::error_code ec;
    switch (d.action) {
        case ConfirmDialog::Action::RestoreProjectAutosave:
            if (pick == 1) {
                open_project_load(app, d.path, d.path2, true);
            } else {
                std::filesystem::remove(d.path, ec);   // declined = stale
                open_project_load(app, d.path2, d.path2, false);
            }
            break;
        case ConfirmDialog::Action::RestoreUntitledAutosave:
            if (pick == 1) {
                std::string error;
                if (auto rec = doc::load_document(d.path, &error)) {
                    app.document = std::move(*rec);
                    app.undo.clear();
                    app.saved_revision = app.document.revision - 1;
                    app.autosaved_revision = app.document.revision;
                    if (!primary_clip_path(app.document).empty()) {
                        const std::filesystem::path clip =
                            primary_clip_path(app.document);
                        if (std::filesystem::exists(clip, ec))
                            open_source(app, clip);
                    }
                    app.status = "restored unsaved session";
                } else {
                    app.status = "autosave restore failed: " + error;
                }
            }
            std::filesystem::remove(d.path, ec);   // retires either way
            break;
        default:
            break;
    }
}

// ---- confirm dialog modal: layout shared by the interaction pass (frame
// start, live input) and the draw pass (frame end, above everything).

struct ConfirmLayout {
    ui::Rect panel;
    ui::Rect button[3];
    int count = 0;                     // 3 = save/discard/cancel, 2 = yes/no
    const std::string* labels[3]{};
    std::vector<std::string> lines;    // body, wrapped to the panel
    float title_h = 0.0f, line_h = 0.0f, pad = 20.0f;
};

ConfirmLayout confirm_layout(const AppState& app, const ui::Font& font,
                             const ui::Rect& viewport) {
    static const std::string kCancel = "cancel";
    const ui::Theme& th = ui::active_theme();
    const ConfirmDialog& d = app.confirm;
    ConfirmLayout cl;
    cl.count = d.kind == ConfirmDialog::Kind::SaveDiscard ? 3 : 2;
    cl.labels[0] = &d.primary;
    cl.labels[1] = &d.secondary;
    cl.labels[2] = &kCancel;
    const float panel_w = std::min(400.0f, viewport.w - 48.0f);
    const float inner_w = panel_w - cl.pad * 2.0f;
    // Greedy word wrap against the panel width.
    std::string line;
    size_t pos = 0;
    while (pos <= d.text.size()) {
        size_t next = d.text.find(' ', pos);
        if (next == std::string::npos) next = d.text.size();
        const std::string word = d.text.substr(pos, next - pos);
        const std::string cand = line.empty() ? word : line + " " + word;
        if (!line.empty() &&
            ui::measure_text(font, cand, th.font_size).x > inner_w) {
            cl.lines.push_back(line);
            line = word;
        } else {
            line = cand;
        }
        pos = next + 1;
    }
    if (!line.empty()) cl.lines.push_back(line);
    cl.line_h = font.line_height() * th.font_size;
    cl.title_h = font.line_height() * th.font_size_heading;
    const float btn_h = 24.0f;
    const float panel_h = cl.pad + cl.title_h + 10.0f +
                          static_cast<float>(cl.lines.size()) * cl.line_h +
                          16.0f + btn_h + cl.pad;
    cl.panel = {std::round((viewport.w - panel_w) * 0.5f),
                std::round(std::max(24.0f, viewport.h * 0.38f -
                                               panel_h * 0.5f)),
                panel_w, panel_h};
    // Buttons right-aligned, primary rightmost.
    float bx = cl.panel.right() - cl.pad;
    const float by = cl.panel.bottom() - cl.pad - btn_h;
    for (int i = 0; i < cl.count; ++i) {
        const float bw = std::max(
            64.0f,
            ui::measure_text(font, *cl.labels[i], th.font_size).x + 24.0f);
        bx -= bw;
        cl.button[i] = {bx, by, bw, btn_h};
        bx -= 8.0f;
    }
    return cl;
}

// Pointer/keyboard logic against the LIVE input; the caller deadens the
// input afterwards so the frame under the scrim sees nothing. pick_key
// carries the event loop's enter (1) / escape (2 or 3) mapping.
void confirm_interact(AppState& app, const ui::UiInput& input,
                      const ui::Font& font, const ui::Rect& viewport,
                      int pick_key, platform::Window* window,
                      bool* running) {
    const ConfirmLayout cl = confirm_layout(app, font, viewport);
    ConfirmDialog& d = app.confirm;
    d.hovered = -1;
    int pick = pick_key;
    for (int i = 0; i < cl.count; ++i) {
        const bool inside = cl.button[i].contains(input.mouse);
        if (inside) d.hovered = i;
        ui::ButtonState& bs = d.buttons[i];
        if (inside && input.left_pressed()) bs.pressed = true;
        if (input.left_released()) {
            if (bs.pressed && inside && pick == 0) pick = i + 1;
            bs.pressed = false;
        }
    }
    if (pick) resolve_confirm(app, pick, window, running);
}

void draw_confirm_dialog(ui::Canvas2D& canvas, const ui::Font& font,
                         const ui::Font* header_font,
                         const ui::Rect& viewport, AppState& app, float dt) {
    const ui::Theme& th = ui::active_theme();
    const ConfirmLayout cl = confirm_layout(app, font, viewport);
    ConfirmDialog& d = app.confirm;
    canvas.draw_sdf_rect(viewport, 0.0f, ui::Color{0.0f, 0.0f, 0.0f, 0.45f});
    const float radius = th.corner_radius * 2.0f;
    canvas.draw_sdf_rect(cl.panel, radius, th.panel_bg);
    canvas.draw_sdf_rect_outline(cl.panel, radius, th.stroke_width,
                                 th.hairline);
    float y = cl.panel.y + cl.pad;
    ui::draw_text(canvas, header_font ? *header_font : font, d.title,
                  {cl.panel.x + cl.pad, y}, th.font_size_heading, th.text);
    y += cl.title_h + 10.0f;
    for (const std::string& ln : cl.lines) {
        ui::draw_text(canvas, font, ln, {cl.panel.x + cl.pad, y},
                      th.font_size, th.text_dim);
        y += cl.line_h;
    }
    for (int i = 0; i < cl.count; ++i) {
        ui::ButtonState& bs = d.buttons[i];
        const float target = d.hovered == i ? 1.0f : 0.0f;
        bs.hover_t += (target - bs.hover_t) *
                      std::min(1.0f, dt * 14.0f);
        ui::Color bg =
            ui::lerp(th.control_bg, th.control_bg_hover, bs.hover_t);
        if (bs.pressed) bg = th.control_bg_active;
        const ui::Rect& r = cl.button[i];
        canvas.draw_sdf_rect(r, th.corner_radius, bg);
        canvas.draw_sdf_rect_outline(
            r, th.corner_radius, th.stroke_width,
            i == 0 ? th.accent_dim : th.hairline);
        const std::string& lab = *cl.labels[i];
        const Vec2 ts = ui::measure_text(font, lab, th.font_size);
        ui::draw_text(
            canvas, font, lab,
            {r.x + (r.w - ts.x) * 0.5f,
             r.y + (r.h - font.line_height() * th.font_size) * 0.5f},
            th.font_size, i == 0 ? th.text : th.text_dim);
    }
}

// UI-side per-frame job post (render thread): copies only what
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
        const bool want_source = app.ab_wipe || app.bypass_all;
        auto set = [&](auto& dst, const auto& src) {
            if (!(dst == src)) {
                dst = src;
                changed = true;
            }
        };
        if (j.bundle_stamp != app.bundle_stamp) {
            j.bundles = app.bundles;
            j.bundle_stamp = app.bundle_stamp;
            changed = true;
        }
        // Selection-follows preview, three states: a picked NODE
        // (effect/source/group card) publishes its own output; else a
        // picked LAYER publishes its whole contribution; else the
        // composite. Node selection comes from the GRAPH, layer
        // selection from the timeline/panel - each highlight means
        // exactly one thing and the monitor always agrees with it.
        uint64_t preview_key = 0;
        uint64_t preview_layer_key = 0;
        switch (app.sel.kind) {
            case SelKind::Effect:
            case SelKind::LayerSource:
            case SelKind::Group:
                preview_key = app.sel.id;
                break;
            default:
                break;
        }
        if (preview_key == 0 && app.layer_sel &&
            app.selected_layer < app.look().layers.size())
            preview_layer_key = app.look().layers[app.selected_layer].id;
        set(j.preview_node, preview_key);
        set(j.preview_layer, preview_layer_key);
        // The scoped ENTITY renders: the sequence's arrangement or the
        // look's graph - never a fallback.
        set(j.look_id, app.scope_look);
        set(j.preview_div, app.preview_div);
        set(j.live_mode, app.live_mode);
        set(j.want_source, want_source);
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
    bool* solo_changed = nullptr;    // solo toggle
    bool* solo_staged = nullptr;
    bool* duplicate = nullptr;       // stack duplicate
};

struct FrameUi {
    std::vector<ParamStage> params;
    std::vector<FxRowActions> rows;
    bool* add_clicked[static_cast<size_t>(doc::EffectType::Count)] = {};
    bool* open_clicked = nullptr;
    bool* new_look_clicked = nullptr;   // blank start: an empty look
    bool* play_clicked = nullptr;
    bool* undo_clicked = nullptr;
    bool* redo_clicked = nullptr;
    bool* export_clicked = nullptr;
    bool* loop_clicked = nullptr;
    float* seek_staged = nullptr;
    bool* seek_changed = nullptr;
    ui::LayoutNode* preview = nullptr;

    // Rail inline value editors: a click on a slider's value text
    // opens the type-in for that ParamKey.
    struct RailEdit {
        doc::ParamKey key;
        float scale;         // display multiplier (deg rows)
        const char* seed;    // current shown value, pre-formatted
        bool* clicked;
    };
    std::vector<RailEdit> rail_edits;

    // Export settings + cancel.
    bool* export_cancel_clicked = nullptr;
    // Monitor volume.
    bool* mute_clicked = nullptr;
    float* volume_staged = nullptr;
    bool* volume_changed = nullptr;
    bool* volume_released = nullptr;
    // Recent-project rows (project tab).
    struct RecentRow {
        size_t index;
        bool* clicked;
    };
    std::vector<RecentRow> recent_rows;
    // Cache management.
    bool* cache_open_clicked = nullptr;
    bool* cache_clear_clicked = nullptr;
    float* export_bitrate_staged = nullptr;
    bool* export_bitrate_changed = nullptr;
    bool* export_bitrate_released = nullptr;
    int* export_scale_selected = nullptr;
    bool* export_audio_staged = nullptr;
    bool* export_audio_changed = nullptr;

    // Modulation UI staging. Wires (routes) edit in the inspector;
    // value-node params edit on their canvas card.
    struct RouteRow {
        uint64_t id;
        int* curve_selected;    // dropdown pick; -1 = untouched
        bool* remove;
    };
    std::vector<RouteRow> route_rows;

    // One value-node card: kind + context dropdowns plus up to four
    // sliders whose meaning the builder and handler map per kind, in
    // lockstep.
    struct NodeRow {
        uint64_t id;
        float* pick_staged[2] = {};   // 0 kind, 1 shape/trigger/chan/op
        bool* pick_changed[2] = {};
        float* slot_staged[4] = {};
        bool* slot_changed[4] = {};
        bool* slot_released[4] = {};
        float slot_original[4] = {};
        bool* remove = nullptr;
    };
    std::vector<NodeRow> node_rows;

    // The "~" micro next to a param: mints a fresh LFO node wired onto
    // that param in one step.
    struct AddRoute {
        doc::ParamKey key;
        bool* clicked;
    };
    std::vector<AddRoute> add_routes;

    struct KeyToggle {
        doc::ParamKey key;
        float value;    // current base value, keyed at the playhead
        bool* clicked;
        // Both surfaces: the k dot toggles ONE key at the playhead
        // — scrub-and-key everywhere. Lane deletion lives on the lane's X
        // in the timeline; a same-looking control must never wipe an
        // animation.
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
    bool* fx_cat_clicked[static_cast<size_t>(doc::FxCategory::Count)] = {};
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
    // Timeline region edits (trim handles + loop region), from
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
    std::vector<LaneLoop> lane_loops;   // per-lane loop chip
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
    // Fields ≤ Rotate double as the kLayerParamBit param indices (v5.7
    // layer-param modulation) — keep the two in lockstep.
    enum class LayerField : int {
        Opacity, ColorAR, ColorAG, ColorAB, ColorBR, ColorBG, ColorBB,
        Scale, Angle,
        CropL, CropR, CropT, CropB, XfScale, Rotate,
        // The clip node's one timing nuance: a static media in-point.
        Slip,
        OscShape,   // waveform/shape selector (canvas dropdown row)
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
    // Timeline blocks (docs/look.md phase 6): a block IS a placement on a
    // track lane; a drag stages the whole placement, named by its id.
    struct BlockStage {
        uint64_t layer_id;
        uint64_t placement_id;
        doc::Placement* staged;
        bool* changed;
        bool* released;
    };
    std::vector<BlockStage> block_stages;
    struct BlockPick {
        size_t layer_index;   // SIZE_MAX = an audio-lane block
        uint64_t layer_id;    // 0 = an audio-lane block (no canvas card)
        uint64_t placement_id;
        bool* pressed;
    };
    std::vector<BlockPick> block_picks;
    // A plain press on empty lane space is DESELECT: no node, no layer,
    // the monitor returns to the film. Shift presses stay inert so a
    // collect spree survives a missed block.
    bool* tl_deselect = nullptr;
    // The selected clip NODE's media binding: pick an imported asset or
    // browse for new media (imports, then binds).
    struct ClipBind {
        uint64_t layer_id;
        int* selected;      // index into the document's asset list
        bool* browse;
    };
    std::vector<ClipBind> clip_binds;
    // The same binding ON THE CARD: a dropdown row whose entries are
    // "(none)", every asset, then "import...". *staged holds the pick.
    struct ClipRowBind {
        uint64_t layer_id;
        float* staged;
        bool* changed;
        int n_assets;
    };
    std::vector<ClipRowBind> clip_row_binds;
    bool* import_media_clicked = nullptr;
    // BROWSER (docs/look.md phase 6): the project tab lists what the user
    // deliberately made - sequences and assets - never the internal graph
    // every clip carries. Open sets the editing scope; place lays a block
    // at the playhead.
    struct BrowserAction {
        uint64_t id;
        bool* open = nullptr;    // sequences only
        bool* place = nullptr;
    };
    std::vector<BrowserAction> browser_looks;
    std::vector<BrowserAction> browser_assets;
    bool* new_sequence_clicked = nullptr;

    // Audio track label controls: gain slider + mute chip per lane.
    struct AudioTrackStage {
        uint64_t track_id;
        float* gain_staged;
        float original;
        bool* gain_changed;
        bool* gain_released;
        bool* mute_clicked;
    };
    std::vector<AudioTrackStage> audio_tracks;
    // A picker edit stages the whole rgb triplet at once — one coalesced
    // layer command instead of three channel commands.
    struct ColorStage {
        uint64_t layer_id;
        bool color_b;
        float* staged;   // [3]
        float original[3];
        bool* changed;
        bool* released;
    };
    std::vector<ColorStage> color_stages;
    struct LayerRow {
        size_t index;
        uint64_t id;
        bool* select;
        bool* visible_changed;
        bool* visible_staged;
        int* blend_selected;    // dropdown pick; -1 = untouched
        int* osc_shape_selected = nullptr;   // oscillator waveform pick
        bool* remove;
        bool* up = nullptr;     // swap toward index 0 (bottom of composite)
        bool* down = nullptr;
        bool* xf_toggle = nullptr;   // fold/unfold the transform section
        bool* flip_h = nullptr;      // toggle clicks (transform)
        bool* flip_v = nullptr;
        bool* audio_mute = nullptr;  // silence this source in the mix
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
    // Group face: expose/hide one member param (texed expose).
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
    // Sidechain + audio nudge.
    int* sc_selected = nullptr;          // [clip, <file>, pick]; -1 untouched
    bool* sc_mux_changed = nullptr;
    bool* sc_mux_staged = nullptr;
    float* nudge_staged = nullptr;
    bool* nudge_changed = nullptr;
    bool* nudge_released = nullptr;
    bool* proxy_toggle_changed = nullptr;   // half-res proxy
    bool* proxy_toggle_staged = nullptr;
    bool* lossless_changed = nullptr;       // lossless import pref
    bool* lossless_staged = nullptr;
    bool* preset_search_clicked = nullptr;  // searchable browser
    bool* preset_import_clicked = nullptr;  // single-file import
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

    bool* add_layer_open = nullptr;   // switch the rail to the layer picker
    bool* fx_search_clicked = nullptr;
    bool* add_frame_clicked = nullptr;
    bool* open_add_clicked = nullptr;   // None-selection "+ add node..."
    // Rail selector dropdown: a picked option index lands as the
    // param's value through set_param_command.
    struct ParamPick {
        size_t layer_index;
        size_t fx_index;
        int param_index;
        float min_value;
        int* selected;   // -1 = untouched this frame
    };
    std::vector<ParamPick> param_picks;
    // Rail text field: clicking opens the shared inline editor.
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

// ASCII ramp atlas for the glyph renderer: 96 tiles of 8x8,
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
    const ui::UiTexture* thumbs = nullptr;   // filmstrip
    uint32_t thumb_count = 0;
    double v0 = 0.0, v1 = 0.0;   // visible frame range (zoom)
};

// Timeline strips union their rects into tl_rect_accum every draw; the
// frame loop swaps it in and pre-routes the wheel against LAST frame's
// region (zoom) — run_frame would otherwise hand the wheel to the
// lane scroll area before any strip could see it.
void tl_extend_rect(AppState& app, const ui::Rect& r) {
    ui::Rect& a = app.tl_rect_accum;
    if (a.w <= 0.0f) {
        a = r;
        return;
    }
    const float x0 = std::min(a.x, r.x);
    const float y0 = std::min(a.y, r.y);
    const float x1 = std::max(a.right(), r.right());
    const float y1 = std::max(a.bottom(), r.bottom());
    a = {x0, y0, x1 - x0, y1 - y0};
}

// Screen x -> timeline frame, through the strip column the ruler and the
// block lanes share. Outside the strip it clamps to its ends.
uint32_t timeline_frame_at(const AppState& app, float x) {
    const float sw = std::max(1.0f, app.tl_strip_w);
    const double t = std::clamp((x - app.tl_strip_x) / sw, 0.0f, 1.0f);
    double v0 = app.tl_v0, v1 = app.tl_v1;
    if (v1 - v0 < 1.0) {
        v0 = 0.0;
        v1 = std::max(1u, app.player.frame_count());
    }
    const double f = v0 + t * (v1 - v0);
    return f <= 0.0 ? 0u : static_cast<uint32_t>(f);
}

// Wheel over the timeline region: zoom around the cursor; shift+wheel
// pans. Zooming out clamps back to the whole clip. The x mapping uses the
// ruler column (label column excluded via tl_strip_x/w).
void timeline_zoom_wheel(AppState& app, ui::UiInput& input,
                         uint32_t frame_count) {
    if (input.wheel_y == 0.0f || frame_count == 0) return;
    const ui::Rect& r = app.tl_rect;
    if (r.w <= 0.0f || input.mouse.x < r.x || input.mouse.x >= r.right() ||
        input.mouse.y < r.y || input.mouse.y >= r.bottom())
        return;
    double v0 = app.tl_v0, v1 = app.tl_v1;
    if (v1 - v0 < 1.0 || v1 > frame_count) {
        v0 = 0.0;
        v1 = frame_count;
    }
    const double span = v1 - v0;
    if (input.mods & platform::kModShift) {
        const double step = span * 0.1 * -input.wheel_y;
        v0 = std::clamp(v0 + step, 0.0,
                        static_cast<double>(frame_count) - span);
        v1 = v0 + span;
    } else {
        const float sx = app.tl_strip_x, sw = std::max(1.0f, app.tl_strip_w);
        const double t =
            std::clamp((input.mouse.x - sx) / sw, 0.0f, 1.0f);
        const double at = v0 + t * span;
        double ns = std::clamp(span * std::pow(1.25, -input.wheel_y), 4.0,
                               static_cast<double>(frame_count));
        v0 = std::clamp(at - (at - v0) * ns / span, 0.0,
                        static_cast<double>(frame_count) - ns);
        v1 = v0 + ns;
    }
    app.tl_v0 = v0;
    app.tl_v1 = v1;
    input.wheel_y = 0.0f;
}

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
    tl_extend_rect(*u->app, r);
    u->app->tl_strip_x = r.x;
    u->app->tl_strip_w = r.w;
    const double v0 = u->v0;
    const double vspan = std::max(1.0, u->v1 - u->v0);
    auto frame_x = [&](double f) {
        return r.x + static_cast<float>((f - v0) / vspan) * r.w;
    };

    // Filmstrip (thumbnail strip) under everything else — the
    // visible view range maps to the matching slice of the strip.
    if (u->thumbs && u->thumb_count > 0) {
        const float u0 =
            static_cast<float>(v0 / std::max(1u, u->frame_count));
        const float u1 = static_cast<float>(
            std::min<double>(u->v1, u->frame_count) /
            std::max(1u, u->frame_count));
        frame.canvas.draw_image_quad(r, u->thumbs, u0, 0.0f, u1, 1.0f,
                                     ui::Color{1.0f, 1.0f, 1.0f, 0.85f},
                                     2.0f);
    }

    frame.canvas.push_clip(r);
    // Trimmed-out zones read as inert.
    ui::Color dim = theme.window_bg;
    dim.a = 0.55f;
    if (u->trim_in > 0 && frame_x(u->trim_in) > r.x)
        frame.canvas.draw_sdf_rect(
            {r.x, r.y, frame_x(u->trim_in) - r.x, r.h}, 2.0f, dim);
    if (u->trim_out < u->frame_count && frame_x(u->trim_out) < r.right())
        frame.canvas.draw_sdf_rect(
            {frame_x(u->trim_out), r.y,
             r.right() - frame_x(u->trim_out), r.h},
            2.0f, dim);

    // Second ticks + time labels: ticks every second, a "12s"
    // label whenever the second spacing leaves ≥ 48 px between labels.
    if (u->fps > 0.0) {
        const float px_per_sec =
            static_cast<float>(u->fps / vspan) * r.w;
        // Ticks thin with density: under ~5 px apart the per-second lines
        // merge into noise and the loop scales with clip length instead of
        // strip width. Labels stay on tick multiples so they still land.
        const int tick_every =
            px_per_sec >= 5.0f
                ? 1
                : static_cast<int>(std::ceil(5.0f / px_per_sec));
        int label_every =
            px_per_sec >= 48.0f
                ? 1
                : static_cast<int>(std::ceil(48.0f / px_per_sec));
        label_every =
            (label_every + tick_every - 1) / tick_every * tick_every;
        const int s0 = std::max(0, static_cast<int>(v0 / u->fps));
        const int s1 =
            static_cast<int>((v0 + vspan) / u->fps) + 1;
        char tick_buf[16];
        for (int s = s0 - s0 % tick_every; s <= s1; s += tick_every) {
            const double f = s * u->fps;
            if (f > u->frame_count) break;
            const float x = frame_x(f);
            frame.canvas.draw_line({x, r.y + r.h * 0.5f}, {x, r.bottom()},
                                   1.0f, theme.hairline);
            if (s % label_every == 0 && x + 30.0f < r.right()) {
                std::snprintf(tick_buf, sizeof(tick_buf), "%ds", s);
                ui::draw_text(frame.canvas, frame.font, tick_buf,
                              {x + 3.0f, r.y + r.h * 0.5f - 5.0f}, 9.0f,
                              theme.text_disabled);
            }
        }
    }

    // Markers: M toggles one at the playhead; diamonds on the top
    // edge, [ ] snap the playhead across keys AND markers. Sequence
    // structure - a scoped look's local ruler has none.
    if (!u->app->scope_is_look())
        for (const uint32_t m : u->app->sequence().markers) {
            const float x = frame_x(m + 0.5);
            if (x < r.x - 4.0f || x > r.right() + 4.0f) continue;
            const float my = r.y + 5.0f;
            frame.canvas.draw_sdf_rect({x - 3.0f, my - 3.0f, 6.0f, 6.0f},
                                       3.0f, theme.accent);
        }

    // Loop region band along the top edge.
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
    frame.canvas.pop_clip();

    // Interaction: trim handles > loop band (top strip) > scrub.
    RulerState& state = u->app->ruler;
    const ui::WidgetId id = frame.ctx.acquire_widget_id(&state);
    auto mouse_frame = [&] {
        const float t =
            std::clamp((frame.input.mouse.x - r.x) / r.w, 0.0f, 1.0f);
        return std::clamp(v0 + static_cast<double>(t) * vspan, 0.0,
                          static_cast<double>(u->frame_count));
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

// Audio strip: loudness silhouette from the import analysis (the
// same per-frame band curves the mod sources read), onset ticks on the top
// edge, scene cuts as full-height lines — keyframing gets the material's
// rhythm in view without touching PCM.
struct AudioStripUser {
    AppState* app;
    const mod::AnalysisCurves* curves;
    uint32_t frame_count;
    uint32_t playhead;
    double v0, v1;
};

// 64 frames per acceleration block: coarse enough that a whole-clip span
// costs span/64 block reads, fine enough that partial edges stay cheap.
constexpr uint32_t kStripBlock = 64;

void draw_audio_strip(ui::LayoutNode& node, ui::LayoutFrame& frame) {
    auto* u = static_cast<AudioStripUser*>(node.user);
    const ui::Rect& r = node.rect;
    const ui::Theme& theme = frame.theme;
    frame.canvas.draw_sdf_rect(r, 2.0f, theme.control_bg_active);
    if (u->frame_count == 0) return;
    tl_extend_rect(*u->app, r);
    const double v0 = u->v0;
    const double vspan = std::max(1.0, u->v1 - u->v0);
    const auto& c = *u->curves;
    const AppState::StripAccel& accel = u->app->strip_accel;
    // Max over [f0, f1): per-frame at the partial edges, per-block inside.
    // Frames past the curve clamp to its last value (same as sample()).
    auto span_max = [](const float* fr, size_t frn,
                       const std::vector<float>& blocks, uint32_t f0,
                       uint32_t f1) -> float {
        if (!frn) return 0.0f;
        float m = 0.0f;
        if (f1 > frn) {
            m = fr[frn - 1];
            f1 = static_cast<uint32_t>(frn);
        }
        if (f0 >= f1) return m;
        const uint32_t first_full =
            (f0 + kStripBlock - 1) / kStripBlock * kStripBlock;
        const uint32_t last_full = f1 / kStripBlock * kStripBlock;
        if (first_full >= last_full) {
            for (uint32_t f = f0; f < f1; ++f) m = std::max(m, fr[f]);
            return m;
        }
        for (uint32_t f = f0; f < first_full; ++f) m = std::max(m, fr[f]);
        for (uint32_t b = first_full / kStripBlock;
             b < last_full / kStripBlock; ++b)
            m = std::max(m, blocks[b]);
        for (uint32_t f = last_full; f < f1; ++f) m = std::max(m, fr[f]);
        return m;
    };
    frame.canvas.push_clip(r);
    const float cy = r.y + r.h * 0.5f;
    const int cols = std::max(1, static_cast<int>(r.w));
    for (int i = 0; i < cols; ++i) {
        // Per-column MAX over the covered frames — averaging (or point
        // sampling) would swallow one-frame onsets when zoomed out.
        const double fa = v0 + static_cast<double>(i) / cols * vspan;
        const double fb = v0 + static_cast<double>(i + 1) / cols * vspan;
        const uint32_t f0 = static_cast<uint32_t>(std::max(0.0, fa));
        const uint32_t f1 = std::min(
            u->frame_count,
            std::max(f0 + 1, static_cast<uint32_t>(std::max(0.0, fb))));
        const float amp = span_max(accel.amp_frame.data(),
                                   accel.amp_frame.size(), accel.amp, f0, f1);
        const float onset =
            span_max(c.onset.data(), c.onset.size(), accel.onset, f0, f1);
        const float cut =
            span_max(c.cut.data(), c.cut.size(), accel.cut, f0, f1);
        const float x = r.x + static_cast<float>(i) + 0.5f;
        if (cut > 0.5f)
            frame.canvas.draw_line({x, r.y}, {x, r.bottom()}, 1.0f,
                                   theme.text_dim.with_alpha(0.8f));
        const float h =
            std::max(1.0f, amp * (r.h * 0.5f - 1.0f));
        frame.canvas.draw_line({x, cy - h}, {x, cy + h}, 1.0f,
                               theme.accent_dim.with_alpha(0.6f));
        if (onset > 0.5f)
            frame.canvas.draw_line({x, r.y}, {x, r.y + 4.0f}, 1.0f,
                                   theme.accent);
    }
    const float px =
        r.x + static_cast<float>((u->playhead + 0.5 - v0) / vspan) * r.w;
    frame.canvas.draw_line({px, r.y}, {px, r.bottom()}, 1.0f,
                           theme.accent.with_alpha(0.5f));
    frame.canvas.pop_clip();
}

// ---- timeline block lanes (docs/look.md phase 5)
//
// The scoped look's placements as BLOCKS: each layer is one block on its
// look's local timeline; a lane is a display row packing non-overlapping
// blocks (a razored pair abuts on one lane). Tracks carry no semantics —
// composite order is layer order, the lane is where the block happens to
// sit.

// One audio contributor inside a block: samples the per-asset amp array
// through the block's (possibly nested) time map. `outer` is the block
// layer's placement in scoped-local time; `inner` is a flattened clip
// placement in the referenced look's local time (nested blocks only).
struct TlAmpSrc {
    const std::vector<float>* amp = nullptr;
    doc::Placement outer;
    doc::ClipInstance inner;
    bool nested = false;
    float gain = 1.0f;
};

inline float tl_amp_sample(const TlAmpSrc& s, double local) {
    double af;
    if (s.nested) {
        const double rl = doc::placement_source_frame(s.outer, local);
        if (!doc::clip_active(s.inner, rl)) return 0.0f;
        af = doc::clip_source_frame(s.inner, rl);
    } else {
        af = doc::placement_source_frame(s.outer, local);
    }
    if (af < 0.0 || s.amp->empty()) return 0.0f;
    const size_t idx = std::min(static_cast<size_t>(af), s.amp->size() - 1);
    return (*s.amp)[idx] * s.gain;
}

struct TlBlock {
    size_t layer_index = 0;
    uint64_t layer_id = 0;
    uint64_t placement_id = 0;
    double t0 = 0.0, t1 = 0.0;    // local frame span (t1 resolved)
    const char* name = "";
    int kind = 0;                 // 0 clip, 2 look
    bool selected = false;
    doc::Placement place;
    uint32_t src_len = 0;         // 0 = unbounded
    TlAmpSrc* srcs = nullptr;     // arena; empty = the block is silent
    size_t src_count = 0;
    // Filmstrip: the asset's thumbnail strip mapped through source frames.
    const ui::UiTexture* thumbs = nullptr;
    uint32_t asset_frames = 0;
    doc::Placement* staged = nullptr;   // block drags write through these
    bool* changed = nullptr;
    bool* released = nullptr;
    bool* pressed = nullptr;
};

struct BlockLaneUser {
    AppState* app;
    FrameUi* out;
    TlBlock* blocks;
    size_t count = 0;
    size_t lane_index = 0;
    size_t layer_index = SIZE_MAX;   // doc layer; SIZE_MAX = audio lane
    uint32_t frame_count = 0;
    uint32_t play_end = 0;   // content ends here; past it is buffer
    uint32_t playhead = 0;
    double v0 = 0.0, v1 = 0.0;
};

void hit_block_lane(ui::LayoutNode& node, ui::LayoutFrame& frame) {
    auto* u = static_cast<BlockLaneUser*>(node.user);
    ui::Rect r = node.rect;
    if (!node.clip.empty()) r = r.intersect(node.clip);
    frame.ctx.add_hit(r, frame.ctx.acquire_widget_id(
                             &u->app->tl_lane_ids[u->lane_index]));
}

void draw_block_lane(ui::LayoutNode& node, ui::LayoutFrame& frame) {
    auto* u = static_cast<BlockLaneUser*>(node.user);
    AppState& app = *u->app;
    const ui::Rect& r = node.rect;
    const ui::Theme& theme = frame.theme;
    frame.canvas.draw_sdf_rect(r, 2.0f, theme.control_bg);
    // The picked layer's lane wears an accent bar on its left edge -
    // the LANE-level highlight; the picked block outlines itself.
    if (u->layer_index != SIZE_MAX && app.layer_sel &&
        u->layer_index == app.selected_layer)
        frame.canvas.draw_sdf_rect({r.x, r.y, 3.0f, r.h}, 1.5f,
                                   theme.accent);
    tl_extend_rect(app, r);
    const double v0 = u->v0;
    const double vspan = std::max(1.0, u->v1 - u->v0);
    auto frame_x = [&](double f) {
        return r.x + static_cast<float>((f - v0) / vspan) * r.w;
    };
    auto mouse_frame = [&] {
        const float t =
            std::clamp((frame.input.mouse.x - r.x) / r.w, 0.0f, 1.0f);
        return std::max(0.0, v0 + static_cast<double>(t) * vspan);
    };

    frame.canvas.push_clip(r);
    for (size_t i = 0; i < u->count; ++i) {
        const TlBlock& b = u->blocks[i];
        const float x0 = frame_x(b.t0);
        const float x1 = std::max(frame_x(b.t1), x0 + 3.0f);
        if (x1 < r.x || x0 > r.right()) continue;
        const ui::Rect br{x0, r.y + 1.0f, x1 - x0, r.h - 2.0f};
        frame.canvas.draw_sdf_rect(br, 3.0f, theme.control_bg_active);
        // Filmstrip: the visible span maps to the SOURCE frames it plays.
        if (b.thumbs && b.asset_frames > 0) {
            const double s0 = doc::placement_source_frame(b.place, b.t0);
            const double s1 = doc::placement_source_frame(b.place, b.t1);
            const float tu0 = static_cast<float>(
                std::clamp(s0 / b.asset_frames, 0.0, 1.0));
            const float tu1 = static_cast<float>(
                std::clamp(s1 / b.asset_frames, 0.0, 1.0));
            frame.canvas.draw_image_quad(
                br, b.thumbs, tu0, 0.0f, tu1, 1.0f,
                ui::Color{1.0f, 1.0f, 1.0f, 0.55f}, 3.0f);
        }
        // AUDIO: silhouette along the bottom third — drawn ONLY when the
        // block's source tree actually holds PCM. A look built from
        // shapes and gradients shows nothing here, which is the whole
        // point: a silent block must not read as having audio. The band
        // gets its own dark backing so it stays legible over a filmstrip.
        if (b.src_count > 0) {
            const float ah = std::max(6.0f, br.h * 0.34f);
            const ui::Rect band{br.x, br.bottom() - ah, br.w, ah};
            frame.canvas.draw_sdf_rect(band, 2.0f,
                                       theme.window_bg.with_alpha(0.72f));
            const float ab = band.bottom() - 1.0f;
            const float span = ah - 2.0f;
            const float cx0 = std::max(br.x, r.x);
            const float cx1 = std::min(br.right(), r.right());
            for (float x = cx0; x < cx1; x += 1.0f) {
                const double f = v0 + (x - r.x) / r.w * vspan;
                float amp = 0.0f;
                for (size_t s = 0; s < b.src_count; ++s)
                    amp = std::max(amp, tl_amp_sample(b.srcs[s], f));
                const float h = std::min(amp, 1.0f) * span;
                if (h > 0.4f)
                    frame.canvas.draw_line({x + 0.5f, ab - h},
                                           {x + 0.5f, ab}, 1.0f,
                                           theme.accent.with_alpha(0.85f));
            }
        }
        // Source-kind cap + name + selection edge.
        const ui::Color cap = b.kind == 2
            ? theme.accent
            : (b.kind == 0 ? theme.accent_dim : theme.text_disabled);
        frame.canvas.draw_sdf_rect({br.x, br.y, 3.0f, br.h}, 1.5f, cap);
        if (b.selected)
            frame.canvas.draw_sdf_rect_outline(br, 3.0f, 1.5f, theme.accent);
        if (br.w > 24.0f) {
            frame.canvas.push_clip(br);
            ui::draw_text(frame.canvas, frame.font, b.name,
                          {br.x + 7.0f, br.y + 3.0f},
                          theme.font_size_small, theme.text);
            frame.canvas.pop_clip();
        }
    }
    // The BUFFER past the content reads inert, the same way the ruler
    // shades what the trim excludes: it is somewhere to drag to, not part
    // of the film.
    if (u->play_end < u->frame_count) {
        const float bx = frame_x(u->play_end);
        if (bx < r.right()) {
            ui::Color dim = theme.window_bg;
            dim.a = 0.5f;
            frame.canvas.draw_sdf_rect(
                {std::max(bx, r.x), r.y, r.right() - std::max(bx, r.x), r.h},
                2.0f, dim);
        }
    }
    // Playhead over the lane.
    const float px = frame_x(u->playhead + 0.5);
    frame.canvas.draw_line({px, r.y}, {px, r.bottom()}, 1.0f,
                           theme.accent.with_alpha(0.5f));
    // Engaged snap target: one accent line straight down the lanes,
    // the NLE "magnet" flash.
    if (app.blk_drag_mode != 0 && app.tl_snap_frame >= 0.0) {
        const float sx = frame_x(app.tl_snap_frame);
        frame.canvas.draw_line({sx, r.y}, {sx, r.bottom()}, 1.0f,
                               theme.accent);
    }
    frame.canvas.pop_clip();

    // Interaction: edges trim, body slides, press selects. One drag at a
    // time app-wide; edits stage a whole placement and coalesce.
    const ui::WidgetId id =
        frame.ctx.acquire_widget_id(&app.tl_lane_ids[u->lane_index]);
    // A drag whose block vanished under it (razor, undo, scope change)
    // would otherwise hold the capture forever: the button is up, so the
    // drag is over whether or not anyone is left to end it.
    if (app.blk_drag_mode != 0 && !frame.input.left_down()) {
        app.blk_drag_mode = 0;
        app.blk_drag_layer = 0;
        app.blk_drag_placement = 0;
        app.tl_snap_frame = -1.0;
        frame.ctx.clear_capture();
    }
    if (frame.input.left_pressed() && frame.ctx.widget_owns_mouse(id)) {
        const float mx = frame.input.mouse.x;
        bool on_block = false;
        for (size_t i = 0; i < u->count; ++i) {
            const TlBlock& b = u->blocks[i];
            const float x0 = frame_x(b.t0);
            const float x1 = std::max(frame_x(b.t1), x0 + 3.0f);
            if (mx < x0 - 4.0f || mx > x1 + 4.0f) continue;
            on_block = true;
            *b.pressed = true;
            app.blk_drag_layer = b.layer_id;
            app.blk_drag_placement = b.placement_id;
            app.blk_drag_anchor = mouse_frame();
            app.blk_drag_orig = b.place;
            if (std::abs(mx - x0) <= 4.0f) app.blk_drag_mode = 2;
            else if (std::abs(mx - x1) <= 4.0f) app.blk_drag_mode = 3;
            else app.blk_drag_mode = 1;
            frame.ctx.set_capture(id);
            break;
        }
        // Empty lane space: a plain press is DESELECT (every NLE's
        // convention) - staged for the post-frame handler like any
        // other timeline action. Shift presses stay inert so a missed
        // block cannot end a collect spree.
        if (!on_block && u->out->tl_deselect &&
            !(frame.input.mods & platform::kModShift))
            *u->out->tl_deselect = true;
    }
    if (app.blk_drag_mode != 0) {
        for (size_t i = 0; i < u->count; ++i) {
            const TlBlock& b = u->blocks[i];
            if (b.placement_id != app.blk_drag_placement) continue;
            // AUTO-SCROLL: pushing the cursor past the strip pans the
            // view under it, so ONE gesture extends a block as far as its
            // media goes. Without this the drag stops at whatever was on
            // screen when it started, and the buffer that opens up beyond
            // is unreachable until you zoom out and drag again. Only the
            // lane holding the dragged block scrolls - every lane runs
            // this loop, and they share one view.
            const float past_right = frame.input.mouse.x - r.right();
            const float past_left = r.x - frame.input.mouse.x;
            if (past_right > 0.0f || past_left > 0.0f) {
                const float over = std::max(past_right, past_left);
                // The further out, the faster - capped so it stays
                // steerable, and in FRAMES PER SECOND so the rate does
                // not depend on how fast this machine renders.
                const double per_second =
                    vspan * std::clamp(over / 120.0f, 0.15f, 1.5f);
                const double step = per_second * app.frame_dt;
                if (past_right > 0.0f) {
                    // Never past the timeline's own end: the content (and
                    // with it the buffer) grows as the block extends, so
                    // this ceiling rises with the drag instead of
                    // stopping it.
                    const double limit = static_cast<double>(u->frame_count);
                    const double nv1 = std::min(u->v1 + step, limit);
                    app.tl_v1 = nv1;
                    app.tl_v0 = std::max(0.0, nv1 - vspan);
                } else {
                    const double nv0 = std::max(0.0, u->v0 - step);
                    app.tl_v0 = nv0;
                    app.tl_v1 = nv0 + vspan;
                }
            }
            const doc::Placement& o = app.blk_drag_orig;
            const double delta = mouse_frame() - app.blk_drag_anchor;
            const int64_t d =
                static_cast<int64_t>(std::llround(delta));
            // SNAP (the NLE magnet): the dragged edge lands on a nearby
            // cut, mark or the playhead when inside ~8px of it. The
            // dragged placement's own edges and its link partners'
            // (they move with it) never count as targets.
            app.tl_snap_frame = -1.0;
            const double thr = 8.0 * vspan / std::max(1.0f, r.w);
            auto snap_shift = [&](double e0, double e1, bool both) {
                double best = 0.0, bd = thr;
                if (!app.tl_snap) return best;
                for (const AppState::TlSnapEdge& s : app.tl_snap_edges) {
                    if (s.pid && s.pid == app.blk_drag_placement) continue;
                    if (s.link && s.link == o.link) continue;
                    double sd = s.frame - e0;
                    if (std::abs(sd) <= bd) {
                        bd = std::abs(sd);
                        best = sd;
                        app.tl_snap_frame = s.frame;
                    }
                    if (both) {
                        sd = s.frame - e1;
                        if (std::abs(sd) < bd) {
                            bd = std::abs(sd);
                            best = sd;
                            app.tl_snap_frame = s.frame;
                        }
                    }
                }
                return best;
            };
            doc::Placement p = o;
            switch (app.blk_drag_mode) {
                case 1: {
                    // Slide: the block moves, its content rides along.
                    // Either end may snap; the nearer target wins.
                    int64_t nt =
                        std::max<int64_t>(0, static_cast<int64_t>(o.t_in) + d);
                    const uint32_t oend = doc::placement_end(o, b.src_len);
                    const int64_t len =
                        oend ? static_cast<int64_t>(oend) -
                                   static_cast<int64_t>(o.t_in)
                             : 0;
                    nt = std::max<int64_t>(
                        0, nt + static_cast<int64_t>(std::llround(
                               snap_shift(static_cast<double>(nt),
                                          static_cast<double>(nt + len),
                                          len > 0))));
                    const int64_t shift = nt - static_cast<int64_t>(o.t_in);
                    p.t_in = static_cast<uint32_t>(nt);
                    if (o.t_out)
                        p.t_out = static_cast<uint32_t>(
                            static_cast<int64_t>(o.t_out) + shift);
                    break;
                }
                case 2: {
                    // Trim in: the start moves, the CONTENT stays put —
                    // source_in compensates through the speed.
                    const uint32_t end = doc::placement_end(o, b.src_len);
                    int64_t nt =
                        std::max<int64_t>(0, static_cast<int64_t>(o.t_in) + d);
                    nt = std::max<int64_t>(
                        0, nt + static_cast<int64_t>(std::llround(
                               snap_shift(static_cast<double>(nt), 0.0,
                                          false))));
                    if (end)
                        nt = std::min<int64_t>(nt,
                                               static_cast<int64_t>(end) - 1);
                    const double src = doc::placement_source_frame(
                        o, static_cast<double>(nt));
                    if (src < 0.0 && o.speed > 0.0f)
                        nt = static_cast<int64_t>(
                            o.t_in -
                            static_cast<double>(o.source_in) / o.speed);
                    p.t_in = static_cast<uint32_t>(std::max<int64_t>(0, nt));
                    const double nsrc = doc::placement_source_frame(
                        o, static_cast<double>(p.t_in));
                    p.source_in = nsrc <= 0.0
                        ? 0u
                        : static_cast<uint32_t>(nsrc);
                    if (o.t_out == 0 && b.src_len)
                        p.t_out = end;   // keep the end where it was
                    break;
                }
                case 3: {
                    // Trim out; dragging to (or past) the source's own end
                    // stores 0 = "runs to the end".
                    int64_t ne0 =
                        static_cast<int64_t>(std::llround(mouse_frame()));
                    ne0 += static_cast<int64_t>(std::llround(
                        snap_shift(static_cast<double>(ne0), 0.0, false)));
                    const int64_t ne = std::max<int64_t>(
                        static_cast<int64_t>(o.t_in) + 1, ne0);
                    p.t_out = static_cast<uint32_t>(ne);
                    if (b.src_len) {
                        doc::Placement natural = o;
                        natural.t_out = 0;
                        if (p.t_out >=
                            doc::placement_end(natural, b.src_len))
                            p.t_out = 0;
                    }
                    break;
                }
            }
            *b.staged = p;
            *b.changed = true;
            if (frame.input.left_released()) {
                *b.released = true;
                app.blk_drag_mode = 0;
                app.blk_drag_layer = 0;
                app.blk_drag_placement = 0;
                app.tl_snap_frame = -1.0;
                frame.ctx.clear_capture();
            }
            break;
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
    double v0 = 0.0, v1 = 0.0;   // shared visible range
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
    tl_extend_rect(*u->app, r);
    const double v0 = u->v0;
    const double vspan = std::max(1.0, u->v1 - u->v0);

    auto to_x = [&](double f) {
        return r.x + static_cast<float>((f - v0) / vspan) * r.w;
    };
    auto to_y = [&](float v) {
        return r.bottom() - (v - u->min_value) / span * r.h;
    };
    auto from_x = [&](float x) {
        return std::clamp(
            v0 + static_cast<double>((x - r.x) / r.w) * vspan, 0.0,
            static_cast<double>(frames - 1));
    };
    auto from_y = [&](float y) {
        return std::clamp(u->min_value + (r.bottom() - y) / r.h * span,
                          u->min_value, u->max_value);
    };
    // Selection is identified by key FRAME (survives the sort a lane
    // command applies); resolve the index set for this frame's key list.
    const auto& keys = u->lane->keys;
    auto is_selected = [&](size_t i) {
        for (const double f : state.sel_frames)
            if (keys[i].frame == f) return true;
        return false;
    };

    frame.canvas.draw_sdf_rect(r, 2.0f, theme.control_bg_active);
    frame.canvas.draw_rect_outline(r, 1.0f, theme.hairline);
    // Everything inside the strip clips to it — keys whose values sit
    // outside the shown range must never paint over the panel.
    frame.canvas.push_clip(r);
    // Muted lanes render dimmed (keys kept, param not driven).
    const float lane_alpha = u->lane->muted ? 0.35f : 1.0f;

    if (state.selected >= static_cast<int>(keys.size())) state.selected = -1;

    // Sampled curve (over the visible range only).
    if (!keys.empty()) {
        const int steps = std::max(2, static_cast<int>(r.w / 3.0f));
        Vec2 prev{};
        for (int i = 0; i <= steps; ++i) {
            const double f = v0 + static_cast<double>(i) / steps * vspan;
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

    // Value axis: range labels so a key's height means something.
    {
        char axis_buf[24];
        std::snprintf(axis_buf, sizeof(axis_buf), "%.5g", u->max_value);
        ui::draw_text(frame.canvas, frame.font, axis_buf,
                      {r.x + 3.0f, r.y + 1.0f}, 9.0f, theme.text_disabled);
        std::snprintf(axis_buf, sizeof(axis_buf), "%.5g", u->min_value);
        ui::draw_text(frame.canvas, frame.font, axis_buf,
                      {r.x + 3.0f, r.bottom() - 11.0f}, 9.0f,
                      theme.text_disabled);
    }

    // Playhead.
    const float px = to_x(u->playhead + 0.5);
    frame.canvas.draw_line({px, r.y}, {px, r.bottom()}, 1.0f,
                           theme.accent.with_alpha(0.5f));

    // Keys (+ selected key's bezier handle dots).
    for (size_t i = 0; i < keys.size(); ++i) {
        const Vec2 p{to_x(keys[i].frame), to_y(keys[i].value)};
        const bool selected =
            static_cast<int>(i) == state.selected || is_selected(i);
        frame.canvas.draw_sdf_rect({p.x - 3, p.y - 3, 6, 6}, 1.0f,
                                   (selected ? theme.text : theme.accent)
                                       .with_alpha(lane_alpha));
        if (static_cast<int>(i) == state.selected && !keys[i].hold) {
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

    // Interp chips: lin / ease / hold for the selected set, drawn
    // top-right of the strip; ease sets flat thirds tangents (easy-ease).
    const bool has_sel = state.selected >= 0 || !state.sel_frames.empty();
    ui::Rect chip_rects[3]{};
    static const char* kChipNames[3] = {"lin", "ease", "hold"};
    if (has_sel) {
        float cx = r.right() - 4.0f;
        for (int ci = 2; ci >= 0; --ci) {
            const float cw = ci == 1 ? 30.0f : 24.0f;
            cx -= cw + 2.0f;
            chip_rects[ci] = {cx, r.y + 2.0f, cw, 12.0f};
            frame.canvas.draw_sdf_rect(chip_rects[ci], 2.0f,
                                       theme.control_bg);
            ui::draw_text(frame.canvas, frame.font, kChipNames[ci],
                          {chip_rects[ci].x + 4.0f,
                           chip_rects[ci].y + 1.5f},
                          9.0f, theme.text_dim);
        }
    }

    // Selected-key readout: click the value to type it, shift+click
    // the frame; the open editor shows the buffer with a caret.
    ui::Rect readout_rect{};
    if (state.selected >= 0 && state.selected < static_cast<int>(keys.size())) {
        const doc::Keyframe& sk = keys[static_cast<size_t>(state.selected)];
        const bool editing = u->app->key_edit_mode != 0 &&
                             u->app->key_edit_target == u->target &&
                             u->app->key_edit_frame == sk.frame;
        char ro[64];
        if (editing)
            std::snprintf(ro, sizeof(ro), "%s %s_",
                          u->app->key_edit_mode == 2 ? "f" : "v",
                          u->app->key_edit_buf.c_str());
        else
            std::snprintf(ro, sizeof(ro), "f %.0f  %.4g", sk.frame,
                          sk.value);
        readout_rect = {r.x + 34.0f, r.y + 2.0f,
                        10.0f + 5.4f * static_cast<float>(std::strlen(ro)),
                        12.0f};
        frame.canvas.draw_sdf_rect(readout_rect, 2.0f, theme.control_bg);
        ui::draw_text(frame.canvas, frame.font, ro,
                      {readout_rect.x + 4.0f, readout_rect.y + 1.5f}, 9.0f,
                      editing ? theme.text : theme.text_dim);
    }
    frame.canvas.pop_clip();

    // ---- interaction (queued into FrameUi, applied post-frame).
    const ui::WidgetId id = frame.ctx.acquire_widget_id(&state);
    const bool owns = frame.ctx.widget_owns_mouse(id);
    const Vec2 mouse = frame.input.mouse;
    const bool dragging = state.dragging_key || state.dragging_in ||
                          state.dragging_out || state.group_drag ||
                          state.box_select;

    auto emit = [&](std::vector<doc::Keyframe> new_keys) {
        u->out->lane_edits.push_back({u->target, std::move(new_keys)});
    };
    auto in_rect = [&](const ui::Rect& rc) {
        return rc.w > 0.0f && mouse.x >= rc.x && mouse.x < rc.right() &&
               mouse.y >= rc.y && mouse.y < rc.bottom();
    };
    auto nearest_key = [&]() {
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
        return hit;
    };

    // Right-click deletes the hit key — and its whole selected set when
    // it is part of one.
    if ((frame.input.buttons_pressed & ui::kMouseRight) && owns &&
        !dragging) {
        const int hit = nearest_key();
        if (hit >= 0) {
            std::vector<doc::Keyframe> edited;
            if (is_selected(static_cast<size_t>(hit))) {
                for (size_t i = 0; i < keys.size(); ++i)
                    if (!is_selected(i)) edited.push_back(keys[i]);
            } else {
                edited = keys;
                edited.erase(edited.begin() + hit);
            }
            state.selected = -1;
            state.sel_frames.clear();
            emit(std::move(edited));
            u->out->lane_release = true;
        }
    }

    if (frame.input.left_pressed() && owns && !dragging) {
        bool consumed = false;
        // Interp chips.
        if (has_sel) {
            for (int ci = 0; ci < 3 && !consumed; ++ci) {
                if (!in_rect(chip_rects[ci])) continue;
                std::vector<doc::Keyframe> edited = keys;
                for (size_t i = 0; i < edited.size(); ++i) {
                    if (!(static_cast<int>(i) == state.selected ||
                          is_selected(i)))
                        continue;
                    doc::Keyframe& k = edited[i];
                    if (ci == 0) {
                        k.hold = false;
                        k.in_dx = k.in_dy = k.out_dx = k.out_dy = 0.0f;
                    } else if (ci == 1) {
                        k.hold = false;
                        const double prev_f =
                            i > 0 ? edited[i - 1].frame : k.frame - 8.0;
                        const double next_f = i + 1 < edited.size()
                                                  ? edited[i + 1].frame
                                                  : k.frame + 8.0;
                        k.out_dx =
                            static_cast<float>((next_f - k.frame) / 3.0);
                        k.out_dy = 0.0f;
                        k.in_dx =
                            static_cast<float>(-((k.frame - prev_f) / 3.0));
                        k.in_dy = 0.0f;
                    } else {
                        k.hold = true;
                    }
                }
                emit(std::move(edited));
                u->out->lane_release = true;
                consumed = true;
            }
        }
        // Readout → inline numeric editor.
        if (!consumed && state.selected >= 0 && in_rect(readout_rect)) {
            const doc::Keyframe& sk =
                keys[static_cast<size_t>(state.selected)];
            AppState& a = *u->app;
            a.key_edit_mode =
                (frame.input.mods & platform::kModShift) ? 2 : 1;
            a.key_edit_target = u->target;
            a.key_edit_frame = sk.frame;
            char seed[32];
            if (a.key_edit_mode == 2)
                std::snprintf(seed, sizeof(seed), "%.0f", sk.frame);
            else
                std::snprintf(seed, sizeof(seed), "%g", sk.value);
            a.key_edit_buf = seed;
            consumed = true;
        }
        if (!consumed) {
            const int grabbed = nearest_key();
            if (grabbed >= 0 && (frame.input.mods & platform::kModCtrl)) {
                std::vector<doc::Keyframe> edited = keys;
                edited.erase(edited.begin() + grabbed);
                state.selected = -1;
                state.sel_frames.clear();
                emit(std::move(edited));
                u->out->lane_release = true;
            } else if (grabbed >= 0 &&
                       (frame.input.mods & platform::kModShift)) {
                // Shift+click toggles set membership.
                const double f = keys[static_cast<size_t>(grabbed)].frame;
                auto it = std::find(state.sel_frames.begin(),
                                    state.sel_frames.end(), f);
                if (it != state.sel_frames.end())
                    state.sel_frames.erase(it);
                else
                    state.sel_frames.push_back(f);
                state.selected = grabbed;
            } else if (grabbed >= 0 &&
                       is_selected(static_cast<size_t>(grabbed))) {
                // Dragging inside the selected set moves the whole set.
                state.group_drag = true;
                state.selected = -1;
                state.drag_anchor_frame = from_x(mouse.x);
                state.drag_anchor_value = from_y(mouse.y);
                state.drag_orig = keys;
                state.drag_sel = state.sel_frames;
                frame.ctx.set_capture(id);
            } else if (grabbed >= 0) {
                state.sel_frames.clear();
                state.selected = grabbed;
                state.dragging_key = true;
                frame.ctx.set_capture(id);
            } else {
                // Handle dots of the selected key?
                bool on_handle = false;
                if (state.selected >= 0 &&
                    state.selected < static_cast<int>(keys.size())) {
                    const doc::Keyframe& k =
                        keys[static_cast<size_t>(state.selected)];
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
                    // Near the curve adds a key; empty strip starts a
                    // box-select marquee.
                    const double mf = from_x(mouse.x);
                    const float curve_y =
                        keys.empty()
                            ? 1.0e9f
                            : to_y(std::clamp(mod::eval_lane(*u->lane, mf),
                                              u->min_value, u->max_value));
                    if (keys.empty() ||
                        std::fabs(curve_y - mouse.y) < 7.0f) {
                        // Remember the added frame so the recovery below
                        // grabs THIS key, never a neighbour.
                        doc::Keyframe k;
                        k.frame = std::round(mf);
                        k.value = from_y(mouse.y);
                        std::vector<doc::Keyframe> edited = keys;
                        edited.push_back(k);
                        state.selected = -1;
                        state.sel_frames.clear();
                        state.pending_add_frame = k.frame;
                        state.dragging_key = true;   // drag-through place
                        frame.ctx.set_capture(id);
                        emit(std::move(edited));
                    } else {
                        state.box_select = true;
                        state.box_anchor = mouse;
                        frame.ctx.set_capture(id);
                    }
                }
            }
        }
    }

    // Box-select marquee: draw + finalize on release.
    if (state.box_select) {
        const ui::Rect bx{std::min(state.box_anchor.x, mouse.x),
                          std::min(state.box_anchor.y, mouse.y),
                          std::fabs(mouse.x - state.box_anchor.x),
                          std::fabs(mouse.y - state.box_anchor.y)};
        frame.canvas.draw_rect_outline(bx, 1.0f, theme.accent_dim);
        if (frame.input.left_released()) {
            if (!(frame.input.mods & platform::kModShift))
                state.sel_frames.clear();
            for (size_t i = 0; i < keys.size(); ++i) {
                const Vec2 p{to_x(keys[i].frame), to_y(keys[i].value)};
                if (p.x >= bx.x && p.x <= bx.right() && p.y >= bx.y &&
                    p.y <= bx.bottom()) {
                    if (!is_selected(i))
                        state.sel_frames.push_back(keys[i].frame);
                    state.selected = static_cast<int>(i);
                }
            }
            state.box_select = false;
            frame.ctx.clear_capture();
        }
    }

    // Group drag: transform the press-time snapshot by the mouse delta.
    if (state.group_drag && frame.input.left_down() &&
        !state.drag_orig.empty()) {
        const double df =
            std::round(from_x(mouse.x) - state.drag_anchor_frame);
        const float dv = from_y(mouse.y) - state.drag_anchor_value;
        std::vector<doc::Keyframe> edited = state.drag_orig;
        std::vector<double> new_sel;
        for (doc::Keyframe& k : edited) {
            bool sel = false;
            for (const double f : state.drag_sel)
                if (k.frame == f) {
                    sel = true;
                    break;
                }
            if (!sel) continue;
            k.frame = std::clamp(k.frame + df, 0.0,
                                 static_cast<double>(frames - 1));
            k.value =
                std::clamp(k.value + dv, u->min_value, u->max_value);
            new_sel.push_back(k.frame);
        }
        state.sel_frames = std::move(new_sel);
        emit(std::move(edited));
    }

    if ((state.dragging_key || state.dragging_in || state.dragging_out) &&
        frame.input.left_down() && state.selected >= 0 &&
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

    if ((state.dragging_key || state.dragging_in || state.dragging_out ||
         state.group_drag) &&
        frame.input.left_released()) {
        state.dragging_key = state.dragging_in = state.dragging_out = false;
        state.group_drag = false;
        state.drag_orig.clear();
        state.drag_sel.clear();
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
                          const char* expose_tip = nullptr,
                          bool keyed = false, bool routed = false) {
    using namespace ui;
    LabelOpts small_dim;
    // Driven params tint like the canvas rows: keyed = accent,
    // routed = dim accent — the inspector shows animation state in place.
    small_dim.color = keyed ? active_theme().accent
                     : routed ? active_theme().accent_dim
                              : active_theme().text_dim;
    small_dim.size = active_theme().font_size_small;
    // The mod gutter is ALWAYS three 18 px slots (wave, key, knob) — absent
    // controls leave blank slots so the label and value columns never shift
    // between grouped/ungrouped/unmodulatable rows.
    std::vector<LayoutNode*> cells;
    if (route_clicked) {
        ButtonOpts micro;
        micro.width = SizeSpec::fixed(18);
        micro.tooltip = routed ? "add modulation route (routed)"
                               : "add modulation route";
        micro.active = routed;
        cells.push_back(
            IconButton(arena, Icon::Wave, route_state, route_clicked, micro));
        micro.tooltip = keyed ? "toggle keyframe at playhead (keyed)"
                              : "toggle keyframe at playhead";
        micro.active = keyed;
        cells.push_back(
            IconButton(arena, Icon::Key, key_state, key_clicked, micro));
        micro.active = false;
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

// Category members sorted by display label: both add menus list
// alphabetically, so newly appended effects sort into place instead of
// sinking to the bottom of their fold (enum order stays frozen for the
// shader/info tables, never for the user).
std::vector<doc::EffectType> category_effects_sorted(doc::FxCategory cat) {
    std::vector<doc::EffectType> sorted;
    for (size_t t = 0; t < static_cast<size_t>(doc::EffectType::Count); ++t)
        if (doc::effect_info(static_cast<doc::EffectType>(t)).category ==
            cat)
            sorted.push_back(static_cast<doc::EffectType>(t));
    std::sort(sorted.begin(), sorted.end(),
              [](doc::EffectType a, doc::EffectType b) {
                  return std::strcmp(doc::effect_info(a).label,
                                     doc::effect_info(b).label) < 0;
              });
    return sorted;
}

// Label/value row on the same grid: [blank gutter][label][value control].
// Enum-ish values (blend, time mode) all share this shape.
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
    const doc::EffectInstance& fx = app.look().layers[app.selected_layer].stack[fx_index];
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
    // toggles it on the group FACE (exposed params).
    const doc::Group* fx_group = nullptr;
    for (const doc::Group& g :
         app.look().layers[app.selected_layer].groups)
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
        fx_index + 1 == app.look().layers[app.selected_layer].stack.size();
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

    uint32_t ordinal = 0;
    // Driven-state lookups: tint the row + light the dots exactly
    // like the canvas cards.
    auto rail_keyed = [&](const doc::ParamKey& k) {
        for (const doc::KeyframeLane& l : app.look().lanes)
            if (l.target == k && !l.keys.empty()) return true;
        return false;
    };
    auto rail_routed = [&](const doc::ParamKey& k) {
        for (const doc::ModRoute& r : app.look().mod_routes)
            if (r.target == k) return true;
        return false;
    };

    auto stage_slider = [&](int param_index, const char* label, float min_v,
                            float max_v, float value, const char* format,
                            SliderState* slider_state,
                            const char* options = nullptr,
                            const char* tip = nullptr) {
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
        opts.tooltip = tip;

        // One grid row: [~ k (m)] mod gutter, label, slider (param_row).
        const doc::ParamKey key{fx.id, param_index};
        FrameUi::AddRoute add_route{key, arena.alloc<bool>()};
        FrameUi::KeyToggle key_toggle{key, value, arena.alloc<bool>()};

        // Rail type-in: commit lands through this row's staged
        // path; while open, the slider is replaced by the edit field.
        bool editing = app.rail_edit_key == key;
        if (editing && app.rail_edit_commit) {
            char* endp = nullptr;
            const double typed =
                std::strtod(app.rail_edit_buf.c_str(), &endp);
            if (endp != app.rail_edit_buf.c_str()) {
                const float scale = app.rail_edit_scale != 0.0f
                                        ? app.rail_edit_scale
                                        : 1.0f;
                *stage.staged = std::clamp(
                    static_cast<float>(typed / scale), min_v, max_v);
                *stage.changed = true;
                *stage.released = true;
            }
            app.rail_edit_key = {};
            app.rail_edit_commit = false;
            editing = false;
        }
        FrameUi::RailEdit redit{};
        redit.key = key;
        redit.scale = 1.0f;
        redit.clicked = arena.alloc<bool>();
        {
            char seed[32];
            std::snprintf(seed, sizeof(seed), "%g", value);
            redit.seed = arena.dup(seed, std::strlen(seed));
        }
        out.rail_edits.push_back(redit);
        opts.out_value_clicked = redit.clicked;
        const uint32_t o = ordinal < 18 ? ordinal : 17;
        ++ordinal;
        // Grouped member: the knob micro toggles this param on/off the
        // group FACE (exposed params — direct aliases, no macros).
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
        // Selector params render as DROPDOWNS: a pick lands as
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
        } else if (editing) {
            // The open type-in editor: buffer + caret in the slider slot.
            std::string shown = app.rail_edit_buf + "_";
            ButtonOpts bo;
            bo.align_left = true;
            bo.width = SizeSpec::fill();
            bo.tooltip = "enter commits, esc cancels";
            control = Button(arena,
                             arena.dup(shown.c_str(), shown.size()),
                             &state.value_edit_button, nullptr, bo);
        } else if (format && std::strstr(format, "deg")) {
            control = DialF(arena, stage.staged, min_v, max_v, slider_state,
                            opts);
        } else {
            control = SliderF(arena, stage.staged, min_v, max_v,
                              slider_state, opts);
        }
        rows.push_back(param_row(
            arena, label, control,
            &state.route_buttons[o], add_route.clicked,
            &state.key_buttons[o], key_toggle.clicked, expose_state,
            expose_clicked, expose_tip, rail_keyed(key),
            rail_routed(key)));
        out.add_routes.push_back(add_route);
        out.key_toggles.push_back(key_toggle);
        out.params.push_back(stage);
    };

    stage_slider(doc::kWetParam, "wet/dry", 0.0f, 1.0f, fx.wet, "%.2f",
                 &state.wet, nullptr,
                 "processed vs input mix - click the value to type it");
    stage_slider(doc::kOpacityParam, "opacity", 0.0f, 1.0f, fx.opacity, "%.2f",
                 &state.opacity, nullptr,
                 "final blend over the input - click the value to type it");
    for (uint32_t p = 0; p < info.param_count && p < 16; ++p) {
        const doc::ParamDesc& desc = info.params[p];
        const char* options =
            fx.type == doc::EffectType::Text && p == 0 &&
                    !app.font_options.empty()
                ? app.font_options.c_str()
                : desc.options;
        // Auto tooltip: full label + range + default, so truncated
        // labels and bare numbers explain themselves on hover.
        char tipbuf[96];
        std::snprintf(tipbuf, sizeof(tipbuf),
                      "%s - %g to %g, default %g. click the value to type.",
                      desc.label, desc.min_value, desc.max_value,
                      desc.default_value);
        stage_slider(static_cast<int>(p), desc.label, desc.min_value,
                     desc.max_value, fx.params[p], desc.format,
                     &state.params[p], options,
                     arena.dup(tipbuf, std::strlen(tipbuf)));
    }
    if (fx.type == doc::EffectType::Text) {
        // The STRING field: click opens the shared inline editor;
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

// Group container: ONE outlined panel holding the header
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

    // The FACE: exposed member params as DIRECT aliases — same
    // ParamStage path as any effect slider, the x hides from the face.
    size_t face_i = 0;
    for (const doc::ParamKey& fkey : group.exposed) {
        if (face_i >= 8) break;
        size_t fli = 0, ffi = 0;
        if (!find_effect_by_id(app.look(), fkey.effect_id, &fli, &ffi))
            continue;
        const doc::EffectInstance& mfx =
            app.look().layers[fli].stack[ffi];
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
            fopts = mfx.type == doc::EffectType::Text &&
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
        // Selector aliases keep their dropdown.
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

// Popup source entries (docs/flow_canvas.md/v4): sources are just
// nodes you add like anything else. "source: clip" is THE input — a tap
// off the project clip (the adjustment type is gone; a clip tap merged
// back through a Blend IS an adjustment). Order here MUST match the pick
// handler's walk.
// "source: look" mints a NEW empty look and places an instance of it —
// the one way to nest from the canvas (docs/look.md); double-click the
// card to go edit it.
static const char* kSrcAddLabels[] = {
    "source: clip",  "source: solid", "source: gradient",
    "source: noise", "source: pattern", "source: osc",
    "source: shape", "source: look"};
static const doc::LayerSourceKind kSrcAddKinds[] = {
    doc::LayerSourceKind::Clip,
    doc::LayerSourceKind::Solid, doc::LayerSourceKind::Gradient,
    doc::LayerSourceKind::Noise, doc::LayerSourceKind::TestPattern,
    doc::LayerSourceKind::Oscillator, doc::LayerSourceKind::Shape,
    doc::LayerSourceKind::LookRef};
// Which entry mints a fresh look to nest instead of binding media.
static const bool kSrcAddIsLook[] = {false, false, false, false,
                                     false, false, false, true};
constexpr int kSrcAddCount =
    static_cast<int>(sizeof(kSrcAddLabels) / sizeof(kSrcAddLabels[0]));

// Popup value-node entries (docs/flow_canvas.md): each spawns an
// unconnected value node at the click point — wiring happens by dragging
// its out port onto a param row (or a helper node's input row). Order
// here MUST match the pick handler's walk.
static const char* kValAddLabels[] = {
    "value: lfo",       "value: random",  "value: audio low",
    "value: audio mid", "value: audio high", "value: onset",
    "value: motion",    "value: bright",  "value: sample",
    "value: region",    "value: math",    "value: normalise"};
static const doc::ModSourceType kValAddTypes[] = {
    doc::ModSourceType::Lfo,        doc::ModSourceType::Drift,
    doc::ModSourceType::AudioLow,   doc::ModSourceType::AudioMid,
    doc::ModSourceType::AudioHigh,  doc::ModSourceType::AudioOnset,
    doc::ModSourceType::VideoMotion, doc::ModSourceType::VideoBrightness,
    doc::ModSourceType::VideoSample, doc::ModSourceType::VideoRegion,
    doc::ModSourceType::Math,       doc::ModSourceType::Normalise};
constexpr int kValAddCount =
    static_cast<int>(sizeof(kValAddLabels) / sizeof(kValAddLabels[0]));

// Source-card rows that alias layer params, in card order: row index ->
// layer param index (-1 = a selector dropdown, not a mod target). MUST
// mirror the source card builder's conditional row order.
static std::vector<int> layer_mod_row_map(const doc::Layer& sl) {
    using LSK = doc::LayerSourceKind;
    std::vector<int> map;
    map.push_back(0);   // opacity
    if (sl.source == LSK::Solid || sl.source == LSK::Gradient ||
        sl.source == LSK::Noise || sl.source == LSK::Oscillator) {
        map.push_back(1);
        map.push_back(2);
        map.push_back(3);
    }
    if (sl.source == LSK::Gradient || sl.source == LSK::Noise ||
        sl.source == LSK::Oscillator) {
        map.push_back(4);
        map.push_back(5);
        map.push_back(6);
    }
    if (sl.source == LSK::Gradient || sl.source == LSK::Oscillator)
        map.push_back(8);
    if (sl.source == LSK::Noise) map.push_back(7);
    if (sl.source == LSK::Oscillator) {
        map.push_back(7);
        map.push_back(-1);   // wave dropdown
    }
    if (sl.source == LSK::Shape) {
        map.push_back(7);
        map.push_back(8);
        map.push_back(-1);   // shape dropdown
    }
    return map;
}

// Value-card operand rows: which helper input (0 = a, 1 = b) a row
// wires; -1 = not an input. Mirrors the value card builder's row order.
static int value_input_of_row(const doc::ValueNode& vn, int row) {
    if (vn.source.type == doc::ModSourceType::Math)
        return row == 2 ? 0 : row == 3 ? 1 : -1;
    if (vn.source.type == doc::ModSourceType::Normalise)
        return row == 1 ? 0 : -1;
    return -1;
}
static int value_row_of_input(const doc::ValueNode& vn, int which) {
    if (vn.source.type == doc::ModSourceType::Math)
        return which == 0 ? 2 : 3;
    if (vn.source.type == doc::ModSourceType::Normalise)
        return which == 0 ? 1 : -1;
    return -1;
}

// Node-canvas graph (docs/flow_canvas.md): the document translated
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
    // Param housekeeping: defaults / clipboard across same-type
    // effects.
    ResetParams, CopyParams, PasteParams,
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
    // SEQUENCE scope shows no graph: sequences own no effects, so the
    // canvas offers nothing to wire. Open a look (double-click a block's
    // target in the browser, or the browser's looks list) to edit one.
    if (!app.scope_is_look()) {
        auto* graph = arena.alloc<flow::Graph>();
        auto* events = arena.alloc<flow::Output>();
        *graph = {};
        *events = {};
        graph->hint =
            "a sequence arranges looks - open a look to edit its graph";
        return {graph, events, nullptr, nullptr};
    }
    const doc::Look& d = app.look();
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
    // Folded groups (subgraphs): members collapse into ONE card that
    // shows the exposed face; links crossing the boundary re-anchor there.
    std::unordered_set<uint64_t> emitted_groups;

    static const char* kModNames[] = {"lfo",    "drift",  "a.low",
                                      "a.mid",  "a.high", "onset",
                                      "motion", "bright", "lfo.bpm",
                                      "env",    "cut",    "beat",
                                      "sample", "region", "math",
                                      "norm"};
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

    // Live values: the look resolved at the playhead (lanes + value
    // graph baked) feeds row ticks and the value-card scopes, so driven
    // params visibly move with the transport. Video-sampling nodes read
    // 0 here (no frame view on the UI thread).
    const double live_fps =
        app.player.fps() > 0.0 ? app.player.fps() : 30.0;
    const double play_frame =
        app.has_timeline() ? app.player.current_frame_index() : 0.0;
    const uint32_t live_frame = static_cast<uint32_t>(play_frame);
    const double live_audio_off =
        static_cast<double>(app.document.audio_offset_ms) * 0.001;
    const mod::AnalysisCurves* live_analysis =
        app.has_analysis ? &app.analysis : nullptr;
    doc::Look resolved = d;
    mod::resolve_look(d, resolved, live_frame, live_fps, live_analysis,
                      live_audio_off);
    mod::ValueEnv venv;
    venv.look = &d;
    venv.t = live_frame / live_fps;
    venv.frame = live_frame;
    venv.analysis = live_analysis;
    venv.fps = live_fps;
    venv.audio_off = live_audio_off;
    auto resolved_fx_value = [&](uint64_t eid, int pi, float* out_v) {
        size_t rli = 0, rfi = 0;
        if (!find_effect_by_id(resolved, eid, &rli, &rfi)) return false;
        *out_v = mod::param_value(resolved.layers[rli].stack[rfi], pi);
        return true;
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
            lrow.remove = arena.alloc<bool>();
            lrow.up = arena.alloc<bool>();
            lrow.down = arena.alloc<bool>();
            out.layer_rows.push_back(lrow);

            // Source card rows (parity): the same conditional field
            // set the rail shows — each continuous field a mod target
            // with key/route dots; the waveform stays a dropdown.
            flow::ParamRow* rows = arena.alloc<flow::ParamRow>(10);
            int srow = 0;
            auto layer_row = [&](FrameUi::LayerField field,
                                 const char* label, float min_v,
                                 float max_v, float value, const char* fmt,
                                 const char* options = nullptr) {
                if (srow >= 10) return;
                FrameUi::LayerStage lstage{};
                lstage.layer_id = layer.id;
                lstage.field = field;
                lstage.staged = arena.alloc<float>();
                *lstage.staged = value;
                lstage.original = value;
                lstage.changed = arena.alloc<bool>();
                lstage.released = arena.alloc<bool>();
                out.layer_stages.push_back(lstage);
                rows[srow].label = label;
                rows[srow].min_v = min_v;
                rows[srow].max_v = max_v;
                rows[srow].format = fmt;
                if (options) {
                    rows[srow].kind = 1;
                    rows[srow].options = options;
                } else {
                    const doc::ParamKey lkey{
                        layer.id | doc::kLayerParamBit,
                        static_cast<int>(field)};
                    FrameUi::KeyToggle ktog{lkey, value,
                                            arena.alloc<bool>()};
                    out.key_toggles.push_back(ktog);
                    FrameUi::AddRoute aroute{lkey, arena.alloc<bool>()};
                    out.add_routes.push_back(aroute);
                    rows[srow].key_clicked = ktog.clicked;
                    rows[srow].route_clicked = aroute.clicked;
                    rows[srow].modulated =
                        param_modulated(lkey.effect_id, lkey.param_index);
                    rows[srow].keyed =
                        param_keyed(lkey.effect_id, lkey.param_index);
                    if (rows[srow].modulated || rows[srow].keyed) {
                        for (doc::Layer& rl : resolved.layers)
                            if (rl.id == layer.id)
                                if (float* slot = mod::layer_param_slot(
                                        rl, static_cast<int>(field))) {
                                    rows[srow].live = *slot;
                                    rows[srow].has_live = true;
                                }
                    }
                }
                rows[srow].staged = lstage.staged;
                rows[srow].changed = lstage.changed;
                rows[srow].released = lstage.released;
                ++srow;
            };
            using LFs = FrameUi::LayerField;
            using LSK = doc::LayerSourceKind;
            layer_row(LFs::Opacity, "opacity", 0.0f, 1.0f, layer.opacity,
                      "%.2f");
            if (layer.source == LSK::Solid ||
                layer.source == LSK::Gradient ||
                layer.source == LSK::Noise ||
                layer.source == LSK::Oscillator) {
                layer_row(LFs::ColorAR, "color a r", 0.0f, 1.0f,
                          layer.color_a[0], "%.2f");
                layer_row(LFs::ColorAG, "color a g", 0.0f, 1.0f,
                          layer.color_a[1], "%.2f");
                layer_row(LFs::ColorAB, "color a b", 0.0f, 1.0f,
                          layer.color_a[2], "%.2f");
            }
            if (layer.source == LSK::Gradient ||
                layer.source == LSK::Noise ||
                layer.source == LSK::Oscillator) {
                layer_row(LFs::ColorBR, "color b r", 0.0f, 1.0f,
                          layer.color_b[0], "%.2f");
                layer_row(LFs::ColorBG, "color b g", 0.0f, 1.0f,
                          layer.color_b[1], "%.2f");
                layer_row(LFs::ColorBB, "color b b", 0.0f, 1.0f,
                          layer.color_b[2], "%.2f");
            }
            if (layer.source == LSK::Gradient ||
                layer.source == LSK::Oscillator)
                layer_row(LFs::Angle, "angle", -3.1416f, 3.1416f,
                          layer.gen_angle, "%.2f");
            if (layer.source == LSK::Noise)
                layer_row(LFs::Scale, "scale", 2.0f, 128.0f,
                          layer.gen_scale, "%.0f px");
            if (layer.source == LSK::Oscillator) {
                layer_row(LFs::Scale, "frequency", 0.5f, 32.0f,
                          layer.gen_scale, "%.1f cyc");
                layer_row(LFs::OscShape, "wave", 0.0f, 3.0f,
                          static_cast<float>(layer.osc_shape), "%.0f",
                          "sine bars|rings|plasma|lissajous");
            }
            if (layer.source == LSK::Shape) {
                layer_row(LFs::Scale, "size", 0.5f, 30.0f, layer.gen_scale,
                          "%.1f");
                layer_row(LFs::Angle, "feather", 0.0f, 3.1416f,
                          layer.gen_angle, "%.2f");
                layer_row(LFs::OscShape, "shape", 0.0f, 2.0f,
                          static_cast<float>(layer.osc_shape % 3), "%.0f",
                          "circle|box|diamond");
            }
            if (doc::layer_is_clip(layer) && srow < 10) {
                // MEDIA on the card: "(none)", every asset, then
                // "import..." - the node names its clip where it lives,
                // not only in the rail.
                std::string opts = "(none)";
                int current = 0;
                for (size_t ai = 0; ai < app.document.assets.size();
                     ++ai) {
                    const doc::Asset& a = app.document.assets[ai];
                    opts += '|';
                    opts += a.name.empty() ? "clip" : a.name;
                    if (a.id == layer.asset)
                        current = static_cast<int>(ai) + 1;
                }
                opts += "|import...";
                FrameUi::ClipRowBind crb{};
                crb.layer_id = layer.id;
                crb.staged = arena.alloc<float>();
                *crb.staged = static_cast<float>(current);
                crb.changed = arena.alloc<bool>();
                crb.n_assets =
                    static_cast<int>(app.document.assets.size());
                out.clip_row_binds.push_back(crb);
                rows[srow].label = "clip";
                rows[srow].kind = 1;
                rows[srow].options = arena.dup(opts.c_str(), opts.size());
                rows[srow].min_v = 0.0f;
                rows[srow].max_v = static_cast<float>(crb.n_assets + 1);
                rows[srow].format = "%.0f";
                rows[srow].staged = crb.staged;
                rows[srow].changed = crb.changed;
                rows[srow].released = arena.alloc<bool>();
                ++srow;
            }

            flow::Node src{};
            src.id = flow::node_id(flow::NodeKind::Source, layer.id);
            src.kind = flow::NodeKind::Source;
            // The card names WHAT the node is — clips and refs title by
            // their kind, generators name theirs. "layer N" was
            // storage-bag residue (flat graph).
            static const char* kSrcTitles[] = {"clip",    "solid",
                                               "gradient", "noise",
                                               "pattern",  "osc",
                                               "shape",    "look",
                                               "sequence"};
            static_assert(sizeof(kSrcTitles) / sizeof(kSrcTitles[0]) ==
                              static_cast<size_t>(
                                  doc::LayerSourceKind::Count),
                          "source card titles track the enum");
            src.title = kSrcTitles[static_cast<size_t>(layer.source) %
                                   static_cast<size_t>(
                                       doc::LayerSourceKind::Count)];
            src.bypassed = !layer.visible;
            src.has_out = true;
            src.has_matte_port = true;
            // A nested ref opens the entity it plays on a body
            // double-click, exactly as a group card opens its subgraph.
            src.is_look = doc::layer_is_nested(layer) && layer.target != 0;
            src.rows = rows;
            src.row_count = srow;
            src.bypass_clicked = lrow.visible_changed;
            src.remove_clicked = lrow.remove;
            set_preview(src, layer.id | (1ull << 62));
            if (layer.node_x != 0.0f || layer.node_y != 0.0f) {
                src.x = layer.node_x;
                src.y = layer.node_y;
            } else {
                src.x = auto_x;
                src.y = auto_y;
                // Materialize the derived slot (docs/flow_canvas.md
                // "auto-layout once"): committed the first frame it
                // appears so later deletions never re-slot survivors.
                // Positions are pure UI state the renderer never reads —
                // command-exempt (undo would splice layout writes into
                // gesture coalescing).
                app.look().layers[li].node_x = src.x;
                app.look().layers[li].node_y = src.y;
            }
            // Derived layout is a PURE GRID: the slot advances by pitch
            // regardless of where the card actually sits, so dragging one
            // node never shifts auto-laid neighbours.
            auto_x += kAutoPitch;
            nodes.push_back(src);
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
                if (!emitted_groups.insert(folded->id).second) continue;

                // Face rows: exposed member params as DIRECT
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
                        // Selector aliases keep their dropdown;
                        // the Text font selector keeps the runtime list.
                        fopts = mfx.type == doc::EffectType::Text &&
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
                                            arena.alloc<bool>()};
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
                    float flv = 0.0f;
                    if ((rows[slot].modulated || rows[slot].keyed) &&
                        resolved_fx_value(fkey.effect_id,
                                          fkey.param_index, &flv)) {
                        rows[slot].live = flv;
                        rows[slot].has_live = true;
                    }
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
                    for (doc::Group& mg : app.look().layers[li].groups)
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

            // Text cards append the STRING row after the params.
            const bool is_text_fx =
                fx.type == doc::EffectType::Text;
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
                                              arena.alloc<bool>()};
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
                // Scoped member rows carry the FACE toggle: the
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
                float lv = 0.0f;
                if ((row.modulated || row.keyed) &&
                    resolved_fx_value(fx.id, param_index, &lv)) {
                    row.live = lv;
                    row.has_live = true;
                }
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
                // .ttf files, not a static table.
                const char* options =
                    is_text_fx && p == 0 && !app.font_options.empty()
                        ? app.font_options.c_str()
                        : desc.options;
                stage_row(2 + p, static_cast<int>(p), desc.label,
                          desc.min_value, desc.max_value, fx.params[p],
                          desc.format, options);
            }
            if (is_text_fx) {
                // The STRING row: a real field on the card — click
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
            en.has_matte_port = true;
            en.has_aux_port = doc::effect_aux_port(fx.type) != nullptr;
            if (en.has_aux_port)
                en.aux_label = doc::effect_aux_port(fx.type);
            en.rows = rows;
            en.row_count = static_cast<int>(n_rows);
            en.bypass_clicked = act.bypass_changed;
            en.remove_clicked = act.remove;
            en.text_edit = fx.type == doc::EffectType::Text;
            set_preview(en, fx.id);
            if (fx.node_x != 0.0f || fx.node_y != 0.0f) {
                en.x = fx.node_x;
                en.y = fx.node_y;
            } else {
                en.x = auto_x;
                en.y = auto_y;
                app.look().layers[li].stack[i].node_x = en.x;
                app.look().layers[li].stack[i].node_y = en.y;
            }
            auto_x += kAutoPitch;
            nodes.push_back(en);
            fx_node[fx.id] = en.id;
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
        // Boundary nodes hold their OWN positions — the member
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
                 app.look().layers[scope_li].groups)
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
                 app.look().layers[scope_li].groups)
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

    // Chain + composite wires come from the TRUE-GRAPH link table; chain
    // documents draw the synthesized equivalent. In a scoped view, links
    // crossing the group boundary re-anchor on the In/Out boundary nodes.
    {
        const std::vector<doc::NodeLink> doc_links =
            d.links.empty() ? doc::synthesize_links(d) : d.links;
        auto canvas_id_of = [&](uint64_t doc_id) -> uint64_t {
            if (doc_id == 0) return flow::kOutNodeId;
            if (layer_index_by_id(d, doc_id) >= 0)
                return flow::node_id(flow::NodeKind::Source, doc_id);
            if (auto it = fx_node.find(doc_id); it != fx_node.end())
                return it->second;
            return 0;
        };
        for (const doc::NodeLink& l : doc_links) {
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

    // Aux row: the value graph's nodes. Cards edit node params; routes
    // and helper inputs draw as dashed wires into their target rows.
    // The value graph is look-global, so a scoped group view (image
    // internals) shows none of it.
    const float aux_y = 40.0f + static_cast<float>(n_layers) * kLanePitch;
    float aux_x = kAutoX0;
    for (size_t ni = 0; !scope && ni < d.value_nodes.size(); ++ni) {
        const doc::ValueNode& vn = d.value_nodes[ni];
        FrameUi::NodeRow nr{};
        nr.id = vn.id;
        nr.remove = arena.alloc<bool>();

        flow::ParamRow* rows = arena.alloc<flow::ParamRow>(6);
        int slot = 0;
        auto pick_row = [&](int which, const char* label,
                            const char* options, int current) {
            nr.pick_staged[which] = arena.alloc<float>();
            *nr.pick_staged[which] = static_cast<float>(current);
            nr.pick_changed[which] = arena.alloc<bool>();
            rows[slot].label = label;
            rows[slot].min_v = 0.0f;
            rows[slot].max_v = static_cast<float>(
                std::max(1, doc::param_option_count(options) - 1));
            rows[slot].format = "%.0f";
            rows[slot].kind = 1;
            rows[slot].options = options;
            rows[slot].staged = nr.pick_staged[which];
            rows[slot].changed = nr.pick_changed[which];
            rows[slot].released = arena.alloc<bool>();
            ++slot;
        };
        auto slider_row = [&](int si, const char* label, float min_v,
                              float max_v, float value, const char* fmt) {
            nr.slot_staged[si] = arena.alloc<float>();
            *nr.slot_staged[si] = value;
            nr.slot_original[si] = value;
            nr.slot_changed[si] = arena.alloc<bool>();
            nr.slot_released[si] = arena.alloc<bool>();
            rows[slot].label = label;
            rows[slot].min_v = min_v;
            rows[slot].max_v = max_v;
            rows[slot].format = fmt;
            rows[slot].staged = nr.slot_staged[si];
            rows[slot].changed = nr.slot_changed[si];
            rows[slot].released = nr.slot_released[si];
            ++slot;
        };
        static const std::string kModOptions = [] {
            std::string s;
            for (size_t i = 0; i < kModNameCount; ++i) {
                if (i) s += '|';
                s += kModNames[i];
            }
            return s;
        }();
        pick_row(0, "kind", kModOptions.c_str(),
                 static_cast<int>(vn.source.type));
        const bool is_lfo = vn.source.type == doc::ModSourceType::Lfo ||
                            vn.source.type == doc::ModSourceType::LfoBeat;
        const bool is_pulse =
            vn.source.type == doc::ModSourceType::Envelope ||
            vn.source.type == doc::ModSourceType::Beat;
        const bool has_rate =
            is_lfo || is_pulse ||
            vn.source.type == doc::ModSourceType::Drift;
        const bool is_video =
            vn.source.type == doc::ModSourceType::VideoSample ||
            vn.source.type == doc::ModSourceType::VideoRegion;
        if (is_lfo)
            pick_row(1, "shape", "sine|triangle|square|s&h",
                     static_cast<int>(vn.source.shape));
        else if (vn.source.type == doc::ModSourceType::Envelope)
            pick_row(1, "trigger", "onset|cut|beat|key",
                     static_cast<int>(vn.source.trigger % 4));
        else if (is_video)
            pick_row(1, "channel", "luma|red|green|blue",
                     static_cast<int>(vn.source.channel % 4));
        else if (vn.source.type == doc::ModSourceType::Math)
            pick_row(1, "op",
                     "add|subtract|multiply|divide|min|max|floor|abs",
                     static_cast<int>(vn.op));
        if (has_rate)
            slider_row(0, is_pulse ? "decay" : "rate", 0.05f, 8.0f,
                       is_pulse ? vn.source.decay : vn.source.rate_hz,
                       is_pulse ? "%.2f s" : "%.2f hz");
        // Wired operand rows show the incoming value's live tick; the
        // slider still edits the (ignored) constant.
        auto input_tick = [&](int row, uint64_t in_id) {
            if (!in_id) return;
            rows[row].modulated = true;
            rows[row].live = mod::eval_value_node(venv, in_id);
            rows[row].has_live = true;
        };
        if (vn.source.type == doc::ModSourceType::Math) {
            // Operand rows are wire drop targets (value_input_of_row
            // maps them); unwired they read their constant slider.
            slider_row(0, "a", -4.0f, 4.0f, vn.const_a, "%.2f");
            slider_row(1, "b", -4.0f, 4.0f, vn.const_b, "%.2f");
            rows[slot - 2].value_input = true;
            rows[slot - 1].value_input = true;
            input_tick(slot - 2, vn.in_a);
            input_tick(slot - 1, vn.in_b);
        }
        if (vn.source.type == doc::ModSourceType::Normalise) {
            slider_row(0, "in", -4.0f, 4.0f, vn.const_a, "%.2f");
            rows[slot - 1].value_input = true;
            input_tick(slot - 1, vn.in_a);
            // Window bounds stay -1..1; magnitude comes from mult
            // (window = [min, max] * mult).
            slider_row(1, "min", -1.0f, 1.0f, vn.in_min, "%.2f");
            slider_row(2, "max", -1.0f, 1.0f, vn.in_max, "%.2f");
            slider_row(3, "mult", 0.01f, 100.0f, vn.const_b, "%.2f x");
        }
        if (is_video) {
            // Sampling window rows: point x/y, region w/h.
            static const char* kPosLabels[4] = {"x", "y", "w", "h"};
            const float cur[4] = {vn.source.px, vn.source.py,
                                  vn.source.pw, vn.source.ph};
            const int n_pos =
                vn.source.type == doc::ModSourceType::VideoRegion ? 4 : 2;
            for (int pi = 0; pi < n_pos; ++pi)
                slider_row(pi, kPosLabels[pi], pi >= 2 ? 0.02f : 0.0f,
                           1.0f, cur[pi], "%.2f");
        }
        out.node_rows.push_back(nr);

        flow::Node rn{};
        rn.id = flow::node_id(flow::NodeKind::ModSource, vn.id);
        rn.kind = flow::NodeKind::ModSource;
        rn.title = kModNames[static_cast<size_t>(vn.source.type) %
                             kModNameCount];
        rn.has_out = true;
        rn.rows = rows;
        rn.row_count = slot;
        rn.remove_clicked = nr.remove;
        // Scope strip: the node's OUTPUT (helper chain included) over
        // the next few seconds from the playhead. Video-sampling nodes
        // skip it - a flat 0 plot would misreport the render.
        if (!is_video) {
            constexpr int kScopeN = 96;
            constexpr double kScopeSeconds = 4.0;
            float* samples = arena.alloc<float>(kScopeN);
            float lo = 0.0f, hi = 1.0f;
            mod::ValueEnv senv = venv;
            for (int si = 0; si < kScopeN; ++si) {
                const double f =
                    play_frame +
                    kScopeSeconds * live_fps * si / (kScopeN - 1);
                senv.t = f / live_fps;
                senv.frame = static_cast<uint32_t>(f);
                samples[si] = mod::eval_value_node(senv, vn.id);
                lo = std::min(lo, samples[si]);
                hi = std::max(hi, samples[si]);
            }
            rn.scope = samples;
            rn.scope_count = kScopeN;
            rn.scope_lo = lo;
            rn.scope_hi = hi;
        }
        if (vn.node_x != 0.0f || vn.node_y != 0.0f) {
            rn.x = vn.node_x;
            rn.y = vn.node_y;
        } else {
            rn.x = aux_x;
            rn.y = aux_y;
            app.look().value_nodes[ni].node_x = rn.x;
            app.look().value_nodes[ni].node_y = rn.y;
        }
        aux_x += kAutoPitch;
        nodes.push_back(rn);
        grid_max_x = std::max(grid_max_x, aux_x);

        // Helper-input wires: upstream node -> this card's operand row.
        for (int which = 0; which < 2; ++which) {
            const uint64_t src_id = which == 0 ? vn.in_a : vn.in_b;
            if (!src_id) continue;
            const int trow = value_row_of_input(vn, which);
            if (trow < 0) continue;
            wires.push_back(
                {flow::node_id(flow::NodeKind::ModSource, src_id), rn.id,
                 2, trow});
        }
    }

    // Route wires: value node -> the driven param's row. Effect targets
    // hidden in a folded group re-anchor on the group card's edge; layer
    // targets land on the source card's matching row; the morph target
    // {0, 0} lands on the Output card.
    for (const doc::ModRoute& r : d.mod_routes) {
        if (scope || !r.node) continue;
        const uint64_t from =
            flow::node_id(flow::NodeKind::ModSource, r.node);
        uint64_t target = 0;
        int to_row = -1;
        if (r.target.effect_id & doc::kLayerParamBit) {
            const uint64_t lid = r.target.effect_id & ~doc::kLayerParamBit;
            if (layer_index_by_id(d, lid) >= 0) {
                target = flow::node_id(flow::NodeKind::Source, lid);
                const std::vector<int> map = layer_mod_row_map(
                    d.layers[static_cast<size_t>(
                        layer_index_by_id(d, lid))]);
                for (size_t rr = 0; rr < map.size(); ++rr)
                    if (map[rr] == r.target.param_index)
                        to_row = static_cast<int>(rr);
            }
        } else if (r.target.effect_id == 0) {
            if (r.target.param_index == 0) target = flow::kOutNodeId;
        } else if (auto it = fx_node.find(r.target.effect_id);
                   it != fx_node.end()) {
            target = it->second;
            // Effect cards: row 0 wet, 1 opacity, 2+p params (the mod
            // wire lands on the driven row's gutter).
            const bool grouped =
                static_cast<flow::NodeKind>((target >> 56) - 1) ==
                flow::NodeKind::Group;
            to_row = grouped ? -1
                : r.target.param_index == doc::kWetParam ? 0
                : r.target.param_index == doc::kOpacityParam ? 1
                : r.target.param_index >= 0 ? 2 + r.target.param_index
                                            : -1;
        }
        if (target) wires.push_back({from, target, 2, to_row});
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
            app.look().out_node_x = on.x;
            app.look().out_node_y = on.y;
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
        case SelKind::ModSource:
            selected = flow::node_id(flow::NodeKind::ModSource, app.sel.id);
            break;
        case SelKind::Output:
            selected = flow::kOutNodeId;
            break;
        default:
            break;
    }

    // Frames: titled grouping boxes, removable from the canvas.
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
    } else if (app.scope_is_look()) {
        // Scoped into a look: the crumb names it and "main" goes back
        // up to the project timeline.
        const std::string& lname =
            d.name.empty() ? std::string("look") : d.name;
        graph->crumb = arena.dup(lname.c_str(), lname.size());
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
                for (const doc::EffectType et : category_effects_sorted(
                         static_cast<doc::FxCategory>(c))) {
                    const doc::EffectInfo& info = doc::effect_info(et);
                    if (!matches(info.label, cat)) continue;
                    if (!head) push(cat, true, {}), head = true;
                    push(info.label, false,
                         {AddAction::Effect,
                          static_cast<int32_t>(et), 0});
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
                    if (find_effect_by_id(app.look(), doc_id, &li, &fi))
                        bypassed = app.look().layers[li]
                                       .stack[fi].bypass;
                } else {
                    for (const doc::Layer& gl : app.look().layers)
                        for (const doc::Group& gr : gl.groups)
                            if (gr.id == doc_id) bypassed = gr.bypass;
                }
                push(bypassed ? "enable (b)" : "bypass (b)",
                     CtxAction::Bypass);
            }
            if (kind == flow::NodeKind::Effect ||
                kind == flow::NodeKind::ModSource)
                push("duplicate (ctrl+d)", CtxAction::Duplicate);
            if (kind == flow::NodeKind::Effect) {
                push("group selection (ctrl+g)", CtxAction::Group);
                // Param housekeeping.
                push("reset params", CtxAction::ResetParams);
                push("copy params", CtxAction::CopyParams);
                if (app.param_clip_valid) {
                    size_t cli = 0, cfi = 0;
                    if (find_effect_by_id(app.look(), doc_id, &cli,
                                          &cfi) &&
                        app.look().layers[cli].stack[cfi].type ==
                            app.param_clip_type)
                        push("paste params", CtxAction::PasteParams);
                }
            }
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

// Two side panels from one pass: LEFT = project, layers,
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
        out.import_media_clicked = arena.alloc<bool>();
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
    const bool has_clip = app.has_media();
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

    // ---- BROWSER: sequences and looks - the things deliberately made
    // and named. Open moves the editing scope; "+" lays a block at the
    // playhead (or a ref node when a look is scoped).
    {
        auto entity_row = [&](uint64_t id, const std::string& name,
                              const char* fallback, const char* open_tip) {
            auto& bui = app.browser_ui[id];
            FrameUi::BrowserAction act{};
            act.id = id;
            act.open = arena.alloc<bool>();
            act.place = arena.alloc<bool>();
            ButtonOpts name_opts;
            name_opts.width = SizeSpec::fill();
            name_opts.flat = true;
            name_opts.align_left = true;
            name_opts.tooltip = open_tip;
            ButtonOpts place_opts;
            place_opts.width = SizeSpec::fixed(24);
            place_opts.tooltip = "place at the playhead";
            const bool scoped = id == app.scope_look;
            std::string nm = scoped ? "> " : "";
            nm += name.empty() ? fallback : name;
            rows.push_back(HStack(
                arena, {4.0f},
                {Button(arena, arena.dup(nm.c_str(), nm.size()),
                        &bui.open, act.open, name_opts),
                 Button(arena, "+", &bui.place, act.place, place_opts)}));
            out.browser_looks.push_back(act);
        };
        rows.push_back(Label(arena, "sequences", small_dim));
        for (const doc::Sequence& sq : app.document.sequences)
            entity_row(sq.id, sq.name, "sequence", "open this sequence");
        rows.push_back(Label(arena, "looks", small_dim));
        for (const doc::Look& lk : app.document.looks)
            entity_row(lk.id, lk.name, "look", "open this look's graph");
        out.new_sequence_clicked = arena.alloc<bool>();
        ButtonOpts seq_opts;
        seq_opts.width = SizeSpec::fill();
        seq_opts.tooltip = "a fresh sequence, placed nowhere";
        rows.push_back(Button(arena, "new sequence", &app.new_seq_button,
                              out.new_sequence_clicked, seq_opts));
        if (!app.document.assets.empty())
            rows.push_back(Label(arena, "assets", small_dim));
        for (const doc::Asset& a : app.document.assets) {
            auto& bui = app.browser_ui[a.id];
            FrameUi::BrowserAction act{};
            act.id = a.id;
            act.place = arena.alloc<bool>();
            ButtonOpts place_opts;
            place_opts.width = SizeSpec::fixed(24);
            place_opts.tooltip = "place at the playhead";
            const std::string an = a.name.empty() ? "clip" : a.name;
            LayoutNode* alabel =
                Label(arena, arena.dup(an.c_str(), an.size()), small_dim);
            alabel->width = SizeSpec::fill();
            rows.push_back(HStack(
                arena, {4.0f},
                {alabel,
                 Button(arena, "+", &bui.place, act.place, place_opts)}));
            out.browser_assets.push_back(act);
        }
    }

    if (has_clip) {
        const bool is_still = is_still_source(primary_clip_path(app.document));
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
        // Time remap — a DOCUMENT parameter, so it lives here
        // rather than in the transport bar. "~" routes a mod source onto
        // speed, "k" drops a keyframe (lanes make it a true speed ramp).
        if (!is_still) {
        out.speed_staged = arena.alloc<float>();
        *out.speed_staged = app.document.speed;
        out.speed_changed = arena.alloc<bool>();
        SliderOpts spd_opts;
        spd_opts.format = "%.2fx";
        spd_opts.out_changed = out.speed_changed;
        // Project speed is a scalar (ramps live per-block or inside
        // looks), so no route/key micros here.
        rows.push_back(value_row(
            arena, "speed",
            SliderF(arena, out.speed_staged, 0.0f, doc::kMaxSpeed,
                    &app.speed_slider, spd_opts)));
        out.time_mode_selected = arena.alloc<int>();
        *out.time_mode_selected = -1;
        static const char* kTimeModes[] = {"forward", "reverse", "ping-pong"};
        rows.push_back(value_row(
            arena, "time",
            Dropdown(arena, kTimeModes, 3,
                     static_cast<int>(app.document.time_mode),
                     &app.time_mode_dd, out.time_mode_selected,
                     SizeSpec::fill(), "playback direction")));
        // Sidechain + audio nudge.
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
        // Half-res proxy toggle, shown when the import
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
    // Lossless import: applies to the next import.
    {
        out.lossless_changed = arena.alloc<bool>();
        out.lossless_staged = arena.alloc<bool>();
        *out.lossless_staged = app.import_lossless;
        rows.push_back(Checkbox(arena, "lossless import",
                                out.lossless_staged, &app.lossless_check,
                                out.lossless_changed));
    }
    // ---- project: save / open, dirty star, Ctrl+S / Ctrl+O.
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
    // Recent projects: newest first, click to open (dirty-guarded
    // in the handler).
    for (size_t r = 0; r < app.recent_projects.size() && r < 6; ++r) {
        const std::filesystem::path rp = app.recent_projects[r];
        if (rp == app.project_path) continue;
        FrameUi::RecentRow rrow{r, arena.alloc<bool>()};
        out.recent_rows.push_back(rrow);
        ButtonOpts ropts;
        ropts.flat = true;
        ropts.align_left = true;
        ropts.width = SizeSpec::fill();
        ropts.tooltip = "open this recent project";
        const std::string rname = rp.filename().string();
        rows.push_back(Button(arena,
                              arena.dup(rname.c_str(), rname.size()),
                              &app.recent_buttons[r], rrow.clicked, ropts));
    }
    // Cache management: size + open + clear-unused.
    {
        char cache_line[64];
        std::snprintf(cache_line, sizeof(cache_line), "cache  %.1f gb",
                      app.cache_bytes / (1024.0 * 1024.0 * 1024.0));
        out.cache_open_clicked = arena.alloc<bool>();
        out.cache_clear_clicked = arena.alloc<bool>();
        ButtonOpts tiny;
        tiny.width = SizeSpec::fixed(48);
        tiny.flat = true;
        StackOpts crow_opts;
        crow_opts.gap = 4.0f;
        crow_opts.cross_align = AlignMode::Center;
        ButtonOpts tiny_clear = tiny;
        tiny_clear.tooltip =
            "delete every cached import bundle except the open clip's";
        tiny.tooltip = "open the cache folder";
        std::vector<LayoutNode*> crow{
            Label(arena, arena.dup(cache_line, std::strlen(cache_line)),
                  small_dim),
            Spacer(arena),
            Button(arena, "open", &app.cache_open_button,
                   out.cache_open_clicked, tiny),
            Button(arena, "clear", &app.cache_clear_button,
                   out.cache_clear_clicked, tiny_clear)};
        LayoutNode* cstack = VStackDyn(arena, crow_opts, crow);
        cstack->kind = NodeKind::HStack;
        rows.push_back(cstack);
    }
    // Message log: the transient status strip, kept — a failure
    // that flashed by is still readable here.
    if (!app.status_log.empty()) {
        LabelOpts log_dim = small_dim;
        log_dim.color = active_theme().text_disabled;
        const size_t n = app.status_log.size();
        for (size_t i = n > 4 ? n - 4 : 0; i < n; ++i)
            rows.push_back(Label(arena,
                                 arena.dup(app.status_log[i].c_str(),
                                           app.status_log[i].size()),
                                 log_dim));
    }
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
        // Frames stay addable from here — an empty graph must offer the
        // layout node too.
        out.add_frame_clicked = arena.alloc<bool>();
        ButtonOpts half;
        half.width = SizeSpec::fill();
        rows.push_back(HStack(
            arena, {4.0f},
            {Button(arena, "+ frame", &app.add_frame_button,
                    out.add_frame_clicked, half)}));
    }
    for (size_t li = 0; li < app.look().layers.size(); ++li) {
        const doc::Layer& layer = app.look().layers[li];
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
        lrow.remove = arena.alloc<bool>();
        lrow.up = arena.alloc<bool>();
        lrow.down = arena.alloc<bool>();

        const bool selected = app.layer_sel && li == app.selected_layer;
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
        down_opts.disabled = li + 1 >= app.look().layers.size();
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
        // Blend on the value grid, a dropdown like every enum.
        layer_rows_ui.push_back(value_row(
            arena, "blend",
            Dropdown(arena, kBlendNames, 5,
                     static_cast<int>(layer.blend), &ls.blend_dd,
                     lrow.blend_selected, SizeSpec::fill(),
                     "blend mode over the composite below")));
        // The clip node's MEDIA: pick any imported asset, or browse to
        // import-and-bind - no invisible binding.
        if (doc::layer_is_clip(layer)) {
            const size_t n_assets = app.document.assets.size();
            const char** items =
                arena.alloc<const char*>(std::max<size_t>(n_assets, 1));
            int current = -1;
            for (size_t ai = 0; ai < n_assets; ++ai) {
                const doc::Asset& a = app.document.assets[ai];
                const std::string an = a.name.empty() ? "clip" : a.name;
                items[ai] = arena.dup(an.c_str(), an.size());
                if (a.id == layer.asset) current = static_cast<int>(ai);
            }
            if (n_assets == 0) items[0] = "(no media imported)";
            FrameUi::ClipBind bind{};
            bind.layer_id = layer.id;
            bind.selected = arena.alloc<int>();
            *bind.selected = -1;
            bind.browse = arena.alloc<bool>();
            ButtonOpts bopts;
            bopts.width = SizeSpec::fixed(24);
            bopts.tooltip = "import media and bind this node";
            StackOpts crow;
            crow.gap = 4.0f;
            crow.width = SizeSpec::fill();
            layer_rows_ui.push_back(value_row(
                arena, "clip",
                HStack(arena, crow,
                       {Dropdown(arena, items,
                                 static_cast<int>(
                                     std::max<size_t>(n_assets, 1)),
                                 current, &ls.clip_dd, bind.selected,
                                 SizeSpec::fill(),
                                 "the media this node reads"),
                        Button(arena, "+", &ls.clip_browse, bind.browse,
                               bopts)})));
            out.clip_binds.push_back(bind);
        }

        int lslider = 0;
        auto lkeyed = [&](const doc::ParamKey& k) {
            for (const doc::KeyframeLane& l : app.look().lanes)
                if (l.target == k && !l.keys.empty()) return true;
            return false;
        };
        auto lrouted = [&](const doc::ParamKey& k) {
            for (const doc::ModRoute& r : app.look().mod_routes)
                if (r.target == k) return true;
            return false;
        };
        auto layer_slider = [&](FrameUi::LayerField field, const char* label,
                                float min_v, float max_v, float value,
                                const char* format,
                                float display_scale = 1.0f) {
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
            opts.display_scale = display_scale;
            // Layer params are mod targets: LayerField indices ≤
            // Rotate ARE the kLayerParamBit param indices.
            const doc::ParamKey lkey{layer.id | doc::kLayerParamBit,
                                     static_cast<int>(field)};
            FrameUi::AddRoute add_route{lkey, arena.alloc<bool>()};
            out.add_routes.push_back(add_route);
            FrameUi::KeyToggle ktog{lkey, value, arena.alloc<bool>()};
            out.key_toggles.push_back(ktog);
            // Rail type-in, same contract as effect rows.
            bool editing = app.rail_edit_key == lkey;
            if (editing && app.rail_edit_commit) {
                char* endp = nullptr;
                const double typed =
                    std::strtod(app.rail_edit_buf.c_str(), &endp);
                if (endp != app.rail_edit_buf.c_str()) {
                    const float scale = app.rail_edit_scale != 0.0f
                                            ? app.rail_edit_scale
                                            : 1.0f;
                    *stage.staged = std::clamp(
                        static_cast<float>(typed / scale), min_v, max_v);
                    *stage.changed = true;
                    *stage.released = true;
                }
                app.rail_edit_key = {};
                app.rail_edit_commit = false;
                editing = false;
            }
            FrameUi::RailEdit redit{};
            redit.key = lkey;
            redit.scale = display_scale;
            redit.clicked = arena.alloc<bool>();
            {
                char seed[32];
                std::snprintf(seed, sizeof(seed), "%g",
                              value * display_scale);
                redit.seed = arena.dup(seed, std::strlen(seed));
            }
            out.rail_edits.push_back(redit);
            opts.out_value_clicked = redit.clicked;
            LayoutNode* control;
            if (editing) {
                std::string shown = app.rail_edit_buf + "_";
                ButtonOpts bo;
                bo.align_left = true;
                bo.width = SizeSpec::fill();
                bo.tooltip = "enter commits, esc cancels";
                control = Button(arena,
                                 arena.dup(shown.c_str(), shown.size()),
                                 &ls.value_edit_button, nullptr, bo);
            } else if (format && std::strstr(format, "deg")) {
                control = DialF(arena, stage.staged, min_v, max_v,
                                &ls.sliders[lslider], opts);
            } else {
                control = SliderF(arena, stage.staged, min_v, max_v,
                                  &ls.sliders[lslider], opts);
            }
            layer_rows_ui.push_back(param_row(
                arena, label, control,
                &ls.route_buttons[lslider], add_route.clicked,
                &ls.key_buttons[lslider], ktog.clicked, nullptr, nullptr,
                nullptr, lkeyed(lkey), lrouted(lkey)));
            out.layer_stages.push_back(stage);
            ++lslider;
        };
        using LF = FrameUi::LayerField;
        layer_slider(LF::Opacity, "opacity", 0.0f, 1.0f, layer.opacity,
                     "%.2f");
        // Swatch opens the picker (fast visual edit); the channel rows
        // below stay because they carry the key/route mod affordances.
        auto color_swatch_row = [&](const char* label, const float rgb[3],
                                    bool is_b, ui::SwatchState& swatch) {
            FrameUi::ColorStage cstage{};
            cstage.layer_id = layer.id;
            cstage.color_b = is_b;
            cstage.staged = arena.alloc<float>(3);
            for (int c = 0; c < 3; ++c) {
                cstage.staged[c] = rgb[c];
                cstage.original[c] = rgb[c];
            }
            cstage.changed = arena.alloc<bool>();
            cstage.released = arena.alloc<bool>();
            out.color_stages.push_back(cstage);
            layer_rows_ui.push_back(value_row(
                arena, label,
                ColorSwatch(arena, rgb, &swatch, cstage.staged,
                            cstage.changed, cstage.released)));
        };
        if (layer.source == doc::LayerSourceKind::Solid ||
            layer.source == doc::LayerSourceKind::Gradient ||
            layer.source == doc::LayerSourceKind::Noise ||
            layer.source == doc::LayerSourceKind::Oscillator) {
            color_swatch_row("color a", layer.color_a, false, ls.swatch_a);
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
            color_swatch_row("color b", layer.color_b, true, ls.swatch_b);
            layer_slider(LF::ColorBR, "color b r", 0.0f, 1.0f,
                         layer.color_b[0], "%.2f");
            layer_slider(LF::ColorBG, "color b g", 0.0f, 1.0f,
                         layer.color_b[1], "%.2f");
            layer_slider(LF::ColorBB, "color b b", 0.0f, 1.0f,
                         layer.color_b[2], "%.2f");
        }
        if (layer.source == doc::LayerSourceKind::Gradient ||
            layer.source == doc::LayerSourceKind::Oscillator)
            // Stored in radians; the readout speaks degrees (units).
            layer_slider(LF::Angle, "angle", -3.1416f, 3.1416f,
                         layer.gen_angle, "%.0f deg", 57.29578f);
        if (layer.source == doc::LayerSourceKind::Noise)
            layer_slider(LF::Scale, "scale", 2.0f, 128.0f, layer.gen_scale,
                         "%.0f px");
        if (layer.source == doc::LayerSourceKind::Oscillator) {
            layer_slider(LF::Scale, "frequency", 0.5f, 32.0f,
                         layer.gen_scale, "%.1f cyc");
            // Waveform dropdown (generators): the oscillator is a
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
            // The matte maker: size + feather + geometry; place
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

        // Transform (crop/flip/scale/rotate + the clip's slip live on
        // the layer). Folded per layer; the header marks
        // itself when the transform is active so a folded card still tells.
        lrow.xf_toggle = arena.alloc<bool>();
        layer_rows_ui.push_back(SectionHeader(
            arena,
            doc::layer_has_transform(layer) ? "transform *" : "transform",
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
                const doc::ParamKey lkey{layer.id | doc::kLayerParamBit,
                                         static_cast<int>(field)};
                FrameUi::AddRoute add_route{lkey, arena.alloc<bool>()};
                out.add_routes.push_back(add_route);
                FrameUi::KeyToggle ktog{lkey, value, arena.alloc<bool>()};
                out.key_toggles.push_back(ktog);
                LayoutNode* xcontrol =
                    format && std::strstr(format, "deg")
                        ? DialF(arena, stage.staged, min_v, max_v,
                                &ls.xf_sliders[xslider], opts)
                        : SliderF(arena, stage.staged, min_v, max_v,
                                  &ls.xf_sliders[xslider], opts);
                layer_rows_ui.push_back(param_row(
                    arena, label, xcontrol,
                    &ls.xf_route_buttons[xslider], add_route.clicked,
                    &ls.xf_key_buttons[xslider], ktog.clicked, nullptr,
                    nullptr, nullptr, lkeyed(lkey), lrouted(lkey)));
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
            // SLIP: the clip node's static media in-point - the one
            // timing nuance a timeless look allows, so two clips can
            // hold a fixed sync offset. All scheduling lives on the
            // sequence's blocks.
            if (doc::layer_is_clip(layer)) {
                const doc::Asset* la = app.document.find_asset(layer.asset);
                const uint32_t media = la ? la->frame_count : 0;
                if (media > 1)
                    xf_slider(LF::Slip, "slip", 0.0f,
                              static_cast<float>(media - 1),
                              static_cast<float>(layer.slip), "%.0f f");
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
        app.look().layers.size() < doc::kMaxLayers) {
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
        !app.look().layers.empty()) {
        // Randomize: chaos slider + whole-stack button; each
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
        if (find_effect_by_id(app.look(), app.sel.id, &li, &fi)) {
            app.selected_layer = li;   // stack staging keys on this layer
            rows.push_back(Label(arena,
                                 app.look().layers[li].name.c_str(),
                                 small_dim));
            rows.push_back(build_effect_panel(arena, app, out, fi));
            // A grouped effect brings its group's face along — groups
            // have no card of their own on the node canvas.
            const doc::Layer& sel_layer = app.look().layers[li];
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
        if (find_group_by_id(app.look(), app.sel.id, &li)) {
            app.selected_layer = li;
            const doc::Layer& sel_layer = app.look().layers[li];
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

    // Add-node browser: category folds, shown after a
    // double-click on the canvas (or an explicit add request). Layers add
    // from the same place.
    if (app.sel.kind == SelKind::AddEffect &&
        layer_index_by_id(app.look(), app.sel.id) >= 0) {
        app.selected_layer = static_cast<size_t>(
            layer_index_by_id(app.look(), app.sel.id));
        rows.push_back(Label(arena,
                             app.insert_before_id != 0
                                 ? "into the clicked slot"
                                 : "at the end of the chain",
                             small_dim));
        {
            out.add_layer_open = arena.alloc<bool>();
            ButtonOpts half;
            half.width = SizeSpec::fill();
            out.add_frame_clicked = arena.alloc<bool>();
            rows.push_back(HStack(
                arena, {4.0f},
                {Button(arena, "+ layer...", &app.add_layer_open_button,
                        out.add_layer_open, half),
                 Button(arena, "+ frame", &app.add_frame_button,
                        out.add_frame_clicked, half)}));
            // Search: type-to-filter across every category.
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
            for (int c = 0;
                 c < static_cast<int>(doc::FxCategory::Count); ++c) {
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
                for (const doc::EffectType et :
                     category_effects_sorted(cat)) {
                    const size_t t = static_cast<size_t>(et);
                    const doc::EffectInfo& info = doc::effect_info(et);
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

    // ---- preset browser: click = drop the group onto the
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
        // Text search (browser is searchable). A flat field that
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
        // Single-file preset import; drag-and-drop works too.
        out.preset_import_clicked = arena.alloc<bool>();
        rows.push_back(Button(arena, "import preset...",
                              &app.preset_import_btn,
                              out.preset_import_clicked));
    }

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
                             app.sel.kind == SelKind::ModSource ||
                             app.sel.kind == SelKind::Output;
    if (app.sel.kind == SelKind::Output)
        rows.push_back(Heading(arena, "project & output"));
    else if (app.sel.kind == SelKind::ModSource)
        rows.push_back(Heading(arena, "value node"));
    else if (show_matrix)
        rows.push_back(Label(arena, "modulation", small_dim));
    size_t routes_shown = 0;
    if (show_matrix) {
    // Per-WIRE rows: which node drives which param, how hard, through
    // which curve. Node params (rate, shape, operands) edit on the
    // node's canvas card - the wire owns only amount + curve.
    const auto table = mod::build_param_table(app.document, app.look());
    auto path_of = [&](const doc::ParamKey& key) -> std::string {
        for (const auto& e : table)
            if (e.key == key) return e.path;
        return "(deleted)";
    };
    static const char* kSourceNames[] = {"lfo",    "drift", "a.low",
                                         "a.mid",  "a.high", "onset",
                                         "motion", "bright", "lfo.bpm",
                                         "env",    "cut",    "beat",
                                         "sample", "region", "math",
                                         "norm"};
    static_assert(sizeof(kSourceNames) / sizeof(kSourceNames[0]) ==
                      static_cast<size_t>(doc::ModSourceType::Count),
                  "route-panel source names track the enum");
    static const char* kCurveNames[] = {"lin", "exp", "s", "inv"};
    for (const doc::ModRoute& route : app.look().mod_routes) {
        // Selection filter (docs/flow_canvas.md): an effect shows the
        // wires driving it, a value node the wires it feeds.
        const bool match =
            app.sel.kind == SelKind::Output ? true
            : app.sel.kind == SelKind::ModSource ? route.node == app.sel.id
            : route.target.effect_id == app.sel.id;
        if (!match) continue;
        ++routes_shown;
        RouteUiState& rs = app.route_ui[route.id];
        FrameUi::RouteRow row{};
        row.id = route.id;
        row.curve_selected = arena.alloc<int>();
        *row.curve_selected = -1;
        row.remove = arena.alloc<bool>();

        const doc::ValueNode* vn =
            doc::find_value_node(app.look(), route.node);
        const char* nname = vn
            ? kSourceNames[static_cast<size_t>(vn->source.type) %
                           static_cast<size_t>(doc::ModSourceType::Count)]
            : "(deleted)";
        const std::string wire_label =
            std::string(nname) + " > " + path_of(route.target);
        std::vector<LayoutNode*> route_rows_ui;
        ButtonOpts route_x;
        route_x.width = SizeSpec::fixed(20);
        route_x.tooltip = "remove wire";
        route_rows_ui.push_back(HStack(
            arena, {2.0f},
            {Label(arena,
                   arena.dup(wire_label.c_str(), wire_label.size()),
                   small_dim),
             Spacer(arena),
             IconButton(arena, Icon::Close, &rs.remove_button, row.remove,
                        route_x)}));
        std::vector<LayoutNode*> pick_cells{Dropdown(
            arena, kCurveNames, 4, static_cast<int>(route.curve),
            &rs.curve_dd, row.curve_selected, SizeSpec::fill(),
            "response curve")};
        StackOpts pick_row;
        pick_row.gap = 4.0f;
        pick_row.cross_align = AlignMode::Center;
        LayoutNode* pick_stack = VStackDyn(arena, pick_row, pick_cells);
        pick_stack->kind = NodeKind::HStack;
        route_rows_ui.push_back(pick_stack);

        StackOpts route_col;
        route_col.gap = 4.0f;
        route_col.cross_align = AlignMode::Stretch;
        rows.push_back(Panel(arena, VStackDyn(arena, route_col, route_rows_ui),
                             PanelOpts{Edges::all(6), -1.0f,
                                       /*outline=*/false}));
        out.route_rows.push_back(row);
    }
    if (routes_shown == 0)
        rows.push_back(Label(
            arena,
            app.sel.kind == SelKind::ModSource
                ? "drag the out port onto a param row"
                : "press ~ next to a param",
            small_dim));
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
            apply_opts.disabled = !app.look().snapshots[s].valid;
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

        // Morph A>B: live once both slots are stored; "~" routes
        // a mod source onto the morph position (ParamKey {0, 0}).
        out.morph_staged = arena.alloc<float>();
        *out.morph_staged = app.look().morph_pos;
        out.morph_changed = arena.alloc<bool>();
        out.morph_released = arena.alloc<bool>();
        SliderOpts mopts;
        mopts.format = "%.2f";
        mopts.out_changed = out.morph_changed;
        mopts.out_released = out.morph_released;
        FrameUi::AddRoute morph_route{{0, 0}, arena.alloc<bool>()};
        ButtonOpts micro2;
        micro2.width = SizeSpec::fixed(22);
        const bool morph_ready = app.look().snapshots[0].valid &&
                                 app.look().snapshots[1].valid;
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

    // ---- export + render queue. The button stays live while a
    // job runs — further exports snapshot the current state and queue up.
    if (app.export_job) {
        const uint32_t total = app.export_job->progress.frames_total.load();
        const uint32_t done = app.export_job->progress.frames_done.load();
        std::snprintf(line, sizeof(line), "exporting %s %u%%",
                      app.export_job->out_path.filename().string().c_str(),
                      total ? done * 100 / total : 0);
        out.export_cancel_clicked = arena.alloc<bool>();
        ButtonOpts cancel_opts;
        cancel_opts.width = SizeSpec::fixed(56);
        std::vector<LayoutNode*> prow{
            Label(arena, line, dim), Spacer(arena),
            Button(arena, "cancel", &app.export_cancel_button,
                   out.export_cancel_clicked, cancel_opts)};
        LayoutNode* pstack = VStackDyn(arena, {}, prow);
        pstack->kind = NodeKind::HStack;
        pstack->gap = 4.0f;
        rows.push_back(pstack);
    }
    if (has_clip) {
        // Export settings: bitrate / output scale / audio — project
        // state through set_export_config_command like every other edit.
        out.export_bitrate_staged = arena.alloc<float>();
        *out.export_bitrate_staged = app.document.export_bitrate_mbps;
        out.export_bitrate_changed = arena.alloc<bool>();
        out.export_bitrate_released = arena.alloc<bool>();
        SliderOpts bopts;
        bopts.format = "%.0f mbps";
        bopts.out_changed = out.export_bitrate_changed;
        bopts.out_released = out.export_bitrate_released;
        rows.push_back(value_row(
            arena, "bitrate",
            SliderF(arena, out.export_bitrate_staged, 1.0f, 60.0f,
                    &app.export_bitrate_slider, bopts)));
        static const char* kExportScaleItems[] = {"full size", "half",
                                                  "quarter"};
        const int scale_current = app.document.export_scale >= 4   ? 2
                                  : app.document.export_scale == 2 ? 1
                                                                   : 0;
        out.export_scale_selected = arena.alloc<int>();
        *out.export_scale_selected = -1;
        rows.push_back(value_row(
            arena, "size",
            Dropdown(arena, kExportScaleItems, 3, scale_current,
                     &app.export_scale_dd, out.export_scale_selected,
                     SizeSpec::fill(),
                     "output resolution: source / half / quarter")));
        out.export_audio_staged = arena.alloc<bool>();
        *out.export_audio_staged = app.document.export_audio;
        out.export_audio_changed = arena.alloc<bool>();
        rows.push_back(Checkbox(arena, "export audio",
                                out.export_audio_staged,
                                &app.export_audio_check,
                                out.export_audio_changed));
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

// Bottom timeline: ruler + one row per keyframe lane. Lanes are
// created with the [k] button next to any param.
// Transport bar under the viewport: playback + monitoring only —
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

    // The FILM's length, not the timeline's: the buffer past the last
    // block is somewhere to drag to, and neither plays nor exports, so
    // the scrubber ends where the content does (which is also as far as
    // the playhead can travel - the trim confines it).
    const uint32_t frames = app.player.trim_out()
                                ? app.player.trim_out()
                                : app.player.frame_count();
    out.seek_staged = arena.alloc<float>();
    *out.seek_staged = static_cast<float>(app.player.current_frame_index());
    out.seek_changed = arena.alloc<bool>();
    items.push_back(Scrubber(arena, out.seek_staged,
                             static_cast<float>(frames), &app.scrubber,
                             out.seek_changed));

    // Frame counter + mm:ss:ff timecode.
    char line[64];
    const double fps = app.player.fps();
    const uint32_t at = app.player.current_frame_index();
    const int tc_min = fps > 0.0
        ? static_cast<int>(at / fps) / 60 : 0;
    const int tc_sec = fps > 0.0
        ? static_cast<int>(at / fps) % 60 : 0;
    const int tc_frm = fps > 0.0
        ? static_cast<int>(at - static_cast<uint32_t>(
              static_cast<int>(at / fps) * fps))
        : 0;
    std::snprintf(line, sizeof(line), "%u / %u  %02d:%02d:%02d", at, frames,
                  tc_min, tc_sec, tc_frm);
    items.push_back(SizedBox(arena, SizeSpec::fixed(120), SizeSpec::fixed(18),
                             Label(arena, line, small_dim)));

    // Monitor volume: mute chip + gain slider, app-level prefs.
    out.mute_clicked = arena.alloc<bool>();
    items.push_back(Chip(arena, "mute", app.audio_muted, &app.mute_button,
                         out.mute_clicked, "mute monitoring"));
    out.volume_staged = arena.alloc<float>();
    *out.volume_staged = app.audio_gain;
    out.volume_changed = arena.alloc<bool>();
    out.volume_released = arena.alloc<bool>();
    SliderOpts vol_opts;
    vol_opts.format = nullptr;
    vol_opts.out_changed = out.volume_changed;
    vol_opts.out_released = out.volume_released;
    vol_opts.tooltip = "monitor volume";
    items.push_back(SizedBox(arena, SizeSpec::fixed(64), SizeSpec::fixed(22),
                             SliderF(arena, out.volume_staged, 0.0f, 1.5f,
                                     &app.volume_slider, vol_opts)));

    out.loop_clicked = arena.alloc<bool>();
    items.push_back(Chip(arena, "loop", app.loop, &app.loop_check,
                         out.loop_clicked, "loop playback"));
    out.live_clicked = arena.alloc<bool>();
    items.push_back(Chip(arena, "live", app.live_mode, &app.live_button,
                         out.live_clicked,
                         "live mode: realtime mod sources, timeline hidden"));
    // Proxy indicator: plain text, only when the player is on
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

// ---- timeline keyboard helpers. Selection lives per lane in
// lane_ui, identified by key frame; edits collect first and execute after
// (set_lane_command can erase lanes — never mutate doc.lanes mid-walk).

// Commits the inline key readout editor: parses the buffer into the
// selected key's value or frame (frame edits clamp between neighbors).
void commit_key_edit(AppState& app) {
    const int mode = app.key_edit_mode;
    app.key_edit_mode = 0;
    if (mode == 0 || app.key_edit_buf.empty()) return;
    char* end = nullptr;
    const double num = std::strtod(app.key_edit_buf.c_str(), &end);
    if (end == app.key_edit_buf.c_str()) return;
    for (const doc::KeyframeLane& lane : app.look().lanes) {
        if (!(lane.target == app.key_edit_target)) continue;
        std::vector<doc::Keyframe> keys = lane.keys;
        for (size_t i = 0; i < keys.size(); ++i) {
            if (keys[i].frame != app.key_edit_frame) continue;
            if (mode == 1) {
                keys[i].value = static_cast<float>(num);
            } else {
                double f = std::round(num);
                if (i > 0) f = std::max(f, keys[i - 1].frame + 1.0);
                if (i + 1 < keys.size())
                    f = std::min(f, keys[i + 1].frame - 1.0);
                keys[i].frame = std::max(0.0, f);
            }
            app.undo.execute(
                app.document,
                doc::set_lane_command(app.scope_look,app.key_edit_target, std::move(keys)));
            return;
        }
        return;
    }
}

bool timeline_delete_selected_keys(AppState& app) {
    std::vector<std::pair<doc::ParamKey, std::vector<doc::Keyframe>>> edits;
    for (auto& entry : app.lane_ui) {
        LaneUiState& ls = entry.second;
        if (ls.sel_frames.empty() && ls.selected < 0) continue;
        const doc::ParamKey target{entry.first.first, entry.first.second};
        for (const doc::KeyframeLane& lane : app.look().lanes) {
            if (!(lane.target == target)) continue;
            std::vector<doc::Keyframe> kept;
            for (size_t i = 0; i < lane.keys.size(); ++i) {
                bool sel = ls.selected == static_cast<int>(i);
                for (const double f : ls.sel_frames)
                    if (lane.keys[i].frame == f) sel = true;
                if (!sel) kept.push_back(lane.keys[i]);
            }
            if (kept.size() != lane.keys.size())
                edits.emplace_back(target, std::move(kept));
            break;
        }
        ls.selected = -1;
        ls.sel_frames.clear();
    }
    for (auto& e : edits)
        app.undo.execute(app.document,
                         doc::set_lane_command(app.scope_look,e.first, std::move(e.second)));
    return !edits.empty();
}

bool timeline_copy_selected_keys(AppState& app) {
    for (auto& entry : app.lane_ui) {
        LaneUiState& ls = entry.second;
        if (ls.sel_frames.empty() && ls.selected < 0) continue;
        const doc::ParamKey target{entry.first.first, entry.first.second};
        for (const doc::KeyframeLane& lane : app.look().lanes) {
            if (!(lane.target == target)) continue;
            std::vector<doc::Keyframe> out;
            for (size_t i = 0; i < lane.keys.size(); ++i) {
                bool sel = ls.selected == static_cast<int>(i);
                for (const double f : ls.sel_frames)
                    if (lane.keys[i].frame == f) sel = true;
                if (sel) out.push_back(lane.keys[i]);
            }
            if (out.empty()) break;
            const double base = out.front().frame;
            for (doc::Keyframe& k : out) k.frame -= base;
            app.key_clipboard = std::move(out);
            app.key_clip_target = target;
            app.status = "copied " +
                         std::to_string(app.key_clipboard.size()) + " keys";
            return true;
        }
    }
    return false;
}

bool timeline_paste_keys(AppState& app) {
    if (app.key_clipboard.empty() || !app.has_timeline()) return false;
    const double at = app.player.current_frame_index();
    for (const doc::KeyframeLane& lane : app.look().lanes) {
        if (!(lane.target == app.key_clip_target)) continue;
        std::vector<doc::Keyframe> keys = lane.keys;
        for (doc::Keyframe k : app.key_clipboard) {
            k.frame += at;
            for (size_t i = 0; i < keys.size(); ++i)
                if (keys[i].frame == k.frame) {
                    keys.erase(keys.begin() + i);
                    break;
                }
            keys.push_back(k);
        }
        app.undo.execute(
            app.document,
            doc::set_lane_command(app.scope_look,app.key_clip_target, std::move(keys)));
        return true;
    }
    return false;
}

// Nearest key or marker strictly before/after the playhead ([ and ]).
double timeline_adjacent_mark(AppState& app, bool forward) {
    const double at = app.player.current_frame_index();
    double best = -1.0;
    auto consider = [&](double f) {
        if (forward ? f > at + 0.5 : f < at - 0.5) {
            if (best < 0.0 || (forward ? f < best : f > best)) best = f;
        }
    };
    if (app.scope_is_look())
        for (const doc::KeyframeLane& lane : app.look().lanes)
            for (const doc::Keyframe& k : lane.keys) consider(k.frame);
    else
        for (const uint32_t m : app.sequence().markers) consider(m);
    return best;
}

ui::LayoutNode* build_timeline(ui::LayoutArena& arena, AppState& app,
                               FrameUi& out) {
    using namespace ui;
    LabelOpts small_dim;
    small_dim.color = active_theme().text_dim;
    small_dim.size = active_theme().font_size_small;

    const uint32_t frame_count = app.player.frame_count();
    const uint32_t playhead = app.player.current_frame_index();
    const auto table = mod::build_param_table(app.document, app.look());

    // Resolve the shared view range (zoom): invalid/stale = whole
    // clip. Every strip below maps through the same [v0, v1).
    double v0 = app.tl_v0, v1 = app.tl_v1;
    if (v1 - v0 < 1.0 || v1 > frame_count || v0 < 0.0) {
        v0 = 0.0;
        v1 = frame_count;
        app.tl_v0 = v0;
        app.tl_v1 = v1;
    }

    std::vector<LayoutNode*> rows;
    out.tl_deselect = arena.alloc<bool>();

    auto* ruler_user = arena.alloc<RulerUser>();
    ruler_user->app = &app;
    ruler_user->out = &out;
    ruler_user->frame_count = frame_count;
    ruler_user->playhead = playhead;
    ruler_user->fps = app.player.fps();
    ruler_user->v0 = v0;
    ruler_user->v1 = v1;
    // The trim/loop region is SEQUENCE structure; a scoped look's local
    // ruler runs its whole duration, no region.
    const bool seq_scope = !app.scope_is_look();
    ruler_user->trim_in = seq_scope
        ? std::min(app.sequence().trim_in,
                   frame_count ? frame_count - 1 : 0u)
        : 0u;
    ruler_user->trim_out =
        seq_scope && app.sequence().trim_out
            ? std::min(app.sequence().trim_out, frame_count)
            : frame_count;
    ruler_user->loop_in = seq_scope ? app.sequence().loop_in : 0u;
    ruler_user->loop_out = seq_scope ? app.sequence().loop_out : 0u;
    // Snap candidates for block drags: the static marks now, every
    // lane's block edges as they build below.
    app.tl_snap_edges.clear();
    app.tl_snap_edges.push_back({0.0, 0, 0});
    app.tl_snap_edges.push_back({static_cast<double>(playhead), 0, 0});
    app.tl_snap_edges.push_back(
        {static_cast<double>(ruler_user->trim_in), 0, 0});
    app.tl_snap_edges.push_back(
        {static_cast<double>(ruler_user->trim_out), 0, 0});
    if (ruler_user->loop_out > ruler_user->loop_in) {
        app.tl_snap_edges.push_back(
            {static_cast<double>(ruler_user->loop_in), 0, 0});
        app.tl_snap_edges.push_back(
            {static_cast<double>(ruler_user->loop_out), 0, 0});
    }
    if (seq_scope)
        for (const uint32_t m : app.sequence().markers)
            app.tl_snap_edges.push_back({static_cast<double>(m), 0, 0});
    // Filmstrips live in the BLOCKS now, not across the ruler — the
    // timeline shows an arrangement, not one clip.
    const float ruler_h = 20.0f;
    LayoutNode* ruler = make_node(arena, NodeKind::Leaf);
    ruler->width = SizeSpec::fill();
    ruler->height = SizeSpec::fixed(ruler_h);
    ruler->user = ruler_user;
    ruler->draw_fn = draw_ruler;
    ruler->hit_fn = hit_ruler;
    // The label column names the SCOPE, so whose local time is on
    // screen is never a guess (the canvas breadcrumb says the same thing).
    const char* scope_label = "project";
    if (app.scope_look != app.document.root_sequence) {
        const std::string& sname =
            app.scope_is_look() ? app.look().name : app.sequence().name;
        const std::string crumb =
            "> " + (sname.empty() ? std::string("scope") : sname);
        scope_label = arena.dup(crumb.c_str(), crumb.size());
    }
    rows.push_back(HStack(arena, {6.0f},
                          {SizedBox(arena, SizeSpec::fixed(150),
                                    SizeSpec::fixed(ruler_h),
                                    Label(arena, scope_label, small_dim)),
                           ruler}));

    // ---- BLOCK LANES: the scoped sequence's video lanes, one row per
    // lane, topmost lane composites last. A scoped LOOK is timeless -
    // no block lanes, its ruler and keyframe rows are the whole editor.
    refresh_asset_amp(app);
    if (seq_scope) {
        const doc::Sequence& seq = app.sequence();
        struct LaneBuild {
            std::vector<TlBlock> blocks;
            size_t layer_index = SIZE_MAX;
        };
        std::vector<LaneBuild> lanes;
        const uint64_t primary =
            app.document.primary_asset() ? app.document.primary_asset()->id
                                         : 0;
        // The clip a wrapper look reads, for the filmstrip: single clip
        // node bound to an asset, whatever effects ride it.
        auto wrapped_asset = [&](uint64_t target) -> uint64_t {
            const doc::Look* l = app.document.find_look(target);
            if (!l || l->layers.size() != 1 ||
                !doc::layer_is_clip(l->layers[0]))
                return 0;
            return l->layers[0].asset;
        };
        for (size_t li = 0; li < seq.tracks.size(); ++li) {
            const doc::SeqTrack& track = seq.tracks[li];
            lanes.push_back({});
            LaneBuild& lane = lanes.back();
            lane.layer_index = li;
            for (const doc::Placement& place : track.placements) {
                TlBlock b;
                b.layer_index = li;
                b.layer_id = track.id;
                b.placement_id = place.id;
                b.place = place;
                b.src_len = doc::source_length(app.document, place);
                const uint32_t end = doc::placement_end(place, b.src_len);
                b.t0 = static_cast<double>(place.t_in);
                // An unbounded block runs as long as the film does, not
                // into the buffer past it.
                b.t1 = end ? static_cast<double>(end)
                           : static_cast<double>(ruler_user->trim_out);
                if (b.t1 <= b.t0) b.t1 = b.t0 + 1.0;
                // Snap targets: real edges only - a virtual runs-to-end
                // edge is display, not a cut.
                app.tl_snap_edges.push_back({b.t0, place.id, place.link});
                if (end)
                    app.tl_snap_edges.push_back(
                        {static_cast<double>(end), place.id, place.link});
                // The block names its TARGET entity; the lane keeps its
                // own name on the label side.
                const char* bname = track.name.c_str();
                size_t bname_len = track.name.size();
                bool is_seq_target = false;
                if (const doc::Look* tl =
                        app.document.find_look(place.target)) {
                    bname = tl->name.c_str();
                    bname_len = tl->name.size();
                } else if (const doc::Sequence* tsq =
                               app.document.find_sequence(place.target)) {
                    bname = tsq->name.c_str();
                    bname_len = tsq->name.size();
                    is_seq_target = true;
                }
                b.name = arena.dup(bname, bname_len);
                b.kind = is_seq_target ? 2 : 0;
                const uint64_t strip_asset = wrapped_asset(place.target);
                // BLOCK outline: the picked block (Delete removes it,
                // razor narrows to its lane). The lane's left accent bar
                // carries the LANE pick; a card pick highlights the card.
                b.selected = app.sel_placement == place.id;
                // Sound rides AUDIO lanes: a video block draws no
                // silhouette - its linked audio block does. Filmstrip:
                // the primary asset's strip when the block plays its
                // wrapper look, mapped through the placement.
                if (strip_asset && strip_asset == primary &&
                    app.thumbs_tex) {
                    const media::AssetBundle* bd =
                        media::find_bundle(app.bundles, strip_asset);
                    if (bd && bd->frames) {
                        b.thumbs = app.thumbs_tex;
                        b.asset_frames = bd->frames;
                    }
                }
                b.staged = arena.alloc<doc::Placement>();
                *b.staged = place;
                b.changed = arena.alloc<bool>();
                b.released = arena.alloc<bool>();
                b.pressed = arena.alloc<bool>();
                out.block_stages.push_back(
                    {track.id, place.id, b.staged, b.changed, b.released});
                out.block_picks.push_back(
                    {li, track.id, place.id, b.pressed});
                lane.blocks.push_back(b);
            }
        }
        // Topmost layers composite last — draw their lanes on top.
        const float lane_h = 30.0f;
        for (size_t lane = lanes.size(); lane-- > 0;) {
            auto* user = arena.alloc<BlockLaneUser>();
            user->app = &app;
            user->out = &out;
            user->count = lanes[lane].blocks.size();
            user->layer_index = lanes[lane].layer_index;
            user->blocks = arena.alloc<TlBlock>(user->count);
            for (size_t i = 0; i < user->count; ++i)
                user->blocks[i] = lanes[lane].blocks[i];
            user->lane_index = std::min(lane, sizeof(app.tl_lane_ids) - 1);
            user->frame_count = frame_count;
            user->play_end = ruler_user->trim_out;
            user->playhead = playhead;
            user->v0 = v0;
            user->v1 = v1;
            LayoutNode* widget = make_node(arena, NodeKind::Leaf);
            widget->width = SizeSpec::fill();
            widget->height = SizeSpec::fixed(lane_h);
            widget->user = user;
            widget->draw_fn = draw_block_lane;
            widget->hit_fn = hit_block_lane;
            char lane_name[16];
            std::snprintf(lane_name, sizeof(lane_name), "v%zu", lane + 1);
            rows.push_back(HStack(
                arena, {6.0f},
                {SizedBox(arena, SizeSpec::fixed(150),
                          SizeSpec::fixed(lane_h),
                          Label(arena,
                                arena.dup(lane_name, std::strlen(lane_name)),
                                small_dim)),
                 widget}));
        }

        // ---- AUDIO LANES: real tracks holding real placements - the
        // silhouette is display, the lane is not. Order is display only
        // (summing commutes), so no reversal.
        for (size_t ai = 0; ai < seq.audio.size(); ++ai) {
            const doc::AudioTrack& track = seq.audio[ai];
            std::vector<TlBlock> ablocks;
            for (const doc::Placement& place : track.placements) {
                TlBlock b;
                b.layer_index = SIZE_MAX;
                b.layer_id = 0;   // no canvas card behind an audio block
                b.placement_id = place.id;
                b.place = place;
                b.src_len = doc::source_length(app.document, place);
                const uint32_t end =
                    doc::placement_end(place, b.src_len);
                b.t0 = static_cast<double>(place.t_in);
                b.t1 = end ? static_cast<double>(end)
                           : static_cast<double>(ruler_user->trim_out);
                if (b.t1 <= b.t0) b.t1 = b.t0 + 1.0;
                // Audio blocks pick and snap like video ones; they just
                // pick no LAYER (there is no video solo to show).
                b.selected = app.sel_placement == place.id;
                app.tl_snap_edges.push_back({b.t0, place.id, place.link});
                if (end)
                    app.tl_snap_edges.push_back(
                        {static_cast<double>(end), place.id, place.link});
                const char* bname = track.name.c_str();
                size_t bname_len = track.name.size();
                if (const doc::Look* tl =
                        app.document.find_look(place.target)) {
                    bname = tl->name.c_str();
                    bname_len = tl->name.size();
                } else if (const doc::Sequence* tsq =
                               app.document.find_sequence(place.target)) {
                    bname = tsq->name.c_str();
                    bname_len = tsq->name.size();
                }
                b.name = arena.dup(bname, bname_len);
                b.kind = 0;
                // The silhouette: the target entity's whole submix
                // through this placement's time map - a look sounds like
                // its clips in lockstep, a sequence like its tracks.
                std::vector<TlAmpSrc> srcs;
                const float pgain =
                    (track.mute || place.audio_mute)
                        ? 0.0f
                        : std::max(track.gain, 0.0f) *
                              std::max(place.audio_gain, 0.0f);
                if (pgain > 0.0f && place.target) {
                    for (const doc::ClipInstance& c :
                         doc::flatten_audio_sources(app.document,
                                                    place.target)) {
                        if (c.gain <= 0.0f) continue;
                        auto it = app.asset_amp.amp.find(c.asset);
                        if (it == app.asset_amp.amp.end()) continue;
                        TlAmpSrc s;
                        s.amp = &it->second;
                        s.outer = place;
                        s.inner = c;
                        s.nested = true;
                        s.gain = c.gain * pgain;
                        srcs.push_back(s);
                    }
                }
                if (!srcs.empty()) {
                    b.srcs = arena.alloc<TlAmpSrc>(srcs.size());
                    for (size_t s = 0; s < srcs.size(); ++s)
                        b.srcs[s] = srcs[s];
                    b.src_count = srcs.size();
                }
                b.staged = arena.alloc<doc::Placement>();
                *b.staged = place;
                b.changed = arena.alloc<bool>();
                b.released = arena.alloc<bool>();
                b.pressed = arena.alloc<bool>();
                out.block_stages.push_back(
                    {0, place.id, b.staged, b.changed, b.released});
                out.block_picks.push_back(
                    {SIZE_MAX, 0, place.id, b.pressed});
                ablocks.push_back(b);
            }

            auto* user = arena.alloc<BlockLaneUser>();
            user->app = &app;
            user->out = &out;
            user->count = ablocks.size();
            user->blocks = arena.alloc<TlBlock>(user->count);
            for (size_t i = 0; i < user->count; ++i)
                user->blocks[i] = ablocks[i];
            user->lane_index =
                std::min(lanes.size() + ai, sizeof(app.tl_lane_ids) - 1);
            user->frame_count = frame_count;
            user->play_end = ruler_user->trim_out;
            user->playhead = playhead;
            user->v0 = v0;
            user->v1 = v1;
            LayoutNode* widget = make_node(arena, NodeKind::Leaf);
            widget->width = SizeSpec::fill();
            widget->height = SizeSpec::fixed(lane_h);
            widget->user = user;
            widget->draw_fn = draw_block_lane;
            widget->hit_fn = hit_block_lane;

            // The lane label carries the per-track mixer: gain + mute.
            auto& aui = app.audio_ui[track.id];
            FrameUi::AudioTrackStage stage{};
            stage.track_id = track.id;
            stage.gain_staged = arena.alloc<float>();
            *stage.gain_staged = track.gain;
            stage.original = track.gain;
            stage.gain_changed = arena.alloc<bool>();
            stage.gain_released = arena.alloc<bool>();
            stage.mute_clicked = arena.alloc<bool>();
            SliderOpts gopts;
            gopts.format = "";
            gopts.out_changed = stage.gain_changed;
            gopts.out_released = stage.gain_released;
            out.audio_tracks.push_back(stage);
            const char* alabel =
                arena.dup(track.name.c_str(), track.name.size());
            rows.push_back(HStack(
                arena, {6.0f},
                {SizedBox(
                     arena, SizeSpec::fixed(150), SizeSpec::fixed(lane_h),
                     HStack(arena, {4.0f},
                            {Label(arena, alabel, small_dim),
                             SliderF(arena, stage.gain_staged, 0.0f, 2.0f,
                                     &aui.gain, gopts),
                             Chip(arena, "m", track.mute, &aui.mute,
                                  stage.mute_clicked,
                                  "mute this track")})),
                 widget}));
        }
    }

    // Analysis strip: loudness + onsets + cuts from the import analysis,
    // aligned to the same view range — keyframe against the material.
    if (app.has_analysis &&
        (!app.analysis.low.empty() || !app.analysis.onset.empty())) {
        if (app.strip_accel.stamp != app.analysis_stamp) {
            const mod::AnalysisCurves& c = app.analysis;
            AppState::StripAccel& a = app.strip_accel;
            const size_t n = std::max(
                {c.low.size(), c.mid.size(), c.high.size()});
            a.amp_frame.assign(n, 0.0f);
            for (size_t f = 0; f < n; ++f) {
                const uint32_t fi = static_cast<uint32_t>(f);
                a.amp_frame[f] = std::max({c.sample(c.low, fi),
                                           c.sample(c.mid, fi),
                                           c.sample(c.high, fi)});
            }
            auto block_max = [](const std::vector<float>& fr,
                                std::vector<float>& out) {
                out.assign((fr.size() + kStripBlock - 1) / kStripBlock,
                           0.0f);
                for (size_t f = 0; f < fr.size(); ++f)
                    out[f / kStripBlock] =
                        std::max(out[f / kStripBlock], fr[f]);
            };
            block_max(a.amp_frame, a.amp);
            block_max(c.onset, a.onset);
            block_max(c.cut, a.cut);
            a.stamp = app.analysis_stamp;
        }
        auto* strip_user = arena.alloc<AudioStripUser>();
        strip_user->app = &app;
        strip_user->curves = &app.analysis;
        strip_user->frame_count = frame_count;
        strip_user->playhead = playhead;
        strip_user->v0 = v0;
        strip_user->v1 = v1;
        LayoutNode* strip = make_node(arena, NodeKind::Leaf);
        strip->width = SizeSpec::fill();
        strip->height = SizeSpec::fixed(16.0f);
        strip->user = strip_user;
        strip->draw_fn = draw_audio_strip;
        rows.push_back(
            HStack(arena, {6.0f},
                   {SizedBox(arena, SizeSpec::fixed(150),
                             SizeSpec::fixed(16.0f),
                             Label(arena, "analysis", small_dim)),
                    strip}));
    }

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
                    if (!(target.effect_id & doc::kLayerParamBit) &&
                        target.effect_id == did)
                        return true;
                    break;
                case flow::NodeKind::Source:
                    if ((target.effect_id & doc::kLayerParamBit) &&
                        (target.effect_id & ~doc::kLayerParamBit) == did)
                        return true;
                    break;
                case flow::NodeKind::Group: {
                    size_t gli = 0;
                    if (find_group_by_id(app.look(), did, &gli))
                        for (const doc::EffectInstance& fx :
                             app.look().layers[gli].stack)
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
    for (const doc::KeyframeLane& lane : app.look().lanes) {
        // Human name ("Dither levels"), not the raw address — the path
        // stays serialize/display sugar elsewhere.
        std::string path;
        float min_v = 0.0f, max_v = 1.0f;
        for (const auto& e : table) {
            if (e.key == lane.target) {
                path = e.label.empty() ? e.path : e.label;
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
        user->v0 = v0;
        user->v1 = v1;
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
            app.look().lanes.empty()
                ? "no keyframe lanes - click the k dot next to a param"
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
    // Fatal failures (device loss, allocation) tell the user before the
    // process dies — the reason also persists to looks.log.
    set_fatal_sink(platform::show_fatal);

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

    // Baked MSDF fonts (fontbake) when the build staged them:
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
    app.cache_bytes = scan_cache_bytes();

    // Preview render thread: takes ownership of the preview
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

    // A fresh document starts MINIMAL (user demand — the demo
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
            if (source.extension() == L".json")
                open_project(app, source, window.get());
            else
                open_source(app, source);
        }
    } else {
        // Crash recovery for untitled sessions: work that only ever
        // lived in cache/untitled.autosave.json is offered back on the
        // next plain launch, then the file retires either way.
        const std::filesystem::path unsaved =
            executable_dir() / "cache" / "untitled.autosave.json";
        std::error_code ec;
        if (std::filesystem::exists(unsaved, ec)) {
            // Offered through the in-app modal on the first frames; the
            // resolution loads and/or retires the file.
            ConfirmDialog d;
            d.kind = ConfirmDialog::Kind::YesNo;
            d.action = ConfirmDialog::Action::RestoreUntitledAutosave;
            d.title = "crash recovery";
            d.text = "restore unsaved work from your last session?";
            d.primary = "restore";
            d.secondary = "discard";
            d.path = unsaved;
            app.confirm = std::move(d);
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
        Vec2 dropped_at{};   // physical client px (see FileDrop)
        // Timeline keyboard routing: Delete / Ctrl+C / Ctrl+V act
        // on keys when the mouse sits over the timeline region (last
        // frame's rect — layout has not run yet).
        const bool tl_hovered =
            app.tl_rect.w > 0.0f && input.mouse.x >= app.tl_rect.x &&
            input.mouse.x < app.tl_rect.right() &&
            input.mouse.y >= app.tl_rect.y &&
            input.mouse.y < app.tl_rect.bottom();
        float key_seek = -1.0f;   // keyboard playhead move (frames)
        // I / O set the trim band at the playhead (NLE in/out points);
        // they land in the ruler handles' channel post-frame.
        float key_trim_in = -1.0f, key_trim_out = -1.0f;
        int confirm_pick = 0;     // modal keyboard: 1 enter, 2/3 escape
        for (const platform::Event& e : events) {
            // Modal confirm owns the keyboard: enter affirms, escape backs
            // out; every other event (shortcuts, typing, drops, close)
            // stays inert until the dialog resolves.
            if (app.confirm.open()) {
                if (e.type == platform::Event::Type::KeyDown) {
                    if (e.key == platform::Key::Enter) {
                        confirm_pick = 1;
                    } else if (e.key == platform::Key::Escape) {
                        confirm_pick =
                            app.confirm.kind ==
                                    ConfirmDialog::Kind::SaveDiscard
                                ? 3
                                : 2;
                    }
                }
                continue;
            }
            switch (e.type) {
                case platform::Event::Type::CloseRequested:
                    // Unsaved-changes guard: closing never silently
                    // drops edits.
                    if (guard_unsaved_changes(
                            app, ConfirmDialog::Action::CloseApp))
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
                    // Key readout typing (timeline inline editor).
                    if (app.key_edit_mode != 0) {
                        if (((e.codepoint >= '0' && e.codepoint <= '9') ||
                             e.codepoint == '.' || e.codepoint == '-') &&
                            app.key_edit_buf.size() < 15)
                            app.key_edit_buf.push_back(
                                static_cast<char>(e.codepoint));
                        break;
                    }
                    // Rail value typing (slider type-in).
                    if (app.rail_edit_key.effect_id != 0) {
                        if (((e.codepoint >= '0' && e.codepoint <= '9') ||
                             e.codepoint == '.' || e.codepoint == '-') &&
                            app.rail_edit_buf.size() < 15)
                            app.rail_edit_buf.push_back(
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
                    // Text card string typing (inline edit).
                    if (app.text_edit_id) {
                        if (e.codepoint >= 32 && e.codepoint < 127 &&
                            app.text_edit_buf.size() < 64)
                            app.text_edit_buf.push_back(
                                static_cast<char>(e.codepoint));
                        break;
                    }
                    // Preset search typing: printable ASCII only.
                    if (app.preset_search_focus && e.codepoint >= 32 &&
                        e.codepoint < 127)
                        app.preset_filter.push_back(
                            static_cast<char>(e.codepoint));
                    // Add-node search (docs/flow_canvas.md).
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
                                doc::set_frame_title_command(app.scope_look,
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
                            if (find_group_by_id(app.look(),
                                                 app.group_rename_id,
                                                 &gli)) {
                                for (const doc::Group& gr :
                                     app.look().layers[gli].groups)
                                    if (gr.id == app.group_rename_id) {
                                        doc::Group edited = gr;
                                        edited.name = app.group_rename_buf;
                                        app.undo.execute(
                                            app.document,
                                            doc::set_group_props_command(app.scope_look,
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
                            if (find_effect_by_id(app.look(),
                                                  app.text_edit_id, &li,
                                                  &fi))
                                app.undo.execute(
                                    app.document,
                                    doc::set_effect_text_command(app.scope_look,
                                        li, fi, app.text_edit_buf));
                            app.text_edit_id = 0;
                            app.text_edit_buf.clear();
                        } else if (e.key == platform::Key::Escape) {
                            app.text_edit_id = 0;
                            app.text_edit_buf.clear();
                        }
                        break;
                    }
                    if (app.key_edit_mode != 0) {
                        // Inline key readout editor: Enter commits,
                        // Escape cancels, Backspace edits the buffer.
                        if (e.key == platform::Key::Backspace) {
                            if (!app.key_edit_buf.empty())
                                app.key_edit_buf.pop_back();
                        } else if (e.key == platform::Key::Enter) {
                            commit_key_edit(app);
                        } else if (e.key == platform::Key::Escape) {
                            app.key_edit_mode = 0;
                        }
                        break;
                    }
                    if (app.rail_edit_key.effect_id != 0) {
                        // Rail value type-in: the commit flag lands
                        // through the row's staged path on this frame's
                        // build.
                        if (e.key == platform::Key::Backspace) {
                            if (!app.rail_edit_buf.empty())
                                app.rail_edit_buf.pop_back();
                        } else if (e.key == platform::Key::Enter) {
                            app.rail_edit_commit = true;
                        } else if (e.key == platform::Key::Escape) {
                            app.rail_edit_key = {};
                            app.rail_edit_commit = false;
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
                        } else if (app.layer_sel || app.sel_placement) {
                            // Third state: drop the layer/block pick,
                            // the monitor returns to the film.
                            app.layer_sel = false;
                            app.sel_placement = 0;
                        } else if (app.scope_look !=
                                   app.document.root_sequence) {
                            // Nothing selected inside a nested scope: go
                            // up to the project, same as the breadcrumb.
                            app.scope_look = app.document.root_sequence;
                            app.selected_layer = 0;
                            app.layer_sel = false;
                            app.multi_sel.clear();
                            app.sel_wires.clear();
                            app.tl_v0 = app.tl_v1 = 0.0;
                            app.canvas_state.view_inited = false;
                        }
                        // Esc never quits — closing goes through
                        // the window X / Alt+F4 and the dirty guard.
                    } else if ((e.key == platform::Key::Delete ||
                                e.key == platform::Key::Backspace) &&
                               !e.repeat) {
                        // Over the timeline, Delete removes selected
                        // keys; else the picked BLOCK goes (with its
                        // whole link group - picture and sound leave
                        // together); else the canvas selection (texed).
                        if (!(tl_hovered &&
                              timeline_delete_selected_keys(app))) {
                            if (app.sel_placement && !app.scope_is_look() &&
                                doc::find_placement(app.sequence(),
                                                    app.sel_placement)) {
                                app.undo.execute(
                                    app.document,
                                    doc::remove_placement_command(
                                        app.sequence().id,
                                        app.sel_placement));
                                app.sel_placement = 0;
                            } else {
                                do_delete_sel = true;
                            }
                        }
                    }
                    else if (e.key == platform::Key::Space && !e.repeat)
                        toggle_play = true;
                    else if ((e.key == platform::Key::Comma ||
                              e.key == platform::Key::Period) &&
                             app.has_timeline()) {
                        // Frame step: , / . nudge the paused
                        // playhead one frame.
                        const uint32_t fc = app.player.frame_count();
                        const uint32_t at =
                            app.player.current_frame_index();
                        app.player.pause();
                        key_seek = static_cast<float>(
                            e.key == platform::Key::Comma
                                ? (at > 0 ? at - 1 : 0u)
                                : std::min(at + 1, fc ? fc - 1 : 0u));
                    } else if (e.key == platform::Key::Home &&
                               app.has_timeline()) {
                        key_seek = 0.0f;
                    } else if (e.key == platform::Key::End &&
                               app.has_timeline()) {
                        const uint32_t fc = app.player.frame_count();
                        key_seek = static_cast<float>(fc ? fc - 1 : 0u);
                    } else if ((e.key == platform::Key::LeftBracket ||
                                e.key == platform::Key::RightBracket) &&
                               app.has_timeline()) {
                        // [ / ] snap the playhead across keys + markers.
                        const double f = timeline_adjacent_mark(
                            app, e.key == platform::Key::RightBracket);
                        if (f >= 0.0) key_seek = static_cast<float>(f);
                    } else if ((e.key == platform::Key::I ||
                                e.key == platform::Key::O) &&
                               !e.repeat &&
                               (e.mods & (platform::kModCtrl |
                                          platform::kModAlt |
                                          platform::kModShift)) == 0 &&
                               app.has_timeline()) {
                        // I / O drop the in/out point at the playhead -
                        // the same trim band the ruler handles drag,
                        // ordered the same way they keep it. Sequence
                        // structure: inert inside a look.
                        if (!app.scope_is_look()) {
                            const uint32_t ph =
                                app.player.current_frame_index();
                            const uint32_t fc = app.player.frame_count();
                            const uint32_t cur_in =
                                app.sequence().trim_in;
                            const uint32_t cur_out =
                                app.sequence().trim_out
                                    ? app.sequence().trim_out
                                    : fc;
                            if (e.key == platform::Key::I)
                                key_trim_in = static_cast<float>(std::min(
                                    ph, cur_out ? cur_out - 1 : 0u));
                            else
                                key_trim_out =
                                    static_cast<float>(std::max(
                                        std::min(ph + 1, fc), cur_in + 1));
                        }
                    } else if (e.key == platform::Key::M && !e.repeat &&
                               !(e.mods & (platform::kModCtrl |
                                           platform::kModAlt |
                                           platform::kModShift)) &&
                               app.has_timeline()) {
                        // M toggles a marker at the playhead (sequence
                        // structure: inert inside a look).
                        if (!app.scope_is_look())
                            app.undo.execute(
                                app.document,
                                doc::toggle_marker_command(
                                    app.sequence().id,
                                    app.player.current_frame_index()));
                    }
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
                        // Over the timeline Ctrl+C copies keys.
                        if (!(tl_hovered &&
                              timeline_copy_selected_keys(app)))
                            do_copy = true;    // texed Ctrl+C
                    } else if (e.key == platform::Key::X && !e.repeat &&
                               (e.mods & platform::kModCtrl)) {
                        do_copy = true;    // texed Ctrl+X = copy + delete
                        do_cut = true;
                    } else if (e.key == platform::Key::V && !e.repeat &&
                               (e.mods & platform::kModCtrl)) {
                        if (!(tl_hovered && timeline_paste_keys(app)))
                            do_paste = true;   // texed Ctrl+V
                    } else if (e.key == platform::Key::A && !e.repeat &&
                               (e.mods & (platform::kModCtrl |
                                          platform::kModAlt)) == 0) {
                        app.ab_wipe = !app.ab_wipe;   // A/B wipe
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
                                find_effect_by_id(app.look(), did, &li,
                                                  &fi)) {
                                if (!any)
                                    app.undo.begin_group("Bypass");
                                any = true;
                                app.undo.execute(
                                    app.document,
                                    doc::set_bypass_command(app.scope_look,
                                        li, fi,
                                        !app.look().layers[li]
                                             .stack[fi].bypass));
                            } else if (kind == flow::NodeKind::Group &&
                                       find_group_by_id(app.look(), did,
                                                        &li)) {
                                for (const doc::Group& gr :
                                     app.look().layers[li].groups)
                                    if (gr.id == did) {
                                        if (!any)
                                            app.undo.begin_group(
                                                "Bypass");
                                        any = true;
                                        doc::Group edited = gr;
                                        edited.bypass = !edited.bypass;
                                        app.undo.execute(
                                            app.document,
                                            doc::set_group_props_command(app.scope_look,
                                                li, edited));
                                        break;
                                    }
                            }
                        }
                        if (any) app.undo.end_group();
                        else app.bypass_all = !app.bypass_all;
                    } else if (e.key == platform::Key::S && !e.repeat &&
                               (e.mods & (platform::kModCtrl |
                                          platform::kModAlt |
                                          platform::kModShift)) == 0) {
                        // S toggles drag snapping (the NLE magnet).
                        app.tl_snap = !app.tl_snap;
                        app.status =
                            app.tl_snap ? "snap on" : "snap off";
                    } else if (e.key == platform::Key::R && !e.repeat &&
                               (e.mods & (platform::kModCtrl |
                                          platform::kModAlt)) == 0) {
                        // RAZOR: cut at the playhead — the picked lane
                        // when its block sits under it, else every block
                        // under the playhead, one undo step. Sequence
                        // structure: inert inside a look (timeless).
                        if (app.scope_is_look()) break;
                        const doc::Sequence& rseq = app.sequence();
                        const uint32_t ph =
                            app.player.current_frame_index();
                        auto inside = [&](const doc::SeqTrack& t) {
                            for (const doc::Placement& place :
                                 t.placements) {
                                const uint32_t len = doc::source_length(
                                    app.document, place);
                                const uint32_t end =
                                    doc::placement_end(place, len);
                                if (ph > place.t_in && (!end || ph < end))
                                    return true;
                            }
                            return false;
                        };
                        // The pick narrows the cut: the picked LANE
                        // (block clicks set it). No pick cuts everything
                        // under the playhead.
                        std::vector<uint64_t> targets;
                        bool narrowed = false;
                        if (app.layer_sel &&
                            app.selected_layer < rseq.tracks.size()) {
                            const doc::SeqTrack& t =
                                rseq.tracks[app.selected_layer];
                            if (inside(t)) {
                                targets.push_back(t.id);
                                narrowed = true;
                            }
                        }
                        if (targets.empty() && !narrowed)
                            for (const doc::SeqTrack& t : rseq.tracks)
                                if (inside(t)) targets.push_back(t.id);
                        // Audio lanes cut too - linked partners already
                        // split with their video half (the group), and a
                        // half-open placement refuses a second cut, so
                        // running every track is idempotent. A narrowed
                        // cut stays on its lane.
                        std::vector<uint64_t> audio_targets;
                        if (!narrowed)
                            for (const doc::AudioTrack& t : rseq.audio)
                                audio_targets.push_back(t.id);
                        app.undo.begin_group("Razor");
                        for (const uint64_t lid : targets)
                            if (auto cmd = doc::razor_track_command(
                                    app.document, rseq.id, lid, ph))
                                app.undo.execute(app.document,
                                                 std::move(cmd));
                        for (const uint64_t tid : audio_targets)
                            if (auto cmd = doc::razor_audio_command(
                                    app.document, rseq.id, tid, ph))
                                app.undo.execute(app.document,
                                                 std::move(cmd));
                        app.undo.end_group();
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
                        // Envelope keypress trigger: live-mode
                        // only — wall-clock triggers are exempt from
                        // determinism there and only there.
                        if (app.live_mode) app.env_key_time = app.app_seconds;
                    }
                    break;
                case platform::Event::Type::FileDrop:
                    dropped_file = e.drop_path;
                    dropped_at = {e.mouse_x, e.mouse_y};
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

        // Finished import → bind the bundle (worker held off its readers).
        if (app.import && app.import->done.load()) {
            auto job = std::move(app.import);
            if (job->thread.joinable()) job->thread.join();
            if (job->result.ok) {
                render_worker.pause();
                render_worker.invalidate();
                struct ResumeGuard {
                    RenderWorker& w;
                    ~ResumeGuard() { w.resume(); }
                } import_resume{render_worker};
                if (job->import_only) {
                    // Generic import: the asset joins the browser and
                    // an asking clip node binds - nothing placed,
                    // nothing plays, no primary rebinding.
                    uint64_t asset_id = 0;
                    for (const doc::Asset& a : app.document.assets)
                        if (a.path == job->source.string())
                            asset_id = a.id;
                    app.undo.begin_group("Import");
                    if (!asset_id) {
                        doc::Asset asset = doc::make_asset(
                            app.document,
                            job->source.filename().string(),
                            job->source.string());
                        asset_id = asset.id;
                        app.undo.execute(
                            app.document,
                            doc::add_asset_command(std::move(asset)));
                    }
                    if (job->bind_layer) {
                        doc::Look* bl2 =
                            app.document.find_look(job->bind_look);
                        if (bl2)
                            for (doc::Layer& l : bl2->layers)
                                if (l.id == job->bind_layer &&
                                    doc::layer_is_clip(l)) {
                                    doc::Layer edited = l;
                                    edited.asset = asset_id;
                                    app.undo.execute(
                                        app.document,
                                        doc::set_layer_props_command(
                                            job->bind_look,
                                            std::move(edited)));
                                }
                    }
                    app.undo.end_group();
                    refresh_bundles(app);
                    app.status =
                        "imported " + job->source.filename().string();
                } else {
                    app.clip_name = job->source.filename().string();
                    app.mez_path = job->result.mez_path;
                    app.pcm_path = job->result.pcm_path;
                    refresh_bundles(app);
                    ensure_clip_placed(app, job->source);
                    load_clip_analysis(app);
                    app.player.set_looping(app.loop);
                    app.player.play();
                    app.status.clear();
                    // A rebuilt still bundle (cache cleared, new
                    // machine) comes back at the import default; the
                    // document's persisted duration wins.
                    if (primary_still_duration(app.document) > 0 &&
                        is_still_source(job->source))
                        set_still_frames(
                            app, primary_still_duration(app.document));
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
            if (job->progress.cancel.load())
                app.status =
                    "export cancelled: " + job->out_path.filename().string();
            else
                app.status = job->result.ok
                    ? "exported " + job->out_path.filename().string()
                    : "export failed: " + job->result.error;
            if (!app.export_queue.empty()) {
                AppState::QueuedExport next =
                    std::move(app.export_queue.front());
                app.export_queue.erase(app.export_queue.begin());
                app.export_job = start_export(
                    renderer->device(), shader_dir, next.bundles, next.pcm,
                    next.scope_pcm, next.doc, next.look_id,
                    next.has_analysis ? &next.analysis : nullptr,
                    next.out_path);
            }
        }

        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - last_time).count();
        last_time = now;
        smoothed_dt += (dt - smoothed_dt) * 0.05f;
        app.app_seconds += dt;   // live-mode mod clock
        // Draw callbacks that move at a RATE (the timeline's drag
        // auto-scroll) need real seconds, not frames — this app runs at
        // 180 fps on one machine and 60 on another.
        app.frame_dt = std::min(dt, 0.1f);

        gfx::FrameContext frame;
        if (!renderer->begin_frame(frame)) continue;
        // Render-thread bookkeeping: begin_frame waited this
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
        // Modal confirm: interacts with the live pointer NOW, then the
        // frame under the scrim gets dead input — no hover, no clicks,
        // no wheel, no capture churn.
        if (app.confirm.open()) {
            confirm_interact(app, input, font, viewport, confirm_pick,
                             window.get(), &running);
            input.buttons_down = 0;
            input.buttons_pressed = 0;
            input.buttons_released = 0;
            input.wheel_x = input.wheel_y = 0.0f;
            input.typed.clear();
            input.mouse = {-4096.0f, -4096.0f};
            input.mouse_delta = {};
            input.consumed = true;
        }
        // Timeline zoom/pan: pre-routed against LAST frame's region
        // rect so the lane scroll area cannot swallow the wheel first.
        app.tl_rect = app.tl_rect_accum;
        app.tl_rect_accum = {};
        timeline_zoom_wheel(app, input, app.player.frame_count());
        canvas.begin_frame(scale, {viewport.w, viewport.h});
        arena.reset();

        FrameUi frame_ui;
        // Keyboard playhead moves (frame step, Home/End, [ ]).
        if (key_seek >= 0.0f) frame_ui.seek_to = key_seek;

        ui::LayoutNode* preview = ui::make_node(arena, ui::NodeKind::Leaf);
        preview->width = ui::SizeSpec::fill();
        preview->height = ui::SizeSpec::fill();
        preview->draw_fn = draw_preview_frame;
        frame_ui.preview = preview;

        // A stale scope (undo removed the entity, a new project) repairs
        // to the root sequence - consumers pass the id raw after this.
        if (!app.document.find_look(app.scope_look) &&
            !app.document.find_sequence(app.scope_look))
            app.scope_look = app.document.root_sequence;
        // Selection stays in range across undo/redo; a pick whose
        // container vanished dies with it (never retargets). The rail
        // indexes the scoped look's layers or the sequence's lanes.
        const size_t rail_count = app.scope_is_look()
            ? app.look().layers.size()
            : app.sequence().tracks.size();
        if (rail_count == 0) {
            app.layer_sel = false;
        } else if (app.selected_layer >= rail_count) {
            app.selected_layer = rail_count - 1;
            app.layer_sel = false;
        }
        // A picked block whose placement went away (razor re-mints ids,
        // undo, scope change) drops the pick; the lane pick survives.
        if (app.sel_placement &&
            (app.scope_is_look() ||
             !doc::find_placement(app.sequence(), app.sel_placement)))
            app.sel_placement = 0;
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
            "open clip...",           "import media...",
            "open project...  (ctrl+o)",
            "save project  (ctrl+s)", "save project as...",
            "import preset...",       "export..."};
        static const char* kEditItems[] = {
            "undo  (ctrl+z)",      "redo  (ctrl+y)",
            "duplicate  (ctrl+d)", "group  (ctrl+g)",
            "ungroup  (ctrl+shift+g)", "select all  (ctrl+a)"};
        static const char* kViewItems[] = {
            "fit graph  (f)",   "find node...  (ctrl+f)",
            "cycle theme  (t)", "a/b wipe  (a)",
            "bypass fx  (b)",   "alpha checker"};
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
            {MenuButton(arena, "file", kFileItems, 7,
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
        if (app.has_timeline() && !app.live_mode) {
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
        // tabbed inspector. The transport is always there — the timeline
        // is the clock now, and an empty look still has one. The
        // blank-start prompt (docs/look.md phase 5) rides over the preview
        // only while the project is untouched.
        ui::LayoutNode* preview_cell = preview;
        const bool blank_start = app.document.assets.empty() &&
                                 app.document.revision == 0 && !app.import;
        {
            ui::StackOpts rc;
            rc.gap = 6.0f;
            rc.cross_align = ui::AlignMode::Stretch;
            rc.width = ui::SizeSpec::fill();
            rc.height = ui::SizeSpec::fill();
            preview_cell = ui::VStack(
                arena, rc, {preview, build_transport(arena, app, frame_ui)});
        }
        if (blank_start) {
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
            // The blank start offers both doors (docs/look.md): a look
            // FROM a clip, or an empty one to build in. Either way you
            // land in a look's editing view - that is home.
            frame_ui.new_look_clicked = arena.alloc<bool>();
            ui::LayoutNode* empty_ui = ui::VStack(
                arena, center,
                {ui::Label(arena, "drag a clip here", hint),
                 ui::Button(arena, "look from clip...", &app.open_big_button,
                            frame_ui.open_clicked, big),
                 ui::Button(arena, "new look", &app.new_look_button,
                            frame_ui.new_look_clicked, big)});
            ui::LayoutNode* z = ui::ZStack(arena, {preview_cell, empty_ui});
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
            case 1:
                if (frame_ui.import_media_clicked)
                    *frame_ui.import_media_clicked = true;
                break;
            case 2: do_open_project = true; break;
            case 3: do_save = true; break;
            case 4: do_save_as = true; break;
            case 5:
                if (frame_ui.preset_import_clicked)
                    *frame_ui.preset_import_clicked = true;
                break;
            case 6:
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
            case 3: app.ab_wipe = !app.ab_wipe; break;
            case 4: app.bypass_all = !app.bypass_all; break;
            case 5: app.alpha_checker = !app.alpha_checker; break;
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
        const size_t ui_layer = app.look().layers.empty()
            ? 0
            : std::min(app.selected_layer, app.look().layers.size() - 1);
        bool did_break = false;

        for (const ParamStage& stage : frame_ui.params) {
            if (*stage.changed && *stage.staged != stage.original &&
                stage.layer_index < app.look().layers.size() &&
                stage.fx_index <
                    app.look().layers[stage.layer_index].stack.size()) {
                // KEYED params: the lane sets the base every frame, so a
                // slider drag must move the key at the playhead (auto-
                // key) — writing the base reads as a dead slider.
                const doc::ParamKey pk{
                    app.look().layers[stage.layer_index]
                        .stack[stage.fx_index]
                        .id,
                    stage.param_index};
                const doc::KeyframeLane* keyed_lane = nullptr;
                for (const doc::KeyframeLane& lane : app.look().lanes)
                    if (lane.target == pk && !lane.keys.empty())
                        keyed_lane = &lane;
                if (keyed_lane) {
                    const double ph = app.has_timeline()
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
                                     doc::set_lane_command(app.scope_look,
                                         pk, std::move(keys2)),
                                     /*coalesce=*/true);
                } else {
                    app.undo.execute(
                        app.document,
                        doc::set_param_command(app.scope_look,stage.layer_index,
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
            if (row_layer >= app.look().layers.size() ||
                row.fx_index >= app.look().layers[row_layer].stack.size())
                continue;
            if (*row.bypass_changed) {
                app.undo.execute(app.document,
                                 doc::set_bypass_command(app.scope_look,row_layer,
                                                         row.fx_index,
                                                         *row.bypass_staged));
                structure_done = true;
            } else if (row.solo_changed && *row.solo_changed) {
                app.undo.execute(app.document,
                                 doc::set_solo_command(app.scope_look,row_layer,
                                                       row.fx_index,
                                                       *row.solo_staged));
                structure_done = true;
            } else if (row.duplicate && *row.duplicate) {
                // Duplicate: identical clone right below, with a
                // fresh id (and seed offset so "same settings" doesn't mean
                // "identical noise"). Spawns unwired.
                doc::EffectInstance copy =
                    app.look().layers[row_layer].stack[row.fx_index];
                copy.id = app.document.next_effect_id++;
                copy.seed = copy.id;
                app.undo.begin_group("Duplicate");
                app.undo.execute(app.document,
                                 doc::materialize_links_command(app.scope_look));
                app.undo.execute(app.document,
                                 doc::add_effect_command(app.scope_look,row_layer,
                                                         std::move(copy),
                                                         row.fx_index + 1));
                app.undo.end_group();
                structure_done = true;
            } else if (*row.remove) {
                app.undo.execute(
                    app.document,
                    doc::remove_effect_command(app.scope_look,row_layer, row.fx_index));
                structure_done = true;
            } else if (*row.up && row.fx_index > 0) {
                app.undo.execute(app.document,
                                 doc::move_effect_command(app.scope_look,row_layer,
                                                          row.fx_index,
                                                          row.fx_index - 1));
                structure_done = true;
            } else if (*row.down &&
                       row.fx_index + 1 <
                           app.look().layers[row_layer].stack.size()) {
                app.undo.execute(app.document,
                                 doc::move_effect_command(app.scope_look,row_layer,
                                                          row.fx_index,
                                                          row.fx_index + 1));
                structure_done = true;
            } else if (*row.group_toggle) {
                // "g": leave the group (dissolving it if now empty), join
                // the group above, or start a new group with the one above.
                auto& stack = app.look().layers[row_layer].stack;
                const doc::EffectInstance& fx = stack[row.fx_index];
                if (fx.group_id != 0) {
                    const uint64_t gid = fx.group_id;
                    app.undo.begin_group("Ungroup Effect");
                    app.undo.execute(app.document,
                                     doc::set_effect_group_command(app.scope_look,
                                         row_layer, row.fx_index, 0));
                    bool any = false;
                    for (const doc::EffectInstance& e : stack)
                        any = any || e.group_id == gid;
                    if (!any)
                        app.undo.execute(app.document,
                                         doc::ungroup_command(app.scope_look,row_layer,
                                                              gid));
                    app.undo.end_group();
                } else if (row.fx_index > 0) {
                    const uint64_t above = stack[row.fx_index - 1].group_id;
                    if (above != 0) {
                        app.undo.execute(app.document,
                                         doc::set_effect_group_command(app.scope_look,
                                             row_layer, row.fx_index,
                                             above));
                    } else {
                        doc::Group g = doc::make_group(app.document, "group");
                        app.undo.execute(app.document,
                                         doc::group_effects_command(app.scope_look,
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
                            if (find_effect_by_id(app.look(), did, &li,
                                                  &fi)) {
                                doc::EffectInstance copy =
                                    app.look().layers[li].stack[fi];
                                copy.node_x = nx;
                                copy.node_y = ny;
                                cb.effects.push_back(std::move(copy));
                                cb_fx.insert(did);
                            }
                            break;
                        case flow::NodeKind::ModSource:
                            if (const doc::ValueNode* vn =
                                    doc::find_value_node(app.look(),
                                                         did)) {
                                doc::ValueNode copy = *vn;
                                copy.node_x = nx;
                                copy.node_y = ny;
                                cb.value_nodes.push_back(copy);
                            }
                            break;
                        default:
                            break;
                    }
                    ox = std::min(ox, nx);
                    oy = std::min(oy, ny);
                }
                if (!cb.effects.empty() || !cb.value_nodes.empty()) {
                    const std::vector<doc::NodeLink> all_links =
                        app.look().links.empty()
                            ? doc::synthesize_links(app.look())
                            : app.look().links;
                    for (const doc::NodeLink& l : all_links)
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
                            layer_index_by_id(app.look(), fdoc);
                        if (idx >= 0) {
                            app.sel = {SelKind::LayerSource, fdoc};
                            app.selected_layer = static_cast<size_t>(idx);
                        }
                        break;
                    }
                    case flow::NodeKind::Effect:
                        if (find_effect_by_id(app.look(), fdoc, &li,
                                              &fi)) {
                            app.sel = {SelKind::Effect, fdoc};
                            app.selected_layer = li;
                        }
                        break;
                    case flow::NodeKind::ModSource:
                        app.sel = {SelKind::ModSource, fdoc};
                        break;
                    case flow::NodeKind::Group:
                        if (find_group_by_id(app.look(), fdoc, &li)) {
                            app.sel = {SelKind::Group, fdoc};
                            app.selected_layer = li;
                        }
                        break;
                    case flow::NodeKind::GroupIn:
                    case flow::NodeKind::GroupOut:
                        // Boundary nodes have no output of their own —
                        // deselect so the big preview shows the
                        // composite.
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
                                   fe.wire_kind, fe.wire_to_row};
                if (fe.wire_clicked_shift) {
                    auto it = std::find_if(
                        app.sel_wires.begin(), app.sel_wires.end(),
                        [&](const flow::Wire& s) {
                            return s.from == w.from && s.to == w.to &&
                                   s.kind == w.kind &&
                                   s.to_row == w.to_row;
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
                        if (find_group_by_id(app.look(), did, &gli))
                            for (const doc::Group& gr :
                                 app.look().layers[gli].groups)
                                if (gr.id == did)
                                    app.group_rename_buf = gr.name;
                        break;
                    }
                    case CtxAction::SavePreset: {
                        size_t gli = 0;
                        if (!find_group_by_id(app.look(), did, &gli))
                            break;
                        const doc::Group* group = nullptr;
                        for (const doc::Group& gr :
                             app.look().layers[gli].groups)
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
                            app.look(), gli, did);
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
                    case CtxAction::ResetParams: {
                        // Back to table defaults, one undo step.
                        size_t rli = 0, rfi = 0;
                        if (!find_effect_by_id(app.look(), did, &rli,
                                               &rfi))
                            break;
                        const doc::EffectInstance& rfx =
                            app.look().layers[rli].stack[rfi];
                        const doc::EffectInfo& rinfo =
                            doc::effect_info(rfx.type);
                        app.undo.begin_group("Reset Params");
                        for (uint32_t p = 0;
                             p < rinfo.param_count &&
                             p < rfx.params.size();
                             ++p)
                            if (rfx.params[p] !=
                                rinfo.params[p].default_value)
                                app.undo.execute(
                                    app.document,
                                    doc::set_param_command(app.scope_look,
                                        rli, rfi, static_cast<int>(p),
                                        rinfo.params[p].default_value));
                        app.undo.execute(app.document,
                                         doc::set_param_command(app.scope_look,
                                             rli, rfi, doc::kWetParam,
                                             1.0f));
                        app.undo.execute(app.document,
                                         doc::set_param_command(app.scope_look,
                                             rli, rfi, doc::kOpacityParam,
                                             1.0f));
                        app.undo.end_group();
                        break;
                    }
                    case CtxAction::CopyParams: {
                        size_t rli = 0, rfi = 0;
                        if (!find_effect_by_id(app.look(), did, &rli,
                                               &rfi))
                            break;
                        const doc::EffectInstance& rfx =
                            app.look().layers[rli].stack[rfi];
                        app.param_clip_valid = true;
                        app.param_clip_type = rfx.type;
                        app.param_clip_values = rfx.params;
                        app.param_clip_wet = rfx.wet;
                        app.param_clip_opacity = rfx.opacity;
                        app.status = "params copied";
                        break;
                    }
                    case CtxAction::PasteParams: {
                        size_t rli = 0, rfi = 0;
                        if (!app.param_clip_valid ||
                            !find_effect_by_id(app.look(), did, &rli,
                                               &rfi))
                            break;
                        const doc::EffectInstance& rfx =
                            app.look().layers[rli].stack[rfi];
                        if (rfx.type != app.param_clip_type) break;
                        app.undo.begin_group("Paste Params");
                        const size_t n =
                            std::min(rfx.params.size(),
                                     app.param_clip_values.size());
                        for (size_t p = 0; p < n; ++p)
                            if (rfx.params[p] != app.param_clip_values[p])
                                app.undo.execute(
                                    app.document,
                                    doc::set_param_command(app.scope_look,
                                        rli, rfi, static_cast<int>(p),
                                        app.param_clip_values[p]));
                        app.undo.execute(app.document,
                                         doc::set_param_command(app.scope_look,
                                             rli, rfi, doc::kWetParam,
                                             app.param_clip_wet));
                        app.undo.execute(
                            app.document,
                            doc::set_param_command(app.scope_look,
                                rli, rfi, doc::kOpacityParam,
                                app.param_clip_opacity));
                        app.undo.end_group();
                        break;
                    }
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
                    const doc::CanvasFrame* fr = nullptr;
                    for (const doc::CanvasFrame& f :
                         app.look().frames)
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
                                doc::set_node_pos_command(app.scope_look,
                                    nref, nrid, nd.x + dx, nd.y + dy),
                                /*coalesce=*/true);
                        }
                        app.undo.execute(app.document,
                                         doc::set_node_pos_command(app.scope_look,
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
                                         doc::set_node_pos_command(app.scope_look,
                                             ref, rid, mx + dx, my + dy),
                                         /*coalesce=*/true);
                    }
                } else if (ref_of(fe.moved, &ref, &rid)) {
                    app.undo.execute(app.document,
                                     doc::set_node_pos_command(app.scope_look,
                                         ref, rid, mvx, mvy),
                                     /*coalesce=*/true);
                }
                }
            }
            if (fe.move_released && !did_break) {
                app.undo.break_coalescing();
                did_break = true;
            }
            // Wire edits: every port goes through the link commands with
            // the cycle guard.
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
                    if (!find_group_by_id(app.look(), gid, &gli))
                        return 0;
                    uint64_t first = 0, last_id = 0, bind = 0;
                    const doc::Group* gr2 = nullptr;
                    for (const doc::Group& g :
                         app.look().layers[gli].groups)
                        if (g.id == gid) gr2 = &g;
                    for (const doc::EffectInstance& e :
                         app.look().layers[gli].stack)
                        if (e.group_id == gid) {
                            if (!first) first = e.id;
                            last_id = e.id;
                            // The boundary BINDINGS win when they name a
                            // live member (intermediaries).
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
                    for (const doc::Layer& sl : app.look().layers)
                        for (const doc::EffectInstance& e : sl.stack)
                            if (e.group_id == app.open_group)
                                scope_members.insert(e.id);
                const auto boundary_links = [&]() {
                    return app.look().links.empty()
                               ? doc::synthesize_links(app.look())
                               : app.look().links;
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
                    if (find_group_by_id(app.look(), app.open_group,
                                         &bgli))
                        for (const doc::Group& g :
                             app.look().layers[bgli].groups)
                            if (g.id == app.open_group) bgroup = &g;
                    app.undo.begin_group("Rewire Boundary");
                    if (is_kind(fe.disconnect_from,
                                flow::NodeKind::GroupIn) &&
                        !fe.connect_requested) {
                        for (const doc::NodeLink& l :
                             boundary_links())
                            if (l.to_port == 0 &&
                                scope_members.count(l.to) &&
                                !scope_members.count(l.from)) {
                                app.undo.execute(
                                    app.document,
                                    doc::disconnect_command(app.scope_look,l));
                                break;
                            }
                    } else if (is_kind(fe.disconnect_to,
                                       flow::NodeKind::GroupOut) &&
                               !fe.connect_requested) {
                        for (const doc::NodeLink& l :
                             boundary_links())
                            if (l.to_port == 0 &&
                                scope_members.count(l.from) &&
                                !scope_members.count(l.to)) {
                                app.undo.execute(
                                    app.document,
                                    doc::disconnect_command(app.scope_look,l));
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
                                doc::set_group_props_command(app.scope_look,bgli,
                                                             edited));
                            // The outer producer's link follows.
                            for (const doc::NodeLink& l :
                                 boundary_links())
                                if (l.to_port == 0 &&
                                    scope_members.count(l.to) &&
                                    !scope_members.count(l.from)) {
                                    app.undo.execute(
                                        app.document,
                                        doc::disconnect_command(app.scope_look,l));
                                    app.undo.execute(
                                        app.document,
                                        doc::connect_command(app.scope_look,
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
                                doc::set_group_props_command(app.scope_look,bgli,
                                                             edited));
                            // Every outer consumer's link follows (the
                            // composite link included).
                            for (const doc::NodeLink& l :
                                 boundary_links())
                                if (l.to_port == 0 &&
                                    scope_members.count(l.from) &&
                                    !scope_members.count(l.to)) {
                                    app.undo.execute(
                                        app.document,
                                        doc::disconnect_command(app.scope_look,l));
                                    app.undo.execute(
                                        app.document,
                                        doc::connect_command(app.scope_look,
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
                    if (fe.disconnect_port == 1) {
                        // Image matte: a plain port-1 link cut.
                        app.undo.execute(
                            app.document,
                            doc::disconnect_command(app.scope_look,
                                {resolve_src(fe.disconnect_from),
                                 doc_id_of(fe.disconnect_to), 1}));
                    } else {
                        app.undo.execute(
                            app.document,
                            doc::disconnect_command(app.scope_look,
                                {resolve_src(fe.disconnect_from),
                                 resolve_dst(fe.disconnect_to),
                                 fe.disconnect_port}));
                    }
                }
                if (fe.connect_requested) {
                    const uint64_t tdoc2 = doc_id_of(fe.connect_to);
                    if (fe.connect_port == 1) {
                        if (tag_kind(fe.connect_from) ==
                                flow::NodeKind::Source ||
                            tag_kind(fe.connect_from) ==
                                flow::NodeKind::Effect ||
                            tag_kind(fe.connect_from) ==
                                flow::NodeKind::Group) {
                            // Masks ARE images: the matte anchor is a
                            // plain port-1 image link — the engine reads
                            // the wired image's luma as the gate.
                            const uint64_t rf =
                                resolve_src(fe.connect_from);
                            if (!rf) {
                                app.status = "that group has no members";
                            } else if (doc::link_would_cycle(
                                           app.look(), rf, tdoc2)) {
                                app.status =
                                    "refused: that matte would loop";
                            } else {
                                app.undo.execute(
                                    app.document,
                                    doc::connect_command(app.scope_look,{rf, tdoc2, 1}));
                            }
                        } else {
                            app.status =
                                "only image nodes feed matte ports";
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
                        } else if (doc::link_would_cycle(app.look(),
                                                         rf, rt)) {
                            app.status =
                                "refused: that connection would loop";
                        } else {
                            app.undo.execute(app.document,
                                             doc::connect_command(app.scope_look,
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
                    if (!find_group_by_id(app.look(), gid, &gli))
                        return 0;
                    uint64_t first = 0, last_id = 0, bind = 0;
                    const doc::Group* gr2 = nullptr;
                    for (const doc::Group& g :
                         app.look().layers[gli].groups)
                        if (g.id == gid) gr2 = &g;
                    for (const doc::EffectInstance& e :
                         app.look().layers[gli].stack)
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
                } else if (doc::link_would_cycle(app.look(), wf, nid) ||
                           doc::link_would_cycle(app.look(), nid, wt)) {
                    app.status = "refused: that splice would loop";
                } else {
                    app.undo.begin_group("Splice Node");
                    if (app.look().links.empty())
                        app.undo.execute(
                            app.document,
                            doc::disconnect_command(app.scope_look,{0, 0, 9999}));
                    app.undo.execute(app.document,
                                     doc::disconnect_command(app.scope_look,
                                         {wf, wt, fe.splice_wire_port}));
                    app.undo.execute(app.document,
                                     doc::connect_command(app.scope_look,{wf, nid, 0}));
                    app.undo.execute(
                        app.document,
                        doc::connect_command(app.scope_look,
                            {nid, wt, fe.splice_wire_port}));
                    app.undo.end_group();
                    structure_done = true;
                }
            }

            // Value wiring: a value node's out wire dropped on a param
            // row ADDS a wire driving that param (fan-out; a repeat drop
            // is a no-op); dropped on a helper node's operand row it
            // wires that input (cycle-guarded). Effect rows: 0 = wet,
            // 1 = opacity, 2+p = params — the card build order. Rows
            // past the param span (the Text card's string row) are not
            // mod targets.
            if (fe.route_drop_requested && !structure_done &&
                tag_kind(fe.route_drop_from) == flow::NodeKind::ModSource &&
                fe.route_drop_row >= 0) {
                const uint64_t nid = tag_doc(fe.route_drop_from);
                doc::ParamKey key{0, -1};
                bool have_key = false;
                if (tag_kind(fe.route_drop_to) ==
                    flow::NodeKind::ModSource) {
                    const uint64_t to_id = tag_doc(fe.route_drop_to);
                    const doc::ValueNode* tn =
                        doc::find_value_node(app.look(), to_id);
                    const int which =
                        tn ? value_input_of_row(*tn, fe.route_drop_row)
                           : -1;
                    if (which >= 0) {
                        if (doc::value_reaches(app.look(), nid, to_id)) {
                            app.status = "value wire would cycle";
                        } else {
                            app.undo.execute(
                                app.document,
                                doc::wire_value_input_command(app.scope_look,
                                    to_id, which, nid));
                        }
                        structure_done = true;
                    }
                } else if (tag_kind(fe.route_drop_to) ==
                           flow::NodeKind::Effect) {
                    const int pi = fe.route_drop_row == 0
                        ? doc::kWetParam
                        : fe.route_drop_row == 1
                            ? doc::kOpacityParam
                            : fe.route_drop_row - 2;
                    size_t rli = 0, rfi = 0;
                    if (pi < 0 ||
                        (find_effect_by_id(app.look(),
                                           tag_doc(fe.route_drop_to), &rli,
                                           &rfi) &&
                         pi < static_cast<int>(
                                  doc::effect_info(
                                      app.look().layers[rli]
                                          .stack[rfi]
                                          .type)
                                      .param_count))) {
                        key = {tag_doc(fe.route_drop_to), pi};
                        have_key = true;
                    }
                } else if (tag_kind(fe.route_drop_to) ==
                           flow::NodeKind::Source) {
                    // Source rows alias layer params via the shared map.
                    const int idx = layer_index_by_id(
                        app.look(), tag_doc(fe.route_drop_to));
                    if (idx >= 0) {
                        const doc::Layer& sl =
                            app.look().layers[static_cast<size_t>(idx)];
                        const std::vector<int> map = layer_mod_row_map(sl);
                        const int row = fe.route_drop_row;
                        if (row < static_cast<int>(map.size()) &&
                            map[static_cast<size_t>(row)] >= 0) {
                            key = {sl.id | doc::kLayerParamBit,
                                   map[static_cast<size_t>(row)]};
                            have_key = true;
                        }
                    }
                } else if (tag_kind(fe.route_drop_to) ==
                           flow::NodeKind::Group) {
                    // Face rows are member-param ALIASES: the drop
                    // targets the row'th valid exposed key.
                    const uint64_t gid = tag_doc(fe.route_drop_to);
                    size_t gli = 0;
                    if (find_group_by_id(app.look(), gid, &gli)) {
                        const doc::Layer& gl = app.look().layers[gli];
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
                                    key = k;
                                    have_key = true;
                                    break;
                                }
                            }
                            break;
                        }
                    }
                }
                if (have_key && !structure_done) {
                    // Same wire again = no-op; a different node's drop
                    // REPLACES the param's wire (add_route_command).
                    bool dup = false;
                    for (const doc::ModRoute& r : app.look().mod_routes)
                        dup = dup || (r.node == nid && r.target == key);
                    if (!dup) {
                        doc::ModRoute route;
                        route.id = app.document.next_route_id++;
                        route.node = nid;
                        route.target = key;
                        app.undo.execute(app.document,
                                         doc::add_route_command(app.scope_look,route));
                    }
                    structure_done = true;
                }
            }

            // Frame corner resize: streamed as one coalesced command per
            // gesture; rename opens the inline title edit.
            if (fe.frame_resized)
                app.undo.execute(app.document,
                                 doc::set_frame_bounds_command(app.scope_look,
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
                for (const doc::CanvasFrame& f : app.look().frames)
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
                if (find_group_by_id(app.look(), gid, &gli))
                    for (const doc::Group& gr :
                         app.look().layers[gli].groups)
                        if (gr.id == gid) app.group_rename_buf = gr.name;
            }
            // Text card title double-click: edit its string through the
            // shared inline editor.
            if (fe.text_edit) {
                const uint64_t tid = tag_doc(fe.text_edit);
                app.text_edit_id = tid;
                app.text_edit_buf.clear();
                size_t tli = 0, tfi = 0;
                if (find_effect_by_id(app.look(), tid, &tli, &tfi))
                    app.text_edit_buf =
                        app.look().layers[tli].stack[tfi].text;
            }
            // Frame colour dot: cycles none → palette hues → none.
            for (size_t f = 0; f < flow_ui.graph->frame_count; ++f) {
                const flow::FrameBox& fb = flow_ui.graph->frames[f];
                if (fb.color_clicked && *fb.color_clicked) {
                    app.undo.execute(app.document,
                                     doc::set_frame_color_command(app.scope_look,
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
                        //: resolve the row to its exposed key so
                        // typed values commit through the same direct
                        // path as effect rows.
                        doc::ParamKey face_key{0, 0};
                        bool face = false;
                        if (tag_kind(nd.id) == flow::NodeKind::Group) {
                            const uint64_t gid = tag_doc(nd.id);
                            size_t gli = 0;
                            if (find_group_by_id(app.look(), gid,
                                                 &gli)) {
                                const doc::Layer& gl =
                                    app.look().layers[gli];
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
                                 app.look().lanes)
                                if (lane.target == pk &&
                                    !lane.keys.empty()) {
                                    keyed = true;
                                    keys2 = lane.keys;
                                }
                            size_t li = 0, fi = 0;
                            if (keyed) {
                                const double ph = app.has_timeline()
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
                                    doc::set_lane_command(app.scope_look,
                                        pk, std::move(keys2)));
                                applied = true;
                            } else if (find_effect_by_id(app.look(),
                                                         pk.effect_id,
                                                         &li, &fi)) {
                                app.undo.execute(
                                    app.document,
                                    doc::set_param_command(app.scope_look,li, fi, pi,
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

            // Frame removal: X on a frame's title strip.
            for (size_t f = 0;
                 f < flow_ui.graph->frame_count && !structure_done; ++f) {
                if (flow_ui.graph->frames[f].remove_clicked &&
                    *flow_ui.graph->frames[f].remove_clicked) {
                    app.undo.execute(app.document,
                                     doc::remove_frame_command(app.scope_look,
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
                                                app.look(), fdoc3,
                                                &fli, &ffi)) {
                                            app.sel = {SelKind::Effect,
                                                       fdoc3};
                                            app.selected_layer = fli;
                                        }
                                        break;
                                    case flow::NodeKind::ModSource:
                                        app.sel = {SelKind::ModSource,
                                                   fdoc3};
                                        break;
                                    case flow::NodeKind::Group:
                                        if (find_group_by_id(
                                                app.look(), fdoc3,
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
                        doc::CanvasFrame fr;
                        fr.id = app.document.next_effect_id++;
                        fr.title = "frame " + std::to_string(fr.id);
                        fr.x = app.canvas_state.add_gx;
                        fr.y = app.canvas_state.add_gy;
                        app.undo.execute(
                            app.document,
                            doc::add_frame_command(app.scope_look,std::move(fr)));
                        structure_done = true;
                    }
                    if (src_kind >= 0 &&
                        app.look().layers.size() < doc::kMaxLayers) {
                        // Source node at the click point. Spawns
                        // UNWIRED — materialize freezes the graph
                        // first so synthesis cannot chain it in. The very
                        // first source in an empty document still wires
                        // (materialize no-ops on zero layers).
                        doc::Layer nl = doc::make_layer(
                            app.document, kSrcAddKinds[src_kind]);
                        nl.node_x = app.canvas_state.add_gx;
                        nl.node_y = app.canvas_state.add_gy;
                        const uint64_t lid = nl.id;
                        app.undo.begin_group("Add Source");
                        // A look source needs a look to play: mint an
                        // empty one and target the placement at it, in
                        // the same undo step.
                        doc::Look fresh_look;
                        if (kSrcAddIsLook[src_kind]) {
                            fresh_look = doc::make_look(app.document, "");
                            nl.target = fresh_look.id;
                            nl.name = fresh_look.name;
                            app.undo.execute(
                                app.document,
                                doc::add_look_command(std::move(fresh_look)));
                        }
                        app.undo.execute(app.document,
                                         doc::materialize_links_command(app.scope_look));
                        app.undo.execute(
                            app.document,
                            doc::add_layer_command(app.scope_look,
                                std::move(nl),
                                app.look().layers.size()));
                        app.undo.end_group();
                        app.sel = {SelKind::LayerSource, lid};
                        app.multi_sel.assign(
                            1, flow::node_id(flow::NodeKind::Source, lid));
                        structure_done = true;
                    }
                    if (val_kind >= 0 && !structure_done) {
                        // Value node at the click point — drag its out
                        // port onto a param row (or a helper's operand
                        // row) to drive something.
                        doc::ValueNode node;
                        node.id = app.document.next_route_id++;
                        node.source.type = kValAddTypes[val_kind];
                        node.node_x = app.canvas_state.add_gx;
                        node.node_y = app.canvas_state.add_gy;
                        const uint64_t nid = node.id;
                        app.undo.execute(app.document,
                                         doc::add_value_node_command(app.scope_look,node));
                        app.sel = {SelKind::ModSource, nid};
                        app.multi_sel.assign(
                            1, flow::node_id(flow::NodeKind::ModSource,
                                             nid));
                        structure_done = true;
                    }
                    if (chosen != doc::EffectType::Count) {
                        app.undo.begin_group("Add Node");
                        // Freeze wiring FIRST: everything added in
                        // this gesture spawns unwired.
                        app.undo.execute(app.document,
                                         doc::materialize_links_command(app.scope_look));
                        if (app.look().layers.empty()) {
                            // v3: effects need a storage bag, never a
                            // user-facing precondition — conjure the
                            // clip host silently.
                            doc::Layer host = doc::make_layer(
                                app.document,
                                doc::LayerSourceKind::Clip);
                            app.undo.execute(app.document,
                                             doc::add_layer_command(app.scope_look,
                                                 std::move(host), 0));
                        }
                        size_t li = std::min(
                            app.selected_layer,
                            app.look().layers.size() - 1);
                        auto fx = doc::make_effect(app.document, chosen);
                        fx.node_x = app.canvas_state.add_gx;
                        fx.node_y = app.canvas_state.add_gy;
                        // Scoped view: the new effect joins the open
                        // group at the end of its member span.
                        size_t insert_at = SIZE_MAX;   // SIZE_MAX = end
                        if (app.open_group) {
                            size_t gli2 = 0;
                            if (find_group_by_id(app.look(),
                                                 app.open_group, &gli2)) {
                                li = gli2;
                                fx.group_id = app.open_group;
                                const auto& stk =
                                    app.look().layers[li].stack;
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
                        app.undo.execute(
                            app.document,
                            doc::add_effect_command(app.scope_look,
                                li, std::move(fx),
                                insert_at == SIZE_MAX
                                    ? app.look().layers[li]
                                          .stack.size()
                                    : insert_at));
                        if (splice) {
                            app.undo.execute(
                                app.document,
                                doc::disconnect_command(app.scope_look,
                                    {splice_doc(
                                         app.canvas_state.splice_from),
                                     splice_doc(
                                         app.canvas_state.splice_to),
                                     app.canvas_state.splice_port}));
                            app.undo.execute(
                                app.document,
                                doc::connect_command(app.scope_look,
                                    {splice_doc(
                                         app.canvas_state.splice_from),
                                     new_id, 0}));
                            app.undo.execute(
                                app.document,
                                doc::connect_command(app.scope_look,
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
                if (app.look().layers.empty()) {
                    app.sel = {SelKind::AddLayer, 0};
                } else {
                    const size_t li =
                        std::min(app.selected_layer,
                                 app.look().layers.size() - 1);
                    app.sel = {SelKind::AddEffect,
                               app.look().layers[li].id};
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
                    if (find_effect_by_id(app.look(), app.sel.id, &li,
                                          &fi)) {
                        doc::EffectInstance copy =
                            app.look().layers[li].stack[fi];
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
                        // Materialize first so the copy spawns unwired
                        // (matches the popup add, v5.8).
                        app.undo.execute(app.document,
                                         doc::materialize_links_command(app.scope_look));
                        app.undo.execute(
                            app.document,
                            doc::add_effect_command(app.scope_look,
                                li, std::move(copy),
                                app.open_group
                                    ? fi + 1
                                    : app.look().layers[li]
                                          .stack.size()));
                        app.undo.end_group();
                        app.sel = {SelKind::Effect, nid};
                        app.multi_sel.assign(
                            1, flow::node_id(flow::NodeKind::Effect, nid));
                        structure_done = true;
                    }
                } else if (app.sel.kind == SelKind::ModSource) {
                    // Duplicate keeps helper inputs (upstream nodes are
                    // shared) but feeds no params until wired.
                    if (const doc::ValueNode* vn = doc::find_value_node(
                            app.look(), app.sel.id)) {
                        doc::ValueNode copy = *vn;
                        copy.id = app.document.next_route_id++;
                        node_pos_of(
                            flow::node_id(flow::NodeKind::ModSource,
                                          app.sel.id),
                            &px, &py);
                        copy.node_x = px + 26.0f;
                        copy.node_y = py + 26.0f;
                        const uint64_t nid = copy.id;
                        app.undo.execute(app.document,
                                         doc::add_value_node_command(app.scope_look,copy));
                        app.sel = {SelKind::ModSource, nid};
                        app.multi_sel.assign(
                            1, flow::node_id(
                                   flow::NodeKind::ModSource, nid));
                        structure_done = true;
                    }
                } else if (app.sel.kind != SelKind::None) {
                    app.status =
                        "duplicate: select an effect or value node";
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
                if (find_group_by_id(app.look(), gid, &gli)) {
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
            // Enter a nested ENTITY (double-click its ref card): the
            // whole editing scope moves — canvas and timeline together,
            // one state. Pure view state; the ref and the target are
            // untouched.
            if (fe.look_open) {
                const uint64_t lid = tag_doc(fe.look_open);
                uint64_t target = 0;
                for (const doc::Layer& l : app.look().layers)
                    if (l.id == lid && doc::layer_is_nested(l))
                        target = l.target;
                if (target && (app.document.find_look(target) ||
                               app.document.find_sequence(target))) {
                    app.scope_look = target;
                    app.open_group = 0;
                    app.sel = {};
                    app.selected_layer = 0;
                    app.layer_sel = false;
                    app.multi_sel.clear();
                    app.sel_wires.clear();
                    app.canvas_state.view_inited = false;
                    app.saved_view_valid = false;
                    // The zoom window belongs to the look you left: a
                    // 60-frame view over a 5000-frame project is a
                    // keyhole. Re-resolve to the new span.
                    app.tl_v0 = app.tl_v1 = 0.0;
                    app.undo.break_coalescing();
                    const doc::Look* tl2 = app.document.find_look(target);
                    app.status = "editing " +
                                 (tl2 ? tl2->name
                                      : app.document.sequence(target).name);
                }
            }
            if (fe.crumb_clicked) {
                app.multi_sel.clear();
                app.sel_wires.clear();
                if (app.open_group) {
                    app.open_group = 0;
                    if (app.saved_view_valid) {
                        app.canvas_state.pan_x = app.saved_pan_x;
                        app.canvas_state.pan_y = app.saved_pan_y;
                        app.canvas_state.zoom = app.saved_zoom;
                        app.saved_view_valid = false;
                    } else {
                        app.canvas_state.view_inited = false;
                    }
                } else if (app.scope_look != app.document.root_sequence) {
                    // Up one level: back to the project timeline.
                    app.scope_look = app.document.root_sequence;
                    app.sel = {};
                    app.selected_layer = 0;
                    app.layer_sel = false;
                    app.tl_v0 = app.tl_v1 = 0.0;
                    app.canvas_state.view_inited = false;
                    app.undo.break_coalescing();
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
            // NEST (docs/look.md): Ctrl+G over selected SOURCES is the
            // one-level-up sibling of grouping effects — the blocks leave
            // this look and become one, with a single instance in their
            // place. Effects group; blocks nest.
            if (do_group && !structure_done) {
                std::vector<uint64_t> block_ids;
                for (const uint64_t cid : app.multi_sel) {
                    if (tag_kind(cid) != flow::NodeKind::Source) continue;
                    const uint64_t did = tag_doc(cid);
                    if (layer_index_by_id(app.look(), did) >= 0)
                        block_ids.push_back(did);
                }
                if (!block_ids.empty() && app.scope_is_look()) {
                    const uint64_t parent = app.scope_look;
                    if (auto cmd = doc::nest_layers_command(
                            app.document, parent, block_ids, "")) {
                        app.undo.execute(app.document, std::move(cmd));
                        // Select the ref that replaced them.
                        for (const doc::Layer& l : app.look().layers) {
                            if (!doc::layer_is_nested(l) || !l.target ||
                                l.target == parent)
                                continue;
                            bool fresh = true;
                            for (const uint64_t b : block_ids)
                                if (b == l.id) fresh = false;
                            if (!fresh) continue;
                            app.sel = {SelKind::LayerSource, l.id};
                            app.multi_sel.assign(
                                1, flow::node_id(flow::NodeKind::Source,
                                                 l.id));
                        }
                        app.status = "nested " +
                                     std::to_string(block_ids.size()) +
                                     " source(s) into a look";
                    } else {
                        app.status = "nest: nothing to nest";
                    }
                    structure_done = true;
                }
            }
            if (do_group && !structure_done) {
                size_t gli = SIZE_MAX, lo = SIZE_MAX, hi = 0;
                int count = 0;
                for (const uint64_t cid : app.multi_sel) {
                    if (tag_kind(cid) != flow::NodeKind::Effect) continue;
                    size_t li = 0, fi = 0;
                    if (!find_effect_by_id(app.look(), tag_doc(cid), &li,
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
                                     doc::group_effects_command(app.scope_look,
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
                    if (find_effect_by_id(app.look(), app.sel.id, &li,
                                          &fi))
                        gid = app.look().layers[li].stack[fi].group_id;
                }
                size_t gli = 0;
                if (gid && find_group_by_id(app.look(), gid, &gli)) {
                    app.undo.execute(app.document,
                                     doc::ungroup_command(app.scope_look,gli, gid));
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
                std::vector<uint64_t> pasted;
                app.undo.begin_group("Paste");
                // Materialize BEFORE any add: pasted nodes spawn
                // wired only to each other — materializing after the adds
                // baked synthesis-chained links alongside the clipboard's
                // (the fan-in bug).
                if (!cb.effects.empty())
                    app.undo.execute(app.document,
                                     doc::materialize_links_command(app.scope_look));
                if (!cb.effects.empty() && app.look().layers.empty()) {
                    doc::Layer host = doc::make_layer(
                        app.document, doc::LayerSourceKind::Clip);
                    app.undo.execute(app.document,
                                     doc::add_layer_command(app.scope_look,
                                         std::move(host), 0));
                }
                uint64_t first_fx = 0;
                if (!cb.effects.empty()) {
                    const size_t li =
                        std::min(app.selected_layer,
                                 app.look().layers.size() - 1);
                    for (const doc::EffectInstance& f0 : cb.effects) {
                        doc::EffectInstance fx = f0;
                        fx.id = app.document.next_effect_id++;
                        fx.group_id = 0;
                        fx.node_x = f0.node_x - cb.origin_x + px0;
                        fx.node_y = f0.node_y - cb.origin_y + py0;
                        fx_remap[f0.id] = fx.id;
                        if (!first_fx) first_fx = fx.id;
                        pasted.push_back(flow::node_id(
                            flow::NodeKind::Effect, fx.id));
                        app.undo.execute(
                            app.document,
                            doc::add_effect_command(app.scope_look,
                                li, std::move(fx),
                                app.look().layers[li].stack.size()));
                    }
                    for (const doc::NodeLink& l : cb.links)
                        app.undo.execute(app.document,
                                         doc::connect_command(app.scope_look,
                                             {fx_remap[l.from],
                                              fx_remap[l.to],
                                              l.to_port}));
                }
                // Value nodes remint; helper inputs remap when both
                // ends were copied, else keep pointing at the shared
                // original. Wires onto params are not copied.
                std::unordered_map<uint64_t, uint64_t> vn_remap;
                for (const doc::ValueNode& n0 : cb.value_nodes)
                    vn_remap[n0.id] = app.document.next_route_id++;
                for (const doc::ValueNode& n0 : cb.value_nodes) {
                    doc::ValueNode n = n0;
                    n.id = vn_remap[n0.id];
                    if (auto ita = vn_remap.find(n.in_a);
                        ita != vn_remap.end())
                        n.in_a = ita->second;
                    if (auto itb = vn_remap.find(n.in_b);
                        itb != vn_remap.end())
                        n.in_b = itb->second;
                    n.node_x = n0.node_x - cb.origin_x + px0;
                    n.node_y = n0.node_y - cb.origin_y + py0;
                    pasted.push_back(flow::node_id(
                        flow::NodeKind::ModSource, n.id));
                    app.undo.execute(app.document,
                                     doc::add_value_node_command(app.scope_look,n));
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
                                     doc::set_node_pos_command(app.scope_look,
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
                                     doc::set_node_pos_command(app.scope_look,ref, rid,
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
                if (!find_group_by_id(app.look(), gid, &gli))
                    return false;
                if (own_undo_group) app.undo.begin_group("Delete Group");
                bool removing = true;
                while (removing) {
                    removing = false;
                    const auto& stk = app.look().layers[gli].stack;
                    for (size_t s = 0; s < stk.size(); ++s)
                        if (stk[s].group_id == gid) {
                            app.undo.execute(
                                app.document,
                                doc::remove_effect_command(app.scope_look,gli, s));
                            removing = true;
                            break;
                        }
                }
                app.undo.execute(app.document,
                                 doc::ungroup_command(app.scope_look,gli, gid));
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
                    if (!find_group_by_id(app.look(), gid, &gli))
                        return 0;
                    uint64_t first = 0, last_id = 0, bind = 0;
                    const doc::Group* gr2 = nullptr;
                    for (const doc::Group& g :
                         app.look().layers[gli].groups)
                        if (g.id == gid) gr2 = &g;
                    for (const doc::EffectInstance& e :
                         app.look().layers[gli].stack)
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
                    for (const doc::Layer& sl : app.look().layers)
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
                            app.look().links.empty()
                                ? doc::synthesize_links(app.look())
                                : app.look().links;
                        for (const doc::NodeLink& l : links) {
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
                                    doc::disconnect_command(app.scope_look,l));
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
                    if (sw.kind == 0 || sw.kind == 3) {
                        app.undo.execute(
                            app.document,
                            doc::disconnect_command(app.scope_look,
                                {wf, wt, sw.kind == 3 ? 2u : 0u}));
                    } else if (sw.kind == 1) {
                        // Image matte: a plain port-1 link cut.
                        app.undo.execute(
                            app.document,
                            doc::disconnect_command(app.scope_look,{wf, wt, 1}));
                    } else if (sw.kind == 2) {
                        // Value wires: into a helper's operand row =
                        // unwire that input; onto a param row = remove
                        // the route(s) this wire drew.
                        const uint64_t src_node = tag_doc(sw.from);
                        if (is_cid_kind(sw.to,
                                        flow::NodeKind::ModSource)) {
                            const uint64_t to_id = tag_doc(sw.to);
                            const doc::ValueNode* tn =
                                doc::find_value_node(app.look(), to_id);
                            for (int which = 0; tn && which < 2;
                                 ++which) {
                                const uint64_t in_id =
                                    which == 0 ? tn->in_a : tn->in_b;
                                if (in_id != src_node) continue;
                                if (sw.to_row >= 0 &&
                                    value_row_of_input(*tn, which) !=
                                        sw.to_row)
                                    continue;
                                app.undo.execute(
                                    app.document,
                                    doc::wire_value_input_command(app.scope_look,
                                        to_id, which, 0));
                                tn = doc::find_value_node(app.look(),
                                                          to_id);
                            }
                        } else {
                            // Match each route's drawn anchor against
                            // the clicked wire (mirrors build_flow).
                            std::vector<uint64_t> cut;
                            for (const doc::ModRoute& r :
                                 app.look().mod_routes) {
                                if (r.node != src_node) continue;
                                uint64_t cid = 0;
                                int row = -1;
                                if (r.target.effect_id &
                                    doc::kLayerParamBit) {
                                    const uint64_t lid =
                                        r.target.effect_id &
                                        ~doc::kLayerParamBit;
                                    const int idx = layer_index_by_id(
                                        app.look(), lid);
                                    if (idx < 0) continue;
                                    cid = flow::node_id(
                                        flow::NodeKind::Source, lid);
                                    const std::vector<int> map =
                                        layer_mod_row_map(
                                            app.look().layers
                                                [static_cast<size_t>(
                                                    idx)]);
                                    for (size_t rr = 0; rr < map.size();
                                         ++rr)
                                        if (map[rr] ==
                                            r.target.param_index)
                                            row = static_cast<int>(rr);
                                } else if (r.target.effect_id == 0) {
                                    if (r.target.param_index == 0)
                                        cid = flow::kOutNodeId;
                                } else {
                                    size_t rli = 0, rfi = 0;
                                    if (!find_effect_by_id(
                                            app.look(),
                                            r.target.effect_id, &rli,
                                            &rfi))
                                        continue;
                                    const doc::EffectInstance& tfx =
                                        app.look().layers[rli]
                                            .stack[rfi];
                                    if (tfx.group_id &&
                                        tfx.group_id != app.open_group) {
                                        cid = flow::node_id(
                                            flow::NodeKind::Group,
                                            tfx.group_id);
                                    } else {
                                        cid = flow::node_id(
                                            flow::NodeKind::Effect,
                                            r.target.effect_id);
                                        row = r.target.param_index ==
                                                      doc::kWetParam
                                            ? 0
                                            : r.target.param_index ==
                                                      doc::kOpacityParam
                                                ? 1
                                                : r.target.param_index >=
                                                          0
                                                    ? 2 + r.target
                                                              .param_index
                                                    : -1;
                                    }
                                }
                                if (cid == sw.to &&
                                    (sw.to_row < 0 || row < 0 ||
                                     row == sw.to_row))
                                    cut.push_back(r.id);
                            }
                            for (const uint64_t rid2 : cut)
                                app.undo.execute(
                                    app.document,
                                    doc::remove_route_command(app.scope_look,rid2));
                        }
                    }
                }
                for (const uint64_t cid : app.multi_sel) {
                    const uint64_t did = tag_doc(cid);
                    size_t li = 0, fi = 0;
                    switch (tag_kind(cid)) {
                        case flow::NodeKind::Effect:
                            if (find_effect_by_id(app.look(), did, &li,
                                                  &fi))
                                app.undo.execute(
                                    app.document,
                                    doc::remove_effect_command(app.scope_look,li, fi));
                            break;
                        case flow::NodeKind::Source: {
                            // Same path as the card's X.
                            const int idx =
                                layer_index_by_id(app.look(), did);
                            if (idx >= 0)
                                app.undo.execute(
                                    app.document,
                                    doc::remove_layer_command(app.scope_look,
                                        static_cast<size_t>(idx)));
                            break;
                        }
                        case flow::NodeKind::ModSource:
                            if (doc::find_value_node(app.look(), did))
                                app.undo.execute(
                                    app.document,
                                    doc::remove_value_node_command(app.scope_look,did));
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
                if (app.look().layers.empty()) {
                    app.layer_sel = false;
                } else if (app.selected_layer >= app.look().layers.size()) {
                    app.selected_layer = app.look().layers.size() - 1;
                    app.layer_sel = false;
                }
                structure_done = true;
            }
            // Delete removes the selected effect (layer removal stays
            // behind its inspector button — docs/flow_canvas.md).
            if (do_delete_sel && !structure_done) {
                if (app.sel.kind == SelKind::Effect) {
                    size_t li = 0, fi = 0;
                    if (find_effect_by_id(app.look(), app.sel.id, &li,
                                          &fi)) {
                        app.undo.execute(
                            app.document,
                            doc::remove_effect_command(app.scope_look,li, fi));
                        // Land on the neighbour that takes the slot.
                        const auto& stack = app.look().layers[li].stack;
                        app.sel = stack.empty()
                            ? Selection{SelKind::LayerSource,
                                        app.look().layers[li].id}
                            : Selection{SelKind::Effect,
                                        stack[std::min(fi,
                                                       stack.size() - 1)]
                                            .id};
                        structure_done = true;
                    }
                } else if (app.sel.kind == SelKind::ModSource) {
                    app.undo.execute(app.document,
                                     doc::remove_value_node_command(app.scope_look,app.sel.id));
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
                        layer_index_by_id(app.look(), app.sel.id);
                    if (idx >= 0) {
                        app.undo.execute(
                            app.document,
                            doc::remove_layer_command(app.scope_look,
                                static_cast<size_t>(idx)));
                        if (!app.look().layers.empty() &&
                            app.selected_layer >=
                                app.look().layers.size())
                            app.selected_layer =
                                app.look().layers.size() - 1;
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
                const auto& stack = app.look().layers[ui_layer].stack;
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
                app.undo.begin_group("Add Effect");
                // Spawns unwired: wiring is a wire gesture.
                app.undo.execute(app.document,
                                 doc::materialize_links_command(app.scope_look));
                app.undo.execute(app.document,
                                 doc::add_effect_command(app.scope_look,
                                     ui_layer, std::move(fx), insert_at));
                app.undo.end_group();
                // Select the newborn so its params appear immediately.
                app.sel = {SelKind::Effect, new_id};
                app.insert_before_id = 0;
                structure_done = true;
            }
        }

        // ---- morph position (coalesced slider drag)
        if (frame_ui.morph_changed && *frame_ui.morph_changed &&
            frame_ui.morph_staged &&
            *frame_ui.morph_staged != app.look().morph_pos) {
            app.undo.execute(
                app.document,
                doc::set_morph_command(app.scope_look,app.look().morph_from,
                                       app.look().morph_to,
                                       *frame_ui.morph_staged),
                /*coalesce=*/true);
        }

        // ---- time remap: speed drag + mode cycle
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

        // ---- sidechain + audio nudge
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
        // ---- export settings + cancel
        if (frame_ui.export_bitrate_changed &&
            *frame_ui.export_bitrate_changed &&
            *frame_ui.export_bitrate_staged !=
                app.document.export_bitrate_mbps) {
            app.undo.execute(app.document,
                             doc::set_export_config_command(
                                 *frame_ui.export_bitrate_staged,
                                 app.document.export_scale,
                                 app.document.export_audio),
                             /*coalesce=*/true);
        }
        if (frame_ui.export_bitrate_released &&
            *frame_ui.export_bitrate_released && !did_break) {
            app.undo.break_coalescing();
            did_break = true;
        }
        if (frame_ui.export_scale_selected &&
            *frame_ui.export_scale_selected >= 0) {
            const uint32_t div = *frame_ui.export_scale_selected == 2   ? 4u
                                 : *frame_ui.export_scale_selected == 1 ? 2u
                                                                        : 1u;
            if (div != app.document.export_scale)
                app.undo.execute(app.document,
                                 doc::set_export_config_command(
                                     app.document.export_bitrate_mbps, div,
                                     app.document.export_audio));
        }
        if (frame_ui.export_audio_changed && *frame_ui.export_audio_changed) {
            app.undo.execute(app.document,
                             doc::set_export_config_command(
                                 app.document.export_bitrate_mbps,
                                 app.document.export_scale,
                                 *frame_ui.export_audio_staged));
        }
        if (frame_ui.export_cancel_clicked &&
            *frame_ui.export_cancel_clicked && app.export_job) {
            // Cancel stops the run AND drains the queue — "stop exporting"
            // must not mean "start the next one".
            app.export_job->progress.cancel = true;
            app.export_queue.clear();
            app.status = "cancelling export...";
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

        // ---- randomize: one gesture = one undo step
        if (frame_ui.chaos_changed && *frame_ui.chaos_changed &&
            frame_ui.chaos_staged)
            app.chaos = *frame_ui.chaos_staged;
        if (frame_ui.randomize_all && *frame_ui.randomize_all &&
            !app.look().layers.empty() &&
            !app.look().layers[ui_layer].stack.empty()) {
            doc::randomize_stack(
                app.document, app.undo, app.scope_look, ui_layer, app.chaos,
                hash_combine(app.document.master_seed, app.rng_counter++));
        }
        for (const FxRowActions& row : frame_ui.rows) {
            if (app.look().layers.empty()) break;
            if (row.randomize && *row.randomize &&
                row.fx_index < app.look().layers[ui_layer].stack.size()) {
                doc::randomize_effect(
                    app.document, app.undo, app.scope_look, ui_layer,
                    row.fx_index, app.chaos,
                    hash_combine(app.document.master_seed,
                                 app.rng_counter++));
                break;
            }
        }

        // ---- group edits (groups + the exposed face)
        for (const FrameUi::ExposeToggle& et : frame_ui.expose_toggles) {
            if (!*et.clicked) continue;
            if (et.layer_index >= app.look().layers.size()) continue;
            app.undo.execute(app.document,
                             doc::set_group_exposed_command(app.scope_look,
                                 et.layer_index, et.group_id, et.key,
                                 et.expose));
            break;
        }
        for (const FrameUi::GroupActions& ga : frame_ui.group_actions) {
            // The group's OWN layer, not the selected one — group cards
            // stage actions from any layer on the canvas.
            size_t ga_layer = 0;
            if (!find_group_by_id(app.look(), ga.group_id, &ga_layer))
                continue;
            const doc::Group* group = nullptr;
            for (const doc::Group& g :
                 app.look().layers[ga_layer].groups)
                if (g.id == ga.group_id) group = &g;
            if (!group) continue;
            if (*ga.fold) {
                doc::Group edited = *group;
                edited.folded = !edited.folded;
                app.undo.execute(app.document,
                                 doc::set_group_props_command(app.scope_look,ga_layer,
                                                              edited));
            } else if (*ga.bypass_changed) {
                doc::Group edited = *group;
                edited.bypass = *ga.bypass_staged;
                app.undo.execute(app.document,
                                 doc::set_group_props_command(app.scope_look,ga_layer,
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
                        app.look(), ga_layer, ga.group_id);
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
                                 doc::ungroup_command(app.scope_look,ga_layer,
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
            if (app.look().layers.empty()) {
                // Same silent host conjure as the effect popup — a
                // storage bag is never a user-facing precondition.
                doc::Layer host = doc::make_layer(
                    app.document, doc::LayerSourceKind::Clip);
                app.undo.execute(app.document,
                                 doc::add_layer_command(app.scope_look,std::move(host),
                                                        0));
            }
            const size_t target_layer =
                std::min(ui_layer, app.look().layers.size() - 1);
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
                             doc::insert_group_command(app.scope_look,
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

        // ---- modulation edits. Value-node cards edit the node; wire
        // rows edit amount/curve; "~" mints an LFO wired onto the param.
        for (const FrameUi::NodeRow& row : frame_ui.node_rows) {
            using MST = doc::ModSourceType;
            const doc::ValueNode* vn =
                doc::find_value_node(app.look(), row.id);
            if (!vn) continue;
            if (row.pick_changed[0] && *row.pick_changed[0]) {
                const int sel = static_cast<int>(
                    *row.pick_staged[0] + 0.5f);
                if (sel >= 0 &&
                    sel < static_cast<int>(MST::Count) &&
                    sel != static_cast<int>(vn->source.type)) {
                    doc::ValueNode n = *vn;
                    n.source.type = static_cast<MST>(sel);
                    app.undo.execute(app.document,
                                     doc::set_value_node_command(app.scope_look,n));
                }
            } else if (row.pick_changed[1] && *row.pick_changed[1]) {
                const int sel = static_cast<int>(
                    *row.pick_staged[1] + 0.5f);
                doc::ValueNode n = *vn;
                bool apply = sel >= 0;
                if (n.source.type == MST::Lfo ||
                    n.source.type == MST::LfoBeat)
                    n.source.shape = static_cast<doc::LfoShape>(sel % 4);
                else if (n.source.type == MST::Envelope)
                    n.source.trigger = static_cast<uint32_t>(sel % 4);
                else if (n.source.type == MST::VideoSample ||
                         n.source.type == MST::VideoRegion)
                    n.source.channel = static_cast<uint32_t>(sel % 4);
                else if (n.source.type == MST::Math)
                    n.op = static_cast<doc::ValueOp>(
                        sel % static_cast<int>(doc::ValueOp::Count));
                else
                    apply = false;
                if (apply)
                    app.undo.execute(app.document,
                                     doc::set_value_node_command(app.scope_look,n));
            }
            // Slider slots, mapped per kind exactly like the card
            // builder laid them out.
            bool slot_released = false;
            for (int si = 0; si < 4; ++si) {
                if (row.slot_released[si] && *row.slot_released[si])
                    slot_released = true;
                if (!row.slot_changed[si] || !*row.slot_changed[si] ||
                    *row.slot_staged[si] == row.slot_original[si])
                    continue;
                doc::ValueNode n =
                    *doc::find_value_node(app.look(), row.id);
                const float val = *row.slot_staged[si];
                if (n.source.type == MST::Math) {
                    (si == 0 ? n.const_a : n.const_b) = val;
                } else if (n.source.type == MST::Normalise) {
                    (si == 0 ? n.const_a
                     : si == 1 ? n.in_min
                     : si == 2 ? n.in_max
                               : n.const_b) = val;
                } else if (n.source.type == MST::VideoSample ||
                           n.source.type == MST::VideoRegion) {
                    (si == 0 ? n.source.px
                     : si == 1 ? n.source.py
                     : si == 2 ? n.source.pw
                               : n.source.ph) = val;
                } else if (n.source.type == MST::Envelope ||
                           n.source.type == MST::Beat) {
                    n.source.decay = val;
                } else {
                    n.source.rate_hz = val;
                }
                app.undo.execute(app.document,
                                 doc::set_value_node_command(app.scope_look,n),
                                 /*coalesce=*/true);
            }
            if (slot_released && !did_break) {
                app.undo.break_coalescing();
                did_break = true;
            }
            if (row.remove && *row.remove) {
                app.undo.execute(app.document,
                                 doc::remove_value_node_command(app.scope_look,row.id));
                if (app.sel.kind == SelKind::ModSource &&
                    app.sel.id == row.id)
                    app.sel = {};
                break;   // node list shifted; one per frame
            }
        }
        for (const FrameUi::RouteRow& row : frame_ui.route_rows) {
            const doc::ModRoute* route = nullptr;
            for (const doc::ModRoute& r : app.look().mod_routes)
                if (r.id == row.id) route = &r;
            if (!route) continue;
            if (*row.curve_selected >= 0 &&
                *row.curve_selected != static_cast<int>(route->curve)) {
                app.undo.execute(
                    app.document,
                    doc::set_route_curve_command(app.scope_look,
                        row.id, static_cast<doc::ResponseCurve>(
                                    *row.curve_selected)));
            }
            if (*row.remove) {
                app.undo.execute(app.document,
                                 doc::remove_route_command(app.scope_look,row.id));
                break;   // indices into mod_routes shifted; one per frame
            }
        }
        for (const FrameUi::AddRoute& add : frame_ui.add_routes) {
            if (!*add.clicked) continue;
            doc::ValueNode node;
            node.id = app.document.next_route_id++;
            doc::ModRoute route;
            route.id = app.document.next_route_id++;
            route.node = node.id;
            route.target = add.key;
            app.undo.begin_group("Add Modulation");
            app.undo.execute(app.document,
                             doc::add_value_node_command(app.scope_look,node));
            app.undo.execute(app.document,
                             doc::add_route_command(app.scope_look,route));
            app.undo.end_group();
            break;
        }
        for (const FrameUi::KeyToggle& toggle : frame_ui.key_toggles) {
            if (!*toggle.clicked) continue;
            const double playhead = app.has_timeline()
                ? app.player.current_frame_index() : 0.0;
            std::vector<doc::Keyframe> keys;
            for (const doc::KeyframeLane& lane : app.look().lanes)
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
                             doc::set_lane_command(app.scope_look,toggle.key, std::move(keys)));
            // Jump-to-lane (docs/flow_canvas.md): k on a card also
            // scrolls the timeline so the param's lane editor is in view
            // (rows are 42 px + 4 gap).
            for (size_t i = 0; i < app.look().lanes.size(); ++i)
                if (app.look().lanes[i].target == toggle.key)
                    app.timeline_scroll.offset =
                        static_cast<float>(i) * 46.0f;
            break;
        }
        for (FrameUi::LaneEdit& edit : frame_ui.lane_edits) {
            app.undo.execute(
                app.document,
                doc::set_lane_command(app.scope_look,edit.target, std::move(edit.keys)),
                /*coalesce=*/true);
        }
        if (frame_ui.lane_release) app.undo.break_coalescing();
        // Rail type-in opens: a click on a slider's value text.
        for (const FrameUi::RailEdit& re : frame_ui.rail_edits) {
            if (!re.clicked || !*re.clicked) continue;
            app.rail_edit_key = re.key;
            app.rail_edit_scale = re.scale;
            app.rail_edit_buf = re.seed ? re.seed : "";
            app.rail_edit_commit = false;
            break;
        }

        if (frame_ui.add_layer_open && *frame_ui.add_layer_open)
            app.sel = {SelKind::AddLayer, 0};
        if (frame_ui.open_add_clicked && *frame_ui.open_add_clicked) {
            if (app.look().layers.empty()) {
                app.sel = {SelKind::AddLayer, 0};
            } else {
                const size_t li = std::min(
                    app.selected_layer, app.look().layers.size() - 1);
                app.sel = {SelKind::AddEffect,
                           app.look().layers[li].id};
                app.insert_before_id = 0;
            }
        }
        if (frame_ui.add_frame_clicked && *frame_ui.add_frame_clicked) {
            doc::CanvasFrame fr;
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
                             doc::add_frame_command(app.scope_look,std::move(fr)));
        }
        // Rail selector dropdowns: a pick becomes the param value.
        for (const FrameUi::ParamPick& pick : frame_ui.param_picks) {
            if (*pick.selected < 0 ||
                pick.layer_index >= app.look().layers.size() ||
                pick.fx_index >=
                    app.look().layers[pick.layer_index].stack.size())
                continue;
            const float next =
                pick.min_value + static_cast<float>(*pick.selected);
            app.undo.execute(app.document,
                             doc::set_param_command(app.scope_look,pick.layer_index,
                                                    pick.fx_index,
                                                    pick.param_index, next));
        }
        // Rail text field: open the shared inline editor.
        for (const FrameUi::TextEditOpen& open : frame_ui.text_edit_opens) {
            if (!*open.clicked) continue;
            app.text_edit_id = open.effect_id;
            app.text_edit_buf.clear();
            size_t tli = 0, tfi = 0;
            if (find_effect_by_id(app.look(), open.effect_id, &tli, &tfi))
                app.text_edit_buf =
                    app.look().layers[tli].stack[tfi].text;
        }

        // ---- layer edits
        for (const FrameUi::LayerRow& lrow : frame_ui.layer_rows) {
            if (*lrow.select) {
                app.selected_layer = lrow.index;
                app.layer_sel = true;   // the LAYER itself is picked
            }
            const doc::Layer* layer = nullptr;
            for (const doc::Layer& l : app.look().layers)
                if (l.id == lrow.id) layer = &l;
            if (!layer) continue;
            if (*lrow.visible_changed) {
                doc::Layer edited = *layer;
                edited.visible = *lrow.visible_staged;
                app.undo.execute(app.document,
                                 doc::set_layer_props_command(app.scope_look,edited));
            } else if (*lrow.blend_selected >= 0 &&
                       *lrow.blend_selected !=
                           static_cast<int>(layer->blend)) {
                doc::Layer edited = *layer;
                edited.blend =
                    static_cast<doc::BlendMode>(*lrow.blend_selected);
                app.undo.execute(app.document,
                                 doc::set_layer_props_command(app.scope_look,edited));
            } else if (lrow.osc_shape_selected &&
                       *lrow.osc_shape_selected >= 0 &&
                       static_cast<uint32_t>(*lrow.osc_shape_selected) !=
                           layer->osc_shape) {
                doc::Layer edited = *layer;
                edited.osc_shape =
                    static_cast<uint32_t>(*lrow.osc_shape_selected);
                app.undo.execute(app.document,
                                 doc::set_layer_props_command(app.scope_look,edited));
            } else if (lrow.xf_toggle && *lrow.xf_toggle) {
                // View state, no undo (like section folds).
                app.layer_ui[lrow.id].xf_open =
                    !app.layer_ui[lrow.id].xf_open;
            } else if (lrow.flip_h && *lrow.flip_h) {
                doc::Layer edited = *layer;
                edited.flip_h = !edited.flip_h;
                app.undo.execute(app.document,
                                 doc::set_layer_props_command(app.scope_look,edited));
            } else if (lrow.flip_v && *lrow.flip_v) {
                doc::Layer edited = *layer;
                edited.flip_v = !edited.flip_v;
                app.undo.execute(app.document,
                                 doc::set_layer_props_command(app.scope_look,edited));
            } else if (*lrow.remove) {
                // Zero layers is a valid document — the viewport shows the
                // raw source and the layers panel just offers the adders.
                app.undo.execute(app.document,
                                 doc::remove_layer_command(app.scope_look,lrow.index));
                if (!app.look().layers.empty() &&
                    app.selected_layer >= app.look().layers.size())
                    app.selected_layer = app.look().layers.size() - 1;
                break;
            } else if (lrow.up && *lrow.up && lrow.index > 0) {
                app.undo.execute(app.document,
                                 doc::move_layer_command(app.scope_look,lrow.index, -1));
                if (app.selected_layer == lrow.index)
                    app.selected_layer = lrow.index - 1;
                else if (app.selected_layer == lrow.index - 1)
                    app.selected_layer = lrow.index;
                break;
            } else if (lrow.down && *lrow.down &&
                       lrow.index + 1 < app.look().layers.size()) {
                app.undo.execute(app.document,
                                 doc::move_layer_command(app.scope_look,lrow.index, 1));
                if (app.selected_layer == lrow.index)
                    app.selected_layer = lrow.index + 1;
                else if (app.selected_layer == lrow.index + 1)
                    app.selected_layer = lrow.index;
                break;
            }
        }
        // Clip node media binding: dropdown picks an imported asset;
        // "+" browses - ready bundles bind now, fresh media runs the
        // import job carrying the bind target.
        for (const FrameUi::ClipBind& cb2 : frame_ui.clip_binds) {
            doc::Layer* layer = nullptr;
            for (doc::Layer& l : app.look().layers)
                if (l.id == cb2.layer_id) layer = &l;
            if (!layer) continue;
            if (*cb2.selected >= 0 &&
                static_cast<size_t>(*cb2.selected) <
                    app.document.assets.size() &&
                app.document.assets[static_cast<size_t>(*cb2.selected)]
                        .id != layer->asset) {
                doc::Layer edited = *layer;
                edited.asset =
                    app.document.assets[static_cast<size_t>(*cb2.selected)]
                        .id;
                app.undo.execute(app.document,
                                 doc::set_layer_props_command(
                                     app.scope_look, std::move(edited)));
                refresh_bundles(app);
            }
            if (*cb2.browse)
                browse_and_bind_clip(app, window.get(), app.scope_look,
                                     layer->id);
        }
        // The same binding from the CARD's dropdown row: entry 0 unbinds
        // (a deliberately dormant node), the tail entry browses, the
        // rest bind the picked asset.
        for (const FrameUi::ClipRowBind& crb : frame_ui.clip_row_binds) {
            if (!*crb.changed) continue;
            doc::Layer* layer = nullptr;
            for (doc::Layer& l : app.look().layers)
                if (l.id == crb.layer_id) layer = &l;
            if (!layer) continue;
            const int pick = static_cast<int>(*crb.staged + 0.5f);
            if (pick == crb.n_assets + 1) {
                browse_and_bind_clip(app, window.get(), app.scope_look,
                                     layer->id);
            } else if (pick == 0) {
                if (layer->asset) {
                    doc::Layer edited = *layer;
                    edited.asset = 0;
                    app.undo.execute(app.document,
                                     doc::set_layer_props_command(
                                         app.scope_look,
                                         std::move(edited)));
                }
            } else if (pick - 1 <
                           static_cast<int>(app.document.assets.size()) &&
                       app.document.assets[static_cast<size_t>(pick - 1)]
                               .id != layer->asset) {
                doc::Layer edited = *layer;
                edited.asset =
                    app.document.assets[static_cast<size_t>(pick - 1)].id;
                app.undo.execute(app.document,
                                 doc::set_layer_props_command(
                                     app.scope_look, std::move(edited)));
                refresh_bundles(app);
            }
        }
        // Empty timeline space: a plain press is DESELECT ALL - no
        // node, no layer, no collect; the monitor returns to the film.
        if (frame_ui.tl_deselect && *frame_ui.tl_deselect) {
            app.sel = {};
            app.insert_before_id = 0;
            app.layer_sel = false;
            app.sel_placement = 0;
            app.multi_sel.clear();
            app.sel_wires.clear();
        }
        // Timeline blocks: a press picks the LANE - accent bar lit,
        // monitor on the lane's own output - plus the BLOCK (outline;
        // Delete removes it, razor narrows to its lane), and selects NO
        // node, so the graph selection never moves from an arrange
        // gesture. Audio blocks pick themselves but no lane. A DOUBLE
        // click opens the block's target for editing - the same gesture
        // as a canvas ref card; Escape / the breadcrumb come back up.
        for (const FrameUi::BlockPick& bp : frame_ui.block_picks) {
            if (!*bp.pressed) continue;
            const bool dbl =
                bp.placement_id == app.last_block_pick &&
                app.app_seconds - app.last_block_pick_time < 0.4;
            app.last_block_pick = bp.placement_id;
            app.last_block_pick_time = app.app_seconds;
            app.sel_placement = bp.placement_id;
            if (bp.layer_index != SIZE_MAX) {
                app.selected_layer = bp.layer_index;
                app.layer_sel = true;
            } else {
                app.layer_sel = false;
            }
            app.sel = {};
            app.multi_sel.clear();
            if (dbl && !app.scope_is_look()) {
                const doc::Placement* p = doc::find_placement(
                    app.sequence(), bp.placement_id);
                if (p && (app.document.find_look(p->target) ||
                          app.document.find_sequence(p->target))) {
                    const doc::Look* tl3 =
                        app.document.find_look(p->target);
                    app.scope_look = p->target;
                    app.open_group = 0;
                    app.sel = {};
                    app.selected_layer = 0;
                    app.layer_sel = false;
                    app.sel_placement = 0;
                    app.sel_wires.clear();
                    app.canvas_state.view_inited = false;
                    app.saved_view_valid = false;
                    app.tl_v0 = app.tl_v1 = 0.0;
                    app.undo.break_coalescing();
                    app.status =
                        "editing " +
                        (tl3 ? tl3->name
                             : app.document.sequence(p->target).name);
                }
            }
            break;
        }
        for (const FrameUi::BlockStage& bs : frame_ui.block_stages) {
            if (*bs.changed) {
                // One placement edit, applied through its LINK GROUP:
                // picture and sound move as one unless unlinked.
                app.undo.execute(
                    app.document,
                    doc::set_placement_command(app.scope_look, *bs.staged),
                    /*coalesce=*/true);
            }
            if (*bs.released) app.undo.break_coalescing();
        }
        for (const FrameUi::AudioTrackStage& as : frame_ui.audio_tracks) {
            const doc::AudioTrack* track = nullptr;
            for (const doc::AudioTrack& t : app.sequence().audio)
                if (t.id == as.track_id) track = &t;
            if (!track) continue;
            if (*as.gain_changed && *as.gain_staged != as.original)
                app.undo.execute(
                    app.document,
                    doc::set_audio_track_props_command(
                        app.scope_look, track->id, track->name,
                        std::clamp(*as.gain_staged, 0.0f, 2.0f),
                        track->mute),
                    /*coalesce=*/true);
            if (*as.gain_released) app.undo.break_coalescing();
            if (*as.mute_clicked)
                app.undo.execute(app.document,
                                 doc::set_audio_track_props_command(
                                     app.scope_look, track->id,
                                     track->name, track->gain,
                                     !track->mute));
        }
        for (const FrameUi::LayerStage& stage : frame_ui.layer_stages) {
            if (*stage.changed && *stage.staged != stage.original) {
                doc::Layer* layer = nullptr;
                for (doc::Layer& l : app.look().layers)
                    if (l.id == stage.layer_id) layer = &l;
                if (!layer) continue;
                const float v = *stage.staged;
                using LF = FrameUi::LayerField;
                doc::Layer edited = *layer;
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
                    case LF::Slip:
                        edited.slip = static_cast<uint32_t>(
                            std::max(v, 0.0f) + 0.5f);
                        break;
                    case LF::OscShape:
                        edited.osc_shape = static_cast<uint32_t>(
                            std::clamp(v, 0.0f, 3.0f) + 0.5f);
                        break;
                }
                app.undo.execute(app.document,
                                 doc::set_layer_props_command(app.scope_look,std::move(edited)),
                                 /*coalesce=*/true);
            }
            if (*stage.released && !did_break) {
                app.undo.break_coalescing();
                did_break = true;
            }
        }
        for (const FrameUi::ColorStage& stage : frame_ui.color_stages) {
            if (*stage.changed &&
                (stage.staged[0] != stage.original[0] ||
                 stage.staged[1] != stage.original[1] ||
                 stage.staged[2] != stage.original[2])) {
                doc::Layer* layer = nullptr;
                for (doc::Layer& l : app.look().layers)
                    if (l.id == stage.layer_id) layer = &l;
                if (layer) {
                    doc::Layer edited = *layer;
                    float* dst =
                        stage.color_b ? edited.color_b : edited.color_a;
                    dst[0] = stage.staged[0];
                    dst[1] = stage.staged[1];
                    dst[2] = stage.staged[2];
                    app.undo.execute(
                        app.document,
                        doc::set_layer_props_command(app.scope_look,std::move(edited)),
                        /*coalesce=*/true);
                }
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
                    app.look().layers.size() < doc::kMaxLayers) {
                    doc::Layer layer =
                        doc::make_layer(app.document, kAddKinds[t]);
                    // Spawns unwired — except the very first
                    // source in an empty document (materialize no-ops).
                    app.undo.begin_group("Add Source");
                    app.undo.execute(app.document,
                                     doc::materialize_links_command(app.scope_look));
                    app.undo.execute(app.document,
                                     doc::add_layer_command(app.scope_look,
                                         std::move(layer),
                                         app.look().layers.size()));
                    app.undo.end_group();
                    app.selected_layer = app.look().layers.size() - 1;
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
        for (int c = 0; c < static_cast<int>(doc::FxCategory::Count); ++c)
            if (frame_ui.fx_cat_clicked[c] && *frame_ui.fx_cat_clicked[c])
                app.fx_cat_open[c] = !app.fx_cat_open[c];
        for (int s = 0; s < 3; ++s) {
            if (frame_ui.snap_store_clicked[s] &&
                *frame_ui.snap_store_clicked[s])
                app.undo.execute(app.document, doc::store_snapshot_command(app.scope_look,s));
            if (frame_ui.snap_apply_clicked[s] &&
                *frame_ui.snap_apply_clicked[s] &&
                app.look().snapshots[s].valid)
                app.undo.execute(app.document, doc::apply_snapshot_command(app.scope_look,s));
        }

        if ((frame_ui.undo_clicked && *frame_ui.undo_clicked) || do_undo)
            app.undo.undo(app.document);
        if ((frame_ui.redo_clicked && *frame_ui.redo_clicked) || do_redo)
            app.undo.redo(app.document);

        // ---- project save / open
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
        if (((frame_ui.open_project_clicked &&
              *frame_ui.open_project_clicked) ||
             do_open_project) &&
            guard_unsaved_changes(
                app, ConfirmDialog::Action::OpenProjectDialog)) {
            open_project_via_dialog(app, window.get());
        }
        // Recent-project opens, dirty-guarded like every open.
        for (const FrameUi::RecentRow& rrow : frame_ui.recent_rows) {
            if (!*rrow.clicked) continue;
            if (rrow.index < app.recent_projects.size() &&
                guard_unsaved_changes(
                    app, ConfirmDialog::Action::OpenProjectPath,
                    app.recent_projects[rrow.index]))
                open_project(app, app.recent_projects[rrow.index],
                             window.get());
            break;
        }
        // Cache management.
        if (frame_ui.cache_open_clicked && *frame_ui.cache_open_clicked) {
            const std::wstring dir =
                (executable_dir() / "cache").wstring();
            ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr,
                          SW_SHOWNORMAL);
        }
        if (frame_ui.cache_clear_clicked &&
            *frame_ui.cache_clear_clicked) {
            // Every bundle dir except the open clip's; autosaves stay.
            const std::filesystem::path root = executable_dir() / "cache";
            const std::filesystem::path keep =
                primary_clip_path(app.document).empty()
                    ? std::filesystem::path{}
                    : bundle_dir_for(primary_clip_path(app.document));
            std::error_code ec;
            uint64_t removed = 0;
            for (auto it = std::filesystem::directory_iterator(root, ec);
                 !ec && it != std::filesystem::directory_iterator();
                 it.increment(ec)) {
                if (!it->is_directory(ec)) continue;
                if (!keep.empty() && it->path() == keep) continue;
                std::error_code rec_ec;
                removed +=
                    std::filesystem::remove_all(it->path(), rec_ec);
            }
            app.cache_bytes = scan_cache_bytes();
            app.status =
                "cache cleared (" + std::to_string(removed) + " files)";
        }
        // Status history: keep what the transient strip drops.
        if (!app.status.empty() && app.status != app.status_log_last) {
            app.status_log_last = app.status;
            app.status_log.push_back(app.status);
            if (app.status_log.size() > 30)
                app.status_log.erase(app.status_log.begin());
        }
        // Autosave: dirty documents snapshot at most once a minute
        // — titled beside their project file, untitled under cache/ — and
        // are offered back on the next run (crash recovery).
        if (app.document.revision != app.autosaved_revision &&
            now - app.last_autosave > std::chrono::seconds(60)) {
            if (doc::save_document(autosave_path_for(app), app.document))
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
        if (frame_ui.import_media_clicked &&
            *frame_ui.import_media_clicked && !app.import) {
            auto picked = platform::show_open_dialog(
                window.get(),
                {{"video / image", "*.mp4;*.mov;*.mez;*.png;*.tga"},
                 {"all files", "*.*"}});
            if (picked) import_media(app, *picked);
        }
        // "new look" from the blank start: mint a look, place one block
        // of it on the project timeline, and enter it — the look's
        // editing view is HOME, the timeline is where you go up to.
        if (frame_ui.new_look_clicked && *frame_ui.new_look_clicked) {
            doc::Look fresh = doc::make_look(app.document, "");
            const uint64_t look_id = fresh.id;
            doc::Layer base = doc::make_layer(app.document,
                                              doc::LayerSourceKind::Solid);
            fresh.layers.push_back(std::move(base));
            doc::Placement block;
            block.id = app.document.next_effect_id++;
            block.target = look_id;
            app.undo.begin_group("New Look");
            app.undo.execute(app.document,
                             doc::add_look_command(std::move(fresh)));
            doc::Sequence& rseq2 = app.document.root();
            if (!rseq2.tracks.empty())
                app.undo.execute(
                    app.document,
                    doc::add_placement_command(
                        rseq2.id, rseq2.tracks.front().id, block));
            app.undo.end_group();
            app.scope_look = look_id;
            app.open_group = 0;
            app.sel = {};
            app.layer_sel = false;
            app.multi_sel.clear();
            app.sel_wires.clear();
            app.canvas_state.view_inited = false;
            app.saved_view_valid = false;
            app.status = "editing " + app.document.look(look_id).name;
        }
        // BROWSER actions: open moves the editing scope (a sequence's
        // timeline or a look's graph), "+" places at the playhead.
        for (const FrameUi::BrowserAction& ba : frame_ui.browser_looks) {
            const doc::Look* bl = app.document.find_look(ba.id);
            const doc::Sequence* bs2 =
                bl ? nullptr : app.document.find_sequence(ba.id);
            if (ba.open && *ba.open && ba.id != app.scope_look &&
                (bl || bs2)) {
                app.scope_look = ba.id;
                app.open_group = 0;
                app.sel = {};
                app.selected_layer = 0;
                app.layer_sel = false;
                app.sel_placement = 0;
                app.multi_sel.clear();
                app.sel_wires.clear();
                app.canvas_state.view_inited = false;
                app.saved_view_valid = false;
                app.tl_v0 = app.tl_v1 = 0.0;
                app.undo.break_coalescing();
                app.status =
                    "editing " + (bl ? bl->name : bs2->name);
            }
            if (ba.place && *ba.place)
                place_look_block(app, ba.id,
                                 app.player.current_frame_index());
        }
        for (const FrameUi::BrowserAction& ba : frame_ui.browser_assets) {
            if (!ba.place || !*ba.place) continue;
            const doc::Asset* asset = app.document.find_asset(ba.id);
            if (!asset || asset->path.empty()) continue;
            if (!place_clip_block(app, std::filesystem::path(asset->path),
                                  app.player.current_frame_index()))
                app.status = "no bundle yet - import the clip first";
        }
        if (frame_ui.new_sequence_clicked &&
            *frame_ui.new_sequence_clicked) {
            doc::Sequence fresh = doc::make_sequence(app.document, "");
            const uint64_t seq_id = fresh.id;
            app.undo.execute(app.document,
                             doc::add_sequence_command(std::move(fresh)));
            app.scope_look = seq_id;
            app.open_group = 0;
            app.sel = {};
            app.selected_layer = 0;
            app.layer_sel = false;
            app.sel_placement = 0;
            app.multi_sel.clear();
            app.sel_wires.clear();
            app.canvas_state.view_inited = false;
            app.saved_view_valid = false;
            app.tl_v0 = app.tl_v1 = 0.0;
            app.undo.break_coalescing();
            app.status =
                "editing " + app.document.sequence(seq_id).name;
        }
        // Drag-and-drop: video files open like the dialog would; preset
        // files import into the browser; other .json loads as a project;
        // a PNG installs a custom glyph set.
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
                } else if (guard_unsaved_changes(
                               app, ConfirmDialog::Action::OpenProjectPath,
                               p)) {
                    open_project(app, p, window.get());
                }
            } else if (ext == ".mp4" || ext == ".mov" || ext == ".mez" ||
                       ext == ".tga") {
                // ONTO THE TIMELINE = a new block at the drop frame; the
                // project's clip is not what a drop is about once there
                // is an arrangement to drop into. Anywhere else keeps the
                // old meaning, and unimported media imports first.
                const Vec2 at{dropped_at.x / scale, dropped_at.y / scale};
                bool placed = false;
                if (app.tl_rect.w > 0.0f && at.x >= app.tl_rect.x &&
                    at.x < app.tl_rect.right() && at.y >= app.tl_rect.y &&
                    at.y < app.tl_rect.bottom())
                    placed = place_clip_block(app, p, timeline_frame_at(app,
                                                                        at.x));
                if (!placed) open_source(app, p);
            } else if (ext == ".png") {
                // A PNG with a sibling .json grid descriptor ({"tile": 8,
                // "cols": 16, "rows": 6}) installs as a custom glyph set
                //; a bare PNG opens as a still clip.
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
                    // Color tilesets (emoji, ): any real chroma
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
            app.has_timeline()) {
            if (app.player.playing()) app.player.pause();
            else app.player.play();
        }
        if (frame_ui.seek_changed && *frame_ui.seek_changed &&
            frame_ui.seek_staged) {
            app.player.seek_frame(
                static_cast<uint32_t>(*frame_ui.seek_staged + 0.5f));
        }
        if (frame_ui.seek_to >= 0.0f && app.has_timeline())
            app.player.seek_frame(
                static_cast<uint32_t>(frame_ui.seek_to + 0.5f));
        // I/O keys land in the ruler handles' channel; a keypress is a
        // discrete edit, so it breaks coalescing like a released drag.
        if (key_trim_in >= 0.0f) {
            frame_ui.trim_in_to = key_trim_in;
            frame_ui.region_released = true;
        }
        if (key_trim_out >= 0.0f) {
            frame_ui.trim_out_to = key_trim_out;
            frame_ui.region_released = true;
        }
        // Timeline region edits: ruler trim handles + loop band.
        if (frame_ui.trim_in_to >= 0.0f || frame_ui.trim_out_to >= 0.0f ||
            frame_ui.loop_in_to >= 0.0f || frame_ui.loop_clear) {
            uint32_t t_in = app.sequence().trim_in;
            uint32_t t_out = app.sequence().trim_out;
            uint32_t l_in = app.sequence().loop_in;
            uint32_t l_out = app.sequence().loop_out;
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
            if (app.has_timeline() && t_out >= app.player.frame_count())
                t_out = 0;
            if (!app.scope_is_look())
                app.undo.execute(
                    app.document,
                    doc::set_timeline_region_command(
                        app.sequence().id, t_in, t_out, l_in, l_out),
                    /*coalesce=*/true);
        }
        if (frame_ui.region_released) app.undo.break_coalescing();
        for (const FrameUi::LaneLoop& ll : frame_ui.lane_loops) {
            if (!*ll.clicked) continue;
            for (const doc::KeyframeLane& lane : app.look().lanes) {
                if (!(lane.target == ll.target)) continue;
                app.undo.execute(app.document,
                                 doc::set_lane_loop_command(app.scope_look,ll.target,
                                                            !lane.loop));
                break;
            }
        }
        for (const FrameUi::LaneMute& lm : frame_ui.lane_mutes) {
            if (!*lm.clicked) continue;
            for (const doc::KeyframeLane& lane : app.look().lanes) {
                if (!(lane.target == lm.target)) continue;
                app.undo.execute(app.document,
                                 doc::set_lane_mute_command(app.scope_look,lm.target,
                                                            !lane.muted));
                break;
            }
        }
        for (const FrameUi::LaneKill& lk : frame_ui.lane_kills) {
            if (!*lk.clicked) continue;
            // Empty keys = remove the lane (set_lane_command semantics).
            app.undo.execute(app.document,
                             doc::set_lane_command(app.scope_look,lk.target, {}));
            break;
        }
        // THE TIMELINE IS THE CLOCK: the transport runs the SCOPED
        // entity's local time at the project rate - a sequence plays its
        // arrangement inside its trim band, a look loops its own
        // duration whole (timeless: no region of its own). Nothing here
        // depends on a clip being open.
        {
            const double rate = project_fps(app.document, app.bundles);
            const uint32_t content = app.scope_duration();
            // BUFFER (the NLE convention): the timeline runs past the last
            // block so there is somewhere to drag a block's end OUT to, and
            // somewhere to drop the next one. Without it a block that ends
            // at the content's end can only ever be shortened - shrinking
            // the timeline with it, so it can never be pulled back.
            // The buffer is scenery: what PLAYS and what EXPORTS is the
            // content, which is why the trim ends there.
            const uint32_t span =
                content ? content + timeline_buffer_frames(rate)
                        : static_cast<uint32_t>(rate * kEmptyTimelineSeconds +
                                                0.5);
            app.player.configure(rate, span);
            const uint32_t play_end = content ? content : span;
            const bool look_scope = app.scope_is_look();
            const uint32_t seq_trim_in =
                look_scope ? 0u : app.sequence().trim_in;
            const uint32_t seq_trim_out =
                look_scope ? 0u : app.sequence().trim_out;
            const uint32_t t_in =
                std::min(seq_trim_in, play_end ? play_end - 1 : 0u);
            const uint32_t t_out =
                seq_trim_out ? std::min(seq_trim_out, span) : play_end;
            if (t_in != app.player.trim_in() ||
                t_out != app.player.trim_out())
                app.player.set_trim(t_in, t_out);
            app.player.set_loop_region(
                look_scope ? 0u : app.sequence().loop_in,
                look_scope ? 0u : app.sequence().loop_out);
            app.player.set_audio_offset(
                static_cast<double>(app.document.audio_offset_ms) * 0.001);
        }
        refresh_mix(app);
        sync_sidechain(app);
        // Half-res proxy: the bundle table names the file every placement
        // decodes, so a toggle is a re-resolve, not a reopen.
        {
            std::filesystem::path proxy = app.mez_path;
            proxy.replace_extension(".proxy.mez");
            std::error_code pec;
            const bool want = app.document.use_proxy && !app.mez_path.empty() &&
                              std::filesystem::exists(proxy, pec);
            if (want != app.proxy_active) {
                render_worker.pause();
                app.proxy_active = want;
                refresh_bundles(app);
                render_worker.resume();
            }
        }
        // Thumbnail strip: register the staged RGBA once.
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
        // Monitor volume: mute is gain 0, the slider value stays.
        if (frame_ui.mute_clicked && *frame_ui.mute_clicked) {
            app.audio_muted = !app.audio_muted;
            save_ui_prefs(app);
        }
        if (frame_ui.volume_changed && *frame_ui.volume_changed)
            app.audio_gain = *frame_ui.volume_staged;
        if (frame_ui.volume_released && *frame_ui.volume_released)
            save_ui_prefs(app);
        app.player.set_gain(app.audio_muted ? 0.0f : app.audio_gain);
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
            app.has_timeline()) {
            std::filesystem::path default_name(app.clip_name);
            default_name.replace_extension("");
            auto out = platform::show_save_dialog(
                window.get(), {{"MP4 video", "*.mp4"}},
                default_name.string() + "_look.mp4");
            if (out) {
                if (out->extension() != ".mp4") out->replace_extension(".mp4");
                // Sidechain mux: the Audio Scope reads the sidechain's PCM
                // instead of the clip's when asked (and available).
                const std::filesystem::path scope_pcm =
                    app.document.sidechain_mux && app.sc_ok
                        ? app.sc_pcm_path
                        : app.pcm_path;
                if (!app.export_job) {
                    app.export_job = start_export(
                        renderer->device(), shader_dir, app.bundles,
                        app.pcm_cache, scope_pcm, app.document, app.look().id,
                        app.has_analysis ? &app.analysis : nullptr, *out);
                } else {
                    // Render queue: snapshot now, render later.
                    AppState::QueuedExport q;
                    q.out_path = *out;
                    q.doc = app.document;
                    q.look_id = app.look().id;
                    q.has_analysis = app.has_analysis;
                    if (app.has_analysis) q.analysis = app.analysis;
                    q.bundles = app.bundles;
                    q.pcm = app.pcm_cache;
                    q.scope_pcm = scope_pcm;
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

        // ---- preview (render thread): post the latest snapshot
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
            const uint32_t amode = app.alpha_checker ? 1u : 0u;
            if (app.bypass_all && source_image) {
                viewport_pass->draw(frame.cmd, ui_view_arena,
                                    frame.frame_index, *source_image,
                                    ui_view_sampler, frame.extent,
                                    px, py, pw, ph, 0.0f, 1.0f, amode);
            } else if (app.ab_wipe && source_image) {
                // Before left of the split, after right of it.
                viewport_pass->draw(frame.cmd, ui_view_arena,
                                    frame.frame_index, *source_image,
                                    ui_view_sampler, frame.extent,
                                    px, py, pw, ph, 0.0f, app.wipe_pos,
                                    amode);
                viewport_pass->draw(frame.cmd, ui_view_arena,
                                    frame.frame_index, *final_image,
                                    ui_view_sampler, frame.extent,
                                    px, py, pw, ph, app.wipe_pos, 1.0f,
                                    amode);
            } else {
                viewport_pass->draw(frame.cmd, ui_view_arena,
                                    frame.frame_index, *final_image,
                                    ui_view_sampler, frame.extent,
                                    px, py, pw, ph, 0.0f, 1.0f, amode);
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

        // Modal confirm: scrim + panel above everything, tooltips included.
        if (app.confirm.open())
            draw_confirm_dialog(canvas, font,
                                header_font ? &*header_font : nullptr,
                                viewport, app, dt);

        ui_renderer->record(frame.cmd, frame.frame_index, frame.extent, canvas);
        renderer->end_frame(frame);
    }

    // Stop the render thread before its device resources unwind.
    app.render_worker = nullptr;
    render_worker.stop();
    vkDestroySampler(renderer->device().device(), ui_view_sampler, nullptr);
    return 0;
}

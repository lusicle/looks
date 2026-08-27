#include "media/decode_pool.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "media/bmff.h"
#include "util/log.h"

namespace looks::media {

namespace {

// How far ahead the pool looks for sources that are not playing yet. A
// cold file open plus its first decode is the hitch a cut would otherwise
// cost, so it happens a second early.
constexpr uint32_t kPrewarmFrames = 60;
// Decoded frames queued ahead per stream (the old player's lookahead,
// widened for native streams' deeper rings).
constexpr uint32_t kDecodeAhead = 12;
// File handles held for placements that are only prewarming.
constexpr size_t kMaxStreams = 32;
// Decoded frames held across all streams: 1080p I420 is ~3 MB each, 4K
// ~12 MB. Native streams ride the deep end of the clamp so a render
// hiccup's playhead jump lands inside the ring instead of past it - a
// jump past the ring is a keyframe roll, which is the hitch.
constexpr size_t kFrameBudget = 96;
// Backward playback keeps this many rolled frames ringed (the half-window
// reverse policy, bounded): a run decodes once and serves backward from
// the ring. 4K I420 is ~12 MB a frame, so this caps a reverse stream near
// 300 MB while it lasts.
constexpr uint32_t kReverseWindow = 24;
// Feeds past the target's own sample before a roll gives up: reorder
// depth is a handful of frames on any conformant stream.
constexpr uint32_t kRollGuard = 64;
// Collects without a plan/prewarm touch before an idle stream's decoder
// sessions, file handles and ring memory release. The stream entry and
// its frame index stay: re-entry pays a session open plus a keyframe
// roll, not a demux, and a SCHEDULED re-entry prewarms through the same
// touch a second ahead of playing.
constexpr uint64_t kIdleCloseCollects = 300;

bool same_bundles(const std::vector<AssetBundle>& a,
                  const std::vector<AssetBundle>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].asset != b[i].asset || a[i].mez != b[i].mez ||
            a[i].native != b[i].native || a[i].frames != b[i].frames)
            return false;
    return true;
}

// The decodable video file a bundle names: the source itself (native) or
// the mezzanine (stills, cover art, direct .mez).
const std::filesystem::path& bundle_video(const AssetBundle& b) {
    return b.native.empty() ? b.mez : b.native;
}

// Content stamps for upload skipping (codec::DecodedFrame::stamp):
// process-wide so keys can never collide across pools.
std::atomic<uint64_t> g_frame_stamp{1};

// NV12 (decoder output) -> a frame the engine binds, ZERO-COPY: the
// decoder's buffer swaps whole into the frame (Y rows then interleaved
// CbCr rows, one stride) and the caller's scratch inherits the old
// capacity - no deinterleave, no copy, no allocation in steady state.
std::shared_ptr<const codec::DecodedFrame> nv12_to_decoded(
    std::shared_ptr<codec::DecodedFrame> out,
    platform::VideoFrameNV12& src) {
    if (!out) out = std::make_shared<codec::DecodedFrame>();
    out->stamp = g_frame_stamp.fetch_add(1, std::memory_order_relaxed);
    out->width = src.width;
    out->height = src.height;
    out->y_stride = src.stride ? src.stride : src.width;
    out->uv_stride = out->y_stride;
    out->nv12 = true;
    out->y.swap(src.data);
    out->u.clear();
    out->v.clear();
    return out;
}

bool read_sample_at(FILE* file, const FrameIndex::Sample& s,
                    std::vector<uint8_t>& out) {
    return BmffFile::read_at(file, s.offset, s.size, out);
}

// The scrub epoch the CURRENT worker thread's job was queued under:
// an advisory roll bails when a drag started after it was queued.
thread_local uint64_t t_job_epoch = 0;

// Per-instance MAPPING hash: the mapping's shape, not its window -
// source = f*speed + intercept reads the same media frame at any
// shared time regardless of t_in/t_out. Feeds the per-flatten key fold
// in set_document; never a stream identity by itself (per-mapping
// streams split every block of a look into its own file open + decoder
// create at the cut, and the open-stream population starved the
// prewarm cap within a minute of playback).
uint64_t bits_of(double v) {
    uint64_t b;
    std::memcpy(&b, &v, sizeof(b));
    return b;
}

uint64_t mapping_hash(const doc::MediaInstance& c) {
    uint64_t k = c.asset * 0x9E3779B97F4A7C15ull + 1;
    auto mix = [&k](uint64_t v) {
        k ^= v + 0x9E3779B97F4A7C15ull + (k << 6) + (k >> 2);
    };
    mix(bits_of(c.source_in - c.t_in * c.speed));
    mix(bits_of(c.speed));
    mix(bits_of(c.rate));
    mix(static_cast<uint64_t>(c.shift));
    return k;
}

}  // namespace

DecodePool::Stream::Session::~Session() {
    if (file) std::fclose(static_cast<FILE*>(file));
}

DecodePool::DecodePool(const char* name) : name_(name) {
    // Workers cover the prewarm backlog across streams: mez streams also
    // fan one stream over its reader bank (~3 concurrent decodes to hold
    // a 4K stream at rate); native streams decode serially per session,
    // so their workers spread across streams and the second session.
    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned count = std::clamp(hw / 3, 2u, 8u);
    for (unsigned i = 0; i < count; ++i)
        workers_.emplace_back([this] { worker_main(); });
}

DecodePool::~DecodePool() {
    {
        std::lock_guard<std::mutex> lock(job_m_);
        quit_ = true;
        jobs_.clear();
    }
    job_cv_.notify_all();
    for (std::thread& t : workers_)
        if (t.joinable()) t.join();
}

void DecodePool::abort() {
    abort_.store(true, std::memory_order_relaxed);
    job_cv_.notify_all();
}

void DecodePool::drain() {
    std::unique_lock<std::mutex> lock(job_m_);
    ++gen_;
    jobs_.clear();
    lock.unlock();
    job_cv_.notify_all();
    lock.lock();
    job_cv_.wait(lock, [this] { return busy_ == 0; });
}

void DecodePool::set_document(const doc::Document& doc, uint64_t look_id,
                              const std::vector<AssetBundle>& bundles,
                              uint64_t revision) {
    if (revision == revision_ && look_id == look_ &&
        same_bundles(bundles, bundles_))
        return;
    // A revision that left the SOURCE TABLE identical (param drags, block
    // Motion, effect edits) must not stall the decode workers - draining
    // per gesture frame is what made dragging hitch during playback.
    std::vector<doc::MediaInstance> next =
        doc::flatten_media_sources(doc, look_id);
    if (look_id == look_ && same_bundles(bundles, bundles_) &&
        next.size() == sources_.size()) {
        bool same = true;
        for (size_t i = 0; i < next.size(); ++i) {
            const doc::MediaInstance& a = next[i];
            const doc::MediaInstance& b = sources_[i];
            if (a.key != b.key || a.owner != b.owner || a.layer != b.layer ||
                a.asset != b.asset || a.t_in != b.t_in ||
                a.t_out != b.t_out || a.source_in != b.source_in ||
                a.speed != b.speed || a.rate != b.rate ||
                a.shift != b.shift || a.gain != b.gain) {
                same = false;
                break;
            }
        }
        if (same) {
            revision_ = revision;
            return;
        }
    }
    // No worker may be inside a stream while the table is rebuilt.
    drain();
    revision_ = revision;
    look_ = look_id;
    bundles_ = bundles;
    sources_ = std::move(next);

    // Stream identity: one stream per instance KEY - blocks of one look
    // share it, the position JUMPS at cuts, the file and decoder stay
    // open - with keys FOLDED together when their whole SCHEDULES match
    // (a matte arm mirroring its main layer, forked duplicates placed
    // identically: identical decode streams that were each paying every
    // keyframe roll). A schedule is the full (window, mapping) list, so
    // folded keys read the SAME frame at every root time; folding on
    // mappings alone merged keys active at different times with
    // different positions, and the shared want then ping-ponged every
    // collect - latching the backward-motion path during forward play.
    canonical_.clear();
    {
        std::unordered_map<uint64_t, std::vector<uint64_t>> key_maps;
        for (const doc::MediaInstance& c : sources_) {
            uint64_t sched = mapping_hash(c);
            sched ^= bits_of(c.t_in) + 0x9E3779B97F4A7C15ull +
                     (sched << 6) + (sched >> 2);
            sched ^= bits_of(c.t_out) + 0x9E3779B97F4A7C15ull +
                     (sched << 6) + (sched >> 2);
            key_maps[c.key].push_back(sched);
        }
        std::unordered_map<uint64_t, uint64_t> group_min;
        std::vector<std::pair<uint64_t, uint64_t>> key_fp;
        key_fp.reserve(key_maps.size());
        for (auto& e : key_maps) {
            std::vector<uint64_t>& maps = e.second;
            std::sort(maps.begin(), maps.end());
            maps.erase(std::unique(maps.begin(), maps.end()), maps.end());
            uint64_t fp = 0xCBF29CE484222325ull;
            for (uint64_t m : maps) fp = (fp ^ m) * 0x100000001B3ull;
            key_fp.emplace_back(e.first, fp);
            auto [g, fresh] = group_min.try_emplace(fp, e.first);
            if (!fresh && e.first < g->second) g->second = e.first;
        }
        for (const auto& e : key_fp) canonical_[e.first] = group_min[e.second];
        std::vector<uint64_t> distinct;
        for (const auto& e : canonical_) {
            bool have = false;
            for (uint64_t v : distinct)
                if (v == e.second) have = true;
            if (!have) distinct.push_back(e.second);
        }
        log_info("decode pool[%s]: %zu sources, %zu keys, %zu streams",
                 name_, sources_.size(), canonical_.size(), distinct.size());
    }
    std::lock_guard<std::mutex> lock(map_m_);
    for (auto it = streams_.begin(); it != streams_.end();) {
        const doc::MediaInstance* src = nullptr;
        for (const doc::MediaInstance& c : sources_) {
            auto ca = canonical_.find(c.key);
            if (ca != canonical_.end() && ca->second == it->first) src = &c;
        }
        const AssetBundle* b =
            src ? find_bundle(bundles_, src->asset) : nullptr;
        // A placement that moved to another asset - or vanished - must not
        // keep serving the old file's pixels.
        if (!b || bundle_video(*b) != it->second->path)
            it = streams_.erase(it);
        else
            ++it;
    }
}

std::vector<DecodePool::Request> DecodePool::plan(uint32_t root_frame) const {
    std::vector<Request> out;
    const double f = static_cast<double>(root_frame);
    for (size_t i = 0; i < sources_.size(); ++i) {
        const doc::MediaInstance& c = sources_[i];
        if (!doc::media_active(c, f)) continue;
        const AssetBundle* b = find_bundle(bundles_, c.asset);
        if (!b || bundle_video(*b).empty() || b->frames == 0) continue;
        // A placement may outlive its media (an explicit out point past
        // the end): it holds the last frame rather than going blank.
        const double src = doc::media_asset_frame(c, f);
        const double last = static_cast<double>(b->frames - 1);
        const uint32_t idx = static_cast<uint32_t>(
            std::clamp(src, 0.0, last));
        // Two overlapping placements of ONE asset on one track share a
        // key and a stream; only the compiler's winner (latest t_in,
        // later in the flatten breaking ties) renders, so decode that
        // one instead of flapping the ring between two frames.
        bool replaced = false;
        for (Request& r : out)
            if (r.key == c.key) {
                if (sources_[r.source].t_in <= c.t_in) {
                    r.source = i;
                    r.frame = idx;
                }
                replaced = true;
                break;
            }
        if (!replaced) {
            const auto ca = canonical_.find(c.key);
            out.push_back({c.key,
                           ca != canonical_.end() ? ca->second : c.key, i,
                           idx});
        }
    }
    return out;
}

void DecodePool::set_loop(uint32_t in_frame, uint32_t out_frame) {
    loop_in_ = in_frame;
    loop_out_ = out_frame;
}

DecodePool::Stream* DecodePool::stream_for(const Request& req) {
    const doc::MediaInstance& c = sources_[req.source];
    const AssetBundle* b = find_bundle(bundles_, c.asset);
    if (!b || bundle_video(*b).empty()) return nullptr;
    std::lock_guard<std::mutex> lock(map_m_);
    auto it = streams_.find(req.alias);
    if (it != streams_.end()) {
        it->second->last_touch = collect_gen_;
        it->second->idle_closed = false;
        return it->second.get();
    }
    auto s = std::make_unique<Stream>();
    s->key = req.alias;
    s->path = bundle_video(*b);
    s->native = !b->native.empty();
    s->frames = b->frames;
    s->last_touch = collect_gen_;
    log_info("decode pool[%s k%08llx]: stream opens - asset %llu "
             "t_in %.1f t_out %.1f src_in %.1f speed %.3f rate %.4f "
             "shift %lld",
             name_, static_cast<unsigned long long>(req.alias & 0xFFFFFFFFu),
             static_cast<unsigned long long>(c.asset), c.t_in,
             c.t_out >= 1e17 ? -1.0 : c.t_out, c.source_in, c.speed,
             c.rate, static_cast<long long>(c.shift));
    Stream* raw = s.get();
    streams_.emplace(req.alias, std::move(s));
    return raw;
}

// Releases what an idle stream holds open — two decoder sessions (an
// MFT and possibly a D3D device each), a FILE*, the mez reader bank and
// the ring's decoded frames — without ever blocking on a busy lock: a
// held session just means a stale prewarm worker is still inside, so
// skip and retry next collect. Idempotent; `idle_closed` latches only
// when every piece released.
void DecodePool::idle_close_scan() {
    std::lock_guard<std::mutex> lock(map_m_);
    for (auto& entry : streams_) {
        Stream& s = *entry.second;
        if (s.idle_closed ||
            collect_gen_ - s.last_touch < kIdleCloseCollects)
            continue;
        bool all = true;
        for (Stream::Session& c : s.sessions) {
            std::unique_lock<std::mutex> sl(c.m, std::try_to_lock);
            if (!sl.owns_lock()) {
                all = false;
                continue;
            }
            if (!c.created) continue;
            c.dec.destroy();
            if (c.file) {
                std::fclose(static_cast<FILE*>(c.file));
                c.file = nullptr;
            }
            c.created = false;
            c.ok = false;
            c.next_decode = 0;
            c.next_present.store(-1, std::memory_order_relaxed);
            c.run_key.store(-1, std::memory_order_relaxed);
            c.draining = false;
            c.scratch = {};
        }
        for (Stream::Slot& slot : s.slots) {
            std::unique_lock<std::mutex> sl(slot.m, std::try_to_lock);
            if (!sl.owns_lock()) {
                all = false;
                continue;
            }
            if (!slot.opened) continue;
            slot.reader.close();
            slot.opened = false;
            slot.ok = false;
        }
        {
            std::unique_lock<std::mutex> sl(s.m, std::try_to_lock);
            if (sl.owns_lock()) {
                s.ring.clear();
                s.spare.clear();
                s.last_payload_frame.reset();
                s.last_payload_off = 0;
            } else {
                all = false;
            }
        }
        if (all) s.idle_closed = true;
    }
}

std::shared_ptr<const codec::DecodedFrame> DecodePool::ring_insert(
    Stream& s, uint32_t frame,
    std::shared_ptr<const codec::DecodedFrame> decoded, size_t depth) {
    std::lock_guard<std::mutex> lock(s.m);
    // A parallel slot/session may have landed the same frame; keep one.
    for (const auto& e : s.ring)
        if (e.first == frame) return e.second;
    s.ring.emplace_back(frame, decoded);
    // Evict against the DIRECTION OF TRAVEL. The live window ahead of
    // the playhead — in whichever direction it moves — is untouchable:
    // evicting a frame it is about to read turns into a re-ask, and a
    // re-ask off the ring is a whole keyframe roll. Frames the playhead
    // is moving AWAY from weigh 4x, so the other direction's leftovers
    // yield first. A forward-only rule here shredded the reverse window
    // fetch_native had just widened (it protected already-shown frames
    // and evicted the backward tail first), so backward playback
    // re-rolled the same run every few frames.
    const bool backward = s.want < s.last_want;
    if (s.ring_target > depth) depth = s.ring_target;
    while (s.ring.size() > std::max<size_t>(depth, 1)) {
        size_t worst = SIZE_MAX;
        uint32_t worst_d = 0;
        for (size_t i = 0; i < s.ring.size(); ++i) {
            const uint32_t idx = s.ring[i].first;
            if (!backward && idx >= s.want && idx <= s.want + kDecodeAhead)
                continue;
            if (backward && idx <= s.want && s.want - idx <= kDecodeAhead)
                continue;
            // The upcoming cuts' landing zones are as live as the
            // playhead's own window - at the GUARD width (2x the decode
            // span), because the entry roll's drain extras land past
            // entry+span and the post-cut frontier continues from them:
            // a narrower window here evicted exactly the frame past it,
            // and the cut then raced a full keyframe restart for the
            // hole, every lap.
            bool marked_live = false;
            for (const uint32_t ne : s.next_entries)
                if (ne != 0xFFFFFFFFu && idx >= ne &&
                    idx <= ne + 2 * kDecodeAhead) {
                    marked_live = true;
                    break;
                }
            if (marked_live) continue;
            uint32_t d;
            if (backward)
                d = idx <= s.want ? s.want - idx : (idx - s.want) * 4;
            else
                d = idx >= s.want ? idx - s.want : (s.want - idx) * 4;
            if (d >= worst_d) {
                worst_d = d;
                worst = i;
            }
        }
        if (worst == SIZE_MAX) break;   // everything held is live
        // Nobody else holds the victim: recycle its buffers.
        if (s.ring[worst].second.use_count() == 1 && s.spare.size() < 8)
            s.spare.push_back(std::const_pointer_cast<codec::DecodedFrame>(
                s.ring[worst].second));
        s.ring.erase(s.ring.begin() + static_cast<ptrdiff_t>(worst));
    }
    s.cv.notify_all();
    return decoded;
}

// A recycled frame if one is waiting; its vectors keep their capacity.
std::shared_ptr<codec::DecodedFrame> DecodePool::take_spare(Stream& s) {
    std::lock_guard<std::mutex> lock(s.m);
    if (s.spare.empty()) return nullptr;
    auto f = std::move(s.spare.back());
    s.spare.pop_back();
    return f;
}

std::shared_ptr<const codec::DecodedFrame> DecodePool::fetch(
    Stream& s, uint32_t frame, bool* was_miss, bool scrub, bool preroll) {
    {
        std::lock_guard<std::mutex> lock(s.m);
        for (const auto& e : s.ring)
            if (e.first == frame) return e.second;
    }
    if (was_miss) *was_miss = true;
    // was_miss == nullptr is the prewarm path: advisory work that must
    // never park a worker behind a busy session - the job re-queues next
    // collect if the frame still matters.
    return s.native
               ? fetch_native(s, frame, was_miss == nullptr, scrub, preroll)
               : fetch_mez(s, frame);
}

std::shared_ptr<const codec::DecodedFrame> DecodePool::fetch_mez(
    Stream& s, uint32_t frame) {
    // Grab a FREE reader slot so concurrent decodes of one stream run
    // in parallel; only when the whole bank is busy does this wait, on
    // a frame-hashed slot, at most one decode deep.
    Stream::Slot* slot = nullptr;
    std::unique_lock<std::mutex> dlock;
    for (Stream::Slot& c : s.slots) {
        std::unique_lock<std::mutex> attempt(c.m, std::try_to_lock);
        if (attempt.owns_lock()) {
            slot = &c;
            dlock = std::move(attempt);
            break;
        }
    }
    if (!slot) {
        slot = &s.slots[frame % Stream::kSlots];
        dlock = std::unique_lock<std::mutex>(slot->m);
    }
    {
        std::lock_guard<std::mutex> lock(s.m);
        for (const auto& e : s.ring)
            if (e.first == frame) return e.second;   // landed while waiting
    }
    if (!slot->opened) {
        slot->opened = true;
        std::string error;
        slot->ok = slot->reader.open(s.path, &error);
        if (!slot->ok)
            log_warn("decode pool: open failed (%s)", error.c_str());
    }
    if (!slot->ok) return nullptr;
    // Hold-frame stills point many frames at ONE payload: alias the
    // pixels decoded last time instead of re-decoding an identical
    // payload every timeline frame (a 4K still would otherwise pay a
    // full decode per frame, forever). Shared pixels share the stamp,
    // so the engine's upload skip rides along.
    const uint64_t off = slot->reader.payload_offset(frame);
    if (off) {
        std::shared_ptr<const codec::DecodedFrame> alias;
        {
            std::lock_guard<std::mutex> lock(s.m);
            if (off == s.last_payload_off) alias = s.last_payload_frame;
        }
        if (alias)
            return ring_insert(s, frame, std::move(alias),
                               std::max<uint32_t>(ring_depth_, 1));
    }
    codec::DecodedFrame decoded;
    if (!slot->reader.decode(frame, decoded)) {
        log_warn("decode pool: decode failed for frame %u", frame);
        return nullptr;
    }
    decoded.stamp = g_frame_stamp.fetch_add(1, std::memory_order_relaxed);
    auto shared =
        std::make_shared<const codec::DecodedFrame>(std::move(decoded));
    {
        std::lock_guard<std::mutex> lock(s.m);
        s.last_payload_off = off;
        s.last_payload_frame = shared;
    }
    return ring_insert(s, frame, std::move(shared),
                       std::max<uint32_t>(ring_depth_, 1));
}

// Native H.264: acquire a session, roll it to the frame. Every frame
// decoded on the way rings, so a scrub inside one keyframe run is all
// hits after the first landing and a backward walk serves from the
// window a forward roll just filled. Advisory (prewarm) calls never
// wait: a parked worker serves nobody, and the job re-queues anyway.
std::shared_ptr<const codec::DecodedFrame> DecodePool::fetch_native(
    Stream& s, uint32_t frame, bool advisory, bool scrub, bool preroll) {
    // COM/MF per thread that touches the decoder MFTs (consumer thread
    // on a blocking miss, workers on prewarm). Refcounted by the OS.
    thread_local platform::MfSession mf_session;
    {
        std::lock_guard<std::mutex> lock(s.index_m);
        if (!s.index_built) {
            s.index_built = true;
            std::string error;
            s.index_ok = load_frame_index(s.path, &s.index, &error);
            if (!s.index_ok)
                log_warn("decode pool: frame index failed (%s)",
                         error.c_str());
        }
    }
    if (!s.index_ok || s.index.frame_count() == 0) return nullptr;
    const FrameIndex& idx = s.index;
    const uint32_t target = std::min(frame, idx.frame_count() - 1);
    const uint32_t key = idx.keyframe_before(target);

    // A consumer miss in steady playback is always the smoking gun of a
    // supply bug; name the ring's actual content when it happens.
    if (!advisory) {
        uint32_t lo = ~0u, hi = 0, n = 0;
        uint32_t want_now = 0, tgt_now = 0;
        uint32_t m0 = 0xFFFFFFFFu, m1 = 0xFFFFFFFFu;
        // Nearest ringed frames around the miss: the gap edges say
        // whether the hole was never decoded or evicted from a span.
        uint32_t below = 0, above = ~0u;
        {
            std::lock_guard<std::mutex> lock(s.m);
            for (const auto& e : s.ring) {
                lo = std::min(lo, e.first);
                hi = std::max(hi, e.first);
                if (e.first < target) below = std::max(below, e.first);
                if (e.first > target) above = std::min(above, e.first);
                ++n;
            }
            want_now = s.want;
            tgt_now = s.ring_target;
            m0 = s.next_entries[0];
            m1 = s.next_entries[1];
        }
        log_info("decode pool[%s k%08llx]: consumer miss %u (ring %u..%u n%u "
                 "gap %u..%u, want %u tgt %u marks %d/%d, "
                 "s0 np %lld rk %lld, s1 np %lld rk %lld, s2 np %lld rk %lld)",
                 name_, static_cast<unsigned long long>(s.key & 0xFFFFFFFFu),
                 target, n ? lo : 0, n ? hi : 0, n, below,
                 above == ~0u ? 0 : above, want_now, tgt_now,
                 m0 == 0xFFFFFFFFu ? -1 : static_cast<int>(m0),
                 m1 == 0xFFFFFFFFu ? -1 : static_cast<int>(m1),
                 static_cast<long long>(s.sessions[0].next_present.load(
                     std::memory_order_relaxed)),
                 static_cast<long long>(s.sessions[0].run_key.load(
                     std::memory_order_relaxed)),
                 static_cast<long long>(s.sessions[1].next_present.load(
                     std::memory_order_relaxed)),
                 static_cast<long long>(s.sessions[1].run_key.load(
                     std::memory_order_relaxed)),
                 static_cast<long long>(s.sessions[2].next_present.load(
                     std::memory_order_relaxed)),
                 static_cast<long long>(s.sessions[2].run_key.load(
                     std::memory_order_relaxed)));
    }

    // SCRUB service, consumer side: NEVER decode mid-drag — serve the
    // nearest ringed frame whatever its distance (it is the best content
    // that exists; the chaser is closing the gap at full decode rate on
    // a worker session) and raise `approximated_` so the caller keeps
    // re-collecting until the ask comes back exact. An empty ring falls
    // through to the bounded slice below to seed it.
    if (scrub && !advisory) {
        std::lock_guard<std::mutex> lock(s.m);
        uint32_t best_d = ~0u;
        std::shared_ptr<const codec::DecodedFrame> best;
        for (const auto& e : s.ring) {
            const uint32_t d = e.first > target ? e.first - target
                                                : target - e.first;
            if (d < best_d) {
                best_d = d;
                best = e.second;
            }
        }
        if (best) {
            approximated_ = true;
            return best;
        }
    }

    // Session pick, routed by KEYFRAME RUN: the session inside the
    // target's run owns every ask in it - waiting on the owner costs the
    // tail of its roll, while grabbing the idle session would re-decode
    // the same run from its keyframe CONCURRENTLY, and that duplicate
    // 4K roll (two software decoders thrashing the cores) is what turned
    // one miss into a multi-second stall.
    Stream::Session* sess = nullptr;
    std::unique_lock<std::mutex> slock;
    Stream::Session* owner = nullptr;
    int64_t owner_np = -1;
    // Supply positions the stream depends on: the live window, and the
    // landing zone of every marked upcoming entry - a session parked
    // there IS the cut's warm supply, and it must keep extending in
    // place when the cut arrives.
    uint32_t live_want = 0;
    uint32_t guard_marks[Stream::kEntryMarks];
    bool mark_ringed[Stream::kEntryMarks] = {};
    {
        std::lock_guard<std::mutex> lock(s.m);
        live_want = s.want;
        for (size_t i = 0; i < Stream::kEntryMarks; ++i) {
            guard_marks[i] = s.next_entries[i];
            // A mark guards only what actually EXISTS: a session can
            // sit inside a mark's window by accident (old playback
            // parked it there, the frames long evicted), and a
            // position-only guard then blocked the real entry preroll
            // from ever claiming a session - the cut went cold with a
            // "supplier" pointing at nothing.
            if (guard_marks[i] != 0xFFFFFFFFu)
                for (const auto& e : s.ring)
                    if (e.first == guard_marks[i]) {
                        mark_ringed[i] = true;
                        break;
                    }
        }
    }
    // What a session's position is guarding: -1 = nothing, 0 = the live
    // want window, else the MARK whose landing it holds. Guards yield
    // in DEADLINE ORDER: an ask for a NEARER window may claim a session
    // holding a farther mark - a dense look can have more upcoming cuts
    // than sessions, and without the yield the nearest entry went cold
    // while sessions sat pinned on cuts several blocks out.
    auto guard_of = [&](int64_t np) -> int64_t {
        if (np < 0) return -1;
        if (np >= static_cast<int64_t>(live_want) &&
            np <= static_cast<int64_t>(live_want) + 2 * kDecodeAhead)
            return 0;
        for (size_t i = 0; i < Stream::kEntryMarks; ++i) {
            const uint32_t m = guard_marks[i];
            if (m != 0xFFFFFFFFu && mark_ringed[i] &&
                np >= static_cast<int64_t>(m) &&
                np <= static_cast<int64_t>(m) + 2 * kDecodeAhead)
                return static_cast<int64_t>(m);
        }
        return -1;
    };
    // Immune to THIS ask: guarding the live window, or a mark at or
    // before the ask's own target (serving a nearer or equal deadline).
    auto guarded_np = [&](int64_t np) {
        const int64_t g = guard_of(np);
        return g == 0 ||
               (g > 0 && g <= static_cast<int64_t>(target));
    };
    for (Stream::Session& c : s.sessions) {
        const int64_t np = c.next_present.load(std::memory_order_relaxed);
        // A session owns the ask when continuing REACHES it: emissions
        // are at or behind the target and the target's keyframe is at or
        // behind the FEED cursor's run. run_key tracks feeds, which lead
        // emissions by the decoder's in-flight depth - demanding equal
        // runs here rejected the session that was already decoding the
        // target's run and handed the ask to its lagging sibling, and
        // the two then leapfrog-decoded every run twice. Closest
        // emissions win the tie.
        //
        // EXCEPT: a PREROLL far ahead of a guarded session must not
        // continue it. The extension rolls through the whole connective
        // span (self-evicting most of it) and reparks the session at
        // its own target, so the window it was supplying - the live
        // frontier, or a nearer cut's landing - has to restart from the
        // keyframe, a race the consumer won at every block seam. A
        // fresh roll on another session costs the same decode and
        // leaves the supply extending in place.
        if (preroll && guarded_np(np) &&
            static_cast<int64_t>(target) - np > 2 * kDecodeAhead)
            continue;
        if (np >= 0 && np <= static_cast<int64_t>(target) &&
            c.run_key.load(std::memory_order_relaxed) >=
                static_cast<int64_t>(key) &&
            np > owner_np) {
            owner = &c;
            owner_np = np;
        }
    }
    if (owner) {
        std::unique_lock<std::mutex> attempt(owner->m, std::try_to_lock);
        if (!attempt.owns_lock()) {
            if (advisory) {
                // Busy owner: ONE worker may wait its turn (it extends
                // the ring the moment the roll ends); the rest skip.
                if (owner->waiter.exchange(true, std::memory_order_acquire))
                    return nullptr;
                attempt = std::unique_lock<std::mutex>(owner->m);
                owner->waiter.store(false, std::memory_order_release);
            } else {
                // The owner is mid-roll TOWARD this frame: wait on the
                // ring it feeds, retrying the session in short slices in
                // case its roll ends short of the target.
                for (int spin = 0; spin < 10; ++spin) {
                    std::shared_ptr<const codec::DecodedFrame> hit;
                    {
                        std::unique_lock<std::mutex> rlock(s.m);
                        s.cv.wait_for(rlock, std::chrono::milliseconds(25),
                                      [&] {
                                          for (const auto& e : s.ring)
                                              if (e.first == target) {
                                                  hit = e.second;
                                                  return true;
                                              }
                                          return false;
                                      });
                    }
                    if (hit) return hit;
                    std::unique_lock<std::mutex> retry(owner->m,
                                                       std::try_to_lock);
                    if (retry.owns_lock()) {
                        attempt = std::move(retry);
                        break;
                    }
                }
                if (!attempt.owns_lock())
                    attempt = std::unique_lock<std::mutex>(owner->m);
            }
        }
        slock = std::move(attempt);
        sess = owner;
    }
    if (!sess) {
        // No owner means this ask RESTARTS whichever session takes it
        // (flush + reseek) unless that session happens to continue.
        // Rank the lockable candidates: CONTINUE beats everything (no
        // flush at all), an IDLE session beats restarting a positioned
        // one, and a GUARDED session - one supplying the live window or
        // a marked entry's landing - is last resort: a preroll or
        // backfill restarting the supplier re-rolled its window from
        // the keyframe and the consumer starved behind it. Advisories
        // never take a guarded one (next collect retries); only a
        // blocking consumer ask may.
        Stream::Session* live_sess = nullptr;
        std::unique_lock<std::mutex> live_lock;
        Stream::Session* idle_sess = nullptr;
        std::unique_lock<std::mutex> idle_lock;
        Stream::Session* stale_sess = nullptr;
        std::unique_lock<std::mutex> stale_lock;
        // Guarding a FARTHER mark than this ask's target: yielded only
        // when nothing unguarded is free, restarting the least-imminent
        // supply in deadline order.
        Stream::Session* yield_sess = nullptr;
        std::unique_lock<std::mutex> yield_lock;
        for (Stream::Session& c : s.sessions) {
            std::unique_lock<std::mutex> attempt(c.m, std::try_to_lock);
            if (!attempt.owns_lock()) continue;
            const int64_t np =
                c.next_present.load(std::memory_order_relaxed);
            // A continuation is the cheapest service - unless it is a
            // far preroll CONTINUING a guarded session, which reparks
            // the supply exactly like a restart would (the owner scan
            // diverts that case and this ranking must not re-grab it).
            const bool would_cont = np >= 0 && !c.draining &&
                                    np <= static_cast<int64_t>(target) &&
                                    key < c.next_decode &&
                                    !(preroll && guarded_np(np) &&
                                      static_cast<int64_t>(target) - np >
                                          2 * kDecodeAhead);
            if (would_cont) {
                sess = &c;
                slock = std::move(attempt);
                break;
            }
            if (np < 0) {
                if (!idle_sess) {
                    idle_sess = &c;
                    idle_lock = std::move(attempt);
                }
            } else if (guarded_np(np)) {
                if (!live_sess) {
                    live_sess = &c;
                    live_lock = std::move(attempt);
                }
            } else if (guard_of(np) < 0) {
                if (!stale_sess) {
                    stale_sess = &c;
                    stale_lock = std::move(attempt);
                }
            } else if (!yield_sess) {
                yield_sess = &c;
                yield_lock = std::move(attempt);
            }
        }
        if (!sess && idle_sess) {
            sess = idle_sess;
            slock = std::move(idle_lock);
        }
        if (!sess && stale_sess) {
            sess = stale_sess;
            slock = std::move(stale_lock);
        }
        if (!sess && yield_sess) {
            sess = yield_sess;
            slock = std::move(yield_lock);
        }
        if (!sess && live_sess && !advisory) {
            sess = live_sess;
            slock = std::move(live_lock);
        }
    }
    if (!sess) {
        if (advisory) return nullptr;   // all busy - do not convoy
        sess = &s.sessions[target % Stream::kSessions];
        slock = std::unique_lock<std::mutex>(sess->m);
    }
    {
        std::lock_guard<std::mutex> lock(s.m);
        for (const auto& e : s.ring)
            if (e.first == target) return e.second;   // landed while waiting
    }

    if (!sess->created) {
        sess->created = true;
        std::string error;
        // DXVA when the machine has it: 4K60 software decode cannot hold
        // the media rate (measured ~34 ms/frame on high-bitrate game
        // capture, twice the 60fps budget); hardware decode is the
        // throughput path and try_d3d falls back to software silently.
        sess->ok = sess->dec.create(idx.avcc, idx.width, idx.height, &error,
                                    /*allow_d3d=*/true,
                                    /*low_latency=*/true);
        if (sess->ok) {
            sess->file = _wfopen(s.path.c_str(), L"rb");
            if (!sess->file) {
                sess->ok = false;
                error = "cannot open source";
            }
        }
        if (!sess->ok)
            log_warn("decode pool: native session failed (%s)",
                     error.c_str());
    }
    if (!sess->ok) return nullptr;

    // Continue when the target's keyframe is already behind the cursor
    // and the target itself is not; otherwise flush and reseek.
    const int64_t np = sess->next_present.load(std::memory_order_relaxed);
    const bool cont = np >= 0 && !sess->draining &&
                      np <= static_cast<int64_t>(target) &&
                      key < sess->next_decode;
    const uint32_t floor_present =
        cont ? static_cast<uint32_t>(np) : idx.decode_to_present[key];
    // Prewarm pays for CONTINUATION freely (never duplicate work) but a
    // cold advisory SEEK only when it starts near its keyframe - that is
    // the next-run preroll. A mid-run advisory re-decode floods the ring
    // with frames the playhead already passed; if the playhead truly
    // needs one of those, the consumer's blocking ask decides it. A
    // BACKWARD walk's backfill is exempt: its target sits deep in the
    // previous run by construction, and the roll rings exactly the
    // frames the descending playhead reads next.
    bool backfill = false;
    {
        std::lock_guard<std::mutex> lock(s.m);
        backfill = s.want < s.last_want && target < s.want;
    }
    // A scrub CHASER (an advisory queued during the live drag) is
    // exempt from the cold-seek gate: its whole job is a mid-run roll
    // to wherever the drag points. A CUT PREROLL is exempt too: the
    // upcoming block's entry lands wherever the arrangement put it -
    // usually deep inside a run on long-GOP sources - and refusing the
    // roll here is what made every cut a blocking consumer seek.
    const bool chaser =
        advisory && scrub_.load(std::memory_order_relaxed) &&
        t_job_epoch == scrub_epoch_.load(std::memory_order_relaxed);
    if (advisory && !backfill && !chaser && !preroll &&
        target - floor_present > (cont ? 48u : kDecodeAhead))
        return nullptr;

    // SCRUB service: mid-drag every position is disposable, so an ask
    // that would cost a long roll serves the roll's FLOOR instead — the
    // run's sync point on a cold seek, the cursor's next emission on a
    // continuation. One feed instead of a GOP, so the preview tracks
    // the drag; the gesture's end re-renders the exact frame, and the
    // caller gates its frame cache while a drag is live.
    constexpr uint32_t kScrubSnapRoll = 6;
    uint32_t serve = target;
    if (scrub && !advisory && target - floor_present > kScrubSnapRoll) {
        // One bounded slice per pass: the re-collect loop turns these
        // into a visible march toward the true frame.
        serve = floor_present + kScrubSnapRoll;
        approximated_ = true;
        std::lock_guard<std::mutex> lock(s.m);
        for (const auto& e : s.ring)
            if (e.first == serve) return e.second;
    }
    const uint32_t serve_decode = idx.present_to_decode[serve];

    if (!cont) {
        sess->dec.flush();
        sess->draining = false;
        sess->next_decode = key;
        sess->next_present.store(idx.decode_to_present[key],
                                 std::memory_order_relaxed);
        sess->run_key.store(key, std::memory_order_relaxed);
    }
    const auto roll_t0 = std::chrono::steady_clock::now();
    const uint32_t fed_from = sess->next_decode;
    const size_t session_index =
        static_cast<size_t>(sess - &s.sessions[0]);

    // Ring depth for this roll: backward motion keeps the top half of
    // the run (bounded) so the next asks serve from the ring.
    size_t depth = std::max<uint32_t>(ring_depth_, 1);
    {
        std::lock_guard<std::mutex> lock(s.m);
        if (s.want < s.last_want) {
            const uint32_t roll = serve - floor_present + 1;
            // Widen toward the reverse window but never shrink an
            // already-deeper ring (depth can exceed the window since
            // the frame budget grew - a clamp with lo > hi asserts).
            depth = std::max(depth,
                             std::min<size_t>(roll / 2 + 1, kReverseWindow));
        }
    }

    std::shared_ptr<const codec::DecodedFrame> result;
    std::vector<uint8_t> sample_bytes;
    platform::VideoFrameNV12& nv12 = sess->scratch;
    // Emissions are labeled by ARRIVAL ORDER from the roll's floor -
    // presentation order is guaranteed from a sync point, while output
    // timestamps are not: the decoder's first post-flush stamp can be
    // garbage (measured: 0), and trusting it skipped the keyframe itself
    // as a straggler, burning a full roll to miss its own target. The
    // stamp only overrides the counter when it is sane (inside the
    // file's real timeline) and clearly names a different frame - that
    // is a decoder legitimately dropping undecodable pictures.
    int64_t expect = floor_present;
    const int64_t half_dur = idx.timescale
        ? static_cast<int64_t>(5.0e6 * idx.frame_duration / idx.timescale)
        : 0;
    bool hold_was_dry = false;
    while (!result) {
        if (abort_.load(std::memory_order_relaxed)) return nullptr;
        // A drag started AFTER this advisory was queued: bail mid-roll.
        // Pre-drag speculation holding a session is exactly what starved
        // scrubs; the drag's own chasers carry the current epoch and
        // roll through.
        if (advisory && scrub_.load(std::memory_order_relaxed) &&
            t_job_epoch != scrub_epoch_.load(std::memory_order_relaxed))
            return nullptr;
        bool held = false;
        if (sess->next_decode < idx.frame_count() &&
            sess->next_decode <= serve_decode + kRollGuard) {
            const FrameIndex::Sample& sm = idx.samples[sess->next_decode];
            // A keyframe is a free seek point: once the target's sample
            // is fed, never feed into the NEXT run - the in-flight
            // frames drain out below, this session hands the new run to
            // its idle sibling fresh (GOP-striped supply), and nothing
            // is decoded twice. If a hold pass yields no emission the
            // pipeline needs a push, so feed anyway.
            if (sm.keyframe && sess->next_decode > key &&
                sess->next_decode > serve_decode && !hold_was_dry) {
                held = true;
            } else if (!read_sample_at(static_cast<FILE*>(sess->file), sm,
                                       sample_bytes) ||
                       !sess->dec.feed(sample_bytes.data(),
                                       sample_bytes.size(), sm.pts_100ns,
                                       sm.duration_100ns, sm.keyframe)) {
                log_warn("decode pool: native feed failed at sample %u",
                         sess->next_decode);
                sess->next_present.store(-1, std::memory_order_relaxed);
                return nullptr;
            } else {
                // Crossing into the next keyframe run hands the session
                // its ownership (the routing hint the pick loop reads).
                if (sm.keyframe)
                    sess->run_key.store(sess->next_decode,
                                        std::memory_order_relaxed);
                ++sess->next_decode;
            }
        } else if (!sess->draining) {
            sess->dec.drain();
            sess->draining = true;
        } else {
            break;   // drained dry without the target
        }
        bool emitted = false;
        while (sess->dec.receive(nv12)) {
            emitted = true;
            uint32_t p = static_cast<uint32_t>(
                std::min<int64_t>(expect, idx.frame_count() - 1));
            const int64_t pts = nv12.pts_100ns;
            if (std::llabs(pts - idx.present_pts[p]) > half_dur &&
                pts + half_dur >= idx.present_pts[floor_present] &&
                pts <= idx.present_pts.back() + half_dur)
                p = idx.present_of_pts(pts);
            expect = static_cast<int64_t>(p) + 1;
            sess->next_present.store(expect, std::memory_order_relaxed);
            if (p < floor_present) continue;   // pre-keyframe stragglers
            auto kept = ring_insert(
                s, p, nv12_to_decoded(take_spare(s), nv12), depth);
            if (p == serve) result = std::move(kept);
        }
        hold_was_dry = held && !emitted;
    }
    if (sess->draining)
        sess->next_present.store(-1, std::memory_order_relaxed);
    if (!result && !abort_.load(std::memory_order_relaxed))
        log_warn("decode pool: native roll missed frame %u", serve);
    // Every expensive roll names itself in looks.log: which session, why
    // it could not continue, how far it fed, what it cost. Hitches are
    // rare by construction, so this stays quiet in steady playback.
    const double roll_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - roll_t0)
                               .count();
    if (roll_ms >= 100.0) {
        const Stream::Session& other =
            s.sessions[(session_index + 1) % Stream::kSessions];
        log_info("decode pool[%s k%08llx]: s%zu %s%s roll %u frames %.0f ms -> frame "
                 "%u (key %u, was np %lld, other np %lld run %lld)",
                 name_, static_cast<unsigned long long>(s.key & 0xFFFFFFFFu),
                 session_index, advisory ? "prewarm " : "",
                 cont ? "cont" : "seek", sess->next_decode - fed_from,
                 roll_ms, serve, key, static_cast<long long>(np),
                 static_cast<long long>(
                     other.next_present.load(std::memory_order_relaxed)),
                 static_cast<long long>(
                     other.run_key.load(std::memory_order_relaxed)));
    }
    return result;
}

const std::vector<SourceFrame>& DecodePool::collect(uint32_t root_frame,
                                                    bool scrub) {
    frames_.clear();
    // The idle clock FREEZES during a scrub: the prewarm section below
    // is skipped mid-drag, so nothing refreshes last_touch, and a long
    // drag aged every stream but the dragged one into idle-close - the
    // whole pool then re-opened cold when playback resumed.
    if (!scrub) ++collect_gen_;
    approximated_ = false;
    if (scrub && !prev_scrub_)
        scrub_epoch_.fetch_add(1, std::memory_order_relaxed);
    prev_scrub_ = scrub;
    scrub_.store(scrub, std::memory_order_relaxed);
    const std::vector<Request> want = plan(root_frame);
    // Depth must clear the prewarm span with SLACK: a ring sized exactly
    // to span+tail evicts decoded-ahead frames the playhead has not
    // reached yet, and every one of those comes back as a re-decode.
    // The budget divides across STREAMS, not requests - folded keys
    // (arms, locked feeds) share a stream and must not shrink it.
    size_t distinct = 0;
    {
        uint64_t seen[16];
        for (const Request& r : want) {
            bool have = false;
            for (size_t i = 0; i < distinct && i < 16; ++i)
                if (seen[i] == r.alias) {
                    have = true;
                    break;
                }
            if (!have && distinct < 16) seen[distinct++] = r.alias;
        }
    }
    ring_depth_ = distinct == 0
        ? 4
        : static_cast<uint32_t>(
              std::clamp(kFrameBudget / distinct, size_t{2}, size_t{40}));

    for (const Request& req : want) {
        // Same-alias requests read the same media frame: one fetch
        // serves them all under their own keys.
        std::shared_ptr<const codec::DecodedFrame> served;
        for (const SourceFrame& sf : frames_)
            if (sf.key != req.key) {
                for (const Request& prior : want) {
                    if (prior.key != sf.key) continue;
                    if (prior.alias == req.alias &&
                        prior.frame == req.frame)
                        served = sf.frame;
                    break;
                }
                if (served) break;
            }
        if (served) {
            frames_.push_back({req.key, std::move(served)});
            continue;
        }
        Stream* s = stream_for(req);
        if (!s) continue;
        {
            std::lock_guard<std::mutex> lock(s->m);
            // Direction memory for the reverse-roll policy; a repeated
            // ask (paused playhead) keeps the last real direction.
            if (req.frame != s->want) {
                s->last_want = s->want;
                s->want = req.frame;
            }
        }
        bool miss = false;
        auto frame = fetch(*s, req.frame, &miss, scrub);
        if (miss) ++misses_;
        if (frame) frames_.push_back({req.key, std::move(frame)});
    }

    // PREWARM. What plays later is a closed form, so the frames a cut or a
    // nested look's in-point will need are known now: queue them nearest
    // first. Streams that only prewarm are capped, so a pathological
    // arrangement cannot open unbounded files. The span never exceeds
    // what the ring can HOLD next to the current frame - prewarming past
    // it decodes frames that evict themselves, then re-queues them every
    // cycle: permanent decode churn whose stream-lock traffic stalled
    // collect() by the whole backlog. Frames already ringed (or already
    // queued - slowed sources repeat source frames) never re-queue, so a
    // settled steady state queues NOTHING. A SCRUB queues nothing either
    // — the drag's next position is unknowable, and speculative rolls
    // saturating the sessions behind it are what the drag then waits on
    // (the gen bump below still cancels whatever an earlier plan queued).
    std::vector<Job> queued;
    // Entry protection collected FIRST, written ONCE per stream at the
    // end: resetting next_entry then re-marking it left a gap between
    // the two writes, and worker ring-inserts landing inside it evicted
    // the protected entry window - the preroll then re-rolled the same
    // frames every collect, starving the live supply off the sessions.
    std::vector<std::pair<uint64_t, uint32_t>> entry_marks;
    // Streams whose want anchored to their nearest upcoming entry this
    // collect: a SECOND upcoming block of the same stream must protect
    // through a mark instead of stealing the anchor.
    std::vector<uint64_t> want_anchored;
    const uint32_t span = std::min(
        kDecodeAhead, ring_depth_ > 1 ? ring_depth_ - 1 : 1u);
    if (!scrub) {
    // ONE standing job per placement: advance its stream to the span
    // FRONTIER. The worker's roll rings every frame on the way, so one
    // job replaces a span of one-frame jobs that each fought the
    // consumer for the session lock - and a cut preroll falls out of
    // the same shape (the roll starts at the entry frame's keyframe).
    //
    // The horizon gains a WRAP window when looping playback approaches
    // the loop end: what plays after the wrap is the loop start, and
    // without warming it every lap re-entered cold. `bias` keeps job
    // deadlines in frames-until-play across both windows.
    struct Horizon {
        double lo, hi, bias;
    };
    Horizon horizons[2];
    size_t horizon_count = 1;
    horizons[0] = {static_cast<double>(root_frame) + 1.0,
                   static_cast<double>(root_frame) + kPrewarmFrames, 0.0};
    if (loop_out_ > loop_in_ && root_frame < loop_out_ &&
        static_cast<double>(root_frame) + kPrewarmFrames >=
            static_cast<double>(loop_out_)) {
        const double over = static_cast<double>(root_frame) +
                            kPrewarmFrames -
                            static_cast<double>(loop_out_);
        const double hi = std::min(static_cast<double>(loop_in_) + over,
                                   static_cast<double>(loop_out_) - 1.0);
        if (hi >= static_cast<double>(loop_in_))
            horizons[horizon_count++] = {
                static_cast<double>(loop_in_), hi,
                static_cast<double>(loop_out_) -
                    static_cast<double>(root_frame)};
    }
    for (size_t h = 0; h < horizon_count; ++h)
    for (size_t i = 0; i < sources_.size(); ++i) {
        const doc::MediaInstance& c = sources_[i];
        const double first = std::max(horizons[h].lo, std::ceil(c.t_in));
        if (first >= c.t_out || first > horizons[h].hi) continue;
        const AssetBundle* b = find_bundle(bundles_, c.asset);
        if (!b || bundle_video(*b).empty() || b->frames == 0) continue;
        double at = std::max(
            first, std::min(first + span - 1.0, c.t_out - 1.0));
        // Wrap-window frontiers stop at the loop end: frames past it
        // play a full lap later, not next.
        if (h > 0)
            at = std::max(first,
                          std::min(at, static_cast<double>(loop_out_) - 1.0));
        const double src = doc::media_asset_frame(c, at);
        Request req;
        req.key = c.key;
        const auto ca = canonical_.find(c.key);
        req.alias = ca != canonical_.end() ? ca->second : c.key;
        req.source = i;
        req.frame = static_cast<uint32_t>(std::clamp(
            src, 0.0, static_cast<double>(b->frames - 1)));
        // An INSTANCE that has not started yet is a CUT PREROLL - even
        // when its key is already playing through an earlier block
        // (blocks of one look share a stream whose position JUMPS at
        // the cut). Prerolls are exempt from the cold-seek gate; the
        // entry anchors the stream's eviction window - either as
        // `want` (stream not playing at all) or as `next_entry`
        // (playing: the consumer owns want, the landing zone is
        // protected alongside it).
        // Upcoming until the block has actually STARTED (t_in > root),
        // not merely until its entry is the next frame: dropping the
        // mark at root = t_in-1 left the entry window unprotected for
        // exactly the collect before the cut - while other blocks'
        // rolls flooded the ring - and the cut then missed inside its
        // own prewarmed window.
        const bool upcoming =
            h > 0 || std::ceil(c.t_in) > static_cast<double>(root_frame);
        bool playing_now = false;
        for (const Request& r : want)
            if (r.alias == req.alias) {
                playing_now = true;
                break;
            }
        bool dup = false;
        for (const Job& q : queued)
            if (q.key == req.alias && q.frame == req.frame) {
                dup = true;
                break;
            }
        if (dup) continue;
        {
            std::lock_guard<std::mutex> lock(map_m_);
            // The cap guards OPEN files and live decoders; idle-closed
            // entries hold neither - counting them silently starved
            // every preroll once a long arrangement had opened 32
            // streams over its lifetime.
            size_t open_streams = 0;
            for (const auto& e : streams_)
                if (!e.second->idle_closed) ++open_streams;
            if (streams_.find(req.alias) == streams_.end() &&
                open_streams >= kMaxStreams)
                continue;
        }
        Stream* ps = stream_for(req);
        if (!ps) continue;
        uint32_t entry = 0xFFFFFFFFu;
        {
            std::lock_guard<std::mutex> lock(ps->m);
            if (upcoming) {
                const double esrc = doc::media_asset_frame(c, first);
                entry = static_cast<uint32_t>(std::clamp(
                    esrc, 0.0, static_cast<double>(b->frames - 1)));
                bool first_up = !playing_now;
                if (first_up)
                    for (uint64_t k2 : want_anchored)
                        if (k2 == req.alias) {
                            first_up = false;
                            break;
                        }
                if (first_up) {
                    // The nearest upcoming block of a stream that is not
                    // playing owns the want anchor.
                    want_anchored.push_back(req.alias);
                    if (ps->want != entry) {
                        ps->last_want = entry > 0 ? entry - 1 : 0;
                        ps->want = entry;
                    }
                } else {
                    // Every other upcoming block protects through a
                    // mark, up to the stream's mark bank (nearest
                    // first - the loop walks instances in t_in order).
                    size_t marks = 0;
                    bool have = false;
                    for (const auto& m2 : entry_marks)
                        if (m2.first == req.alias) {
                            ++marks;
                            if (m2.second == entry) have = true;
                        }
                    if (!have && marks < Stream::kEntryMarks)
                        entry_marks.emplace_back(req.alias, entry);
                }
            }
            // A stream moving BACKWARD skips its forward frontier: those
            // frames were just evicted against the direction of travel,
            // and re-rolling them ping-pongs the sessions against the
            // backfill below.
            if (ps->want < ps->last_want) continue;
            bool ringed = false;
            for (const auto& e : ps->ring)
                if (e.first == req.frame) {
                    ringed = true;
                    break;
                }
            if (ringed) continue;
        }
        // The ENTRY rolls as its own job ahead of the frontier: an fps
        // conform can land the entry a frame under source_in, in the
        // PREVIOUS keyframe run - the frontier's roll starts at its own
        // keyframe and can never ring it, and that one frame was a
        // full-GOP blocking roll at every cut.
        if (upcoming && entry != 0xFFFFFFFFu && entry != req.frame) {
            bool entry_ringed = false;
            {
                std::lock_guard<std::mutex> lock(ps->m);
                for (const auto& e : ps->ring)
                    if (e.first == entry) {
                        entry_ringed = true;
                        break;
                    }
            }
            bool entry_dup = false;
            for (const Job& q : queued)
                if (q.key == req.alias && q.frame == entry) {
                    entry_dup = true;
                    break;
                }
            if (!entry_ringed && !entry_dup)
                queued.push_back(
                    {req.alias, entry, 0, true,
                     static_cast<uint32_t>(first + horizons[h].bias)});
        }
        queued.push_back({req.alias, req.frame, 0, upcoming,
                          static_cast<uint32_t>(first + horizons[h].bias)});
    }
    // BACKWARD prewarm: reverse motion exhausts its ring window every
    // kReverseWindow frames, and each exhaustion was a BLOCKING roll on
    // the consumer. Queue the frame one window below the ask so the
    // idle session pre-rolls the PREVIOUS keyframe run concurrently —
    // the same GOP-striped supply forward playback gets. (Two windows
    // measured WORSE: their rolls compete for the same two sessions.)
    for (const Request& req : want) {
        Stream* ps = nullptr;
        {
            std::lock_guard<std::mutex> lock(map_m_);
            if (auto it = streams_.find(req.alias); it != streams_.end())
                ps = it->second.get();
        }
        if (!ps || !ps->native || req.frame == 0) continue;
        bool backward = false;
        bool ringed = false;
        const uint32_t back = req.frame > kReverseWindow
                                  ? req.frame - kReverseWindow
                                  : 0u;
        {
            std::lock_guard<std::mutex> lock(ps->m);
            backward = ps->want < ps->last_want;
            for (const auto& e : ps->ring)
                if (e.first == back) {
                    ringed = true;
                    break;
                }
        }
        if (!backward || ringed) continue;
        bool dup = false;
        for (const Job& q : queued)
            if (q.key == req.alias && q.frame == back) {
                dup = true;
                break;
            }
        if (!dup) queued.push_back({req.alias, back, 0, false, root_frame});
    }
    } else {
        // SCRUBBING: one CHASER per stream toward the drag target, and
        // nothing else. The consumer serves ringed frames instantly
        // (never decodes); the chaser rolls at full decode rate on a
        // worker session and re-queues each collect with the live
        // target, so the ring frontier follows the drag as fast as the
        // codec structure physically allows.
        for (const Request& req : want) {
            Stream* ps = nullptr;
            {
                std::lock_guard<std::mutex> lock(map_m_);
                if (auto it = streams_.find(req.alias);
                    it != streams_.end())
                    ps = it->second.get();
            }
            if (!ps || !ps->native) continue;
            bool ringed = false;
            {
                std::lock_guard<std::mutex> lock(ps->m);
                for (const auto& e : ps->ring)
                    if (e.first == req.frame) {
                        ringed = true;
                        break;
                    }
            }
            if (ringed) continue;
            queued.push_back({req.alias, req.frame, 0, false, root_frame});
        }
    }
    // One write per stream: the marks it earned this collect, or none.
    {
        std::lock_guard<std::mutex> lock(map_m_);
        for (auto& entry : streams_) {
            uint32_t marks[Stream::kEntryMarks] = {
                0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
            size_t n = 0;
            for (const auto& m2 : entry_marks)
                if (m2.first == entry.first && n < Stream::kEntryMarks)
                    marks[n++] = m2.second;
            Stream& s = *entry.second;
            std::lock_guard<std::mutex> sl(s.m);
            for (size_t i = 0; i < Stream::kEntryMarks; ++i)
                s.next_entries[i] = marks[i];
            // Depth demand: the live window, the connective path a
            // long roll rings on its way, and a guard-width window per
            // mark.
            s.ring_target = static_cast<uint32_t>(std::min<size_t>(
                2 * kDecodeAhead + 8 + n * (2 * kDecodeAhead + 1), 96));
        }
    }
    // Nearest deadline first: with more standing jobs than workers,
    // flatten order let far-future prerolls starve the boundary about
    // to play. Stable, so an instance's entry job stays ahead of its
    // own frontier.
    std::stable_sort(queued.begin(), queued.end(),
                     [](const Job& a, const Job& b) {
                         return a.deadline < b.deadline;
                     });
    {
        std::lock_guard<std::mutex> lock(job_m_);
        // Last plan wins: a seek must not decode the frames the old
        // playhead was heading for.
        ++gen_;
        jobs_.clear();
        for (Job& job : queued) {
            job.gen = gen_;
            jobs_.push_back(job);
        }
    }
    job_cv_.notify_all();
    if (!scrub) idle_close_scan();
    return frames_;
}

void DecodePool::worker_main() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(job_m_);
            job_cv_.wait(lock, [this] { return quit_ || !jobs_.empty(); });
            if (quit_) return;
            job = jobs_.front();
            jobs_.pop_front();
            if (job.gen != gen_) continue;
            ++busy_;
        }
        Stream* s = nullptr;
        {
            std::lock_guard<std::mutex> lock(map_m_);
            if (auto it = streams_.find(job.key); it != streams_.end())
                s = it->second.get();
        }
        t_job_epoch = scrub_epoch_.load(std::memory_order_relaxed);
        if (s) fetch(*s, job.frame, nullptr, false, job.preroll);
        {
            std::lock_guard<std::mutex> lock(job_m_);
            --busy_;
        }
        job_cv_.notify_all();
    }
}

}  // namespace looks::media

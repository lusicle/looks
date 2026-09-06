#include "media/decode_pool.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "media/bmff.h"
#include "util/hash.h"
#include "util/log.h"

namespace looks::media {

namespace {

// How far ahead the pool looks for sources that do not play yet.
constexpr uint32_t kPrewarmFrames = 60;
// Decoded frames queued ahead per stream.
constexpr uint32_t kDecodeAhead = 12;
// File handles held for placements that are only prewarming.
constexpr size_t kMaxStreams = 32;
// Decoded frames held across all streams; 1080p I420 is ~3 MB, 4K ~12 MB.
constexpr size_t kFrameBudget = 96;
// Rolled frames kept ringed for backward play; ~12 MB each at 4K.
constexpr uint32_t kReverseWindow = 24;
// Feeds past the target sample before a roll gives up.
constexpr uint32_t kRollGuard = 64;
// Collects without a touch before an idle stream releases its resources.
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

const std::filesystem::path& bundle_video(const AssetBundle& b) {
    return b.native.empty() ? b.mez : b.native;
}

// Process wide, so a stamp never collides across pools.
std::atomic<uint64_t> g_frame_stamp{1};

// Swaps the decoder buffer whole: Y rows, then interleaved CbCr, one stride.
// The caller scratch takes the old capacity; steady state allocates nothing.
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

// The scrub epoch of the job this worker thread runs.
thread_local uint64_t t_job_epoch = 0;

// Hashes the mapping shape, not its window.
// Never use this alone as a stream identity.
uint64_t bits_of(double v) {
    uint64_t b;
    std::memcpy(&b, &v, sizeof(b));
    return b;
}

uint64_t mix64(uint64_t k, uint64_t v) {
    return k ^ (v + 0x9E3779B97F4A7C15ull + (k << 6) + (k >> 2));
}

uint64_t mapping_hash(const doc::MediaInstance& c) {
    uint64_t k = c.asset * 0x9E3779B97F4A7C15ull + 1;
    k = mix64(k, bits_of(c.source_in - c.t_in * c.speed));
    k = mix64(k, bits_of(c.speed));
    k = mix64(k, bits_of(c.rate));
    k = mix64(k, static_cast<uint64_t>(c.shift));
    k = mix64(k, c.slide_count);
    k = mix64(k, c.slide_index);
    k = mix64(k, bits_of(c.slide_period));
    k = mix64(k, bits_of(c.slide_fade));
    k = mix64(k, c.slide_end);
    k = mix64(k, c.repeat_frames);
    return k;
}

}  // namespace

DecodePool::Stream::Session::~Session() {
    if (file) std::fclose(static_cast<FILE*>(file));
}

DecodePool::DecodePool(const char* name) : name_(name) {
    // Native streams decode serially per session; mez streams fan over slots.
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
    // An unchanged source table must not stall the decode workers.
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
                a.shift != b.shift || a.gain != b.gain ||
                a.slide_count != b.slide_count || a.slide_index != b.slide_index ||
                a.slide_period != b.slide_period || a.slide_fade != b.slide_fade ||
                a.slide_end != b.slide_end || a.repeat_frames != b.repeat_frames) {
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

    // One stream per instance key; keys fold when their whole schedules
    // match. Fold on schedules, not mappings, or folded keys read different
    // frames.
    canonical_.clear();
    {
        std::unordered_map<uint64_t, std::vector<uint64_t>> key_maps;
        for (const doc::MediaInstance& c : sources_) {
            uint64_t sched = mapping_hash(c);
            sched = mix64(sched, bits_of(c.t_in));
            sched = mix64(sched, bits_of(c.t_out));
            key_maps[c.key].push_back(sched);
        }
        std::unordered_map<uint64_t, uint64_t> group_min;
        std::vector<std::pair<uint64_t, uint64_t>> key_fp;
        key_fp.reserve(key_maps.size());
        for (auto& e : key_maps) {
            std::vector<uint64_t>& maps = e.second;
            std::sort(maps.begin(), maps.end());
            maps.erase(std::unique(maps.begin(), maps.end()), maps.end());
            uint64_t fp = kFnvOffset;
            for (uint64_t m : maps) fp = (fp ^ m) * kFnvPrime;
            key_fp.emplace_back(e.first, fp);
            auto [g, fresh] = group_min.try_emplace(fp, e.first);
            if (!fresh && e.first < g->second) g->second = e.first;
        }
        for (const auto& e : key_fp) canonical_[e.first] = group_min[e.second];
        log_info("decode pool[%s]: %zu sources, %zu keys, %zu streams",
                 name_, sources_.size(), canonical_.size(), group_min.size());
    }
    std::lock_guard<std::mutex> lock(map_m_);
    std::unordered_map<uint64_t, const doc::MediaInstance*> by_stream;
    for (const doc::MediaInstance& c : sources_)
        by_stream[alias_of(c.key)] = &c;
    for (auto it = streams_.begin(); it != streams_.end();) {
        const auto sit = by_stream.find(it->first);
        const AssetBundle* b = sit != by_stream.end()
                                   ? find_bundle(bundles_, sit->second->asset)
                                   : nullptr;
        // A placement that changed asset must not serve the old file pixels.
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
        // A placement can outlive its media; it holds the last frame.
        const double src = doc::media_asset_frame(c, f);
        const double last = static_cast<double>(b->frames - 1);
        const uint32_t idx = static_cast<uint32_t>(
            std::clamp(src, 0.0, last));
        // Overlapping placements share a stream; decode only the compiler
        // winner, which is the latest t_in.
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
        if (!replaced) out.push_back({c.key, alias_of(c.key), i, idx});
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

// Never blocks on a busy lock; it skips a held session and retries later.
// Idempotent: idle_closed latches only when every piece releases.
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
    if (auto kept = s.ringed(frame)) return kept;
    s.ring.emplace_back(frame, decoded);
    // Evict against the direction of travel; the live window ahead of the
    // playhead is untouchable. Frames it moves away from weigh 4x.
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
            // An upcoming cut landing zone is as live as the playhead
            // window, at the guard width of 2x the decode span.
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
        if (s.ring[worst].second.use_count() == 1 && s.spare.size() < 8)
            s.spare.push_back(std::const_pointer_cast<codec::DecodedFrame>(
                s.ring[worst].second));
        s.ring.erase(s.ring.begin() + static_cast<ptrdiff_t>(worst));
    }
    s.cv.notify_all();
    return decoded;
}

// Returns a recycled frame that keeps its capacity, or null.
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
        if (auto hit = s.ringed(frame)) return hit;
    }
    if (was_miss) *was_miss = true;
    // was_miss == nullptr marks advisory work that must never park a worker.
    return s.native
               ? fetch_native(s, frame, was_miss == nullptr, scrub, preroll)
               : fetch_mez(s, frame);
}

std::shared_ptr<const codec::DecodedFrame> DecodePool::fetch_mez(
    Stream& s, uint32_t frame) {
    // Wait at most one decode deep, and only when the whole bank is busy.
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
        if (auto hit = s.ringed(frame)) return hit;   // landed while waiting
    }
    if (!slot->opened) {
        slot->opened = true;
        std::string error;
        slot->ok = slot->reader.open(s.path, &error);
        if (!slot->ok)
            log_warn("decode pool: open failed (%s)", error.c_str());
    }
    if (!slot->ok) return nullptr;
    // Many frames can share one payload; reuse the pixels and the stamp.
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

// Every frame decoded on the way rings. Advisory calls never wait.
std::shared_ptr<const codec::DecodedFrame> DecodePool::fetch_native(
    Stream& s, uint32_t frame, bool advisory, bool scrub, bool preroll) {
    // Every thread that touches a decoder MFT needs its own MF session.
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

    if (!advisory) {
        uint32_t lo = ~0u, hi = 0, n = 0;
        uint32_t want_now = 0, tgt_now = 0;
        uint32_t m0 = 0xFFFFFFFFu, m1 = 0xFFFFFFFFu;
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

    // Never decode mid drag; serve the nearest ringed frame and set
    // approximated_. An empty ring falls through to the slice below.
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

    // The session inside the target keyframe run owns every ask in that run.
    Stream::Session* sess = nullptr;
    std::unique_lock<std::mutex> slock;
    Stream::Session* owner = nullptr;
    int64_t owner_np = -1;
    // Supply positions: the live window and every marked entry landing zone.
    uint32_t live_want = 0;
    uint32_t guard_marks[Stream::kEntryMarks];
    bool mark_ringed[Stream::kEntryMarks] = {};
    {
        std::lock_guard<std::mutex> lock(s.m);
        live_want = s.want;
        for (size_t i = 0; i < Stream::kEntryMarks; ++i) {
            guard_marks[i] = s.next_entries[i];
            // A mark guards only frames that exist in the ring.
            if (guard_marks[i] != 0xFFFFFFFFu && s.has_ringed(guard_marks[i]))
                mark_ringed[i] = true;
        }
    }
    // Returns -1 for nothing, 0 for the live want window, else the mark it
    // holds. Guards yield in deadline order.
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
    // Immune to this ask: the live window, or a mark at or before the target.
    auto guarded_np = [&](int64_t np) {
        const int64_t g = guard_of(np);
        return g == 0 ||
               (g > 0 && g <= static_cast<int64_t>(target));
    };
    for (Stream::Session& c : s.sessions) {
        const int64_t np = c.next_present.load(std::memory_order_relaxed);
        // A session owns the ask when continuing reaches it; the closest
        // emissions win the tie. A far preroll must not continue a guarded
        // session.
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
                // Only one worker may wait on a busy owner; the rest skip.
                if (owner->waiter.exchange(true, std::memory_order_acquire))
                    return nullptr;
                attempt = std::unique_lock<std::mutex>(owner->m);
                owner->waiter.store(false, std::memory_order_release);
            } else {
                // Wait on the ring the owner feeds, and retry in short slices.
                for (int spin = 0; spin < 10; ++spin) {
                    std::shared_ptr<const codec::DecodedFrame> hit;
                    {
                        std::unique_lock<std::mutex> rlock(s.m);
                        s.cv.wait_for(rlock, std::chrono::milliseconds(25),
                                      [&] {
                                          hit = s.ringed(target);
                                          return hit != nullptr;
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
        // Candidate order: continue, then idle, then positioned, then
        // guarded. An advisory must never take a guarded session.
        Stream::Session* live_sess = nullptr;
        std::unique_lock<std::mutex> live_lock;
        Stream::Session* idle_sess = nullptr;
        std::unique_lock<std::mutex> idle_lock;
        Stream::Session* stale_sess = nullptr;
        std::unique_lock<std::mutex> stale_lock;
        // Yield a farther mark only when nothing unguarded is free.
        Stream::Session* yield_sess = nullptr;
        std::unique_lock<std::mutex> yield_lock;
        for (Stream::Session& c : s.sessions) {
            std::unique_lock<std::mutex> attempt(c.m, std::try_to_lock);
            if (!attempt.owns_lock()) continue;
            const int64_t np =
                c.next_present.load(std::memory_order_relaxed);
            // A far preroll must not continue a guarded session; that
            // reparks the supply the same way a restart does.
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
        if (auto hit = s.ringed(target)) return hit;   // landed while waiting
    }

    if (!sess->created) {
        sess->created = true;
        std::string error;
        // try_d3d asks for hardware decode and falls back to software
        // silently.
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

    const int64_t np = sess->next_present.load(std::memory_order_relaxed);
    const bool cont = np >= 0 && !sess->draining &&
                      np <= static_cast<int64_t>(target) &&
                      key < sess->next_decode;
    const uint32_t floor_present =
        cont ? static_cast<uint32_t>(np) : idx.decode_to_present[key];
    // An advisory may continue freely, but may seek only near its keyframe.
    // A backward backfill is exempt from that gate.
    bool backfill = false;
    {
        std::lock_guard<std::mutex> lock(s.m);
        backfill = s.want < s.last_want && target < s.want;
    }
    // A scrub chaser and a cut preroll are exempt from the cold-seek gate.
    const bool chaser =
        advisory && scrub_.load(std::memory_order_relaxed) &&
        t_job_epoch == scrub_epoch_.load(std::memory_order_relaxed);
    if (advisory && !backfill && !chaser && !preroll &&
        target - floor_present > (cont ? 48u : kDecodeAhead))
        return nullptr;

    // Mid drag, an ask that costs a long roll serves the roll floor.
    // The caller must not cache that frame.
    constexpr uint32_t kScrubSnapRoll = 6;
    uint32_t serve = target;
    if (scrub && !advisory && target - floor_present > kScrubSnapRoll) {
        // One bounded slice per pass; the re-collect loop marches to the
        // true frame.
        serve = floor_present + kScrubSnapRoll;
        approximated_ = true;
        std::lock_guard<std::mutex> lock(s.m);
        if (auto hit = s.ringed(serve)) return hit;
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

    // Backward motion keeps the top half of the run ringed.
    size_t depth = std::max<uint32_t>(ring_depth_, 1);
    {
        std::lock_guard<std::mutex> lock(s.m);
        if (s.want < s.last_want) {
            const uint32_t roll = serve - floor_present + 1;
            // Widen only, never shrink; a clamp with lo > hi asserts.
            depth = std::max(depth,
                             std::min<size_t>(roll / 2 + 1, kReverseWindow));
        }
    }

    std::shared_ptr<const codec::DecodedFrame> result;
    std::vector<uint8_t>& sample_bytes = sess->sample_bytes;
    platform::VideoFrameNV12& nv12 = sess->scratch;
    // Label emissions by arrival order from the roll floor. A post-flush
    // stamp can be garbage, so trust it only when it is sane.
    int64_t expect = floor_present;
    const int64_t half_dur = idx.timescale
        ? static_cast<int64_t>(5.0e6 * idx.frame_duration / idx.timescale)
        : 0;
    bool hold_was_dry = false;
    while (!result) {
        if (abort_.load(std::memory_order_relaxed)) return nullptr;
        // Bail mid roll when a drag started after this advisory was queued.
        if (advisory && scrub_.load(std::memory_order_relaxed) &&
            t_job_epoch != scrub_epoch_.load(std::memory_order_relaxed))
            return nullptr;
        bool held = false;
        if (sess->next_decode < idx.frame_count() &&
            sess->next_decode <= serve_decode + kRollGuard) {
            const FrameIndex::Sample& sm = idx.samples[sess->next_decode];
            // Never feed into the next run once the target sample is fed.
            // If a hold pass gives no emission, feed anyway to push it.
            if (sm.keyframe && sess->next_decode > key &&
                sess->next_decode > serve_decode && !hold_was_dry) {
                held = true;
            } else if (!BmffFile::read_at(sess->file, sm.offset, sm.size,
                                          sample_bytes) ||
                       !sess->dec.feed(sample_bytes.data(),
                                       sample_bytes.size(), sm.pts_100ns,
                                       sm.duration_100ns, sm.keyframe)) {
                log_warn("decode pool: native feed failed at sample %u",
                         sess->next_decode);
                sess->next_present.store(-1, std::memory_order_relaxed);
                return nullptr;
            } else {
                // Crossing into the next keyframe run moves the ownership
                // hint the pick loop reads.
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
    // The idle clock must freeze during a scrub, or streams age into close.
    if (!scrub) ++collect_gen_;
    approximated_ = false;
    if (scrub && !prev_scrub_)
        scrub_epoch_.fetch_add(1, std::memory_order_relaxed);
    prev_scrub_ = scrub;
    scrub_.store(scrub, std::memory_order_relaxed);
    const std::vector<Request> want = plan(root_frame);
    // Ring depth must clear the prewarm span with slack.
    // The budget divides across streams, not requests.
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

    // Same-alias requests read one media frame; one fetch serves them all.
    struct Served {
        uint64_t alias;
        uint32_t frame;
        std::shared_ptr<const codec::DecodedFrame> pixels;
    };
    std::vector<Served> served;
    for (const Request& req : want) {
        const Served* prior = nullptr;
        for (const Served& sv : served)
            if (sv.alias == req.alias && sv.frame == req.frame) {
                prior = &sv;
                break;
            }
        if (prior) {
            frames_.push_back({req.key, prior->pixels});
            continue;
        }
        Stream* s = stream_for(req);
        if (!s) continue;
        {
            std::lock_guard<std::mutex> lock(s->m);
            // A repeated ask keeps the last real direction of travel.
            if (req.frame != s->want) {
                s->last_want = s->want;
                s->want = req.frame;
            }
        }
        bool miss = false;
        auto frame = fetch(*s, req.frame, &miss, scrub);
        if (miss) ++misses_;
        if (frame) {
            served.push_back({req.alias, req.frame, frame});
            frames_.push_back({req.key, std::move(frame)});
        }
    }

    // The prewarm span must never exceed what the ring can hold next to
    // the current frame. A scrub queues nothing.
    std::vector<Job> queued;
    auto queued_has = [&queued](uint64_t key, uint32_t frame) {
        for (const Job& q : queued)
            if (q.key == key && q.frame == frame) return true;
        return false;
    };
    // Collect entry marks first and write them once per stream at the end.
    std::vector<std::pair<uint64_t, uint32_t>> entry_marks;
    // A second upcoming block must protect through a mark, not the anchor.
    std::vector<uint64_t> want_anchored;
    const uint32_t span = std::min(
        kDecodeAhead, ring_depth_ > 1 ? ring_depth_ - 1 : 1u);
    if (!scrub) {
    // One standing job per placement advances its stream to the span
    // frontier. bias keeps job deadlines in frames until play in both
    // windows.
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
        if (c.slide_count && !doc::media_active(c, first) &&
            !doc::media_active(c, std::min(horizons[h].hi, c.t_out - 1.0))) continue;
        const AssetBundle* b = find_bundle(bundles_, c.asset);
        if (!b || bundle_video(*b).empty() || b->frames == 0) continue;
        double at = std::max(
            first, std::min(first + span - 1.0, c.t_out - 1.0));
        // A wrap-window frontier stops at the loop end.
        if (h > 0)
            at = std::max(first,
                          std::min(at, static_cast<double>(loop_out_) - 1.0));
        const double src = doc::media_asset_frame(c, at);
        Request req;
        req.key = c.key;
        req.alias = alias_of(c.key);
        req.source = i;
        req.frame = static_cast<uint32_t>(std::clamp(
            src, 0.0, static_cast<double>(b->frames - 1)));
        // An instance that has not started is a cut preroll, even when its
        // key already plays through an earlier block. A block stays upcoming
        // until t_in <= root, not until its entry is the next frame.
        const bool upcoming =
            h > 0 || std::ceil(c.t_in) > static_cast<double>(root_frame);
        bool playing_now = false;
        for (const Request& r : want)
            if (r.alias == req.alias) {
                playing_now = true;
                break;
            }
        if (queued_has(req.alias, req.frame)) continue;
        {
            std::lock_guard<std::mutex> lock(map_m_);
            // The cap counts open files and live decoders, not idle-closed
            // entries.
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
                    // The nearest upcoming block of a stream that does not
                    // play owns the want anchor.
                    want_anchored.push_back(req.alias);
                    if (ps->want != entry) {
                        ps->last_want = entry > 0 ? entry - 1 : 0;
                        ps->want = entry;
                    }
                } else {
                    // Every other upcoming block protects through a mark,
                    // nearest first, up to the mark bank size.
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
            // A stream that moves backward skips its forward frontier.
            if (ps->want < ps->last_want) continue;
            if (ps->has_ringed(req.frame)) continue;
        }
        // The entry rolls as its own job; an fps conform can land it in
        // the previous keyframe run, which the frontier roll cannot ring.
        if (upcoming && entry != 0xFFFFFFFFu && entry != req.frame) {
            bool entry_ringed = false;
            {
                std::lock_guard<std::mutex> lock(ps->m);
                entry_ringed = ps->has_ringed(entry);
            }
            if (!entry_ringed && !queued_has(req.alias, entry))
                queued.push_back(
                    {req.alias, entry, 0, true,
                     static_cast<uint32_t>(first + horizons[h].bias)});
        }
        queued.push_back({req.alias, req.frame, 0, upcoming,
                          static_cast<uint32_t>(first + horizons[h].bias)});
    }
    // Queue the frame one window below the ask, so the idle session
    // prerolls the previous keyframe run. Two windows measure worse.
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
            ringed = ps->has_ringed(back);
        }
        if (!backward || ringed) continue;
        if (!queued_has(req.alias, back))
            queued.push_back({req.alias, back, 0, false, root_frame});
    }
    } else {
        // While scrubbing, queue one chaser per stream and nothing else.
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
                ringed = ps->has_ringed(req.frame);
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
            // Depth demand: the live window, the roll path, and one
            // window per mark.
            s.ring_target = static_cast<uint32_t>(std::min<size_t>(
                2 * kDecodeAhead + 8 + n * (2 * kDecodeAhead + 1), 96));
        }
    }
    // Sort nearest deadline first. Keep the sort stable, so an instance
    // entry job stays ahead of its own frontier.
    std::stable_sort(queued.begin(), queued.end(),
                     [](const Job& a, const Job& b) {
                         return a.deadline < b.deadline;
                     });
    {
        std::lock_guard<std::mutex> lock(job_m_);
        // Last plan wins; a seek must not decode the old playhead frames.
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

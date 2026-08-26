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
constexpr size_t kFrameBudget = 48;
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

}  // namespace

DecodePool::Stream::Session::~Session() {
    if (file) std::fclose(static_cast<FILE*>(file));
}

DecodePool::DecodePool() {
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

    std::lock_guard<std::mutex> lock(map_m_);
    for (auto it = streams_.begin(); it != streams_.end();) {
        const doc::MediaInstance* src = nullptr;
        for (const doc::MediaInstance& c : sources_)
            if (c.key == it->first) src = &c;
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
        if (!replaced) out.push_back({c.key, i, idx});
    }
    return out;
}

DecodePool::Stream* DecodePool::stream_for(const Request& req) {
    const doc::MediaInstance& c = sources_[req.source];
    const AssetBundle* b = find_bundle(bundles_, c.asset);
    if (!b || bundle_video(*b).empty()) return nullptr;
    std::lock_guard<std::mutex> lock(map_m_);
    auto it = streams_.find(req.key);
    if (it != streams_.end()) {
        it->second->last_touch = collect_gen_;
        it->second->idle_closed = false;
        return it->second.get();
    }
    auto s = std::make_unique<Stream>();
    s->path = bundle_video(*b);
    s->native = !b->native.empty();
    s->frames = b->frames;
    s->last_touch = collect_gen_;
    Stream* raw = s.get();
    streams_.emplace(req.key, std::move(s));
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
    while (s.ring.size() > std::max<size_t>(depth, 1)) {
        size_t worst = SIZE_MAX;
        uint32_t worst_d = 0;
        for (size_t i = 0; i < s.ring.size(); ++i) {
            const uint32_t idx = s.ring[i].first;
            if (!backward && idx >= s.want && idx <= s.want + kDecodeAhead)
                continue;
            if (backward && idx <= s.want && s.want - idx <= kDecodeAhead)
                continue;
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
    Stream& s, uint32_t frame, bool* was_miss, bool scrub) {
    {
        std::lock_guard<std::mutex> lock(s.m);
        for (const auto& e : s.ring)
            if (e.first == frame) return e.second;
    }
    if (was_miss) *was_miss = true;
    // was_miss == nullptr is the prewarm path: advisory work that must
    // never park a worker behind a busy session - the job re-queues next
    // collect if the frame still matters.
    return s.native ? fetch_native(s, frame, was_miss == nullptr, scrub)
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
    Stream& s, uint32_t frame, bool advisory, bool scrub) {
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
        {
            std::lock_guard<std::mutex> lock(s.m);
            for (const auto& e : s.ring) {
                lo = std::min(lo, e.first);
                hi = std::max(hi, e.first);
                ++n;
            }
        }
        log_info("decode pool: consumer miss %u (ring %u..%u n%u, "
                 "s0 np %lld rk %lld, s1 np %lld rk %lld)",
                 target, n ? lo : 0, n ? hi : 0, n,
                 static_cast<long long>(s.sessions[0].next_present.load(
                     std::memory_order_relaxed)),
                 static_cast<long long>(s.sessions[0].run_key.load(
                     std::memory_order_relaxed)),
                 static_cast<long long>(s.sessions[1].next_present.load(
                     std::memory_order_relaxed)),
                 static_cast<long long>(s.sessions[1].run_key.load(
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
        for (Stream::Session& c : s.sessions) {
            std::unique_lock<std::mutex> attempt(c.m, std::try_to_lock);
            if (attempt.owns_lock()) {
                sess = &c;
                slock = std::move(attempt);
                break;
            }
        }
    }
    if (!sess) {
        if (advisory) return nullptr;   // both busy - do not convoy
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
    // to wherever the drag points.
    const bool chaser =
        advisory && scrub_.load(std::memory_order_relaxed) &&
        t_job_epoch == scrub_epoch_.load(std::memory_order_relaxed);
    if (advisory && !backfill && !chaser &&
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
            depth = std::clamp<size_t>(roll / 2 + 1, depth, kReverseWindow);
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
            s.sessions[session_index ^ 1];
        log_info("decode pool: s%zu %s%s roll %u frames %.0f ms -> frame "
                 "%u (key %u, was np %lld, other np %lld run %lld)",
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
    ++collect_gen_;
    approximated_ = false;
    if (scrub && !prev_scrub_)
        scrub_epoch_.fetch_add(1, std::memory_order_relaxed);
    prev_scrub_ = scrub;
    scrub_.store(scrub, std::memory_order_relaxed);
    const std::vector<Request> want = plan(root_frame);
    // Depth must clear the prewarm span with SLACK: a ring sized exactly
    // to span+tail evicts decoded-ahead frames the playhead has not
    // reached yet, and every one of those comes back as a re-decode.
    ring_depth_ = want.empty()
        ? 4
        : static_cast<uint32_t>(
              std::clamp(kFrameBudget / want.size(), size_t{2}, size_t{24}));

    for (const Request& req : want) {
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
    const uint32_t span = std::min(
        kDecodeAhead, ring_depth_ > 1 ? ring_depth_ - 1 : 1u);
    if (!scrub) {
    // ONE standing job per placement: advance its stream to the span
    // FRONTIER. The worker's roll rings every frame on the way, so one
    // job replaces a span of one-frame jobs that each fought the
    // consumer for the session lock - and a cut preroll falls out of
    // the same shape (the roll starts at the entry frame's keyframe).
    for (size_t i = 0; i < sources_.size(); ++i) {
        const doc::MediaInstance& c = sources_[i];
        const double first = std::max(
            static_cast<double>(root_frame) + 1.0, std::ceil(c.t_in));
        if (first >= c.t_out ||
            first > static_cast<double>(root_frame) + kPrewarmFrames)
            continue;
        const AssetBundle* b = find_bundle(bundles_, c.asset);
        if (!b || bundle_video(*b).empty() || b->frames == 0) continue;
        const double at = std::max(
            first, std::min(first + span - 1.0, c.t_out - 1.0));
        const double src = doc::media_asset_frame(c, at);
        Request req;
        req.key = c.key;
        req.source = i;
        req.frame = static_cast<uint32_t>(std::clamp(
            src, 0.0, static_cast<double>(b->frames - 1)));
        bool dup = false;
        for (const Job& q : queued)
            if (q.key == req.key && q.frame == req.frame) {
                dup = true;
                break;
            }
        if (dup) continue;
        {
            std::lock_guard<std::mutex> lock(map_m_);
            if (streams_.find(req.key) == streams_.end() &&
                streams_.size() >= kMaxStreams)
                continue;
        }
        Stream* ps = stream_for(req);
        if (!ps) continue;
        {
            std::lock_guard<std::mutex> lock(ps->m);
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
        queued.push_back({req.key, req.frame, 0});
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
            if (auto it = streams_.find(req.key); it != streams_.end())
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
            if (q.key == req.key && q.frame == back) {
                dup = true;
                break;
            }
        if (!dup) queued.push_back({req.key, back, 0});
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
                if (auto it = streams_.find(req.key);
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
            queued.push_back({req.key, req.frame, 0});
        }
    }
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
    idle_close_scan();
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
        if (s) fetch(*s, job.frame, nullptr);
        {
            std::lock_guard<std::mutex> lock(job_m_);
            --busy_;
        }
        job_cv_.notify_all();
    }
}

}  // namespace looks::media

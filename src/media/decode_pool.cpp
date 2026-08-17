#include "media/decode_pool.h"

#include <algorithm>
#include <cmath>

#include "util/log.h"

namespace looks::media {

namespace {

// How far ahead the pool looks for sources that are not playing yet. A
// cold file open plus its first decode is the hitch a cut would otherwise
// cost, so it happens a second early.
constexpr uint32_t kPrewarmFrames = 60;
// Decoded frames queued ahead per stream (the old player's lookahead).
constexpr uint32_t kDecodeAhead = 8;
// File handles held for placements that are only prewarming.
constexpr size_t kMaxStreams = 32;
// Decoded frames held across all streams: 1080p I420 is ~3 MB each.
constexpr size_t kFrameBudget = 32;

bool same_bundles(const std::vector<AssetBundle>& a,
                  const std::vector<AssetBundle>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].asset != b[i].asset || a[i].mez != b[i].mez ||
            a[i].frames != b[i].frames)
            return false;
    return true;
}

}  // namespace

DecodePool::DecodePool() {
    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned count = std::clamp(hw / 4, 1u, 4u);
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
    // A revision that left the CLIP TABLE identical (param drags, block
    // Motion, effect edits) must not stall the decode workers - draining
    // per gesture frame is what made dragging hitch during playback.
    std::vector<doc::ClipInstance> next =
        doc::flatten_clip_sources(doc, look_id);
    if (look_id == look_ && same_bundles(bundles, bundles_) &&
        next.size() == clips_.size()) {
        bool same = true;
        for (size_t i = 0; i < next.size(); ++i) {
            const doc::ClipInstance& a = next[i];
            const doc::ClipInstance& b = clips_[i];
            if (a.key != b.key || a.owner != b.owner || a.layer != b.layer ||
                a.asset != b.asset || a.t_in != b.t_in ||
                a.t_out != b.t_out || a.source_in != b.source_in ||
                a.speed != b.speed || a.gain != b.gain) {
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
    clips_ = std::move(next);

    std::lock_guard<std::mutex> lock(map_m_);
    for (auto it = streams_.begin(); it != streams_.end();) {
        const doc::ClipInstance* clip = nullptr;
        for (const doc::ClipInstance& c : clips_)
            if (c.key == it->first) clip = &c;
        const AssetBundle* b =
            clip ? find_bundle(bundles_, clip->asset) : nullptr;
        // A placement that moved to another asset - or vanished - must not
        // keep serving the old file's pixels.
        if (!b || b->mez != it->second->path)
            it = streams_.erase(it);
        else
            ++it;
    }
}

std::vector<DecodePool::Request> DecodePool::plan(uint32_t root_frame) const {
    std::vector<Request> out;
    const double f = static_cast<double>(root_frame);
    for (size_t i = 0; i < clips_.size(); ++i) {
        const doc::ClipInstance& c = clips_[i];
        if (!doc::clip_active(c, f)) continue;
        const AssetBundle* b = find_bundle(bundles_, c.asset);
        if (!b || b->mez.empty() || b->frames == 0) continue;
        // A placement may outlive its media (an explicit out point past
        // the end): it holds the last frame rather than going blank.
        const double src = std::floor(doc::clip_source_frame(c, f));
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
                if (clips_[r.clip].t_in <= c.t_in) {
                    r.clip = i;
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
    const doc::ClipInstance& c = clips_[req.clip];
    const AssetBundle* b = find_bundle(bundles_, c.asset);
    if (!b || b->mez.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(map_m_);
    auto it = streams_.find(req.key);
    if (it != streams_.end()) return it->second.get();
    auto s = std::make_unique<Stream>();
    s->path = b->mez;
    s->frames = b->frames;
    Stream* raw = s.get();
    streams_.emplace(req.key, std::move(s));
    return raw;
}

std::shared_ptr<const codec::DecodedFrame> DecodePool::fetch(
    Stream& s, uint32_t frame, bool* was_miss) {
    {
        std::lock_guard<std::mutex> lock(s.m);
        for (const auto& e : s.ring)
            if (e.first == frame) return e.second;
    }
    if (was_miss) *was_miss = true;
    // The reader is stateful, so decodes serialize per stream - but on
    // decode_m, never on the ring lock: a probe returns immediately and
    // a miss waits at most the ONE decode in flight.
    std::lock_guard<std::mutex> dlock(s.decode_m);
    {
        std::lock_guard<std::mutex> lock(s.m);
        for (const auto& e : s.ring)
            if (e.first == frame) return e.second;   // landed while waiting
    }
    if (!s.opened) {
        s.opened = true;
        std::string error;
        s.ok = s.reader.open(s.path, &error);
        if (!s.ok)
            log_warn("decode pool: open failed (%s)", error.c_str());
    }
    if (!s.ok) return nullptr;
    codec::DecodedFrame decoded;
    if (!s.reader.decode(frame, decoded)) {
        log_warn("decode pool: decode failed for frame %u", frame);
        return nullptr;
    }
    auto shared =
        std::make_shared<const codec::DecodedFrame>(std::move(decoded));
    std::lock_guard<std::mutex> lock(s.m);
    s.ring.emplace_back(frame, shared);
    // Evict what the playhead has left furthest behind (or never reaches).
    const size_t depth = std::max<size_t>(ring_depth_, 1);
    while (s.ring.size() > depth) {
        size_t worst = 0;
        uint32_t worst_d = 0;
        for (size_t i = 0; i < s.ring.size(); ++i) {
            const uint32_t idx = s.ring[i].first;
            const uint32_t d = idx >= s.want ? idx - s.want : (s.want - idx) * 4;
            if (d >= worst_d) {
                worst_d = d;
                worst = i;
            }
        }
        s.ring.erase(s.ring.begin() + static_cast<ptrdiff_t>(worst));
    }
    return shared;
}

const std::vector<SourceFrame>& DecodePool::collect(uint32_t root_frame) {
    frames_.clear();
    const std::vector<Request> want = plan(root_frame);
    ring_depth_ = want.empty()
        ? 4
        : static_cast<uint32_t>(
              std::clamp(kFrameBudget / want.size(), size_t{2}, size_t{8}));

    for (const Request& req : want) {
        Stream* s = stream_for(req);
        if (!s) continue;
        {
            std::lock_guard<std::mutex> lock(s->m);
            s->want = req.frame;
        }
        bool miss = false;
        auto frame = fetch(*s, req.frame, &miss);
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
    // queued - slowed clips repeat source frames) never re-queue, so a
    // settled steady state queues NOTHING.
    std::vector<Job> queued;
    const uint32_t span = std::min(
        kDecodeAhead, ring_depth_ > 1 ? ring_depth_ - 1 : 1u);
    for (uint32_t n = 0; n < span; ++n) {
        for (size_t i = 0; i < clips_.size(); ++i) {
            const doc::ClipInstance& c = clips_[i];
            const double first =
                std::max(static_cast<double>(root_frame) + 1.0,
                         std::ceil(c.t_in));
            const double at = first + n;
            if (at >= c.t_out ||
                at > static_cast<double>(root_frame) + kPrewarmFrames)
                continue;
            const AssetBundle* b = find_bundle(bundles_, c.asset);
            if (!b || b->mez.empty() || b->frames == 0) continue;
            const double src = std::floor(doc::clip_source_frame(c, at));
            Request req;
            req.key = c.key;
            req.clip = i;
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
        if (s) fetch(*s, job.frame, nullptr);
        {
            std::lock_guard<std::mutex> lock(job_m_);
            --busy_;
        }
        job_cv_.notify_all();
    }
}

}  // namespace looks::media

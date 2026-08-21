// Decode pool: one decoded frame per PLACEMENT.
//
// The single Player owned one reader and one playhead, which is exactly as
// many sources as a rack could show. A look tree can have several playing at
// once - at different source frames, at different speeds, nested - so the
// pool keeps a stream per active placement, keyed by the same instance key
// compile_graph stamps on the matching Source node.
//
// What plays when is a closed form (doc/instances.h), not a walk, so the
// pool can evaluate any frame directly. PREWARM uses that: it asks what
// will be playing a second from now and decodes into those streams early,
// which is what makes a cut - or a nested look's in-point - land without a
// hitch instead of stalling on a cold file open.
//
// Decode is exact, never nearest-available: a frame the render cache stores
// must be the frame that was asked for. Streams decode in parallel across
// worker threads; within a stream, order is serial.
//
// One pool per consumer thread (preview worker / export worker) - the
// public calls are single-threaded by design; only the internal workers are
// concurrent.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "codec/mez.h"
#include "doc/instances.h"
#include "media/bundle.h"
#include "media/frame_index.h"
#include "platform/win/mf_codec.h"

namespace looks::media {

struct SourceFrame {
    uint64_t key = 0;
    std::shared_ptr<const codec::DecodedFrame> frame;
};

class DecodePool {
public:
    DecodePool();
    ~DecodePool();

    DecodePool(const DecodePool&) = delete;
    DecodePool& operator=(const DecodePool&) = delete;

    // Re-flattens the look tree when `revision` moves; cheap otherwise, so
    // callers hand it the document every frame without checking.
    void set_document(const doc::Document& doc, uint64_t look_id,
                      const std::vector<AssetBundle>& bundles,
                      uint64_t revision);

    struct Request {
        uint64_t key = 0;
        size_t source = 0;      // index into sources()
        uint32_t frame = 0;   // asset frame, clamped into the media
    };
    // Every placement playing at a root frame. Pure - no decode, no state.
    std::vector<Request> plan(uint32_t root_frame) const;

    // Decodes the whole plan (blocking on a miss) and queues the prewarm
    // for the frames after it. Frames stay alive until the next collect().
    const std::vector<SourceFrame>& collect(uint32_t root_frame);

    const std::vector<doc::MediaInstance>& sources() const { return sources_; }

    // Frames decoded from disk since the last reset - a decode this frame
    // that prewarm did not already have. Playback smoothness, measurable.
    uint64_t misses() const { return misses_; }

private:
    struct Stream {
        std::filesystem::path path;   // decodable file: source (native) or mez
        bool native = false;
        uint32_t frames = 0;
        // `m` guards the ring/want and is only ever held briefly - a
        // ring PROBE must never wait behind a decode in flight. `cv`
        // fires on every ring insert: a consumer whose frame is already
        // on a supplier's in-flight roll waits HERE, not on the session
        // mutex - seizing the session serialized all decode into the
        // consumer's blocked window and pinned supply to the playhead.
        std::mutex m;
        std::condition_variable cv;
        // Mez reader BANK (stills, cover art, direct .mez): intra-only,
        // so frames decode independently - each slot owns its own FILE* +
        // scratch and several frames of one stream decode concurrently.
        struct Slot {
            std::mutex m;
            codec::MezReader reader;
            bool opened = false;   // guarded by the slot mutex
            bool ok = false;
        };
        static constexpr size_t kSlots = 4;
        Slot slots[kSlots];
        // Hold-frame alias (stills, cover art): the payload decoded last
        // and where it lives, guarded by `m`. A frame pointing at the
        // same offset reuses the decoded pixels instead of re-decoding
        // an identical payload every timeline frame.
        uint64_t last_payload_off = 0;
        std::shared_ptr<const codec::DecodedFrame> last_payload_frame;
        // Native SESSION bank: a session is a demux cursor over the frame
        // index plus an H.264 decoder rolling forward through the source.
        // The playback session tracks the playhead; the second serves a
        // seek or a prewarmed cut without disturbing it. Same locking
        // shape as the slots: one mutex per session, held across a
        // decode; the ring mutex stays brief.
        struct Session {
            std::mutex m;
            platform::H264Decoder dec;
            void* file = nullptr;         // FILE* on the source
            bool created = false;         // guarded by the session mutex
            bool ok = false;
            uint32_t next_decode = 0;     // next decode-order sample to feed
            // Expected next output (-1 = must seek) and the keyframe run
            // the session currently sits in (its start, decode order).
            // Written under the session mutex; read lock-free as routing
            // HINTS: an ask inside a session's run WAITS on that session
            // - duplicating its roll on the idle one is what turned one
            // miss into two concurrent full-GOP decodes.
            std::atomic<int64_t> next_present{-1};
            std::atomic<int64_t> run_key{-1};
            // One advisory waiter may queue behind a busy owner: with a
            // plain try-lock, the consumer's own roll starved every
            // prewarm worker and ended up doing all decode serially
            // inside collect; with unbounded waiters, workers convoy.
            std::atomic<bool> waiter{false};
            bool draining = false;        // end of stream was signalled
            // Receive scratch: capacity survives across fetches, so the
            // decoder fills it without a fresh multi-MB allocation per
            // frame.
            platform::VideoFrameNV12 scratch;

            ~Session();
        };
        static constexpr size_t kSessions = 2;
        Session sessions[kSessions];
        // Built once on first use, then immutable; sessions share it.
        std::mutex index_m;
        bool index_built = false;
        bool index_ok = false;
        FrameIndex index;
        // Decoded frames by index, newest last. Bounded by ring_depth_.
        std::deque<std::pair<uint32_t, std::shared_ptr<const codec::DecodedFrame>>>
            ring;
        // Evicted frames nobody else holds recycle here (guarded by `m`):
        // their vectors keep capacity, so the next decode reuses pages
        // instead of paying an ~18 MB alloc + fault per 4K frame.
        std::vector<std::shared_ptr<codec::DecodedFrame>> spare;
        uint32_t want = 0;       // the frame the consumer last asked for
        uint32_t last_want = 0;  // previous ask: backward motion widens rolls
    };

    struct Job {
        uint64_t key = 0;
        uint32_t frame = 0;
        uint64_t gen = 0;
    };

    Stream* stream_for(const Request& req);
    // Returns the decoded frame; a ring miss decodes on a free slot or
    // session (waiting at most one in-flight decode, never the prewarm
    // backlog).
    std::shared_ptr<const codec::DecodedFrame> fetch(Stream& s, uint32_t frame,
                                                     bool* was_miss);
    std::shared_ptr<const codec::DecodedFrame> fetch_mez(Stream& s,
                                                         uint32_t frame);
    std::shared_ptr<const codec::DecodedFrame> fetch_native(Stream& s,
                                                            uint32_t frame,
                                                            bool advisory);
    std::shared_ptr<const codec::DecodedFrame> ring_insert(
        Stream& s, uint32_t frame,
        std::shared_ptr<const codec::DecodedFrame> decoded, size_t depth);
    static std::shared_ptr<codec::DecodedFrame> take_spare(Stream& s);
    void worker_main();
    void drain();

    std::vector<doc::MediaInstance> sources_;
    std::vector<AssetBundle> bundles_;
    uint64_t revision_ = ~0ull;
    uint64_t look_ = 0;
    std::vector<SourceFrame> frames_;
    uint64_t misses_ = 0;
    // Read by the decode workers: a size hint, sized to how many streams
    // are live so the frame budget divides across them.
    std::atomic<uint32_t> ring_depth_{4};

    std::mutex map_m_;
    std::unordered_map<uint64_t, std::unique_ptr<Stream>> streams_;

    std::mutex job_m_;
    std::condition_variable job_cv_;
    std::deque<Job> jobs_;
    uint64_t gen_ = 0;
    int busy_ = 0;
    bool quit_ = false;
    std::vector<std::thread> workers_;
};

}  // namespace looks::media

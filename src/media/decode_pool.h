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
        std::filesystem::path path;
        uint32_t frames = 0;
        // `m` guards the ring/want and is only ever held briefly - a
        // ring PROBE must never wait behind a decode in flight.
        std::mutex m;
        // Reader BANK: the mezzanine is intra-only, so frames decode
        // independently - each slot owns its own FILE* + scratch and
        // several frames of one stream decode concurrently. One
        // stateful reader capped a 4K stream at ~20 fps, under the
        // media's own rate; the bank multiplies that by the slots a
        // worker can grab.
        struct Slot {
            std::mutex m;
            codec::MezReader reader;
            bool opened = false;   // guarded by the slot mutex
            bool ok = false;
        };
        static constexpr size_t kSlots = 4;
        Slot slots[kSlots];
        // Decoded frames by index, newest last. Bounded by ring_depth_.
        std::deque<std::pair<uint32_t, std::shared_ptr<const codec::DecodedFrame>>>
            ring;
        uint32_t want = 0;   // the frame the consumer last asked for
    };

    struct Job {
        uint64_t key = 0;
        uint32_t frame = 0;
        uint64_t gen = 0;
    };

    Stream* stream_for(const Request& req);
    // Returns the decoded frame; a ring miss decodes under `decode_m`
    // (waiting at most one in-flight decode, never the prewarm backlog).
    std::shared_ptr<const codec::DecodedFrame> fetch(Stream& s, uint32_t frame,
                                                     bool* was_miss);
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

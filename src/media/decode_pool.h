// Decode is exact, never nearest available.
// Streams decode in parallel; inside one stream, decode order is serial.
// Use one pool per consumer thread; the public calls are single-threaded.

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
    uint32_t index = 0;
};

class DecodePool {
public:
    explicit DecodePool(const char* name = "pool");
    ~DecodePool();

    DecodePool(const DecodePool&) = delete;
    DecodePool& operator=(const DecodePool&) = delete;

    // Re-flattens only when revision moves; callers can call this per frame.
    void set_document(const doc::Document& doc, uint64_t look_id,
                      const std::vector<AssetBundle>& bundles,
                      uint64_t revision);

    struct Request {
        uint64_t key = 0;
        // Canonical stream key: keys with the same mapping set fold onto one.
        uint64_t alias = 0;
        size_t source = 0;      // index into the flattened media instances
        uint32_t frame = 0;   // asset frame, clamped into the media
    };
    // Pure: it does no decode and changes no state.
    std::vector<Request> plan(uint32_t root_frame) const;

    // Loop wrap window [in, out) for the prewarm horizon; 0/0 turns it off.
    // Keep it off under a time remap, which does not wrap to a loop frame.
    void set_loop(uint32_t in_frame, uint32_t out_frame);

    // Frames stay alive until the next collect().
    // With scrub, a frame can be approximate; the caller must not store it.
    const std::vector<SourceFrame>& collect(uint32_t root_frame,
        bool scrub = false, const std::vector<std::pair<uint64_t, uint32_t>>* exact = nullptr);

    // Point of no return: every roll bails and later misses return null.
    void abort();

    // Frames decoded from disk that prewarm did not already hold.
    uint64_t misses() const { return misses_; }

    // True when the last collect served an approximate frame; do not store it.
    bool approximated() const { return approximated_; }

private:
    struct Stream {
        std::filesystem::path path;   // decodable file: source (native) or mez
        bool native = false;
        uint32_t frames = 0;
        uint64_t key = 0;             // the instance key
        // m guards the ring and want; hold it briefly, never across a decode.
        // cv fires on every ring insert; a consumer waits here, not on a
        // session mutex.
        std::mutex m;
        std::condition_variable cv;
        // Each slot owns its FILE* and scratch, so slots decode concurrently.
        struct Slot {
            std::mutex m;
            codec::MezReader reader;
            bool opened = false;   // guarded by the slot mutex
            bool ok = false;
        };
        static constexpr size_t kSlots = 4;
        Slot slots[kSlots];
        // The last decoded payload and its offset, guarded by m. An equal
        // offset reuses the pixels.
        uint64_t last_payload_off = 0;
        std::shared_ptr<const codec::DecodedFrame> last_payload_frame;
        // One mutex per session, held across a decode; the ring mutex
        // stays brief.
        struct Session {
            std::mutex m;
            platform::H264Decoder dec;
            void* file = nullptr;         // FILE* on the source
            bool created = false;         // guarded by the session mutex
            bool ok = false;
            uint32_t next_decode = 0;     // next decode-order sample to feed
            // -1 means the session must seek; run_key is its keyframe run
            // start, in decode order. The session mutex guards the writes;
            // reads are lock-free routing hints.
            std::atomic<int64_t> next_present{-1};
            std::atomic<int64_t> run_key{-1};
            // At most one advisory waiter may queue behind a busy owner.
            std::atomic<bool> waiter{false};
            bool draining = false;        // end of stream was signalled
            // Capacity must survive across fetches to stop a per-frame
            // allocation.
            platform::VideoFrameNV12 scratch;
            std::vector<uint8_t> sample_bytes;

            ~Session();
        };
        static constexpr size_t kSessions = 3;
        Session sessions[kSessions];
        // Built once on first use, then immutable; sessions share it.
        std::mutex index_m;
        bool index_built = false;
        bool index_ok = false;
        FrameIndex index;
        // Decoded frames by index, newest last. Bounded by ring_depth_.
        std::deque<std::pair<uint32_t, std::shared_ptr<const codec::DecodedFrame>>>
            ring;
        // Returns the ringed frame or null; the caller holds m.
        std::shared_ptr<const codec::DecodedFrame> ringed(uint32_t frame) const {
            for (const auto& e : ring)
                if (e.first == frame) return e.second;
            return nullptr;
        }
        bool has_ringed(uint32_t frame) const {
            for (const auto& e : ring)
                if (e.first == frame) return true;
            return false;
        }
        // Recycled frames, guarded by m; they keep capacity for the next
        // decode.
        std::vector<std::shared_ptr<codec::DecodedFrame>> spare;
        uint32_t want = 0;       // the frame the consumer last asked for
        uint32_t last_want = 0;  // previous ask: backward motion widens rolls
        // Upcoming block entries on this stream; 0xFFFFFFFF is an empty slot.
        // Their windows stay protected from eviction.
        static constexpr size_t kEntryMarks = 4;
        uint32_t next_entries[kEntryMarks] = {0xFFFFFFFFu, 0xFFFFFFFFu,
                                              0xFFFFFFFFu, 0xFFFFFFFFu};
        // Ring depth this stream needs, guarded by m: the live window plus
        // one window per entry mark.
        uint32_t ring_target = 0;
        // Idle-close bookkeeping; the consumer thread owns it, under map_m_.
        uint64_t last_touch = 0;
        bool idle_closed = false;
    };

    struct Job {
        uint64_t key = 0;
        uint32_t frame = 0;
        uint64_t gen = 0;
        // A cut preroll is exempt from the cold-seek distance gate.
        bool preroll = false;
        // Root frame the decode is needed by; the queue runs nearest
        // deadline first.
        uint32_t deadline = 0;
    };

    // Returns the canonical key, or the key itself when it is not in the fold.
    uint64_t alias_of(uint64_t key) const {
        const auto it = canonical_.find(key);
        return it != canonical_.end() ? it->second : key;
    }
    Stream* stream_for(const Request& req);
    // A ring miss waits at most one in-flight decode, never the backlog.
    std::shared_ptr<const codec::DecodedFrame> fetch(Stream& s, uint32_t frame,
                                                     bool* was_miss,
                                                     bool scrub = false,
                                                     bool preroll = false);
    std::shared_ptr<const codec::DecodedFrame> fetch_mez(Stream& s,
                                                         uint32_t frame);
    std::shared_ptr<const codec::DecodedFrame> fetch_native(Stream& s,
                                                            uint32_t frame,
                                                            bool advisory,
                                                            bool scrub,
                                                            bool preroll);
    std::shared_ptr<const codec::DecodedFrame> ring_insert(
        Stream& s, uint32_t frame,
        std::shared_ptr<const codec::DecodedFrame> decoded, size_t depth);
    static std::shared_ptr<codec::DecodedFrame> take_spare(Stream& s);
    void worker_main();
    void drain();
    void idle_close_scan();

    const char* name_;
    std::vector<doc::MediaInstance> sources_;
    // Instance key to stream identity; the consumer thread owns it.
    std::unordered_map<uint64_t, uint64_t> canonical_;
    // Loop wrap window, consumer thread only. out > in = active.
    uint32_t loop_in_ = 0;
    uint32_t loop_out_ = 0;
    std::vector<AssetBundle> bundles_;
    uint64_t revision_ = ~0ull;
    uint64_t look_ = 0;
    std::vector<SourceFrame> frames_;
    uint64_t misses_ = 0;
    bool approximated_ = false;   // consumer thread only
    // The decode workers read this size hint.
    std::atomic<uint32_t> ring_depth_{4};

    std::mutex map_m_;
    std::unordered_map<uint64_t, std::unique_ptr<Stream>> streams_;
    uint64_t collect_gen_ = 0;
    std::atomic<bool> abort_{false};

    std::mutex job_m_;
    std::condition_variable job_cv_;
    std::deque<Job> jobs_;
    uint64_t gen_ = 0;
    int busy_ = 0;
    bool quit_ = false;
    // While scrub is set, the consumer never decodes; the workers chase
    // the target. Advisories from before the epoch bail at the next sample.
    std::atomic<bool> scrub_{false};
    std::atomic<uint64_t> scrub_epoch_{0};
    bool prev_scrub_ = false;
    std::vector<std::thread> workers_;
};

}  // namespace looks::media

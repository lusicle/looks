// Frame render cache: CPU-side store of final rendered frames
// keyed on (frame index, upstream-graph hash) with an LRU byte budget
// (default 2 GB). The "upstream graph" of the final frame is the whole
// document plus everything else that shapes the output (proxy divisor,
// preview tap, clip identity); callers fold all of it into one context
// hash. A context change flushes the cache wholesale — an entry from an
// edited document can never be served.
//
// Only pure frames belong here: anything history-bearing (feedback,
// slit-scan, Codec-Box, ...) is not a function of (document, frame index)
// and must bypass the cache — see doc::document_uses_history.
//
// Vulkan-free on purpose: unit-tested without a GPU, like graph.cpp.

#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace looks::gfx {

class RenderCache {
public:
    struct Frame {
        std::vector<uint16_t> halves;   // RGBA16F, tightly packed
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t stamp = 0;             // LRU clock
    };

    // Changing the context invalidates every entry.
    void set_context(uint64_t context);
    uint64_t context() const { return context_; }

    // Lowering the budget evicts immediately; 0 disables (and clears).
    void set_budget(size_t bytes);
    size_t budget() const { return budget_; }
    size_t bytes() const { return total_bytes_; }
    size_t count() const { return frames_.size(); }
    uint64_t hits() const { return hits_; }

    // Freshens the entry's LRU stamp. Pointer valid until the next
    // insert/set_budget/set_context/clear.
    const Frame* find(uint32_t frame);

    // Takes ownership of `halves` (width*height*4 values). Dropped when
    // `context` is stale, the cache is disabled, or the single frame
    // exceeds the whole budget; otherwise evicts LRU entries to fit.
    void insert(uint64_t context, uint32_t frame, uint32_t width,
                uint32_t height, std::vector<uint16_t> halves);

    void clear();

private:
    void evict_to(size_t target_bytes);

    std::unordered_map<uint32_t, Frame> frames_;
    uint64_t context_ = 0;
    size_t budget_ = size_t{2048} << 20;   // default 2 GB
    size_t total_bytes_ = 0;
    uint64_t clock_ = 0;
    uint64_t hits_ = 0;
};

}  // namespace looks::gfx

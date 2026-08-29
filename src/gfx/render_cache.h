// History-bearing frames must bypass the cache: see document_uses_history.
// Keep this file Vulkan-free; unit tests link it without a GPU.

#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace looks::gfx {

class RenderCache {
public:
    struct Frame {
        std::vector<uint16_t> halves;  // RGBA16F, tightly packed
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t stamp = 0;
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

    // The pointer stays valid only until the next mutating call.
    const Frame* find(uint32_t frame);

    // halves holds width*height*4 values.
    // Stale context, disabled cache, or oversize frame drops inserts silently.
    void insert(uint64_t context, uint32_t frame, uint32_t width,
                uint32_t height, std::vector<uint16_t> halves);

    void clear();

private:
    void evict_to(size_t target_bytes);

    std::unordered_map<uint32_t, Frame> frames_;
    uint64_t context_ = 0;
    size_t budget_ = size_t{2048} << 20;
    size_t total_bytes_ = 0;
    uint64_t clock_ = 0;
    uint64_t hits_ = 0;
};

}  // namespace looks::gfx

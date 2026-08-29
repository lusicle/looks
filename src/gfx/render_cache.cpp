#include "gfx/render_cache.h"

namespace looks::gfx {

void RenderCache::set_context(uint64_t context) {
    if (context == context_) return;
    context_ = context;
    clear();
}

void RenderCache::set_budget(size_t bytes) {
    if (bytes == budget_) return;
    budget_ = bytes;
    evict_to(budget_);
}

const RenderCache::Frame* RenderCache::find(uint32_t frame) {
    auto it = frames_.find(frame);
    if (it == frames_.end()) return nullptr;
    it->second.stamp = ++clock_;
    ++hits_;
    return &it->second;
}

void RenderCache::insert(uint64_t context, uint32_t frame, uint32_t width,
                         uint32_t height, std::vector<uint16_t> halves) {
    if (context != context_ || budget_ == 0) return;
    const size_t bytes = halves.size() * sizeof(uint16_t);
    if (bytes == 0 || bytes > budget_ ||
        halves.size() != size_t{width} * height * 4)
        return;
    Frame& entry = frames_[frame];
    total_bytes_ -= entry.halves.size() * sizeof(uint16_t);
    entry.halves = std::move(halves);
    entry.width = width;
    entry.height = height;
    entry.stamp = ++clock_;
    total_bytes_ += bytes;
    // The fresh entry has the newest stamp; eviction takes it last.
    evict_to(budget_);
}

void RenderCache::clear() {
    frames_.clear();
    total_bytes_ = 0;
}

void RenderCache::evict_to(size_t target_bytes) {
    while (total_bytes_ > target_bytes && !frames_.empty()) {
        auto oldest = frames_.begin();
        for (auto it = frames_.begin(); it != frames_.end(); ++it)
            if (it->second.stamp < oldest->second.stamp) oldest = it;
        total_bytes_ -= oldest->second.halves.size() * sizeof(uint16_t);
        frames_.erase(oldest);
    }
}

}  // namespace looks::gfx

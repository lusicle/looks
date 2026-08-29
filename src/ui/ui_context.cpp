#include "ui/ui_context.h"

namespace looks::ui {

namespace {

size_t hash_pointer(const void* p, size_t mask) {
    uintptr_t bits = reinterpret_cast<uintptr_t>(p);
    return static_cast<size_t>(((bits ^ (bits >> 16)) * 0x9E3779B9u)) & mask;
}

}  // namespace

Context::Context() : slots_(256) {}

void Context::begin_frame() {
    ++frame_;
    hits_.clear();
    hit_order_ = 0;
    winner_ = {};
}

WidgetId Context::acquire_widget_id(const void* state_ptr) {
    if (!state_ptr) return {};
    for (;;) {
        const size_t mask = slots_.size() - 1;
        size_t i = hash_pointer(state_ptr, mask);
        for (size_t probes = 0; probes <= mask; ++probes, i = (i + 1) & mask) {
            Slot& slot = slots_[i];
            if (slot.state == state_ptr) {
                slot.last_acquired = frame_;
                return {static_cast<uint32_t>(i + 1), slot.generation};
            }
            if (slot.state == nullptr) {
                if (live_slots_ * 4 >= slots_.size() * 3) break;  // grow at 75%
                slot.state = state_ptr;
                slot.last_acquired = frame_;
                ++live_slots_;
                return {static_cast<uint32_t>(i + 1), slot.generation};
            }
        }
        grow_slots();
    }
}

void Context::grow_slots() {
    std::vector<Slot> old = std::move(slots_);
    slots_.assign(old.size() * 2, Slot{});
    // Slot indices change here; id_is_live rejects ids from before the grow.
    const size_t mask = slots_.size() - 1;
    for (const Slot& s : old) {
        if (!s.state) continue;
        size_t i = hash_pointer(s.state, mask);
        while (slots_[i].state) i = (i + 1) & mask;
        slots_[i] = s;
    }
}

void Context::gc_widget_slots() {
    for (Slot& slot : slots_) {
        if (slot.state && slot.last_acquired != frame_) {
            slot.state = nullptr;
            ++slot.generation;   // stale ids to this slot now compare unequal
            --live_slots_;
        }
    }
    if (!id_is_live(capture_)) capture_ = {};
}

bool Context::id_is_live(WidgetId id) const {
    if (id.is_null() || id.slot > slots_.size()) return false;
    const Slot& slot = slots_[id.slot - 1];
    return slot.state != nullptr && slot.generation == id.generation;
}

void Context::add_hit(const Rect& rect, WidgetId id, HitLayer layer) {
    if (rect.empty() || id.is_null()) return;
    hits_.push_back({rect, id, layer, hit_order_++});
}

void Context::finalize_hits(Vec2 mouse_logical) {
    winner_ = {};
    HitLayer best_layer = HitLayer::Tree;
    uint32_t best_order = 0;
    bool found = false;
    for (const HitEntry& e : hits_) {
        if (!e.rect.contains(mouse_logical)) continue;
        if (!found || e.layer > best_layer ||
            (e.layer == best_layer && e.order >= best_order)) {
            winner_ = e.id;
            best_layer = e.layer;
            best_order = e.order;
            found = true;
        }
    }
}

bool Context::has_capture() const {
    return id_is_live(capture_);
}

bool Context::widget_owns_mouse(WidgetId id) const {
    if (id.is_null()) return false;
    if (id_is_live(capture_)) return capture_ == id;
    return winner_ == id;
}

}  // namespace looks::ui

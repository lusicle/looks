#include "ui/ui_context.h"

#include <cmath>

namespace looks::ui {

namespace {

size_t hash_pointer(const void* p, size_t mask) {
    uintptr_t bits = reinterpret_cast<uintptr_t>(p);
    return static_cast<size_t>(((bits ^ (bits >> 16)) * 0x9E3779B9u)) & mask;
}

constexpr double kDoubleClickSeconds = 0.4;
constexpr float kDoubleClickSlop = 8.0f;

}  // namespace

Context::Context() : slots_(256) {}

void Context::begin_frame(const FrameInput& in) {
    ++frame_;
    hits_.clear();
    hit_order_ = 0;
    winner_ = {};
    winner_layer_ = HitLayer::Tree;
    modal_ = false;
    in_ = in;
    wheel_ = in.wheel_y;
    seconds_ += in.dt;
    if (in.buttons_down == 0 && in.buttons_released == 0) {
        capture_ = {};
        press_id_ = {};
        press_moved_ = false;
    }
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

WidgetId Context::acquire_widget_id(uint64_t key) {
    return acquire_widget_id(
        reinterpret_cast<const void*>(key | (1ull << 63)));
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
    if (!id_is_live(focus_)) focus_ = {};
}

bool Context::id_is_live(WidgetId id) const {
    if (id.is_null() || id.slot > slots_.size()) return false;
    const Slot& slot = slots_[id.slot - 1];
    return slot.state != nullptr && slot.generation == id.generation;
}

void Context::add_hit(const Rect& rect, WidgetId id, HitLayer layer) {
    if (rect.empty() || id.is_null()) return;
    if (layer == HitLayer::Modal) modal_ = true;
    hits_.push_back({rect, id, layer, hit_order_++});
}

void Context::push_overlay(const Rect& rect, WidgetId id, bool exclusive) {
    add_hit(rect, id, exclusive ? HitLayer::Modal : HitLayer::Popup);
}

void Context::finalize_hits() {
    winner_ = {};
    winner_layer_ = HitLayer::Tree;
    HitLayer best_layer = HitLayer::Tree;
    uint32_t best_order = 0;
    bool found = false;
    for (const HitEntry& e : hits_) {
        if (modal_ && e.layer != HitLayer::Modal) continue;
        if (!e.rect.contains(in_.mouse)) continue;
        if (!found || e.layer > best_layer ||
            (e.layer == best_layer && e.order >= best_order)) {
            winner_ = e.id;
            best_layer = e.layer;
            best_order = e.order;
            found = true;
        }
    }
    winner_layer_ = best_layer;
    if (in_.buttons_pressed && !focus_.is_null() && !(winner_ == focus_))
        focus_ = {};
}

float Context::take_wheel(WidgetId id) {
    if (wheel_ == 0.0f || !widget_owns_mouse(id)) return 0.0f;
    const float w = wheel_;
    wheel_ = 0.0f;
    return w;
}

float Context::take_wheel_in_tree() {
    if (wheel_ == 0.0f || pointer_over_overlay()) return 0.0f;
    const float w = wheel_;
    wheel_ = 0.0f;
    return w;
}

bool Context::drag_active() const {
    return id_is_live(capture_);
}

bool Context::widget_owns_mouse(WidgetId id) const {
    if (id.is_null()) return false;
    if (id_is_live(capture_)) return capture_ == id;
    return winner_ == id;
}

bool Context::take_double(uint64_t key) {
    ClickSlot* slot = nullptr;
    for (ClickSlot& s : click_slots_)
        if (s.key == key && s.time >= 0.0) slot = &s;
    if (slot) {
        const float away = std::fabs(in_.mouse.x - slot->pos.x) +
                           std::fabs(in_.mouse.y - slot->pos.y);
        if (seconds_ - slot->time < kDoubleClickSeconds &&
            away < kDoubleClickSlop) {
            slot->time = -1.0;
            return true;
        }
    } else {
        slot = &click_slots_[click_next_];
        click_next_ = (click_next_ + 1) % kClickSlots;
    }
    slot->key = key;
    slot->time = seconds_;
    slot->pos = in_.mouse;
    return false;
}

bool Context::press_is_double(uint64_t key) {
    return take_double(key | (1ull << 63));
}

Gesture Context::gesture(WidgetId id, const Rect& rect, float drag_px) {
    Gesture g;
    if (id.is_null()) return g;
    const bool owns = widget_owns_mouse(id);
    const bool mine = press_id_ == id && !id.is_null();
    g.hovered = owns;
    g.press_pos = press_pos_;

    if ((in_.buttons_pressed & kMouseLeft) && owns && !mine) {
        press_id_ = id;
        press_pos_ = in_.mouse;
        press_moved_ = false;
        begin_drag(id);
        g.pressed = true;
        g.press_pos = press_pos_;
        g.double_clicked = take_double(
            (static_cast<uint64_t>(id.slot) << 32) | id.generation);
    }
    if ((in_.buttons_pressed & kMouseRight) && owns &&
        rect.contains(in_.mouse))
        g.right_clicked = true;

    if (press_id_ == id) {
        const float moved = std::fabs(in_.mouse.x - press_pos_.x) +
                            std::fabs(in_.mouse.y - press_pos_.y);
        if (!press_moved_ && moved > drag_px) {
            press_moved_ = true;
            g.drag_started = true;
        }
        g.drag_active = capture_ == id;
        g.drag_moved = press_moved_;
        g.drag_delta = in_.mouse - press_pos_;
        if (in_.buttons_released & kMouseLeft) {
            g.drag_released = true;
            if (rect.contains(in_.mouse)) g.clicked = true;
            press_id_ = {};
            press_moved_ = false;
        }
    }
    return g;
}

}  // namespace looks::ui

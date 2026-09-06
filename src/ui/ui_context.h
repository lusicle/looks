// Widget identity keys on the caller-owned state pointer.

#pragma once

#include <cstdint>
#include <vector>

#include "ui/types.h"

namespace looks::ui {

struct WidgetId {
    uint32_t slot = 0;        // 0 = null
    uint32_t generation = 0;

    bool is_null() const { return slot == 0; }
    bool operator==(const WidgetId&) const = default;
};

enum class HitLayer : uint8_t { Tree = 0, Popup = 1, Modal = 2 };

struct FrameInput {
    Vec2 mouse{};
    uint8_t buttons_down = 0;
    uint8_t buttons_pressed = 0;
    uint8_t buttons_released = 0;
    float wheel_y = 0.0f;
    float dt = 0.0f;
};

struct Gesture {
    bool hovered = false;
    bool pressed = false;
    bool clicked = false;
    bool right_clicked = false;
    bool double_clicked = false;
    bool drag_active = false;
    bool drag_started = false;
    bool drag_moved = false;
    bool drag_released = false;
    Vec2 drag_delta{};
};

class Context {
public:
    Context();

    void begin_frame(const FrameInput& in);
    void gc_widget_slots();      // call after the draw pass

    WidgetId acquire_widget_id(const void* state_ptr);
    WidgetId acquire_widget_id(uint64_t key);

    // Hit rects are logical px and the caller clips them first.
    void add_hit(const Rect& rect, WidgetId id, HitLayer layer = HitLayer::Tree);
    void push_overlay(const Rect& rect, WidgetId id, bool exclusive);
    void push_hit_layer(HitLayer layer);
    void pop_hit_layer();
    void finalize_hits();
    WidgetId hit_winner() const { return winner_; }
    bool any_hit() const { return !winner_.is_null(); }
    bool modal_open() const { return modal_; }
    bool pointer_over_overlay() const {
        return modal_ || winner_layer_ > HitLayer::Tree;
    }

    float take_wheel(WidgetId id);
    float take_wheel_in_tree();
    float take_wheel_any();

    void set_focus(WidgetId id) { focus_ = id; }
    void clear_focus() { focus_ = {}; }
    WidgetId focus() const { return focus_; }
    bool has_focus(WidgetId id) const {
        return !id.is_null() && focus_ == id;
    }

    void begin_drag(WidgetId id) { capture_ = id; }
    bool drag_active() const;

    Gesture gesture(WidgetId id, const Rect& rect, float drag_px = 6.0f);
    bool press_is_double(uint64_t key);
    bool press_moved(WidgetId id) const {
        return !id.is_null() && press_id_ == id && press_moved_;
    }

    // A live drag wins; if there is none, the hit-test winner wins.
    bool widget_owns_mouse(WidgetId id) const;

    uint64_t frame() const { return frame_; }

    // The text pointer must stay valid for the whole frame.
    void set_tooltip(const char* text, Vec2 pos) {
        tooltip_ = text;
        tooltip_pos_ = pos;
    }
    const char* tooltip() const { return tooltip_; }
    Vec2 tooltip_pos() const { return tooltip_pos_; }
    void clear_tooltip() {
        tooltip_ = nullptr;
    }

    // Run RunPopup after run_frame and before the frame edit handlers.
    enum class PopupKind : uint8_t { List, Color };
    struct PopupRequest {
        PopupKind kind = PopupKind::List;
        Rect anchor;
        Rect rect;
        // List kind only.
        const char* const* items = nullptr;
        int count = 0;
        int selected = -1;
        void* state = nullptr;
        int* out_selected = nullptr;
        // Color kind only.
        float* out_rgb = nullptr;
        bool* out_changed = nullptr;
        bool* out_released = nullptr;
    };
    void set_popup(const PopupRequest& request) {
        popup_ = request;
        has_popup_ = true;
    }
    bool has_popup() const { return has_popup_; }
    const PopupRequest& popup() const { return popup_; }
    void clear_popup() { has_popup_ = false; }
    void set_popup_owner(void* owner) { popup_owner_ = owner; }
    void* popup_owner() const { return popup_owner_; }

private:
    const char* tooltip_ = nullptr;
    Vec2 tooltip_pos_{};
    PopupRequest popup_{};
    bool has_popup_ = false;
    void* popup_owner_ = nullptr;

    struct Slot {
        const void* state = nullptr;
        uint32_t generation = 0;
        uint64_t last_acquired = 0;
    };
    struct HitEntry {
        Rect rect;
        WidgetId id;
        HitLayer layer;
        uint32_t order;
    };

    bool id_is_live(WidgetId id) const;
    void grow_slots();

    std::vector<Slot> slots_;      // open-addressed, power-of-two
    size_t live_slots_ = 0;
    std::vector<HitEntry> hits_;
    std::vector<HitLayer> layer_stack_;
    WidgetId winner_{};
    HitLayer winner_layer_ = HitLayer::Tree;
    WidgetId capture_{};
    WidgetId focus_{};
    bool modal_ = false;
    float wheel_ = 0.0f;
    uint64_t frame_ = 0;
    uint32_t hit_order_ = 0;

    FrameInput in_{};
    double seconds_ = 0.0;
    WidgetId press_id_{};
    Vec2 press_pos_{};
    bool press_moved_ = false;
    struct ClickSlot {
        uint64_t key = 0;
        double time = -1.0;
        Vec2 pos{};
    };
    static constexpr size_t kClickSlots = 8;
    ClickSlot click_slots_[kClickSlots];
    size_t click_next_ = 0;

    bool take_double(uint64_t key);
};

}  // namespace looks::ui

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

enum class HitLayer : uint8_t { Tree = 0, Popup = 1, Overlay = 2, Modal = 3 };

class Context {
public:
    Context();

    void begin_frame();
    void gc_widget_slots();      // call after the draw pass

    WidgetId acquire_widget_id(const void* state_ptr);

    // Hit rects are logical px and the caller clips them first.
    void add_hit(const Rect& rect, WidgetId id, HitLayer layer = HitLayer::Tree);
    void finalize_hits(Vec2 mouse_logical);
    WidgetId hit_winner() const { return winner_; }
    bool any_hit() const { return !winner_.is_null(); }

    void set_capture(WidgetId id) { capture_ = id; }
    void clear_capture() { capture_ = {}; }
    bool has_capture() const;

    // A live capture wins; if there is none, the hit-test winner wins.
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
    WidgetId winner_{};
    WidgetId capture_{};
    uint64_t frame_ = 0;
    uint32_t hit_order_ = 0;
};

}  // namespace looks::ui

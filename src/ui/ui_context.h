// Context — cross-frame UI interaction state (first-party mirror of the
// reference toolkit's ui_context).
//
// Widget identity is keyed on the WIDGET-STATE POINTER: the same
// caller-owned state struct yields the same WidgetId every frame via an
// open-addressed slot table. Slots not re-acquired in a frame are recycled
// by gc_widget_slots() with a generation bump, so stale captures compare
// unequal instead of aliasing a new widget.
//
// Hit testing: widgets register rects during the hit pass; finalize() picks
// the top entry under the cursor by (layer, registration order). Input
// capture (drags) overrides the winner until released.

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

    // ---- frame flow
    void begin_frame();          // bumps frame counter, resets hit state
    void gc_widget_slots();      // call after the draw pass

    // ---- widget identity
    WidgetId acquire_widget_id(const void* state_ptr);

    // ---- hit testing (rects in logical px, pre-clipped by the caller)
    void add_hit(const Rect& rect, WidgetId id, HitLayer layer = HitLayer::Tree);
    void finalize_hits(Vec2 mouse_logical);
    WidgetId hit_winner() const { return winner_; }
    bool any_hit() const { return !winner_.is_null(); }

    // ---- capture (drag ownership)
    void set_capture(WidgetId id) { capture_ = id; }
    void clear_capture() { capture_ = {}; }
    bool has_capture() const;

    // The one canonical mouse-ownership check widgets use: a live capture
    // wins; otherwise the hit-test winner.
    bool widget_owns_mouse(WidgetId id) const;

    uint64_t frame() const { return frame_; }

    // ---- deferred tooltip: a widget sets it during the draw pass after a
    // sustained hover; the app renders it once after run_frame so it
    // overlays everything. Pointer must stay valid through the frame
    // (string literals / arena copies).
    void set_tooltip(const char* text, Vec2 pos) {
        tooltip_ = text;
        tooltip_pos_ = pos;
    }
    const char* tooltip() const { return tooltip_; }
    Vec2 tooltip_pos() const { return tooltip_pos_; }
    void clear_tooltip() {
        tooltip_ = nullptr;
    }

    // ---- deferred dropdown popup. The open Dropdown registers its list
    // each frame during draw; the app runs RunPopup right after run_frame —
    // BEFORE the frame's edit handlers — so a selection lands in its
    // out-param in time to be applied the same frame. `owner` (persistent)
    // enforces a single open popup.
    struct PopupRequest {
        Rect anchor;
        Rect rect;
        const char* const* items = nullptr;
        int count = 0;
        int selected = -1;
        void* state = nullptr;
        int* out_selected = nullptr;
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

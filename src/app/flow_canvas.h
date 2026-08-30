#pragma once

#include <cstdint>

#include "ui/layout.h"

namespace looks::ui {
struct UiTexture;
struct SwatchState;
}

namespace looks::flow {

enum class NodeKind : uint8_t {
    Source, Effect, ModSource, Output,
    Frame,    // id tag for FrameBox events only, never a card
    Group,
    GroupIn,
    GroupOut, // GroupIn and GroupOut are derived: never move or delete them
};

// A node id is a kind tag byte plus a doc id. Use these accessors, not shifts.
inline uint64_t node_id(NodeKind kind, uint64_t doc_id) {
    return (static_cast<uint64_t>(kind) + 1ull) << 56 |
           (doc_id & 0x00FFFFFFFFFFFFFFull);
}
inline NodeKind node_kind_of(uint64_t id) {
    return static_cast<NodeKind>((id >> 56) - 1);
}
inline uint64_t node_doc_id(uint64_t id) {
    return id & 0x00FFFFFFFFFFFFFFull;
}
inline constexpr uint64_t kOutNodeId = 0xFFull << 56;

struct ParamRow {
    const char* label = "";
    float min_v = 0.0f, max_v = 1.0f;
    // Typed values can go up to hard_max. 0 means clamp to max_v.
    float hard_max = 0.0f;
    const char* format = "%.2f";
    // The row shows value * display_scale. Typed input divides it back.
    // A "deg" format makes the row a dial.
    float display_scale = 1.0f;
    float* staged = nullptr;
    bool* changed = nullptr;
    bool* released = nullptr;
    // 0 slider, 1 dropdown, 2 text, 3 swatch, 4 button, 5 status label.
    // A swatch stages three floats (rgb); the others stage one.
    uint8_t kind = 0;
    const char* options = nullptr;
    const char* text = nullptr;
    ui::SwatchState* swatch = nullptr;
    float live = 0.0f;
    bool has_live = false;
    bool value_input = false;
    // A null pointer hides the hotspot.
    bool* route_clicked = nullptr;
    bool* key_clicked = nullptr;
    bool* expose_clicked = nullptr;
    bool exposed = false;
    bool modulated = false;
    bool keyed = false;
};

struct Node {
    uint64_t id = 0;
    NodeKind kind = NodeKind::Effect;
    const char* title = "";
    float x = 0.0f, y = 0.0f;        // graph units, top left corner
    uint8_t tint = 255;              // 255 = no tint
    bool bypassed = false;
    bool solo = false;
    bool feedback = false;
    bool has_in = false;
    bool has_matte_port = false;
    // Port 1 label. A null pointer shows "matte".
    const char* matte_label = nullptr;
    bool has_aux_port = false;
    const char* aux_label = "b";
    // Slot k uses flow port k + 1. Port 0 is In and port 1 is matte.
    // The ghost slot uses flow port slot_rows + 2.
    int slot_rows = 0;
    bool ghost_in = false;
    // Exit row k is slot k. Wires from row k carry from_port = k.
    int exit_rows = 0;
    bool has_out = false;
    const ui::UiTexture* preview = nullptr;
    float pu0 = 0.0f, pv0 = 0.0f, pu1 = 1.0f, pv1 = 1.0f;
    // wave_in and wave_out hold interleaved lo, hi pairs, one pair per column.
    // The values stay in the range -1 to 1.
    bool wave_card = false;
    const float* wave_in = nullptr;
    const float* wave_out = nullptr;
    int wave_count = 0;
    // scope[0] is the value at the playhead. The window goes forward in time.
    const float* scope = nullptr;
    const float* scope_min = nullptr;
    const float* scope_max = nullptr;
    int scope_count = 0;
    float scope_lo = 0.0f, scope_hi = 1.0f;
    const ParamRow* rows = nullptr;
    int row_count = 0;
    bool* remove_clicked = nullptr;  // a null pointer hides the X
    bool* bypass_clicked = nullptr;
    bool text_edit = false;
    bool is_look = false;
};

// Media wire ports: 0 In, 1 matte or audio, 2 aux. Data wires use to_row only.
struct Wire {
    uint64_t from = 0;
    uint64_t to = 0;
    uint32_t to_port = 0;
    bool data = false;
    // -1 means no row: the wire lands on the card edge.
    int to_row = -1;
    uint32_t from_port = 0;
};

struct FrameBox {
    uint64_t id = 0;
    float x = 0.0f, y = 0.0f, w = 480.0f, h = 360.0f;
    const char* title = "";
    uint32_t color = 0;              // 0 = none, 1 to 8 = palette hue
    bool* remove_clicked = nullptr;
    bool* color_clicked = nullptr;
};

struct Graph {
    const Node* nodes = nullptr;
    size_t node_count = 0;
    const Wire* wires = nullptr;
    size_t wire_count = 0;
    const FrameBox* frames = nullptr;
    size_t frame_count = 0;
    uint64_t selected = 0;
    const uint64_t* multi = nullptr;
    size_t multi_count = 0;
    // add_headers has add_count entries. A header row is not pickable.
    const char* const* add_items = nullptr;
    size_t add_count = 0;
    const char* add_filter = "";
    const uint8_t* add_headers = nullptr;
    const char* const* ctx_items = nullptr;
    size_t ctx_count = 0;
    // Wires match on endpoints and kind. A to_row of -1 matches any row.
    const Wire* sel_wires = nullptr;
    size_t sel_wire_count = 0;
    uint64_t rename_frame = 0;
    uint64_t rename_node = 0;
    const char* rename_text = "";
    uint64_t value_edit_node = 0;
    int value_edit_row = -1;
    const char* value_edit_text = "";
    const char* crumb = nullptr;
    const char* hint = nullptr;
};

struct CanvasState {
    // screen = graph * zoom + pan
    float pan_x = 0.0f, pan_y = 0.0f;
    float zoom = 1.0f;
    bool view_inited = false;
    uint64_t hover = 0;
    // 1 node 2 pan 3 slider 4 wire 5 rewire 6 marquee 7 resize 8 dial+reverse
    uint8_t drag_kind = 0;
    uint64_t drag_id = 0;
    int drag_row = -1;
    uint64_t wire_from = 0;
    uint32_t wire_from_port = 0;
    uint64_t wire_old_to = 0;
    uint32_t wire_old_port = 0;
    Vec2 press_screen{};
    float node_grab_x = 0.0f, node_grab_y = 0.0f;   // grab offset, graph units
    bool drag_moved = false;
    // Seconds on the canvas clock. -1 = no armed first click.
    double clock = 0.0;
    double last_click_time = -1.0;
    Vec2 last_click_pos{};
    uint64_t last_click_id = 0;
    Vec2 last_mouse{};
    float last_gx = 0.0f, last_gy = 0.0f;
    // One shot. The canvas clears it after the view centers. 0 = idle.
    uint64_t center_on = 0;
    // add_anchor is screen space. add_gx and add_gy are graph space.
    bool add_open = false;
    Vec2 add_anchor{};
    float add_gx = 0.0f, add_gy = 0.0f;
    float add_scroll = 0.0f;
    // Flat index into add_items of the open category. -1 = none.
    int add_cat = -1;
    uint64_t splice_from = 0, splice_to = 0;
    uint32_t splice_port = 0;
    // dd_field is the field rect in screen space at open time.
    bool dd_open = false;
    uint64_t dd_node = 0;
    int dd_row = -1;
    ui::Rect dd_field{};
    ui::SwatchState* swatch_open = nullptr;
    ui::Rect swatch_anchor{};
    // dial_accum stays unsnapped so drags below one degree accumulate.
    Vec2 dial_center{};
    float dial_last = 0.0f;
    float dial_accum = 0.0f;
    uint64_t drag_splice_from = 0, drag_splice_to = 0;
    uint32_t drag_splice_port = 0;
    // ctx_target holds a tagged node id, not a raw document id.
    bool ctx_open = false;
    Vec2 ctx_anchor{};
    uint64_t ctx_target = 0;
    // The popup rows show link order. The top row draws last.
    bool port_menu_open = false;
    Vec2 port_menu_anchor{};
    uint64_t port_menu_node = 0;
    uint32_t port_menu_port = 0;
    // Seconds on the canvas clock, like last_click_time.
    double last_port_time = -1.0;
    Vec2 last_port_pos{};
    uint64_t last_port_node = 0;
    uint32_t last_port_port = 0;
    bool drag_alt = false;
};

struct Output {
    uint64_t clicked = 0;
    bool clicked_shift = false;
    bool clicked_empty = false;
    // Graph space rect in min then max order.
    bool marquee_done = false;
    float mq_x0 = 0.0f, mq_y0 = 0.0f, mq_x1 = 0.0f, mq_y1 = 0.0f;
    int mq_wire_count = 0;
    Wire mq_wires[64];
    // moved_x and moved_y use graph units. move_released ends coalescing.
    uint64_t moved = 0;
    float moved_x = 0.0f, moved_y = 0.0f;
    bool move_released = false;
    // add_x and add_y use graph units.
    bool add_requested = false;
    float add_x = 0.0f, add_y = 0.0f;
    // add_pick indexes Graph::add_items. -1 = none.
    bool add_menu_opened = false;
    int add_pick = -1;
    // A rewire emits disconnect and connect in the same frame.
    bool connect_requested = false;
    uint64_t connect_from = 0, connect_to = 0;
    uint32_t connect_port = 0;
    uint32_t connect_from_port = 0;
    bool disconnect_requested = false;
    uint64_t disconnect_from = 0, disconnect_to = 0;
    uint32_t disconnect_port = 0;
    uint32_t disconnect_from_port = 0;
    bool route_drop_requested = false;
    uint64_t route_drop_from = 0;
    uint64_t route_drop_to = 0;
    int route_drop_row = -1;
    bool wire_clicked = false;
    bool wire_clicked_shift = false;
    uint64_t wire_from = 0, wire_to = 0;
    uint32_t wire_to_port = 0;
    uint32_t wire_from_port = 0;
    bool wire_data = false;
    int wire_to_row = -1;
    // ctx_pick indexes ctx_items. Reset it to -1 after each arena alloc.
    int ctx_pick = -1;
    uint64_t ctx_node = 0;
    uint64_t group_rename = 0;
    uint64_t text_edit = 0;
    uint64_t frame_resized = 0;
    float frame_w = 0.0f, frame_h = 0.0f;
    bool frame_resize_released = false;
    uint64_t frame_rename = 0;
    uint64_t group_open = 0;
    uint64_t look_open = 0;
    bool crumb_clicked = false;
    uint64_t value_edit_node = 0;
    int value_edit_row = -1;
    bool node_splice_requested = false;
    uint64_t splice_node = 0;
    uint64_t splice_wire_from = 0, splice_wire_to = 0;
    uint32_t splice_wire_port = 0;
    bool moved_alt = false;
    // reorder_index counts from the bottom. A delta of +1 moves to the top.
    bool port_reorder = false;
    uint64_t reorder_node = 0;
    uint32_t reorder_port = 0;
    int reorder_index = -1;
    int reorder_delta = 0;
};

ui::LayoutNode* FlowCanvas(ui::LayoutArena& arena, const Graph* graph,
                           CanvasState* state, Output* out);

float node_width();
float node_height(int row_count, bool has_preview, int port_rows);
float node_height_of(const Node& nd);

// The point is in screen space. The ids come back canvas tagged.
bool pick_wire(const Graph& g, const CanvasState& st, const ui::Rect& canvas,
               Vec2 screen, uint64_t* from, uint64_t* to, uint32_t* port);

}  // namespace looks::flow

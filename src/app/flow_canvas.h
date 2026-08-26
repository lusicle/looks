// Node canvas - the full node editor. Document-agnostic: the app
// translates the document into a flow::Graph each frame
// (positions from the document, derived auto-layout for unplaced nodes)
// and turns flow::Output events + staged param edits into commands. The
// widget owns pan/zoom, card chrome, inline sliders, ports, and wires —
// drawn entirely with Canvas2D primitives in graph space.

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
    Frame,    // id-tag space for FrameBox move events only
    Group,    // doc::Group as one card (subgraphs)
    GroupIn,  // boundary nodes inside the OPEN group's scoped view
    GroupOut, // (texed sgin/sgout) — derived, never move or delete
};

// Node identity: tag byte | document id (effect/route ids come from
// separate counters and may collide across kinds).
inline uint64_t node_id(NodeKind kind, uint64_t doc_id) {
    return (static_cast<uint64_t>(kind) + 1ull) << 56 |
           (doc_id & 0x00FFFFFFFFFFFFFFull);
}
inline constexpr uint64_t kOutNodeId = 0xFFull << 56;

// One inline parameter row. The canvas writes staged/changed/released
// during slider drags — the app's existing post-frame handlers apply them
// as coalesced commands, exactly like the panel sliders did.
struct ParamRow {
    const char* label = "";
    float min_v = 0.0f, max_v = 1.0f;
    // Typed values may exceed the SLIDER range up to here (0 = they
    // clamp to max_v like the slider). For params whose kernel wraps or
    // is open-ended (oscillator phase past one period).
    float hard_max = 0.0f;
    const char* format = "%.2f";
    // Readout multiplier: the row DISPLAYS value*display_scale (radian
    // params speak degrees). Typed input divides back. A "deg" format
    // renders the row as a DIAL instead of a slider.
    float display_scale = 1.0f;
    float* staged = nullptr;
    bool* changed = nullptr;
    bool* released = nullptr;
    // Row kind (real controls): 0 slider; 1 dropdown — `options` is
    // a '|'-separated list, *staged holds the index, a pick stages it
    // exactly like a slider release; 2 text — `text` shows in the field,
    // a click emits Output::text_edit (the shared inline editor draws in
    // the field while active); 3 swatch — `staged` points at THREE
    // floats (rgb), `swatch` holds the picker state, a click opens the
    // shared color popup; 4 button — `text` is the label, a click sets
    // *changed (probed by the label); 5 status label — `text` drawn
    // dim, inert. Same geometry as slider rows.
    uint8_t kind = 0;
    const char* options = nullptr;
    const char* text = nullptr;
    ui::SwatchState* swatch = nullptr;
    // Live resolved value at the playhead (lanes + wires baked): a
    // driven row draws an accent tick at this position so modulation
    // visibly moves, while the slider keeps editing the stored base.
    float live = 0.0f;
    bool has_live = false;
    // Helper-node operand row: accepts a value out-wire drop (rings up
    // as a candidate during the drag) without carrying the param-row
    // route micro.
    bool value_input = false;
    // Row micro-hotspots; null hides each.
    bool* route_clicked = nullptr;
    bool* key_clicked = nullptr;
    // Scoped-view member rows: toggle this param on/off the open
    // group's FACE; `exposed` fills the dot.
    bool* expose_clicked = nullptr;
    bool exposed = false;
    bool modulated = false;          // tint the label: driven by a route
    bool keyed = false;              // tint the label: has a keyframe lane
};

struct Node {
    uint64_t id = 0;
    NodeKind kind = NodeKind::Effect;
    const char* title = "";
    float x = 0.0f, y = 0.0f;        // graph units (top-left)
    uint8_t tint = 255;              // category strip; 255 = none
    bool bypassed = false;
    bool solo = false;
    bool feedback = false;           // self-loop glyph in the title bar
    bool has_in = false;
    bool has_matte_port = false;
    // Port-1 label; null = "matte". The Output relabels its port-1 slot
    // "audio" (the split-mode audio-in rides the same anchor + wire kind).
    const char* matte_label = nullptr;
    bool has_aux_port = false;   // second image input (N-ports)
    const char* aux_label = "b"; // port name on the card ("b", "map")
    bool has_out = false;
    // Live preview: an atlas cell (draw_image_quad); null = flat slot.
    const ui::UiTexture* preview = nullptr;
    float pu0 = 0.0f, pv0 = 0.0f, pu1 = 1.0f, pv1 = 1.0f;
    // Value-node scope: the node's output sampled over a window that
    // starts at the playhead, drawn as a strip under the title (sample
    // 0 is NOW). lo/hi frame the plot; a bipolar window keeps its zero
    // baseline visible.
    const float* scope = nullptr;
    int scope_count = 0;
    float scope_lo = 0.0f, scope_hi = 1.0f;
    const ParamRow* rows = nullptr;
    int row_count = 0;
    // Title-bar actions, staged per frame by the app.
    bool* remove_clicked = nullptr;  // X (null hides it)
    bool* bypass_clicked = nullptr;  // enable dot
    // Text node: a title double-click edits the STRING (the same
    // inline editor groups use for renames).
    bool text_edit = false;
    // A LOOK INSTANCE card: a body double-click enters the
    // referenced look, the way it enters a group's subgraph.
    bool is_look = false;
};

// TWO families, nothing else: MEDIA wires mirror the doc link's
// to_port verbatim (0 = In, 1 = matte/audio slot, 2 = aux) and draw
// one generic solid style; DATA wires (the value graph) land on param
// rows and draw dashed dim. Selection alone wears the accent.
struct Wire {
    uint64_t from = 0;   // leaves from's Out port
    uint64_t to = 0;
    uint32_t to_port = 0;   // media: the doc link's port; data: unused
    bool data = false;
    // Data wires land on the driven PARAM's row instead of the card
    // edge; -1 = no row (card-edge fallback).
    int to_row = -1;
};

// Titled grouping box (texed frames): drawn behind the cards, dragged by
// its title strip, removed via its X. Pure annotation.
struct FrameBox {
    uint64_t id = 0;
    float x = 0.0f, y = 0.0f, w = 480.0f, h = 360.0f;
    const char* title = "";
    uint32_t color = 0;              // 0 none, 1..8 palette hue
    bool* remove_clicked = nullptr;
    bool* color_clicked = nullptr;   // title-strip dot cycles the tag
};

struct Graph {
    const Node* nodes = nullptr;
    size_t node_count = 0;
    const Wire* wires = nullptr;
    size_t wire_count = 0;
    const FrameBox* frames = nullptr;
    size_t frame_count = 0;
    uint64_t selected = 0;
    // Multi-selection (texed): every id here draws the accent outline;
    // group move/delete operate on the set. `selected` stays the primary.
    const uint64_t* multi = nullptr;
    size_t multi_count = 0;
    // Add-menu content (texed openAddMenu): the app passes the FILTERED
    // effect labels each frame plus the live filter text to display.
    // add_headers marks category header rows (dim, never pickable).
    const char* const* add_items = nullptr;
    size_t add_count = 0;
    const char* add_filter = "";
    const uint8_t* add_headers = nullptr;
    // Context-menu content (texed popupMenu): the app builds the item
    // list for state.ctx_target each frame while the menu is open.
    const char* const* ctx_items = nullptr;
    size_t ctx_count = 0;
    // Selected wires (click / marquee, texed sel.links): matched by
    // endpoints + kind; to_row -1 matches any row of that pair. Draw
    // accent, Delete cuts all.
    const Wire* sel_wires = nullptr;
    size_t sel_wire_count = 0;
    // Inline frame rename in flight: that frame draws rename_text + caret
    // instead of its title (the app owns the edit buffer). rename_node
    // does the same for a group CARD's title (texed subgraph rename).
    uint64_t rename_frame = 0;
    uint64_t rename_node = 0;
    const char* rename_text = "";
    // Inline value edit (texed click-to-type): this node+row draws the
    // typed buffer + caret instead of its formatted value.
    uint64_t value_edit_node = 0;
    int value_edit_row = -1;
    const char* value_edit_text = "";
    // Subgraph view (texed breadcrumbs): non-null = the canvas is scoped
    // to an open group of this name; "main" in the crumb exits.
    const char* crumb = nullptr;
    // Empty-canvas hint, drawn centered and dim when there are no nodes
    // (a sequence scope has no graph to show).
    const char* hint = nullptr;
};

struct CanvasState {
    // View transform: screen = graph * zoom + pan.
    float pan_x = 0.0f, pan_y = 0.0f;
    float zoom = 1.0f;
    bool view_inited = false;        // first frame: fit content
    uint64_t hover = 0;
    // Interaction in flight (element addressed by node id + row).
    // 4 = wire from an Out port, 5 = rewire (grabbed a fed In/matte port),
    // 6 = marquee (shift+drag on empty canvas), 7 = frame corner resize.
    uint8_t drag_kind = 0;           // 0 none, 1 node, 2 pan, 3 slider
    uint64_t drag_id = 0;
    int drag_row = -1;
    uint64_t wire_from = 0;          // wire drag origin node
    uint64_t wire_old_to = 0;        // rewire: the grabbed link's consumer
    uint32_t wire_old_port = 0;
    Vec2 press_screen{};
    float node_grab_x = 0.0f, node_grab_y = 0.0f;   // grab offset (graph)
    bool drag_moved = false;
    // Double-click detection: SECONDS on the canvas's accumulated
    // clock (a frame-count window shrinks with the UI rate and misses
    // real mouse doubles). -1 = no armed first click.
    double clock = 0.0;
    double last_click_time = -1.0;
    Vec2 last_click_pos{};
    uint64_t last_click_id = 0;      // what the click landed on
    // Cursor tracking published every frame: paste-at-cursor and the
    // find popup anchor read these outside canvas event flow.
    Vec2 last_mouse{};
    float last_gx = 0.0f, last_gy = 0.0f;
    // One-shot: pan so this node is centered (find jump); 0 = idle.
    uint64_t center_on = 0;
    // Add menu at the cursor (texed): open state + anchor (screen) +
    // spawn point (graph) + the wire being spliced (0 0 0 = none). The
    // app closes it by clearing add_open (Escape, add applied).
    bool add_open = false;
    Vec2 add_anchor{};
    float add_gx = 0.0f, add_gy = 0.0f;
    float add_scroll = 0.0f;
    // With no filter the popup lists CATEGORY rows; the hovered one
    // opens a flyout submenu (user request). Flat index of the open
    // category header; -1 = none. Typing collapses to the flat list.
    int add_cat = -1;
    uint64_t splice_from = 0, splice_to = 0;
    uint32_t splice_port = 0;
    // Param dropdown popup: the open field's node/row + its
    // screen rect captured at open time. Esc/click-away closes.
    bool dd_open = false;
    uint64_t dd_node = 0;
    int dd_row = -1;
    ui::Rect dd_field{};
    // Open card color swatch: the shared picker popup rides the row's
    // persistent SwatchState; the anchor is the field rect at open.
    ui::SwatchState* swatch_open = nullptr;
    ui::Rect swatch_anchor{};
    // Dial-row drag (deg-formatted rows): angular, relative, integrated
    // unsnapped so slow drags below one degree still accumulate.
    Vec2 dial_center{};
    float dial_last = 0.0f;
    float dial_accum = 0.0f;
    // Splice-on-drop (texed _findSpliceLink): while dragging a single
    // unfed node over a wire, that wire highlights and a drop splices.
    uint64_t drag_splice_from = 0, drag_splice_to = 0;
    uint32_t drag_splice_port = 0;
    // Context menu (texed openNodeMenu/openFrameMenu): open state +
    // anchor (screen) + the card/frame it targets (node_id tagged).
    bool ctx_open = false;
    Vec2 ctx_anchor{};
    uint64_t ctx_target = 0;
    // Port STACK popup: double-click on an input port with two or more
    // feeds lists them in stacking order (link order, top row = drawn
    // last) with reorder arrows. Rows re-read the wires every frame, so
    // a reorder shows live while the popup stays open.
    bool port_menu_open = false;
    Vec2 port_menu_anchor{};
    uint64_t port_menu_node = 0;
    uint32_t port_menu_port = 0;
    // Double-click detection on input ports (ports grab wires on press,
    // so they need their own tracker). Seconds, like last_click_time.
    double last_port_time = -1.0;
    Vec2 last_port_pos{};
    uint64_t last_port_node = 0;
    uint32_t last_port_port = 0;
    // Alt held when a frame drag started: move the frame WITHOUT its
    // contained nodes (texed alt+drag).
    bool drag_alt = false;
};

struct Output {
    uint64_t clicked = 0;            // select (kOutNodeId = Output card)
    bool clicked_shift = false;      // shift-click: toggle in the multi set
    bool clicked_empty = false;      // deselect
    // Marquee (shift+drag on empty): graph-space rect, min/max order,
    // plus every wire whose stroke crosses the rect (texed link marquee).
    bool marquee_done = false;
    float mq_x0 = 0.0f, mq_y0 = 0.0f, mq_x1 = 0.0f, mq_y1 = 0.0f;
    int mq_wire_count = 0;
    Wire mq_wires[64];
    // Node move: streamed while dragging (coalesced command), released on
    // mouse-up so the app can break coalescing.
    uint64_t moved = 0;
    float moved_x = 0.0f, moved_y = 0.0f;
    bool move_released = false;
    // Double-click on empty canvas: add-node request at this graph pos.
    bool add_requested = false;
    float add_x = 0.0f, add_y = 0.0f;
    // Cursor add menu: opened this frame (app focuses the filter), or an
    // item picked (index into Graph::add_items; splice data in state).
    bool add_menu_opened = false;
    int add_pick = -1;
    // Wire edits: connect from→(to, port); port 0 = In, 1 = matte.
    // A rewire release emits disconnect (the grabbed link) + connect.
    bool connect_requested = false;
    uint64_t connect_from = 0, connect_to = 0;
    uint32_t connect_port = 0;
    bool disconnect_requested = false;
    uint64_t disconnect_from = 0, disconnect_to = 0;
    uint32_t disconnect_port = 0;
    // Value-node wiring: a ModSource out wire dropped on a param row
    // retargets that route to the row's param.
    bool route_drop_requested = false;
    uint64_t route_drop_from = 0;    // ModSource canvas id
    uint64_t route_drop_to = 0;      // target card canvas id
    int route_drop_row = -1;         // row index on the target card
    // Wire click-select: a still click landing on a wire's bezier.
    // Shift toggles it in the wire selection instead of replacing.
    // wire_to_row keeps mod wires distinct when one value node drives
    // several rows of the same card.
    bool wire_clicked = false;
    bool wire_clicked_shift = false;
    uint64_t wire_from = 0, wire_to = 0;
    uint32_t wire_to_port = 0;
    bool wire_data = false;
    int wire_to_row = -1;
    // Context menu: item picked this frame (index into Graph::ctx_items;
    // -1 = none — the app MUST re-init the sentinel after arena alloc),
    // with the target the menu was opened on.
    int ctx_pick = -1;
    uint64_t ctx_node = 0;
    // Double-click on a group CARD's title strip: inline rename request
    // (texed renames subgraphs by title, opens by body).
    uint64_t group_rename = 0;
    // Double-click on a Text card's title strip: edit its string.
    uint64_t text_edit = 0;
    // Frame gestures: corner-resize stream (coalesced command) and the
    // double-click rename request on a title strip.
    uint64_t frame_resized = 0;
    float frame_w = 0.0f, frame_h = 0.0f;
    bool frame_resize_released = false;
    uint64_t frame_rename = 0;
    // Double-click on a Group card: ENTER its scoped subgraph view
    // (texed enterSubgraph).
    uint64_t group_open = 0;
    // Double-click on a look-instance card: ENTER that look (the canvas
    // and the timeline scope together).
    uint64_t look_open = 0;
    // Breadcrumb "main" clicked: exit the open group's view, or leave the
    // scoped look for the project.
    bool crumb_clicked = false;
    // Double-click on a row's slider: type the exact value.
    uint64_t value_edit_node = 0;
    int value_edit_row = -1;
    // Splice-on-drop: the dragged node released over this wire.
    bool node_splice_requested = false;
    uint64_t splice_node = 0;
    uint64_t splice_wire_from = 0, splice_wire_to = 0;
    uint32_t splice_wire_port = 0;
    // Frame move with alt held: the frame moves alone.
    bool moved_alt = false;
    // Port stack popup: one feed swapped with its neighbour
    // (reorder_index counts from the BOTTOM of the fan-in, delta +1 =
    // toward the top). The app permutes the link vector, un-coalesced.
    bool port_reorder = false;
    uint64_t reorder_node = 0;
    uint32_t reorder_port = 0;
    int reorder_index = -1;
    int reorder_delta = 0;
};

ui::LayoutNode* FlowCanvas(ui::LayoutArena& arena, const Graph* graph,
                           CanvasState* state, Output* out);

// Card geometry helpers shared with the app's auto-layout. Matte/aux
// ports occupy dedicated strip rows between the preview and the param
// rows (they must never overlap a param row), so height depends on them.
float node_width();
float node_height(int row_count, bool has_preview, int port_rows = 0);
float node_height_of(const Node& nd);

// The image wire nearest a SCREEN position (within the canvas's own
// splice pick radius) — external drops (the preset drag) target their
// splice through this. Endpoint ids are canvas-tagged, exactly as
// FlowEvents carries them; false = no wire close enough.
bool pick_wire(const Graph& g, const CanvasState& st, const ui::Rect& canvas,
               Vec2 screen, uint64_t* from, uint64_t* to, uint32_t* port);

}  // namespace looks::flow

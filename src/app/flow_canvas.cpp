#include "app/flow_canvas.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ui/interact.h"
#include "ui/probe.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace looks::flow {

namespace {

using ui::Color;
using ui::Rect;

// Card geometry in graph units at zoom 1.
constexpr float kNodeW = 200.0f;
constexpr float kTitleH = 24.0f;
constexpr float kPrevH = 106.0f;   // 16:9 in the 188 px inner width
constexpr float kScopeH = 32.0f;
float row_h() { return ui::active_theme().row_height_compact; }
constexpr float kPadB = 8.0f;
constexpr float kGridMinor = 24.0f;
constexpr float kGridMajor = 120.0f;
constexpr size_t kMaxNodes = 512;

struct CanvasUser {
    const Graph* graph;
    CanvasState* state;
    Output* out;
};

ui::SliderOpts row_slider_opts(const ParamRow& row) {
    ui::SliderOpts opts;
    opts.format = row.format;
    opts.display_scale = row.display_scale;
    opts.hard_max = row.hard_max;
    opts.out_changed = row.changed;
    opts.out_released = row.released;
    return opts;
}

bool node_has_preview(const Node& nd) {
    return nd.kind != NodeKind::ModSource &&
           nd.kind != NodeKind::GroupIn && nd.kind != NodeKind::GroupOut;
}

bool is_boundary(const Node& nd) {
    return nd.kind == NodeKind::GroupIn || nd.kind == NodeKind::GroupOut;
}

// Port strip rows sit between the preview and the param rows.
// Group input slots are dots, not rows, so they do not count here.
int port_row_count(const Node& nd) {
    return nd.exit_rows +
           (nd.kind == NodeKind::GroupIn && nd.ghost_in ? 1 : 0) +
           (nd.has_matte_port ? 1 : 0) + (nd.has_aux_port ? 1 : 0);
}

// Input dot stack pitch, graph units.
constexpr float kInPitch = 14.0f;

void hit_canvas(ui::LayoutNode& node, ui::LayoutFrame& frame) {
    auto* u = static_cast<CanvasUser*>(node.user);
    CanvasState& st = *u->state;
    ui::register_rect_hit(node, frame, u->state);
    if (st.slider_state.editing) {
        Rect box = st.value_edit_rect.intersect(node.rect);
        if (!node.clip.empty()) box = box.intersect(node.clip);
        frame.ctx.add_hit(box, frame.ctx.acquire_widget_id(&st.slider_state.edit_state));
    }
    st.menu_up = st.ctx_open || st.port_menu_open || st.dd_open;
    if (st.menu_up)
        frame.ctx.push_overlay(node.rect,
                               frame.ctx.acquire_widget_id(&st.menu_up),
                               false);
    if (st.dd_state.open)
        frame.ctx.push_overlay(st.dd_rect,
                               frame.ctx.acquire_widget_id(&st.dd_state),
                               false);
    if (st.ctx_dd.open)
        frame.ctx.push_overlay(st.ctx_rect,
                               frame.ctx.acquire_widget_id(&st.ctx_dd),
                               false);
    // The open swatch takes the popup layer so clicks miss the cards below.
    if (st.swatch_open)
        frame.ctx.push_overlay(
            ui::swatch_popup_rect(st.swatch_anchor, *st.swatch_open, frame),
            frame.ctx.acquire_widget_id(st.swatch_open), false);
}

// Draw and hit tests must share this geometry or wires miss the cursor.
void wire_controls(Vec2 p0, Vec2 p3, Vec2* p1, Vec2* p2) {
    const float reach =
        std::clamp(std::fabs(p3.x - p0.x) * 0.5f, 24.0f, 140.0f);
    *p1 = {p0.x + reach, p0.y};
    *p2 = {p3.x - reach, p3.y};
}

Vec2 wire_point(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, float t) {
    const float u = 1.0f - t;
    return {u * u * u * p0.x + 3 * u * u * t * p1.x + 3 * u * t * t * p2.x +
                t * t * t * p3.x,
            u * u * u * p0.y + 3 * u * u * t * p1.y + 3 * u * t * t * p2.y +
                t * t * t * p3.y};
}

// Returns the squared distance. The hit test samples 24 segments.
float wire_dist2(Vec2 p0, Vec2 p3, Vec2 probe) {
    Vec2 p1, p2;
    wire_controls(p0, p3, &p1, &p2);
    float best = 1e9f;
    for (int i = 0; i <= 24; ++i) {
        const Vec2 p =
            wire_point(p0, p1, p2, p3, static_cast<float>(i) / 24.0f);
        const float dx = probe.x - p.x, dy = probe.y - p.y;
        best = std::min(best, dx * dx + dy * dy);
    }
    return best;
}

// Draw one polyline: per segment strokes make beads at the joints.
void draw_wire(ui::Canvas2D& canvas, Vec2 p0, Vec2 p3, float thickness,
               Color color, bool dashed) {
    Vec2 p1, p2;
    wire_controls(p0, p3, &p1, &p2);
    const float core = std::max(thickness, 2.0f);
    constexpr int kSeg = 36;
    Vec2 pts[kSeg + 1];
    pts[0] = p0;
    for (int i = 1; i <= kSeg; ++i)
        pts[i] = wire_point(p0, p1, p2, p3, static_cast<float>(i) / kSeg);
    if (!dashed) {
        canvas.draw_polyline(pts, kSeg + 1, core, color);
        return;
    }
    for (int i = 0; i < kSeg; i += 2)
        canvas.draw_polyline(&pts[i], 2, core, color);
}

void draw_loop_glyph(ui::Canvas2D& canvas, Vec2 center, float r, Color color,
                     Color gap_fill) {
    canvas.draw_sdf_rect_outline({center.x - r, center.y - r, r * 2, r * 2},
                                 r, 1.3f, color);
    canvas.draw_rect(
        {center.x + r * 0.15f, center.y - r - 1.0f, r * 0.9f, r * 0.9f},
        gap_fill);
}

constexpr uint64_t kEmptyPress = ~0ull;

void draw_canvas(ui::LayoutNode& node, ui::LayoutFrame& frame) {
    auto* u = static_cast<CanvasUser*>(node.user);
    const Graph& g = *u->graph;
    CanvasState& st = *u->state;
    Output& out = *u->out;
    const ui::Theme& theme = frame.theme;
    const Rect& r = node.rect;
    ui::Canvas2D& canvas = frame.canvas;
    const size_t n = std::min(g.node_count, kMaxNodes);

    if (!st.view_inited) {
        st.view_inited = true;
        if (n) {
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            for (size_t i = 0; i < n; ++i) {
                x0 = std::min(x0, g.nodes[i].x);
                y0 = std::min(y0, g.nodes[i].y);
                x1 = std::max(x1, g.nodes[i].x + kNodeW);
                y1 = std::max(y1, g.nodes[i].y +
                                      node_height_of(g.nodes[i]));
            }
            const float bw = std::max(x1 - x0, 1.0f);
            const float bh = std::max(y1 - y0, 1.0f);
            st.zoom = std::clamp(
                std::min((r.w - 60.0f) / bw, (r.h - 40.0f) / bh), 0.35f,
                1.0f);
            st.pan_x = (r.w - bw * st.zoom) * 0.5f - x0 * st.zoom;
            st.pan_y = (r.h - bh * st.zoom) * 0.5f - y0 * st.zoom;
        }
    }
    auto to_screen = [&](Vec2 gp) {
        return Vec2{r.x + st.pan_x + gp.x * st.zoom,
                    r.y + st.pan_y + gp.y * st.zoom};
    };
    auto to_graph = [&](Vec2 sp) {
        return Vec2{(sp.x - r.x - st.pan_x) / st.zoom,
                    (sp.y - r.y - st.pan_y) / st.zoom};
    };
    // Not const: the wheel handler updates z in the middle of the frame.
    float z = st.zoom;

    auto node_rect_s = [&](const Node& nd) {   // screen space card rect
        const Vec2 tl = to_screen({nd.x, nd.y});
        return Rect{tl.x, tl.y, kNodeW * z, node_height_of(nd) * z};
    };
    // Graph units from the card top.
    auto strip_top = [](const Node& nd) {
        return kTitleH + (node_has_preview(nd) ? kPrevH + 6.0f
                          : nd.scope && nd.scope_count > 1
                              ? kScopeH + 6.0f
                              : 2.0f);
    };
    auto rows_top_g = [&](const Node& nd) {
        return strip_top(nd) +
               static_cast<float>(port_row_count(nd)) * row_h();
    };
    // Stack index 0 is the In dot, k is slot k, the last one is the ghost.
    auto in_stack_count = [](const Node& nd) {
        return (nd.has_in ? 1 + nd.slot_rows : 0) +
               (nd.ghost_in ? 1 : 0);
    };
    // ghost_in means a ghost exit on a GroupIn card, not an input dot.
    auto in_stacked = [](const Node& nd) {
        return (nd.ghost_in && nd.kind == NodeKind::Group) ||
               nd.slot_rows > 0;
    };
    auto exit_count = [](const Node& nd) {
        return nd.exit_rows +
               (nd.kind == NodeKind::GroupIn && nd.ghost_in ? 1 : 0);
    };
    auto port_stack = [&](const Node& nd, int i) {
        const float cy = node_has_preview(nd) ? kTitleH + kPrevH * 0.5f
                                              : kTitleH * 0.5f;
        const float off =
            (static_cast<float>(i) -
             static_cast<float>(in_stack_count(nd) - 1) * 0.5f) *
            kInPitch;
        return to_screen({nd.x, nd.y + cy + off});
    };
    auto port_in = [&](const Node& nd) {
        if (in_stacked(nd) && nd.has_in) return port_stack(nd, 0);
        const float py = node_has_preview(nd) ? kTitleH + kPrevH * 0.5f
                                              : kTitleH * 0.5f;
        return to_screen({nd.x, nd.y + py});
    };
    auto port_out = [&](const Node& nd) {
        const float py = node_has_preview(nd) ? kTitleH + kPrevH * 0.5f
                                              : kTitleH * 0.5f;
        return to_screen({nd.x + kNodeW, nd.y + py});
    };
    auto port_slot = [&](const Node& nd, int slot_index) {
        return port_stack(nd, slot_index);
    };
    auto port_ghost = [&](const Node& nd) {
        return port_stack(nd, nd.has_in ? 1 + nd.slot_rows : 0);
    };
    auto port_matte = [&](const Node& nd) {
        return to_screen(
            {nd.x, nd.y + strip_top(nd) + 0.5f * row_h()});
    };
    auto port_aux = [&](const Node& nd) {
        return to_screen(
            {nd.x, nd.y + strip_top(nd) +
                       (nd.has_matte_port ? 1.5f : 0.5f) * row_h()});
    };
    auto port_exit = [&](const Node& nd, int k) {
        return to_screen(
            {nd.x + kNodeW, nd.y + strip_top(nd) +
                                (static_cast<float>(k) + 0.5f) * row_h()});
    };
    auto row_anchor = [&](const Node& nd, int row) {
        return to_screen({nd.x, nd.y + rows_top_g(nd) +
                                    (static_cast<float>(row) + 0.5f) *
                                        row_h()});
    };
    auto find_node = [&](uint64_t id) -> const Node* {
        for (size_t i = 0; i < n; ++i)
            if (g.nodes[i].id == id) return &g.nodes[i];
        return nullptr;
    };

    const ui::WidgetId wid = frame.ctx.acquire_widget_id(&st);
    const ui::Gesture cg = frame.ctx.gesture(wid, r, 3.0f);
    const bool owns = cg.hovered;
    const bool menu_owns =
        frame.ctx.widget_owns_mouse(frame.ctx.acquire_widget_id(&st.menu_up));
    const Vec2 mouse = frame.input.mouse;
    Vec2 gmouse = to_graph(mouse);
    st.last_mouse = mouse;
    st.last_gx = gmouse.x;
    st.last_gy = gmouse.y;

    if (st.center_on) {
        for (size_t i = 0; i < n; ++i)
            if (g.nodes[i].id == st.center_on) {
                const Node& nd = g.nodes[i];
                st.pan_x = r.w * 0.5f -
                           (nd.x + kNodeW * 0.5f) * st.zoom;
                st.pan_y = r.h * 0.5f -
                           (nd.y + node_height_of(nd) * 0.5f) * st.zoom;
                break;
            }
        st.center_on = 0;
    }

    auto row_under_mouse = [&](const Node& nd) {
        const Rect cr = node_rect_s(nd);
        if (!cr.contains(mouse)) return -1;
        const float rows_top = cr.y + rows_top_g(nd) * z;
        if (mouse.y < rows_top) return -1;
        const int row = static_cast<int>((mouse.y - rows_top) / (row_h() * z));
        if (row < 0 || row >= nd.row_count) return -1;
        return nd.rows[row].route_clicked || nd.rows[row].value_input
                   ? row
                   : -1;
    };
    // The drag rings and the drop must use this same rule.
    auto out_feeds_input = [](const Node& src_nd, const Node& to_nd,
                              uint32_t port) {
        if (!src_nd.has_out && src_nd.exit_rows == 0) return false;
        if (src_nd.kind == NodeKind::ModSource) return false;
        if (port == 1 || to_nd.kind == NodeKind::Group)
            return src_nd.kind == NodeKind::Source ||
                   src_nd.kind == NodeKind::Effect ||
                   src_nd.kind == NodeKind::Group ||
                   src_nd.kind == NodeKind::GroupIn;
        if (to_nd.kind == NodeKind::GroupOut)
            return src_nd.kind == NodeKind::Effect;
        if (src_nd.kind == NodeKind::GroupIn)
            return to_nd.kind == NodeKind::Effect;
        return true;
    };

    // Draw, splice hit and click select must use these endpoints.
    auto wire_ends = [&](const Wire& w, Vec2* p0, Vec2* p3) {
        const Node* a = find_node(w.from);
        const Node* b = find_node(w.to);
        if (!a || !b) return false;
        *p0 = a->exit_rows > 0
            ? port_exit(*a,
                        std::min(static_cast<int>(w.from_port),
                                 a->exit_rows - 1))
            : port_out(*a);
        const bool row_end =
            w.data && w.to_row >= 0 && w.to_row < b->row_count;
        const bool slot_end = !w.data && w.to_port >= 2 &&
                              static_cast<int>(w.to_port) - 1 <=
                                  b->slot_rows;
        *p3 = !w.data && w.to_port == 1 ? port_matte(*b)
            : slot_end ? port_slot(*b, static_cast<int>(w.to_port) - 1)
            : !w.data && w.to_port == 2 ? port_aux(*b)
            : row_end     ? row_anchor(*b, w.to_row)
                          : port_in(*b);
        return true;
    };

    // Clear a drag if the pointer left the window with the button down.
    if (st.drag_kind && !frame.input.left_down() &&
        !frame.input.left_released() &&
        !(frame.input.buttons_down & ui::kMouseMiddle)) {
        if (st.drag_kind == 1 && cg.drag_moved) out.move_released = true;
        st.drag_kind = 0;
        st.drag_row = -1;
    }

    // The last card in array order wins: it draws on top.
    st.hover = 0;
    int hover_i = -1;
    if (owns && st.drag_kind == 0) {
        for (size_t i = 0; i < n; ++i)
            if (node_rect_s(g.nodes[i]).contains(mouse)) {
                st.hover = g.nodes[i].id;
                hover_i = static_cast<int>(i);
            }
    }

    const bool hosting = g.edit_field && (g.rename_frame != 0 ||
                                          g.rename_node != 0);
    bool efocus = false;
    if (hosting) {
        ui::TextHostOuts outs;
        outs.commit = &out.text_commit;
        outs.cancel = &out.text_cancel;
        outs.blur = &out.text_blur;
        efocus = ui::text_host_frame(frame, wid, st.edit_state,
                                     *g.edit_field, st.edit_rect, true,
                                     outs).focused;
    } else {
        st.edit_state = {};
    }
    const std::string edit_shown = hosting ? g.edit_field->buf : std::string();
    const ui::TextCaret edit_caret =
        efocus ? ui::field_caret(*g.edit_field, 0,
                                 ui::caret_blink_on(frame.ctx))
               : ui::TextCaret{};

    const float canvas_wheel = frame.ctx.take_wheel(wid);
    if (canvas_wheel != 0.0f) {
        const float nz =
            std::clamp(z * std::pow(1.15f, canvas_wheel), 0.25f, 2.5f);
        const float k = nz / z;
        st.pan_x = (mouse.x - r.x) - ((mouse.x - r.x) - st.pan_x) * k;
        st.pan_y = (mouse.y - r.y) - ((mouse.y - r.y) - st.pan_y) * k;
        st.zoom = nz;
        z = nz;
        gmouse = to_graph(mouse);
    }

    const float ts = 12.0f * z;
    const float rs = theme.font_size_compact * z;
    // zone: 0 none, 1 slider, 2 value text, 3 key toggle, 4 expose toggle.
    struct RowHit {
        int row = -1;
        int zone = 0;
    };
    // Field rect in screen space.
    auto row_field_rect = [&](const Node& nd, int row) {
        const Rect cr = node_rect_s(nd);
        const float ry = cr.y + rows_top_g(nd) * z +
                         static_cast<float>(row) * row_h() * z;
        const float fx0 = cr.x + 64.0f * z;
        return Rect{fx0, ry + 1.5f * z, (cr.right() - 8.0f * z) - fx0,
                    row_h() * z - 3.0f * z};
    };
    auto hit_row = [&](const Node& nd) -> RowHit {
        RowHit h;
        const Rect cr = node_rect_s(nd);
        const float rows_top = cr.y + rows_top_g(nd) * z;
        if (mouse.y < rows_top) return h;
        const int row = static_cast<int>((mouse.y - rows_top) / (row_h() * z));
        if (row < 0 || row >= nd.row_count) return h;
        h.row = row;
        const float lx = (mouse.x - cr.x) / z;   // graph units into the card
        if (nd.rows[row].key_clicked && lx >= 3.0f && lx < 15.0f) {
            h.zone = 3;
        } else if (nd.rows[row].expose_clicked && lx >= 15.0f && lx < 27.0f) {
            h.zone = 4;
        } else if (lx >= 64.0f && lx <= kNodeW - 8.0f) {
            h.zone = 1;
            const ParamRow& pr = nd.rows[row];
            if (pr.kind == 0 && pr.staged && pr.format) {
                const float ds =
                    pr.display_scale != 0.0f ? pr.display_scale : 1.0f;
                char val[32];
                std::snprintf(val, sizeof(val), pr.format, *pr.staged * ds);
                const float bw = ui::value_box_width(frame.font, val, rs, z);
                if (mouse.x >=
                    ui::slider_value_rect(row_field_rect(nd, row), bw).x)
                    h.zone = 2;
            }
        }
        return h;
    };
    auto slider_input = [&](const Node& nd, int row, bool pressed,
                            bool released) {
        const ParamRow& pr = nd.rows[row];
        const Rect fr = row_field_rect(nd, row);
        char val[32] = {};
        if (pr.staged && pr.format) {
            const float ds =
                pr.display_scale != 0.0f ? pr.display_scale : 1.0f;
            std::snprintf(val, sizeof(val), pr.format, *pr.staged * ds);
        }
        const float bw =
            pr.format ? ui::value_box_width(frame.font, val, rs, z) : 0.0f;
        const Rect track = ui::slider_track_rect(fr, bw, z);
        ui::Gesture gesture;
        gesture.pressed = pressed;
        gesture.drag_released = released;
        ui::slider_input_frame(
            frame.input, gesture, track,
            {fr.x + 7.0f * z, fr.y + fr.h * 0.5f},
            pr.format && std::strstr(pr.format, "deg"),
            *pr.staged, pr.min_v, pr.max_v, st.slider_state, row_slider_opts(pr));
    };

    if (st.slider_state.editing) {
        const Node* nd = find_node(st.value_edit_node);
        if (nd && st.value_edit_row >= 0 && st.value_edit_row < nd->row_count &&
            nd->rows[st.value_edit_row].kind == 0 &&
            nd->rows[st.value_edit_row].staged) {
            const ParamRow& pr = nd->rows[st.value_edit_row];
            ui::slider_edit_frame(frame, st.slider_state, st.value_edit_rect,
                                    *pr.staged, pr.min_v, pr.max_v,
                                    row_slider_opts(pr));
        } else {
            const ui::WidgetId edit_id =
                frame.ctx.acquire_widget_id(&st.slider_state.edit_state);
            if (frame.ctx.has_focus(edit_id)) frame.ctx.clear_focus();
            st.slider_state.editing = false;
        }
    }

    if (owns && (frame.input.buttons_pressed & ui::kMouseMiddle) &&
        st.drag_kind == 0) {
        st.drag_kind = 2;
        frame.ctx.begin_drag(wid);
    }
    if (st.drag_kind == 2 &&
        (frame.input.buttons_down &
         (ui::kMouseMiddle | ui::kMouseLeft))) {
        st.pan_x += frame.input.mouse_delta.x;
        st.pan_y += frame.input.mouse_delta.y;
    }

    auto near2 = [&](Vec2 p, float r2) {
        const float dx = mouse.x - p.x, dy = mouse.y - p.y;
        return dx * dx + dy * dy < r2;
    };

    auto wire_near = [&](Vec2 p0, Vec2 p3) {
        return wire_dist2(p0, p3, mouse);
    };
    auto nearest_wire = [&](float radius, auto skip) {
        float best = radius * radius;
        int hit = -1;
        for (size_t w = 0; w < g.wire_count; ++w) {
            if (skip(g.wires[w])) continue;
            Vec2 p0, p3;
            if (!wire_ends(g.wires[w], &p0, &p3)) continue;
            const float d = wire_near(p0, p3);
            if (d < best) {
                best = d;
                hit = static_cast<int>(w);
            }
        }
        return hit;
    };
    struct PortHit {
        const Node* nd = nullptr;
        uint32_t port = 0;
    };
    auto pick_out_port = [&](float radius, auto accept) {
        std::vector<PortHit> cands;
        ui::Picker pick;
        auto consider = [&](const Node& nd, Vec2 p, uint32_t pt) {
            pick.add_point(mouse, p, radius, 0,
                           static_cast<int>(cands.size()));
            cands.push_back({&nd, pt});
        };
        for (size_t i = 0; i < n; ++i) {
            const Node& nd = g.nodes[i];
            if (!accept(nd)) continue;
            if (nd.has_out) consider(nd, port_out(nd), 0);
            for (int k = 0; k < exit_count(nd); ++k)
                consider(nd, port_exit(nd, k), static_cast<uint32_t>(k));
        }
        return pick.hit() ? cands[static_cast<size_t>(pick.best())]
                          : PortHit{};
    };
    auto pick_in_port = [&](float radius, bool aux_first, auto accept) {
        std::vector<PortHit> cands;
        ui::Picker pick;
        auto consider = [&](const Node& nd, Vec2 p, uint32_t pt) {
            pick.add_point(mouse, p, radius, 0,
                           static_cast<int>(cands.size()));
            cands.push_back({&nd, pt});
        };
        for (size_t i = 0; i < n; ++i) {
            const Node& nd = g.nodes[i];
            if (!accept(nd)) continue;
            if (nd.has_in) consider(nd, port_in(nd), 0);
            if (aux_first && nd.has_aux_port) consider(nd, port_aux(nd), 2);
            if (nd.has_matte_port) consider(nd, port_matte(nd), 1);
            if (!aux_first && nd.has_aux_port) consider(nd, port_aux(nd), 2);
            for (int k = 1; k <= nd.slot_rows; ++k)
                consider(nd, port_slot(nd, k), static_cast<uint32_t>(k + 1));
            if (nd.ghost_in && nd.kind == NodeKind::Group)
                consider(nd, port_ghost(nd),
                         static_cast<uint32_t>(nd.slot_rows + 2));
        }
        return pick.hit() ? cands[static_cast<size_t>(pick.best())]
                          : PortHit{};
    };
    auto open_add_menu = [&]() {
        if (!g.add_count) return;
        st.add_open = true;
        st.add_anchor = mouse;
        st.add_gx = gmouse.x;
        st.add_gy = gmouse.y;
        st.add_cat = -1;
        st.splice_from = st.splice_to = 0;
        st.splice_port = 0;
        const int hit = nearest_wire(10.0f, [](const Wire& wr) {
            return wr.data || wr.to_port == 1;
        });
        if (hit >= 0) {
            st.splice_from = g.wires[hit].from;
            st.splice_to = g.wires[hit].to;
            st.splice_port = g.wires[hit].to_port;
        }
        out.add_menu_opened = true;
    };

    if ((owns || menu_owns) && st.drag_kind == 0 &&
        (frame.input.buttons_pressed & ui::kMouseRight)) {
        st.add_open = false;
        st.ctx_open = false;
        uint64_t target = 0;
        if (st.hover && hover_i >= 0 &&
            !is_boundary(g.nodes[static_cast<size_t>(hover_i)])) {
            target = st.hover;
        } else if (!st.hover) {
            for (size_t f = 0; f < g.frame_count; ++f) {
                const FrameBox& fb = g.frames[f];
                const Vec2 tl = to_screen({fb.x, fb.y});
                if (Rect{tl.x, tl.y, fb.w * z, 20.0f * z}.contains(mouse))
                    target = node_id(NodeKind::Frame, fb.id);
            }
        }
        if (target) {
            st.ctx_open = true;
            st.ctx_anchor = mouse;
            st.ctx_target = target;
            st.ctx_dd.open = true;
            st.ctx_selected = -1;
            frame.ctx.set_popup_owner(&st.ctx_dd);
            if (target == kOutNodeId ||
                node_kind_of(target) != NodeKind::Frame)
                out.clicked = target;
        } else {
            open_add_menu();
        }
    }

    if (st.ctx_open) {
        if (frame.ctx.popup_owner() != &st.ctx_dd) st.ctx_dd.open = false;
        if (frame.input.left_pressed() && !st.ctx_rect.contains(mouse))
            st.ctx_dd.open = false;
        if (!st.ctx_dd.open) st.ctx_open = false;
    }

    auto port_feed_count = [&]() -> size_t {
        size_t feeds = 0;
        for (size_t w = 0; w < g.wire_count; ++w)
            if (!g.wires[w].data && g.wires[w].to == st.port_menu_node &&
                g.wires[w].to_port == st.port_menu_port)
                ++feeds;
        return feeds;
    };
    auto port_menu_rect = [&](size_t feeds) -> Rect {
        return {st.port_menu_anchor.x, st.port_menu_anchor.y, 200.0f,
                static_cast<float>(feeds) * 20.0f + 8.0f};
    };
    if (st.port_menu_open) {
        const size_t feeds = port_feed_count();
        if (feeds < 2) st.port_menu_open = false;
        if (st.port_menu_open && frame.input.left_pressed() && menu_owns) {
            const Rect mr = port_menu_rect(feeds);
            if (mr.contains(mouse)) {
                const int row =
                    static_cast<int>((mouse.y - (mr.y + 4.0f)) / 20.0f);
                if (row >= 0 && row < static_cast<int>(feeds)) {
                    const int index =
                        static_cast<int>(feeds) - 1 - row;
                    if (mouse.x >= mr.right() - 20.0f) {
                        out.port_reorder = true;
                        out.reorder_node = st.port_menu_node;
                        out.reorder_port = st.port_menu_port;
                        out.reorder_index = index;
                        out.reorder_delta = -1;
                    } else if (mouse.x >= mr.right() - 40.0f) {
                        out.port_reorder = true;
                        out.reorder_node = st.port_menu_node;
                        out.reorder_port = st.port_menu_port;
                        out.reorder_index = index;
                        out.reorder_delta = 1;
                    }
                }
            } else {
                st.port_menu_open = false;
            }
        }
    }

    // Row pointers live one frame: resolve the open row again each frame.
    const Node* dd_node_p = nullptr;
    const ParamRow* dd_row_p = nullptr;
    if (st.dd_open) {
        for (size_t i = 0; i < g.node_count; ++i)
            if (g.nodes[i].id == st.dd_node)
                dd_node_p = &g.nodes[i];
        if (dd_node_p && st.dd_row >= 0 &&
            st.dd_row < dd_node_p->row_count &&
            dd_node_p->rows[st.dd_row].kind == 1 &&
            dd_node_p->rows[st.dd_row].staged) {
            dd_row_p = &dd_node_p->rows[st.dd_row];
        } else {
            st.dd_open = false;
            dd_node_p = nullptr;
        }
    }
    if (dd_row_p) {
        if (frame.ctx.popup_owner() != &st.dd_state) st.dd_state.open = false;
        if (frame.input.left_pressed() && !st.dd_rect.contains(mouse))
            st.dd_state.open = false;
        if (!st.dd_state.open) {
            st.dd_open = false;
            dd_row_p = nullptr;
        }
    }

    const ParamRow* sw_row = nullptr;
    if (st.swatch_open) {
        for (size_t i = 0; i < g.node_count && !sw_row; ++i) {
            const Node& snd = g.nodes[i];
            for (int r2 = 0; r2 < snd.row_count; ++r2)
                if (snd.rows[r2].kind == 3 &&
                    snd.rows[r2].swatch == st.swatch_open &&
                    snd.rows[r2].staged) {
                    sw_row = &snd.rows[r2];
                    st.swatch_anchor = row_field_rect(snd, r2);
                    break;
                }
        }
        const bool lost = !sw_row || !st.swatch_open->open ||
                          frame.ctx.popup_owner() != st.swatch_open;
        const Rect sw_popup =
            ui::swatch_popup_rect(st.swatch_anchor, *st.swatch_open, frame);
        const bool away =
            frame.input.left_pressed() && !sw_popup.contains(mouse);
        if (lost || away) {
            ui::swatch_commit_field(*st.swatch_open,
                                    sw_row ? sw_row->staged : nullptr,
                                    sw_row ? sw_row->changed : nullptr,
                                    sw_row ? sw_row->released : nullptr);
            st.swatch_open->open = false;
            st.swatch_open = nullptr;
            sw_row = nullptr;
        }
    }

    auto popup_row = [&](const Rect& mr, float ry) {
        const bool hot = mouse.x >= mr.x && mouse.x < mr.right() &&
                         mouse.y >= ry && mouse.y < ry + 20.0f;
        ui::draw_popup_row(canvas, theme, {mr.x + 4.0f, ry, mr.w - 8.0f, 20.0f},
                           hot);
        return hot;
    };

    auto crumb_main_rect = [&]() {
        return Rect{r.x + 10.0f, r.y + 8.0f,
                    ui::measure_text(frame.font, "main", 11.0f).x + 16.0f,
                    20.0f};
    };
    const bool on_crumb = g.crumb && crumb_main_rect().contains(mouse);
    if (on_crumb && frame.input.left_pressed() && owns &&
        st.drag_kind == 0)
        out.crumb_clicked = true;

    if (frame.input.left_pressed() && owns && st.drag_kind == 0 &&
        !on_crumb) {
        // Port grabs come before card hits.
        bool port_handled = false;
        // The nearest anchor wins: exits sit inside each other's grab radius.
        if (const PortHit pr =
                pick_out_port(9.0f, [](const Node&) { return true; });
            pr.nd) {
            st.drag_kind = 4;
            st.wire_from = pr.nd->id;
            st.wire_from_port = pr.port;
            frame.ctx.begin_drag(wid);
            port_handled = true;
        }
        if (!port_handled) {
            const PortHit best =
                pick_in_port(9.0f, false, [](const Node&) { return true; });
            if (best.nd) {
            const uint32_t best_port = best.port;
            const Node& nd = *best.nd;
            auto grab_input = [&](uint32_t port) {
                size_t feeds = 0;
                for (size_t w = 0; w < g.wire_count; ++w)
                    if (!g.wires[w].data && g.wires[w].to == nd.id &&
                        g.wires[w].to_port == port)
                        ++feeds;
                const bool second = frame.ctx.press_is_double(
                    nd.id * 31ull + port + 1ull);
                if (feeds >= 2 && second) {
                    st.port_menu_open = true;
                    st.port_menu_anchor = mouse;
                    st.port_menu_node = nd.id;
                    st.port_menu_port = port;
                    port_handled = true;
                    return;
                }
                for (size_t w = 0; w < g.wire_count; ++w)
                    if (!g.wires[w].data &&
                        g.wires[w].to_port == port &&
                        g.wires[w].to == nd.id) {
                        st.drag_kind = 5;
                        st.wire_from = g.wires[w].from;
                        st.wire_from_port = g.wires[w].from_port;
                        st.wire_old_to = nd.id;
                        st.wire_old_port = port;
                        frame.ctx.begin_drag(wid);
                        port_handled = true;
                        return;
                    }
                st.drag_kind = 8;
                st.wire_from = 0;
                st.wire_from_port = 0;
                st.wire_old_to = nd.id;
                st.wire_old_port = port;
                frame.ctx.begin_drag(wid);
                port_handled = true;
            };
            grab_input(best_port);
            }
        }
        if (!port_handled && hover_i >= 0) {
            const Node& nd = g.nodes[static_cast<size_t>(hover_i)];
            const Rect cr = node_rect_s(nd);
            bool handled = false;
            if ((frame.input.mods & platform::kModShift) &&
                hit_row(nd).zone == 0) {
                out.clicked = nd.id;
                out.clicked_shift = true;
                handled = true;
            }
            if (mouse.y < cr.y + kTitleH * z) {
                const float lx = (mouse.x - cr.x) / z;
                if (nd.remove_clicked && lx > kNodeW - 20.0f) {
                    *nd.remove_clicked = true;
                    handled = true;
                } else if (nd.bypass_clicked && lx > kNodeW - 38.0f &&
                           lx <= kNodeW - 20.0f) {
                    *nd.bypass_clicked = true;
                    handled = true;
                }
            }
            if (!handled) {
                const RowHit rh = hit_row(nd);
                if (rh.row >= 0 && rh.zone == 3 &&
                    nd.rows[rh.row].key_clicked) {
                    *nd.rows[rh.row].key_clicked = true;
                    handled = true;
                } else if (rh.row >= 0 && rh.zone == 4 &&
                           nd.rows[rh.row].expose_clicked) {
                    *nd.rows[rh.row].expose_clicked = true;
                    handled = true;
                } else if (rh.row >= 0 && nd.rows[rh.row].kind != 0 &&
                           (rh.zone == 1 || rh.zone == 2)) {
                    const ParamRow& pr = nd.rows[rh.row];
                    if (pr.kind == 1 && pr.staged) {
                        st.dd_open = true;
                        st.dd_node = nd.id;
                        st.dd_row = rh.row;
                        st.dd_field = row_field_rect(nd, rh.row);
                        st.dd_state.open = true;
                        st.dd_selected = -1;
                        frame.ctx.set_popup_owner(&st.dd_state);
                    } else if (pr.kind == 2) {
                        out.text_edit = nd.id;
                    } else if (pr.kind == 3 && pr.swatch && pr.staged) {
                        st.swatch_open = pr.swatch;
                        st.swatch_anchor = row_field_rect(nd, rh.row);
                        pr.swatch->open = true;
                        ui::swatch_seed_from(*pr.swatch, pr.staged);
                        frame.ctx.set_popup_owner(pr.swatch);
                    } else if (pr.kind == 4 && pr.changed) {
                        *pr.changed = true;
                    }
                    handled = true;
                } else if (rh.row >= 0 && rh.zone == 1 &&
                           nd.rows[rh.row].staged) {
                    st.drag_kind = 3;
                    st.drag_id = nd.id;
                    st.drag_row = rh.row;
                    slider_input(nd, rh.row, true, false);
                    frame.ctx.begin_drag(wid);
                    handled = true;
                } else if (rh.row >= 0 && rh.zone == 2 &&
                           nd.rows[rh.row].staged) {
                    st.value_edit_node = nd.id;
                    st.value_edit_row = rh.row;
                    const ParamRow& pr = nd.rows[rh.row];
                    ui::begin_slider_edit(frame, st.slider_state, *pr.staged,
                                            row_slider_opts(pr));
                    handled = true;
                }
            }
            if (!handled) {
                const bool second = frame.ctx.press_is_double(nd.id);
                if (nd.kind == NodeKind::Group && second) {
                    if (mouse.y < cr.y + kTitleH * z)
                        out.group_rename = nd.id;
                    else
                        out.group_open = nd.id;
                } else if (nd.is_look && second &&
                           mouse.y >= cr.y + kTitleH * z) {
                    out.look_open = nd.id;
                } else if (nd.text_edit && second &&
                           mouse.y < cr.y + kTitleH * z) {
                    out.text_edit = nd.id;
                } else {
                    out.clicked = nd.id;
                    // Only the title bar drags a card: a body press must not
                    // move it.
                    if (mouse.y < cr.y + kTitleH * z) {
                        st.drag_kind = 1;
                        st.drag_id = nd.id;
                        st.drag_alt = false;
                        st.node_grab_x = gmouse.x - nd.x;
                        st.node_grab_y = gmouse.y - nd.y;
                        frame.ctx.begin_drag(wid);
                    }
                }
            }
        } else if (!port_handled &&
                   (frame.input.mods & platform::kModShift)) {
            st.drag_kind = 6;
            st.node_grab_x = gmouse.x;
            st.node_grab_y = gmouse.y;
            frame.ctx.begin_drag(wid);
        } else if (!port_handled) {
            bool frame_handled = false;
            for (size_t f = 0; f < g.frame_count && !frame_handled; ++f) {
                const FrameBox& fb = g.frames[f];
                const Vec2 tl = to_screen({fb.x, fb.y});
                const Rect box{tl.x, tl.y, fb.w * z, fb.h * z};
                const Rect corner{box.right() - 14.0f * z,
                                  box.bottom() - 14.0f * z, 14.0f * z,
                                  14.0f * z};
                if (corner.contains(mouse)) {
                    st.drag_kind = 7;
                    st.drag_id = node_id(NodeKind::Frame, fb.id);
                    frame.ctx.begin_drag(wid);
                    frame_handled = true;
                    break;
                }
                const Rect title{tl.x, tl.y, fb.w * z, 20.0f * z};
                if (!title.contains(mouse)) continue;
                if (fb.remove_clicked &&
                    mouse.x > title.right() - 20.0f * z) {
                    *fb.remove_clicked = true;
                } else if (fb.color_clicked &&
                           mouse.x > title.right() - 38.0f * z &&
                           mouse.x <= title.right() - 20.0f * z) {
                    *fb.color_clicked = true;
                } else {
                    const uint64_t fid = node_id(NodeKind::Frame, fb.id);
                    if (frame.ctx.press_is_double(fid)) {
                        out.frame_rename = fb.id;
                    } else {
                        st.drag_kind = 1;
                        st.drag_id = fid;
                        st.drag_alt =
                            (frame.input.mods & platform::kModAlt) != 0;
                        st.node_grab_x = gmouse.x - fb.x;
                        st.node_grab_y = gmouse.y - fb.y;
                        frame.ctx.begin_drag(wid);
                    }
                }
                frame_handled = true;
            }
            if (!frame_handled) {
                st.drag_kind = 2;
                st.drag_id = kEmptyPress;
                frame.ctx.begin_drag(wid);
            }
        }
    }

    if (st.drag_kind != 0 && frame.input.left_down()) {
        if (st.drag_kind == 1) {
            out.moved = st.drag_id;
            out.moved_x = gmouse.x - st.node_grab_x;
            out.moved_y = gmouse.y - st.node_grab_y;
            out.moved_alt = st.drag_alt;
            st.drag_splice_from = st.drag_splice_to = 0;
            st.drag_splice_port = 0;
            const Node* dn = find_node(st.drag_id);
            if (dn &&
                (dn->kind == NodeKind::Effect ||
                 dn->kind == NodeKind::Group) &&
                dn->has_in && g.multi_count <= 1) {
                bool fed = false;
                for (size_t w = 0; w < g.wire_count; ++w)
                    fed = fed || (!g.wires[w].data &&
                                  g.wires[w].to_port != 1 &&
                                  g.wires[w].to == dn->id);
                if (!fed) {
                    const int hit = nearest_wire(12.0f, [&](const Wire& wr) {
                        return wr.data || wr.to_port == 1 ||
                               wr.from == dn->id || wr.to == dn->id;
                    });
                    if (hit >= 0) {
                        st.drag_splice_from = g.wires[hit].from;
                        st.drag_splice_to = g.wires[hit].to;
                        st.drag_splice_port = g.wires[hit].to_port;
                    }
                }
            }
        } else if (st.drag_kind == 3) {
            const Node* nd = find_node(st.drag_id);
            if (nd && st.drag_row >= 0 && st.drag_row < nd->row_count &&
                nd->rows[st.drag_row].staged) {
                slider_input(*nd, st.drag_row, false, false);
            }
        } else if (st.drag_kind == 7) {
            for (size_t f = 0; f < g.frame_count; ++f) {
                const FrameBox& fb = g.frames[f];
                if (node_id(NodeKind::Frame, fb.id) != st.drag_id) continue;
                out.frame_resized = fb.id;
                out.frame_w = std::max(gmouse.x - fb.x, 120.0f);
                out.frame_h = std::max(gmouse.y - fb.y, 60.0f);
                break;
            }
        }
    }

    if (frame.input.left_released() && st.drag_kind != 0) {
        if (st.drag_kind == 8) {
            // The nearest anchor wins: exits sit inside each other's radius.
            const Node* to_nd = find_node(st.wire_old_to);
            if (to_nd) {
                const PortHit pr = pick_out_port(18.0f, [&](const Node& nd) {
                    return nd.id != st.wire_old_to &&
                           out_feeds_input(nd, *to_nd, st.wire_old_port);
                });
                if (pr.nd) {
                    out.connect_requested = true;
                    out.connect_from = pr.nd->id;
                    out.connect_from_port = pr.port;
                    out.connect_to = st.wire_old_to;
                    out.connect_port = st.wire_old_port;
                }
            }
            st.wire_old_to = 0;
            st.wire_old_port = 0;
        } else if (st.drag_kind == 4 || st.drag_kind == 5) {
            const Node* from_nd = find_node(st.wire_from);
            const bool from_mod =
                from_nd && from_nd->kind == NodeKind::ModSource;
            uint64_t to = 0;
            uint32_t port = 0;
            bool found = false;
            if (from_mod) {
                for (size_t i = 0; i < n; ++i) {
                    const Node& nd = g.nodes[i];
                    if (nd.id == st.wire_from) continue;
                    const int row = row_under_mouse(nd);
                    if (row < 0) continue;
                    out.route_drop_requested = true;
                    out.route_drop_from = st.wire_from;
                    out.route_drop_to = nd.id;
                    out.route_drop_row = row;
                    break;
                }
            }
            // The nearest anchor wins, never list order: rows sit 18 px apart.
            if (!from_mod) {
                const PortHit pr =
                    pick_in_port(18.0f, true, [&](const Node& nd) {
                        return nd.id != st.wire_from;
                    });
                if (pr.nd) {
                    to = pr.nd->id;
                    port = pr.port;
                    found = true;
                }
            }
            if (found) {
                const bool same = st.drag_kind == 5 &&
                                  to == st.wire_old_to &&
                                  port == st.wire_old_port;
                if (!same) {
                    if (st.drag_kind == 5) {
                        out.disconnect_requested = true;
                        out.disconnect_from = st.wire_from;
                        out.disconnect_from_port = st.wire_from_port;
                        out.disconnect_to = st.wire_old_to;
                        out.disconnect_port = st.wire_old_port;
                    }
                    out.connect_requested = true;
                    out.connect_from = st.wire_from;
                    out.connect_from_port = st.wire_from_port;
                    out.connect_to = to;
                    out.connect_port = port;
                }
            } else if (st.drag_kind == 5) {
                out.disconnect_requested = true;
                out.disconnect_from = st.wire_from;
                out.disconnect_from_port = st.wire_from_port;
                out.disconnect_to = st.wire_old_to;
                out.disconnect_port = st.wire_old_port;
            }
            st.wire_from = 0;
            st.wire_from_port = 0;
        } else if (st.drag_kind == 6) {
            out.marquee_done = true;
            out.mq_x0 = std::min(st.node_grab_x, gmouse.x);
            out.mq_y0 = std::min(st.node_grab_y, gmouse.y);
            out.mq_x1 = std::max(st.node_grab_x, gmouse.x);
            out.mq_y1 = std::max(st.node_grab_y, gmouse.y);
            const Vec2 ra = to_screen({out.mq_x0, out.mq_y0});
            const Vec2 rb = to_screen({out.mq_x1, out.mq_y1});
            for (size_t w = 0;
                 w < g.wire_count && out.mq_wire_count < 64; ++w) {
                Vec2 p0, p3;
                if (!wire_ends(g.wires[w], &p0, &p3)) continue;
                Vec2 p1, p2;
                wire_controls(p0, p3, &p1, &p2);
                for (int s = 0; s <= 24; ++s) {
                    const Vec2 sp = wire_point(
                        p0, p1, p2, p3, static_cast<float>(s) / 24.0f);
                    const float x = sp.x, y = sp.y;
                    if (x >= ra.x && x <= rb.x && y >= ra.y &&
                        y <= rb.y) {
                        out.mq_wires[out.mq_wire_count++] = g.wires[w];
                        break;
                    }
                }
            }
        } else if (st.drag_kind == 3) {
            const Node* nd = find_node(st.drag_id);
            if (nd && st.drag_row >= 0 && st.drag_row < nd->row_count &&
                nd->rows[st.drag_row].staged)
                slider_input(*nd, st.drag_row, false, true);
        } else if (st.drag_kind == 1 && cg.drag_moved) {
            out.move_released = true;
            if (st.drag_splice_to) {
                out.node_splice_requested = true;
                out.splice_node = st.drag_id;
                out.splice_wire_from = st.drag_splice_from;
                out.splice_wire_to = st.drag_splice_to;
                out.splice_wire_port = st.drag_splice_port;
            }
            st.drag_splice_from = st.drag_splice_to = 0;
            st.drag_splice_port = 0;
        } else if (st.drag_kind == 7) {
            out.frame_resize_released = true;
        } else if (st.drag_kind == 2 && !cg.drag_moved &&
                   st.drag_id == kEmptyPress && owns) {
            if (frame.ctx.press_is_double(kEmptyPress)) {
                open_add_menu();
            } else {
                const int hit =
                    nearest_wire(9.0f, [](const Wire&) { return false; });
                if (hit >= 0) {
                    out.wire_clicked = true;
                    out.wire_clicked_shift =
                        (frame.input.mods & platform::kModShift) != 0;
                    out.wire_from = g.wires[hit].from;
                    out.wire_from_port = g.wires[hit].from_port;
                    out.wire_to = g.wires[hit].to;
                    out.wire_to_port = g.wires[hit].to_port;
                    out.wire_data = g.wires[hit].data;
                    out.wire_to_row = g.wires[hit].to_row;
                    // 1 marks a wire click, so the next press is not a double.
                    frame.ctx.press_is_double(1);
                } else {
                    out.clicked_empty = true;
                }
            }
        }
        st.drag_kind = 0;
        st.drag_id = 0;
        st.drag_row = -1;
    }

    if (st.drag_kind != 0 &&
        !(frame.input.buttons_down &
          (ui::kMouseMiddle | ui::kMouseLeft))) {
        st.drag_kind = 0;
        st.drag_id = 0;
        st.drag_row = -1;
    }

    canvas.draw_rect(r, theme.window_bg);
    canvas.push_clip(r);

    if (g.hint && g.node_count == 0) {
        const float tw =
            ui::measure_text(frame.font, g.hint, theme.font_size_small).x;
        ui::draw_text(canvas, frame.font, g.hint,
                      {r.x + (r.w - tw) * 0.5f, r.y + r.h * 0.5f},
                      theme.font_size_small, theme.text_disabled);
    }

    {
        const Color minor = theme.hairline.with_alpha(0.35f);
        const Color major = theme.hairline.with_alpha(0.8f);
        const float step = kGridMinor * z;
        if (step >= 6.0f) {
            const float gx0 = std::fmod(st.pan_x, step);
            for (float x = gx0; x < r.w; x += step)
                canvas.draw_rect({r.x + x, r.y, 1.0f, r.h}, minor);
            const float gy0 = std::fmod(st.pan_y, step);
            for (float y = gy0; y < r.h; y += step)
                canvas.draw_rect({r.x, r.y + y, r.w, 1.0f}, minor);
        }
        const float mstep = kGridMajor * z;
        const float mx0 = std::fmod(st.pan_x, mstep);
        for (float x = mx0; x < r.w; x += mstep)
            canvas.draw_rect({r.x + x, r.y, 1.0f, r.h}, major);
        const float my0 = std::fmod(st.pan_y, mstep);
        for (float y = my0; y < r.h; y += mstep)
            canvas.draw_rect({r.x, r.y + y, r.w, 1.0f}, major);
    }

    for (size_t f = 0; f < g.frame_count; ++f) {
        const FrameBox& fb = g.frames[f];
        float fx = fb.x, fy = fb.y;
        if (st.drag_kind == 1 &&
            st.drag_id == node_id(NodeKind::Frame, fb.id)) {
            fx = gmouse.x - st.node_grab_x;
            fy = gmouse.y - st.node_grab_y;
        }
        float fw = fb.w, fh = fb.h;
        if (st.drag_kind == 7 &&
            st.drag_id == node_id(NodeKind::Frame, fb.id) &&
            out.frame_resized == fb.id) {
            fw = out.frame_w;
            fh = out.frame_h;
        }
        const Vec2 tl = to_screen({fx, fy});
        const Rect box{tl.x, tl.y, fw * z, fh * z};
        const bool tinted = fb.color >= 1 && fb.color <= 8;
        const Color fcol = tinted ? ui::category_palette()[(fb.color - 1) & 7]
                                  : theme.hairline;
        canvas.draw_sdf_rect(box, 6.0f * z,
                             tinted ? fcol.with_alpha(0.10f)
                                    : theme.control_bg.with_alpha(0.35f));
        canvas.draw_sdf_rect_outline(box, 6.0f * z, 1.0f,
                                     tinted ? fcol.with_alpha(0.55f)
                                            : theme.hairline);
        canvas.draw_rect({box.x, box.y, box.w, 20.0f * z},
                         theme.control_bg.with_alpha(0.5f));
        {
            const float dr = 3.5f * z;
            const Vec2 dc{box.right() - 29.0f * z, box.y + 10.0f * z};
            if (tinted)
                canvas.draw_sdf_rect({dc.x - dr, dc.y - dr, dr * 2,
                                      dr * 2},
                                     dr, fcol);
            else
                canvas.draw_sdf_rect_outline(
                    {dc.x - dr, dc.y - dr, dr * 2, dr * 2}, dr, 1.2f,
                    theme.text_disabled);
        }
        const bool renaming = g.rename_frame == fb.id;
        const Rect title_r{box.x, box.y, box.w - 40.0f * z, 20.0f * z};
        if (renaming) {
            st.edit_rect = title_r;
            ui::draw_text_input_face(canvas, frame.font, theme, title_r,
                                     edit_shown, edit_caret, efocus, true,
                                     false, 0.0f, 11.0f * z, 4.0f * z, z);
        } else {
            canvas.push_clip(title_r);
            ui::draw_text(canvas, frame.font, fb.title,
                          {box.x + 8.0f * z, box.y + 4.0f * z}, 11.0f * z,
                          theme.text_dim);
            canvas.pop_clip();
        }
        for (int t = 0; t < 2; ++t) {
            const float o = (5.0f + t * 4.0f) * z;
            canvas.draw_line({box.right() - o, box.bottom() - 2.0f * z},
                             {box.right() - 2.0f * z, box.bottom() - o},
                             1.2f, theme.text_disabled);
        }
        const float s = 3.0f * z;
        const Vec2 c{box.right() - 10.0f * z, box.y + 10.0f * z};
        canvas.draw_line({c.x - s, c.y - s}, {c.x + s, c.y + s}, 1.2f,
                         theme.text_disabled);
        canvas.draw_line({c.x - s, c.y + s}, {c.x + s, c.y - s}, 1.2f,
                         theme.text_disabled);
    }

    for (size_t w = 0; w < g.wire_count; ++w) {
        const Node* a = find_node(g.wires[w].from);
        const Node* b = find_node(g.wires[w].to);
        if (!a || !b) continue;
        Vec2 p0, p3;
        if (!wire_ends(g.wires[w], &p0, &p3)) continue;
        const bool row_end = g.wires[w].data &&
                             g.wires[w].to_row >= 0 &&
                             g.wires[w].to_row < b->row_count;
        const bool hot = st.hover == a->id || st.hover == b->id ||
                         g.selected == a->id || g.selected == b->id;
        Color col = theme.text_disabled.with_alpha(0.7f);
        bool dashed = false;
        if (g.wires[w].data) {
            col = hot ? theme.text_dim
                      : theme.text_disabled.with_alpha(0.4f);
            dashed = true;
        } else if (hot) {
            col = theme.text_dim;
        }
        for (size_t sw = 0; sw < g.sel_wire_count; ++sw)
            if (g.sel_wires[sw].from == g.wires[w].from &&
                g.sel_wires[sw].from_port == g.wires[w].from_port &&
                g.sel_wires[sw].to == g.wires[w].to &&
                g.sel_wires[sw].data == g.wires[w].data &&
                g.sel_wires[sw].to_port == g.wires[w].to_port &&
                (g.sel_wires[sw].to_row < 0 ||
                 g.sel_wires[sw].to_row == g.wires[w].to_row))
                col = theme.accent;
        // Match the port too: image and audio wires share endpoints.
        if (st.drag_kind == 1 && st.drag_splice_to && !g.wires[w].data &&
            g.wires[w].from == st.drag_splice_from &&
            g.wires[w].to == st.drag_splice_to &&
            g.wires[w].to_port == st.drag_splice_port)
            col = theme.accent;
        draw_wire(canvas, p0, p3, std::max(2.0f, 2.0f * z), col, dashed);
        if (row_end) {
            const float dr = 2.6f * z;
            canvas.draw_sdf_rect({p3.x - dr, p3.y - dr, dr * 2, dr * 2},
                                 dr, col);
        }
    }

    // Array order is z order. The dragged card draws last.
    ui::probe_add("canvas", r);
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < n; ++i) {
            const Node& nd = g.nodes[i];
            const bool dragging_this =
                st.drag_kind == 1 && st.drag_id == nd.id;
            if ((pass == 1) != dragging_this) continue;
            Rect cr = node_rect_s(nd);
            if (dragging_this && out.moved == nd.id) {
                const Vec2 tl = to_screen({out.moved_x, out.moved_y});
                cr.x = tl.x;
                cr.y = tl.y;
            }
            if (cr.right() < r.x || cr.x > r.right() || cr.bottom() < r.y ||
                cr.y > r.bottom())
                continue;
            // Scripts address these probe names. Keep them stable.
            {
                const std::string pname =
                    nd.id == kOutNodeId || nd.kind == NodeKind::GroupOut
                        ? std::string("node:output")
                    : nd.kind == NodeKind::GroupIn
                        ? std::string("node:input")
                        : "node:" + std::to_string(node_doc_id(nd.id));
                ui::probe_add(pname, cr);
                const float pr = 6.0f * z;
                const Vec2 po = port_out(nd);
                ui::probe_add(pname + "/out",
                              {po.x - pr, po.y - pr, pr * 2, pr * 2});
                const Vec2 pi = port_in(nd);
                ui::probe_add(pname + "/in",
                              {pi.x - pr, pi.y - pr, pr * 2, pr * 2});
            }
            const bool selected = g.selected && nd.id == g.selected;
            const bool hovered = st.hover == nd.id;
            bool in_multi = false;
            for (size_t m = 0; m < g.multi_count && !in_multi; ++m)
                in_multi = g.multi[m] == nd.id;

            // Draw the outline last so the title bar does not cover it.
            canvas.draw_sdf_rect(cr, 5.0f * z,
                                 nd.bypassed ? theme.control_bg_active
                                             : theme.panel_bg);
            const Rect tb{cr.x + 1.0f, cr.y + 1.0f, cr.w - 2.0f,
                          kTitleH * z - 1.0f};
            canvas.draw_sdf_rect(tb, 4.0f * z,
                                 theme.control_bg.with_alpha(
                                     nd.bypassed ? 0.4f : 1.0f));
            if (nd.tint != 255)
                canvas.draw_rect(
                    {tb.x + 1.0f, tb.y + 3.0f * z, 3.0f * z,
                     tb.h - 6.0f * z},
                    ui::category_palette()[nd.tint & 7].with_alpha(
                        nd.bypassed ? 0.35f : 0.9f));
            canvas.draw_sdf_rect_outline(
                cr, 5.0f * z, selected || in_multi ? 2.0f : 1.0f,
                selected ? theme.accent
                         : (in_multi ? theme.accent_dim
                                     : (hovered ? theme.text_disabled
                                                : theme.hairline)));
            bool has_text_row = false;
            for (int tr = 0; tr < nd.row_count; ++tr)
                if (nd.rows[tr].kind == 2) has_text_row = true;
            const bool card_renaming =
                g.rename_node == nd.id && !has_text_row;
            const Rect title_r{tb.x, tb.y, tb.w - 40.0f * z, tb.h};
            if (card_renaming) {
                st.edit_rect = title_r;
                ui::draw_text_input_face(canvas, frame.font, theme, title_r,
                                         edit_shown, edit_caret, efocus, true,
                                         false, 0.0f, ts, 4.0f * z, z);
            } else {
                canvas.push_clip(title_r);
                ui::draw_text(canvas, frame.font, nd.title,
                              {tb.x + 8.0f * z, tb.y + (tb.h - ts) * 0.4f},
                              ts,
                              nd.bypassed ? theme.text_disabled
                                          : theme.text);
                canvas.pop_clip();
            }
            float tcx = tb.right() - 12.0f * z;
            if (nd.remove_clicked) {
                const float s = 3.2f * z;
                const Vec2 c{tcx, tb.y + tb.h * 0.5f};
                const Color xc =
                    hovered ? theme.text_dim : theme.text_disabled;
                canvas.draw_line({c.x - s, c.y - s}, {c.x + s, c.y + s},
                                 1.3f, xc);
                canvas.draw_line({c.x - s, c.y + s}, {c.x + s, c.y - s},
                                 1.3f, xc);
                tcx -= 18.0f * z;
            }
            if (nd.bypass_clicked) {
                const float pr = 3.5f * z;
                const Vec2 c{tcx, tb.y + tb.h * 0.5f};
                if (nd.bypassed)
                    canvas.draw_sdf_rect_outline(
                        {c.x - pr, c.y - pr, pr * 2, pr * 2}, pr, 1.2f,
                        theme.text_disabled);
                else
                    canvas.draw_sdf_rect({c.x - pr, c.y - pr, pr * 2,
                                          pr * 2},
                                         pr, theme.accent);
                tcx -= 18.0f * z;
            }
            if (nd.solo) {
                ui::draw_text(canvas, frame.font, "S",
                              {tcx - 4.0f * z, tb.y + (tb.h - rs) * 0.4f},
                              rs, theme.accent);
                tcx -= 14.0f * z;
            }
            if (nd.feedback)
                draw_loop_glyph(canvas,
                                {tcx - 4.0f * z, tb.y + tb.h * 0.5f},
                                4.0f * z, theme.text_dim,
                                theme.control_bg);

            float cy = cr.y + kTitleH * z;
            if (node_has_preview(nd)) {
                const Rect pv{cr.x + 1.0f, cy, cr.w - 2.0f, kPrevH * z};
                if (nd.wave_card) {
                    // Draw the input trace first: the output stays in front.
                    canvas.draw_rect(pv, theme.control_bg_active);
                    const float mid = pv.y + pv.h * 0.5f;
                    const float half = pv.h * 0.5f - 2.0f * z;
                    canvas.draw_rect({pv.x, mid - 0.5f, pv.w, 1.0f},
                                     theme.hairline);
                    auto trace = [&](const float* w, Color col) {
                        if (!w) return;
                        const float cw =
                            pv.w / static_cast<float>(nd.wave_count);
                        for (int c = 0; c < nd.wave_count; ++c) {
                            const float lo = w[c * 2];
                            const float hi = w[c * 2 + 1];
                            float y0 = mid - hi * half;
                            float y1 = mid - lo * half;
                            if (y1 - y0 < 1.0f) {
                                const float cc = (y0 + y1) * 0.5f;
                                y0 = cc - 0.5f;
                                y1 = cc + 0.5f;
                            }
                            canvas.draw_rect(
                                {pv.x + cw * static_cast<float>(c), y0,
                                 cw, y1 - y0},
                                col);
                        }
                    };
                    if (nd.wave_count > 0) {
                        trace(nd.wave_in,
                              theme.text_dim.with_alpha(0.55f));
                        trace(nd.wave_out,
                              theme.accent.with_alpha(0.85f));
                    }
                    canvas.draw_rect_outline(pv, 1.0f, theme.hairline);
                } else if (nd.preview) {
                    canvas.draw_image_quad(pv, nd.preview, nd.pu0, nd.pv0,
                                           nd.pu1, nd.pv1,
                                           Color::rgba(1, 1, 1, 1), 0.0f);
                } else {
                    canvas.draw_rect(pv, theme.control_bg_active);
                    canvas.draw_rect_outline(pv, 1.0f, theme.hairline);
                }
                cy += (kPrevH + 6.0f) * z;
            } else if (nd.scope && nd.scope_count > 1) {
                const Rect sv{cr.x + 6.0f * z, cy + 3.0f * z,
                              cr.w - 12.0f * z, kScopeH * z - 6.0f * z};
                canvas.draw_sdf_rect(sv, 3.0f * z, theme.control_bg);
                const float span =
                    std::max(nd.scope_hi - nd.scope_lo, 1e-6f);
                auto sy_of = [&](float v) {
                    const float t = std::clamp(
                        (v - nd.scope_lo) / span, 0.0f, 1.0f);
                    return sv.y + 2.0f * z +
                           (1.0f - t) * (sv.h - 4.0f * z);
                };
                if (nd.scope_lo < 0.0f && nd.scope_hi > 0.0f)
                    canvas.draw_rect({sv.x, sy_of(0.0f), sv.w, 1.0f},
                                     theme.hairline);
                std::vector<Vec2> spts(
                    static_cast<size_t>(nd.scope_count));
                for (int si = 0; si < nd.scope_count; ++si)
                    spts[static_cast<size_t>(si)] = {
                        sv.x + sv.w * static_cast<float>(si) /
                                   static_cast<float>(nd.scope_count - 1),
                        sy_of(nd.scope[si])};
                const float sw = std::max(1.0f, 1.2f * z);
                if (nd.scope_min && nd.scope_max)
                    for (int si = 0; si < nd.scope_count; ++si) {
                        const float y0 = sy_of(nd.scope_max[si]);
                        const float y1 = sy_of(nd.scope_min[si]);
                        if (y1 - y0 <= sw) continue;
                        canvas.draw_line({spts[static_cast<size_t>(si)].x, y0},
                                         {spts[static_cast<size_t>(si)].x, y1},
                                         sw, theme.accent_dim);
                    }
                canvas.draw_polyline(spts.data(), nd.scope_count, sw,
                                     theme.accent_dim);
                const float dy = sy_of(nd.scope[0]);
                canvas.draw_sdf_rect({sv.x - 1.0f, dy - 2.0f * z,
                                      4.0f * z, 4.0f * z},
                                     2.0f * z, theme.accent);
                cy += (kScopeH + 6.0f) * z;
            } else {
                cy += 2.0f * z;
            }
            cy += static_cast<float>(port_row_count(nd)) * row_h() * z;

            const RowHit rh = hovered ? hit_row(nd) : RowHit{};
            for (int row = 0; row < nd.row_count; ++row) {
                const ParamRow& pr = nd.rows[row];
                const float ry = cy + row * row_h() * z;
                const bool row_hot = rh.row == row;
                Color lab = theme.text_dim;
                if (pr.keyed) lab = theme.accent;
                else if (pr.modulated) lab = theme.accent_dim;
                float label_x = cr.x + 8.0f * z;
                if (pr.key_clicked) {
                    const float kr2 = 3.0f * z;
                    const Vec2 kc{cr.x + 9.0f * z,
                                  ry + row_h() * z * 0.5f};
                    if (pr.keyed)
                        canvas.draw_sdf_rect({kc.x - kr2, kc.y - kr2,
                                              kr2 * 2, kr2 * 2},
                                             kr2, theme.accent);
                    else
                        canvas.draw_sdf_rect_outline(
                            {kc.x - kr2, kc.y - kr2, kr2 * 2, kr2 * 2},
                            kr2, 1.2f,
                            row_hot && rh.zone == 3
                                ? theme.text_dim
                                : theme.text_disabled);
                    label_x = cr.x + 17.0f * z;
                }
                if (pr.expose_clicked) {
                    const float er = 2.8f * z;
                    const Vec2 ec{cr.x + 21.0f * z,
                                  ry + row_h() * z * 0.5f};
                    if (pr.exposed)
                        canvas.draw_rect({ec.x - er, ec.y - er, er * 2,
                                          er * 2},
                                         theme.accent_dim);
                    else
                        canvas.draw_rect_outline(
                            {ec.x - er, ec.y - er, er * 2, er * 2}, 1.2f,
                            row_hot && rh.zone == 4
                                ? theme.text_dim
                                : theme.text_disabled);
                    label_x = cr.x + 29.0f * z;
                }
                canvas.push_clip({cr.x, ry, 62.0f * z, row_h() * z});
                ui::draw_text(canvas, frame.font, pr.label,
                              {label_x, ry + (row_h() * z - rs) * 0.4f},
                              rs, lab);
                canvas.pop_clip();
                if (pr.kind != 0) {
                    // Keep this rect equal to row_field_rect: hits use that.
                    const float fx0 = cr.x + 64.0f * z;
                    const Rect fr{fx0, ry + 1.5f * z,
                                  (cr.right() - 8.0f * z) - fx0,
                                  row_h() * z - 3.0f * z};
                    const bool field_hot =
                        row_hot && (rh.zone == 1 || rh.zone == 2);
                    if (pr.kind == 5) {
                        canvas.push_clip({fx0, ry,
                                          (cr.right() - 8.0f * z) - fx0,
                                          row_h() * z});
                        ui::draw_text(canvas, frame.font,
                                      pr.text ? pr.text : "",
                                      {fx0, ry + (row_h() * z - rs) * 0.4f},
                                      rs, theme.text_dim);
                        canvas.pop_clip();
                        continue;
                    }
                    if (pr.kind == 4) {
                        const char* blabel = pr.text ? pr.text : pr.label;
                        ui::ButtonFace bf;
                        bf.hover_t = field_hot ? 1.0f : 0.0f;
                        bf.font_size = rs;
                        bf.radius = 2.0f * z;
                        bf.scale = z;
                        ui::draw_button_face(canvas, frame.font, theme, fr,
                                             blabel, bf);
                        ui::probe_add(blabel, fr);
                        continue;
                    }
                    if (pr.kind == 3 && pr.staged) {
                        ui::draw_swatch_face(
                            canvas, fr, 2.0f * z,
                            pr.swatch ? pr.swatch->mode
                                      : ui::SwatchMode::Rgba,
                            pr.staged,
                            pr.swatch ? pr.swatch->hue_light : 0.5f);
                        canvas.draw_sdf_rect_outline(
                            fr, 2.0f * z, 1.0f,
                            pr.swatch && st.swatch_open == pr.swatch
                                ? theme.accent
                                : (field_hot ? theme.text_dim
                                             : theme.hairline));
                        continue;
                    }
                    if (pr.kind == 1) {
                        const bool dd_here = st.dd_open &&
                                             st.dd_node == nd.id &&
                                             st.dd_row == row;
                        const int idx =
                            pr.staged && pr.option_count > 0
                                ? std::clamp(static_cast<int>(*pr.staged -
                                                              pr.min_v +
                                                              0.5f),
                                             0, pr.option_count - 1)
                                : 0;
                        const char* shown =
                            pr.option_count > 0 ? pr.option_items[idx] : "";
                        ui::draw_dropdown_face(canvas, frame.font, theme, fr,
                                               shown, dd_here,
                                               field_hot ? 1.0f : 0.0f, rs,
                                               2.0f * z, z);
                        continue;
                    }
                    const bool editing_text =
                        pr.kind == 2 && g.rename_node == nd.id;
                    if (editing_text) st.edit_rect = fr;
                    ui::draw_text_input_face(
                        canvas, frame.font, theme, fr,
                        editing_text ? edit_shown
                                     : std::string(pr.text ? pr.text : ""),
                        editing_text ? edit_caret : ui::TextCaret{},
                        editing_text && efocus, false, false,
                        field_hot ? 1.0f : 0.0f, rs, 2.0f * z, z);
                    continue;
                }
                const Rect fr = row_field_rect(nd, row);
                const bool dial_row =
                    pr.format && std::strstr(pr.format, "deg");
                const float row_ds =
                    pr.display_scale != 0.0f ? pr.display_scale : 1.0f;
                const bool row_editing = st.slider_state.editing &&
                                         st.value_edit_node == nd.id &&
                                         st.value_edit_row == row;
                char val[32] = {};
                if (pr.staged && pr.format)
                    std::snprintf(val, sizeof(val), pr.format,
                                  *pr.staged * row_ds);
                const std::string_view box_text =
                    row_editing ? std::string_view(st.slider_state.edit.buf)
                                : std::string_view(val);
                const float box_w =
                    pr.format || row_editing
                        ? ui::value_box_width(frame.font, box_text, rs, z)
                        : 0.0f;
                const Rect box = ui::slider_value_rect(fr, box_w);
                const Rect track = ui::slider_track_rect(fr, box_w, z);
                if (dial_row) {
                    ui::draw_dial_face(
                        canvas, theme, {fr.x + 7.0f * z, fr.y + fr.h * 0.5f},
                        5.5f * z, pr.staged ? *pr.staged * row_ds : 0.0f,
                        row_hot, pr.live * row_ds, pr.has_live, z);
                } else {
                    const float span = std::max(pr.max_v - pr.min_v, 1e-6f);
                    const float t = pr.staged
                        ? std::clamp((*pr.staged - pr.min_v) / span, 0.0f,
                                     1.0f)
                        : 0.0f;
                    const float lt = pr.has_live
                        ? std::clamp((pr.live - pr.min_v) / span, 0.0f, 1.0f)
                        : -1.0f;
                    const bool dragging_row = st.drag_kind == 3 &&
                                              st.drag_id == nd.id &&
                                              st.drag_row == row;
                    ui::draw_slider_track(canvas, theme, track, t, lt,
                                          dragging_row, z);
                }
                if (box_w > 0.0f) {
                    if (row_editing) st.value_edit_rect = box;
                    const bool focused = row_editing && frame.ctx.has_focus(
                        frame.ctx.acquire_widget_id(&st.slider_state.edit_state));
                    ui::draw_text_input_face(
                        canvas, frame.font, theme, box, box_text,
                        focused ? ui::field_caret(st.slider_state.edit, 0,
                                                   ui::caret_blink_on(frame.ctx))
                                : ui::TextCaret{},
                        focused, false, false, 0.0f, rs,
                        2.0f * z, z);
                }
            }

            const float pr2 = 4.0f * z;
            bool in_fed = false, matte_fed = false, aux_fed = false,
                 out_fed = false;
            for (size_t w = 0; w < g.wire_count; ++w) {
                const Wire& wr = g.wires[w];
                if (wr.from == nd.id) out_fed = true;
                if (wr.to != nd.id) continue;
                if (wr.data) continue;
                if (wr.to_port == 0) in_fed = true;
                else if (wr.to_port == 1) matte_fed = true;
                else if (wr.to_port == 2) aux_fed = true;
            }
            auto draw_port = [&](Vec2 c, Color col, bool fed) {
                const Color pc = near2(c, 100.0f) ? theme.accent : col;
                if (fed)
                    canvas.draw_sdf_rect({c.x - pr2, c.y - pr2, pr2 * 2,
                                          pr2 * 2},
                                         pr2, pc);
                else
                    canvas.draw_sdf_rect_outline({c.x - pr2, c.y - pr2,
                                                  pr2 * 2, pr2 * 2},
                                                 pr2, 1.3f, pc);
            };
            if (nd.has_in) draw_port(port_in(nd), theme.text_disabled,
                                     in_fed);
            if (nd.has_out) draw_port(port_out(nd), theme.text_disabled,
                                      out_fed);
            for (int k = 1; k <= nd.slot_rows; ++k) {
                bool fed = false;
                for (size_t w = 0; w < g.wire_count; ++w)
                    if (!g.wires[w].data && g.wires[w].to == nd.id &&
                        g.wires[w].to_port ==
                            static_cast<uint32_t>(k + 1))
                        fed = true;
                draw_port(port_slot(nd, k), theme.text_disabled, fed);
            }
            if (nd.ghost_in && nd.kind == NodeKind::Group) {
                const Vec2 p = port_ghost(nd);
                const Color gc =
                    (near2(p, 100.0f) ? theme.accent : theme.text_disabled)
                        .with_alpha(0.55f);
                canvas.draw_sdf_rect_outline(
                    {p.x - pr2, p.y - pr2, pr2 * 2, pr2 * 2}, pr2, 1.3f,
                    gc);
            }
            for (int k = 0; k < nd.exit_rows; ++k) {
                bool fed = false;
                for (size_t w = 0; w < g.wire_count; ++w)
                    if (!g.wires[w].data && g.wires[w].from == nd.id &&
                        g.wires[w].from_port == static_cast<uint32_t>(k))
                        fed = true;
                const Vec2 p = port_exit(nd, k);
                draw_port(p, theme.text_disabled, fed);
                char exit_buf[16];
                std::snprintf(exit_buf, sizeof(exit_buf), "in %d", k + 1);
                ui::draw_text(canvas, frame.font, exit_buf,
                              {cr.x + 8.0f * z, p.y - rs * 0.5f},
                              rs * 0.9f, theme.text_disabled);
            }
            if (nd.ghost_in && nd.kind == NodeKind::GroupIn) {
                const Vec2 p = port_exit(nd, nd.exit_rows);
                const Color gc =
                    (near2(p, 100.0f) ? theme.accent : theme.text_disabled)
                        .with_alpha(0.55f);
                canvas.draw_sdf_rect_outline(
                    {p.x - pr2, p.y - pr2, pr2 * 2, pr2 * 2}, pr2, 1.3f,
                    gc);
                ui::draw_text(canvas, frame.font, "new input",
                              {cr.x + 8.0f * z, p.y - rs * 0.5f},
                              rs * 0.9f,
                              theme.text_disabled.with_alpha(0.55f));
            }
            if (nd.has_matte_port) {
                draw_port(port_matte(nd), theme.accent_dim, matte_fed);
                ui::draw_text(canvas, frame.font,
                              nd.matte_label ? nd.matte_label : "matte",
                              {cr.x + 8.0f * z,
                               port_matte(nd).y - rs * 0.5f},
                              rs * 0.9f, theme.text_disabled);
            }
            if (nd.has_aux_port) {
                draw_port(port_aux(nd), theme.text_dim, aux_fed);
                ui::draw_text(canvas, frame.font,
                              nd.aux_label ? nd.aux_label : "b",
                              {cr.x + 8.0f * z,
                               port_aux(nd).y - rs * 0.5f},
                              rs * 0.9f, theme.text_disabled);
            }
        }
    }

    if (g.crumb) {
        const Rect mr2 = crumb_main_rect();
        const bool hot = mr2.contains(mouse);
        canvas.draw_sdf_rect(mr2, 4.0f,
                             hot ? theme.control_bg_hover
                                 : theme.control_bg);
        canvas.draw_sdf_rect_outline(mr2, 4.0f, 1.0f, theme.hairline);
        ui::draw_text(canvas, frame.font, "main",
                      {mr2.x + 8.0f, mr2.y + 4.5f}, 11.0f,
                      hot ? theme.text : theme.text_dim);
        char crumb_buf[80];
        std::snprintf(crumb_buf, sizeof(crumb_buf), "> %s", g.crumb);
        ui::draw_text(canvas, frame.font, crumb_buf,
                      {mr2.right() + 8.0f, mr2.y + 4.5f}, 11.0f,
                      theme.text);
    }

    if (st.drag_kind == 6) {
        const Vec2 a = to_screen({st.node_grab_x, st.node_grab_y});
        const Rect mq{std::min(a.x, mouse.x), std::min(a.y, mouse.y),
                      std::fabs(mouse.x - a.x), std::fabs(mouse.y - a.y)};
        canvas.draw_rect(mq, theme.accent.with_alpha(0.08f));
        canvas.draw_rect_outline(mq, 1.0f, theme.accent_dim);
    }

    if (st.drag_kind == 4 || st.drag_kind == 5) {
        const Node* from_nd = find_node(st.wire_from);
        if (from_nd) {
            const bool from_mod = from_nd->kind == NodeKind::ModSource;
            const Vec2 origin = exit_count(*from_nd) > 0
                ? port_exit(*from_nd,
                            std::min(static_cast<int>(st.wire_from_port),
                                     exit_count(*from_nd) - 1))
                : port_out(*from_nd);
            draw_wire(canvas, origin, mouse,
                      std::max(1.4f, 1.8f * z), theme.accent, from_mod);
            const float pr3 = 6.0f * z;
            for (size_t i = 0; i < n && !from_mod; ++i) {
                const Node& nd = g.nodes[i];
                if (nd.id == st.wire_from) continue;
                Vec2 candidates[12]{};
                int n_cand = 0;
                if (nd.has_in) candidates[n_cand++] = port_in(nd);
                if (nd.has_aux_port) candidates[n_cand++] = port_aux(nd);
                if (nd.has_matte_port)
                    candidates[n_cand++] = port_matte(nd);
                for (int k = 1; k <= nd.slot_rows && n_cand < 11; ++k)
                    candidates[n_cand++] = port_slot(nd, k);
                if (nd.ghost_in && nd.kind == NodeKind::Group &&
                    n_cand < 12)
                    candidates[n_cand++] = port_ghost(nd);
                for (int c = 0; c < n_cand; ++c) {
                    const Vec2 p = candidates[c];
                    canvas.draw_sdf_rect_outline(
                        {p.x - pr3, p.y - pr3, pr3 * 2, pr3 * 2}, pr3,
                        1.5f,
                        near2(p, 324.0f) ? theme.accent
                                         : theme.accent_dim);
                }
            }
            if (from_mod) {
                for (size_t i = 0; i < n; ++i) {
                    const Node& nd = g.nodes[i];
                    if (nd.id == st.wire_from) continue;
                    const int hot_row = row_under_mouse(nd);
                    for (int row = 0; row < nd.row_count; ++row) {
                        if (!nd.rows[row].route_clicked &&
                            !nd.rows[row].value_input)
                            continue;
                        const Vec2 p = row_anchor(nd, row);
                        const float rr2 = 3.5f * z;
                        canvas.draw_sdf_rect_outline(
                            {p.x - rr2, p.y - rr2, rr2 * 2, rr2 * 2}, rr2,
                            1.3f,
                            row == hot_row ? theme.accent
                                           : theme.accent_dim.with_alpha(
                                                 0.5f));
                    }
                    if (hot_row >= 0) {
                        const Rect cr = node_rect_s(nd);
                        const Vec2 p = row_anchor(nd, hot_row);
                        canvas.draw_rect({cr.x, p.y - row_h() * z * 0.5f,
                                          cr.w, row_h() * z},
                                         theme.accent.with_alpha(0.10f));
                    }
                }
            }
        }
    }

    if (st.drag_kind == 8) {
        const Node* to_nd = find_node(st.wire_old_to);
        if (to_nd) {
            const bool slot_fixed =
                st.wire_old_port >= 2 &&
                static_cast<int>(st.wire_old_port) - 1 <=
                    to_nd->slot_rows + (to_nd->ghost_in ? 1 : 0);
            const Vec2 fixed_p = st.wire_old_port == 1
                ? port_matte(*to_nd)
                : slot_fixed
                    ? (static_cast<int>(st.wire_old_port) - 1 <=
                               to_nd->slot_rows
                           ? port_slot(*to_nd,
                                       static_cast<int>(
                                           st.wire_old_port) - 1)
                           : port_ghost(*to_nd))
                    : (st.wire_old_port == 2 ? port_aux(*to_nd)
                                             : port_in(*to_nd));
            draw_wire(canvas, mouse, fixed_p, std::max(1.4f, 1.8f * z),
                      theme.accent, st.wire_old_port == 1);
            const float pr3 = 6.0f * z;
            for (size_t i = 0; i < n; ++i) {
                const Node& nd = g.nodes[i];
                if (nd.id == st.wire_old_to) continue;
                if (!out_feeds_input(nd, *to_nd, st.wire_old_port))
                    continue;
                auto ring = [&](Vec2 p) {
                    canvas.draw_sdf_rect_outline(
                        {p.x - pr3, p.y - pr3, pr3 * 2, pr3 * 2}, pr3,
                        1.5f,
                        near2(p, 324.0f) ? theme.accent
                                         : theme.accent_dim);
                };
                if (nd.has_out) ring(port_out(nd));
                for (int k = 0; k < exit_count(nd); ++k)
                    ring(port_exit(nd, k));
            }
        }
    }

    if (st.add_open && st.splice_to) {
        Wire sw{st.splice_from, st.splice_to, st.splice_port};
        Vec2 p0, p3;
        if (wire_ends(sw, &p0, &p3))
            draw_wire(canvas, p0, p3, std::max(2.2f, 2.4f * z),
                      theme.accent, false);
    }

    if (st.ctx_open && g.ctx_count && st.ctx_dd.open) {
        st.ctx_rect = ui::list_popup_rect(
            {st.ctx_anchor.x, st.ctx_anchor.y, 0.0f, 0.0f}, 170.0f,
            g.ctx_items, static_cast<int>(g.ctx_count), frame);
        ui::Context::PopupRequest req;
        req.anchor = {st.ctx_anchor.x, st.ctx_anchor.y, 0.0f, 0.0f};
        req.rect = st.ctx_rect;
        req.items = g.ctx_items;
        req.count = static_cast<int>(g.ctx_count);
        req.selected = -1;
        req.state = &st.ctx_dd;
        req.out_selected = &st.ctx_selected;
        frame.ctx.set_popup(req);
    }

    if (st.port_menu_open) {
        const size_t feeds = port_feed_count();
        const Rect mr = port_menu_rect(feeds);
        ui::draw_popup_chrome(canvas, theme, mr);
        size_t seen = 0;
        for (size_t w = 0; w < g.wire_count; ++w) {
            const Wire& wr = g.wires[w];
            if (wr.data || wr.to != st.port_menu_node ||
                wr.to_port != st.port_menu_port)
                continue;
            // The feed index counts from the bottom, so mirror the row.
            const size_t row = feeds - 1 - seen;
            const float ry = mr.y + 4.0f + static_cast<float>(row) * 20.0f;
            const char* title = "?";
            char exit_title[16];
            for (size_t i = 0; i < g.node_count; ++i)
                if (g.nodes[i].id == wr.from) {
                    if (g.nodes[i].exit_rows > 0) {
                        std::snprintf(exit_title, sizeof(exit_title),
                                      "in %u", wr.from_port + 1);
                        title = exit_title;
                    } else {
                        title = g.nodes[i].title;
                    }
                }
            ui::probe_add("stack:" + std::string(title),
                          {mr.x, ry, mr.w, 20.0f});
            const bool hot = popup_row(mr, ry);
            ui::draw_text(canvas, frame.font, title,
                          {mr.x + 10.0f, ry + 4.0f}, 11.0f,
                          hot ? theme.text : theme.text_dim);
            const bool can_up = seen + 1 < feeds;
            const bool can_dn = seen > 0;
            ui::draw_icon_glyph(canvas, frame.font, ui::Icon::Up,
                                {mr.right() - 30.0f, ry + 10.0f},
                                can_up ? theme.text_dim
                                       : theme.text_disabled,
                                11.0f);
            ui::draw_icon_glyph(canvas, frame.font, ui::Icon::Down,
                                {mr.right() - 10.0f, ry + 10.0f},
                                can_dn ? theme.text_dim
                                       : theme.text_disabled,
                                11.0f);
            ++seen;
        }
    }

    if (dd_row_p && st.dd_state.open) {
        const int n = dd_row_p->option_count;
        st.dd_rect = ui::list_popup_rect(st.dd_field,
                                         std::max(st.dd_field.w, 110.0f),
                                         dd_row_p->option_items, n, frame);
        ui::Context::PopupRequest req;
        req.anchor = st.dd_field;
        req.rect = st.dd_rect;
        req.items = dd_row_p->option_items;
        req.count = n;
        req.selected = std::clamp(
            static_cast<int>(*dd_row_p->staged - dd_row_p->min_v + 0.5f),
            0, n > 0 ? n - 1 : 0);
        req.state = &st.dd_state;
        req.out_selected = &st.dd_selected;
        frame.ctx.set_popup(req);
    }

    if (st.swatch_open && sw_row) {
        ui::Context::PopupRequest req;
        req.kind = ui::Context::PopupKind::Color;
        req.anchor = st.swatch_anchor;
        req.rect = ui::swatch_popup_rect(st.swatch_anchor, *st.swatch_open,
                                         frame);
        req.state = st.swatch_open;
        req.out_rgb = sw_row->staged;
        req.out_changed = sw_row->changed;
        req.out_released = sw_row->released;
        frame.ctx.set_popup(req);
    }

    canvas.pop_clip();
}

}  // namespace

float node_width() { return kNodeW; }

float node_height(int row_count, bool has_preview, int port_rows) {
    return kTitleH + (has_preview ? kPrevH + 6.0f : 2.0f) +
           static_cast<float>(port_rows + row_count) * row_h() + kPadB;
}

float node_height_of(const Node& nd) {
    float h = node_height(nd.row_count, node_has_preview(nd),
                          port_row_count(nd));
    if (!node_has_preview(nd) && nd.scope && nd.scope_count > 1)
        h += kScopeH + 6.0f - 2.0f;   // the scope strip replaces the 2 px gap
    return h;
}

bool pick_wire(const Graph& g, const CanvasState& st, const ui::Rect& canvas,
               Vec2 screen, uint64_t* from, uint64_t* to, uint32_t* port) {
    const float z = std::max(st.zoom, 1e-3f);
    auto to_screen = [&](Vec2 p) {
        return Vec2{canvas.x + st.pan_x + p.x * z,
                    canvas.y + st.pan_y + p.y * z};
    };
    auto find_node = [&](uint64_t id) -> const Node* {
        for (size_t i = 0; i < g.node_count; ++i)
            if (g.nodes[i].id == id) return &g.nodes[i];
        return nullptr;
    };
    auto strip_top = [](const Node& nd) {
        return kTitleH + (node_has_preview(nd) ? kPrevH + 6.0f
                          : nd.scope && nd.scope_count > 1 ? kScopeH + 6.0f
                                                           : 2.0f);
    };
    auto port_y = [&](const Node& nd) {
        return node_has_preview(nd) ? kTitleH + kPrevH * 0.5f
                                    : kTitleH * 0.5f;
    };
    float best = 12.0f * 12.0f;
    bool found = false;
    for (size_t w = 0; w < g.wire_count; ++w) {
        const Wire& wr = g.wires[w];
        if (wr.data || wr.to_port == 1) continue;
        const Node* a = find_node(wr.from);
        const Node* b = find_node(wr.to);
        if (!a || !b) continue;
        // Keep in sync with port_stack and port_exit in the canvas.
        auto stack_y = [](const Node& nd, int i) {
            const float cy = node_has_preview(nd)
                ? kTitleH + kPrevH * 0.5f
                : kTitleH * 0.5f;
            const int count = (nd.has_in ? 1 + nd.slot_rows : 0) +
                              (nd.ghost_in ? 1 : 0);
            return cy + (static_cast<float>(i) -
                         static_cast<float>(count - 1) * 0.5f) * kInPitch;
        };
        const Vec2 p0 =
            a->exit_rows > 0
                ? to_screen({a->x + kNodeW,
                             a->y + strip_top(*a) +
                                 (static_cast<float>(wr.from_port) +
                                  0.5f) * row_h()})
                : to_screen({a->x + kNodeW, a->y + port_y(*a)});
        const bool slot_end = wr.to_port >= 2 &&
                              static_cast<int>(wr.to_port) - 1 <=
                                  b->slot_rows;
        const bool stacked = b->ghost_in || b->slot_rows > 0;
        const Vec2 p3 =
            slot_end
                ? to_screen({b->x,
                             b->y + stack_y(*b, static_cast<int>(
                                                    wr.to_port) - 1)})
                : wr.to_port == 2
                ? to_screen({b->x, b->y + strip_top(*b) +
                                       (b->has_matte_port ? 1.5f : 0.5f) *
                                           row_h()})
                : stacked && wr.to_port == 0 && b->has_in
                ? to_screen({b->x, b->y + stack_y(*b, 0)})
                : to_screen({b->x, b->y + port_y(*b)});
        const float d2 = wire_dist2(p0, p3, screen);
        if (d2 < best) {
            best = d2;
            *from = wr.from;
            *to = wr.to;
            *port = wr.to_port;
            found = true;
        }
    }
    return found;
}

ui::LayoutNode* FlowCanvas(ui::LayoutArena& arena, const Graph* graph,
                           CanvasState* state, Output* out) {
    auto* user = arena.alloc<CanvasUser>();
    user->graph = graph;
    user->state = state;
    user->out = out;
    ui::LayoutNode* node = ui::make_node(arena, ui::NodeKind::Leaf);
    node->width = ui::SizeSpec::fill();
    node->height = ui::SizeSpec::fill();
    node->user = user;
    node->draw_fn = draw_canvas;
    node->hit_fn = hit_canvas;
    node->debug_name = "flow_canvas";
    return node;
}

}  // namespace looks::flow

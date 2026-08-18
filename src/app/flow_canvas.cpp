// Node canvas implementation. Everything draws
// in graph space through one screen transform; interaction resolves
// geometrically against the same transform. One WidgetId owns the whole
// surface; drags hold ctx capture with the element remembered in
// CanvasState.

#include "app/flow_canvas.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "doc/effects.h"   // param_option_count/_at (dropdown rows)
#include "ui/text.h"
#include "ui/theme.h"

namespace looks::flow {

namespace {

using ui::Color;
using ui::Rect;

// Card geometry, graph units at zoom 1.
constexpr float kNodeW = 200.0f;
constexpr float kTitleH = 24.0f;
constexpr float kPrevH = 106.0f;   // 16:9 inside the 188 px inner width
constexpr float kScopeH = 32.0f;   // value-node signal strip
constexpr float kRowH = 18.0f;
constexpr float kPadB = 8.0f;
constexpr float kGridMinor = 24.0f;
constexpr float kGridMajor = 120.0f;
constexpr size_t kMaxNodes = 512;

// One subdued hue per FxCategory (enum order: Time, Warp, Mosaic, Optics,
// Color, Texture, Overlay, Codec) for the title strip.
const Color* tint_palette() {
    static const Color palette[8] = {
        Color::hex(0x7E57C2), Color::hex(0x26A69A), Color::hex(0xEC7063),
        Color::hex(0x5DADE2), Color::hex(0xF5B041), Color::hex(0xA1887F),
        Color::hex(0x66BB6A), Color::hex(0xB0BEC5),
    };
    return palette;
}

struct CanvasUser {
    const Graph* graph;
    CanvasState* state;
    Output* out;
};

bool node_has_preview(const Node& nd) {
    // Every kind gets a preview slot; mod sources and the subgraph
    // boundary nodes stay compact.
    return nd.kind != NodeKind::ModSource &&
           nd.kind != NodeKind::GroupIn && nd.kind != NodeKind::GroupOut;
}

bool is_boundary(const Node& nd) {
    return nd.kind == NodeKind::GroupIn || nd.kind == NodeKind::GroupOut;
}

// Matte/aux ports get dedicated strip rows between the preview and the
// param rows — a port pinned at a fixed offset lands on top of row 0
// (the port label painted over wet/dry shipped once).
int port_row_count(const Node& nd) {
    return (nd.has_matte_port ? 1 : 0) + (nd.has_aux_port ? 1 : 0);
}

void hit_canvas(ui::LayoutNode& node, ui::LayoutFrame& frame) {
    auto* u = static_cast<CanvasUser*>(node.user);
    Rect r = node.rect;
    if (!node.clip.empty()) r = r.intersect(node.clip);
    frame.ctx.add_hit(r, frame.ctx.acquire_widget_id(u->state));
}

void dashed_line(ui::Canvas2D& canvas, Vec2 a, Vec2 b, float thickness,
                 Color color) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1.0f) return;
    const float ux = dx / len, uy = dy / len;
    for (float t = 0.0f; t < len; t += 9.0f) {
        const float e = std::min(t + 5.0f, len);
        canvas.draw_line({a.x + ux * t, a.y + uy * t},
                         {a.x + ux * e, a.y + uy * e}, thickness, color);
    }
}

// Cubic bezier with horizontal tangents — the wire idiom. Canvas2D lines
// are pass-through primitives (no analytic AA), so each segment gets a
// layered stroke: a soft halo + a mid pass + the core, which reads as a
// feathered edge at wire thickness.
void draw_wire(ui::Canvas2D& canvas, Vec2 p0, Vec2 p3, float thickness,
               Color color, bool dashed) {
    const float reach =
        std::clamp(std::fabs(p3.x - p0.x) * 0.5f, 24.0f, 140.0f);
    const Vec2 p1{p0.x + reach, p0.y};
    const Vec2 p2{p3.x - reach, p3.y};
    const float core = std::max(thickness, 2.0f);
    constexpr int kSeg = 36;
    Vec2 prev = p0;
    int dash = 0;
    for (int i = 1; i <= kSeg; ++i) {
        const float t = static_cast<float>(i) / kSeg;
        const float u = 1.0f - t;
        const Vec2 p{u * u * u * p0.x + 3 * u * u * t * p1.x +
                         3 * u * t * t * p2.x + t * t * t * p3.x,
                     u * u * u * p0.y + 3 * u * u * t * p1.y +
                         3 * u * t * t * p2.y + t * t * t * p3.y};
        const bool skip = dashed && (dash++ & 1);
        if (!skip) {
            canvas.draw_line(prev, p, core + 2.4f,
                             color.with_alpha(color.a * 0.22f));
            canvas.draw_line(prev, p, core + 1.2f,
                             color.with_alpha(color.a * 0.5f));
            canvas.draw_line(prev, p, core, color);
        }
        prev = p;
    }
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

    // ---- view transform
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
    // NOT const: the wheel handler below updates the zoom mid-frame and
    // every size derived from z must follow, or content draws one frame
    // with new positions and old metrics (a direction-specific one-frame
    // misalignment shipped exactly that way).
    float z = st.zoom;

    auto node_rect_s = [&](const Node& nd) {   // screen-space card rect
        const Vec2 tl = to_screen({nd.x, nd.y});
        return Rect{tl.x, tl.y, kNodeW * z, node_height_of(nd) * z};
    };
    // Top of the port strip / param rows, graph units from the card top.
    auto strip_top = [](const Node& nd) {
        return kTitleH + (node_has_preview(nd) ? kPrevH + 6.0f
                          : nd.scope && nd.scope_count > 1
                              ? kScopeH + 6.0f
                              : 2.0f);
    };
    auto rows_top_g = [&](const Node& nd) {
        return strip_top(nd) +
               static_cast<float>(port_row_count(nd)) * kRowH;
    };
    auto port_in = [&](const Node& nd) {
        const float py = node_has_preview(nd) ? kTitleH + kPrevH * 0.5f
                                              : kTitleH * 0.5f;
        return to_screen({nd.x, nd.y + py});
    };
    auto port_out = [&](const Node& nd) {
        const float py = node_has_preview(nd) ? kTitleH + kPrevH * 0.5f
                                              : kTitleH * 0.5f;
        return to_screen({nd.x + kNodeW, nd.y + py});
    };
    auto port_matte = [&](const Node& nd) {
        return to_screen(
            {nd.x, nd.y + strip_top(nd) + 0.5f * kRowH});
    };
    auto port_aux = [&](const Node& nd) {
        return to_screen(
            {nd.x, nd.y + strip_top(nd) +
                       (nd.has_matte_port ? 1.5f : 0.5f) * kRowH});
    };
    // Left-edge anchor of a param row — where mod wires land (routes
    // wire into the PARAM, not the card).
    auto row_anchor = [&](const Node& nd, int row) {
        return to_screen({nd.x, nd.y + rows_top_g(nd) +
                                    (static_cast<float>(row) + 0.5f) *
                                        kRowH});
    };
    auto find_node = [&](uint64_t id) -> const Node* {
        for (size_t i = 0; i < n; ++i)
            if (g.nodes[i].id == id) return &g.nodes[i];
        return nullptr;
    };

    // ---- interaction
    const ui::WidgetId wid = frame.ctx.acquire_widget_id(&st);
    const bool owns = frame.ctx.widget_owns_mouse(wid);
    const Vec2 mouse = frame.input.mouse;
    Vec2 gmouse = to_graph(mouse);
    // Published cursor: paste-at-cursor and the find anchor read these.
    st.last_mouse = mouse;
    st.last_gx = gmouse.x;
    st.last_gy = gmouse.y;

    // Find jump: center the view on the requested node once.
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

    // Routable row under the cursor on a card: route_clicked marks
    // param rows, value_input marks helper operand rows; -1 = none.
    auto row_under_mouse = [&](const Node& nd) {
        const Rect cr = node_rect_s(nd);
        if (!cr.contains(mouse)) return -1;
        const float rows_top = cr.y + rows_top_g(nd) * z;
        if (mouse.y < rows_top) return -1;
        const int row = static_cast<int>((mouse.y - rows_top) / (kRowH * z));
        if (row < 0 || row >= nd.row_count) return -1;
        return nd.rows[row].route_clicked || nd.rows[row].value_input
                   ? row
                   : -1;
    };
    // Which node outputs may feed a given input port — one truth for the
    // reverse wire drag's candidate rings and its drop resolution.
    // port 1 (matte): ANY image out — masks ARE images. Image In / aux:
    // Source, Effect, Group (boundary proxy), or GroupIn outs. Value
    // nodes never feed image ports; only image nodes feed GroupOut.
    auto out_feeds_input = [](const Node& src_nd, const Node& to_nd,
                              uint32_t port) {
        if (!src_nd.has_out) return false;
        if (src_nd.kind == NodeKind::ModSource) return false;
        if (port == 1)
            return src_nd.kind == NodeKind::Source ||
                   src_nd.kind == NodeKind::Effect ||
                   src_nd.kind == NodeKind::Group;
        if (to_nd.kind == NodeKind::GroupOut)
            return src_nd.kind == NodeKind::Effect;
        if (src_nd.kind == NodeKind::GroupIn)
            return to_nd.kind == NodeKind::Effect;
        return true;
    };

    // A wire's screen endpoints — one truth for draw, splice hit, and
    // click-select.
    auto wire_ends = [&](const Wire& w, Vec2* p0, Vec2* p3) {
        const Node* a = find_node(w.from);
        const Node* b = find_node(w.to);
        if (!a || !b) return false;
        *p0 = port_out(*a);
        const bool row_end =
            w.data && w.to_row >= 0 && w.to_row < b->row_count;
        *p3 = !w.data && w.to_port == 1 ? port_matte(*b)
            : !w.data && w.to_port == 2 ? port_aux(*b)
            : row_end     ? row_anchor(*b, w.to_row)
                          : port_in(*b);
        return true;
    };

    // Missed release: pointer left with a drag armed.
    if (st.drag_kind && !frame.input.left_down() &&
        !frame.input.left_released() &&
        !(frame.input.buttons_down & ui::kMouseMiddle)) {
        if (st.drag_kind == 1 && st.drag_moved) out.move_released = true;
        st.drag_kind = 0;
        st.drag_row = -1;
    }

    // Hover: topmost card under the cursor (last drawn wins).
    st.hover = 0;
    int hover_i = -1;
    if (owns && st.drag_kind == 0) {
        for (size_t i = 0; i < n; ++i)
            if (node_rect_s(g.nodes[i]).contains(mouse)) {
                st.hover = g.nodes[i].id;
                hover_i = static_cast<int>(i);
            }
    }

    // Add-menu geometry (screen-space popup, texed openAddMenu). Defined
    // BEFORE the zoom handler: the wheel over the open menu scrolls its
    // list — zoom used to consume the wheel first, so the popup never
    // scrolled at all.
    // Category mode (user request): while the filter is empty the menu
    // lists CATEGORY rows; the hovered one opens a flyout submenu with
    // its items. Typing collapses to the flat filtered list. Find mode
    // has no headers, so it stays flat automatically.
    int cat_rows[32];
    int cat_count = 0;
    if (g.add_headers && (!g.add_filter || g.add_filter[0] == '\0'))
        for (size_t i = 0; i < g.add_count && cat_count < 32; ++i)
            if (g.add_headers[i])
                cat_rows[cat_count++] = static_cast<int>(i);
    const bool cat_mode = cat_count > 0;
    if (!cat_mode) st.add_cat = -1;
    if (cat_mode && st.add_cat >= 0 &&
        (st.add_cat >= static_cast<int>(g.add_count) ||
         !g.add_headers[st.add_cat]))
        st.add_cat = -1;
    // The open category's item range [fly0, fly1) in the flat list.
    int fly0 = 0, fly1 = 0;
    if (cat_mode && st.add_cat >= 0) {
        fly0 = st.add_cat + 1;
        fly1 = fly0;
        while (fly1 < static_cast<int>(g.add_count) &&
               !g.add_headers[fly1])
            ++fly1;
    }
    const float kMenuW = 190.0f;
    const int menu_visible = static_cast<int>(
        std::min<size_t>(cat_mode ? static_cast<size_t>(cat_count)
                                  : g.add_count,
                         14));
    const float menu_h = 26.0f + menu_visible * 18.0f + 6.0f;
    auto menu_rect = [&]() {
        return Rect{std::min(st.add_anchor.x, r.right() - kMenuW - 8.0f),
                    std::min(st.add_anchor.y, r.bottom() - menu_h - 8.0f),
                    kMenuW, menu_h};
    };
    // Flyout beside the open category row; flips left when clipped.
    auto fly_rect = [&]() {
        const Rect mr = menu_rect();
        int vis_row = 0;
        for (int c = 0; c < cat_count; ++c)
            if (cat_rows[c] == st.add_cat) vis_row = c;
        const float h = static_cast<float>(fly1 - fly0) * 18.0f + 8.0f;
        float x = mr.right() + 2.0f;
        if (x + kMenuW > r.right() - 4.0f) x = mr.x - kMenuW - 2.0f;
        const float y = std::min(mr.y + 26.0f + vis_row * 18.0f,
                                 r.bottom() - h - 8.0f);
        return Rect{x, y, kMenuW, h};
    };
    if (owns && st.add_open && frame.input.wheel_y != 0.0f &&
        menu_rect().contains(mouse)) {
        st.add_scroll = std::clamp(
            st.add_scroll - frame.input.wheel_y * 36.0f, 0.0f,
            std::max(0.0f, static_cast<float>(g.add_count) * 18.0f -
                               menu_visible * 18.0f));
        frame.input.wheel_y = 0.0f;
    }

    // Wheel: zoom around the cursor. z and gmouse are refreshed so the
    // REST OF THIS FRAME draws and hit-tests with the new transform.
    if (owns && frame.input.wheel_y != 0.0f) {
        const float nz =
            std::clamp(z * std::pow(1.15f, frame.input.wheel_y), 0.25f,
                       2.5f);
        const float k = nz / z;
        st.pan_x = (mouse.x - r.x) - ((mouse.x - r.x) - st.pan_x) * k;
        st.pan_y = (mouse.y - r.y) - ((mouse.y - r.y) - st.pan_y) * k;
        st.zoom = nz;
        z = nz;
        gmouse = to_graph(mouse);
        frame.input.wheel_y = 0.0f;
    }

    // Row hit helper: which row + zone is under the mouse for a card.
    // zone: 0 none, 1 slider strip (incl. the value — still-click types),
    // 3 the PERSISTENT keyframe toggle at the row's left, 4 the expose
    // toggle beside it (scoped member rows — the group FACE, v5.3).
    struct RowHit {
        int row = -1;
        int zone = 0;
    };
    auto hit_row = [&](const Node& nd) -> RowHit {
        RowHit h;
        const Rect cr = node_rect_s(nd);
        const float rows_top = cr.y + rows_top_g(nd) * z;
        if (mouse.y < rows_top) return h;
        const int row = static_cast<int>((mouse.y - rows_top) / (kRowH * z));
        if (row < 0 || row >= nd.row_count) return h;
        h.row = row;
        const float lx = (mouse.x - cr.x) / z;   // graph units into card
        if (nd.rows[row].key_clicked && lx >= 3.0f && lx < 15.0f)
            h.zone = 3;
        else if (nd.rows[row].expose_clicked && lx >= 15.0f && lx < 27.0f)
            h.zone = 4;
        else if (lx >= 64.0f && lx < kNodeW - 46.0f)
            h.zone = 1;   // the slider track — press jumps + drags
        else if (lx >= kNodeW - 46.0f && lx <= kNodeW - 8.0f)
            h.zone = 2;   // the value text — click types
        return h;
    };
    // Screen rect of a non-slider row's FIELD (dropdown/text, v5.6):
    // the slider + value span, edit-box height.
    auto row_field_rect = [&](const Node& nd, int row) {
        const Rect cr = node_rect_s(nd);
        const float ry = cr.y + rows_top_g(nd) * z +
                         static_cast<float>(row) * kRowH * z;
        const float fx0 = cr.x + 64.0f * z;
        return Rect{fx0, ry + 1.5f * z, (cr.right() - 8.0f * z) - fx0,
                    kRowH * z - 3.0f * z};
    };
    auto slider_value = [&](const Node& nd, int row) {
        const Rect cr = node_rect_s(nd);
        const float t = std::clamp(
            ((mouse.x - cr.x) / z - 64.0f) / (kNodeW - 46.0f - 64.0f), 0.0f,
            1.0f);
        const ParamRow& pr = nd.rows[row];
        return pr.min_v + t * (pr.max_v - pr.min_v);
    };

    // Middle-drag pan.
    if (owns && (frame.input.buttons_pressed & ui::kMouseMiddle) &&
        st.drag_kind == 0) {
        st.drag_kind = 2;
        frame.ctx.set_capture(wid);
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

    // Distance from the cursor to a wire's bezier (sampled) — the splice
    // target for a right-click on a wire (texed contextmenu).
    auto wire_near = [&](Vec2 p0, Vec2 p3) {
        const float reach =
            std::clamp(std::fabs(p3.x - p0.x) * 0.5f, 24.0f, 140.0f);
        const Vec2 p1{p0.x + reach, p0.y};
        const Vec2 p2{p3.x - reach, p3.y};
        float best = 1e9f;
        for (int i = 0; i <= 24; ++i) {
            const float t = static_cast<float>(i) / 24.0f;
            const float u = 1.0f - t;
            const float x = u * u * u * p0.x + 3 * u * u * t * p1.x +
                            3 * u * t * t * p2.x + t * t * t * p3.x;
            const float y = u * u * u * p0.y + 3 * u * u * t * p1.y +
                            3 * u * t * t * p2.y + t * t * t * p3.y;
            const float dx = mouse.x - x, dy = mouse.y - y;
            best = std::min(best, dx * dx + dy * dy);
        }
        return best;
    };
    auto open_add_menu = [&]() {
        // Nothing to add (sequence scope): no menu - an empty popup is
        // worse than none.
        if (!g.add_count) return;
        st.add_open = true;
        st.add_anchor = mouse;
        st.add_gx = gmouse.x;
        st.add_gy = gmouse.y;
        st.add_scroll = 0.0f;
        st.add_cat = -1;
        st.splice_from = st.splice_to = 0;
        st.splice_port = 0;
        // Wire under the cursor → the added node splices into it.
        float best = 10.0f * 10.0f;
        for (size_t w = 0; w < g.wire_count; ++w) {
            if (g.wires[w].data || g.wires[w].to_port == 1) continue;
            const Node* a = find_node(g.wires[w].from);
            const Node* b = find_node(g.wires[w].to);
            if (!a || !b) continue;
            const Vec2 p3 = g.wires[w].to_port == 2 ? port_aux(*b)
                                                 : port_in(*b);
            const float d = wire_near(port_out(*a), p3);
            if (d < best) {
                best = d;
                st.splice_from = g.wires[w].from;
                st.splice_to = g.wires[w].to;
                st.splice_port = g.wires[w].to_port;
            }
        }
        out.add_menu_opened = true;
    };

    // Context-menu geometry (texed popupMenu): plain rows, no filter.
    const float kCtxW = 170.0f;
    const float ctx_h = static_cast<float>(g.ctx_count) * 20.0f + 8.0f;
    auto ctx_rect = [&]() {
        return Rect{std::min(st.ctx_anchor.x, r.right() - kCtxW - 8.0f),
                    std::min(st.ctx_anchor.y, r.bottom() - ctx_h - 8.0f),
                    kCtxW, ctx_h};
    };

    // Right-click: a card or frame title gets its CONTEXT menu (texed
    // openNodeMenu/openFrameMenu); anywhere else gets the add menu (a
    // wire under the cursor arms the splice as before).
    if (owns && st.drag_kind == 0 &&
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
            // The menu applies to the selection when the card is in it;
            // otherwise the click selects the card first (texed).
            if (target >> 56 !=
                static_cast<uint64_t>(NodeKind::Frame) + 1)
                out.clicked = target;
        } else {
            open_add_menu();
        }
    }

    // Open context menu: clicks route to it first; outside closes.
    bool menu_swallowed_press = false;
    if (st.ctx_open) {
        const Rect mr = ctx_rect();
        if (frame.input.left_pressed() && owns) {
            menu_swallowed_press = true;
            if (mr.contains(mouse)) {
                const int row =
                    static_cast<int>((mouse.y - (mr.y + 4.0f)) / 20.0f);
                if (row >= 0 && row < static_cast<int>(g.ctx_count)) {
                    out.ctx_pick = row;
                    out.ctx_node = st.ctx_target;
                }
            }
            st.ctx_open = false;
        }
    }

    // Param dropdown popup: resolve the open field's row each
    // frame (pointers are per-frame); a pick stages the option index
    // exactly like a slider release, so the existing param appliers and
    // undo coalescing do the rest. Clicks route here before the nodes.
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
    auto dd_rect = [&]() {
        const int n =
            dd_row_p ? doc::param_option_count(dd_row_p->options) : 0;
        const float w = std::max(st.dd_field.w, 110.0f);
        const float h = static_cast<float>(n) * 20.0f + 8.0f;
        return Rect{
            std::clamp(st.dd_field.x, r.x + 4.0f, r.right() - w - 4.0f),
            std::min(st.dd_field.bottom() + 2.0f, r.bottom() - h - 4.0f),
            w, h};
    };
    if (dd_row_p && frame.input.left_pressed() && owns) {
        menu_swallowed_press = true;
        const Rect mr = dd_rect();
        if (mr.contains(mouse)) {
            const int row =
                static_cast<int>((mouse.y - (mr.y + 4.0f)) / 20.0f);
            const int n = doc::param_option_count(dd_row_p->options);
            if (row >= 0 && row < n) {
                *dd_row_p->staged =
                    dd_row_p->min_v + static_cast<float>(row);
                if (dd_row_p->changed) *dd_row_p->changed = true;
                if (dd_row_p->released) *dd_row_p->released = true;
            }
        }
        st.dd_open = false;
        dd_row_p = nullptr;
    }

    // Open menu: clicks route to it before anything else; wheel scrolls
    // its list; clicking outside closes and swallows the press. In
    // category mode the HOVERED category opens its flyout; a click in
    // the flyout picks the item.
    if (st.add_open) {
        const Rect mr = menu_rect();
        const bool have_fly =
            cat_mode && st.add_cat >= 0 && fly1 > fly0;
        const Rect fr2 = have_fly ? fly_rect() : Rect{};
        // Hover-open (standard menu feel): a category row under the
        // cursor opens its submenu; the flyout keeps itself open.
        if (cat_mode && mr.contains(mouse) &&
            mouse.y >= mr.y + 26.0f) {
            const int row =
                static_cast<int>((mouse.y - (mr.y + 26.0f)) / 18.0f);
            if (row >= 0 && row < cat_count)
                st.add_cat = cat_rows[row];
        }
        if (frame.input.left_pressed() && owns) {
            menu_swallowed_press = true;
            if (have_fly && fr2.contains(mouse)) {
                const int row = static_cast<int>(
                    (mouse.y - (fr2.y + 4.0f)) / 18.0f);
                const int idx = fly0 + row;
                if (row >= 0 && idx < fly1) out.add_pick = idx;
            } else if (mr.contains(mouse)) {
                if (!cat_mode) {
                    const int row = static_cast<int>(
                        (mouse.y - (mr.y + 26.0f) + st.add_scroll) /
                        18.0f);
                    if (mouse.y >= mr.y + 26.0f && row >= 0 &&
                        row < static_cast<int>(g.add_count) &&
                        !(g.add_headers && g.add_headers[row]))
                        out.add_pick = row;
                }
                // Category rows just keep/open the flyout (hover
                // already set add_cat) — the click stays swallowed.
            } else {
                st.add_open = false;
            }
        }
    }

    // Breadcrumb (texed #crumbs): "main > group" chip row while a group
    // is open; clicking the "main" half exits the subgraph view.
    auto crumb_main_rect = [&]() {
        return Rect{r.x + 10.0f, r.y + 8.0f,
                    ui::measure_text(frame.font, "main", 11.0f).x + 16.0f,
                    20.0f};
    };
    if (g.crumb && frame.input.left_pressed() && owns &&
        st.drag_kind == 0 && !menu_swallowed_press &&
        crumb_main_rect().contains(mouse)) {
        out.crumb_clicked = true;
        menu_swallowed_press = true;
    }

    if (frame.input.left_pressed() && owns && st.drag_kind == 0 &&
        !menu_swallowed_press) {
        st.drag_moved = false;
        st.press_screen = mouse;
        // Port grabs win over everything: Out starts a wire, a FED In or
        // matte port grabs its wire for rewiring (texed editor idiom).
        bool port_handled = false;
        for (size_t i = 0; i < n && !port_handled; ++i) {
            const Node& nd = g.nodes[i];
            if (nd.has_out && near2(port_out(nd), 81.0f)) {
                st.drag_kind = 4;
                st.wire_from = nd.id;
                frame.ctx.set_capture(wid);
                port_handled = true;
                break;
            }
            // Input ports: a FED port picks its wire up (rewire, texed
            // pick-up-from-source); an EMPTY one extends a new wire whose
            // fixed end is the input, seeking an Out port (kind 8).
            auto grab_input = [&](Vec2 p,
                                  uint32_t port) {
                if (!near2(p, 81.0f)) return;
                for (size_t w = 0; w < g.wire_count; ++w)
                    if (!g.wires[w].data &&
                        g.wires[w].to_port == port &&
                        g.wires[w].to == nd.id) {
                        st.drag_kind = 5;
                        st.wire_from = g.wires[w].from;
                        st.wire_old_to = nd.id;
                        st.wire_old_port = port;
                        frame.ctx.set_capture(wid);
                        port_handled = true;
                        return;
                    }
                st.drag_kind = 8;
                st.wire_from = 0;
                st.wire_old_to = nd.id;
                st.wire_old_port = port;
                frame.ctx.set_capture(wid);
                port_handled = true;
            };
            if (!port_handled && nd.has_in) grab_input(port_in(nd), 0);
            if (!port_handled && nd.has_matte_port)
                grab_input(port_matte(nd), 1);
            if (!port_handled && nd.has_aux_port)
                grab_input(port_aux(nd), 2);
        }
        if (!port_handled && hover_i >= 0) {
            const Node& nd = g.nodes[static_cast<size_t>(hover_i)];
            const Rect cr = node_rect_s(nd);
            bool handled = false;
            // Shift-click: toggle membership in the multi selection.
            if (frame.input.mods & platform::kModShift) {
                out.clicked = nd.id;
                out.clicked_shift = true;
                handled = true;
            }
            // Title-bar controls: X, then the enable dot.
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
                    // Non-slider rows: the whole field is one
                    // control — dropdowns open their option popup, text
                    // rows open the shared inline editor.
                    const ParamRow& pr = nd.rows[rh.row];
                    if (pr.kind == 1 && pr.staged) {
                        st.dd_open = true;
                        st.dd_node = nd.id;
                        st.dd_row = rh.row;
                        st.dd_field = row_field_rect(nd, rh.row);
                    } else if (pr.kind == 2) {
                        out.text_edit = nd.id;
                    }
                    handled = true;
                } else if (rh.row >= 0 && rh.zone == 1 &&
                           nd.rows[rh.row].staged) {
                    // Press JUMPS the handle to the clicked point and
                    // starts the drag (standard slider feel).
                    st.drag_kind = 3;
                    st.drag_id = nd.id;
                    st.drag_row = rh.row;
                    *nd.rows[rh.row].staged = slider_value(nd, rh.row);
                    if (nd.rows[rh.row].changed)
                        *nd.rows[rh.row].changed = true;
                    frame.ctx.set_capture(wid);
                    handled = true;
                } else if (rh.row >= 0 && rh.zone == 2 &&
                           nd.rows[rh.row].staged) {
                    // The VALUE text opens the inline editor — only the
                    // text area, never the track.
                    out.value_edit_node = nd.id;
                    out.value_edit_row = rh.row;
                    handled = true;
                }
            }
            if (!handled) {
                // Body press: select now, drag moves the node. A double
                // click on a folded Group card enters it (subgraphs).
                const uint64_t fnum = frame.ctx.frame();
                const float dd =
                    std::fabs(mouse.x - st.last_click_pos.x) +
                    std::fabs(mouse.y - st.last_click_pos.y);
                if (nd.kind == NodeKind::Group &&
                    fnum - st.last_click_frame < 24 && dd < 8.0f &&
                    st.last_click_id == nd.id) {
                    // Title double-click renames, body opens (texed
                    // subgraph: title = rename, body = enter).
                    if (mouse.y < cr.y + kTitleH * z)
                        out.group_rename = nd.id;
                    else
                        out.group_open = nd.id;
                    st.last_click_frame = 0;
                } else if (nd.is_look &&
                           fnum - st.last_click_frame < 24 && dd < 8.0f &&
                           st.last_click_id == nd.id &&
                           mouse.y >= cr.y + kTitleH * z) {
                    // A look instance: enter the look it plays.
                    out.look_open = nd.id;
                    st.last_click_frame = 0;
                } else if (nd.text_edit &&
                           fnum - st.last_click_frame < 24 && dd < 8.0f &&
                           st.last_click_id == nd.id &&
                           mouse.y < cr.y + kTitleH * z) {
                    // Text card: title double-click edits the
                    // string through the shared inline editor.
                    out.text_edit = nd.id;
                    st.last_click_frame = 0;
                } else {
                    st.last_click_frame = fnum;
                    st.last_click_pos = mouse;
                    st.last_click_id = nd.id;
                    out.clicked = nd.id;
                    // Cards move by their TITLE BAR only — a body press
                    // just selects (sliders, ports, and preview clicks
                    // must never fling the card).
                    if (mouse.y < cr.y + kTitleH * z) {
                        st.drag_kind = 1;
                        st.drag_id = nd.id;
                        st.drag_alt = false;
                        st.node_grab_x = gmouse.x - nd.x;
                        st.node_grab_y = gmouse.y - nd.y;
                        frame.ctx.set_capture(wid);
                    }
                }
            }
        } else if (!port_handled &&
                   (frame.input.mods & platform::kModShift)) {
            // Shift+drag on empty canvas: marquee selection.
            st.drag_kind = 6;
            st.node_grab_x = gmouse.x;
            st.node_grab_y = gmouse.y;
            frame.ctx.set_capture(wid);
        } else if (!port_handled) {
            // Frame title strips: X removes, double-click renames,
            // elsewhere drags; the bottom-right corner resizes (texed
            // frame-resize handle).
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
                    frame.ctx.set_capture(wid);
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
                    *fb.color_clicked = true;   // cycles the colour tag
                } else {
                    const uint64_t fid = node_id(NodeKind::Frame, fb.id);
                    const uint64_t fnum = frame.ctx.frame();
                    const float dd =
                        std::fabs(mouse.x - st.last_click_pos.x) +
                        std::fabs(mouse.y - st.last_click_pos.y);
                    if (fnum - st.last_click_frame < 24 && dd < 8.0f &&
                        st.last_click_id == fid) {
                        out.frame_rename = fb.id;
                        st.last_click_frame = 0;
                    } else {
                        st.last_click_frame = fnum;
                        st.last_click_pos = mouse;
                        st.last_click_id = fid;
                        st.drag_kind = 1;
                        st.drag_id = fid;
                        // Alt: the frame moves ALONE (texed alt+drag).
                        st.drag_alt =
                            (frame.input.mods & platform::kModAlt) != 0;
                        st.node_grab_x = gmouse.x - fb.x;
                        st.node_grab_y = gmouse.y - fb.y;
                        frame.ctx.set_capture(wid);
                    }
                }
                frame_handled = true;
            }
            if (!frame_handled) {
                // Empty canvas: pan; a still click deselects;
                // double-click requests the add popup.
                st.drag_kind = 2;
                st.drag_id = kEmptyPress;
                frame.ctx.set_capture(wid);
            }
        }
    }

    if (st.drag_kind != 0 && frame.input.left_down()) {
        const float md = std::fabs(mouse.x - st.press_screen.x) +
                         std::fabs(mouse.y - st.press_screen.y);
        if (md > 3.0f) st.drag_moved = true;
        if (st.drag_kind == 1) {
            out.moved = st.drag_id;
            out.moved_x = gmouse.x - st.node_grab_x;
            out.moved_y = gmouse.y - st.node_grab_y;
            out.moved_alt = st.drag_alt;
            // Splice-on-drop (texed): a single UNFED effect card dragged
            // over a wire arms that wire; the drop splices it in.
            st.drag_splice_from = st.drag_splice_to = 0;
            st.drag_splice_port = 0;
            const Node* dn = find_node(st.drag_id);
            if (dn && dn->kind == NodeKind::Effect && dn->has_in &&
                g.multi_count <= 1) {
                bool fed = false;
                for (size_t w = 0; w < g.wire_count; ++w)
                    fed = fed || ((!g.wires[w].data && (g.wires[w].to_port == 0 ||
                                   g.wires[w].to_port == 2)) &&
                                  g.wires[w].to == dn->id);
                if (!fed) {
                    float best = 12.0f * 12.0f;
                    for (size_t w = 0; w < g.wire_count; ++w) {
                        const Wire& wr = g.wires[w];
                        if (wr.data || wr.to_port == 1) continue;
                        if (wr.from == dn->id || wr.to == dn->id)
                            continue;
                        Vec2 p0, p3;
                        if (!wire_ends(wr, &p0, &p3)) continue;
                        const float d2 = wire_near(p0, p3);
                        if (d2 < best) {
                            best = d2;
                            st.drag_splice_from = wr.from;
                            st.drag_splice_to = wr.to;
                            st.drag_splice_port =
                                wr.to_port;
                        }
                    }
                }
            }
        } else if (st.drag_kind == 3) {
            const Node* nd = find_node(st.drag_id);
            if (nd && st.drag_row >= 0 && st.drag_row < nd->row_count &&
                nd->rows[st.drag_row].staged) {
                *nd->rows[st.drag_row].staged =
                    slider_value(*nd, st.drag_row);
                *nd->rows[st.drag_row].changed = true;
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
            // Reverse wire drop: the fixed end is an input; connect from
            // the Out port under the cursor.
            const Node* to_nd = find_node(st.wire_old_to);
            for (size_t i = 0; to_nd && i < n; ++i) {
                const Node& nd = g.nodes[i];
                if (nd.id == st.wire_old_to) continue;
                if (!out_feeds_input(nd, *to_nd, st.wire_old_port))
                    continue;
                if (!near2(port_out(nd), 324.0f)) continue;
                out.connect_requested = true;
                out.connect_from = nd.id;
                out.connect_to = st.wire_old_to;
                out.connect_port = st.wire_old_port;
                break;
            }
            st.wire_old_to = 0;
            st.wire_old_port = 0;
        } else if (st.drag_kind == 4 || st.drag_kind == 5) {
            // Drop resolution: nearest compatible port under the cursor.
            // Mod sources wire into a PARAM ROW; everything else into
            // In / aux / matte.
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
            for (size_t i = 0; i < n && !found && !from_mod; ++i) {
                const Node& nd = g.nodes[i];
                if (nd.id == st.wire_from) continue;
                if (nd.has_in && near2(port_in(nd), 324.0f)) {
                    to = nd.id;
                    port = 0;
                    found = true;
                } else if (nd.has_aux_port &&
                           near2(port_aux(nd), 324.0f)) {
                    to = nd.id;
                    port = 2;
                    found = true;
                } else if (nd.has_matte_port &&
                           near2(port_matte(nd), 324.0f)) {
                    // ANY image out on a matte anchor (masks ARE images)
                    // — the app wires a plain port-1 link.
                    to = nd.id;
                    port = 1;
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
                        out.disconnect_to = st.wire_old_to;
                        out.disconnect_port = st.wire_old_port;
                    }
                    out.connect_requested = true;
                    out.connect_from = st.wire_from;
                    out.connect_to = to;
                    out.connect_port = port;
                }
            } else if (st.drag_kind == 5) {
                out.disconnect_requested = true;
                out.disconnect_from = st.wire_from;
                out.disconnect_to = st.wire_old_to;
                out.disconnect_port = st.wire_old_port;
            }
            st.wire_from = 0;
        } else if (st.drag_kind == 6) {
            out.marquee_done = true;
            out.mq_x0 = std::min(st.node_grab_x, gmouse.x);
            out.mq_y0 = std::min(st.node_grab_y, gmouse.y);
            out.mq_x1 = std::max(st.node_grab_x, gmouse.x);
            out.mq_y1 = std::max(st.node_grab_y, gmouse.y);
            // Wires whose stroke crosses the rect join the selection
            // (texed link marquee) — sampled in screen space.
            const Vec2 ra = to_screen({out.mq_x0, out.mq_y0});
            const Vec2 rb = to_screen({out.mq_x1, out.mq_y1});
            for (size_t w = 0;
                 w < g.wire_count && out.mq_wire_count < 64; ++w) {
                Vec2 p0, p3;
                if (!wire_ends(g.wires[w], &p0, &p3)) continue;
                const float reach = std::clamp(
                    std::fabs(p3.x - p0.x) * 0.5f, 24.0f, 140.0f);
                const Vec2 p1{p0.x + reach, p0.y};
                const Vec2 p2{p3.x - reach, p3.y};
                for (int s = 0; s <= 24; ++s) {
                    const float t = static_cast<float>(s) / 24.0f;
                    const float u = 1.0f - t;
                    const float x = u * u * u * p0.x +
                                    3 * u * u * t * p1.x +
                                    3 * u * t * t * p2.x +
                                    t * t * t * p3.x;
                    const float y = u * u * u * p0.y +
                                    3 * u * u * t * p1.y +
                                    3 * u * t * t * p2.y +
                                    t * t * t * p3.y;
                    if (x >= ra.x && x <= rb.x && y >= ra.y &&
                        y <= rb.y) {
                        out.mq_wires[out.mq_wire_count++] = g.wires[w];
                        break;
                    }
                }
            }
        } else if (st.drag_kind == 3) {
            // A press always wrote a value (jump-to-point), so the
            // release always breaks undo coalescing.
            const Node* nd = find_node(st.drag_id);
            if (nd && st.drag_row >= 0 && st.drag_row < nd->row_count &&
                nd->rows[st.drag_row].released)
                *nd->rows[st.drag_row].released = true;
        } else if (st.drag_kind == 1 && st.drag_moved) {
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
        } else if (st.drag_kind == 2 && !st.drag_moved &&
                   st.drag_id == kEmptyPress && owns) {
            // Still click on empty canvas: a wire under the cursor gets
            // selected (texed sel.links); else deselect; double = add.
            const uint64_t f = frame.ctx.frame();
            const float dd =
                std::fabs(mouse.x - st.last_click_pos.x) +
                std::fabs(mouse.y - st.last_click_pos.y);
            if (f - st.last_click_frame < 24 && dd < 8.0f &&
                st.last_click_id == kEmptyPress) {
                open_add_menu();
                st.last_click_frame = 0;
            } else {
                float best = 9.0f * 9.0f;
                int hit = -1;
                for (size_t w = 0; w < g.wire_count; ++w) {
                    Vec2 p0, p3;
                    if (!wire_ends(g.wires[w], &p0, &p3)) continue;
                    const float d = wire_near(p0, p3);
                    if (d < best) {
                        best = d;
                        hit = static_cast<int>(w);
                    }
                }
                if (hit >= 0) {
                    out.wire_clicked = true;
                    out.wire_clicked_shift =
                        (frame.input.mods & platform::kModShift) != 0;
                    out.wire_from = g.wires[hit].from;
                    out.wire_to = g.wires[hit].to;
                    out.wire_to_port = g.wires[hit].to_port;
                    out.wire_data = g.wires[hit].data;
                    out.wire_to_row = g.wires[hit].to_row;
                    st.last_click_id = 1;   // wire marker, not empty
                } else {
                    out.clicked_empty = true;
                    st.last_click_id = kEmptyPress;
                }
                st.last_click_frame = f;
                st.last_click_pos = mouse;
            }
        }
        st.drag_kind = 0;
        st.drag_id = 0;
        st.drag_row = -1;
        frame.ctx.clear_capture();
    }

    // ---- draw
    canvas.draw_rect(r, theme.window_bg);
    canvas.push_clip(r);

    if (g.hint && g.node_count == 0) {
        const float tw =
            ui::measure_text(frame.font, g.hint, theme.font_size_small).x;
        ui::draw_text(canvas, frame.font, g.hint,
                      {r.x + (r.w - tw) * 0.5f, r.y + r.h * 0.5f},
                      theme.font_size_small, theme.text_disabled);
    }

    // Grid, screen-space lines derived from the graph transform.
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

    // Frames: titled grouping boxes behind everything else.
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
        // Colour tag (texed frame colour): tint fill + outline.
        const bool tinted = fb.color >= 1 && fb.color <= 8;
        const Color fcol =
            tinted ? tint_palette()[(fb.color - 1) & 7] : theme.hairline;
        canvas.draw_sdf_rect(box, 6.0f * z,
                             tinted ? fcol.with_alpha(0.10f)
                                    : theme.control_bg.with_alpha(0.35f));
        canvas.draw_sdf_rect_outline(box, 6.0f * z, 1.0f,
                                     tinted ? fcol.with_alpha(0.55f)
                                            : theme.hairline);
        canvas.draw_rect({box.x, box.y, box.w, 20.0f * z},
                         theme.control_bg.with_alpha(0.5f));
        // Colour dot (cycles on click), left of the X.
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
        canvas.push_clip({box.x, box.y, box.w - 40.0f * z, 20.0f * z});
        char rename_buf[96];
        if (renaming)
            std::snprintf(rename_buf, sizeof(rename_buf), "%s%s",
                          g.rename_text ? g.rename_text : "",
                          (frame.ctx.frame() / 30) % 2 == 0 ? "_" : "");
        ui::draw_text(canvas, frame.font, renaming ? rename_buf : fb.title,
                      {box.x + 8.0f * z, box.y + 4.0f * z}, 11.0f * z,
                      renaming ? theme.text : theme.text_dim);
        canvas.pop_clip();
        // Corner resize handle: two diagonal ticks (texed frame-resize).
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

    // Wires under the cards. Mod wires land on the driven param's row
    // gutter with a terminal dot; the rest end on ports.
    for (size_t w = 0; w < g.wire_count; ++w) {
        const Node* a = find_node(g.wires[w].from);
        const Node* b = find_node(g.wires[w].to);
        if (!a || !b) continue;
        const Vec2 p0 = port_out(*a);
        const bool row_end = g.wires[w].data &&
                             g.wires[w].to_row >= 0 &&
                             g.wires[w].to_row < b->row_count;
        const Vec2 p3 = !g.wires[w].data && g.wires[w].to_port == 1
            ? port_matte(*b)
            : (!g.wires[w].data && g.wires[w].to_port == 2
                   ? port_aux(*b)
                   : (row_end ? row_anchor(*b, g.wires[w].to_row)
                              : port_in(*b)));
        const bool hot = st.hover == a->id || st.hover == b->id ||
                         g.selected == a->id || g.selected == b->id;
        // TWO families only: MEDIA (chain/matte/aux/audio - one generic
        // solid style, whatever the port) and DATA (the value graph,
        // dashed dim). Selection alone wears the accent.
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
                g.sel_wires[sw].to == g.wires[w].to &&
                g.sel_wires[sw].data == g.wires[w].data &&
                g.sel_wires[sw].to_port == g.wires[w].to_port &&
                (g.sel_wires[sw].to_row < 0 ||
                 g.sel_wires[sw].to_row == g.wires[w].to_row))
                col = theme.accent;
        // Armed splice target under the drag: exactly the ONE wire the
        // drop will cut - same endpoints AND same port (image and audio
        // wires share endpoints on the Output), never a data wire.
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

    // Cards. Array order = z-order; the dragged card renders last.
    const float ts = 12.0f * z;    // title em
    const float rs = 10.0f * z;    // row em
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
            const bool selected = g.selected && nd.id == g.selected;
            const bool hovered = st.hover == nd.id;
            bool in_multi = false;
            for (size_t m = 0; m < g.multi_count && !in_multi; ++m)
                in_multi = g.multi[m] == nd.id;

            // Chrome: fill, title bar INSIDE the outline (it was painting
            // over it), then the outline last so it always reads.
            canvas.draw_sdf_rect(cr, 5.0f * z,
                                 nd.bypassed ? theme.control_bg_active
                                             : theme.panel_bg);
            const Rect tb{cr.x + 1.0f, cr.y + 1.0f, cr.w - 2.0f,
                          kTitleH * z - 1.0f};
            canvas.draw_sdf_rect(tb, 4.0f * z,
                                 theme.control_bg.with_alpha(
                                     nd.bypassed ? 0.4f : 1.0f));
            if (nd.tint != 255)
                canvas.draw_rect({tb.x + 1.0f, tb.y + 3.0f * z, 3.0f * z,
                                  tb.h - 6.0f * z},
                                 tint_palette()[nd.tint & 7].with_alpha(
                                     nd.bypassed ? 0.35f : 0.9f));
            canvas.draw_sdf_rect_outline(
                cr, 5.0f * z, selected || in_multi ? 2.0f : 1.0f,
                selected ? theme.accent
                         : (in_multi ? theme.accent_dim
                                     : (hovered ? theme.text_disabled
                                                : theme.hairline)));
            canvas.push_clip({tb.x, tb.y, tb.w - 40.0f * z, tb.h});
            // Cards with a TEXT row edit in the row, not the
            // title — suppress the title editor there.
            bool has_text_row = false;
            for (int tr = 0; tr < nd.row_count; ++tr)
                if (nd.rows[tr].kind == 2) has_text_row = true;
            const bool card_renaming =
                g.rename_node == nd.id && !has_text_row;
            char title_buf[96];
            if (card_renaming)
                std::snprintf(title_buf, sizeof(title_buf), "%s%s",
                              g.rename_text ? g.rename_text : "",
                              (frame.ctx.frame() / 30) % 2 == 0 ? "_"
                                                                : "");
            ui::draw_text(canvas, frame.font,
                          card_renaming ? title_buf : nd.title,
                          {tb.x + 8.0f * z, tb.y + (tb.h - ts) * 0.4f}, ts,
                          nd.bypassed ? theme.text_disabled : theme.text);
            canvas.pop_clip();
            float tcx = tb.right() - 12.0f * z;   // control cursor
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

            // Preview slot, then the port strip (matte/aux live in their
            // own rows so their labels never overlap params).
            float cy = cr.y + kTitleH * z;
            if (node_has_preview(nd)) {
                const Rect pv{cr.x + 6.0f * z, cy + 3.0f * z,
                              cr.w - 12.0f * z, kPrevH * z - 6.0f * z};
                if (nd.preview) {
                    canvas.draw_image_quad(pv, nd.preview, nd.pu0, nd.pv0,
                                           nd.pu1, nd.pv1,
                                           Color::rgba(1, 1, 1, 1),
                                           3.0f * z);
                } else {
                    canvas.draw_sdf_rect(pv, 3.0f * z,
                                         theme.control_bg_active);
                    canvas.draw_sdf_rect_outline(pv, 3.0f * z, 1.0f,
                                                 theme.hairline);
                }
                cy += (kPrevH + 6.0f) * z;
            } else if (nd.scope && nd.scope_count > 1) {
                // Signal strip: the node's output from NOW (left edge)
                // across the sampled window, auto-framed by lo/hi.
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
                Vec2 pp{sv.x, sy_of(nd.scope[0])};
                for (int si = 1; si < nd.scope_count; ++si) {
                    const Vec2 p{
                        sv.x + sv.w * static_cast<float>(si) /
                                   static_cast<float>(nd.scope_count - 1),
                        sy_of(nd.scope[si])};
                    canvas.draw_line(pp, p, std::max(1.0f, 1.2f * z),
                                     theme.accent_dim);
                    pp = p;
                }
                const float dy = sy_of(nd.scope[0]);
                canvas.draw_sdf_rect({sv.x - 1.0f, dy - 2.0f * z,
                                      4.0f * z, 4.0f * z},
                                     2.0f * z, theme.accent);
                cy += (kScopeH + 6.0f) * z;
            } else {
                cy += 2.0f * z;
            }
            cy += static_cast<float>(port_row_count(nd)) * kRowH * z;

            // Param rows.
            const RowHit rh = hovered ? hit_row(nd) : RowHit{};
            for (int row = 0; row < nd.row_count; ++row) {
                const ParamRow& pr = nd.rows[row];
                const float ry = cy + row * kRowH * z;
                const bool row_hot = rh.row == row;
                Color lab = theme.text_dim;
                if (pr.keyed) lab = theme.accent;
                else if (pr.modulated) lab = theme.accent_dim;
                // Persistent keyframe toggle (no hover hotspots): filled
                // accent = the param is keyed (click removes its lane),
                // hollow = unkeyed (click starts one at the playhead).
                float label_x = cr.x + 8.0f * z;
                if (pr.key_clicked) {
                    const float kr2 = 3.0f * z;
                    const Vec2 kc{cr.x + 9.0f * z,
                                  ry + kRowH * z * 0.5f};
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
                // Expose toggle (group face): square dot beside the
                // key dot on scoped member rows — filled = on the face.
                if (pr.expose_clicked) {
                    const float er = 2.8f * z;
                    const Vec2 ec{cr.x + 21.0f * z,
                                  ry + kRowH * z * 0.5f};
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
                canvas.push_clip({cr.x, ry, 62.0f * z, kRowH * z});
                ui::draw_text(canvas, frame.font, pr.label,
                              {label_x, ry + (kRowH * z - rs) * 0.4f},
                              rs, lab);
                canvas.pop_clip();
                if (pr.kind != 0) {
                    // Dropdown / text FIELD spanning the slider + value
                    // area — same row height and margins.
                    const float fx0 = cr.x + 64.0f * z;
                    const Rect fr{fx0, ry + 1.5f * z,
                                  (cr.right() - 8.0f * z) - fx0,
                                  kRowH * z - 3.0f * z};
                    const bool field_hot =
                        row_hot && (rh.zone == 1 || rh.zone == 2);
                    const bool dd_here = st.dd_open &&
                                         st.dd_node == nd.id &&
                                         st.dd_row == row;
                    canvas.draw_sdf_rect(fr, 2.0f * z,
                                         field_hot
                                             ? theme.control_bg_hover
                                             : theme.control_bg);
                    canvas.draw_sdf_rect_outline(
                        fr, 2.0f * z, 1.0f,
                        dd_here ? theme.accent : theme.hairline);
                    const bool editing_text =
                        pr.kind == 2 && g.rename_node == nd.id;
                    const float tx = fr.x + 4.0f * z;
                    canvas.push_clip(
                        {fr.x, fr.y, fr.w - 10.0f * z, fr.h});
                    if (editing_text) {
                        const char* etext =
                            g.rename_text ? g.rename_text : "";
                        const Vec2 es =
                            ui::measure_text(frame.font, etext, rs);
                        ui::draw_text(canvas, frame.font, etext,
                                      {tx, ry + (kRowH * z - rs) * 0.4f},
                                      rs, theme.text);
                        if ((frame.ctx.frame() / 30) % 2 == 0)
                            canvas.draw_rect(
                                {tx + es.x + 1.0f, fr.y + 2.0f * z,
                                 std::max(1.0f, z), fr.h - 4.0f * z},
                                theme.accent);
                    } else {
                        char opt_buf[48];
                        const char* shown = "";
                        if (pr.kind == 1 && pr.staged) {
                            const int n =
                                doc::param_option_count(pr.options);
                            const int idx = std::clamp(
                                static_cast<int>(*pr.staged - pr.min_v +
                                                 0.5f),
                                0, n > 0 ? n - 1 : 0);
                            int olen = 0;
                            const char* o = doc::param_option_at(
                                pr.options, idx, &olen);
                            olen = std::min(
                                olen, static_cast<int>(sizeof(opt_buf)) -
                                          1);
                            std::memcpy(opt_buf, o,
                                        static_cast<size_t>(olen));
                            opt_buf[olen] = '\0';
                            shown = opt_buf;
                        } else if (pr.kind == 2) {
                            shown = pr.text ? pr.text : "";
                        }
                        ui::draw_text(canvas, frame.font, shown,
                                      {tx, ry + (kRowH * z - rs) * 0.4f},
                                      rs,
                                      field_hot ? theme.text
                                                : theme.text_dim);
                    }
                    canvas.pop_clip();
                    if (pr.kind == 1) {
                        // Chevron at the field's right edge.
                        const float cx2 = fr.right() - 8.0f * z;
                        const float cy2 = ry + kRowH * z * 0.5f - 1.0f * z;
                        const float cs = 2.6f * z;
                        const Color cc = field_hot ? theme.text_dim
                                                   : theme.text_disabled;
                        canvas.draw_line({cx2 - cs, cy2},
                                         {cx2, cy2 + cs}, 1.2f, cc);
                        canvas.draw_line({cx2, cy2 + cs},
                                         {cx2 + cs, cy2}, 1.2f, cc);
                    }
                    continue;
                }
                // Slider strip.
                const float sx0 = cr.x + 64.0f * z;
                const float sx1 = cr.x + (kNodeW - 46.0f) * z;
                const float sy = ry + kRowH * z * 0.5f;
                canvas.draw_rect({sx0, sy - 1.0f, sx1 - sx0, 2.0f},
                                 theme.control_bg_hover);
                if (pr.staged) {
                    const float t = std::clamp(
                        (*pr.staged - pr.min_v) /
                            std::max(pr.max_v - pr.min_v, 1e-6f),
                        0.0f, 1.0f);
                    canvas.draw_rect({sx0, sy - 1.0f, (sx1 - sx0) * t,
                                      2.0f},
                                     theme.accent_dim);
                    const float hx = sx0 + (sx1 - sx0) * t;
                    canvas.draw_sdf_rect({hx - 2.5f * z, sy - 4.0f * z,
                                          5.0f * z, 8.0f * z},
                                         1.5f * z,
                                         row_hot ? theme.text
                                                 : theme.text_dim);
                }
                // Live tick: where the driven value actually sits this
                // frame — the handle keeps editing the stored base.
                if (pr.has_live) {
                    const float lt = std::clamp(
                        (pr.live - pr.min_v) /
                            std::max(pr.max_v - pr.min_v, 1e-6f),
                        0.0f, 1.0f);
                    const float lx = sx0 + (sx1 - sx0) * lt;
                    canvas.draw_rect({lx - 1.0f, sy - 5.0f * z,
                                      std::max(2.0f, 1.5f * z),
                                      10.0f * z},
                                     theme.accent);
                }
                // Value text — ALWAYS visible (the typed buffer replaces
                // it only while this row is being edited).
                const bool row_editing = g.value_edit_node == nd.id &&
                                         g.value_edit_row == row;
                if (row_editing) {
                    // Text field matching the value column: text RIGHT-
                    // justified to the same 8-unit margin the value uses,
                    // padded box, blinking caret at the insertion point.
                    const char* etext =
                        g.value_edit_text ? g.value_edit_text : "";
                    const Vec2 es =
                        ui::measure_text(frame.font, etext, rs);
                    const float pad = 5.0f * z;
                    const float caret_w = std::max(1.0f, z);
                    const float ew = std::max(
                        es.x + caret_w + 2.0f * pad, 44.0f * z);
                    const float right = cr.right() - 8.0f * z;
                    const Rect ebox{right + pad - ew, ry + 1.5f * z, ew,
                                    kRowH * z - 3.0f * z};
                    canvas.draw_sdf_rect(ebox, 2.0f * z,
                                         theme.control_bg);
                    canvas.draw_sdf_rect_outline(ebox, 2.0f * z, 1.0f,
                                                 theme.accent);
                    ui::draw_text(canvas, frame.font, etext,
                                  {right - caret_w - es.x,
                                   ry + (kRowH * z - rs) * 0.4f},
                                  rs, theme.text);
                    if ((frame.ctx.frame() / 30) % 2 == 0)
                        canvas.draw_rect({right - caret_w + 1.0f,
                                          ebox.y + 2.0f * z, caret_w,
                                          ebox.h - 4.0f * z},
                                         theme.accent);
                } else if (pr.staged) {
                    char val[32];
                    std::snprintf(val, sizeof(val), pr.format, *pr.staged);
                    const Vec2 vs = ui::measure_text(frame.font, val, rs);
                    ui::draw_text(canvas, frame.font, val,
                                  {cr.right() - 8.0f * z - vs.x,
                                   ry + (kRowH * z - rs) * 0.4f},
                                  rs, theme.text_dim);
                }
            }

            // Ports. Connected ports draw FILLED, empty ones as rings;
            // the port under the cursor lights accent (texed port hover
            // + connected-port fill).
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

    // Breadcrumb chips over everything but the menus.
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

    // Marquee rectangle while dragging.
    if (st.drag_kind == 6) {
        const Vec2 a = to_screen({st.node_grab_x, st.node_grab_y});
        const Rect mq{std::min(a.x, mouse.x), std::min(a.y, mouse.y),
                      std::fabs(mouse.x - a.x), std::fabs(mouse.y - a.y)};
        canvas.draw_rect(mq, theme.accent.with_alpha(0.08f));
        canvas.draw_rect_outline(mq, 1.0f, theme.accent_dim);
    }

    // Live wire drag: rubber band from the origin Out port + accent rings
    // on every compatible drop port (a mod source targets param ROWS —
    // the hovered row lights up).
    if (st.drag_kind == 4 || st.drag_kind == 5) {
        const Node* from_nd = find_node(st.wire_from);
        if (from_nd) {
            const bool from_mod = from_nd->kind == NodeKind::ModSource;
            draw_wire(canvas, port_out(*from_nd), mouse,
                      std::max(1.4f, 1.8f * z), theme.accent, from_mod);
            const float pr3 = 6.0f * z;
            for (size_t i = 0; i < n && !from_mod; ++i) {
                const Node& nd = g.nodes[i];
                if (nd.id == st.wire_from) continue;
                Vec2 candidates[3]{};
                int n_cand = 0;
                if (nd.has_in) candidates[n_cand++] = port_in(nd);
                if (nd.has_aux_port) candidates[n_cand++] = port_aux(nd);
                if (nd.has_matte_port)
                    candidates[n_cand++] = port_matte(nd);
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
                        canvas.draw_rect({cr.x, p.y - kRowH * z * 0.5f,
                                          cr.w, kRowH * z},
                                         theme.accent.with_alpha(0.10f));
                    }
                }
            }
        }
    }

    // Reverse wire drag (kind 8): rubber band into the fixed input port +
    // rings on every Out port that may feed it.
    if (st.drag_kind == 8) {
        const Node* to_nd = find_node(st.wire_old_to);
        if (to_nd) {
            const Vec2 fixed_p = st.wire_old_port == 1
                ? port_matte(*to_nd)
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
                const Vec2 p = port_out(nd);
                canvas.draw_sdf_rect_outline(
                    {p.x - pr3, p.y - pr3, pr3 * 2, pr3 * 2}, pr3, 1.5f,
                    near2(p, 324.0f) ? theme.accent : theme.accent_dim);
            }
        }
    }

    // Add menu at the cursor (texed): filter header + scrolling list; the
    // splice-target wire redraws highlighted while it is open.
    if (st.add_open) {
        if (st.splice_to) {
            const Node* a = find_node(st.splice_from);
            const Node* b = find_node(st.splice_to);
            if (a && b)
                draw_wire(canvas, port_out(*a),
                          st.splice_port == 2 ? port_aux(*b) : port_in(*b),
                          std::max(2.2f, 2.4f * z), theme.accent, false);
        }
        const Rect mr = menu_rect();
        canvas.draw_sdf_rect(mr, 5.0f, theme.control_bg);
        canvas.draw_sdf_rect_outline(mr, 5.0f, 1.0f, theme.accent_dim);
        const bool empty_filter =
            !g.add_filter || g.add_filter[0] == '\0';
        char header[80];
        std::snprintf(header, sizeof(header), "%s%s",
                      empty_filter ? "" : g.add_filter,
                      (frame.ctx.frame() / 30) % 2 == 0 ? "_" : "");
        ui::draw_text(canvas, frame.font,
                      empty_filter ? "type to search..." : header,
                      {mr.x + 8.0f, mr.y + 6.0f}, 11.0f,
                      empty_filter ? theme.text_disabled : theme.text);
        canvas.draw_rect({mr.x + 6.0f, mr.y + 24.0f, mr.w - 12.0f, 1.0f},
                         theme.hairline);
        canvas.push_clip({mr.x, mr.y + 26.0f, mr.w,
                          mr.h - 32.0f});
        if (cat_mode) {
            // Category rows; the open/hovered one carries a ▸ flyout.
            for (int c = 0; c < cat_count; ++c) {
                const float ry = mr.y + 26.0f + c * 18.0f;
                const bool open = cat_rows[c] == st.add_cat;
                const bool hot = mouse.x >= mr.x &&
                                 mouse.x < mr.right() &&
                                 mouse.y >= ry && mouse.y < ry + 18.0f;
                if (open || hot)
                    canvas.draw_rect(
                        {mr.x + 2.0f, ry, mr.w - 4.0f, 18.0f},
                        theme.control_bg_hover);
                ui::draw_text(canvas, frame.font,
                              g.add_items[cat_rows[c]],
                              {mr.x + 10.0f, ry + 3.0f}, 11.0f,
                              open || hot ? theme.text : theme.text_dim);
                const float ax = mr.right() - 12.0f;
                const float ay = ry + 9.0f;
                canvas.draw_line({ax, ay - 3.5f}, {ax + 3.5f, ay}, 1.2f,
                                 theme.text_disabled);
                canvas.draw_line({ax + 3.5f, ay}, {ax, ay + 3.5f}, 1.2f,
                                 theme.text_disabled);
            }
        } else {
            const int first =
                static_cast<int>(st.add_scroll / 18.0f);
            for (int i = first;
                 i < static_cast<int>(g.add_count) && i < first + 15;
                 ++i) {
                const float ry =
                    mr.y + 26.0f + i * 18.0f - st.add_scroll;
                const bool header = g.add_headers && g.add_headers[i];
                const bool hot = !header && mouse.x >= mr.x &&
                                 mouse.x < mr.right() &&
                                 mouse.y >= ry && mouse.y < ry + 18.0f;
                if (hot)
                    canvas.draw_rect(
                        {mr.x + 2.0f, ry, mr.w - 4.0f, 18.0f},
                        theme.control_bg_hover);
                ui::draw_text(canvas, frame.font, g.add_items[i],
                              {mr.x + (header ? 6.0f : 10.0f),
                               ry + (header ? 4.5f : 3.0f)},
                              header ? 9.0f : 11.0f,
                              header ? theme.text_disabled
                                     : (hot ? theme.text
                                            : theme.text_dim));
            }
        }
        canvas.pop_clip();
        // Overflow scrollbar (flat mode): track + proportional thumb.
        const float content_h = static_cast<float>(g.add_count) * 18.0f;
        const float view_h = menu_visible * 18.0f;
        if (!cat_mode && content_h > view_h) {
            const Rect track{mr.right() - 5.0f, mr.y + 26.0f, 3.0f,
                             view_h};
            canvas.draw_rect(track, theme.control_bg_hover);
            const float th =
                std::max(18.0f, view_h * view_h / content_h);
            const float ty =
                track.y + st.add_scroll / (content_h - view_h) *
                              (track.h - th);
            canvas.draw_sdf_rect({track.x, ty, 3.0f, th}, 1.5f,
                                 theme.text_disabled);
        }
        if (g.add_count == 0)
            ui::draw_text(canvas, frame.font, "no match",
                          {mr.x + 10.0f, mr.y + 30.0f}, 11.0f,
                          theme.text_disabled);
        // Flyout submenu for the open category.
        if (cat_mode && st.add_cat >= 0 && fly1 > fly0) {
            const Rect fr2 = fly_rect();
            canvas.draw_sdf_rect(fr2, 5.0f, theme.control_bg);
            canvas.draw_sdf_rect_outline(fr2, 5.0f, 1.0f,
                                         theme.accent_dim);
            for (int i = fly0; i < fly1; ++i) {
                const float ry =
                    fr2.y + 4.0f + static_cast<float>(i - fly0) * 18.0f;
                const bool hot = mouse.x >= fr2.x &&
                                 mouse.x < fr2.right() &&
                                 mouse.y >= ry && mouse.y < ry + 18.0f;
                if (hot)
                    canvas.draw_rect(
                        {fr2.x + 2.0f, ry, fr2.w - 4.0f, 18.0f},
                        theme.control_bg_hover);
                ui::draw_text(canvas, frame.font, g.add_items[i],
                              {fr2.x + 10.0f, ry + 3.0f}, 11.0f,
                              hot ? theme.text : theme.text_dim);
            }
        }
    }

    // Context menu (texed popupMenu): plain hover rows at the cursor.
    if (st.ctx_open && g.ctx_count) {
        const Rect mr = ctx_rect();
        canvas.draw_sdf_rect(mr, 5.0f, theme.control_bg);
        canvas.draw_sdf_rect_outline(mr, 5.0f, 1.0f, theme.accent_dim);
        for (size_t i = 0; i < g.ctx_count; ++i) {
            const float ry = mr.y + 4.0f + static_cast<float>(i) * 20.0f;
            const bool hot = mouse.x >= mr.x && mouse.x < mr.right() &&
                             mouse.y >= ry && mouse.y < ry + 20.0f;
            if (hot)
                canvas.draw_rect({mr.x + 2.0f, ry, mr.w - 4.0f, 20.0f},
                                 theme.control_bg_hover);
            ui::draw_text(canvas, frame.font, g.ctx_items[i],
                          {mr.x + 10.0f, ry + 4.0f}, 11.0f,
                          hot ? theme.text : theme.text_dim);
        }
    }

    // Param dropdown popup: options under the field, current one
    // in accent, hover fill — the ctx menu's visual language.
    if (dd_row_p) {
        const int n = doc::param_option_count(dd_row_p->options);
        const Rect mr = dd_rect();
        canvas.draw_sdf_rect(mr, 5.0f, theme.control_bg);
        canvas.draw_sdf_rect_outline(mr, 5.0f, 1.0f, theme.accent_dim);
        const int cur = std::clamp(
            static_cast<int>(*dd_row_p->staged - dd_row_p->min_v + 0.5f),
            0, n > 0 ? n - 1 : 0);
        for (int i = 0; i < n; ++i) {
            const float ry = mr.y + 4.0f + static_cast<float>(i) * 20.0f;
            const bool hot = mouse.x >= mr.x && mouse.x < mr.right() &&
                             mouse.y >= ry && mouse.y < ry + 20.0f;
            if (hot)
                canvas.draw_rect({mr.x + 2.0f, ry, mr.w - 4.0f, 20.0f},
                                 theme.control_bg_hover);
            char opt_buf[48];
            int olen = 0;
            const char* o =
                doc::param_option_at(dd_row_p->options, i, &olen);
            olen = std::min(olen,
                            static_cast<int>(sizeof(opt_buf)) - 1);
            std::memcpy(opt_buf, o, static_cast<size_t>(olen));
            opt_buf[olen] = '\0';
            ui::draw_text(canvas, frame.font, opt_buf,
                          {mr.x + 10.0f, ry + 4.0f}, 11.0f,
                          i == cur ? theme.accent
                                   : (hot ? theme.text : theme.text_dim));
        }
    }

    canvas.pop_clip();
}

}  // namespace

float node_width() { return kNodeW; }

float node_height(int row_count, bool has_preview, int port_rows) {
    return kTitleH + (has_preview ? kPrevH + 6.0f : 2.0f) +
           static_cast<float>(port_rows + row_count) * kRowH + kPadB;
}

float node_height_of(const Node& nd) {
    float h = node_height(nd.row_count, node_has_preview(nd),
                          port_row_count(nd));
    if (!node_has_preview(nd) && nd.scope && nd.scope_count > 1)
        h += kScopeH + 6.0f - 2.0f;   // scope strip replaces the 2px gap
    return h;
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

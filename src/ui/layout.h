#pragma once

#include <initializer_list>
#include <vector>

#include "ui/arena.h"
#include "ui/canvas2d.h"
#include "ui/font.h"
#include "ui/input.h"
#include "ui/types.h"
#include "ui/ui_context.h"

namespace looks::ui {

inline constexpr float kUnboundedAxis = 1.0e30f;

struct Constraints {
    float min_w = 0, max_w = kUnboundedAxis;
    float min_h = 0, max_h = kUnboundedAxis;

    static Constraints tight(Vec2 size) {
        return {size.x, size.x, size.y, size.y};
    }
    static Constraints loose(Vec2 size) { return {0, size.x, 0, size.y}; }
    bool bounded_w() const { return max_w < kUnboundedAxis * 0.5f; }
};

enum class SizeMode : uint8_t { Auto, Fixed, Fill, Percent };

struct SizeSpec {
    SizeMode mode = SizeMode::Auto;
    float value = 0.0f;   // px for Fixed, weight for Fill, fraction for Percent

    static SizeSpec fixed(float px) { return {SizeMode::Fixed, px}; }
    static SizeSpec fill(float weight = 1.0f) { return {SizeMode::Fill, weight}; }
    static SizeSpec percent(float fraction) { return {SizeMode::Percent, fraction}; }
};

enum class NodeKind : uint8_t {
    Leaf, VStack, HStack, ZStack, Padding, ScrollArea
};

enum class AlignMode : uint8_t { Start, Center, End, Stretch };
enum class Justify : uint8_t {
    Start, Center, End, SpaceBetween, SpaceAround, SpaceEvenly
};

struct ScrollState {
    float offset = 0.0f;        // px scrolled along the main axis
    float content = 0.0f;       // measured content extent (set by layout)
    float viewport = 0.0f;      // visible extent (set by layout)
    float drag_grab = 0.0f;     // offset of grab point within the thumb
};

struct LayoutNode;
class Theme;

struct LayoutFrame {
    Canvas2D& canvas;
    UiInput& input;
    Context& ctx;
    const Font& font;
    const Theme& theme;
    float dt = 0.0f;
    // Optional serif face for headers; null means use font.
    const Font* header_font = nullptr;
};

using MeasureFn = Vec2 (*)(LayoutNode&, const Constraints&, const LayoutFrame&);
using DrawFn = void (*)(LayoutNode&, LayoutFrame&);
using HitFn = void (*)(LayoutNode&, LayoutFrame&);

struct LayoutNode {
    NodeKind kind = NodeKind::Leaf;
    SizeSpec width, height;
    Edges padding;
    AlignMode cross_align = AlignMode::Stretch;
    Justify justify = Justify::Start;
    float gap = 0.0f;
    LayoutNode** children = nullptr;
    uint16_t child_count = 0;
    ScrollState* scroll = nullptr;  // ScrollArea only

    MeasureFn measure_fn = nullptr; // Leaf content measure (optional)
    DrawFn draw_fn = nullptr;
    HitFn hit_fn = nullptr;
    void* user = nullptr;
    const char* debug_name = nullptr;

    // The layout passes write these.
    Rect rect{};
    Vec2 desired{};
    Rect clip{};    // empty() = unclipped
};

struct StackOpts {
    float gap = 0.0f;
    Edges padding;
    Justify justify = Justify::Start;
    AlignMode cross_align = AlignMode::Stretch;
    SizeSpec width, height;
};

LayoutNode* make_node(LayoutArena& arena, NodeKind kind);
LayoutNode* with_children(LayoutArena& arena, LayoutNode* node,
                          std::initializer_list<LayoutNode*> children);

LayoutNode* VStack(LayoutArena& arena, const StackOpts& opts,
                   std::initializer_list<LayoutNode*> children);
LayoutNode* HStack(LayoutArena& arena, const StackOpts& opts,
                   std::initializer_list<LayoutNode*> children);
// Null children are skipped, the same as the initializer-list forms.
LayoutNode* VStackDyn(LayoutArena& arena, const StackOpts& opts,
                      const std::vector<LayoutNode*>& children);
LayoutNode* HStackDyn(LayoutArena& arena, const StackOpts& opts,
                      const std::vector<LayoutNode*>& children);
LayoutNode* ZStack(LayoutArena& arena,
                   std::initializer_list<LayoutNode*> children);
LayoutNode* Padding_(LayoutArena& arena, Edges edges, LayoutNode* child);
LayoutNode* SizedBox(LayoutArena& arena, SizeSpec width, SizeSpec height,
                     LayoutNode* child /* may be null */);
LayoutNode* Spacer(LayoutArena& arena, float weight = 1.0f);
LayoutNode* ScrollAreaV(LayoutArena& arena, ScrollState* state,
                        LayoutNode* child, SizeSpec width = SizeSpec::fill(),
                        SizeSpec height = SizeSpec::fill());

Vec2 measure(LayoutNode& node, const Constraints& c, const LayoutFrame& frame);
void arrange(LayoutNode& node, const Rect& rect, const Rect& parent_clip);
void layout(LayoutNode& root, const Rect& rect, const LayoutFrame& frame);

// Runs layout, hit, wheel, draw, and gc in that order.
void run_frame(LayoutNode* root, const Rect& rect, LayoutFrame& frame);

}  // namespace looks::ui

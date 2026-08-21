#include "doc/stack_commands.h"

#include <cassert>
#include <utility>

#include "doc/effects.h"

namespace looks::doc {

namespace {

std::vector<EffectInstance>& stack_of(Look& look, size_t layer_index) {
    assert(layer_index < look.layers.size());
    return look.layers[layer_index].stack;
}

float& param_ref(Look& look, size_t layer_index, size_t effect_index,
                 int param_index) {
    auto& stack = stack_of(look, layer_index);
    assert(effect_index < stack.size());
    EffectInstance& fx = stack[effect_index];
    if (param_index == kWetParam) return fx.wet;
    if (param_index == kOpacityParam) return fx.opacity;
    assert(param_index >= 0 &&
           static_cast<size_t>(param_index) < fx.params.size());
    return fx.params[static_cast<size_t>(param_index)];
}

class SetParamCommand final : public LookCommand {
public:
    SetParamCommand(uint64_t look, size_t layer_index, size_t effect_index,
                    int param_index, float new_value)
        : LookCommand(look), layer_index_(layer_index),
          effect_index_(effect_index), param_index_(param_index),
          new_value_(new_value) {}

    std::string name() const override { return "Edit Parameter"; }

    void apply(Document& doc) override {
        float& p = param_ref(look_of(doc), layer_index_, effect_index_,
                             param_index_);
        old_value_ = p;
        p = new_value_;
    }

    void revert(Document& doc) override {
        param_ref(look_of(doc), layer_index_, effect_index_, param_index_) =
            old_value_;
    }

    bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetParamCommand*>(&next);
        if (!other || !same_look(*other) ||
            other->layer_index_ != layer_index_ ||
            other->effect_index_ != effect_index_ ||
            other->param_index_ != param_index_)
            return false;
        new_value_ = other->new_value_;   // keep our old_value_
        return true;
    }

private:
    size_t layer_index_;
    size_t effect_index_;
    int param_index_;
    float new_value_;
    float old_value_ = 0.0f;
};

class SetBypassCommand final : public LookCommand {
public:
    SetBypassCommand(uint64_t look, size_t layer_index, size_t effect_index,
                     bool bypass)
        : LookCommand(look), layer_index_(layer_index),
          effect_index_(effect_index), bypass_(bypass) {}

    std::string name() const override {
        return bypass_ ? "Bypass Effect" : "Enable Effect";
    }

    void apply(Document& doc) override {
        auto& stack = stack_of(look_of(doc), layer_index_);
        assert(effect_index_ < stack.size());
        old_bypass_ = stack[effect_index_].bypass;
        stack[effect_index_].bypass = bypass_;
    }

    void revert(Document& doc) override {
        stack_of(look_of(doc), layer_index_)[effect_index_].bypass =
            old_bypass_;
    }

private:
    size_t layer_index_;
    size_t effect_index_;
    bool bypass_;
    bool old_bypass_ = false;
};

class SetEffectTextCommand final : public LookCommand {
public:
    SetEffectTextCommand(uint64_t look, size_t layer_index, size_t effect_index,
                         std::string text)
        : LookCommand(look), layer_index_(layer_index),
          effect_index_(effect_index), text_(std::move(text)) {}

    std::string name() const override { return "Edit Text"; }

    void apply(Document& doc) override {
        auto& stack = stack_of(look_of(doc), layer_index_);
        assert(effect_index_ < stack.size());
        old_text_ = stack[effect_index_].text;
        stack[effect_index_].text = text_;
    }

    void revert(Document& doc) override {
        stack_of(look_of(doc), layer_index_)[effect_index_].text = old_text_;
    }

private:
    size_t layer_index_;
    size_t effect_index_;
    std::string text_;
    std::string old_text_;
};

class SetSoloCommand final : public LookCommand {
public:
    SetSoloCommand(uint64_t look, size_t layer_index, size_t effect_index,
                   bool solo)
        : LookCommand(look), layer_index_(layer_index),
          effect_index_(effect_index), solo_(solo) {}

    std::string name() const override {
        return solo_ ? "Solo Effect" : "Unsolo Effect";
    }

    void apply(Document& doc) override {
        auto& stack = stack_of(look_of(doc), layer_index_);
        assert(effect_index_ < stack.size());
        old_solo_ = stack[effect_index_].solo;
        stack[effect_index_].solo = solo_;
    }

    void revert(Document& doc) override {
        stack_of(look_of(doc), layer_index_)[effect_index_].solo = old_solo_;
    }

private:
    size_t layer_index_;
    size_t effect_index_;
    bool solo_;
    bool old_solo_ = false;
};

class AddEffectCommand final : public LookCommand {
public:
    AddEffectCommand(uint64_t look, size_t layer_index, EffectInstance instance,
                     size_t insert_index)
        : LookCommand(look), layer_index_(layer_index),
          instance_(std::move(instance)), insert_index_(insert_index) {}

    std::string name() const override {
        return std::string("Add ") + effect_info(instance_.type).label;
    }

    void apply(Document& doc) override {
        auto& stack = stack_of(look_of(doc), layer_index_);
        assert(insert_index_ <= stack.size());
        stack.insert(stack.begin() + insert_index_, instance_);
    }

    void revert(Document& doc) override {
        auto& stack = stack_of(look_of(doc), layer_index_);
        stack.erase(stack.begin() + insert_index_);
    }

private:
    size_t layer_index_;
    EffectInstance instance_;
    size_t insert_index_;
};

class RemoveEffectCommand final : public LookCommand {
public:
    RemoveEffectCommand(uint64_t look, size_t layer_index, size_t effect_index)
        : LookCommand(look), layer_index_(layer_index),
          effect_index_(effect_index) {}

    std::string name() const override { return "Remove Effect"; }

    void apply(Document& doc) override {
        auto& stack = stack_of(look_of(doc), layer_index_);
        assert(effect_index_ < stack.size());
        removed_ = stack[effect_index_];
        stack.erase(stack.begin() + effect_index_);
    }

    void revert(Document& doc) override {
        auto& stack = stack_of(look_of(doc), layer_index_);
        stack.insert(stack.begin() + effect_index_, removed_);
    }

private:
    size_t layer_index_;
    size_t effect_index_;
    EffectInstance removed_;
};

class MoveEffectCommand final : public LookCommand {
public:
    MoveEffectCommand(uint64_t look, size_t layer_index, size_t from_index,
                      size_t to_index)
        : LookCommand(look), layer_index_(layer_index), from_(from_index),
          to_(to_index) {}

    std::string name() const override { return "Move Effect"; }

    void apply(Document& doc) override {
        shift(look_of(doc), layer_index_, from_, to_);
    }
    void revert(Document& doc) override {
        shift(look_of(doc), layer_index_, to_, from_);
    }

private:
    static void shift(Look& look, size_t layer, size_t from, size_t to) {
        auto& stack = stack_of(look, layer);
        assert(from < stack.size() && to < stack.size());
        EffectInstance fx = std::move(stack[from]);
        stack.erase(stack.begin() + from);
        stack.insert(stack.begin() + to, std::move(fx));
    }

    size_t layer_index_;
    size_t from_;
    size_t to_;
};

// Node-canvas placement. Resolves the target by
// document id on every apply/revert — indices may shift under undo, ids
// never do. A missing target is a silent no-op (node deleted mid-history).
class SetNodePosCommand final : public LookCommand {
public:
    SetNodePosCommand(uint64_t look, NodeRef kind, uint64_t id, float x,
                      float y)
        : LookCommand(look), kind_(kind), id_(id), x_(x), y_(y) {}

    std::string name() const override { return "Move Node"; }

    void apply(Document& doc) override {
        const Pos p = resolve(look_of(doc));
        if (!p.x) return;
        old_x_ = *p.x;
        old_y_ = *p.y;
        *p.x = x_;
        *p.y = y_;
    }

    void revert(Document& doc) override {
        const Pos p = resolve(look_of(doc));
        if (!p.x) return;
        *p.x = old_x_;
        *p.y = old_y_;
    }

    bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetNodePosCommand*>(&next);
        if (!other || !same_look(*other) || other->kind_ != kind_ ||
            other->id_ != id_)
            return false;
        x_ = other->x_;
        y_ = other->y_;
        return true;
    }

private:
    struct Pos {
        float* x = nullptr;
        float* y = nullptr;
    };
    Pos resolve(Look& look) const {
        switch (kind_) {
            case NodeRef::Effect:
                if (EffectInstance* fx = find_effect(look, id_))
                    return {&fx->node_x, &fx->node_y};
                return {};
            case NodeRef::Layer:
                if (Layer* l = find_layer(look, id_))
                    return {&l->node_x, &l->node_y};
                return {};
            case NodeRef::Route:
                if (ValueNode* n = find_value_node(look, id_))
                    return {&n->node_x, &n->node_y};
                return {};
            case NodeRef::Output:
                return {&look.out_node_x, &look.out_node_y};
            case NodeRef::Frame:
                for (CanvasFrame& f : look.frames)
                    if (f.id == id_) return {&f.x, &f.y};
                return {};
            case NodeRef::Group:
                for (Layer& l : look.layers)
                    for (Group& g : l.groups)
                        if (g.id == id_) return {&g.node_x, &g.node_y};
                return {};
            case NodeRef::GroupIn:
                for (Layer& l : look.layers)
                    for (Group& g : l.groups)
                        if (g.id == id_) return {&g.in_x, &g.in_y};
                return {};
            case NodeRef::GroupOut:
                for (Layer& l : look.layers)
                    for (Group& g : l.groups)
                        if (g.id == id_) return {&g.out_x, &g.out_y};
                return {};
        }
        return {};
    }

    NodeRef kind_;
    uint64_t id_;
    float x_, y_;
    float old_x_ = 0.0f, old_y_ = 0.0f;
};

// Freezes the implicit stack-order wiring into the link table:
// every node-creation path runs this FIRST so newborns spawn UNWIRED —
// with the table empty, stack-order synthesis would chain them straight
// into the composite. Wiring is a wire gesture, never a side effect of
// adding. On a look with no layers the synthesis is empty and this
// stays a no-op: the very first source keeps auto-wiring to the Output
// (a black one-node composite would be hostile).
class MaterializeLinksCommand final : public LookCommand {
public:
    explicit MaterializeLinksCommand(uint64_t look) : LookCommand(look) {}
    std::string name() const override { return "Materialize Links"; }

    void apply(Document& doc) override {
        Look& look = look_of(doc);
        materialized_ = look.links.empty();
        ensure_links(look);
    }

    void revert(Document& doc) override {
        if (materialized_) look_of(doc).links.clear();
    }

private:
    bool materialized_ = false;
};

// TRUE GRAPH link edits. apply/revert address
// links by value — ids are stable, indices are not. First edit
// materializes the synthesized legacy table (reverted symmetrically).
class ConnectCommand final : public LookCommand {
public:
    ConnectCommand(uint64_t look, NodeLink link)
        : LookCommand(look), link_(link) {}
    std::string name() const override { return "Connect Nodes"; }

    void apply(Document& doc) override {
        Look& look = look_of(doc);
        materialized_ = look.links.empty();
        ensure_links(look);
        pruned_ = prune_tombstone(look);   // a real wire replaces it
        had_replaced_ = false;
        if (link_.to != 0 || link_.to_port != 0) {
            // One feed per (to, port) - the Output's audio-in included:
            // wiring it must never touch the image feed on port 0.
            for (auto it = look.links.begin(); it != look.links.end(); ++it)
                if (it->to == link_.to && it->to_port == link_.to_port) {
                    replaced_ = *it;
                    had_replaced_ = true;
                    look.links.erase(it);
                    break;
                }
        } else {
            // Output composites ONE contribution per owner layer (cross-
            // layer fan-in is the layer merge); a new chain end REPLACES
            // the same layer's old one — leaving it produced ghost wires
            // whose contribution double-composited the chain prefix.
            auto owner_of = [&](uint64_t id) -> uint64_t {
                if (find_layer(look, id)) return id;
                const Layer* owner = nullptr;
                if (find_effect(std::as_const(look), id, &owner))
                    return owner->id;
                return 0;
            };
            const uint64_t own = owner_of(link_.from);
            for (auto it = look.links.begin();
                 own && it != look.links.end(); ++it)
                if (it->to == 0 && it->to_port == 0 &&
                    owner_of(it->from) == own) {
                    replaced_ = *it;
                    had_replaced_ = true;
                    look.links.erase(it);
                    break;
                }
        }
        look.links.push_back(link_);
    }

    void revert(Document& doc) override {
        Look& look = look_of(doc);
        for (auto it = look.links.rbegin(); it != look.links.rend(); ++it)
            if (it->from == link_.from && it->to == link_.to &&
                it->to_port == link_.to_port) {
                look.links.erase(std::next(it).base());
                break;
            }
        if (had_replaced_) look.links.push_back(replaced_);
        if (pruned_) seal_links(look);
        if (materialized_) look.links.clear();
    }

private:
    NodeLink link_;
    NodeLink replaced_{};
    bool had_replaced_ = false;
    bool materialized_ = false;
    bool pruned_ = false;
};

class DisconnectCommand final : public LookCommand {
public:
    DisconnectCommand(uint64_t look, NodeLink link)
        : LookCommand(look), link_(link) {}
    std::string name() const override { return "Disconnect Nodes"; }

    void apply(Document& doc) override {
        Look& look = look_of(doc);
        materialized_ = look.links.empty();
        ensure_links(look);
        removed_ = false;
        for (auto it = look.links.begin(); it != look.links.end(); ++it)
            if (it->from == link_.from && it->to == link_.to &&
                it->to_port == link_.to_port) {
                look.links.erase(it);
                removed_ = true;
                break;
            }
        sealed_ = false;
        if (removed_ && look.links.empty()) {
            seal_links(look);
            sealed_ = true;
        }
    }

    void revert(Document& doc) override {
        Look& look = look_of(doc);
        if (sealed_) prune_tombstone(look);
        if (removed_) look.links.push_back(link_);
        if (materialized_) look.links.clear();
    }

private:
    NodeLink link_;
    bool removed_ = false;
    bool materialized_ = false;
    bool sealed_ = false;
};

// Canvas frames: pure annotations, but still
// undoable like every mutation.
class AddFrameCommand final : public LookCommand {
public:
    AddFrameCommand(uint64_t look, CanvasFrame frame)
        : LookCommand(look), frame_(std::move(frame)) {}
    std::string name() const override { return "Add Frame"; }
    void apply(Document& doc) override {
        look_of(doc).frames.push_back(frame_);
    }
    void revert(Document& doc) override {
        Look& look = look_of(doc);
        for (auto it = look.frames.begin(); it != look.frames.end(); ++it)
            if (it->id == frame_.id) {
                look.frames.erase(it);
                break;
            }
    }

private:
    CanvasFrame frame_;
};

class RemoveFrameCommand final : public LookCommand {
public:
    RemoveFrameCommand(uint64_t look, uint64_t id)
        : LookCommand(look), id_(id) {}
    std::string name() const override { return "Remove Frame"; }
    void apply(Document& doc) override {
        Look& look = look_of(doc);
        for (auto it = look.frames.begin(); it != look.frames.end(); ++it)
            if (it->id == id_) {
                removed_ = *it;
                had_ = true;
                look.frames.erase(it);
                break;
            }
    }
    void revert(Document& doc) override {
        if (had_) look_of(doc).frames.push_back(removed_);
    }

private:
    uint64_t id_;
    CanvasFrame removed_{};
    bool had_ = false;
};

// Frame resize drags coalesce per frame id (one undo step per gesture);
// rename is a single edit.
class SetFrameBoundsCommand final : public LookCommand {
public:
    SetFrameBoundsCommand(uint64_t look, uint64_t id, float w, float h)
        : LookCommand(look), id_(id), w_(w), h_(h) {}
    std::string name() const override { return "Resize Frame"; }
    void apply(Document& doc) override {
        for (CanvasFrame& f : look_of(doc).frames)
            if (f.id == id_) {
                old_w_ = f.w;
                old_h_ = f.h;
                f.w = w_;
                f.h = h_;
                break;
            }
    }
    void revert(Document& doc) override {
        for (CanvasFrame& f : look_of(doc).frames)
            if (f.id == id_) {
                f.w = old_w_;
                f.h = old_h_;
                break;
            }
    }
    bool merge(const Command& next) override {
        const auto* other =
            dynamic_cast<const SetFrameBoundsCommand*>(&next);
        if (!other || !same_look(*other) || other->id_ != id_) return false;
        w_ = other->w_;
        h_ = other->h_;
        return true;
    }

private:
    uint64_t id_;
    float w_, h_;
    float old_w_ = 0.0f, old_h_ = 0.0f;
};

class SetFrameColorCommand final : public LookCommand {
public:
    SetFrameColorCommand(uint64_t look, uint64_t id, uint32_t color)
        : LookCommand(look), id_(id), color_(color) {}
    std::string name() const override { return "Tag Frame Colour"; }
    void apply(Document& doc) override {
        for (CanvasFrame& f : look_of(doc).frames)
            if (f.id == id_) {
                old_color_ = f.color;
                f.color = color_;
                break;
            }
    }
    void revert(Document& doc) override {
        for (CanvasFrame& f : look_of(doc).frames)
            if (f.id == id_) {
                f.color = old_color_;
                break;
            }
    }

private:
    uint64_t id_;
    uint32_t color_;
    uint32_t old_color_ = 0;
};

class SetFrameTitleCommand final : public LookCommand {
public:
    SetFrameTitleCommand(uint64_t look, uint64_t id, std::string title)
        : LookCommand(look), id_(id), title_(std::move(title)) {}
    std::string name() const override { return "Rename Frame"; }
    void apply(Document& doc) override {
        for (CanvasFrame& f : look_of(doc).frames)
            if (f.id == id_) {
                old_title_ = f.title;
                f.title = title_;
                break;
            }
    }
    void revert(Document& doc) override {
        for (CanvasFrame& f : look_of(doc).frames)
            if (f.id == id_) {
                f.title = old_title_;
                break;
            }
    }

private:
    uint64_t id_;
    std::string title_;
    std::string old_title_;
};

}  // namespace

std::unique_ptr<Command> add_frame_command(uint64_t look, CanvasFrame frame) {
    return std::make_unique<AddFrameCommand>(look, std::move(frame));
}

std::unique_ptr<Command> set_frame_bounds_command(uint64_t look,
                                                  uint64_t frame_id, float w,
                                                  float h) {
    return std::make_unique<SetFrameBoundsCommand>(look, frame_id, w, h);
}

std::unique_ptr<Command> set_frame_title_command(uint64_t look,
                                                 uint64_t frame_id,
                                                 std::string title) {
    return std::make_unique<SetFrameTitleCommand>(look, frame_id,
                                                  std::move(title));
}

std::unique_ptr<Command> set_effect_text_command(uint64_t look,
                                                 size_t layer_index,
                                                 size_t effect_index,
                                                 std::string text) {
    return std::make_unique<SetEffectTextCommand>(look, layer_index,
                                                  effect_index,
                                                  std::move(text));
}

std::unique_ptr<Command> set_frame_color_command(uint64_t look,
                                                 uint64_t frame_id,
                                                 uint32_t color) {
    return std::make_unique<SetFrameColorCommand>(look, frame_id, color);
}

std::unique_ptr<Command> remove_frame_command(uint64_t look,
                                              uint64_t frame_id) {
    return std::make_unique<RemoveFrameCommand>(look, frame_id);
}

bool link_would_cycle(const Look& look, uint64_t from, uint64_t to) {
    if (from == to) return true;
    if (to == 0) return false;   // Output has no outgoing links
    const std::vector<NodeLink> links =
        look.links.empty() ? synthesize_links(look) : look.links;
    // Downstream walk from `to`: reaching `from` means the new link would
    // close a loop (texed graph.js isReachable).
    std::vector<uint64_t> stack{to};
    std::vector<uint64_t> seen;
    while (!stack.empty()) {
        const uint64_t n = stack.back();
        stack.pop_back();
        if (n == from) return true;
        bool visited = false;
        for (uint64_t s : seen) visited = visited || s == n;
        if (visited) continue;
        seen.push_back(n);
        for (const NodeLink& l : links)
            if (l.from == n && l.to != 0) stack.push_back(l.to);
    }
    return false;
}

std::unique_ptr<Command> materialize_links_command(uint64_t look) {
    return std::make_unique<MaterializeLinksCommand>(look);
}

std::unique_ptr<Command> connect_command(uint64_t look, NodeLink link) {
    return std::make_unique<ConnectCommand>(look, link);
}

std::unique_ptr<Command> disconnect_command(uint64_t look, NodeLink link) {
    return std::make_unique<DisconnectCommand>(look, link);
}

std::unique_ptr<Command> set_node_pos_command(uint64_t look, NodeRef kind,
                                              uint64_t id, float x, float y) {
    return std::make_unique<SetNodePosCommand>(look, kind, id, x, y);
}

std::unique_ptr<Command> set_param_command(uint64_t look, size_t layer_index,
                                           size_t effect_index,
                                           int param_index, float new_value) {
    return std::make_unique<SetParamCommand>(look, layer_index, effect_index,
                                             param_index, new_value);
}

std::unique_ptr<Command> set_bypass_command(uint64_t look, size_t layer_index,
                                            size_t effect_index, bool bypass) {
    return std::make_unique<SetBypassCommand>(look, layer_index, effect_index,
                                              bypass);
}

std::unique_ptr<Command> set_solo_command(uint64_t look, size_t layer_index,
                                          size_t effect_index, bool solo) {
    return std::make_unique<SetSoloCommand>(look, layer_index, effect_index,
                                            solo);
}

std::unique_ptr<Command> add_effect_command(uint64_t look, size_t layer_index,
                                            EffectInstance instance,
                                            size_t insert_index) {
    return std::make_unique<AddEffectCommand>(look, layer_index,
                                              std::move(instance),
                                              insert_index);
}

std::unique_ptr<Command> remove_effect_command(uint64_t look,
                                               size_t layer_index,
                                               size_t effect_index) {
    return std::make_unique<RemoveEffectCommand>(look, layer_index,
                                                 effect_index);
}

std::unique_ptr<Command> move_effect_command(uint64_t look, size_t layer_index,
                                             size_t from_index,
                                             size_t to_index) {
    return std::make_unique<MoveEffectCommand>(look, layer_index, from_index,
                                               to_index);
}

}  // namespace looks::doc

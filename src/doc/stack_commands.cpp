#include "doc/stack_commands.h"

#include <cassert>
#include <utility>

#include "doc/effects.h"

namespace looks::doc {

namespace {

std::vector<EffectInstance>& stack_of(Document& doc, size_t layer_index) {
    assert(layer_index < doc.layers.size());
    return doc.layers[layer_index].stack;
}

float& param_ref(Document& doc, size_t layer_index, size_t effect_index,
                 int param_index) {
    auto& stack = stack_of(doc, layer_index);
    assert(effect_index < stack.size());
    EffectInstance& fx = stack[effect_index];
    if (param_index == kWetParam) return fx.wet;
    if (param_index == kOpacityParam) return fx.opacity;
    assert(param_index >= 0 &&
           static_cast<size_t>(param_index) < fx.params.size());
    return fx.params[static_cast<size_t>(param_index)];
}

class SetParamCommand final : public Command {
public:
    SetParamCommand(size_t layer_index, size_t effect_index, int param_index,
                    float new_value)
        : layer_index_(layer_index), effect_index_(effect_index),
          param_index_(param_index), new_value_(new_value) {}

    std::string name() const override { return "Edit Parameter"; }

    void apply(Document& doc) override {
        float& p = param_ref(doc, layer_index_, effect_index_, param_index_);
        old_value_ = p;
        p = new_value_;
    }

    void revert(Document& doc) override {
        param_ref(doc, layer_index_, effect_index_, param_index_) = old_value_;
    }

    bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetParamCommand*>(&next);
        if (!other || other->layer_index_ != layer_index_ ||
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

class SetBypassCommand final : public Command {
public:
    SetBypassCommand(size_t layer_index, size_t effect_index, bool bypass)
        : layer_index_(layer_index), effect_index_(effect_index),
          bypass_(bypass) {}

    std::string name() const override {
        return bypass_ ? "Bypass Effect" : "Enable Effect";
    }

    void apply(Document& doc) override {
        auto& stack = stack_of(doc, layer_index_);
        assert(effect_index_ < stack.size());
        old_bypass_ = stack[effect_index_].bypass;
        stack[effect_index_].bypass = bypass_;
    }

    void revert(Document& doc) override {
        stack_of(doc, layer_index_)[effect_index_].bypass = old_bypass_;
    }

private:
    size_t layer_index_;
    size_t effect_index_;
    bool bypass_;
    bool old_bypass_ = false;
};

class SetEffectTextCommand final : public Command {
public:
    SetEffectTextCommand(size_t layer_index, size_t effect_index,
                         std::string text)
        : layer_index_(layer_index), effect_index_(effect_index),
          text_(std::move(text)) {}

    std::string name() const override { return "Edit Text"; }

    void apply(Document& doc) override {
        auto& stack = stack_of(doc, layer_index_);
        assert(effect_index_ < stack.size());
        old_text_ = stack[effect_index_].text;
        stack[effect_index_].text = text_;
    }

    void revert(Document& doc) override {
        stack_of(doc, layer_index_)[effect_index_].text = old_text_;
    }

private:
    size_t layer_index_;
    size_t effect_index_;
    std::string text_;
    std::string old_text_;
};

class SetSoloCommand final : public Command {
public:
    SetSoloCommand(size_t layer_index, size_t effect_index, bool solo)
        : layer_index_(layer_index), effect_index_(effect_index),
          solo_(solo) {}

    std::string name() const override {
        return solo_ ? "Solo Effect" : "Unsolo Effect";
    }

    void apply(Document& doc) override {
        auto& stack = stack_of(doc, layer_index_);
        assert(effect_index_ < stack.size());
        old_solo_ = stack[effect_index_].solo;
        stack[effect_index_].solo = solo_;
    }

    void revert(Document& doc) override {
        stack_of(doc, layer_index_)[effect_index_].solo = old_solo_;
    }

private:
    size_t layer_index_;
    size_t effect_index_;
    bool solo_;
    bool old_solo_ = false;
};

class AddEffectCommand final : public Command {
public:
    AddEffectCommand(size_t layer_index, EffectInstance instance,
                     size_t insert_index)
        : layer_index_(layer_index), instance_(std::move(instance)),
          insert_index_(insert_index) {}

    std::string name() const override {
        return std::string("Add ") + effect_info(instance_.type).label;
    }

    void apply(Document& doc) override {
        auto& stack = stack_of(doc, layer_index_);
        assert(insert_index_ <= stack.size());
        stack.insert(stack.begin() + insert_index_, instance_);
    }

    void revert(Document& doc) override {
        auto& stack = stack_of(doc, layer_index_);
        stack.erase(stack.begin() + insert_index_);
    }

private:
    size_t layer_index_;
    EffectInstance instance_;
    size_t insert_index_;
};

class RemoveEffectCommand final : public Command {
public:
    RemoveEffectCommand(size_t layer_index, size_t effect_index)
        : layer_index_(layer_index), effect_index_(effect_index) {}

    std::string name() const override { return "Remove Effect"; }

    void apply(Document& doc) override {
        auto& stack = stack_of(doc, layer_index_);
        assert(effect_index_ < stack.size());
        removed_ = stack[effect_index_];
        stack.erase(stack.begin() + effect_index_);
    }

    void revert(Document& doc) override {
        auto& stack = stack_of(doc, layer_index_);
        stack.insert(stack.begin() + effect_index_, removed_);
    }

private:
    size_t layer_index_;
    size_t effect_index_;
    EffectInstance removed_;
};

class MoveEffectCommand final : public Command {
public:
    MoveEffectCommand(size_t layer_index, size_t from_index, size_t to_index)
        : layer_index_(layer_index), from_(from_index), to_(to_index) {}

    std::string name() const override { return "Move Effect"; }

    void apply(Document& doc) override { shift(doc, layer_index_, from_, to_); }
    void revert(Document& doc) override { shift(doc, layer_index_, to_, from_); }

private:
    static void shift(Document& doc, size_t layer, size_t from, size_t to) {
        auto& stack = stack_of(doc, layer);
        assert(from < stack.size() && to < stack.size());
        EffectInstance fx = std::move(stack[from]);
        stack.erase(stack.begin() + from);
        stack.insert(stack.begin() + to, std::move(fx));
    }

    size_t layer_index_;
    size_t from_;
    size_t to_;
};

// Node-canvas placement (docs/flow_canvas.md). Resolves the target by
// document id on every apply/revert — indices may shift under undo, ids
// never do. A missing target is a silent no-op (node deleted mid-history).
class SetNodePosCommand final : public Command {
public:
    SetNodePosCommand(NodeRef kind, uint64_t id, float x, float y)
        : kind_(kind), id_(id), x_(x), y_(y) {}

    std::string name() const override { return "Move Node"; }

    void apply(Document& doc) override {
        const Pos p = resolve(doc);
        if (!p.x) return;
        old_x_ = *p.x;
        old_y_ = *p.y;
        *p.x = x_;
        *p.y = y_;
    }

    void revert(Document& doc) override {
        const Pos p = resolve(doc);
        if (!p.x) return;
        *p.x = old_x_;
        *p.y = old_y_;
    }

    bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetNodePosCommand*>(&next);
        if (!other || other->kind_ != kind_ || other->id_ != id_)
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
    Pos resolve(Document& doc) const {
        switch (kind_) {
            case NodeRef::Effect:
                for (Layer& l : doc.layers)
                    for (EffectInstance& fx : l.stack)
                        if (fx.id == id_) return {&fx.node_x, &fx.node_y};
                return {};
            case NodeRef::Layer:
                for (Layer& l : doc.layers)
                    if (l.id == id_) return {&l.node_x, &l.node_y};
                return {};
            case NodeRef::Mask:
                for (Mask& m : doc.masks)
                    if (m.id == id_) return {&m.node_x, &m.node_y};
                return {};
            case NodeRef::Route:
                for (ModRoute& r : doc.mod_routes)
                    if (r.id == id_) return {&r.node_x, &r.node_y};
                return {};
            case NodeRef::Output:
                return {&doc.out_node_x, &doc.out_node_y};
            case NodeRef::Frame:
                for (Document::Frame& f : doc.frames)
                    if (f.id == id_) return {&f.x, &f.y};
                return {};
            case NodeRef::Group:
                for (Layer& l : doc.layers)
                    for (Group& g : l.groups)
                        if (g.id == id_) return {&g.node_x, &g.node_y};
                return {};
            case NodeRef::GroupIn:
                for (Layer& l : doc.layers)
                    for (Group& g : l.groups)
                        if (g.id == id_) return {&g.in_x, &g.in_y};
                return {};
            case NodeRef::GroupOut:
                for (Layer& l : doc.layers)
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

// TRUE GRAPH link edits (docs/flow_canvas.md v3). apply/revert address
// links by value — ids are stable, indices are not. First edit
// materializes the synthesized legacy table (reverted symmetrically).
class ConnectCommand final : public Command {
public:
    explicit ConnectCommand(Document::NodeLink link) : link_(link) {}
    std::string name() const override { return "Connect Nodes"; }

    void apply(Document& doc) override {
        materialized_ = doc.links.empty();
        ensure_links(doc);
        had_replaced_ = false;
        if (link_.to != 0) {
            for (auto it = doc.links.begin(); it != doc.links.end(); ++it)
                if (it->to == link_.to && it->to_port == link_.to_port) {
                    replaced_ = *it;
                    had_replaced_ = true;
                    doc.links.erase(it);
                    break;
                }
        } else {
            // Output composites ONE contribution per owner layer (cross-
            // layer fan-in is the layer merge); a new chain end REPLACES
            // the same layer's old one — leaving it produced ghost wires
            // whose contribution double-composited the chain prefix.
            auto owner_of = [&](uint64_t id) -> uint64_t {
                for (const Layer& l : doc.layers) {
                    if (l.id == id) return l.id;
                    for (const EffectInstance& fx : l.stack)
                        if (fx.id == id) return l.id;
                }
                return 0;
            };
            const uint64_t own = owner_of(link_.from);
            for (auto it = doc.links.begin();
                 own && it != doc.links.end(); ++it)
                if (it->to == 0 && it->to_port == 0 &&
                    owner_of(it->from) == own) {
                    replaced_ = *it;
                    had_replaced_ = true;
                    doc.links.erase(it);
                    break;
                }
        }
        doc.links.push_back(link_);
    }

    void revert(Document& doc) override {
        for (auto it = doc.links.rbegin(); it != doc.links.rend(); ++it)
            if (it->from == link_.from && it->to == link_.to &&
                it->to_port == link_.to_port) {
                doc.links.erase(std::next(it).base());
                break;
            }
        if (had_replaced_) doc.links.push_back(replaced_);
        if (materialized_) doc.links.clear();
    }

private:
    Document::NodeLink link_;
    Document::NodeLink replaced_{};
    bool had_replaced_ = false;
    bool materialized_ = false;
};

class DisconnectCommand final : public Command {
public:
    explicit DisconnectCommand(Document::NodeLink link) : link_(link) {}
    std::string name() const override { return "Disconnect Nodes"; }

    void apply(Document& doc) override {
        materialized_ = doc.links.empty();
        ensure_links(doc);
        removed_ = false;
        for (auto it = doc.links.begin(); it != doc.links.end(); ++it)
            if (it->from == link_.from && it->to == link_.to &&
                it->to_port == link_.to_port) {
                doc.links.erase(it);
                removed_ = true;
                break;
            }
    }

    void revert(Document& doc) override {
        if (removed_) doc.links.push_back(link_);
        if (materialized_) doc.links.clear();
    }

private:
    Document::NodeLink link_;
    bool removed_ = false;
    bool materialized_ = false;
};

// Canvas frames (docs/flow_canvas.md v3): pure annotations, but still
// undoable like every mutation.
class AddFrameCommand final : public Command {
public:
    explicit AddFrameCommand(Document::Frame frame)
        : frame_(std::move(frame)) {}
    std::string name() const override { return "Add Frame"; }
    void apply(Document& doc) override { doc.frames.push_back(frame_); }
    void revert(Document& doc) override {
        for (auto it = doc.frames.begin(); it != doc.frames.end(); ++it)
            if (it->id == frame_.id) {
                doc.frames.erase(it);
                break;
            }
    }

private:
    Document::Frame frame_;
};

class RemoveFrameCommand final : public Command {
public:
    explicit RemoveFrameCommand(uint64_t id) : id_(id) {}
    std::string name() const override { return "Remove Frame"; }
    void apply(Document& doc) override {
        for (auto it = doc.frames.begin(); it != doc.frames.end(); ++it)
            if (it->id == id_) {
                removed_ = *it;
                had_ = true;
                doc.frames.erase(it);
                break;
            }
    }
    void revert(Document& doc) override {
        if (had_) doc.frames.push_back(removed_);
    }

private:
    uint64_t id_;
    Document::Frame removed_{};
    bool had_ = false;
};

// Frame resize drags coalesce per frame id (one undo step per gesture);
// rename is a single edit.
class SetFrameBoundsCommand final : public Command {
public:
    SetFrameBoundsCommand(uint64_t id, float w, float h)
        : id_(id), w_(w), h_(h) {}
    std::string name() const override { return "Resize Frame"; }
    void apply(Document& doc) override {
        for (Document::Frame& f : doc.frames)
            if (f.id == id_) {
                old_w_ = f.w;
                old_h_ = f.h;
                f.w = w_;
                f.h = h_;
                break;
            }
    }
    void revert(Document& doc) override {
        for (Document::Frame& f : doc.frames)
            if (f.id == id_) {
                f.w = old_w_;
                f.h = old_h_;
                break;
            }
    }
    bool merge(const Command& next) override {
        const auto* other =
            dynamic_cast<const SetFrameBoundsCommand*>(&next);
        if (!other || other->id_ != id_) return false;
        w_ = other->w_;
        h_ = other->h_;
        return true;
    }

private:
    uint64_t id_;
    float w_, h_;
    float old_w_ = 0.0f, old_h_ = 0.0f;
};

class SetFrameColorCommand final : public Command {
public:
    SetFrameColorCommand(uint64_t id, uint32_t color)
        : id_(id), color_(color) {}
    std::string name() const override { return "Tag Frame Colour"; }
    void apply(Document& doc) override {
        for (Document::Frame& f : doc.frames)
            if (f.id == id_) {
                old_color_ = f.color;
                f.color = color_;
                break;
            }
    }
    void revert(Document& doc) override {
        for (Document::Frame& f : doc.frames)
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

class SetFrameTitleCommand final : public Command {
public:
    SetFrameTitleCommand(uint64_t id, std::string title)
        : id_(id), title_(std::move(title)) {}
    std::string name() const override { return "Rename Frame"; }
    void apply(Document& doc) override {
        for (Document::Frame& f : doc.frames)
            if (f.id == id_) {
                old_title_ = f.title;
                f.title = title_;
                break;
            }
    }
    void revert(Document& doc) override {
        for (Document::Frame& f : doc.frames)
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

std::unique_ptr<Command> add_frame_command(Document::Frame frame) {
    return std::make_unique<AddFrameCommand>(std::move(frame));
}

std::unique_ptr<Command> set_frame_bounds_command(uint64_t frame_id, float w,
                                                  float h) {
    return std::make_unique<SetFrameBoundsCommand>(frame_id, w, h);
}

std::unique_ptr<Command> set_frame_title_command(uint64_t frame_id,
                                                 std::string title) {
    return std::make_unique<SetFrameTitleCommand>(frame_id, std::move(title));
}

std::unique_ptr<Command> set_effect_text_command(size_t layer_index,
                                                 size_t effect_index,
                                                 std::string text) {
    return std::make_unique<SetEffectTextCommand>(layer_index, effect_index,
                                                  std::move(text));
}

std::unique_ptr<Command> set_frame_color_command(uint64_t frame_id,
                                                 uint32_t color) {
    return std::make_unique<SetFrameColorCommand>(frame_id, color);
}

std::unique_ptr<Command> remove_frame_command(uint64_t frame_id) {
    return std::make_unique<RemoveFrameCommand>(frame_id);
}

bool link_would_cycle(const Document& doc, uint64_t from, uint64_t to) {
    if (from == to) return true;
    if (to == 0) return false;   // Output has no outgoing links
    const std::vector<Document::NodeLink> links =
        doc.links.empty() ? synthesize_links(doc) : doc.links;
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
        for (const Document::NodeLink& l : links)
            if (l.from == n && l.to != 0) stack.push_back(l.to);
    }
    return false;
}

std::unique_ptr<Command> connect_command(Document::NodeLink link) {
    return std::make_unique<ConnectCommand>(link);
}

std::unique_ptr<Command> disconnect_command(Document::NodeLink link) {
    return std::make_unique<DisconnectCommand>(link);
}

std::unique_ptr<Command> set_node_pos_command(NodeRef kind, uint64_t id,
                                              float x, float y) {
    return std::make_unique<SetNodePosCommand>(kind, id, x, y);
}

std::unique_ptr<Command> set_param_command(size_t layer_index,
                                           size_t effect_index,
                                           int param_index, float new_value) {
    return std::make_unique<SetParamCommand>(layer_index, effect_index,
                                             param_index, new_value);
}

std::unique_ptr<Command> set_bypass_command(size_t layer_index,
                                            size_t effect_index, bool bypass) {
    return std::make_unique<SetBypassCommand>(layer_index, effect_index,
                                              bypass);
}

std::unique_ptr<Command> set_solo_command(size_t layer_index,
                                          size_t effect_index, bool solo) {
    return std::make_unique<SetSoloCommand>(layer_index, effect_index, solo);
}

std::unique_ptr<Command> add_effect_command(size_t layer_index,
                                            EffectInstance instance,
                                            size_t insert_index) {
    return std::make_unique<AddEffectCommand>(layer_index, std::move(instance),
                                              insert_index);
}

std::unique_ptr<Command> remove_effect_command(size_t layer_index,
                                               size_t effect_index) {
    return std::make_unique<RemoveEffectCommand>(layer_index, effect_index);
}

std::unique_ptr<Command> move_effect_command(size_t layer_index,
                                             size_t from_index,
                                             size_t to_index) {
    return std::make_unique<MoveEffectCommand>(layer_index, from_index,
                                               to_index);
}

}  // namespace looks::doc

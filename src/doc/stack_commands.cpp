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
        new_value_ = other->new_value_;   // keep old_value_ for the undo
        return true;
    }

private:
    size_t layer_index_;
    size_t effect_index_;
    int param_index_;
    float new_value_;
    float old_value_ = 0.0f;
};

// Keyed params arrive as lane writes, unkeyed params as base writes.
class SetParamGestureCommand final : public LookCommand {
public:
    SetParamGestureCommand(uint64_t look, size_t layer_index,
                           size_t effect_index,
                           std::vector<ParamWrite> base_writes,
                           std::vector<KeyframeLane> lane_writes)
        : LookCommand(look), layer_index_(layer_index),
          effect_index_(effect_index), base_writes_(std::move(base_writes)),
          lane_writes_(std::move(lane_writes)) {}

    std::string name() const override { return "Edit Parameter"; }

    void apply(Document& doc) override {
        Look& look = look_of(doc);
        old_base_.clear();
        for (const ParamWrite& w : base_writes_) {
            float& p = param_ref(look, layer_index_, effect_index_,
                                 w.param_index);
            old_base_.push_back(p);
            p = w.value;
        }
        old_lanes_.clear();
        for (const KeyframeLane& lw : lane_writes_) {
            bool found = false;
            for (KeyframeLane& lane : look.lanes) {
                if (lane.target == lw.target) {
                    old_lanes_.push_back({true, lane.keys});
                    lane.keys = lw.keys;
                    found = true;
                    break;
                }
            }
            if (!found) {
                old_lanes_.push_back({false, {}});
                look.lanes.push_back({lw.target, lw.keys});
            }
        }
    }

    void revert(Document& doc) override {
        Look& look = look_of(doc);
        for (size_t i = lane_writes_.size(); i-- > 0;) {
            for (size_t li = 0; li < look.lanes.size(); ++li) {
                if (look.lanes[li].target == lane_writes_[i].target) {
                    if (old_lanes_[i].first)
                        look.lanes[li].keys = old_lanes_[i].second;
                    else
                        look.lanes.erase(look.lanes.begin() + li);
                    break;
                }
            }
        }
        for (size_t i = base_writes_.size(); i-- > 0;)
            param_ref(look, layer_index_, effect_index_,
                      base_writes_[i].param_index) = old_base_[i];
    }

    bool merge(const Command& next) override {
        const auto* o = dynamic_cast<const SetParamGestureCommand*>(&next);
        if (!o || !same_look(*o) || o->layer_index_ != layer_index_ ||
            o->effect_index_ != effect_index_ ||
            o->base_writes_.size() != base_writes_.size() ||
            o->lane_writes_.size() != lane_writes_.size())
            return false;
        for (size_t i = 0; i < base_writes_.size(); ++i)
            if (o->base_writes_[i].param_index != base_writes_[i].param_index)
                return false;
        for (size_t i = 0; i < lane_writes_.size(); ++i)
            if (!(o->lane_writes_[i].target == lane_writes_[i].target))
                return false;
        for (size_t i = 0; i < base_writes_.size(); ++i)
            base_writes_[i].value = o->base_writes_[i].value;
        for (size_t i = 0; i < lane_writes_.size(); ++i)
            lane_writes_[i].keys = o->lane_writes_[i].keys;
        return true;   // the old_* stashes stay at the gesture start
    }

private:
    size_t layer_index_;
    size_t effect_index_;
    std::vector<ParamWrite> base_writes_;
    std::vector<KeyframeLane> lane_writes_;
    std::vector<float> old_base_;
    // Pairs of {the lane was there, its keys before}, one per lane write.
    std::vector<std::pair<bool, std::vector<Keyframe>>> old_lanes_;
};

template <class T, T EffectInstance::*Field>
class SetEffectFieldCommand final : public LookCommand {
public:
    SetEffectFieldCommand(uint64_t look, size_t layer_index,
                          size_t effect_index, T value, const char* name)
        : LookCommand(look), layer_index_(layer_index),
          effect_index_(effect_index), value_(std::move(value)),
          name_(name) {}

    std::string name() const override { return name_; }

    void apply(Document& doc) override {
        auto& stack = stack_of(look_of(doc), layer_index_);
        assert(effect_index_ < stack.size());
        old_ = stack[effect_index_].*Field;
        stack[effect_index_].*Field = value_;
    }

    void revert(Document& doc) override {
        stack_of(look_of(doc), layer_index_)[effect_index_].*Field = old_;
    }

private:
    size_t layer_index_;
    size_t effect_index_;
    T value_;
    const char* name_;
    T old_{};
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

// A missing target is a silent no-op: the node can be gone in the history.
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
                if (CanvasFrame* f = find_frame(look, id_))
                    return {&f->x, &f->y};
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

// On a look with no layers this is a no-op: the first source auto-wires.
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

// Link order is stacking order: the first link of a port is the bottom.
// Keep positions exact. Only a new wire appends to the end.
class ConnectCommand final : public LookCommand {
public:
    ConnectCommand(uint64_t look, NodeLink link)
        : LookCommand(look), link_(link) {}
    std::string name() const override { return "Connect Nodes"; }

    void apply(Document& doc) override {
        Look& look = look_of(doc);
        materialized_ = look.links.empty();
        ensure_links(look);
        pruned_ = prune_tombstone(look);   // a real wire replaces the seal
        had_replaced_ = false;
        appended_ = false;
        auto replace_first = [&](auto&& match) {
            for (size_t i = 0; i < look.links.size(); ++i)
                if (match(look.links[i])) {
                    replaced_ = look.links[i];
                    replaced_at_ = i;
                    had_replaced_ = true;
                    look.links[i] = link_;
                    return;
                }
        };
        if (link_.to == 0) {
            // The Output keeps one contribution per owner layer, per port.
            // A chain that re-terminates replaces its old end in place.
            auto owner_of = [&](uint64_t id) -> uint64_t {
                if (find_layer(look, id)) return id;
                const Layer* owner = nullptr;
                if (find_effect(std::as_const(look), id, &owner))
                    return owner->id;
                return 0;
            };
            const uint64_t own = owner_of(link_.from);
            replace_first([&](const NodeLink& l) {
                return own && l.to == 0 && l.to_port == link_.to_port &&
                       owner_of(l.from) == own;
            });
        } else {
            // An exact duplicate replaces itself: a port never double-feeds.
            replace_first([&](const NodeLink& l) {
                return l.from == link_.from && l.to == link_.to &&
                       l.to_port == link_.to_port;
            });
        }
        if (!had_replaced_) {
            look.links.push_back(link_);
            appended_ = true;
        }
    }

    void revert(Document& doc) override {
        Look& look = look_of(doc);
        if (appended_) {
            erase_last_link(look, link_);
        } else if (had_replaced_ && replaced_at_ < look.links.size()) {
            look.links[replaced_at_] = replaced_;
        }
        if (pruned_) seal_links(look);
        if (materialized_) look.links.clear();
    }

private:
    NodeLink link_;
    NodeLink replaced_{};
    size_t replaced_at_ = 0;
    bool had_replaced_ = false;
    bool appended_ = false;
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
        for (size_t i = 0; i < look.links.size(); ++i)
            if (look.links[i].from == link_.from &&
                look.links[i].to == link_.to &&
                look.links[i].to_port == link_.to_port) {
                removed_at_ = i;
                look.links.erase(look.links.begin() +
                                 static_cast<ptrdiff_t>(i));
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
        if (removed_)
            look.links.insert(
                look.links.begin() +
                    static_cast<ptrdiff_t>(
                        std::min(removed_at_, look.links.size())),
                link_);
        if (materialized_) look.links.clear();
    }

private:
    NodeLink link_;
    size_t removed_at_ = 0;
    bool removed_ = false;
    bool materialized_ = false;
    bool sealed_ = false;
};

// If old_link is absent this appends. If new_link is there this only
// removes old_link.
class ReconnectCommand final : public LookCommand {
public:
    ReconnectCommand(uint64_t look, NodeLink old_link, NodeLink new_link)
        : LookCommand(look), old_(old_link), new_(new_link) {}
    std::string name() const override { return "Rewire Nodes"; }

    void apply(Document& doc) override {
        Look& look = look_of(doc);
        materialized_ = look.links.empty();
        ensure_links(look);
        pruned_ = prune_tombstone(look);
        mode_ = kAppended;
        size_t old_at = look.links.size();
        bool have_old = false;
        bool have_new = false;
        for (size_t i = 0; i < look.links.size(); ++i) {
            const NodeLink& l = look.links[i];
            if (!have_old && l.from == old_.from && l.to == old_.to &&
                l.to_port == old_.to_port) {
                old_at = i;
                have_old = true;
            }
            if (l.from == new_.from && l.to == new_.to &&
                l.to_port == new_.to_port)
                have_new = true;
        }
        if (have_old && have_new) {
            at_ = old_at;
            look.links.erase(look.links.begin() +
                             static_cast<ptrdiff_t>(old_at));
            mode_ = kRemovedOnly;
        } else if (have_old) {
            at_ = old_at;
            look.links[old_at] = new_;
            mode_ = kReplaced;
        } else if (!have_new) {
            look.links.push_back(new_);
            mode_ = kAppended;
        } else {
            mode_ = kNoop;
        }
    }

    void revert(Document& doc) override {
        Look& look = look_of(doc);
        switch (mode_) {
            case kReplaced:
                if (at_ < look.links.size()) look.links[at_] = old_;
                break;
            case kRemovedOnly:
                look.links.insert(
                    look.links.begin() +
                        static_cast<ptrdiff_t>(
                            std::min(at_, look.links.size())),
                    old_);
                break;
            case kAppended:
                erase_last_link(look, new_);
                break;
            case kNoop:
                break;
        }
        if (pruned_) seal_links(look);
        if (materialized_) look.links.clear();
    }

private:
    enum Mode { kReplaced, kRemovedOnly, kAppended, kNoop };
    NodeLink old_;
    NodeLink new_;
    size_t at_ = 0;
    Mode mode_ = kNoop;
    bool materialized_ = false;
    bool pruned_ = false;
};

// The swap is self-inverse, thus apply and revert do the same operation.
class MovePortLinkCommand final : public LookCommand {
public:
    MovePortLinkCommand(uint64_t look, uint64_t to, uint32_t to_port,
                        size_t index, int delta)
        : LookCommand(look), to_(to), port_(to_port), index_(index),
          delta_(delta) {}
    std::string name() const override { return "Reorder Feed"; }

    void apply(Document& doc) override { swap_links(doc); }
    void revert(Document& doc) override { swap_links(doc); }

private:
    void swap_links(Document& doc) {
        Look& look = look_of(doc);
        ensure_links(look);
        std::vector<size_t> pos;
        for (size_t i = 0; i < look.links.size(); ++i)
            if (look.links[i].to == to_ && look.links[i].to_port == port_)
                pos.push_back(i);
        const long other = static_cast<long>(index_) + delta_;
        if (index_ >= pos.size() || other < 0 ||
            static_cast<size_t>(other) >= pos.size())
            return;
        std::swap(look.links[pos[index_]],
                  look.links[pos[static_cast<size_t>(other)]]);
    }

    uint64_t to_;
    uint32_t port_;
    size_t index_;
    int delta_;
};

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

class SetFrameBoundsCommand final : public LookCommand {
public:
    SetFrameBoundsCommand(uint64_t look, uint64_t id, float w, float h)
        : LookCommand(look), id_(id), w_(w), h_(h) {}
    std::string name() const override { return "Resize Frame"; }
    void apply(Document& doc) override {
        if (CanvasFrame* f = find_frame(look_of(doc), id_)) {
            old_w_ = f->w;
            old_h_ = f->h;
            f->w = w_;
            f->h = h_;
        }
    }
    void revert(Document& doc) override {
        if (CanvasFrame* f = find_frame(look_of(doc), id_)) {
            f->w = old_w_;
            f->h = old_h_;
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

template <class T, T CanvasFrame::*Field>
class SetFrameFieldCommand final : public LookCommand {
public:
    SetFrameFieldCommand(uint64_t look, uint64_t id, T value,
                         const char* name)
        : LookCommand(look), id_(id), value_(std::move(value)), name_(name) {}
    std::string name() const override { return name_; }
    void apply(Document& doc) override {
        if (CanvasFrame* f = find_frame(look_of(doc), id_)) {
            old_ = f->*Field;
            f->*Field = value_;
        }
    }
    void revert(Document& doc) override {
        if (CanvasFrame* f = find_frame(look_of(doc), id_)) f->*Field = old_;
    }

private:
    uint64_t id_;
    T value_;
    const char* name_;
    T old_{};
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
    return std::make_unique<
        SetFrameFieldCommand<std::string, &CanvasFrame::title>>(
        look, frame_id, std::move(title), "Rename Frame");
}

std::unique_ptr<Command> set_effect_text_command(uint64_t look,
                                                 size_t layer_index,
                                                 size_t effect_index,
                                                 std::string text) {
    return std::make_unique<
        SetEffectFieldCommand<std::string, &EffectInstance::text>>(
        look, layer_index, effect_index, std::move(text), "Edit Text");
}

std::unique_ptr<Command> set_frame_color_command(uint64_t look,
                                                 uint64_t frame_id,
                                                 uint32_t color) {
    return std::make_unique<
        SetFrameFieldCommand<uint32_t, &CanvasFrame::color>>(
        look, frame_id, color, "Tag Frame Colour");
}

std::unique_ptr<Command> remove_frame_command(uint64_t look,
                                              uint64_t frame_id) {
    return std::make_unique<RemoveFrameCommand>(look, frame_id);
}

bool link_would_cycle(const Look& look, uint64_t from, uint64_t to) {
    if (from == to) return true;
    if (to == 0) return false;   // the Output node has no outgoing links
    std::vector<NodeLink> synth;
    const std::vector<NodeLink>& links = effective_links(look, synth);
    std::vector<uint64_t> stack{to};
    std::vector<uint64_t> seen;
    while (!stack.empty()) {
        const uint64_t n = stack.back();
        stack.pop_back();
        if (n == from) return true;
        bool visited = false;
        for (uint64_t s : seen)
            if (s == n) {
                visited = true;
                break;
            }
        if (visited) continue;
        seen.push_back(n);
        // A group id has no outgoing links. The walk continues at the face.
        if (const uint64_t face = group_face_member(look, n))
            stack.push_back(face);
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

std::unique_ptr<Command> reconnect_command(uint64_t look, NodeLink old_link,
                                           NodeLink new_link) {
    return std::make_unique<ReconnectCommand>(look, old_link, new_link);
}

std::unique_ptr<Command> move_port_link_command(uint64_t look, uint64_t to,
                                                uint32_t to_port,
                                                size_t index, int delta) {
    return std::make_unique<MovePortLinkCommand>(look, to, to_port, index,
                                                 delta);
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

std::unique_ptr<Command> set_param_gesture_command(
    uint64_t look, size_t layer_index, size_t effect_index,
    std::vector<ParamWrite> base_writes,
    std::vector<KeyframeLane> lane_writes) {
    return std::make_unique<SetParamGestureCommand>(
        look, layer_index, effect_index, std::move(base_writes),
        std::move(lane_writes));
}

std::unique_ptr<Command> set_bypass_command(uint64_t look, size_t layer_index,
                                            size_t effect_index, bool bypass) {
    return std::make_unique<SetEffectFieldCommand<bool, &EffectInstance::bypass>>(
        look, layer_index, effect_index, bypass,
        bypass ? "Bypass Effect" : "Enable Effect");
}

std::unique_ptr<Command> set_effect_blend_command(uint64_t look,
                                                  size_t layer_index,
                                                  size_t effect_index,
                                                  BlendMode blend) {
    return std::make_unique<
        SetEffectFieldCommand<BlendMode, &EffectInstance::blend>>(
        look, layer_index, effect_index, blend, "Effect Blend");
}

std::unique_ptr<Command> set_solo_command(uint64_t look, size_t layer_index,
                                          size_t effect_index, bool solo) {
    return std::make_unique<SetEffectFieldCommand<bool, &EffectInstance::solo>>(
        look, layer_index, effect_index, solo,
        solo ? "Solo Effect" : "Unsolo Effect");
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

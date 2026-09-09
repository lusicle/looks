#include "doc/stack_commands.h"

#include <cassert>
#include <utility>

#include "doc/effects.h"

namespace looks::doc {

std::unique_ptr<Command> set_generated_frame_command(uint64_t look, uint64_t effect,
                                                    std::string path, std::string signature) {
    class SetGenerated final : public LookCommand {
    public:
        SetGenerated(uint64_t look, uint64_t effect, std::string path, std::string signature)
            : LookCommand(look), effect_(effect), path_(std::move(path)), signature_(std::move(signature)) {}
        std::string name() const override { return "generate Mode"; }
        void apply(Document& doc) override {
            if (auto* fx = find_effect(entity_of(doc), effect_)) {
                old_path_ = fx->generated_path;
                old_signature_ = fx->generated_signature;
                fx->generated_path = path_;
                fx->generated_signature = signature_;
            }
        }
        void revert(Document& doc) override {
            if (auto* fx = find_effect(entity_of(doc), effect_)) {
                fx->generated_path = old_path_;
                fx->generated_signature = old_signature_;
            }
        }
    private:
        uint64_t effect_;
        std::string path_, signature_, old_path_, old_signature_;
    };
    return std::make_unique<SetGenerated>(look, effect, std::move(path), std::move(signature));
}

void NodeReferenceState::detach(Look& look, uint64_t node) {
    lanes = look.lanes;
    routes = look.mod_routes;
    for (size_t i = 0; i < 3; ++i) snapshots[i] = look.snapshots[i];
    std::vector<uint64_t> targets;
    if (const auto* source = find_source(look, node)) {
        targets.push_back(node | kSourceParamBit);
        for (const auto& stop : source->stops) targets.push_back(stop.id | kStopParamBit);
    } else if (find_group(look, node)) targets.push_back(node | kGroupParamBit);
    else targets.push_back(node);
    auto matches = [&](uint64_t id) {
        return std::find(targets.begin(), targets.end(), id) != targets.end();
    };
    look.lanes.erase(std::remove_if(look.lanes.begin(), look.lanes.end(),
        [&](const KeyframeLane& lane) { return matches(lane.target.effect_id); }), look.lanes.end());
    look.mod_routes.erase(std::remove_if(look.mod_routes.begin(), look.mod_routes.end(),
        [&](const ModRoute& route) { return matches(route.target.effect_id); }), look.mod_routes.end());
    for (auto& snapshot : look.snapshots)
        snapshot.entries.erase(std::remove_if(snapshot.entries.begin(), snapshot.entries.end(),
            [&](const SnapshotEntry& entry) { return matches(entry.effect_id); }), snapshot.entries.end());
    media_users.clear();
    for (auto& value : look.value_nodes)
        if (value.audio_src == node) {
            media_users.push_back(value.id);
            value.audio_src = 0;
        }
}

void NodeReferenceState::restore(Look& look, uint64_t node) const {
    look.lanes = lanes;
    look.mod_routes = routes;
    for (size_t i = 0; i < 3; ++i) look.snapshots[i] = snapshots[i];
    for (const auto id : media_users)
        if (auto* value = find_value_node(look, id)) value->audio_src = node;
}

namespace {

bool chain_pair(const Look& look, uint64_t first, uint64_t second) {
    const auto* a = find_effect(look, first);
    const auto* b = find_effect(look, second);
    if (!a || !b || a->group_id != b->group_id) return false;
    if (const auto* group = find_group(look, a->group_id))
        if (group->face_out == first) return false;
    size_t outputs = 0, inputs = 0;
    bool connected = false;
    for (const auto& link : look.links) {
        if (link.from == first) ++outputs;
        if (link.to == second && link.to_port == 0) ++inputs;
        if (link.from == first && link.to == second && link.to_port == 0) connected = true;
    }
    return connected && outputs == 1 && inputs == 1;
}

float* param_ref(Look& look, uint64_t effect_id, int param_index) {
    EffectInstance* target = find_effect(look, effect_id);
    if (!target) return nullptr;
    EffectInstance& fx = *target;
    if (param_index == kWetParam) return &fx.wet;
    if (param_index == kOpacityParam) return &fx.opacity;
    if (param_index < 0 || static_cast<size_t>(param_index) >= fx.params.size()) return nullptr;
    return &fx.params[static_cast<size_t>(param_index)];
}

class SetParamCommand final : public LookCommand {
public:
    SetParamCommand(uint64_t look, uint64_t effect_id,
                    int param_index, float new_value)
        : LookCommand(look), effect_id_(effect_id), param_index_(param_index),
          new_value_(new_value) {}

    std::string name() const override { return "Edit Parameter"; }

    void apply(Document& doc) override {
        float* p = param_ref(entity_of(doc), effect_id_, param_index_);
        applied_ = p != nullptr;
        if (!applied_) return;
        old_value_ = *p;
        *p = new_value_;
    }

    void revert(Document& doc) override {
        if (applied_)
            if (auto* p = param_ref(entity_of(doc), effect_id_, param_index_)) *p = old_value_;
    }

    bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetParamCommand*>(&next);
        if (!other || !applied_ || !other->applied_ || !same_entity(*other) ||
            other->effect_id_ != effect_id_ ||
            other->param_index_ != param_index_)
            return false;
        new_value_ = other->new_value_;   // keep old_value_ for the undo
        return true;
    }

private:
    uint64_t effect_id_;
    int param_index_;
    float new_value_;
    float old_value_ = 0.0f;
    bool applied_ = false;
};

// Keyed params arrive as lane writes, unkeyed params as base writes.
class SetParamGestureCommand final : public LookCommand {
public:
    SetParamGestureCommand(uint64_t look, uint64_t effect_id,
                           std::vector<ParamWrite> base_writes,
                           std::vector<KeyframeLane> lane_writes)
        : LookCommand(look), effect_id_(effect_id), base_writes_(std::move(base_writes)),
          lane_writes_(std::move(lane_writes)) {}

    std::string name() const override { return "Edit Parameter"; }

    void apply(Document& doc) override {
        Look& look = entity_of(doc);
        applied_ = false;
        if (!find_effect(look, effect_id_)) return;
        for (const auto& write : base_writes_)
            if (!param_ref(look, effect_id_, write.param_index)) return;
        for (const auto& lane : lane_writes_)
            if (lane.target.effect_id != effect_id_ || !param_ref(look, effect_id_, lane.target.param_index)) return;
        applied_ = true;
        old_base_.clear();
        for (const ParamWrite& w : base_writes_) {
            float& p = *param_ref(look, effect_id_, w.param_index);
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
        if (!applied_) return;
        Look& look = entity_of(doc);
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
            if (auto* p = param_ref(look, effect_id_, base_writes_[i].param_index)) *p = old_base_[i];
    }

    bool merge(const Command& next) override {
        const auto* o = dynamic_cast<const SetParamGestureCommand*>(&next);
        if (!o || !applied_ || !o->applied_ || !same_entity(*o) || o->effect_id_ != effect_id_ ||
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
    uint64_t effect_id_;
    std::vector<ParamWrite> base_writes_;
    std::vector<KeyframeLane> lane_writes_;
    std::vector<float> old_base_;
    bool applied_ = false;
    // Pairs of {the lane was there, its keys before}, one per lane write.
    std::vector<std::pair<bool, std::vector<Keyframe>>> old_lanes_;
};

template <class T, T EffectInstance::*Field>
class SetEffectFieldCommand final : public LookCommand {
public:
    SetEffectFieldCommand(uint64_t look, uint64_t effect_id, T value, const char* name)
        : LookCommand(look), effect_id_(effect_id), value_(std::move(value)),
          name_(name) {}

    std::string name() const override { return name_; }

    void apply(Document& doc) override {
        applied_ = false;
        if (auto* fx = find_effect(entity_of(doc), effect_id_)) {
            applied_ = true;
            old_ = fx->*Field;
            fx->*Field = value_;
        }
    }

    void revert(Document& doc) override {
        if (applied_)
            if (auto* fx = find_effect(entity_of(doc), effect_id_)) fx->*Field = old_;
    }

private:
    uint64_t effect_id_;
    T value_;
    const char* name_;
    T old_{};
    bool applied_ = false;
};

class AddEffectCommand final : public LookCommand {
public:
    AddEffectCommand(uint64_t look, EffectInstance instance,
                     size_t insert_index)
        : LookCommand(look), instance_(std::move(instance)), insert_index_(insert_index) {}

    std::string name() const override {
        return std::string("Add ") + effect_info(instance_.type).label;
    }

    void apply(Document& doc) override {
        auto& stack = entity_of(doc).effects;
        stack.insert(stack.begin() + std::min(insert_index_, stack.size()), instance_);
    }

    void revert(Document& doc) override {
        auto& stack = entity_of(doc).effects;
        stack.erase(std::remove_if(stack.begin(), stack.end(),
            [&](const EffectInstance& fx) { return fx.id == instance_.id; }), stack.end());
    }

private:
    EffectInstance instance_;
    size_t insert_index_;
};

class RemoveEffectCommand final : public LookCommand {
public:
    RemoveEffectCommand(uint64_t look, uint64_t effect_id)
        : LookCommand(look), effect_id_(effect_id) {}

    std::string name() const override { return "Remove Effect"; }

    void apply(Document& doc) override {
        Look& look = entity_of(doc);
        auto& stack = look.effects;
        const EffectInstance* fx = find_effect(look, effect_id_);
        applied_ = fx != nullptr;
        if (!applied_) return;
        effect_index_ = static_cast<size_t>(fx - stack.data());
        removed_ = stack[effect_index_];
        links_ = look.links;
        groups_ = look.groups;
        references_.detach(look, effect_id_);
        look.links.erase(std::remove_if(look.links.begin(), look.links.end(),
            [&](const NodeLink& link) { return link.from == effect_id_ || link.to == effect_id_; }),
            look.links.end());
        for (Group& group : look.groups) {
            group.exposed.erase(std::remove_if(group.exposed.begin(), group.exposed.end(),
                [&](const ParamKey& key) { return key.effect_id == effect_id_; }), group.exposed.end());
            if (group.face_out == effect_id_) group.face_out = 0;
        }
        stack.erase(stack.begin() + effect_index_);
    }

    void revert(Document& doc) override {
        if (!applied_) return;
        Look& look = entity_of(doc);
        auto& stack = look.effects;
        stack.insert(stack.begin() + effect_index_, removed_);
        look.links = links_;
        look.groups = groups_;
        references_.restore(look, effect_id_);
    }

private:
    uint64_t effect_id_;
    size_t effect_index_ = 0;
    bool applied_ = false;
    EffectInstance removed_;
    std::vector<NodeLink> links_;
    std::vector<Group> groups_;
    NodeReferenceState references_;
};

class MoveEffectCommand final : public LookCommand {
public:
    MoveEffectCommand(uint64_t look, uint64_t effect_id, uint64_t other_id)
        : LookCommand(look), from_(effect_id), to_(other_id) {}

    std::string name() const override { return "Move Effect"; }

    void apply(Document& doc) override {
        Look& look = entity_of(doc);
        old_links_ = look.links;
        old_groups_ = look.groups;
        uint64_t first = from_, second = to_;
        if (!chain_pair(look, first, second)) std::swap(first, second);
        if (!chain_pair(look, first, second)) return;
        for (NodeLink& link : look.links) {
            if (link.from == first && link.to == second && link.to_port == 0) {
                link.from = second;
                link.to = first;
            } else {
                if (link.to == first && link.to_port == 0) link.to = second;
                if (link.from == second) link.from = first;
            }
        }
        for (const auto& link : look.links)
            if (link_would_cycle(look, link.from, link.to)) {
                look.links = old_links_;
                return;
            }
        for (auto& group : look.groups)
            if (group.face_out == second) group.face_out = first;
    }
    void revert(Document& doc) override {
        entity_of(doc).links = old_links_;
        entity_of(doc).groups = old_groups_;
    }

private:
    uint64_t from_;
    uint64_t to_;
    std::vector<NodeLink> old_links_;
    std::vector<Group> old_groups_;
};

// A missing target is a silent no-op: the node can be gone in the history.
class SetNodePosCommand final : public LookCommand {
public:
    SetNodePosCommand(uint64_t look, NodeRef kind, uint64_t id, float x,
                      float y)
        : LookCommand(look), kind_(kind), id_(id), x_(x), y_(y) {}

    std::string name() const override { return "Move Node"; }

    void apply(Document& doc) override {
        const Pos p = resolve(entity_of(doc));
        if (!p.x) return;
        old_x_ = *p.x;
        old_y_ = *p.y;
        *p.x = x_;
        *p.y = y_;
    }

    void revert(Document& doc) override {
        const Pos p = resolve(entity_of(doc));
        if (!p.x) return;
        *p.x = old_x_;
        *p.y = old_y_;
    }

    bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetNodePosCommand*>(&next);
        if (!other || !same_entity(*other) || other->kind_ != kind_ ||
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
            case NodeRef::Source:
                if (Source* l = find_source(look, id_))
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
                for (Group& g : look.groups)
                        if (g.id == id_) return {&g.node_x, &g.node_y};
                return {};
            case NodeRef::GroupIn:
                for (Group& g : look.groups)
                        if (g.id == id_) return {&g.in_x, &g.in_y};
                return {};
            case NodeRef::GroupOut:
                for (Group& g : look.groups)
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

// Link order is stacking order: the first link of a port is the bottom.
// Keep positions exact. Only a new wire appends to the end.
class ConnectCommand final : public LookCommand {
public:
    ConnectCommand(uint64_t look, NodeLink link)
        : LookCommand(look), link_(link) {}
    std::string name() const override { return "Connect Nodes"; }

    void apply(Document& doc) override {
        Look& look = entity_of(doc);
        appended_ = std::none_of(look.links.begin(), look.links.end(),
            [&](const NodeLink& link) { return link.same_endpoints(link_); });
        if (appended_) look.links.push_back(link_);
    }

    void revert(Document& doc) override {
        if (appended_) erase_last_link(entity_of(doc), link_);
    }

private:
    NodeLink link_;
    bool appended_ = false;
};

class DisconnectCommand final : public LookCommand {
public:
    DisconnectCommand(uint64_t look, NodeLink link)
        : LookCommand(look), link_(link) {}
    std::string name() const override { return "Disconnect Nodes"; }

    void apply(Document& doc) override {
        Look& look = entity_of(doc);
        removed_ = false;
        for (size_t i = 0; i < look.links.size(); ++i)
            if (look.links[i].from == link_.from &&
                look.links[i].to == link_.to &&
                look.links[i].to_port == link_.to_port) {
                removed_at_ = i;
                link_ = look.links[i];
                look.links.erase(look.links.begin() +
                                 static_cast<ptrdiff_t>(i));
                removed_ = true;
                break;
            }
    }

    void revert(Document& doc) override {
        Look& look = entity_of(doc);
        if (removed_)
            look.links.insert(
                look.links.begin() +
                    static_cast<ptrdiff_t>(
                        std::min(removed_at_, look.links.size())),
                link_);
    }

private:
    NodeLink link_;
    size_t removed_at_ = 0;
    bool removed_ = false;
};

// If old_link is absent this appends. If new_link is there this only
// removes old_link.
class ReconnectCommand final : public LookCommand {
public:
    ReconnectCommand(uint64_t look, NodeLink old_link, NodeLink new_link)
        : LookCommand(look), old_(old_link), new_(new_link) {}
    std::string name() const override { return "Rewire Nodes"; }

    void apply(Document& doc) override {
        Look& look = entity_of(doc);
        mode_ = kAppended;
        if (old_.from == new_.from && old_.to == new_.to && old_.to_port == new_.to_port) {
            mode_ = kNoop;
            return;
        }
        size_t old_at = look.links.size();
        bool have_old = false;
        bool have_new = false;
        for (size_t i = 0; i < look.links.size(); ++i) {
            const NodeLink& l = look.links[i];
            if (!have_old && l.from == old_.from && l.to == old_.to &&
                l.to_port == old_.to_port) {
                old_at = i;
                have_old = true;
                old_ = l;
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
            new_.blend = old_.blend;
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
        Look& look = entity_of(doc);
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
    }

private:
    enum Mode { kReplaced, kRemovedOnly, kAppended, kNoop };
    NodeLink old_;
    NodeLink new_;
    size_t at_ = 0;
    Mode mode_ = kNoop;
};

class SetLinkBlendCommand final : public LookCommand {
public:
    SetLinkBlendCommand(uint64_t look, NodeLink link, BlendMode blend)
        : LookCommand(look), link_(link), blend_(blend) {}
    std::string name() const override { return "Edit Connection Blend"; }
    void apply(Document& doc) override {
        applied_ = false;
        if (static_cast<uint32_t>(blend_) > static_cast<uint32_t>(BlendMode::Difference)) return;
        for (auto& link : entity_of(doc).links)
            if (link.same_endpoints(link_)) {
                old_ = link.blend;
                link.blend = blend_;
                applied_ = true;
                break;
            }
    }
    void revert(Document& doc) override {
        if (!applied_) return;
        for (auto& link : entity_of(doc).links)
            if (link.same_endpoints(link_)) { link.blend = old_; break; }
    }
private:
    NodeLink link_;
    BlendMode blend_, old_ = BlendMode::Normal;
    bool applied_ = false;
};

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
        Look& look = entity_of(doc);
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
        entity_of(doc).frames.push_back(frame_);
    }
    void revert(Document& doc) override {
        Look& look = entity_of(doc);
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
        Look& look = entity_of(doc);
        for (auto it = look.frames.begin(); it != look.frames.end(); ++it)
            if (it->id == id_) {
                removed_ = *it;
                had_ = true;
                look.frames.erase(it);
                break;
            }
    }
    void revert(Document& doc) override {
        if (had_) entity_of(doc).frames.push_back(removed_);
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
        if (CanvasFrame* f = find_frame(entity_of(doc), id_)) {
            old_w_ = f->w;
            old_h_ = f->h;
            f->w = w_;
            f->h = h_;
        }
    }
    void revert(Document& doc) override {
        if (CanvasFrame* f = find_frame(entity_of(doc), id_)) {
            f->w = old_w_;
            f->h = old_h_;
        }
    }
    bool merge(const Command& next) override {
        const auto* other =
            dynamic_cast<const SetFrameBoundsCommand*>(&next);
        if (!other || !same_entity(*other) || other->id_ != id_) return false;
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
        if (CanvasFrame* f = find_frame(entity_of(doc), id_)) {
            old_ = f->*Field;
            f->*Field = value_;
        }
    }
    void revert(Document& doc) override {
        if (CanvasFrame* f = find_frame(entity_of(doc), id_)) f->*Field = old_;
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
                                                 uint64_t effect_id,
                                                 std::string text) {
    return std::make_unique<
        SetEffectFieldCommand<std::string, &EffectInstance::text>>(
        look, effect_id, std::move(text), "Edit Text");
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
    const std::vector<NodeLink>& links = (look).links;
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

uint64_t effect_chain_neighbor(const Look& look, uint64_t effect_id, int direction) {
    for (const auto& link : look.links) {
        if (link.to_port != 0) continue;
        if (direction < 0 && link.to == effect_id && chain_pair(look, link.from, effect_id))
            return link.from;
        if (direction > 0 && link.from == effect_id && chain_pair(look, effect_id, link.to))
            return link.to;
    }
    return 0;
}

std::unique_ptr<Command> connect_command(uint64_t look, NodeLink link) {
    return std::make_unique<ConnectCommand>(look, link);
}

std::unique_ptr<Command> disconnect_command(uint64_t look, NodeLink link) {
    return std::make_unique<DisconnectCommand>(look, link);
}

std::unique_ptr<Command> set_link_blend_command(uint64_t look, NodeLink link, BlendMode blend) {
    return std::make_unique<SetLinkBlendCommand>(look, link, blend);
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

std::unique_ptr<Command> set_param_command(uint64_t look, uint64_t effect_id,
                                           int param_index, float new_value) {
    return std::make_unique<SetParamCommand>(look, effect_id,
                                             param_index, new_value);
}

std::unique_ptr<Command> set_param_gesture_command(
    uint64_t look, uint64_t effect_id,
    std::vector<ParamWrite> base_writes,
    std::vector<KeyframeLane> lane_writes) {
    return std::make_unique<SetParamGestureCommand>(
        look, effect_id, std::move(base_writes),
        std::move(lane_writes));
}

std::unique_ptr<Command> set_bypass_command(uint64_t look, uint64_t effect_id, bool bypass) {
    return std::make_unique<SetEffectFieldCommand<bool, &EffectInstance::bypass>>(
        look, effect_id, bypass,
        bypass ? "Bypass Effect" : "Enable Effect");
}

std::unique_ptr<Command> set_effect_blend_command(uint64_t look,
                                                  uint64_t effect_id,
                                                  BlendMode blend) {
    return std::make_unique<
        SetEffectFieldCommand<BlendMode, &EffectInstance::blend>>(
        look, effect_id, blend, "Effect Blend");
}

std::unique_ptr<Command> set_solo_command(uint64_t look, uint64_t effect_id, bool solo) {
    return std::make_unique<SetEffectFieldCommand<bool, &EffectInstance::solo>>(
        look, effect_id, solo,
        solo ? "Solo Effect" : "Unsolo Effect");
}

std::unique_ptr<Command> add_effect_command(uint64_t look, EffectInstance instance,
                                            size_t insert_index) {
    return std::make_unique<AddEffectCommand>(look, std::move(instance),
                                              insert_index);
}

std::unique_ptr<Command> remove_effect_command(uint64_t look,
                                               uint64_t effect_id) {
    return std::make_unique<RemoveEffectCommand>(look, effect_id);
}

std::unique_ptr<Command> move_effect_command(uint64_t look, uint64_t effect_id,
                                             uint64_t other_id) {
    return std::make_unique<MoveEffectCommand>(look, effect_id, other_id);
}

}  // namespace looks::doc

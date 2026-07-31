#include "doc/group_commands.h"

#include <algorithm>
#include <cassert>
#include <utility>

namespace looks::doc {

namespace {

Group* find_group(Layer& layer, uint64_t group_id) {
    for (Group& g : layer.groups)
        if (g.id == group_id) return &g;
    return nullptr;
}

class GroupEffectsCommand final : public Command {
public:
    GroupEffectsCommand(size_t layer_index, Group group, size_t from,
                        size_t to)
        : layer_index_(layer_index), group_(std::move(group)), from_(from),
          to_(to) {}
    std::string name() const override { return "Group Effects"; }

    void apply(Document& doc) override {
        Layer& layer = doc.layers[layer_index_];
        assert(from_ <= to_ && to_ < layer.stack.size());
        old_group_ids_.clear();
        for (size_t i = from_; i <= to_; ++i) {
            old_group_ids_.push_back(layer.stack[i].group_id);
            layer.stack[i].group_id = group_.id;
        }
        // Boundary bindings default to the span's ends (v5.3).
        if (group_.face_in == 0) group_.face_in = layer.stack[from_].id;
        if (group_.face_out == 0) group_.face_out = layer.stack[to_].id;
        layer.groups.push_back(group_);
    }

    void revert(Document& doc) override {
        Layer& layer = doc.layers[layer_index_];
        for (size_t i = from_; i <= to_; ++i)
            layer.stack[i].group_id = old_group_ids_[i - from_];
        layer.groups.pop_back();
    }

private:
    size_t layer_index_;
    Group group_;
    size_t from_, to_;
    std::vector<uint64_t> old_group_ids_;
};

class UngroupCommand final : public Command {
public:
    UngroupCommand(size_t layer_index, uint64_t group_id)
        : layer_index_(layer_index), group_id_(group_id) {}
    std::string name() const override { return "Ungroup"; }

    void apply(Document& doc) override {
        Layer& layer = doc.layers[layer_index_];
        members_.clear();
        for (size_t i = 0; i < layer.stack.size(); ++i)
            if (layer.stack[i].group_id == group_id_) {
                members_.push_back(i);
                layer.stack[i].group_id = 0;
            }
        for (size_t i = 0; i < layer.groups.size(); ++i)
            if (layer.groups[i].id == group_id_) {
                group_slot_ = i;
                removed_ = layer.groups[i];
                layer.groups.erase(layer.groups.begin() +
                                   static_cast<ptrdiff_t>(i));
                break;
            }
    }

    void revert(Document& doc) override {
        Layer& layer = doc.layers[layer_index_];
        for (size_t i : members_) layer.stack[i].group_id = group_id_;
        layer.groups.insert(layer.groups.begin() +
                                static_cast<ptrdiff_t>(group_slot_),
                            removed_);
    }

private:
    size_t layer_index_;
    uint64_t group_id_;
    std::vector<size_t> members_;
    size_t group_slot_ = 0;
    Group removed_;
};

class SetEffectGroupCommand final : public Command {
public:
    SetEffectGroupCommand(size_t layer_index, size_t effect_index,
                          uint64_t group_id)
        : layer_index_(layer_index), effect_index_(effect_index),
          group_id_(group_id) {}
    std::string name() const override {
        return group_id_ ? "Join Group" : "Leave Group";
    }

    void apply(Document& doc) override {
        EffectInstance& fx = doc.layers[layer_index_].stack[effect_index_];
        old_group_id_ = fx.group_id;
        fx.group_id = group_id_;
    }

    void revert(Document& doc) override {
        doc.layers[layer_index_].stack[effect_index_].group_id = old_group_id_;
    }

private:
    size_t layer_index_;
    size_t effect_index_;
    uint64_t group_id_;
    uint64_t old_group_id_ = 0;
};

class SetGroupPropsCommand final : public Command {
public:
    SetGroupPropsCommand(size_t layer_index, Group updated)
        : layer_index_(layer_index), updated_(std::move(updated)) {}
    std::string name() const override { return "Edit Group"; }

    void apply(Document& doc) override {
        Group* g = find_group(doc.layers[layer_index_], updated_.id);
        assert(g);
        old_ = *g;
        *g = updated_;
    }

    void revert(Document& doc) override {
        if (Group* g = find_group(doc.layers[layer_index_], updated_.id))
            *g = old_;
    }

    bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetGroupPropsCommand*>(&next);
        if (!other || other->layer_index_ != layer_index_ ||
            other->updated_.id != updated_.id)
            return false;
        updated_ = other->updated_;
        return true;
    }

private:
    size_t layer_index_;
    Group updated_;
    Group old_;
};

// The group FACE (texed expose): toggle one member param's presence in
// Group::exposed. Symmetric add/remove keeps position on re-add simple —
// re-exposure appends (face order = expose order).
class SetGroupExposedCommand final : public Command {
public:
    SetGroupExposedCommand(size_t layer_index, uint64_t group_id,
                           ParamKey key, bool exposed)
        : layer_index_(layer_index), group_id_(group_id), key_(key),
          exposed_(exposed) {}
    std::string name() const override {
        return exposed_ ? "Expose Param" : "Hide Param";
    }

    void apply(Document& doc) override {
        Group* g = find_group(doc.layers[layer_index_], group_id_);
        if (!g) return;
        auto it = std::find(g->exposed.begin(), g->exposed.end(), key_);
        did_ = false;
        if (exposed_ && it == g->exposed.end()) {
            g->exposed.push_back(key_);
            did_ = true;
        } else if (!exposed_ && it != g->exposed.end()) {
            removed_at_ = static_cast<size_t>(it - g->exposed.begin());
            g->exposed.erase(it);
            did_ = true;
        }
    }

    void revert(Document& doc) override {
        Group* g = find_group(doc.layers[layer_index_], group_id_);
        if (!g || !did_) return;
        if (exposed_) {
            auto it =
                std::find(g->exposed.begin(), g->exposed.end(), key_);
            if (it != g->exposed.end()) g->exposed.erase(it);
        } else {
            g->exposed.insert(g->exposed.begin() +
                                  static_cast<ptrdiff_t>(std::min(
                                      removed_at_, g->exposed.size())),
                              key_);
        }
    }

private:
    size_t layer_index_;
    uint64_t group_id_;
    ParamKey key_;
    bool exposed_;
    bool did_ = false;
    size_t removed_at_ = 0;
};

class InsertGroupCommand final : public Command {
public:
    InsertGroupCommand(size_t layer_index, Group group,
                       std::vector<EffectInstance> effects)
        : layer_index_(layer_index), group_(std::move(group)),
          effects_(std::move(effects)) {}
    std::string name() const override { return "Add Preset"; }

    void apply(Document& doc) override {
        Layer& layer = doc.layers[layer_index_];
        insert_at_ = layer.stack.size();
        layer.stack.insert(layer.stack.end(), effects_.begin(),
                           effects_.end());
        layer.groups.push_back(group_);
        // A materialized link table gets the wiring stack-order synthesis
        // would have produced: the internal chain among the members, and
        // the group spliced between the layer's old chain end and the
        // Output. Without this the preset landed fully DORMANT (unwired
        // inputs process nothing) the moment any wire had been edited.
        added_links_.clear();
        had_replaced_ = false;
        if (!doc.links.empty() && !effects_.empty()) {
            for (size_t i = 0; i + 1 < effects_.size(); ++i)
                added_links_.push_back(
                    {effects_[i].id, effects_[i + 1].id, 0});
            for (auto it = doc.links.begin(); it != doc.links.end(); ++it)
                if (it->to == 0 && it->to_port == 0 &&
                    link_owner(doc, it->from) == layer.id) {
                    replaced_ = *it;
                    had_replaced_ = true;
                    doc.links.erase(it);
                    break;
                }
            if (had_replaced_)
                added_links_.push_back(
                    {replaced_.from, effects_.front().id, 0});
            added_links_.push_back({effects_.back().id, 0, 0});
            doc.links.insert(doc.links.end(), added_links_.begin(),
                             added_links_.end());
        }
    }

    void revert(Document& doc) override {
        Layer& layer = doc.layers[layer_index_];
        layer.stack.erase(layer.stack.begin() +
                              static_cast<ptrdiff_t>(insert_at_),
                          layer.stack.begin() + static_cast<ptrdiff_t>(
                                                    insert_at_ +
                                                    effects_.size()));
        layer.groups.pop_back();
        for (const Document::NodeLink& added : added_links_)
            for (auto it = doc.links.rbegin(); it != doc.links.rend(); ++it)
                if (it->from == added.from && it->to == added.to &&
                    it->to_port == added.to_port) {
                    doc.links.erase(std::next(it).base());
                    break;
                }
        if (had_replaced_) doc.links.push_back(replaced_);
    }

private:
    // Owner layer of a link endpoint (layer id itself or a stack member).
    static uint64_t link_owner(const Document& doc, uint64_t id) {
        for (const Layer& l : doc.layers) {
            if (l.id == id) return l.id;
            for (const EffectInstance& fx : l.stack)
                if (fx.id == id) return l.id;
        }
        return 0;
    }

    size_t layer_index_;
    Group group_;
    std::vector<EffectInstance> effects_;
    size_t insert_at_ = 0;
    std::vector<Document::NodeLink> added_links_;
    Document::NodeLink replaced_{};
    bool had_replaced_ = false;
};

}  // namespace

Group make_group(Document& doc, std::string name) {
    Group g;
    g.id = doc.next_effect_id++;
    g.name = std::move(name);
    return g;
}

std::unique_ptr<Command> group_effects_command(size_t layer_index, Group group,
                                               size_t from, size_t to) {
    return std::make_unique<GroupEffectsCommand>(layer_index,
                                                 std::move(group), from, to);
}

std::unique_ptr<Command> ungroup_command(size_t layer_index,
                                         uint64_t group_id) {
    return std::make_unique<UngroupCommand>(layer_index, group_id);
}

std::unique_ptr<Command> set_effect_group_command(size_t layer_index,
                                                  size_t effect_index,
                                                  uint64_t group_id) {
    return std::make_unique<SetEffectGroupCommand>(layer_index, effect_index,
                                                   group_id);
}

std::unique_ptr<Command> set_group_props_command(size_t layer_index,
                                                 Group updated) {
    return std::make_unique<SetGroupPropsCommand>(layer_index,
                                                  std::move(updated));
}

std::unique_ptr<Command> set_group_exposed_command(size_t layer_index,
                                                   uint64_t group_id,
                                                   ParamKey key,
                                                   bool exposed) {
    return std::make_unique<SetGroupExposedCommand>(layer_index, group_id,
                                                    key, exposed);
}

std::unique_ptr<Command> insert_group_command(
    size_t layer_index, Group group, std::vector<EffectInstance> effects) {
    return std::make_unique<InsertGroupCommand>(layer_index, std::move(group),
                                                std::move(effects));
}

}  // namespace looks::doc

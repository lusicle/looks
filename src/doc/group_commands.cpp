#include "doc/group_commands.h"

#include <algorithm>
#include <cassert>
#include <utility>

namespace looks::doc {

namespace {

Group* group_in(Look& look, size_t layer_index, uint64_t group_id) {
    if (layer_index >= look.layers.size()) return nullptr;
    return find_group(look.layers[layer_index], group_id);
}

class GroupEffectsCommand final : public LookCommand {
public:
    GroupEffectsCommand(uint64_t look, size_t layer_index, Group group,
                        size_t from, size_t to)
        : LookCommand(look), layer_index_(layer_index),
          group_(std::move(group)), from_(from), to_(to) {}
    std::string name() const override { return "Group Effects"; }

    void apply(Document& doc) override {
        Look& look = look_of(doc);
        Layer& layer = look.layers[layer_index_];
        assert(from_ <= to_ && to_ < layer.stack.size());
        old_group_ids_.clear();
        for (size_t i = from_; i <= to_; ++i) {
            old_group_ids_.push_back(layer.stack[i].group_id);
            layer.stack[i].group_id = group_.id;
        }
        // face_out defaults to the last effect of the span.
        // Slot ids mint on the first apply and replay on redo.
        if (group_.face_out == 0) group_.face_out = layer.stack[to_].id;
        old_links_ = look.links;
        if (!planned_) {
            normalize_group_inputs(doc, look, layer_index_, group_,
                                   layer.stack[from_].id);
            planned_ = true;
            new_links_ = look.links;
        } else {
            look.links = new_links_;
        }
        layer.groups.push_back(group_);
    }

    void revert(Document& doc) override {
        Look& look = look_of(doc);
        Layer& layer = look.layers[layer_index_];
        for (size_t i = from_; i <= to_; ++i)
            layer.stack[i].group_id = old_group_ids_[i - from_];
        layer.groups.pop_back();
        look.links = old_links_;
    }

private:
    size_t layer_index_;
    Group group_;
    size_t from_, to_;
    std::vector<uint64_t> old_group_ids_;
    std::vector<NodeLink> old_links_;
    std::vector<NodeLink> new_links_;
    bool planned_ = false;
};

class UngroupCommand final : public LookCommand {
public:
    UngroupCommand(uint64_t look, size_t layer_index, uint64_t group_id)
        : LookCommand(look), layer_index_(layer_index), group_id_(group_id) {}
    std::string name() const override { return "Ungroup"; }

    void apply(Document& doc) override {
        Look& look = look_of(doc);
        Layer& layer = look.layers[layer_index_];
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
        // Splice the slots out: the producers wire direct to the members.
        // The matte wire on port 1 goes away with the group card.
        old_links_ = look.links;
        auto is_slot = [&](uint64_t id) {
            for (uint64_t s : removed_.inputs)
                if (s == id) return true;
            return false;
        };
        std::vector<NodeLink> next;
        for (const NodeLink& l : look.links) {
            if (is_slot(l.to) || (l.to == group_id_ && l.to_port == 1))
                continue;   // the exterior side comes back in the splice
            if (is_slot(l.from)) {
                for (const NodeLink& e : look.links)
                    if (e.to == l.from && e.to_port == 0)
                        next.push_back({e.from, l.to, l.to_port});
                continue;
            }
            next.push_back(l);
        }
        look.links = std::move(next);
    }

    void revert(Document& doc) override {
        Look& look = look_of(doc);
        Layer& layer = look.layers[layer_index_];
        for (size_t i : members_) layer.stack[i].group_id = group_id_;
        layer.groups.insert(layer.groups.begin() +
                                static_cast<ptrdiff_t>(group_slot_),
                            removed_);
        look.links = old_links_;
    }

private:
    size_t layer_index_;
    uint64_t group_id_;
    std::vector<size_t> members_;
    size_t group_slot_ = 0;
    Group removed_;
    std::vector<NodeLink> old_links_;
};

class SetEffectGroupCommand final : public LookCommand {
public:
    SetEffectGroupCommand(uint64_t look, size_t layer_index,
                          size_t effect_index, uint64_t group_id)
        : LookCommand(look), layer_index_(layer_index),
          effect_index_(effect_index), group_id_(group_id) {}
    std::string name() const override {
        return group_id_ ? "Join Group" : "Leave Group";
    }

    void apply(Document& doc) override {
        EffectInstance& fx =
            look_of(doc).layers[layer_index_].stack[effect_index_];
        old_group_id_ = fx.group_id;
        fx.group_id = group_id_;
    }

    void revert(Document& doc) override {
        look_of(doc).layers[layer_index_].stack[effect_index_].group_id =
            old_group_id_;
    }

private:
    size_t layer_index_;
    size_t effect_index_;
    uint64_t group_id_;
    uint64_t old_group_id_ = 0;
};

class SetGroupPropsCommand final : public LookCommand {
public:
    SetGroupPropsCommand(uint64_t look, size_t layer_index, Group updated)
        : LookCommand(look), layer_index_(layer_index),
          updated_(std::move(updated)) {}
    std::string name() const override { return "Edit Group"; }

    void apply(Document& doc) override {
        Look& look = look_of(doc);
        Group* g = group_in(look, layer_index_, updated_.id);
        if (!g) return;
        old_ = *g;
        *g = updated_;
    }

    void revert(Document& doc) override {
        Look& look = look_of(doc);
        if (Group* g = group_in(look, layer_index_, updated_.id))
            *g = old_;
    }

    bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetGroupPropsCommand*>(&next);
        if (!other || !same_look(*other) ||
            other->layer_index_ != layer_index_ ||
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

// Face order is expose order: a param that comes back goes to the end.
class SetGroupExposedCommand final : public LookCommand {
public:
    SetGroupExposedCommand(uint64_t look, size_t layer_index,
                           uint64_t group_id, ParamKey key, bool exposed)
        : LookCommand(look), layer_index_(layer_index), group_id_(group_id),
          key_(key), exposed_(exposed) {}
    std::string name() const override {
        return exposed_ ? "Expose Param" : "Hide Param";
    }

    void apply(Document& doc) override {
        Group* g = find_group(look_of(doc).layers[layer_index_], group_id_);
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
        Group* g = find_group(look_of(doc).layers[layer_index_], group_id_);
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

class InsertGroupCommand final : public LookCommand {
public:
    InsertGroupCommand(uint64_t look, size_t layer_index, Group group,
                       std::vector<EffectInstance> effects, uint64_t face_in)
        : LookCommand(look), layer_index_(layer_index),
          group_(std::move(group)), effects_(std::move(effects)),
          face_in_(face_in) {}
    std::string name() const override { return "Add Preset"; }

    void apply(Document& doc) override {
        // Materialize first, or stack-order wiring chains the new effects.
        // The members chain internally only. The group card lands unwired.
        Look& look = look_of(doc);
        materialized_ = look.links.empty();
        ensure_links(look);
        Layer& layer = look.layers[layer_index_];
        insert_at_ = layer.stack.size();
        layer.stack.insert(layer.stack.end(), effects_.begin(),
                           effects_.end());
        // The slot id mints one time and replays on redo.
        if (group_.inputs.empty() && !effects_.empty()) {
            if (!slot_) slot_ = doc.next_effect_id++;
            group_.inputs.push_back(slot_);
        }
        layer.groups.push_back(group_);
        added_links_.clear();
        for (size_t i = 0; i + 1 < effects_.size(); ++i)
            added_links_.push_back(
                {effects_[i].id, effects_[i + 1].id, 0});
        if (slot_) {
            uint64_t in_member = effects_.front().id;
            for (const EffectInstance& fx : effects_)
                if (fx.id == face_in_) in_member = fx.id;
            added_links_.push_back({slot_, in_member, 0});
        }
        look.links.insert(look.links.end(), added_links_.begin(),
                          added_links_.end());
    }

    void revert(Document& doc) override {
        Look& look = look_of(doc);
        Layer& layer = look.layers[layer_index_];
        layer.stack.erase(layer.stack.begin() +
                              static_cast<ptrdiff_t>(insert_at_),
                          layer.stack.begin() + static_cast<ptrdiff_t>(
                                                    insert_at_ +
                                                    effects_.size()));
        layer.groups.pop_back();
        for (const NodeLink& added : added_links_) erase_last_link(look, added);
        if (materialized_) look.links.clear();
    }

private:
    size_t layer_index_;
    Group group_;
    std::vector<EffectInstance> effects_;
    uint64_t face_in_ = 0;
    uint64_t slot_ = 0;
    size_t insert_at_ = 0;
    std::vector<NodeLink> added_links_;
    bool materialized_ = false;
};

class AddGroupInputCommand final : public LookCommand {
public:
    AddGroupInputCommand(uint64_t look, size_t layer_index, uint64_t group_id,
                         uint64_t slot_id)
        : LookCommand(look), layer_index_(layer_index), group_id_(group_id),
          slot_id_(slot_id) {}
    std::string name() const override { return "Add Group Input"; }

    void apply(Document& doc) override {
        Look& look = look_of(doc);
        if (Group* g = group_in(look, layer_index_, group_id_))
            g->inputs.push_back(slot_id_);
    }

    void revert(Document& doc) override {
        Look& look = look_of(doc);
        if (Group* g = group_in(look, layer_index_, group_id_))
            if (!g->inputs.empty() && g->inputs.back() == slot_id_)
                g->inputs.pop_back();
    }

private:
    size_t layer_index_;
    uint64_t group_id_;
    uint64_t slot_id_;
};

class RemoveGroupInputCommand final : public LookCommand {
public:
    RemoveGroupInputCommand(uint64_t look, size_t layer_index,
                            uint64_t group_id, uint64_t slot_id)
        : LookCommand(look), layer_index_(layer_index), group_id_(group_id),
          slot_id_(slot_id) {}
    std::string name() const override { return "Remove Group Input"; }

    void apply(Document& doc) override {
        Look& look = look_of(doc);
        Group* g = group_in(look, layer_index_, group_id_);
        if (!g) return;
        removed_at_ = g->inputs.size();
        for (size_t i = 0; i < g->inputs.size(); ++i)
            if (g->inputs[i] == slot_id_) {
                removed_at_ = i;
                g->inputs.erase(g->inputs.begin() +
                                static_cast<ptrdiff_t>(i));
                break;
            }
        old_links_ = look.links;
        look.links.erase(
            std::remove_if(look.links.begin(), look.links.end(),
                           [&](const NodeLink& l) {
                               return l.from == slot_id_ || l.to == slot_id_;
                           }),
            look.links.end());
    }

    void revert(Document& doc) override {
        Look& look = look_of(doc);
        if (Group* g = group_in(look, layer_index_, group_id_))
            if (removed_at_ <= g->inputs.size())
                g->inputs.insert(g->inputs.begin() +
                                     static_cast<ptrdiff_t>(removed_at_),
                                 slot_id_);
        look.links = old_links_;
    }

private:
    size_t layer_index_;
    uint64_t group_id_;
    uint64_t slot_id_;
    size_t removed_at_ = 0;
    std::vector<NodeLink> old_links_;
};

}  // namespace

Group make_group(Document& doc, std::string name) {
    Group g;
    g.id = doc.next_effect_id++;
    g.name = std::move(name);
    return g;
}

std::unique_ptr<Command> group_effects_command(uint64_t look,
                                               size_t layer_index, Group group,
                                               size_t from, size_t to) {
    return std::make_unique<GroupEffectsCommand>(look, layer_index,
                                                 std::move(group), from, to);
}

std::unique_ptr<Command> ungroup_command(uint64_t look, size_t layer_index,
                                         uint64_t group_id) {
    return std::make_unique<UngroupCommand>(look, layer_index, group_id);
}

std::unique_ptr<Command> set_effect_group_command(uint64_t look,
                                                  size_t layer_index,
                                                  size_t effect_index,
                                                  uint64_t group_id) {
    return std::make_unique<SetEffectGroupCommand>(look, layer_index,
                                                   effect_index, group_id);
}

std::unique_ptr<Command> set_group_props_command(uint64_t look,
                                                 size_t layer_index,
                                                 Group updated) {
    return std::make_unique<SetGroupPropsCommand>(look, layer_index,
                                                  std::move(updated));
}

std::unique_ptr<Command> set_group_exposed_command(uint64_t look,
                                                   size_t layer_index,
                                                   uint64_t group_id,
                                                   ParamKey key,
                                                   bool exposed) {
    return std::make_unique<SetGroupExposedCommand>(look, layer_index,
                                                    group_id, key, exposed);
}

std::unique_ptr<Command> insert_group_command(
    uint64_t look, size_t layer_index, Group group,
    std::vector<EffectInstance> effects, uint64_t face_in) {
    return std::make_unique<InsertGroupCommand>(look, layer_index,
                                                std::move(group),
                                                std::move(effects), face_in);
}

std::unique_ptr<Command> add_group_input_command(uint64_t look,
                                                 size_t layer_index,
                                                 uint64_t group_id,
                                                 uint64_t slot_id) {
    return std::make_unique<AddGroupInputCommand>(look, layer_index, group_id,
                                                  slot_id);
}

std::unique_ptr<Command> remove_group_input_command(uint64_t look,
                                                    size_t layer_index,
                                                    uint64_t group_id,
                                                    uint64_t slot_id) {
    return std::make_unique<RemoveGroupInputCommand>(look, layer_index,
                                                     group_id, slot_id);
}

void normalize_group_inputs(Document& doc, Look& look, size_t layer_index,
                            Group& g, uint64_t seed_member) {
    Layer& layer = look.layers[layer_index];
    auto is_member = [&](uint64_t id) {
        if (!id) return false;
        for (const EffectInstance& e : layer.stack)
            if (e.id == id) return e.group_id == g.id;
        return false;
    };
    auto is_slot = [&](uint64_t id) {
        for (uint64_t s : g.inputs)
            if (s == id) return true;
        return false;
    };
    auto is_crossing = [&](const NodeLink& l) {
        return l.from && is_member(l.to) && !is_member(l.from) &&
               !is_slot(l.from) && !link_is_tombstone(l);
    };
    // Touch the document only if wiring changes: keep synthesized chains.
    bool any_crossing = false;
    {
        std::vector<NodeLink> synth;
        for (const NodeLink& l : effective_links(look, synth))
            if (is_crossing(l)) {
                any_crossing = true;
                break;
            }
    }
    if (!any_crossing && !seed_member) return;
    ensure_links(look);

    // The In pair sorts first, thus its slot lands at inputs[0].
    // A slot takes the position of the first crossing in the fan-in.
    uint64_t in_member = 0;
    if (seed_member) {
        in_member = is_member(seed_member) ? seed_member : 0;
        if (!in_member)
            for (const EffectInstance& e : layer.stack)
                if (e.group_id == g.id) {
                    in_member = e.id;
                    break;
                }
    }
    struct Key {
        uint64_t member;
        uint32_t port;
    };
    std::vector<Key> keys;
    auto seen = [&](uint64_t m, uint32_t p) {
        for (const Key& k : keys)
            if (k.member == m && k.port == p) return true;
        return false;
    };
    for (const NodeLink& l : look.links)
        if (is_crossing(l) && !seen(l.to, l.to_port))
            keys.push_back({l.to, l.to_port});
    std::stable_sort(keys.begin(), keys.end(),
                     [&](const Key& a, const Key& b) {
                         const bool ai = a.member == in_member && a.port == 0;
                         const bool bi = b.member == in_member && b.port == 0;
                         return ai && !bi;
                     });
    for (const Key& k : keys) {
        const uint64_t slot = doc.next_effect_id++;
        g.inputs.push_back(slot);
        std::vector<uint64_t> producers;
        bool first = true;
        for (NodeLink& l : look.links) {
            if (!(l.to == k.member && l.to_port == k.port && is_crossing(l)))
                continue;
            producers.push_back(l.from);
            if (first) {
                l = {slot, k.member, k.port};
                first = false;
            } else {
                l = {0, 0, 9999};   // tombstone shape, removed just below
            }
        }
        look.links.erase(std::remove_if(look.links.begin(), look.links.end(),
                                        [](const NodeLink& l) {
                                            return link_is_tombstone(l);
                                        }),
                         look.links.end());
        for (uint64_t p : producers) look.links.push_back({p, slot, 0});
    }

    // A group with no slot on a member port 0 gets an interior In slot.
    if (in_member) {
        bool has_in = false;
        for (const NodeLink& l : look.links)
            if (is_slot(l.from) && l.to_port == 0 && is_member(l.to))
                has_in = true;
        if (!has_in) {
            const uint64_t slot = doc.next_effect_id++;
            g.inputs.insert(g.inputs.begin(), slot);
            look.links.push_back({slot, in_member, 0});
        }
    }
}

}  // namespace looks::doc

#include "doc/group_commands.h"

#include <algorithm>
#include <cassert>
#include <utility>
#include <unordered_map>

#include "doc/effects.h"
#include "doc/stack_commands.h"

namespace looks::doc {

namespace {

template <class Crosses, class RemoveInput>
void splice_group_inputs(Document& doc, Look& look, Crosses crosses, RemoveInput remove_input) {
    std::vector<NodeLink> links;
    std::unordered_map<uint64_t, uint64_t> merges;
    for (const auto& link : look.links) {
        if (remove_input(link)) continue;
        if (!crosses(link)) { links.push_back(link); continue; }
        std::vector<NodeLink> feeds;
        for (const auto& input : look.links)
            if (input.to == link.from && input.to_port == 0) feeds.push_back(input);
        if (feeds.empty()) continue;
        auto outgoing = link;
        if (feeds.size() == 1) outgoing.from = feeds.front().from;
        else {
            auto found = merges.find(link.from);
            if (found == merges.end()) {
                auto merge = make_effect(doc, EffectType::BlendNode);
                merge.bypass = true;
                const auto* group = group_of_input(look, link.from);
                if (group) { merge.node_x = group->in_x; merge.node_y = group->in_y; }
                found = merges.emplace(link.from, merge.id).first;
                look.effects.push_back(std::move(merge));
                for (auto input : feeds) {
                    input.to = found->second;
                    links.push_back(input);
                }
            }
            outgoing.from = found->second;
        }
        links.push_back(outgoing);
    }
    look.links = std::move(links);
}

void repair_group_boundaries(Document& doc, Look& look) {
    splice_group_inputs(doc, look, [&](const NodeLink& link) {
        const Group* owner = group_of_input(look, link.from);
        const EffectInstance* member = find_effect(look, link.to);
        return owner && member && member->group_id != owner->id;
    }, [](const NodeLink&) { return false; });
    for (Group& group : look.groups) {
        group.exposed.erase(std::remove_if(group.exposed.begin(), group.exposed.end(),
            [&](const ParamKey& key) {
                const auto* effect = find_effect(look, key.effect_id);
                return !effect || effect->group_id != group.id;
            }), group.exposed.end());
        if (!group_face_member(look, group)) group.face_out = 0;
        normalize_group_inputs(doc, look, group, 0);
    }
}

class GroupEffectsCommand final : public LookCommand {
public:
    GroupEffectsCommand(uint64_t look, Group group, std::vector<uint64_t> members)
        : LookCommand(look), group_(std::move(group)), members_(std::move(members)) {}
    std::string name() const override { return "Group Effects"; }

    void apply(Document& doc) override {
        Look& look = entity_of(doc);
        applied_ = false;
        if (!group_.id || find_group(look, group_.id) || members_.empty()) return;
        for (size_t i = 0; i < members_.size(); ++i)
            if (!find_effect(look, members_[i]) ||
                std::find(members_.begin(), members_.begin() + i, members_[i]) != members_.begin() + i) return;
        applied_ = true;
        old_groups_ = look.groups;
        old_links_ = look.links;
        old_effects_ = look.effects;
        for (uint64_t id : members_) {
            EffectInstance* fx = find_effect(look, id);
            fx->group_id = group_.id;
        }
        // Slot ids mint on the first apply and replay on redo.
        if (group_.face_out == 0) group_.face_out = members_.back();
        if (!planned_) {
            repair_group_boundaries(doc, look);
            normalize_group_inputs(doc, look, group_,
                                   members_.front());
            planned_ = true;
            new_links_ = look.links;
            new_groups_ = look.groups;
            new_effects_ = look.effects;
        } else {
            look.links = new_links_;
            look.groups = new_groups_;
            look.effects = new_effects_;
        }
        look.groups.push_back(group_);
    }

    void revert(Document& doc) override {
        if (!applied_) return;
        Look& look = entity_of(doc);
        look.groups = old_groups_;
        look.links = old_links_;
        look.effects = old_effects_;
    }

private:
    Group group_;
    std::vector<uint64_t> members_;
    std::vector<NodeLink> old_links_;
    std::vector<NodeLink> new_links_;
    std::vector<Group> old_groups_, new_groups_;
    std::vector<EffectInstance> old_effects_, new_effects_;
    bool planned_ = false;
    bool applied_ = false;
};

class UngroupCommand final : public LookCommand {
public:
    UngroupCommand(uint64_t look, uint64_t group_id)
        : LookCommand(look), group_id_(group_id) {}
    std::string name() const override { return "Ungroup"; }

    void apply(Document& doc) override {
        Look& look = entity_of(doc);
        applied_ = find_group(look, group_id_) != nullptr;
        if (!applied_) return;
        references_.detach(look, group_id_);
        old_effects_ = look.effects;
        for (size_t i = 0; i < look.effects.size(); ++i)
            if (look.effects[i].group_id == group_id_) {
                look.effects[i].group_id = 0;
            }
        for (size_t i = 0; i < look.groups.size(); ++i)
            if (look.groups[i].id == group_id_) {
                group_slot_ = i;
                removed_ = look.groups[i];
                look.groups.erase(look.groups.begin() +
                                   static_cast<ptrdiff_t>(i));
                break;
            }
        // The matte wire on port 1 goes away with the group card.
        old_links_ = look.links;
        auto is_slot = [&](uint64_t id) {
            for (uint64_t s : removed_.inputs)
                if (s == id) return true;
            return false;
        };
        if (!planned_) {
            splice_group_inputs(doc, look,
                [&](const NodeLink& link) { return is_slot(link.from); },
                [&](const NodeLink& link) { return is_slot(link.to) || (link.to == group_id_ && link.to_port == 1); });
            new_links_ = look.links;
            new_effects_ = look.effects;
            planned_ = true;
        } else {
            look.links = new_links_;
            look.effects = new_effects_;
        }
    }

    void revert(Document& doc) override {
        Look& look = entity_of(doc);
        if (!applied_) return;
        look.effects = old_effects_;
        look.groups.insert(look.groups.begin() +
                                static_cast<ptrdiff_t>(group_slot_),
                            removed_);
        look.links = old_links_;
        references_.restore(look, group_id_);
    }

private:
    uint64_t group_id_;
    bool applied_ = false, planned_ = false;
    std::vector<EffectInstance> old_effects_, new_effects_;
    size_t group_slot_ = 0;
    Group removed_;
    std::vector<NodeLink> old_links_, new_links_;
    NodeReferenceState references_;
};

class SetEffectGroupCommand final : public LookCommand {
public:
    SetEffectGroupCommand(uint64_t look, uint64_t effect_id, uint64_t group_id)
        : LookCommand(look), effect_id_(effect_id), group_id_(group_id) {}
    std::string name() const override {
        return group_id_ ? "Join Group" : "Leave Group";
    }

    void apply(Document& doc) override {
        Look& look = entity_of(doc);
        auto* fx = find_effect(look, effect_id_);
        applied_ = fx && (!group_id_ || find_group(look, group_id_));
        if (!applied_) return;
        old_effects_ = look.effects;
        old_groups_ = look.groups;
        old_links_ = look.links;
        fx->group_id = group_id_;
        if (!planned_) {
            repair_group_boundaries(doc, look);
            new_groups_ = look.groups;
            new_links_ = look.links;
            new_effects_ = look.effects;
            planned_ = true;
        } else {
            look.groups = new_groups_;
            look.links = new_links_;
            look.effects = new_effects_;
        }
    }

    void revert(Document& doc) override {
        if (!applied_) return;
        Look& look = entity_of(doc);
        look.effects = old_effects_;
        look.groups = old_groups_;
        look.links = old_links_;
    }

private:
    uint64_t effect_id_;
    uint64_t group_id_;
    std::vector<EffectInstance> old_effects_, new_effects_;
    bool applied_ = false, planned_ = false;
    std::vector<Group> old_groups_, new_groups_;
    std::vector<NodeLink> old_links_, new_links_;
};

class SetGroupPropsCommand final : public LookCommand {
public:
    SetGroupPropsCommand(uint64_t look, Group updated)
        : LookCommand(look), updated_(std::move(updated)) {}
    std::string name() const override { return "Edit Group"; }

    void apply(Document& doc) override {
        Look& look = entity_of(doc);
        Group* g = find_group(look, updated_.id);
        if (!g) return;
        old_ = *g;
        *g = updated_;
    }

    void revert(Document& doc) override {
        Look& look = entity_of(doc);
        if (Group* g = find_group(look, updated_.id))
            *g = old_;
    }

    bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetGroupPropsCommand*>(&next);
        if (!other || !same_entity(*other) ||
            other->updated_.id != updated_.id)
            return false;
        updated_ = other->updated_;
        return true;
    }

private:
    Group updated_;
    Group old_;
};

// Face order is expose order: a param that comes back goes to the end.
class SetGroupExposedCommand final : public LookCommand {
public:
    SetGroupExposedCommand(uint64_t look, uint64_t group_id, ParamKey key, bool exposed)
        : LookCommand(look), group_id_(group_id),
          key_(key), exposed_(exposed) {}
    std::string name() const override {
        return exposed_ ? "Expose Param" : "Hide Param";
    }

    void apply(Document& doc) override {
        Group* g = find_group(entity_of(doc), group_id_);
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
        Group* g = find_group(entity_of(doc), group_id_);
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
    uint64_t group_id_;
    ParamKey key_;
    bool exposed_;
    bool did_ = false;
    size_t removed_at_ = 0;
};

class InsertGroupCommand final : public LookCommand {
public:
    InsertGroupCommand(uint64_t look, Group group,
                       std::vector<EffectInstance> effects, std::vector<NodeLink> links)
        : LookCommand(look), group_(std::move(group)), effects_(std::move(effects)),
          added_links_(std::move(links)) {}
    std::string name() const override { return "Add Preset"; }

    void apply(Document& doc) override {
        Look& look = entity_of(doc);
        look.effects.insert(look.effects.end(), effects_.begin(),
                           effects_.end());
        look.groups.push_back(group_);
        look.links.insert(look.links.end(), added_links_.begin(),
                          added_links_.end());
    }

    void revert(Document& doc) override {
        Look& look = entity_of(doc);
        look.effects.erase(std::remove_if(look.effects.begin(), look.effects.end(),
            [&](const EffectInstance& fx) {
                return std::any_of(effects_.begin(), effects_.end(),
                    [&](const EffectInstance& added) { return fx.id == added.id; });
            }), look.effects.end());
        look.groups.erase(std::remove_if(look.groups.begin(), look.groups.end(),
            [&](const Group& group) { return group.id == group_.id; }), look.groups.end());
        for (const NodeLink& added : added_links_) erase_last_link(look, added);
    }

private:
    Group group_;
    std::vector<EffectInstance> effects_;
    std::vector<NodeLink> added_links_;
};

class AddGroupInputCommand final : public LookCommand {
public:
    AddGroupInputCommand(uint64_t look, uint64_t group_id,
                         uint64_t slot_id)
        : LookCommand(look), group_id_(group_id),
          slot_id_(slot_id) {}
    std::string name() const override { return "Add Group Input"; }

    void apply(Document& doc) override {
        Look& look = entity_of(doc);
        if (Group* g = find_group(look, group_id_))
            g->inputs.push_back(slot_id_);
    }

    void revert(Document& doc) override {
        Look& look = entity_of(doc);
        if (Group* g = find_group(look, group_id_))
            if (!g->inputs.empty() && g->inputs.back() == slot_id_)
                g->inputs.pop_back();
    }

private:
    uint64_t group_id_;
    uint64_t slot_id_;
};

class RemoveGroupInputCommand final : public LookCommand {
public:
    RemoveGroupInputCommand(uint64_t look, uint64_t group_id, uint64_t slot_id)
        : LookCommand(look), group_id_(group_id),
          slot_id_(slot_id) {}
    std::string name() const override { return "Remove Group Input"; }

    void apply(Document& doc) override {
        Look& look = entity_of(doc);
        Group* g = find_group(look, group_id_);
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
        Look& look = entity_of(doc);
        if (Group* g = find_group(look, group_id_))
            if (removed_at_ <= g->inputs.size())
                g->inputs.insert(g->inputs.begin() +
                                     static_cast<ptrdiff_t>(removed_at_),
                                 slot_id_);
        look.links = old_links_;
    }

private:
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
                                               Group group,
                                               std::vector<uint64_t> members) {
    return std::make_unique<GroupEffectsCommand>(look,
                                                 std::move(group), std::move(members));
}

std::unique_ptr<Command> ungroup_command(uint64_t look, uint64_t group_id) {
    return std::make_unique<UngroupCommand>(look, group_id);
}

std::unique_ptr<Command> set_effect_group_command(uint64_t look,
                                                  uint64_t effect_id,
                                                  uint64_t group_id) {
    return std::make_unique<SetEffectGroupCommand>(look,
                                                   effect_id, group_id);
}

std::unique_ptr<Command> set_group_props_command(uint64_t look,
                                                 Group updated) {
    return std::make_unique<SetGroupPropsCommand>(look,
                                                  std::move(updated));
}

std::unique_ptr<Command> set_group_exposed_command(uint64_t look,
                                                   uint64_t group_id,
                                                   ParamKey key,
                                                   bool exposed) {
    return std::make_unique<SetGroupExposedCommand>(look,
                                                    group_id, key, exposed);
}

std::unique_ptr<Command> insert_group_command(
    uint64_t look, Group group,
    std::vector<EffectInstance> effects, std::vector<NodeLink> links) {
    return std::make_unique<InsertGroupCommand>(look,
                                                std::move(group),
                                                std::move(effects), std::move(links));
}

std::unique_ptr<Command> add_group_input_command(uint64_t look,
                                                 uint64_t group_id,
                                                 uint64_t slot_id) {
    return std::make_unique<AddGroupInputCommand>(look, group_id,
                                                  slot_id);
}

std::unique_ptr<Command> remove_group_input_command(uint64_t look,
                                                    uint64_t group_id,
                                                    uint64_t slot_id) {
    return std::make_unique<RemoveGroupInputCommand>(look,
                                                     group_id, slot_id);
}

void normalize_group_inputs(Document& doc, Look& look, Group& g, uint64_t seed_member) {
    auto is_member = [&](uint64_t id) {
        if (!id) return false;
        for (const EffectInstance& e : look.effects)
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
               !is_slot(l.from);
    };
    std::vector<NodeLink> links;
    links.reserve(look.links.size() * 2 + 1);
    for (const NodeLink& link : look.links) {
        if (!is_crossing(link)) {
            links.push_back(link);
            continue;
        }
        const uint64_t slot = doc.next_effect_id++;
        g.inputs.push_back(slot);
        links.push_back({slot, link.to, link.to_port, link.blend});
        links.push_back({link.from, slot, 0});
    }
    if (seed_member && is_member(seed_member)) {
        auto entry = std::find_if(links.begin(), links.end(), [&](const NodeLink& link) {
            return link.to == seed_member && link.to_port == 0 && is_slot(link.from);
        });
        if (entry != links.end()) {
            const auto slot = std::find(g.inputs.begin(), g.inputs.end(), entry->from);
            std::rotate(g.inputs.begin(), slot, slot + 1);
        } else {
            const uint64_t slot = doc.next_effect_id++;
            g.inputs.insert(g.inputs.begin(), slot);
            links.push_back({slot, seed_member, 0});
        }
    }
    look.links = std::move(links);

}

}  // namespace looks::doc

#include "doc/group_commands.h"

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

class AddMacroCommand final : public Command {
public:
    AddMacroCommand(size_t layer_index, uint64_t group_id, MacroKnob knob)
        : layer_index_(layer_index), group_id_(group_id),
          knob_(std::move(knob)) {}
    std::string name() const override { return "Add Macro"; }

    void apply(Document& doc) override {
        Group* g = find_group(doc.layers[layer_index_], group_id_);
        assert(g);
        g->macros.push_back(knob_);
    }

    void revert(Document& doc) override {
        if (Group* g = find_group(doc.layers[layer_index_], group_id_))
            g->macros.pop_back();
    }

private:
    size_t layer_index_;
    uint64_t group_id_;
    MacroKnob knob_;
};

class RemoveMacroCommand final : public Command {
public:
    RemoveMacroCommand(size_t layer_index, uint64_t group_id,
                       size_t macro_index)
        : layer_index_(layer_index), group_id_(group_id),
          macro_index_(macro_index) {}
    std::string name() const override { return "Remove Macro"; }

    void apply(Document& doc) override {
        Group* g = find_group(doc.layers[layer_index_], group_id_);
        assert(g && macro_index_ < g->macros.size());
        removed_ = g->macros[macro_index_];
        g->macros.erase(g->macros.begin() +
                        static_cast<ptrdiff_t>(macro_index_));
    }

    void revert(Document& doc) override {
        if (Group* g = find_group(doc.layers[layer_index_], group_id_))
            g->macros.insert(g->macros.begin() +
                                 static_cast<ptrdiff_t>(macro_index_),
                             removed_);
    }

private:
    size_t layer_index_;
    uint64_t group_id_;
    size_t macro_index_;
    MacroKnob removed_;
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
    }

    void revert(Document& doc) override {
        Layer& layer = doc.layers[layer_index_];
        layer.stack.erase(layer.stack.begin() +
                              static_cast<ptrdiff_t>(insert_at_),
                          layer.stack.begin() + static_cast<ptrdiff_t>(
                                                    insert_at_ +
                                                    effects_.size()));
        layer.groups.pop_back();
    }

private:
    size_t layer_index_;
    Group group_;
    std::vector<EffectInstance> effects_;
    size_t insert_at_ = 0;
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

std::unique_ptr<Command> add_macro_command(size_t layer_index,
                                           uint64_t group_id, MacroKnob knob) {
    return std::make_unique<AddMacroCommand>(layer_index, group_id,
                                             std::move(knob));
}

std::unique_ptr<Command> remove_macro_command(size_t layer_index,
                                              uint64_t group_id,
                                              size_t macro_index) {
    return std::make_unique<RemoveMacroCommand>(layer_index, group_id,
                                                macro_index);
}

std::unique_ptr<Command> insert_group_command(
    size_t layer_index, Group group, std::vector<EffectInstance> effects) {
    return std::make_unique<InsertGroupCommand>(layer_index, std::move(group),
                                                std::move(effects));
}

}  // namespace looks::doc

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

}  // namespace

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

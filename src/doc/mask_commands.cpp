#include "doc/mask_commands.h"

#include <algorithm>
#include <cassert>
#include <utility>

#include "doc/stack_commands.h"

namespace looks::doc {

Mask* find_mask(Document& doc, uint64_t mask_id) {
    for (Mask& m : doc.masks)
        if (m.id == mask_id) return &m;
    return nullptr;
}

const Mask* find_mask(const Document& doc, uint64_t mask_id) {
    for (const Mask& m : doc.masks)
        if (m.id == mask_id) return &m;
    return nullptr;
}

namespace {

class AddMaskCommand final : public Command {
public:
    explicit AddMaskCommand(Mask mask) : mask_(std::move(mask)) {}
    std::string name() const override { return "Add Mask"; }
    void apply(Document& doc) override { doc.masks.push_back(mask_); }
    void revert(Document& doc) override { doc.masks.pop_back(); }

private:
    Mask mask_;
};

class RemoveMaskCommand final : public Command {
public:
    explicit RemoveMaskCommand(uint64_t mask_id) : mask_id_(mask_id) {}
    std::string name() const override { return "Remove Mask"; }

    void apply(Document& doc) override {
        referencing_.clear();
        layer_refs_.clear();
        for (size_t l = 0; l < doc.layers.size(); ++l) {
            if (doc.layers[l].mask_id == mask_id_) {
                layer_refs_.push_back(l);
                doc.layers[l].mask_id = 0;
            }
            auto& stack = doc.layers[l].stack;
            for (size_t i = 0; i < stack.size(); ++i) {
                if (stack[i].mask_id == mask_id_) {
                    referencing_.push_back({l, i});
                    stack[i].mask_id = 0;
                }
            }
        }
        for (size_t i = 0; i < doc.masks.size(); ++i) {
            if (doc.masks[i].id == mask_id_) {
                index_ = i;
                removed_ = doc.masks[i];
                doc.masks.erase(doc.masks.begin() + i);
                return;
            }
        }
        assert(false && "mask not found");
    }

    void revert(Document& doc) override {
        doc.masks.insert(doc.masks.begin() + index_, removed_);
        for (const auto& [l, i] : referencing_)
            if (l < doc.layers.size() && i < doc.layers[l].stack.size())
                doc.layers[l].stack[i].mask_id = mask_id_;
        for (const size_t l : layer_refs_)
            if (l < doc.layers.size()) doc.layers[l].mask_id = mask_id_;
    }

private:
    uint64_t mask_id_;
    size_t index_ = 0;
    Mask removed_;
    std::vector<std::pair<size_t, size_t>> referencing_;
    std::vector<size_t> layer_refs_;
};

class SetMaskParamsCommand final : public Command {
public:
    explicit SetMaskParamsCommand(Mask updated) : updated_(std::move(updated)) {}
    std::string name() const override { return "Edit Mask"; }

    void apply(Document& doc) override {
        Mask* m = find_mask(doc, updated_.id);
        assert(m);
        old_ = *m;
        assign(*m, updated_);
    }

    void revert(Document& doc) override {
        if (Mask* m = find_mask(doc, updated_.id)) assign(*m, old_);
    }

    bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetMaskParamsCommand*>(&next);
        if (!other || other->updated_.id != updated_.id) return false;
        updated_ = other->updated_;
        return true;
    }

private:
    // Everything but the chain (chain edits have their own commands).
    static void assign(Mask& dst, const Mask& src) {
        std::vector<EffectInstance> chain = std::move(dst.chain);
        dst = src;
        dst.chain = std::move(chain);
    }

    Mask updated_;
    Mask old_;
};

class RemoveMaskPointCommand final : public Command {
public:
    RemoveMaskPointCommand(uint64_t mask_id, size_t point_index)
        : mask_id_(mask_id), point_(point_index) {}
    std::string name() const override { return "Remove Mask Point"; }

    void apply(Document& doc) override {
        Mask* m = find_mask(doc, mask_id_);
        assert(m && point_ * 2 + 1 < m->points.size());
        old_points_ = m->points;
        m->points.erase(m->points.begin() + static_cast<ptrdiff_t>(point_ * 2),
                        m->points.begin() +
                            static_cast<ptrdiff_t>(point_ * 2 + 2));
        // Point lanes follow the points: the removed point's lanes die,
        // higher points' lanes shift down one slot pair.
        removed_lanes_.clear();
        const uint64_t key_id = mask_id_ | kMaskParamBit;
        for (size_t i = 0; i < doc.lanes.size();) {
            KeyframeLane& lane = doc.lanes[i];
            if (lane.target.effect_id != key_id ||
                lane.target.param_index < kMaskPointParamBase) {
                ++i;
                continue;
            }
            const size_t pt = static_cast<size_t>(lane.target.param_index -
                                                  kMaskPointParamBase) / 2;
            if (pt == point_) {
                removed_lanes_.emplace_back(i, lane);
                doc.lanes.erase(doc.lanes.begin() + static_cast<ptrdiff_t>(i));
                continue;
            }
            if (pt > point_) lane.target.param_index -= 2;
            ++i;
        }
    }

    void revert(Document& doc) override {
        const uint64_t key_id = mask_id_ | kMaskParamBit;
        for (KeyframeLane& lane : doc.lanes) {
            if (lane.target.effect_id != key_id ||
                lane.target.param_index < kMaskPointParamBase)
                continue;
            const size_t pt = static_cast<size_t>(lane.target.param_index -
                                                  kMaskPointParamBase) / 2;
            if (pt >= point_) lane.target.param_index += 2;
        }
        // Positions were recorded ascending pre-erase, so ascending
        // re-insertion restores the original lane order.
        for (const auto& [pos, lane] : removed_lanes_)
            doc.lanes.insert(
                doc.lanes.begin() +
                    static_cast<ptrdiff_t>(std::min(pos, doc.lanes.size())),
                lane);
        if (Mask* m = find_mask(doc, mask_id_)) m->points = old_points_;
    }

private:
    uint64_t mask_id_;
    size_t point_;
    std::vector<float> old_points_;
    std::vector<std::pair<size_t, KeyframeLane>> removed_lanes_;
};

class SetEffectMaskCommand final : public Command {
public:
    SetEffectMaskCommand(size_t layer_index, size_t effect_index,
                         uint64_t mask_id)
        : layer_index_(layer_index), effect_index_(effect_index),
          mask_id_(mask_id) {}
    std::string name() const override { return "Assign Mask"; }

    void apply(Document& doc) override {
        assert(layer_index_ < doc.layers.size() &&
               effect_index_ < doc.layers[layer_index_].stack.size());
        old_mask_ = doc.layers[layer_index_].stack[effect_index_].mask_id;
        doc.layers[layer_index_].stack[effect_index_].mask_id = mask_id_;
    }

    void revert(Document& doc) override {
        doc.layers[layer_index_].stack[effect_index_].mask_id = old_mask_;
    }

private:
    size_t layer_index_;
    size_t effect_index_;
    uint64_t mask_id_;
    uint64_t old_mask_ = 0;
};

class MaskChainAddCommand final : public Command {
public:
    MaskChainAddCommand(uint64_t mask_id, EffectInstance instance)
        : mask_id_(mask_id), instance_(std::move(instance)) {}
    std::string name() const override { return "Add Mask Chain Effect"; }

    void apply(Document& doc) override {
        Mask* m = find_mask(doc, mask_id_);
        assert(m);
        m->chain.push_back(instance_);
    }

    void revert(Document& doc) override {
        if (Mask* m = find_mask(doc, mask_id_)) m->chain.pop_back();
    }

private:
    uint64_t mask_id_;
    EffectInstance instance_;
};

class MaskChainRemoveCommand final : public Command {
public:
    MaskChainRemoveCommand(uint64_t mask_id, size_t chain_index)
        : mask_id_(mask_id), chain_index_(chain_index) {}
    std::string name() const override { return "Remove Mask Chain Effect"; }

    void apply(Document& doc) override {
        Mask* m = find_mask(doc, mask_id_);
        assert(m && chain_index_ < m->chain.size());
        removed_ = m->chain[chain_index_];
        m->chain.erase(m->chain.begin() + chain_index_);
    }

    void revert(Document& doc) override {
        if (Mask* m = find_mask(doc, mask_id_))
            m->chain.insert(m->chain.begin() + chain_index_, removed_);
    }

private:
    uint64_t mask_id_;
    size_t chain_index_;
    EffectInstance removed_;
};

class MaskChainSetParamCommand final : public Command {
public:
    MaskChainSetParamCommand(uint64_t mask_id, size_t chain_index,
                             int param_index, float new_value)
        : mask_id_(mask_id), chain_index_(chain_index),
          param_index_(param_index), new_value_(new_value) {}
    std::string name() const override { return "Edit Mask Chain Param"; }

    void apply(Document& doc) override {
        float& p = slot(doc);
        old_value_ = p;
        p = new_value_;
    }

    void revert(Document& doc) override { slot(doc) = old_value_; }

    bool merge(const Command& next) override {
        const auto* other =
            dynamic_cast<const MaskChainSetParamCommand*>(&next);
        if (!other || other->mask_id_ != mask_id_ ||
            other->chain_index_ != chain_index_ ||
            other->param_index_ != param_index_)
            return false;
        new_value_ = other->new_value_;
        return true;
    }

private:
    float& slot(Document& doc) {
        Mask* m = find_mask(doc, mask_id_);
        assert(m && chain_index_ < m->chain.size());
        EffectInstance& fx = m->chain[chain_index_];
        if (param_index_ == kWetParam) return fx.wet;
        if (param_index_ == kOpacityParam) return fx.opacity;
        assert(param_index_ >= 0 &&
               static_cast<size_t>(param_index_) < fx.params.size());
        return fx.params[static_cast<size_t>(param_index_)];
    }

    uint64_t mask_id_;
    size_t chain_index_;
    int param_index_;
    float new_value_;
    float old_value_ = 0.0f;
};

}  // namespace

std::unique_ptr<Command> add_mask_command(Mask mask) {
    return std::make_unique<AddMaskCommand>(std::move(mask));
}
std::unique_ptr<Command> remove_mask_command(uint64_t mask_id) {
    return std::make_unique<RemoveMaskCommand>(mask_id);
}
std::unique_ptr<Command> set_mask_params_command(Mask updated) {
    return std::make_unique<SetMaskParamsCommand>(std::move(updated));
}
std::unique_ptr<Command> remove_mask_point_command(uint64_t mask_id,
                                                   size_t point_index) {
    return std::make_unique<RemoveMaskPointCommand>(mask_id, point_index);
}
std::unique_ptr<Command> set_effect_mask_command(size_t layer_index,
                                                 size_t effect_index,
                                                 uint64_t mask_id) {
    return std::make_unique<SetEffectMaskCommand>(layer_index, effect_index,
                                                  mask_id);
}
std::unique_ptr<Command> mask_chain_add_command(uint64_t mask_id,
                                                EffectInstance instance) {
    return std::make_unique<MaskChainAddCommand>(mask_id, std::move(instance));
}
std::unique_ptr<Command> mask_chain_remove_command(uint64_t mask_id,
                                                   size_t chain_index) {
    return std::make_unique<MaskChainRemoveCommand>(mask_id, chain_index);
}
std::unique_ptr<Command> mask_chain_set_param_command(uint64_t mask_id,
                                                      size_t chain_index,
                                                      int param_index,
                                                      float new_value) {
    return std::make_unique<MaskChainSetParamCommand>(mask_id, chain_index,
                                                      param_index, new_value);
}

}  // namespace looks::doc

#include "doc/layer_commands.h"

#include <cassert>
#include <utility>

namespace looks::doc {

namespace {

Layer* find_layer(Document& doc, uint64_t layer_id) {
    for (Layer& l : doc.layers)
        if (l.id == layer_id) return &l;
    return nullptr;
}

class AddLayerCommand final : public Command {
public:
    AddLayerCommand(Layer layer, size_t insert_index)
        : layer_(std::move(layer)), insert_index_(insert_index) {}
    std::string name() const override { return "Add Layer"; }

    void apply(Document& doc) override {
        assert(insert_index_ <= doc.layers.size());
        doc.layers.insert(doc.layers.begin() + insert_index_, layer_);
    }

    void revert(Document& doc) override {
        doc.layers.erase(doc.layers.begin() + insert_index_);
    }

private:
    Layer layer_;
    size_t insert_index_;
};

class RemoveLayerCommand final : public Command {
public:
    explicit RemoveLayerCommand(size_t layer_index)
        : layer_index_(layer_index) {}
    std::string name() const override { return "Remove Layer"; }

    void apply(Document& doc) override {
        assert(layer_index_ < doc.layers.size());
        removed_ = doc.layers[layer_index_];
        doc.layers.erase(doc.layers.begin() + layer_index_);
    }

    void revert(Document& doc) override {
        doc.layers.insert(doc.layers.begin() + layer_index_, removed_);
    }

private:
    size_t layer_index_;
    Layer removed_;
};

class SetLayerPropsCommand final : public Command {
public:
    explicit SetLayerPropsCommand(Layer updated)
        : updated_(std::move(updated)) {}
    std::string name() const override { return "Edit Layer"; }

    void apply(Document& doc) override {
        Layer* l = find_layer(doc, updated_.id);
        assert(l);
        old_ = *l;
        assign(*l, updated_);
    }

    void revert(Document& doc) override {
        if (Layer* l = find_layer(doc, updated_.id)) assign(*l, old_);
    }

    bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetLayerPropsCommand*>(&next);
        if (!other || other->updated_.id != updated_.id) return false;
        updated_ = other->updated_;
        return true;
    }

private:
    static void assign(Layer& dst, const Layer& src) {
        std::vector<EffectInstance> stack = std::move(dst.stack);
        dst = src;
        dst.stack = std::move(stack);
    }

    Layer updated_;
    Layer old_;
};

class MoveLayerCommand final : public Command {
public:
    MoveLayerCommand(size_t index, int direction)
        : index_(index), direction_(direction) {}
    std::string name() const override { return "Move Layer"; }

    void apply(Document& doc) override { swap(doc); }
    void revert(Document& doc) override { swap(doc); }

private:
    void swap(Document& doc) {
        const size_t other = index_ + static_cast<size_t>(direction_);
        assert(index_ < doc.layers.size() && other < doc.layers.size());
        std::swap(doc.layers[index_], doc.layers[other]);
    }

    size_t index_;
    int direction_;
};

class ReplaceLayerCommand final : public Command {
public:
    ReplaceLayerCommand(size_t index, Layer fresh)
        : index_(index), fresh_(std::move(fresh)) {}
    std::string name() const override { return "Reset Layer"; }

    void apply(Document& doc) override {
        assert(index_ < doc.layers.size());
        old_ = doc.layers[index_];
        doc.layers[index_] = fresh_;
    }

    void revert(Document& doc) override { doc.layers[index_] = old_; }

private:
    size_t index_;
    Layer fresh_;
    Layer old_;
};

}  // namespace

Layer make_layer(Document& doc, LayerSourceKind kind) {
    Layer layer;
    layer.id = doc.next_effect_id++;
    layer.source = kind;
    static const char* kNames[] = {"clip", "solid", "gradient", "noise",
                                   "pattern", "adjust"};
    layer.name = std::string(kNames[static_cast<size_t>(kind)]) + " " +
                 std::to_string(layer.id);
    // Generators default to half opacity so adding one doesn't blank the
    // composite.
    if (kind == LayerSourceKind::Solid || kind == LayerSourceKind::Gradient ||
        kind == LayerSourceKind::Noise ||
        kind == LayerSourceKind::TestPattern)
        layer.opacity = 0.5f;
    return layer;
}

std::unique_ptr<Command> add_layer_command(Layer layer, size_t insert_index) {
    return std::make_unique<AddLayerCommand>(std::move(layer), insert_index);
}

std::unique_ptr<Command> remove_layer_command(size_t layer_index) {
    return std::make_unique<RemoveLayerCommand>(layer_index);
}

std::unique_ptr<Command> set_layer_props_command(Layer updated) {
    return std::make_unique<SetLayerPropsCommand>(std::move(updated));
}

std::unique_ptr<Command> move_layer_command(size_t index, int direction) {
    return std::make_unique<MoveLayerCommand>(index, direction);
}

std::unique_ptr<Command> replace_layer_command(size_t index, Layer fresh) {
    return std::make_unique<ReplaceLayerCommand>(index, std::move(fresh));
}

}  // namespace looks::doc

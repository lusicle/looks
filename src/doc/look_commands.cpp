#include "doc/look_commands.h"

#include <cassert>
#include <unordered_map>
#include <utility>

namespace looks::doc {

namespace {

template <class V>
void erase_by_id(V& v, uint64_t id) {
    for (auto it = v.begin(); it != v.end(); ++it)
        if (it->id == id) {
            v.erase(it);
            break;
        }
}

template <class T, std::vector<T> Document::*List>
class AddEntityCommand final : public Command {
public:
    AddEntityCommand(T entity, const char* label)
        : entity_(std::move(entity)), label_(label) {}
    std::string name() const override { return label_; }
    void apply(Document& doc) override { (doc.*List).push_back(entity_); }
    void revert(Document& doc) override {
        erase_by_id(doc.*List, entity_.id);
    }

private:
    T entity_;
    const char* label_;
};

// Revert clamps the index: the vector can shrink between apply and revert.
// The index matters: the first asset drives the automatic canvas and fps.
template <class T, std::vector<T> Document::*List>
class RemoveEntityCommand final : public Command {
public:
    RemoveEntityCommand(uint64_t id, const char* label)
        : id_(id), label_(label) {}
    std::string name() const override { return label_; }

    void apply(Document& doc) override {
        std::vector<T>& v = doc.*List;
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i].id == id_) {
                index_ = i;
                removed_ = v[i];
                had_ = true;
                v.erase(v.begin() + static_cast<ptrdiff_t>(i));
                break;
            }
    }

    void revert(Document& doc) override {
        if (!had_) return;
        std::vector<T>& v = doc.*List;
        const size_t at = std::min(index_, v.size());
        v.insert(v.begin() + static_cast<ptrdiff_t>(at), removed_);
    }

private:
    uint64_t id_;
    const char* label_;
    size_t index_ = 0;
    T removed_{};
    bool had_ = false;
};

// The fork is one level deep: nested look and sequence targets stay shared.
Look clone_look_for_unique(Document& doc, const Look& src) {
    Look out = src;
    out.id = doc.next_effect_id++;
    out.name = src.name.empty() ? "look copy" : src.name + " copy";

    std::unordered_map<uint64_t, uint64_t> map;
    auto remint = [&](uint64_t old) {
        if (!old) return uint64_t{0};
        const uint64_t id = doc.next_effect_id++;
        map.emplace(old, id);
        return id;
    };
    auto mapped = [&](uint64_t old) {
        const auto it = map.find(old);
        return it == map.end() ? old : it->second;
    };

    for (Layer& l : out.layers) {
        l.id = remint(l.id);
        for (EffectInstance& fx : l.stack) fx.id = remint(fx.id);
        for (Group& g : l.groups) {
            g.id = remint(g.id);
            for (uint64_t& s : g.inputs) s = remint(s);
        }
    }
    for (CanvasFrame& f : out.frames) f.id = remint(f.id);

    // An id the look does not own passes through unchanged.
    auto mapped_key = [&](ParamKey k) {
        if (k.effect_id & kLayerParamBit)
            k.effect_id = mapped(k.effect_id & ~kLayerParamBit) |
                          kLayerParamBit;
        else if (k.effect_id & kGroupParamBit)
            k.effect_id = mapped(k.effect_id & ~kGroupParamBit) |
                          kGroupParamBit;
        else if (k.effect_id)
            k.effect_id = mapped(k.effect_id);
        return k;
    };
    for (Layer& l : out.layers) {
        for (EffectInstance& fx : l.stack)
            fx.group_id = mapped(fx.group_id);
        for (Group& g : l.groups) {
            g.face_out = mapped(g.face_out);
            for (ParamKey& k : g.exposed) k = mapped_key(k);
        }
    }
    for (NodeLink& l : out.links) {
        l.from = mapped(l.from);
        if (l.to) l.to = mapped(l.to);
    }
    // Value node ids come from next_route_id but share the same map.
    for (ValueNode& n : out.value_nodes) {
        const uint64_t id = doc.next_route_id++;
        map.emplace(n.id, id);
        n.id = id;
    }
    for (ValueNode& n : out.value_nodes) {
        n.in_a = mapped(n.in_a);
        n.in_b = mapped(n.in_b);
    }
    for (ModRoute& r : out.mod_routes) {
        r.id = doc.next_route_id++;
        r.node = mapped(r.node);
        r.target = mapped_key(r.target);
    }
    for (KeyframeLane& lane : out.lanes)
        lane.target = mapped_key(lane.target);
    for (Snapshot& s : out.snapshots)
        for (SnapshotEntry& e : s.entries)
            e.effect_id =
                e.effect_id & kGroupParamBit
                    ? mapped(e.effect_id & ~kGroupParamBit) | kGroupParamBit
                    : mapped(e.effect_id);
    return out;
}

// A link id remints one time only, thus pairs stay pairs in the copy.
// Placement targets stay shared: the fork is one level deep.
Sequence clone_sequence_for_unique(Document& doc, const Sequence& src) {
    Sequence out = src;
    out.id = doc.next_effect_id++;
    out.name = src.name.empty() ? "sequence copy" : src.name + " copy";

    std::unordered_map<uint64_t, uint64_t> links;
    auto mapped_link = [&](uint64_t old) {
        if (!old) return uint64_t{0};
        const auto it = links.find(old);
        if (it != links.end()) return it->second;
        const uint64_t id = doc.next_effect_id++;
        links.emplace(old, id);
        return id;
    };
    for (SeqTrack& t : out.tracks) {
        t.id = doc.next_effect_id++;
        for (Placement& p : t.placements) {
            p.id = doc.next_effect_id++;
            p.link = mapped_link(p.link);
        }
    }
    for (AudioTrack& t : out.audio) {
        t.id = doc.next_effect_id++;
        for (Placement& p : t.placements) {
            p.id = doc.next_effect_id++;
            p.link = mapped_link(p.link);
        }
    }
    return out;
}

class RemoveBinCommand final : public Command {
public:
    explicit RemoveBinCommand(uint64_t bin_id) : bin_id_(bin_id) {}
    std::string name() const override { return "Remove Bin"; }

    void apply(Document& doc) override {
        had_ = false;
        moved_.clear();
        reparented_.clear();
        for (size_t i = 0; i < doc.bins.size(); ++i)
            if (doc.bins[i].id == bin_id_) {
                index_ = i;
                removed_ = doc.bins[i];
                had_ = true;
                doc.bins.erase(doc.bins.begin() +
                               static_cast<ptrdiff_t>(i));
                break;
            }
        if (!had_) return;
        // The contents climb to the parent of the deleted bin.
        auto climb = [&](uint64_t id, uint64_t* slot) {
            if (*slot != bin_id_) return;
            moved_.push_back(id);
            *slot = removed_.parent;
        };
        for (Look& l : doc.looks) climb(l.id, &l.bin);
        for (Sequence& s : doc.sequences) climb(s.id, &s.bin);
        for (Asset& a : doc.assets) climb(a.id, &a.bin);
        for (Bin& b : doc.bins)
            if (b.parent == bin_id_) {
                reparented_.push_back(b.id);
                b.parent = removed_.parent;
            }
    }

    void revert(Document& doc) override {
        if (!had_) return;
        doc.bins.insert(doc.bins.begin() + static_cast<ptrdiff_t>(
                            std::min(index_, doc.bins.size())),
                        removed_);
        auto restore = [&](uint64_t id, uint64_t* slot, uint64_t moved) {
            if (id == moved) *slot = bin_id_;
        };
        for (const uint64_t id : moved_) {
            for (Look& l : doc.looks) restore(l.id, &l.bin, id);
            for (Sequence& s : doc.sequences) restore(s.id, &s.bin, id);
            for (Asset& a : doc.assets) restore(a.id, &a.bin, id);
        }
        for (const uint64_t id : reparented_)
            if (Bin* b = doc.find_bin(id)) b->parent = bin_id_;
    }

private:
    uint64_t bin_id_;
    Bin removed_;
    size_t index_ = 0;
    bool had_ = false;
    std::vector<uint64_t> moved_;
    std::vector<uint64_t> reparented_;
};

class SetBinPropsCommand final : public Command {
public:
    SetBinPropsCommand(uint64_t bin, std::string name, uint64_t parent)
        : bin_id_(bin), name_(std::move(name)), parent_(parent) {}
    std::string name() const override { return "Edit Bin"; }

    void apply(Document& doc) override {
        if (Bin* b = doc.find_bin(bin_id_)) {
            old_name_ = b->name;
            old_parent_ = b->parent;
            b->name = name_;
            b->parent = parent_;
        }
    }

    void revert(Document& doc) override {
        if (Bin* b = doc.find_bin(bin_id_)) {
            b->name = old_name_;
            b->parent = old_parent_;
        }
    }

    // A rename comes one keystroke at a time: merge it into one undo step.
    bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetBinPropsCommand*>(&next);
        if (!other || other->bin_id_ != bin_id_) return false;
        name_ = other->name_;
        parent_ = other->parent_;
        return true;
    }

private:
    uint64_t bin_id_;
    std::string name_;
    uint64_t parent_;
    std::string old_name_;
    uint64_t old_parent_ = 0;
};

class SetEntityBinCommand final : public Command {
public:
    SetEntityBinCommand(uint64_t entity_id, uint64_t bin)
        : entity_id_(entity_id), bin_(bin) {}
    std::string name() const override { return "Move To Bin"; }

    uint64_t* slot_of(Document& doc) const {
        if (Look* l = doc.find_look(entity_id_)) return &l->bin;
        if (Sequence* s = doc.find_sequence(entity_id_)) return &s->bin;
        for (Asset& a : doc.assets)
            if (a.id == entity_id_) return &a.bin;
        return nullptr;
    }

    void apply(Document& doc) override {
        if (uint64_t* slot = slot_of(doc)) {
            old_bin_ = *slot;
            *slot = bin_;
        }
    }

    void revert(Document& doc) override {
        if (uint64_t* slot = slot_of(doc)) *slot = old_bin_;
    }

private:
    uint64_t entity_id_;
    uint64_t bin_;
    uint64_t old_bin_ = 0;
};

class SetLookPropsCommand final : public LookCommand {
public:
    SetLookPropsCommand(uint64_t look, std::string name, uint32_t duration)
        : LookCommand(look), name_(std::move(name)), duration_(duration) {}
    std::string name() const override { return "Edit Look"; }

    void apply(Document& doc) override {
        Look& look = look_of(doc);
        old_name_ = look.name;
        old_duration_ = look.duration;
        look.name = name_;
        look.duration = duration_;
    }

    void revert(Document& doc) override {
        Look& look = look_of(doc);
        look.name = old_name_;
        look.duration = old_duration_;
    }

    bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetLookPropsCommand*>(&next);
        if (!other || !same_look(*other)) return false;
        name_ = other->name_;
        duration_ = other->duration_;
        return true;
    }

private:
    std::string name_;
    uint32_t duration_;
    std::string old_name_;
    uint32_t old_duration_ = 0;
};

// Combining drops the audio wire: a hidden port must not hold a live link.
class SetLookAudioSplitCommand final : public LookCommand {
public:
    SetLookAudioSplitCommand(uint64_t look, bool split)
        : LookCommand(look), split_(split) {}
    std::string name() const override {
        return split_ ? "Split Output Audio" : "Combine Output Audio";
    }

    void apply(Document& doc) override {
        Look& look = look_of(doc);
        old_split_ = look.audio_split;
        look.audio_split = split_;
        had_wire_ = false;
        sealed_ = false;
        if (!split_)
            for (auto it = look.links.begin(); it != look.links.end();
                 ++it)
                if (it->to == 0 && it->to_port == 1) {
                    wire_ = *it;
                    had_wire_ = true;
                    look.links.erase(it);
                    if (look.links.empty()) {
                        seal_links(look);
                        sealed_ = true;
                    }
                    break;
                }
    }

    void revert(Document& doc) override {
        Look& look = look_of(doc);
        look.audio_split = old_split_;
        if (sealed_) prune_tombstone(look);
        if (had_wire_) look.links.push_back(wire_);
    }

private:
    bool split_;
    bool old_split_ = false;
    NodeLink wire_{};
    bool had_wire_ = false;
    bool sealed_ = false;
};

class SetSequencePropsCommand final : public SequenceCommand {
public:
    SetSequencePropsCommand(uint64_t seq, std::string name,
                            uint32_t duration)
        : SequenceCommand(seq), name_(std::move(name)),
          duration_(duration) {}
    std::string name() const override { return "Edit Sequence"; }

    void apply(Document& doc) override {
        Sequence& seq = sequence_of(doc);
        old_name_ = seq.name;
        old_duration_ = seq.duration;
        seq.name = name_;
        seq.duration = duration_;
    }

    void revert(Document& doc) override {
        Sequence& seq = sequence_of(doc);
        seq.name = old_name_;
        seq.duration = old_duration_;
    }

    bool merge(const Command& next) override {
        const auto* other =
            dynamic_cast<const SetSequencePropsCommand*>(&next);
        if (!other || !same_sequence(*other)) return false;
        name_ = other->name_;
        duration_ = other->duration_;
        return true;
    }

private:
    std::string name_;
    uint32_t duration_;
    std::string old_name_;
    uint32_t old_duration_ = 0;
};

class SetAssetCommand final : public Command {
public:
    explicit SetAssetCommand(Asset updated) : updated_(std::move(updated)) {}
    std::string name() const override { return "Edit Media"; }

    void apply(Document& doc) override {
        Asset* a = doc.find_asset(updated_.id);
        if (!a) return;
        old_ = *a;
        had_ = true;
        *a = updated_;
    }

    void revert(Document& doc) override {
        if (!had_) return;
        if (Asset* a = doc.find_asset(updated_.id)) *a = old_;
    }

    bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetAssetCommand*>(&next);
        if (!other || other->updated_.id != updated_.id) return false;
        updated_ = other->updated_;
        return true;
    }

private:
    Asset updated_;
    Asset old_;
    bool had_ = false;
};


// Links name effects too: the boundary test needs the whole subtree.
std::vector<uint64_t> owned_ids(const Look& look,
                                const std::vector<uint64_t>& layer_ids) {
    std::vector<uint64_t> ids;
    for (const uint64_t sel : layer_ids) {
        for (const Layer& l : look.layers) {
            if (l.id != sel) continue;
            ids.push_back(l.id);
            for (const EffectInstance& fx : l.stack) ids.push_back(fx.id);
            for (const Group& g : l.groups) ids.push_back(g.id);
        }
    }
    return ids;
}

class NestLayersCommand final : public LookCommand {
public:
    NestLayersCommand(uint64_t look, Look nested, Layer ref,
                      std::vector<uint64_t> layers,
                      std::vector<uint64_t> owned, size_t insert_index)
        : LookCommand(look), nested_(std::move(nested)),
          ref_(std::move(ref)), layers_(std::move(layers)),
          owned_(std::move(owned)), insert_index_(insert_index) {}
    std::string name() const override { return "Nest"; }

    void apply(Document& doc) override {
        Look& parent = look_of(doc);
        removed_layers_.clear();
        removed_links_.clear();
        // An empty link table means the wiring comes from stack order.
        // Freeze it first, or the layers that stay behind go unwired.
        materialized_ = parent.links.empty();
        if (materialized_) parent.links = synthesize_links(parent);
        for (size_t i = parent.layers.size(); i-- > 0;) {
            if (!is_layer(parent.layers[i].id)) continue;
            removed_layers_.push_back({i, parent.layers[i]});
            parent.layers.erase(parent.layers.begin() +
                                static_cast<ptrdiff_t>(i));
        }
        // Every link that touches the selection goes. Undo rebuilds them.
        for (size_t i = parent.links.size(); i-- > 0;) {
            const NodeLink& l = parent.links[i];
            if (!selected(l.from) && !selected(l.to)) continue;
            removed_links_.push_back({i, l});
            parent.links.erase(parent.links.begin() +
                               static_cast<ptrdiff_t>(i));
        }
        parent.layers.insert(
            parent.layers.begin() +
                static_cast<ptrdiff_t>(
                    std::min(insert_index_, parent.layers.size())),
            ref_);
        parent.links.push_back({ref_.id, 0, 0});
        doc.looks.push_back(nested_);
    }

    void revert(Document& doc) override {
        Look& parent = look_of(doc);
        erase_by_id(doc.looks, nested_.id);
        for (size_t i = parent.links.size(); i-- > 0;)
            if (parent.links[i].from == ref_.id && parent.links[i].to == 0) {
                parent.links.erase(parent.links.begin() +
                                   static_cast<ptrdiff_t>(i));
                break;
            }
        erase_by_id(parent.layers, ref_.id);
        // Restore back-to-front: apply walked the vectors backwards.
        for (size_t i = removed_layers_.size(); i-- > 0;) {
            const auto& [index, layer] = removed_layers_[i];
            parent.layers.insert(
                parent.layers.begin() +
                    static_cast<ptrdiff_t>(std::min(index,
                                                    parent.layers.size())),
                layer);
        }
        for (size_t i = removed_links_.size(); i-- > 0;) {
            const auto& [index, link] = removed_links_[i];
            parent.links.insert(
                parent.links.begin() +
                    static_cast<ptrdiff_t>(std::min(index,
                                                    parent.links.size())),
                link);
        }
        // Go back to synthesized wiring if that is what it was.
        if (materialized_) parent.links.clear();
    }

private:
    bool is_layer(uint64_t id) const {
        for (const uint64_t m : layers_)
            if (m == id) return true;
        return false;
    }
    bool selected(uint64_t id) const {
        for (const uint64_t m : owned_)
            if (m == id) return true;
        return false;
    }

    Look nested_;
    Layer ref_;
    std::vector<uint64_t> layers_;   // the chosen layers
    std::vector<uint64_t> owned_;    // plus their effects and groups
    size_t insert_index_;
    bool materialized_ = false;   // this command froze the wiring
    std::vector<std::pair<size_t, Layer>> removed_layers_;
    std::vector<std::pair<size_t, NodeLink>> removed_links_;
};

class MakeUniqueCommand final : public SequenceCommand {
public:
    MakeUniqueCommand(uint64_t sequence, uint64_t placement_id, Look look,
                      Sequence seq, bool is_look)
        : SequenceCommand(sequence), placement_id_(placement_id),
          look_(std::move(look)), seq_(std::move(seq)), is_look_(is_look) {}
    std::string name() const override { return "Make Unique"; }

    void apply(Document& doc) override {
        Placement* p = find_placement(sequence_of(doc), placement_id_);
        if (!p || !p->target) return;
        old_target_ = p->target;
        p->target = is_look_ ? look_.id : seq_.id;
        if (is_look_) doc.looks.push_back(look_);
        else doc.sequences.push_back(seq_);
        applied_ = true;
    }

    void revert(Document& doc) override {
        if (!applied_) return;
        if (Placement* p = find_placement(sequence_of(doc), placement_id_))
            p->target = old_target_;
        if (is_look_) erase_by_id(doc.looks, look_.id);
        else erase_by_id(doc.sequences, seq_.id);
        applied_ = false;
    }

private:
    uint64_t placement_id_;
    Look look_;
    Sequence seq_;
    bool is_look_;
    uint64_t old_target_ = 0;
    bool applied_ = false;
};

}  // namespace

std::unique_ptr<Command> make_unique_command(Document& doc, uint64_t seq_id,
                                             uint64_t placement_id) {
    if (doc.looks.size() >= kMaxLooks) return nullptr;
    const Sequence* parent = doc.find_sequence(seq_id);
    if (!parent) return nullptr;
    const Placement* p = find_placement(*parent, placement_id);
    if (!p || !p->target) return nullptr;
    if (const Look* target = doc.find_look(p->target)) {
        Look clone = clone_look_for_unique(doc, *target);
        return std::make_unique<MakeUniqueCommand>(
            seq_id, placement_id, std::move(clone), Sequence{}, true);
    }
    if (const Sequence* target = doc.find_sequence(p->target)) {
        Sequence clone = clone_sequence_for_unique(doc, *target);
        return std::make_unique<MakeUniqueCommand>(
            seq_id, placement_id, Look{}, std::move(clone), false);
    }
    return nullptr;
}

std::unique_ptr<Command> nest_layers_command(
    Document& doc, uint64_t look_id, const std::vector<uint64_t>& layer_ids,
    std::string name) {
    if (layer_ids.empty()) return nullptr;
    const Look* parent = doc.find_look(look_id);
    if (!parent) return nullptr;

    auto picked = [&](uint64_t id) {
        for (const uint64_t l : layer_ids)
            if (l == id) return true;
        return false;
    };
    // Keep compositing order when you resolve the selection.
    std::vector<const Layer*> members;
    size_t insert_index = 0;
    bool have_index = false;
    for (size_t i = 0; i < parent->layers.size(); ++i) {
        if (!picked(parent->layers[i].id)) continue;
        if (!have_index) {
            insert_index = i;
            have_index = true;
        }
        members.push_back(&parent->layers[i]);
    }
    if (members.empty()) return nullptr;

    Look nested = make_look(doc, std::move(name));
    for (const Layer* l : members) nested.layers.push_back(*l);
    // A link travels if both ends are inside, or if it feeds the Output.
    // Read the effective table: synthesized wiring must travel too.
    std::vector<NodeLink> synth;
    const std::vector<NodeLink>& table = effective_links(*parent, synth);
    const std::vector<uint64_t> owned = owned_ids(*parent, layer_ids);
    auto inside = [&](uint64_t id) {
        for (const uint64_t o : owned)
            if (o == id) return true;
        return false;
    };
    for (const NodeLink& l : table) {
        if (!inside(l.from)) continue;
        if (inside(l.to) || l.to == 0) nested.links.push_back(l);
    }

    Layer ref;
    ref.id = doc.next_effect_id++;
    ref.name = nested.name;
    ref.source = LayerSourceKind::LookRef;
    ref.target = nested.id;
    ref.node_x = members.front()->node_x;
    ref.node_y = members.front()->node_y;

    return std::make_unique<NestLayersCommand>(look_id, std::move(nested),
                                               std::move(ref), layer_ids,
                                               owned, insert_index);
}

Look make_look(Document& doc, std::string name) {
    Look look;
    look.id = doc.next_effect_id++;
    look.name = name.empty() ? ("look " + std::to_string(doc.looks.size() + 1))
                             : std::move(name);
    return look;
}

Sequence make_sequence(Document& doc, std::string name) {
    Sequence seq;
    seq.id = doc.next_effect_id++;
    seq.name = name.empty()
        ? ("sequence " + std::to_string(doc.sequences.size() + 1))
        : std::move(name);
    SeqTrack lane;
    lane.id = doc.next_effect_id++;
    lane.name = "v1";
    seq.tracks.push_back(std::move(lane));
    AudioTrack atrack;
    atrack.id = doc.next_effect_id++;
    atrack.name = "a1";
    seq.audio.push_back(std::move(atrack));
    return seq;
}

Asset make_asset(Document& doc, std::string name, std::string path) {
    Asset asset;
    asset.id = doc.next_effect_id++;
    asset.name = std::move(name);
    asset.path = std::move(path);
    return asset;
}

Bin make_bin(Document& doc, std::string name) {
    Bin bin;
    bin.id = doc.next_effect_id++;
    bin.name = name.empty()
        ? ("bin " + std::to_string(doc.bins.size() + 1))
        : std::move(name);
    return bin;
}

std::unique_ptr<Command> add_bin_command(Bin bin) {
    return std::make_unique<AddEntityCommand<Bin, &Document::bins>>(
        std::move(bin), "Add Bin");
}

std::unique_ptr<Command> remove_bin_command(uint64_t bin_id) {
    return std::make_unique<RemoveBinCommand>(bin_id);
}

std::unique_ptr<Command> set_bin_props_command(uint64_t bin,
                                               std::string name,
                                               uint64_t parent) {
    return std::make_unique<SetBinPropsCommand>(bin, std::move(name),
                                                parent);
}

std::unique_ptr<Command> set_entity_bin_command(uint64_t entity_id,
                                                uint64_t bin) {
    return std::make_unique<SetEntityBinCommand>(entity_id, bin);
}

std::unique_ptr<Command> add_look_command(Look look) {
    return std::make_unique<AddEntityCommand<Look, &Document::looks>>(
        std::move(look), "Add Look");
}

std::unique_ptr<Command> remove_look_command(uint64_t look_id) {
    return std::make_unique<RemoveEntityCommand<Look, &Document::looks>>(
        look_id, "Remove Look");
}

std::unique_ptr<Command> set_look_props_command(uint64_t look,
                                                std::string name,
                                                uint32_t duration) {
    return std::make_unique<SetLookPropsCommand>(look, std::move(name),
                                                 duration);
}

std::unique_ptr<Command> set_look_audio_split_command(uint64_t look,
                                                      bool split) {
    return std::make_unique<SetLookAudioSplitCommand>(look, split);
}

std::unique_ptr<Command> add_sequence_command(Sequence seq) {
    return std::make_unique<
        AddEntityCommand<Sequence, &Document::sequences>>(std::move(seq),
                                                          "Add Sequence");
}

std::unique_ptr<Command> remove_sequence_command(uint64_t seq_id) {
    return std::make_unique<
        RemoveEntityCommand<Sequence, &Document::sequences>>(
        seq_id, "Remove Sequence");
}

std::unique_ptr<Command> set_sequence_props_command(uint64_t seq,
                                                    std::string name,
                                                    uint32_t duration) {
    return std::make_unique<SetSequencePropsCommand>(seq, std::move(name),
                                                     duration);
}

std::unique_ptr<Command> add_asset_command(Asset asset) {
    return std::make_unique<AddEntityCommand<Asset, &Document::assets>>(
        std::move(asset), "Add Media");
}

std::unique_ptr<Command> remove_asset_command(uint64_t asset_id) {
    return std::make_unique<RemoveEntityCommand<Asset, &Document::assets>>(
        asset_id, "Remove Media");
}

std::unique_ptr<Command> set_asset_command(Asset updated) {
    return std::make_unique<SetAssetCommand>(std::move(updated));
}

}  // namespace looks::doc

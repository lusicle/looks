#include "doc/look_commands.h"

#include <type_traits>

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
        if constexpr (std::is_same_v<T, Sequence>) {
            prev_root_ = doc.root_sequence;
            if (had_ && doc.root_sequence == id_)
                doc.root_sequence =
                    doc.sequences.empty() ? 0 : doc.sequences.front().id;
        }
    }

    void revert(Document& doc) override {
        if (!had_) return;
        std::vector<T>& v = doc.*List;
        const size_t at = std::min(index_, v.size());
        v.insert(v.begin() + static_cast<ptrdiff_t>(at), removed_);
        if constexpr (std::is_same_v<T, Sequence>)
            doc.root_sequence = prev_root_;
    }

private:
    uint64_t id_;
    const char* label_;
    size_t index_ = 0;
    T removed_{};
    uint64_t prev_root_ = 0;
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

    for (Source& l : out.sources) {
        l.id = remint(l.id);
        for (GradientStop& stop : l.stops) stop.id = remint(stop.id);
    }
    for (EffectInstance& fx : out.effects) fx.id = remint(fx.id);
    for (Group& g : out.groups) {
        g.id = remint(g.id);
        for (uint64_t& s : g.inputs) s = remint(s);
    }
    for (CanvasFrame& f : out.frames) f.id = remint(f.id);

    // An id the look does not own passes through unchanged.
    auto mapped_key = [&](ParamKey k) {
        if (k.effect_id & kSourceParamBit)
            k.effect_id = mapped(k.effect_id & ~kSourceParamBit) |
                          kSourceParamBit;
        else if (k.effect_id & kGroupParamBit)
            k.effect_id = mapped(k.effect_id & ~kGroupParamBit) |
                          kGroupParamBit;
        else if (k.effect_id & kStopParamBit)
            k.effect_id = mapped(k.effect_id & ~kStopParamBit) | kStopParamBit;
        else if (k.effect_id)
            k.effect_id = mapped(k.effect_id);
        return k;
    };
    for (EffectInstance& fx : out.effects) fx.group_id = mapped(fx.group_id);
    for (Group& g : out.groups) {
        g.face_out = mapped(g.face_out);
        for (ParamKey& k : g.exposed) k = mapped_key(k);
    }
    for (NodeLink& l : out.links) {
        l.from = mapped(l.from);
        if (l.to) l.to = mapped(l.to);
    }
    std::unordered_map<uint64_t, uint64_t> value_map;
    for (ValueNode& n : out.value_nodes) {
        const uint64_t id = doc.next_route_id++;
        value_map.emplace(n.id, id);
        n.id = id;
    }
    auto mapped_value = [&](uint64_t id) {
        const auto it = value_map.find(id);
        return it == value_map.end() ? id : it->second;
    };
    for (ValueNode& n : out.value_nodes) {
        n.in_a = mapped_value(n.in_a);
        n.in_b = mapped_value(n.in_b);
        n.audio_src = mapped(n.audio_src);
    }
    for (ModRoute& r : out.mod_routes) {
        r.id = doc.next_route_id++;
        r.node = mapped_value(r.node);
        r.target = mapped_key(r.target);
    }
    for (KeyframeLane& lane : out.lanes)
        lane.target = mapped_key(lane.target);
    for (Snapshot& s : out.snapshots)
        for (SnapshotEntry& e : s.entries)
            e.effect_id = mapped_key({e.effect_id, 0}).effect_id;
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
    auto remint_track = [&](auto& t) {
        t.id = doc.next_effect_id++;
        for (Placement& p : t.placements) {
            p.id = doc.next_effect_id++;
            p.link = mapped_link(p.link);
        }
    };
    for_each_lane(out, remint_track);
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

template <class Base>
class SetPropsCommand final : public Base {
public:
    SetPropsCommand(uint64_t id, std::string name, uint32_t duration,
                    const char* label)
        : Base(id), name_(std::move(name)), duration_(duration),
          label_(label) {}
    std::string name() const override { return label_; }

    void apply(Document& doc) override {
        auto& e = this->entity_of(doc);
        old_name_ = e.name;
        old_duration_ = e.duration;
        e.name = name_;
        e.duration = duration_;
    }

    void revert(Document& doc) override {
        auto& e = this->entity_of(doc);
        e.name = old_name_;
        e.duration = old_duration_;
    }

    bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetPropsCommand*>(&next);
        if (!other || !this->same_entity(*other)) return false;
        name_ = other->name_;
        duration_ = other->duration_;
        return true;
    }

private:
    std::string name_;
    uint32_t duration_;
    const char* label_;
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
        Look& look = entity_of(doc);
        old_split_ = look.audio_split;
        look.audio_split = split_;
        old_links_ = look.links;
        if (!split_) look.links.erase(std::remove_if(look.links.begin(), look.links.end(),
            [](const NodeLink& link) { return link.to == 0 && link.to_port == 1; }), look.links.end());
    }

    void revert(Document& doc) override {
        Look& look = entity_of(doc);
        look.audio_split = old_split_;
        look.links = old_links_;
    }

private:
    bool split_;
    bool old_split_ = false;
    std::vector<NodeLink> old_links_;
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


class MakeUniqueCommand final : public SequenceCommand {
public:
    MakeUniqueCommand(uint64_t sequence, uint64_t placement_id, Look look,
                      Sequence seq, bool is_look)
        : SequenceCommand(sequence), placement_id_(placement_id),
          look_(std::move(look)), seq_(std::move(seq)), is_look_(is_look) {}
    std::string name() const override { return "Make Unique"; }

    void apply(Document& doc) override {
        Placement* p = find_placement(entity_of(doc), placement_id_);
        if (!p || !p->target) return;
        old_target_ = p->target;
        p->target = is_look_ ? look_.id : seq_.id;
        if (is_look_) doc.looks.push_back(look_);
        else doc.sequences.push_back(seq_);
        applied_ = true;
    }

    void revert(Document& doc) override {
        if (!applied_) return;
        if (Placement* p = find_placement(entity_of(doc), placement_id_))
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
    return std::make_unique<SetPropsCommand<LookCommand>>(
        look, std::move(name), duration, "Edit Look");
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
    return std::make_unique<SetPropsCommand<SequenceCommand>>(
        seq, std::move(name), duration, "Edit Sequence");
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

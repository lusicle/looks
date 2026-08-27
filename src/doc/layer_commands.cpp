#include "doc/layer_commands.h"

#include <cassert>
#include <utility>

namespace looks::doc {

namespace {

class AddLayerCommand final : public LookCommand {
public:
    AddLayerCommand(uint64_t look, Layer layer, size_t insert_index)
        : LookCommand(look), layer_(std::move(layer)),
          insert_index_(insert_index) {}
    std::string name() const override { return "Add Layer"; }

    void apply(Document& doc) override {
        Look& look = look_of(doc);
        assert(insert_index_ <= look.layers.size());
        look.layers.insert(look.layers.begin() + insert_index_, layer_);
    }

    void revert(Document& doc) override {
        Look& look = look_of(doc);
        look.layers.erase(look.layers.begin() + insert_index_);
    }

private:
    Layer layer_;
    size_t insert_index_;
};

class RemoveLayerCommand final : public LookCommand {
public:
    RemoveLayerCommand(uint64_t look, size_t layer_index)
        : LookCommand(look), layer_index_(layer_index) {}
    std::string name() const override { return "Remove Layer"; }

    void apply(Document& doc) override {
        Look& look = look_of(doc);
        assert(layer_index_ < look.layers.size());
        removed_ = look.layers[layer_index_];
        look.layers.erase(look.layers.begin() + layer_index_);
    }

    void revert(Document& doc) override {
        Look& look = look_of(doc);
        look.layers.insert(look.layers.begin() + layer_index_, removed_);
    }

private:
    size_t layer_index_;
    Layer removed_;
};

class SetLayerPropsCommand final : public LookCommand {
public:
    SetLayerPropsCommand(uint64_t look, Layer updated)
        : LookCommand(look), updated_(std::move(updated)) {}
    std::string name() const override { return "Edit Layer"; }

    void apply(Document& doc) override {
        Layer* l = find_layer(look_of(doc), updated_.id);
        assert(l);
        old_ = *l;
        assign(*l, updated_);
    }

    void revert(Document& doc) override {
        if (Layer* l = find_layer(look_of(doc), updated_.id)) assign(*l, old_);
    }

    bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetLayerPropsCommand*>(&next);
        if (!other || !same_look(*other) || other->updated_.id != updated_.id)
            return false;
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

// One half of a razor: the placement being shortened and the fresh right
// half that resumes at the cut's source frame.
struct PlacementSplit {
    uint64_t left_id = 0;
    Placement right;
};

// RAZOR: a cut is two abutting placements on ONE lane. Nothing is
// cloned and no wiring moves - sequences own no effects, so the cut
// cannot touch state anywhere and razor identity is structural. The
// cut applies to the WHOLE LINK GROUP: a linked audio partner splits at
// the same frame and the right halves link to each other, so picture
// and sound stay lockstep through the edit.
class RazorPlacementCommand final : public SequenceCommand {
public:
    RazorPlacementCommand(uint64_t sequence, uint32_t at,
                          std::vector<PlacementSplit> splits)
        : SequenceCommand(sequence), at_(at), splits_(std::move(splits)) {}
    std::string name() const override { return "Razor"; }

    void apply(Document& doc) override {
        Sequence& seq = sequence_of(doc);
        old_outs_.clear();
        for (const PlacementSplit& s : splits_) {
            PlacementSlot slot;
            if (!find_placement_slot(seq, s.left_id, &slot)) continue;
            std::vector<Placement>* c = slot.list;
            old_outs_.emplace_back(s.left_id, (*c)[slot.index].t_out);
            (*c)[slot.index].t_out = at_;
            c->insert(c->begin() + static_cast<ptrdiff_t>(slot.index + 1),
                      s.right);
        }
    }

    void revert(Document& doc) override {
        Sequence& seq = sequence_of(doc);
        for (const PlacementSplit& s : splits_) {
            PlacementSlot slot;
            if (find_placement_slot(seq, s.right.id, &slot))
                slot.list->erase(slot.list->begin() +
                                 static_cast<ptrdiff_t>(slot.index));
        }
        for (const auto& [id, out] : old_outs_)
            if (Placement* p = find_placement(seq, id)) p->t_out = out;
    }

private:
    uint32_t at_;
    std::vector<PlacementSplit> splits_;
    std::vector<std::pair<uint64_t, uint32_t>> old_outs_;
};

}  // namespace

namespace {

// The latest-starting placement strictly containing `at` - the one the
// track shows there.
const Placement* placement_under(const Document& doc,
                                 const std::vector<Placement>& placements,
                                 uint32_t at, double parent_fps) {
    const Placement* place = nullptr;
    for (const Placement& p : placements) {
        const uint32_t len = source_length(doc, p);
        const uint32_t end =
            placement_end(p, len, placement_ratio(doc, p, parent_fps));
        if (at <= p.t_in || (end && at >= end)) continue;
        if (!place || p.t_in >= place->t_in) place = &p;
    }
    return place;
}

// The split for one placement: same target, fresh id, resuming at the
// cut's source frame so the halves are continuous.
PlacementSplit split_of(Document& doc, const Placement& place, uint32_t at,
                        double parent_fps) {
    PlacementSplit s;
    s.left_id = place.id;
    s.right = place;
    s.right.id = doc.next_effect_id++;
    s.right.t_out = place.t_out;   // 0 stays "to the source end"
    trim_placement_head(s.right, at, placement_ratio(doc, place, parent_fps));
    return s;
}

// The whole group's splits: the named placement plus every link partner
// the cut lands strictly inside. Right halves link to each other.
std::unique_ptr<Command> razor_group(Document& doc, uint64_t seq_id,
                                     const Placement& primary, uint32_t at) {
    const double eff = effective_fps(doc, doc.sequence(seq_id));
    std::vector<PlacementSplit> splits;
    splits.push_back(split_of(doc, primary, at, eff));
    if (primary.link) {
        const Sequence& seq = doc.sequence(seq_id);
        auto try_partner = [&](const Placement& p) {
            if (p.link != primary.link || p.id == primary.id) return;
            const uint32_t len = source_length(doc, p);
            const uint32_t end =
                placement_end(p, len, placement_ratio(doc, p, eff));
            if (at <= p.t_in || (end && at >= end)) return;
            splits.push_back(split_of(doc, p, at, eff));
        };
        for (const SeqTrack& t : seq.tracks)
            for (const Placement& p : t.placements) try_partner(p);
        for (const AudioTrack& t : seq.audio)
            for (const Placement& p : t.placements) try_partner(p);
    }
    if (splits.size() >= 2) {
        const uint64_t fresh_link = doc.next_effect_id++;
        for (PlacementSplit& s : splits) s.right.link = fresh_link;
    } else {
        splits[0].right.link = 0;
    }
    return std::make_unique<RazorPlacementCommand>(seq_id, at,
                                                   std::move(splits));
}

}  // namespace

std::unique_ptr<Command> razor_track_command(Document& doc, uint64_t seq_id,
                                             uint64_t track_id, uint32_t at) {
    const Sequence& seq = doc.sequence(seq_id);
    const SeqTrack* src = nullptr;
    for (const SeqTrack& t : seq.tracks)
        if (t.id == track_id) src = &t;
    if (!src) return nullptr;
    if (src->placements.size() >= kMaxPlacementsPerTrack) return nullptr;
    const Placement* place = placement_under(
        doc, src->placements, at, effective_fps(doc, seq));
    if (!place) return nullptr;
    return razor_group(doc, seq_id, *place, at);
}

std::unique_ptr<Command> razor_audio_command(Document& doc, uint64_t seq_id,
                                             uint64_t track_id, uint32_t at) {
    const Sequence& seq = doc.sequence(seq_id);
    const AudioTrack* track = nullptr;
    for (const AudioTrack& t : seq.audio)
        if (t.id == track_id) track = &t;
    if (!track) return nullptr;
    if (track->placements.size() >= kMaxPlacementsPerTrack) return nullptr;
    const Placement* place = placement_under(
        doc, track->placements, at, effective_fps(doc, seq));
    if (!place) return nullptr;
    return razor_group(doc, seq_id, *place, at);
}

Layer make_layer(Document& doc, LayerSourceKind kind) {
    Layer layer;
    layer.id = doc.next_effect_id++;
    layer.source = kind;
    static const char* kNames[] = {"media", "solid", "gradient",
                                   "noise", "pattern", "osc",
                                   "shape", "look",  "sequence"};
    static_assert(sizeof(kNames) / sizeof(kNames[0]) ==
                      static_cast<size_t>(LayerSourceKind::Count),
                  "layer names track the enum");
    layer.name = std::string(kNames[static_cast<size_t>(kind)]) + " " +
                 std::to_string(layer.id);
    // A media node binds to the project's first asset by default; the
    // browser retargets it. Nested refs bind when the caller names the
    // entity (and checks nest_reaches first).
    if (kind == LayerSourceKind::Media && !doc.assets.empty())
        layer.asset = doc.assets.front().id;
    // Generators default to half opacity so adding one doesn't blank the
    // composite.
    if (kind == LayerSourceKind::Solid || kind == LayerSourceKind::Gradient ||
        kind == LayerSourceKind::Noise ||
        kind == LayerSourceKind::TestPattern ||
        kind == LayerSourceKind::Oscillator)
        layer.opacity = 0.5f;
    if (kind == LayerSourceKind::Oscillator) {
        // Oscillator frequency reads in cycles, not px — a usable default.
        layer.gen_scale = 8.0f;
        layer.color_b[0] = layer.color_b[1] = layer.color_b[2] = 0.0f;
        layer.color_a[0] = layer.color_a[1] = layer.color_a[2] = 1.0f;
    }
    if (kind == LayerSourceKind::TestPattern) {
        // Checkerboard at a readable cell size, white on black.
        layer.gen_scale = 64.0f;
        layer.color_a[0] = layer.color_a[1] = layer.color_a[2] = 1.0f;
        layer.color_b[0] = layer.color_b[1] = layer.color_b[2] = 0.0f;
    }
    if (kind == LayerSourceKind::Shape) {
        // A matte maker: opaque coverage on transparent, a visible size,
        // a soft edge (gen_scale = size, gen_angle = feather — see
        // gen.comp.slang).
        layer.gen_scale = 6.0f;
        layer.gen_angle = 0.35f;
        layer.color_a[0] = layer.color_a[1] = layer.color_a[2] = 1.0f;
        layer.color_b[0] = layer.color_b[1] = layer.color_b[2] = 0.0f;
    }
    return layer;
}

std::unique_ptr<Command> add_layer_command(uint64_t look, Layer layer,
                                           size_t insert_index) {
    return std::make_unique<AddLayerCommand>(look, std::move(layer),
                                             insert_index);
}

std::unique_ptr<Command> remove_layer_command(uint64_t look,
                                              size_t layer_index) {
    return std::make_unique<RemoveLayerCommand>(look, layer_index);
}

std::unique_ptr<Command> set_layer_props_command(uint64_t look, Layer updated) {
    return std::make_unique<SetLayerPropsCommand>(look, std::move(updated));
}

}  // namespace looks::doc

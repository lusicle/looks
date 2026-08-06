#include "doc/placement_commands.h"

#include <algorithm>
#include <utility>

namespace looks::doc {

namespace {

// Every placement in the sequence sharing `link` (nonzero), the named one
// included. The group is how one drop's picture and sound stay lockstep.
std::vector<Placement*> group_members(Sequence& seq, uint64_t link) {
    std::vector<Placement*> out;
    if (!link) return out;
    for (SeqTrack& t : seq.tracks)
        for (Placement& p : t.placements)
            if (p.link == link) out.push_back(&p);
    for (AudioTrack& t : seq.audio)
        for (Placement& p : t.placements)
            if (p.link == link) out.push_back(&p);
    return out;
}

class AddPlacementCommand final : public SequenceCommand {
public:
    AddPlacementCommand(uint64_t sequence, uint64_t track_id,
                        Placement place)
        : SequenceCommand(sequence), track_id_(track_id), place_(place) {}
    std::string name() const override { return "Place Block"; }

    void apply(Document& doc) override {
        for (SeqTrack& t : sequence_of(doc).tracks)
            if (t.id == track_id_) {
                t.placements.push_back(place_);
                return;
            }
    }

    void revert(Document& doc) override {
        for (SeqTrack& t : sequence_of(doc).tracks) {
            if (t.id != track_id_) continue;
            for (size_t i = t.placements.size(); i-- > 0;)
                if (t.placements[i].id == place_.id)
                    t.placements.erase(t.placements.begin() +
                                       static_cast<ptrdiff_t>(i));
        }
    }

private:
    uint64_t track_id_;
    Placement place_;
};

class SetPlacementCommand final : public SequenceCommand {
public:
    SetPlacementCommand(uint64_t sequence, Placement updated)
        : SequenceCommand(sequence), updated_(updated) {}
    std::string name() const override { return "Edit Placement"; }

    void apply(Document& doc) override {
        Sequence& seq = sequence_of(doc);
        Placement* target = find_placement(seq, updated_.id);
        if (!target) return;
        old_.clear();
        old_.emplace_back(target->id, *target);
        // Timing travels with the group; identity and target do not.
        for (Placement* p : group_members(seq, target->link)) {
            if (p->id == target->id) continue;
            old_.emplace_back(p->id, *p);
            p->t_in = updated_.t_in;
            p->t_out = updated_.t_out;
            p->source_in = updated_.source_in;
            p->speed = updated_.speed;
        }
        const uint64_t id = target->id;
        *target = updated_;
        target->id = id;   // identity is not editable
    }

    void revert(Document& doc) override {
        Sequence& seq = sequence_of(doc);
        for (const auto& [id, place] : old_)
            if (Placement* p = find_placement(seq, id)) *p = place;
    }

    bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetPlacementCommand*>(&next);
        if (!other || !same_sequence(*other) ||
            other->updated_.id != updated_.id)
            return false;
        updated_ = other->updated_;
        return true;
    }

private:
    Placement updated_;
    std::vector<std::pair<uint64_t, Placement>> old_;
};

class UnlinkPlacementCommand final : public SequenceCommand {
public:
    UnlinkPlacementCommand(uint64_t sequence, uint64_t placement_id)
        : SequenceCommand(sequence), placement_id_(placement_id) {}
    std::string name() const override { return "Unlink"; }

    void apply(Document& doc) override {
        Sequence& seq = sequence_of(doc);
        Placement* target = find_placement(seq, placement_id_);
        if (!target || !target->link) return;
        link_ = target->link;
        members_.clear();
        for (Placement* p : group_members(seq, link_)) {
            members_.push_back(p->id);
            p->link = 0;
        }
    }

    void revert(Document& doc) override {
        Sequence& seq = sequence_of(doc);
        for (const uint64_t id : members_)
            if (Placement* p = find_placement(seq, id)) p->link = link_;
    }

private:
    uint64_t placement_id_;
    uint64_t link_ = 0;
    std::vector<uint64_t> members_;
};

class RemovePlacementCommand final : public SequenceCommand {
public:
    RemovePlacementCommand(uint64_t sequence, uint64_t placement_id)
        : SequenceCommand(sequence), placement_id_(placement_id) {}
    std::string name() const override { return "Remove Block"; }

    void apply(Document& doc) override {
        Sequence& seq = sequence_of(doc);
        removed_.clear();
        Placement* target = find_placement(seq, placement_id_);
        if (!target) return;
        const uint64_t link = target->link;
        // The whole LINK GROUP goes: picture and sound leave together,
        // exactly as one drop laid them down. Descending index order
        // keeps every captured index valid at its own removal.
        auto take = [&](std::vector<Placement>& list, bool audio,
                        uint64_t container) {
            for (size_t i = list.size(); i-- > 0;) {
                const Placement& p = list[i];
                if (p.id != placement_id_ && !(link && p.link == link))
                    continue;
                removed_.push_back({audio, container, i, p});
                list.erase(list.begin() + static_cast<ptrdiff_t>(i));
            }
        };
        for (SeqTrack& t : seq.tracks) take(t.placements, false, t.id);
        for (AudioTrack& t : seq.audio) take(t.placements, true, t.id);
    }

    void revert(Document& doc) override {
        Sequence& seq = sequence_of(doc);
        // Reverse capture order: within a list the lowest index was
        // captured last, so it re-inserts first and each later one
        // lands exactly where it was.
        for (size_t k = removed_.size(); k-- > 0;) {
            const Slot& s = removed_[k];
            std::vector<Placement>* list = nullptr;
            if (s.audio) {
                for (AudioTrack& t : seq.audio)
                    if (t.id == s.container) list = &t.placements;
            } else {
                for (SeqTrack& t : seq.tracks)
                    if (t.id == s.container) list = &t.placements;
            }
            if (!list) continue;
            list->insert(
                list->begin() +
                    static_cast<ptrdiff_t>(std::min(s.index, list->size())),
                s.place);
        }
    }

private:
    struct Slot {
        bool audio;
        uint64_t container;
        size_t index;
        Placement place;
    };
    uint64_t placement_id_;
    std::vector<Slot> removed_;
};

class AddAudioPlacementCommand final : public SequenceCommand {
public:
    AddAudioPlacementCommand(uint64_t sequence, uint64_t track_id,
                             AudioTrack fresh_track, Placement place,
                             uint64_t video_placement, uint64_t link_id)
        : SequenceCommand(sequence), track_id_(track_id),
          fresh_track_(std::move(fresh_track)), place_(place),
          video_placement_(video_placement), link_id_(link_id) {}
    std::string name() const override { return "Add Audio"; }

    void apply(Document& doc) override {
        Sequence& seq = sequence_of(doc);
        AudioTrack* track = nullptr;
        minted_track_ = false;
        for (AudioTrack& t : seq.audio)
            if (t.id == track_id_) track = &t;
        if (!track) {
            seq.audio.push_back(fresh_track_);
            track = &seq.audio.back();
            minted_track_ = true;
        }
        Placement p = place_;
        // The pair links iff both ends exist; sound alone lays unlinked.
        if (video_placement_) {
            if (Placement* v = find_placement(seq, video_placement_)) {
                v->link = link_id_;
                p.link = link_id_;
            }
        }
        track->placements.push_back(p);
    }

    void revert(Document& doc) override {
        Sequence& seq = sequence_of(doc);
        if (video_placement_)
            if (Placement* v = find_placement(seq, video_placement_))
                if (v->link == link_id_) v->link = 0;
        for (size_t t = 0; t < seq.audio.size(); ++t) {
            AudioTrack& track = seq.audio[t];
            if (track.id != (minted_track_ ? fresh_track_.id : track_id_))
                continue;
            for (size_t i = 0; i < track.placements.size(); ++i)
                if (track.placements[i].id == place_.id) {
                    track.placements.erase(track.placements.begin() +
                                           static_cast<ptrdiff_t>(i));
                    break;
                }
            if (minted_track_ && track.placements.empty())
                seq.audio.erase(seq.audio.begin() +
                                static_cast<ptrdiff_t>(t));
            break;
        }
    }

private:
    uint64_t track_id_;
    AudioTrack fresh_track_;
    Placement place_;
    uint64_t video_placement_;
    uint64_t link_id_;
    bool minted_track_ = false;
};

class SetAudioTrackPropsCommand final : public SequenceCommand {
public:
    SetAudioTrackPropsCommand(uint64_t sequence, uint64_t track_id,
                              std::string name, float gain, bool mute)
        : SequenceCommand(sequence), track_id_(track_id),
          name_(std::move(name)), gain_(gain), mute_(mute) {}
    std::string name() const override { return "Edit Audio Track"; }

    void apply(Document& doc) override {
        for (AudioTrack& t : sequence_of(doc).audio) {
            if (t.id != track_id_) continue;
            old_name_ = t.name;
            old_gain_ = t.gain;
            old_mute_ = t.mute;
            t.name = name_;
            t.gain = gain_;
            t.mute = mute_;
        }
    }

    void revert(Document& doc) override {
        for (AudioTrack& t : sequence_of(doc).audio) {
            if (t.id != track_id_) continue;
            t.name = old_name_;
            t.gain = old_gain_;
            t.mute = old_mute_;
        }
    }

    bool merge(const Command& next) override {
        const auto* other =
            dynamic_cast<const SetAudioTrackPropsCommand*>(&next);
        if (!other || !same_sequence(*other) ||
            other->track_id_ != track_id_)
            return false;
        name_ = other->name_;
        gain_ = other->gain_;
        mute_ = other->mute_;
        return true;
    }

private:
    uint64_t track_id_;
    std::string name_;
    float gain_;
    bool mute_;
    std::string old_name_;
    float old_gain_ = 1.0f;
    bool old_mute_ = false;
};

}  // namespace

std::unique_ptr<Command> add_placement_command(uint64_t sequence,
                                               uint64_t track_id,
                                               Placement place) {
    return std::make_unique<AddPlacementCommand>(sequence, track_id, place);
}

std::unique_ptr<Command> set_placement_command(uint64_t sequence,
                                               Placement updated) {
    return std::make_unique<SetPlacementCommand>(sequence, updated);
}

std::unique_ptr<Command> unlink_placement_command(uint64_t sequence,
                                                  uint64_t placement_id) {
    return std::make_unique<UnlinkPlacementCommand>(sequence, placement_id);
}

std::unique_ptr<Command> remove_placement_command(uint64_t sequence,
                                                  uint64_t placement_id) {
    return std::make_unique<RemovePlacementCommand>(sequence, placement_id);
}

std::unique_ptr<Command> add_audio_placement_command(
    Document& doc, uint64_t sequence_id, uint64_t track_id, Placement place,
    uint64_t video_placement) {
    const Sequence* seq = doc.find_sequence(sequence_id);
    if (!seq) return nullptr;
    if (!place.id) place.id = doc.next_effect_id++;
    AudioTrack fresh;
    if (!track_id) {
        fresh.id = doc.next_effect_id++;
        fresh.name = "a" + std::to_string(seq->audio.size() + 1);
    }
    const uint64_t link_id =
        video_placement ? doc.next_effect_id++ : 0;
    return std::make_unique<AddAudioPlacementCommand>(
        sequence_id, track_id, std::move(fresh), place, video_placement,
        link_id);
}

std::unique_ptr<Command> set_audio_track_props_command(uint64_t sequence,
                                                       uint64_t track_id,
                                                       std::string name,
                                                       float gain,
                                                       bool mute) {
    return std::make_unique<SetAudioTrackPropsCommand>(
        sequence, track_id, std::move(name), gain, mute);
}

}  // namespace looks::doc

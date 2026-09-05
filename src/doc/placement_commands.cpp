#include "doc/placement_commands.h"

#include <algorithm>
#include <utility>

#include "doc/layer_commands.h"

namespace looks::doc {

namespace {

std::vector<Placement>* track_placements(Sequence& seq, uint64_t track_id,
                                         bool audio) {
    if (audio) {
        for (AudioTrack& t : seq.audio)
            if (t.id == track_id) return &t.placements;
    } else {
        for (SeqTrack& t : seq.tracks)
            if (t.id == track_id) return &t.placements;
    }
    return nullptr;
}

// A link of 0 has no group: this returns an empty list.
std::vector<Placement*> group_members(Sequence& seq, uint64_t link) {
    std::vector<Placement*> out;
    if (!link) return out;
    for_each_lane(seq, [&](auto& t) {
        for (Placement& p : t.placements)
            if (p.link == link) out.push_back(&p);
    });
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
        // Timing fields travel with the group. Identity and target do not.
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
        target->id = id;   // the id is not editable
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
        // Remove in descending index order to keep each index valid.
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
        // Insert in reverse capture order to put each one back at its index.
        for (size_t k = removed_.size(); k-- > 0;) {
            const Slot& s = removed_[k];
            std::vector<Placement>* list =
                track_placements(seq, s.container, s.audio);
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
        // The pair links only if both ends are there. Sound alone is free.
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
                              std::string name, float gain, bool mute,
                              bool lock)
        : SequenceCommand(sequence), track_id_(track_id),
          name_(std::move(name)), gain_(gain), mute_(mute), lock_(lock) {}
    std::string name() const override { return "Edit Audio Track"; }

    void apply(Document& doc) override {
        for (AudioTrack& t : sequence_of(doc).audio) {
            if (t.id != track_id_) continue;
            old_name_ = t.name;
            old_gain_ = t.gain;
            old_mute_ = t.mute;
            old_lock_ = t.lock;
            t.name = name_;
            t.gain = gain_;
            t.mute = mute_;
            t.lock = lock_;
        }
    }

    void revert(Document& doc) override {
        for (AudioTrack& t : sequence_of(doc).audio) {
            if (t.id != track_id_) continue;
            t.name = old_name_;
            t.gain = old_gain_;
            t.mute = old_mute_;
            t.lock = old_lock_;
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
        lock_ = other->lock_;
        return true;
    }

private:
    uint64_t track_id_;
    std::string name_;
    float gain_;
    bool mute_;
    bool lock_;
    std::string old_name_;
    float old_gain_ = 1.0f;
    bool old_mute_ = false;
    bool old_lock_ = false;
};

class SetTrackPropsCommand final : public SequenceCommand {
public:
    SetTrackPropsCommand(uint64_t sequence, uint64_t track_id,
                         std::string name, bool hidden, bool lock)
        : SequenceCommand(sequence), track_id_(track_id),
          name_(std::move(name)), hidden_(hidden), lock_(lock) {}
    std::string name() const override { return "Edit Lane"; }

    void apply(Document& doc) override {
        for (SeqTrack& t : sequence_of(doc).tracks) {
            if (t.id != track_id_) continue;
            old_name_ = t.name;
            old_hidden_ = t.hidden;
            old_lock_ = t.lock;
            t.name = name_;
            t.hidden = hidden_;
            t.lock = lock_;
        }
    }

    void revert(Document& doc) override {
        for (SeqTrack& t : sequence_of(doc).tracks) {
            if (t.id != track_id_) continue;
            t.name = old_name_;
            t.hidden = old_hidden_;
            t.lock = old_lock_;
        }
    }

private:
    uint64_t track_id_;
    std::string name_;
    bool hidden_;
    bool lock_;
    std::string old_name_;
    bool old_hidden_ = false;
    bool old_lock_ = false;
};

class MovePlacementCommand final : public SequenceCommand {
public:
    MovePlacementCommand(uint64_t sequence, uint64_t placement_id,
                         uint64_t to_track_id)
        : SequenceCommand(sequence), placement_id_(placement_id),
          to_track_(to_track_id) {}
    std::string name() const override { return "Move Block"; }

    void apply(Document& doc) override {
        Sequence& seq = sequence_of(doc);
        PlacementSlot from;
        if (!find_placement_slot(seq, placement_id_, &from)) return;
        std::vector<Placement>* dst =
            track_placements(seq, to_track_, from.audio);
        if (!dst || dst == from.list) return;
        from_track_ = from.track_id;
        from_index_ = from.index;
        moved_ = true;
        Placement p = (*from.list)[from.index];
        from.list->erase(from.list->begin() +
                         static_cast<ptrdiff_t>(from.index));
        dst->push_back(p);
    }

    void revert(Document& doc) override {
        if (!moved_) return;
        Sequence& seq = sequence_of(doc);
        PlacementSlot at;
        if (!find_placement_slot(seq, placement_id_, &at)) return;
        Placement p = (*at.list)[at.index];
        at.list->erase(at.list->begin() + static_cast<ptrdiff_t>(at.index));
        std::vector<Placement>* src =
            track_placements(seq, from_track_, at.audio);
        if (!src) return;
        src->insert(src->begin() + static_cast<ptrdiff_t>(
                        std::min(from_index_, src->size())),
                    p);
    }

private:
    uint64_t placement_id_;
    uint64_t to_track_;
    uint64_t from_track_ = 0;
    size_t from_index_ = 0;
    bool moved_ = false;
};

template <class Track, std::vector<Track> Sequence::*Member>
class AddTrackCommand final : public SequenceCommand {
public:
    AddTrackCommand(uint64_t sequence, Track track, size_t at_index,
                    const char* label)
        : SequenceCommand(sequence), track_(std::move(track)),
          at_index_(at_index), label_(label) {}
    std::string name() const override { return label_; }

    void apply(Document& doc) override {
        auto& tracks = sequence_of(doc).*Member;
        tracks.insert(tracks.begin() + static_cast<ptrdiff_t>(
                          std::min(at_index_, tracks.size())),
                      track_);
    }

    void revert(Document& doc) override {
        auto& tracks = sequence_of(doc).*Member;
        for (size_t i = tracks.size(); i-- > 0;)
            if (tracks[i].id == track_.id)
                tracks.erase(tracks.begin() + static_cast<ptrdiff_t>(i));
    }

private:
    Track track_;
    size_t at_index_;
    const char* label_;
};

// Revert clamps the index: the vector can shrink between apply and revert.
template <class Track, std::vector<Track> Sequence::*Member>
class RemoveTrackCommand final : public SequenceCommand {
public:
    RemoveTrackCommand(uint64_t sequence, uint64_t track_id,
                       const char* label)
        : SequenceCommand(sequence), track_id_(track_id), label_(label) {}
    std::string name() const override { return label_; }

    void apply(Document& doc) override {
        auto& tracks = sequence_of(doc).*Member;
        for (size_t i = 0; i < tracks.size(); ++i)
            if (tracks[i].id == track_id_) {
                index_ = i;
                removed_ = std::move(tracks[i]);
                tracks.erase(tracks.begin() + static_cast<ptrdiff_t>(i));
                return;
            }
    }

    void revert(Document& doc) override {
        auto& tracks = sequence_of(doc).*Member;
        tracks.insert(tracks.begin() + static_cast<ptrdiff_t>(
                          std::min(index_, tracks.size())),
                      removed_);
    }

private:
    uint64_t track_id_;
    const char* label_;
    Track removed_;
    size_t index_ = 0;
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
                                                       bool mute,
                                                       bool lock) {
    return std::make_unique<SetAudioTrackPropsCommand>(
        sequence, track_id, std::move(name), gain, mute, lock);
}

std::unique_ptr<Command> set_track_props_command(uint64_t sequence,
                                                 uint64_t track_id,
                                                 std::string name,
                                                 bool hidden, bool lock) {
    return std::make_unique<SetTrackPropsCommand>(
        sequence, track_id, std::move(name), hidden, lock);
}

std::unique_ptr<Command> move_placement_command(const Document& doc,
                                                uint64_t sequence,
                                                uint64_t placement_id,
                                                uint64_t to_track_id) {
    const Sequence* seq = doc.find_sequence(sequence);
    if (!seq) return nullptr;
    Sequence& mut = const_cast<Sequence&>(*seq);
    PlacementSlot from;
    if (!find_placement_slot(mut, placement_id, &from)) return nullptr;
    if (from.track_id == to_track_id) return nullptr;
    if (!track_placements(mut, to_track_id, from.audio)) return nullptr;
    return std::make_unique<MovePlacementCommand>(sequence, placement_id,
                                                  to_track_id);
}

SeqTrack make_track(Document& doc, const Sequence& seq) {
    SeqTrack lane;
    lane.id = doc.next_effect_id++;
    lane.name = "v" + std::to_string(seq.tracks.size() + 1);
    return lane;
}

AudioTrack make_audio_track(Document& doc, const Sequence& seq) {
    AudioTrack track;
    track.id = doc.next_effect_id++;
    track.name = "a" + std::to_string(seq.audio.size() + 1);
    return track;
}

std::unique_ptr<Command> add_track_command(uint64_t sequence, SeqTrack track,
                                           size_t at_index) {
    return std::make_unique<AddTrackCommand<SeqTrack, &Sequence::tracks>>(
        sequence, std::move(track), at_index, "Add Lane");
}

std::unique_ptr<Command> remove_track_command(const Document& doc,
                                              uint64_t sequence,
                                              uint64_t track_id) {
    const Sequence* seq = doc.find_sequence(sequence);
    if (!seq || seq->tracks.size() <= 1) return nullptr;
    bool found = false;
    for (const SeqTrack& t : seq->tracks)
        if (t.id == track_id) found = true;
    if (!found) return nullptr;
    return std::make_unique<
        RemoveTrackCommand<SeqTrack, &Sequence::tracks>>(sequence, track_id,
                                                         "Remove Lane");
}

std::unique_ptr<Command> add_audio_track_command(uint64_t sequence,
                                                 AudioTrack track) {
    return std::make_unique<AddTrackCommand<AudioTrack, &Sequence::audio>>(
        sequence, std::move(track), SIZE_MAX, "Add Audio Track");
}

std::unique_ptr<Command> remove_audio_track_command(uint64_t sequence,
                                                    uint64_t track_id) {
    return std::make_unique<
        RemoveTrackCommand<AudioTrack, &Sequence::audio>>(
        sequence, track_id, "Remove Audio Track");
}

void overwrite_lane_span(Document& doc, UndoStack& undo, uint64_t sequence,
                         uint64_t track_id, uint64_t keep_id,
                         uint64_t keep_link, uint32_t t0, uint32_t t1) {
    const uint64_t e1 = t1 ? t1 : UINT64_MAX;
    bool audio_lane = false;
    auto lane_of = [&](const Sequence& seq)
        -> const std::vector<Placement>* {
        for (const SeqTrack& t : seq.tracks)
            if (t.id == track_id) return &t.placements;
        for (const AudioTrack& t : seq.audio)
            if (t.id == track_id) {
                audio_lane = true;
                return &t.placements;
            }
        return nullptr;
    };
    // Snapshot the victims first: each edit below reshapes the lane.
    struct Victim {
        uint64_t id;
        uint32_t t_in;
        uint64_t end;   // an end of UINT64_MAX is unbounded
    };
    std::vector<Victim> victims;
    // The fps stays the same below: these commands only edit placements.
    double eff = 0.0;
    {
        const Sequence* seq = doc.find_sequence(sequence);
        if (!seq) return;
        const std::vector<Placement>* lane = lane_of(*seq);
        if (!lane) return;
        eff = effective_fps(doc, *seq);
        for (const Placement& p : *lane) {
            if (p.id == keep_id) continue;
            if (keep_link && p.link == keep_link) continue;
            const uint32_t pe32 = placement_end(
                p, source_length(doc, p), placement_ratio(doc, p, eff));
            const uint64_t pe = pe32 ? pe32 : UINT64_MAX;
            if (p.t_in >= e1 || pe <= t0) continue;
            victims.push_back({p.id, p.t_in, pe});
        }
    }
    for (Victim v : victims) {
        // A group edit can reshape this one, thus read the live span again.
        {
            const Sequence* seq = doc.find_sequence(sequence);
            const Placement* live =
                seq ? find_placement(*seq, v.id) : nullptr;
            if (!live) continue;
            const uint32_t le32 = placement_end(
                *live, source_length(doc, *live),
                placement_ratio(doc, *live, eff));
            const uint64_t le = le32 ? le32 : UINT64_MAX;
            if (live->t_in >= e1 || le <= t0) continue;
            v.t_in = live->t_in;
            v.end = le;
        }
        if (v.t_in >= t0 && v.end <= e1) {
            undo.execute(doc, remove_placement_command(sequence, v.id));
            continue;
        }
        if (v.t_in < t0 && v.end > e1) {
            auto cut = audio_lane
                ? razor_audio_command(doc, sequence, track_id, t0)
                : razor_track_command(doc, sequence, track_id, t0);
            if (cut)
                undo.execute(doc, std::move(cut));
            else
                continue;   // at the lane bound: the overlap stays
            const Sequence* seq = doc.find_sequence(sequence);
            const std::vector<Placement>* lane = lane_of(*seq);
            const Placement* right = nullptr;
            if (lane)
                for (const Placement& p : *lane)
                    if (p.t_in == t0 && p.id != keep_id &&
                        !(keep_link && p.link == keep_link))
                        right = &p;
            if (!right) continue;
            Placement np = *right;
            trim_placement_head(np, t1, placement_ratio(doc, np, eff));
            undo.execute(doc, set_placement_command(sequence, np));
            continue;
        }
        const Sequence* seq = doc.find_sequence(sequence);
        const Placement* p = seq ? find_placement(*seq, v.id) : nullptr;
        if (!p) continue;
        Placement np = *p;
        if (v.t_in < t0) {
            np.t_out = t0;
        } else {
            // trim_placement_head moves source_in, thus content holds still.
            trim_placement_head(np, t1, placement_ratio(doc, np, eff));
        }
        undo.execute(doc, set_placement_command(sequence, np));
    }
}

void overwrite_group_spans(Document& doc, UndoStack& undo,
                           uint64_t sequence, uint64_t placement_id) {
    Sequence* seq = doc.find_sequence(sequence);
    if (!seq) return;
    const Placement* landed = find_placement(*seq, placement_id);
    if (!landed) return;
    // Snapshot the members first: each overwrite reshapes the sequence.
    struct Member {
        uint64_t track;
        uint64_t id;
    };
    std::vector<Member> members;
    const uint64_t link = landed->link;
    auto collect = [&](const std::vector<Placement>& list, uint64_t track) {
        for (const Placement& p : list)
            if (p.id == placement_id || (link && p.link == link))
                members.push_back({track, p.id});
    };
    for (const SeqTrack& t : seq->tracks) collect(t.placements, t.id);
    for (const AudioTrack& t : seq->audio) collect(t.placements, t.id);
    for (const Member& m : members) {
        const Placement* p = find_placement(*seq, m.id);
        if (!p) continue;
        const uint32_t end = placement_end(
            *p, source_length(doc, *p),
            placement_ratio(doc, *p, effective_fps(doc, *seq)));
        overwrite_lane_span(doc, undo, sequence, m.track, p->id, p->link,
                            p->t_in, end);
    }
}

}  // namespace looks::doc

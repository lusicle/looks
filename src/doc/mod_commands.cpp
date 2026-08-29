#include "doc/mod_commands.h"

#include <algorithm>
#include <cassert>
#include <utility>

namespace looks::doc {

namespace {

ModRoute* find_route(Look& look, uint64_t route_id) {
    for (ModRoute& r : look.mod_routes)
        if (r.id == route_id) return &r;
    return nullptr;
}

class SetMorphCommand final : public LookCommand {
public:
    SetMorphCommand(uint64_t look, int from, int to, float pos)
        : LookCommand(look), from_(from), to_(to), pos_(pos) {}
    std::string name() const override { return "Set Morph"; }

    void apply(Document& doc) override {
        Look& look = look_of(doc);
        old_from_ = look.morph_from;
        old_to_ = look.morph_to;
        old_pos_ = look.morph_pos;
        look.morph_from = from_;
        look.morph_to = to_;
        look.morph_pos = pos_;
    }

    void revert(Document& doc) override {
        Look& look = look_of(doc);
        look.morph_from = old_from_;
        look.morph_to = old_to_;
        look.morph_pos = old_pos_;
    }

    bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetMorphCommand*>(&next);
        if (!other || !same_look(*other)) return false;
        from_ = other->from_;
        to_ = other->to_;
        pos_ = other->pos_;
        return true;
    }

private:
    int from_, to_;
    float pos_;
    int old_from_ = 0, old_to_ = 1;
    float old_pos_ = 0.0f;
};

class SetTimelineRegionCommand final : public SequenceCommand {
public:
    SetTimelineRegionCommand(uint64_t sequence, uint32_t trim_in,
                             uint32_t trim_out, uint32_t loop_in,
                             uint32_t loop_out)
        : SequenceCommand(sequence), trim_in_(trim_in), trim_out_(trim_out),
          loop_in_(loop_in), loop_out_(loop_out) {}
    std::string name() const override { return "Edit Timeline Region"; }

    void apply(Document& doc) override {
        Sequence& seq = sequence_of(doc);
        old_[0] = seq.trim_in;
        old_[1] = seq.trim_out;
        old_[2] = seq.loop_in;
        old_[3] = seq.loop_out;
        seq.trim_in = trim_in_;
        seq.trim_out = trim_out_;
        seq.loop_in = loop_in_;
        seq.loop_out = loop_out_;
    }

    void revert(Document& doc) override {
        Sequence& seq = sequence_of(doc);
        seq.trim_in = old_[0];
        seq.trim_out = old_[1];
        seq.loop_in = old_[2];
        seq.loop_out = old_[3];
    }

    bool merge(const Command& next) override {
        const auto* other =
            dynamic_cast<const SetTimelineRegionCommand*>(&next);
        if (!other || !same_sequence(*other)) return false;
        trim_in_ = other->trim_in_;
        trim_out_ = other->trim_out_;
        loop_in_ = other->loop_in_;
        loop_out_ = other->loop_out_;
        return true;
    }

private:
    uint32_t trim_in_, trim_out_, loop_in_, loop_out_;
    uint32_t old_[4] = {};
};

class SetLaneLoopCommand final : public LookCommand {
public:
    SetLaneLoopCommand(uint64_t look, ParamKey target, bool loop)
        : LookCommand(look), target_(target), loop_(loop) {}
    std::string name() const override {
        return loop_ ? "Loop Keyframes" : "Unloop Keyframes";
    }

    void apply(Document& doc) override {
        for (KeyframeLane& lane : look_of(doc).lanes) {
            if (!(lane.target == target_)) continue;
            old_ = lane.loop;
            lane.loop = loop_;
        }
    }

    void revert(Document& doc) override {
        for (KeyframeLane& lane : look_of(doc).lanes)
            if (lane.target == target_) lane.loop = old_;
    }

private:
    ParamKey target_;
    bool loop_;
    bool old_ = false;
};

class SetLaneMuteCommand final : public LookCommand {
public:
    SetLaneMuteCommand(uint64_t look, ParamKey target, bool muted)
        : LookCommand(look), target_(target), muted_(muted) {}
    std::string name() const override {
        return muted_ ? "Mute Keyframes" : "Unmute Keyframes";
    }

    void apply(Document& doc) override {
        for (KeyframeLane& lane : look_of(doc).lanes) {
            if (!(lane.target == target_)) continue;
            old_ = lane.muted;
            lane.muted = muted_;
        }
    }

    void revert(Document& doc) override {
        for (KeyframeLane& lane : look_of(doc).lanes)
            if (lane.target == target_) lane.muted = old_;
    }

private:
    ParamKey target_;
    bool muted_;
    bool old_ = false;
};

class SetAudioConfigCommand final : public Command {
public:
    SetAudioConfigCommand(std::string sidechain_path, bool sidechain_mux,
                          float audio_offset_ms)
        : path_(std::move(sidechain_path)), mux_(sidechain_mux),
          offset_(audio_offset_ms) {}
    std::string name() const override { return "Audio Settings"; }

    void apply(Document& doc) override {
        old_path_ = doc.sidechain_path;
        old_mux_ = doc.sidechain_mux;
        old_offset_ = doc.audio_offset_ms;
        doc.sidechain_path = path_;
        doc.sidechain_mux = mux_;
        doc.audio_offset_ms = offset_;
    }

    void revert(Document& doc) override {
        doc.sidechain_path = old_path_;
        doc.sidechain_mux = old_mux_;
        doc.audio_offset_ms = old_offset_;
    }

    bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetAudioConfigCommand*>(&next);
        if (!other) return false;
        path_ = other->path_;
        mux_ = other->mux_;
        offset_ = other->offset_;
        return true;
    }

private:
    std::string path_;
    bool mux_;
    float offset_;
    std::string old_path_;
    bool old_mux_ = false;
    float old_offset_ = 0.0f;
};

class ToggleMarkerCommand final : public SequenceCommand {
public:
    ToggleMarkerCommand(uint64_t sequence, uint32_t frame)
        : SequenceCommand(sequence), frame_(frame) {}
    std::string name() const override { return "Toggle Marker"; }

    void apply(Document& doc) override {
        std::vector<uint32_t>& markers = sequence_of(doc).markers;
        auto it = std::find(markers.begin(), markers.end(), frame_);
        if (it != markers.end()) {
            removed_ = true;
            markers.erase(it);
        } else {
            removed_ = false;
            markers.insert(
                std::upper_bound(markers.begin(), markers.end(), frame_),
                frame_);
        }
    }

    void revert(Document& doc) override {
        std::vector<uint32_t>& markers = sequence_of(doc).markers;
        if (removed_) {
            markers.insert(
                std::upper_bound(markers.begin(), markers.end(), frame_),
                frame_);
        } else {
            auto it = std::find(markers.begin(), markers.end(), frame_);
            if (it != markers.end()) markers.erase(it);
        }
    }

private:
    uint32_t frame_;
    bool removed_ = false;
};

class SetExportConfigCommand final : public Command {
public:
    SetExportConfigCommand(float bitrate_mbps, uint32_t scale, bool audio)
        : bitrate_(bitrate_mbps), scale_(scale), audio_(audio) {}
    std::string name() const override { return "Export Settings"; }

    void apply(Document& doc) override {
        old_bitrate_ = doc.export_bitrate_mbps;
        old_scale_ = doc.export_scale;
        old_audio_ = doc.export_audio;
        doc.export_bitrate_mbps = bitrate_;
        doc.export_scale = scale_;
        doc.export_audio = audio_;
    }

    void revert(Document& doc) override {
        doc.export_bitrate_mbps = old_bitrate_;
        doc.export_scale = old_scale_;
        doc.export_audio = old_audio_;
    }

    bool merge(const Command& next) override {
        const auto* other =
            dynamic_cast<const SetExportConfigCommand*>(&next);
        if (!other) return false;
        bitrate_ = other->bitrate_;
        scale_ = other->scale_;
        audio_ = other->audio_;
        return true;
    }

private:
    float bitrate_;
    uint32_t scale_;
    bool audio_;
    float old_bitrate_ = 8.0f;
    uint32_t old_scale_ = 1;
    bool old_audio_ = true;
};

class SetUseProxyCommand final : public Command {
public:
    explicit SetUseProxyCommand(bool use_proxy) : use_proxy_(use_proxy) {}
    std::string name() const override {
        return use_proxy_ ? "Enable Proxy" : "Disable Proxy";
    }

    void apply(Document& doc) override {
        old_ = doc.use_proxy;
        doc.use_proxy = use_proxy_;
    }
    void revert(Document& doc) override { doc.use_proxy = old_; }

private:
    bool use_proxy_;
    bool old_ = false;
};

class SetProjectFormatCommand final : public Command {
public:
    SetProjectFormatCommand(double fps, uint32_t w, uint32_t h)
        : fps_(fps), w_(w), h_(h) {}
    std::string name() const override { return "Project Format"; }

    void apply(Document& doc) override {
        old_fps_ = doc.fps;
        old_w_ = doc.canvas_w;
        old_h_ = doc.canvas_h;
        doc.fps = fps_;
        doc.canvas_w = w_;
        doc.canvas_h = h_;
    }

    void revert(Document& doc) override {
        doc.fps = old_fps_;
        doc.canvas_w = old_w_;
        doc.canvas_h = old_h_;
    }

private:
    double fps_;
    uint32_t w_;
    uint32_t h_;
    double old_fps_ = 0.0;
    uint32_t old_w_ = 0;
    uint32_t old_h_ = 0;
};

// Per-entity FORMAT (look or sequence): all-zero = inherit the project.
class SetEntityFormatCommand final : public Command {
public:
    SetEntityFormatCommand(uint64_t entity, EntityFormat next)
        : entity_(entity), next_(next) {}
    std::string name() const override { return "Entity Format"; }

    void apply(Document& doc) override {
        if (Look* l = doc.find_look(entity_)) {
            old_ = l->format;
            l->format = next_;
        } else if (Sequence* s = doc.find_sequence(entity_)) {
            old_ = s->format;
            s->format = next_;
        }
    }

    void revert(Document& doc) override {
        if (Look* l = doc.find_look(entity_)) l->format = old_;
        else if (Sequence* s = doc.find_sequence(entity_))
            s->format = old_;
    }

private:
    uint64_t entity_;
    EntityFormat next_;
    EntityFormat old_;
};

class SetTimeRemapCommand final : public Command {
public:
    SetTimeRemapCommand(float speed, uint32_t mode)
        : speed_(speed), mode_(mode) {}
    std::string name() const override { return "Set Speed"; }

    void apply(Document& doc) override {
        old_speed_ = doc.speed;
        old_mode_ = doc.time_mode;
        doc.speed = speed_;
        doc.time_mode = mode_;
    }

    void revert(Document& doc) override {
        doc.speed = old_speed_;
        doc.time_mode = old_mode_;
    }

    bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetTimeRemapCommand*>(&next);
        if (!other) return false;
        speed_ = other->speed_;
        mode_ = other->mode_;
        return true;
    }

private:
    float speed_;
    uint32_t mode_;
    float old_speed_ = 1.0f;
    uint32_t old_mode_ = 0;
};

class AddRouteCommand final : public LookCommand {
public:
    AddRouteCommand(uint64_t look, ModRoute route)
        : LookCommand(look), route_(std::move(route)) {}
    std::string name() const override { return "Wire Param"; }

    void apply(Document& doc) override {
        // One wire per param: wiring an already-driven target replaces
        // its wire, exactly like image links at (to, port).
        std::vector<ModRoute>& routes = look_of(doc).mod_routes;
        replaced_.clear();
        for (size_t i = routes.size(); i-- > 0;) {
            if (!(routes[i].target == route_.target)) continue;
            replaced_.push_back({i, routes[i]});
            routes.erase(routes.begin() + i);
        }
        routes.push_back(route_);
    }

    void revert(Document& doc) override {
        std::vector<ModRoute>& routes = look_of(doc).mod_routes;
        routes.pop_back();
        for (size_t i = replaced_.size(); i-- > 0;)
            routes.insert(routes.begin() + replaced_[i].first,
                          replaced_[i].second);
    }

private:
    ModRoute route_;
    std::vector<std::pair<size_t, ModRoute>> replaced_;
};

class RemoveRouteCommand final : public LookCommand {
public:
    RemoveRouteCommand(uint64_t look, uint64_t route_id)
        : LookCommand(look), route_id_(route_id) {}
    std::string name() const override { return "Remove Mod Route"; }

    void apply(Document& doc) override {
        std::vector<ModRoute>& routes = look_of(doc).mod_routes;
        for (size_t i = 0; i < routes.size(); ++i) {
            if (routes[i].id == route_id_) {
                index_ = i;
                removed_ = routes[i];
                routes.erase(routes.begin() + i);
                return;
            }
        }
        assert(false && "route not found");
    }

    void revert(Document& doc) override {
        std::vector<ModRoute>& routes = look_of(doc).mod_routes;
        routes.insert(routes.begin() + index_, removed_);
    }

private:
    uint64_t route_id_;
    size_t index_ = 0;
    ModRoute removed_;
};

class AddValueNodeCommand final : public LookCommand {
public:
    AddValueNodeCommand(uint64_t look, ValueNode node)
        : LookCommand(look), node_(node) {}
    std::string name() const override { return "Add Value Node"; }
    void apply(Document& doc) override {
        look_of(doc).value_nodes.push_back(node_);
    }
    void revert(Document& doc) override {
        look_of(doc).value_nodes.pop_back();
    }

private:
    ValueNode node_;
};

class RemoveValueNodeCommand final : public LookCommand {
public:
    RemoveValueNodeCommand(uint64_t look, uint64_t node_id)
        : LookCommand(look), node_id_(node_id) {}
    std::string name() const override { return "Remove Value Node"; }

    void apply(Document& doc) override {
        Look& look = look_of(doc);
        routes_.clear();
        unwired_.clear();
        removed_ = {};
        index_ = 0;
        // Cascade: every wire out of this node goes with it - routes it
        // feeds, and helper inputs reading it (captured for undo).
        for (size_t i = look.mod_routes.size(); i-- > 0;) {
            if (look.mod_routes[i].node != node_id_) continue;
            routes_.push_back({i, look.mod_routes[i]});
            look.mod_routes.erase(look.mod_routes.begin() + i);
        }
        for (ValueNode& n : look.value_nodes) {
            if (n.in_a == node_id_) {
                unwired_.push_back({n.id, 0});
                n.in_a = 0;
            }
            if (n.in_b == node_id_) {
                unwired_.push_back({n.id, 1});
                n.in_b = 0;
            }
        }
        for (size_t i = 0; i < look.value_nodes.size(); ++i) {
            if (look.value_nodes[i].id != node_id_) continue;
            index_ = i;
            removed_ = look.value_nodes[i];
            look.value_nodes.erase(look.value_nodes.begin() + i);
            return;
        }
        assert(false && "value node not found");
    }

    void revert(Document& doc) override {
        Look& look = look_of(doc);
        look.value_nodes.insert(look.value_nodes.begin() + index_, removed_);
        for (const auto& [nid, which] : unwired_)
            if (ValueNode* n = find_value_node(look, nid))
                (which == 0 ? n->in_a : n->in_b) = node_id_;
        // routes_ was captured back-to-front; reinsert front-to-back so
        // the stored indices land exactly.
        for (size_t i = routes_.size(); i-- > 0;)
            look.mod_routes.insert(look.mod_routes.begin() +
                                       routes_[i].first,
                                   routes_[i].second);
    }

private:
    uint64_t node_id_;
    size_t index_ = 0;
    ValueNode removed_;
    std::vector<std::pair<size_t, ModRoute>> routes_;
    std::vector<std::pair<uint64_t, int>> unwired_;
};

class SetValueNodeCommand final : public LookCommand {
public:
    SetValueNodeCommand(uint64_t look, ValueNode node)
        : LookCommand(look), node_(node) {}
    std::string name() const override { return "Edit Value Node"; }

    void apply(Document& doc) override {
        ValueNode* n = find_value_node(look_of(doc), node_.id);
        assert(n);
        old_ = *n;
        *n = node_;
    }

    void revert(Document& doc) override {
        if (ValueNode* n = find_value_node(look_of(doc), node_.id))
            *n = old_;
    }

    bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetValueNodeCommand*>(&next);
        if (!other || !same_look(*other) || other->node_.id != node_.id)
            return false;
        node_ = other->node_;
        return true;
    }

private:
    ValueNode node_;
    ValueNode old_;
};

class WireValueInputCommand final : public LookCommand {
public:
    WireValueInputCommand(uint64_t look, uint64_t node_id, int which,
                          uint64_t from)
        : LookCommand(look), node_id_(node_id), which_(which), from_(from) {}
    std::string name() const override {
        return from_ ? "Wire Value Input" : "Unwire Value Input";
    }

    void apply(Document& doc) override {
        ValueNode* n = find_value_node(look_of(doc), node_id_);
        assert(n);
        uint64_t& slot = which_ == 0 ? n->in_a : n->in_b;
        old_ = slot;
        slot = from_;
    }

    void revert(Document& doc) override {
        if (ValueNode* n = find_value_node(look_of(doc), node_id_))
            (which_ == 0 ? n->in_a : n->in_b) = old_;
    }

private:
    uint64_t node_id_;
    int which_;
    uint64_t from_;
    uint64_t old_ = 0;
};

class SetRouteCurveCommand final : public LookCommand {
public:
    SetRouteCurveCommand(uint64_t look, uint64_t route_id, ResponseCurve curve)
        : LookCommand(look), route_id_(route_id), curve_(curve) {}
    std::string name() const override { return "Set Route Curve"; }

    void apply(Document& doc) override {
        ModRoute* r = find_route(look_of(doc), route_id_);
        assert(r);
        old_curve_ = r->curve;
        r->curve = curve_;
    }

    void revert(Document& doc) override {
        if (ModRoute* r = find_route(look_of(doc), route_id_))
            r->curve = old_curve_;
    }

private:
    uint64_t route_id_;
    ResponseCurve curve_;
    ResponseCurve old_curve_ = ResponseCurve::Linear;
};

class SetLaneCommand final : public LookCommand {
public:
    SetLaneCommand(uint64_t look, ParamKey target, std::vector<Keyframe> keys)
        : LookCommand(look), target_(target), keys_(std::move(keys)) {
        std::sort(keys_.begin(), keys_.end(),
                  [](const Keyframe& a, const Keyframe& b) {
                      return a.frame < b.frame;
                  });
    }
    std::string name() const override { return "Edit Keyframes"; }

    void apply(Document& doc) override {
        std::vector<KeyframeLane>& lanes = look_of(doc).lanes;
        old_keys_.clear();
        had_lane_ = false;
        for (size_t i = 0; i < lanes.size(); ++i) {
            if (lanes[i].target == target_) {
                had_lane_ = true;
                old_keys_ = lanes[i].keys;
                if (keys_.empty()) lanes.erase(lanes.begin() + i);
                else lanes[i].keys = keys_;
                return;
            }
        }
        if (!keys_.empty()) lanes.push_back({target_, keys_});
    }

    void revert(Document& doc) override {
        std::vector<KeyframeLane>& lanes = look_of(doc).lanes;
        for (size_t i = 0; i < lanes.size(); ++i) {
            if (lanes[i].target == target_) {
                if (had_lane_) lanes[i].keys = old_keys_;
                else lanes.erase(lanes.begin() + i);
                return;
            }
        }
        if (had_lane_) lanes.push_back({target_, old_keys_});
    }

    bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetLaneCommand*>(&next);
        if (!other || !same_look(*other) || !(other->target_ == target_))
            return false;
        keys_ = other->keys_;   // keep our old_keys_ — gesture start
        return true;
    }

private:
    ParamKey target_;
    std::vector<Keyframe> keys_;
    std::vector<Keyframe> old_keys_;
    bool had_lane_ = false;
};

class SetLanesCommand final : public LookCommand {
public:
    SetLanesCommand(uint64_t look, std::vector<KeyframeLane> lanes)
        : LookCommand(look), lanes_(std::move(lanes)) {
        for (KeyframeLane& lane : lanes_)
            std::sort(lane.keys.begin(), lane.keys.end(),
                      [](const Keyframe& a, const Keyframe& b) {
                          return a.frame < b.frame;
                      });
    }
    std::string name() const override { return "Edit Keyframes"; }

    void apply(Document& doc) override {
        std::vector<KeyframeLane>& doc_lanes = look_of(doc).lanes;
        old_.clear();
        for (const KeyframeLane& lane : lanes_) {
            Old o;
            bool found = false;
            for (size_t i = 0; i < doc_lanes.size() && !found; ++i) {
                if (!(doc_lanes[i].target == lane.target)) continue;
                found = true;
                o.had = true;
                o.keys = doc_lanes[i].keys;
                if (lane.keys.empty())
                    doc_lanes.erase(doc_lanes.begin() + i);
                else doc_lanes[i].keys = lane.keys;
            }
            if (!found && !lane.keys.empty())
                doc_lanes.push_back({lane.target, lane.keys});
            old_.push_back(std::move(o));
        }
    }

    void revert(Document& doc) override {
        std::vector<KeyframeLane>& doc_lanes = look_of(doc).lanes;
        for (size_t n = lanes_.size(); n-- > 0;) {
            const KeyframeLane& lane = lanes_[n];
            const Old& o = old_[n];
            bool found = false;
            for (size_t i = 0; i < doc_lanes.size() && !found; ++i) {
                if (!(doc_lanes[i].target == lane.target)) continue;
                found = true;
                if (o.had) doc_lanes[i].keys = o.keys;
                else doc_lanes.erase(doc_lanes.begin() + i);
            }
            if (!found && o.had)
                doc_lanes.push_back({lane.target, o.keys});
        }
    }

    bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetLanesCommand*>(&next);
        if (!other || !same_look(*other) ||
            other->lanes_.size() != lanes_.size())
            return false;
        for (size_t i = 0; i < lanes_.size(); ++i)
            if (!(other->lanes_[i].target == lanes_[i].target)) return false;
        for (size_t i = 0; i < lanes_.size(); ++i)
            lanes_[i].keys = other->lanes_[i].keys;   // keep our old_
        return true;
    }

private:
    struct Old {
        bool had = false;
        std::vector<Keyframe> keys;
    };
    std::vector<KeyframeLane> lanes_;
    std::vector<Old> old_;
};

// The snapshot of a look's current effect state (params, wet, opacity).
// Group composites snapshot their wet/opacity the same way, keyed with
// kGroupParamBit.
Snapshot capture_snapshot(const Look& look) {
    Snapshot snapshot;
    snapshot.valid = true;
    for (const Layer& layer : look.layers) {
        for (const EffectInstance& fx : layer.stack) {
            SnapshotEntry entry;
            entry.effect_id = fx.id;
            entry.params = fx.params;
            entry.wet = fx.wet;
            entry.opacity = fx.opacity;
            snapshot.entries.push_back(std::move(entry));
        }
        for (const Group& g : layer.groups) {
            SnapshotEntry entry;
            entry.effect_id = g.id | kGroupParamBit;
            entry.wet = g.wet;
            entry.opacity = g.opacity;
            snapshot.entries.push_back(std::move(entry));
        }
    }
    return snapshot;
}

class StoreSnapshotCommand final : public LookCommand {
public:
    StoreSnapshotCommand(uint64_t look, int slot)
        : LookCommand(look), slot_(slot) {}
    std::string name() const override { return "Store Snapshot"; }

    void apply(Document& doc) override {
        assert(slot_ >= 0 && slot_ < 3);
        Look& look = look_of(doc);
        previous_ = look.snapshots[slot_];
        look.snapshots[slot_] = capture_snapshot(look);
    }

    void revert(Document& doc) override {
        look_of(doc).snapshots[slot_] = previous_;
    }

private:
    int slot_;
    Snapshot previous_;
};

class ApplySnapshotCommand final : public LookCommand {
public:
    ApplySnapshotCommand(uint64_t look, int slot)
        : LookCommand(look), slot_(slot) {}
    std::string name() const override { return "Apply Snapshot"; }

    void apply(Document& doc) override {
        assert(slot_ >= 0 && slot_ < 3);
        Look& look = look_of(doc);
        previous_ = capture_snapshot(look);
        restore(look, look.snapshots[slot_]);
    }

    void revert(Document& doc) override {
        restore(look_of(doc), previous_);
    }

private:
    static void restore(Look& look, const Snapshot& snapshot) {
        if (!snapshot.valid) return;
        for (const SnapshotEntry& entry : snapshot.entries) {
            if (entry.effect_id & kGroupParamBit) {
                if (Group* g = find_group(
                        look, entry.effect_id & ~kGroupParamBit)) {
                    g->wet = entry.wet;
                    g->opacity = entry.opacity;
                }
                continue;
            }
            for (Layer& layer : look.layers) {
                for (EffectInstance& fx : layer.stack) {
                    if (fx.id != entry.effect_id) continue;
                    fx.wet = entry.wet;
                    fx.opacity = entry.opacity;
                    const size_t n =
                        std::min(fx.params.size(), entry.params.size());
                    for (size_t p = 0; p < n; ++p)
                        fx.params[p] = entry.params[p];
                }
            }
        }
    }

    int slot_;
    Snapshot previous_;
};

}  // namespace

std::unique_ptr<Command> add_route_command(uint64_t look, ModRoute route) {
    return std::make_unique<AddRouteCommand>(look, std::move(route));
}
std::unique_ptr<Command> remove_route_command(uint64_t look,
                                              uint64_t route_id) {
    return std::make_unique<RemoveRouteCommand>(look, route_id);
}
std::unique_ptr<Command> add_value_node_command(uint64_t look,
                                                ValueNode node) {
    return std::make_unique<AddValueNodeCommand>(look, node);
}
std::unique_ptr<Command> remove_value_node_command(uint64_t look,
                                                   uint64_t node_id) {
    return std::make_unique<RemoveValueNodeCommand>(look, node_id);
}
std::unique_ptr<Command> set_value_node_command(uint64_t look,
                                                ValueNode node) {
    return std::make_unique<SetValueNodeCommand>(look, node);
}
std::unique_ptr<Command> wire_value_input_command(uint64_t look,
                                                  uint64_t node_id,
                                                  int which, uint64_t from) {
    return std::make_unique<WireValueInputCommand>(look, node_id, which,
                                                   from);
}
std::unique_ptr<Command> set_route_curve_command(uint64_t look,
                                                 uint64_t route_id,
                                                 ResponseCurve curve) {
    return std::make_unique<SetRouteCurveCommand>(look, route_id, curve);
}
std::unique_ptr<Command> set_lane_command(uint64_t look, ParamKey target,
                                          std::vector<Keyframe> keys) {
    return std::make_unique<SetLaneCommand>(look, target, std::move(keys));
}
std::unique_ptr<Command> set_lanes_command(uint64_t look,
                                           std::vector<KeyframeLane> lanes) {
    return std::make_unique<SetLanesCommand>(look, std::move(lanes));
}
std::unique_ptr<Command> store_snapshot_command(uint64_t look, int slot) {
    return std::make_unique<StoreSnapshotCommand>(look, slot);
}
std::unique_ptr<Command> apply_snapshot_command(uint64_t look, int slot) {
    return std::make_unique<ApplySnapshotCommand>(look, slot);
}
std::unique_ptr<Command> set_morph_command(uint64_t look, int from, int to,
                                           float pos) {
    return std::make_unique<SetMorphCommand>(look, from, to, pos);
}
std::unique_ptr<Command> set_time_remap_command(float speed, uint32_t mode) {
    return std::make_unique<SetTimeRemapCommand>(speed, mode);
}
std::unique_ptr<Command> set_project_format_command(double fps, uint32_t w,
                                                    uint32_t h) {
    return std::make_unique<SetProjectFormatCommand>(fps, w, h);
}
std::unique_ptr<Command> set_entity_format_command(uint64_t entity,
                                                   EntityFormat format) {
    return std::make_unique<SetEntityFormatCommand>(entity, format);
}
std::unique_ptr<Command> set_timeline_region_command(uint64_t look,
                                                     uint32_t trim_in,
                                                     uint32_t trim_out,
                                                     uint32_t loop_in,
                                                     uint32_t loop_out) {
    return std::make_unique<SetTimelineRegionCommand>(look, trim_in, trim_out,
                                                      loop_in, loop_out);
}
std::unique_ptr<Command> set_lane_loop_command(uint64_t look, ParamKey target,
                                               bool loop) {
    return std::make_unique<SetLaneLoopCommand>(look, target, loop);
}
std::unique_ptr<Command> set_lane_mute_command(uint64_t look, ParamKey target,
                                               bool muted) {
    return std::make_unique<SetLaneMuteCommand>(look, target, muted);
}
std::unique_ptr<Command> set_audio_config_command(std::string sidechain_path,
                                                  bool sidechain_mux,
                                                  float audio_offset_ms) {
    return std::make_unique<SetAudioConfigCommand>(std::move(sidechain_path),
                                                   sidechain_mux,
                                                   audio_offset_ms);
}

std::unique_ptr<Command> set_export_config_command(float bitrate_mbps,
                                                   uint32_t scale,
                                                   bool audio) {
    return std::make_unique<SetExportConfigCommand>(bitrate_mbps, scale,
                                                    audio);
}

std::unique_ptr<Command> toggle_marker_command(uint64_t look, uint32_t frame) {
    return std::make_unique<ToggleMarkerCommand>(look, frame);
}
std::unique_ptr<Command> set_use_proxy_command(bool use_proxy) {
    return std::make_unique<SetUseProxyCommand>(use_proxy);
}

}  // namespace looks::doc

#include "doc/mod_commands.h"

#include <algorithm>
#include <cassert>
#include <utility>

namespace looks::doc {

namespace {

ModRoute* find_route(Document& doc, uint64_t route_id) {
    for (ModRoute& r : doc.mod_routes)
        if (r.id == route_id) return &r;
    return nullptr;
}

class SetMorphCommand final : public Command {
public:
    SetMorphCommand(int from, int to, float pos)
        : from_(from), to_(to), pos_(pos) {}
    std::string name() const override { return "Set Morph"; }

    void apply(Document& doc) override {
        old_from_ = doc.morph_from;
        old_to_ = doc.morph_to;
        old_pos_ = doc.morph_pos;
        doc.morph_from = from_;
        doc.morph_to = to_;
        doc.morph_pos = pos_;
    }

    void revert(Document& doc) override {
        doc.morph_from = old_from_;
        doc.morph_to = old_to_;
        doc.morph_pos = old_pos_;
    }

    bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetMorphCommand*>(&next);
        if (!other) return false;
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

class SetTimelineRegionCommand final : public Command {
public:
    SetTimelineRegionCommand(uint32_t trim_in, uint32_t trim_out,
                             uint32_t loop_in, uint32_t loop_out)
        : trim_in_(trim_in), trim_out_(trim_out), loop_in_(loop_in),
          loop_out_(loop_out) {}
    std::string name() const override { return "Edit Timeline Region"; }

    void apply(Document& doc) override {
        old_[0] = doc.clip_trim_in;
        old_[1] = doc.clip_trim_out;
        old_[2] = doc.loop_in;
        old_[3] = doc.loop_out;
        doc.clip_trim_in = trim_in_;
        doc.clip_trim_out = trim_out_;
        doc.loop_in = loop_in_;
        doc.loop_out = loop_out_;
    }

    void revert(Document& doc) override {
        doc.clip_trim_in = old_[0];
        doc.clip_trim_out = old_[1];
        doc.loop_in = old_[2];
        doc.loop_out = old_[3];
    }

    bool merge(const Command& next) override {
        const auto* other =
            dynamic_cast<const SetTimelineRegionCommand*>(&next);
        if (!other) return false;
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

class SetLaneLoopCommand final : public Command {
public:
    SetLaneLoopCommand(ParamKey target, bool loop)
        : target_(target), loop_(loop) {}
    std::string name() const override {
        return loop_ ? "Loop Keyframes" : "Unloop Keyframes";
    }

    void apply(Document& doc) override {
        for (KeyframeLane& lane : doc.lanes) {
            if (!(lane.target == target_)) continue;
            old_ = lane.loop;
            lane.loop = loop_;
        }
    }

    void revert(Document& doc) override {
        for (KeyframeLane& lane : doc.lanes)
            if (lane.target == target_) lane.loop = old_;
    }

private:
    ParamKey target_;
    bool loop_;
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

class AddRouteCommand final : public Command {
public:
    explicit AddRouteCommand(ModRoute route) : route_(std::move(route)) {}
    std::string name() const override { return "Add Mod Route"; }
    void apply(Document& doc) override { doc.mod_routes.push_back(route_); }
    void revert(Document& doc) override { doc.mod_routes.pop_back(); }

private:
    ModRoute route_;
};

class RemoveRouteCommand final : public Command {
public:
    explicit RemoveRouteCommand(uint64_t route_id) : route_id_(route_id) {}
    std::string name() const override { return "Remove Mod Route"; }

    void apply(Document& doc) override {
        for (size_t i = 0; i < doc.mod_routes.size(); ++i) {
            if (doc.mod_routes[i].id == route_id_) {
                index_ = i;
                removed_ = doc.mod_routes[i];
                doc.mod_routes.erase(doc.mod_routes.begin() + i);
                return;
            }
        }
        assert(false && "route not found");
    }

    void revert(Document& doc) override {
        doc.mod_routes.insert(doc.mod_routes.begin() + index_, removed_);
    }

private:
    uint64_t route_id_;
    size_t index_ = 0;
    ModRoute removed_;
};

class SetRouteAmountCommand final : public Command {
public:
    SetRouteAmountCommand(uint64_t route_id, float amount)
        : route_id_(route_id), amount_(amount) {}
    std::string name() const override { return "Set Route Amount"; }

    void apply(Document& doc) override {
        ModRoute* r = find_route(doc, route_id_);
        assert(r);
        old_amount_ = r->amount;
        r->amount = amount_;
    }

    void revert(Document& doc) override {
        if (ModRoute* r = find_route(doc, route_id_)) r->amount = old_amount_;
    }

    bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetRouteAmountCommand*>(&next);
        if (!other || other->route_id_ != route_id_) return false;
        amount_ = other->amount_;
        return true;
    }

private:
    uint64_t route_id_;
    float amount_;
    float old_amount_ = 0.0f;
};

class SetRouteSourceCommand final : public Command {
public:
    SetRouteSourceCommand(uint64_t route_id, ModSource source)
        : route_id_(route_id), source_(source) {}
    std::string name() const override { return "Edit Mod Source"; }

    void apply(Document& doc) override {
        ModRoute* r = find_route(doc, route_id_);
        assert(r);
        old_source_ = r->source;
        r->source = source_;
    }

    void revert(Document& doc) override {
        if (ModRoute* r = find_route(doc, route_id_)) r->source = old_source_;
    }

    bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetRouteSourceCommand*>(&next);
        if (!other || other->route_id_ != route_id_) return false;
        source_ = other->source_;
        return true;
    }

private:
    uint64_t route_id_;
    ModSource source_;
    ModSource old_source_;
};

class SetRouteCurveCommand final : public Command {
public:
    SetRouteCurveCommand(uint64_t route_id, ResponseCurve curve)
        : route_id_(route_id), curve_(curve) {}
    std::string name() const override { return "Set Route Curve"; }

    void apply(Document& doc) override {
        ModRoute* r = find_route(doc, route_id_);
        assert(r);
        old_curve_ = r->curve;
        r->curve = curve_;
    }

    void revert(Document& doc) override {
        if (ModRoute* r = find_route(doc, route_id_)) r->curve = old_curve_;
    }

private:
    uint64_t route_id_;
    ResponseCurve curve_;
    ResponseCurve old_curve_ = ResponseCurve::Linear;
};

class SetLaneCommand final : public Command {
public:
    SetLaneCommand(ParamKey target, std::vector<Keyframe> keys)
        : target_(target), keys_(std::move(keys)) {
        std::sort(keys_.begin(), keys_.end(),
                  [](const Keyframe& a, const Keyframe& b) {
                      return a.frame < b.frame;
                  });
    }
    std::string name() const override { return "Edit Keyframes"; }

    void apply(Document& doc) override {
        old_keys_.clear();
        had_lane_ = false;
        for (size_t i = 0; i < doc.lanes.size(); ++i) {
            if (doc.lanes[i].target == target_) {
                had_lane_ = true;
                old_keys_ = doc.lanes[i].keys;
                if (keys_.empty()) doc.lanes.erase(doc.lanes.begin() + i);
                else doc.lanes[i].keys = keys_;
                return;
            }
        }
        if (!keys_.empty()) doc.lanes.push_back({target_, keys_});
    }

    void revert(Document& doc) override {
        for (size_t i = 0; i < doc.lanes.size(); ++i) {
            if (doc.lanes[i].target == target_) {
                if (had_lane_) doc.lanes[i].keys = old_keys_;
                else doc.lanes.erase(doc.lanes.begin() + i);
                return;
            }
        }
        if (had_lane_) doc.lanes.push_back({target_, old_keys_});
    }

    bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetLaneCommand*>(&next);
        if (!other || !(other->target_ == target_)) return false;
        keys_ = other->keys_;   // keep our old_keys_ — gesture start
        return true;
    }

private:
    ParamKey target_;
    std::vector<Keyframe> keys_;
    std::vector<Keyframe> old_keys_;
    bool had_lane_ = false;
};

class SetLanesCommand final : public Command {
public:
    explicit SetLanesCommand(std::vector<KeyframeLane> lanes)
        : lanes_(std::move(lanes)) {
        for (KeyframeLane& lane : lanes_)
            std::sort(lane.keys.begin(), lane.keys.end(),
                      [](const Keyframe& a, const Keyframe& b) {
                          return a.frame < b.frame;
                      });
    }
    std::string name() const override { return "Edit Keyframes"; }

    void apply(Document& doc) override {
        old_.clear();
        for (const KeyframeLane& lane : lanes_) {
            Old o;
            bool found = false;
            for (size_t i = 0; i < doc.lanes.size() && !found; ++i) {
                if (!(doc.lanes[i].target == lane.target)) continue;
                found = true;
                o.had = true;
                o.keys = doc.lanes[i].keys;
                if (lane.keys.empty()) doc.lanes.erase(doc.lanes.begin() + i);
                else doc.lanes[i].keys = lane.keys;
            }
            if (!found && !lane.keys.empty())
                doc.lanes.push_back({lane.target, lane.keys});
            old_.push_back(std::move(o));
        }
    }

    void revert(Document& doc) override {
        for (size_t n = lanes_.size(); n-- > 0;) {
            const KeyframeLane& lane = lanes_[n];
            const Old& o = old_[n];
            bool found = false;
            for (size_t i = 0; i < doc.lanes.size() && !found; ++i) {
                if (!(doc.lanes[i].target == lane.target)) continue;
                found = true;
                if (o.had) doc.lanes[i].keys = o.keys;
                else doc.lanes.erase(doc.lanes.begin() + i);
            }
            if (!found && o.had) doc.lanes.push_back({lane.target, o.keys});
        }
    }

    bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetLanesCommand*>(&next);
        if (!other || other->lanes_.size() != lanes_.size()) return false;
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

class StoreSnapshotCommand final : public Command {
public:
    explicit StoreSnapshotCommand(int slot) : slot_(slot) {}
    std::string name() const override { return "Store Snapshot"; }

    void apply(Document& doc) override {
        assert(slot_ >= 0 && slot_ < 3);
        previous_ = doc.snapshots[slot_];
        doc.snapshots[slot_] = capture_snapshot(doc);
    }

    void revert(Document& doc) override { doc.snapshots[slot_] = previous_; }

private:
    int slot_;
    Snapshot previous_;
};

class ApplySnapshotCommand final : public Command {
public:
    explicit ApplySnapshotCommand(int slot) : slot_(slot) {}
    std::string name() const override { return "Apply Snapshot"; }

    void apply(Document& doc) override {
        assert(slot_ >= 0 && slot_ < 3);
        previous_ = capture_snapshot(doc);
        restore(doc, doc.snapshots[slot_]);
    }

    void revert(Document& doc) override { restore(doc, previous_); }

private:
    static void restore(Document& doc, const Snapshot& snapshot) {
        if (!snapshot.valid) return;
        for (const SnapshotEntry& entry : snapshot.entries) {
            for (Layer& layer : doc.layers) {
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

Snapshot capture_snapshot(const Document& doc) {
    Snapshot snapshot;
    snapshot.valid = true;
    for (const Layer& layer : doc.layers) {
        for (const EffectInstance& fx : layer.stack) {
            SnapshotEntry entry;
            entry.effect_id = fx.id;
            entry.params = fx.params;
            entry.wet = fx.wet;
            entry.opacity = fx.opacity;
            snapshot.entries.push_back(std::move(entry));
        }
    }
    return snapshot;
}

std::unique_ptr<Command> add_route_command(ModRoute route) {
    return std::make_unique<AddRouteCommand>(std::move(route));
}
std::unique_ptr<Command> remove_route_command(uint64_t route_id) {
    return std::make_unique<RemoveRouteCommand>(route_id);
}
std::unique_ptr<Command> set_route_amount_command(uint64_t route_id,
                                                  float amount) {
    return std::make_unique<SetRouteAmountCommand>(route_id, amount);
}
std::unique_ptr<Command> set_route_source_command(uint64_t route_id,
                                                  ModSource source) {
    return std::make_unique<SetRouteSourceCommand>(route_id, source);
}
std::unique_ptr<Command> set_route_curve_command(uint64_t route_id,
                                                 ResponseCurve curve) {
    return std::make_unique<SetRouteCurveCommand>(route_id, curve);
}
std::unique_ptr<Command> set_lane_command(ParamKey target,
                                          std::vector<Keyframe> keys) {
    return std::make_unique<SetLaneCommand>(target, std::move(keys));
}
std::unique_ptr<Command> set_lanes_command(std::vector<KeyframeLane> lanes) {
    return std::make_unique<SetLanesCommand>(std::move(lanes));
}
std::unique_ptr<Command> store_snapshot_command(int slot) {
    return std::make_unique<StoreSnapshotCommand>(slot);
}
std::unique_ptr<Command> apply_snapshot_command(int slot) {
    return std::make_unique<ApplySnapshotCommand>(slot);
}
std::unique_ptr<Command> set_morph_command(int from, int to, float pos) {
    return std::make_unique<SetMorphCommand>(from, to, pos);
}
std::unique_ptr<Command> set_time_remap_command(float speed, uint32_t mode) {
    return std::make_unique<SetTimeRemapCommand>(speed, mode);
}
std::unique_ptr<Command> set_timeline_region_command(uint32_t trim_in,
                                                     uint32_t trim_out,
                                                     uint32_t loop_in,
                                                     uint32_t loop_out) {
    return std::make_unique<SetTimelineRegionCommand>(trim_in, trim_out,
                                                      loop_in, loop_out);
}
std::unique_ptr<Command> set_lane_loop_command(ParamKey target, bool loop) {
    return std::make_unique<SetLaneLoopCommand>(target, loop);
}
std::unique_ptr<Command> set_audio_config_command(std::string sidechain_path,
                                                  bool sidechain_mux,
                                                  float audio_offset_ms) {
    return std::make_unique<SetAudioConfigCommand>(std::move(sidechain_path),
                                                   sidechain_mux,
                                                   audio_offset_ms);
}
std::unique_ptr<Command> set_use_proxy_command(bool use_proxy) {
    return std::make_unique<SetUseProxyCommand>(use_proxy);
}

}  // namespace looks::doc

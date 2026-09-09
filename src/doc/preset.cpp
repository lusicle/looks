#include "doc/preset.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "doc/serialize.h"
#include "doc/stack_commands.h"
#include "util/file.h"

namespace looks::doc {

json::Value preset_to_json(const Preset& p) {
    json::Value v = json::Value::make_object();
    v.set("looks_preset", kPresetVersion);
    v.set("name", p.name);
    json::Value tags = json::Value::make_array();
    for (const std::string& t : p.tags) tags.push(json::Value(t));
    v.set("tags", std::move(tags));
    v.set("group", group_to_json(p.group));
    json::Value effects = json::Value::make_array();
    for (const EffectInstance& fx : p.effects)
        effects.push(effect_to_json(fx));
    v.set("effects", std::move(effects));
    json::Value links = json::Value::make_array();
    for (const NodeLink& link : p.links) {
        links.push(link_to_json(link));
    }
    v.set("links", std::move(links));
    return v;
}

std::optional<Preset> preset_from_json(const json::Value& v) {
    if (!v.is_object() || v.get("looks_preset").as_int(0) != kPresetVersion)
        return std::nullopt;
    Preset p;
    p.name = v.get("name").as_string();
    if (p.name.empty()) p.name = "preset";
    for (const json::Value& t : v.get("tags").array())
        if (t.is_string()) p.tags.push_back(t.as_string());
    p.group = group_from_json(v.get("group"));
    if (!p.group.id) return std::nullopt;
    std::unordered_set<uint64_t> ids{p.group.id};
    for (uint64_t slot : p.group.inputs)
        if (!slot || !ids.insert(slot).second) return std::nullopt;
    std::unordered_set<uint64_t> members;
    for (const json::Value& fv : v.get("effects").array()) {
        auto fx = effect_from_json(fv);
        if (!fx || !fx->id || !ids.insert(fx->id).second || fx->group_id != p.group.id)
            return std::nullopt;
        members.insert(fx->id);
        p.effects.push_back(std::move(*fx));
    }
    if (p.effects.empty()) return std::nullopt;
    if (p.group.face_out && !members.count(p.group.face_out)) return std::nullopt;
    for (const ParamKey& key : p.group.exposed) {
        const auto it = std::find_if(p.effects.begin(), p.effects.end(),
            [&](const EffectInstance& effect) { return effect.id == key.effect_id; });
        if (it == p.effects.end() || key.param_index < 0 ||
            static_cast<size_t>(key.param_index) >= it->params.size()) return std::nullopt;
    }
    ids.erase(p.group.id);
    for (const json::Value& value : v.get("links").array()) {
        NodeLink link = link_from_json(value);
        if (link.blend >= BlendMode::Count || !ids.count(link.from) || !ids.count(link.to) || link.to_port > 2 ||
            (!members.count(link.to) && link.to_port != 0) ||
            std::any_of(p.links.begin(), p.links.end(),
                [&](const NodeLink& other) { return link.same_endpoints(other); }))
            return std::nullopt;
        p.links.push_back(link);
    }
    Look graph;
    graph.effects = p.effects;
    graph.groups = {p.group};
    graph.links = p.links;
    for (const auto& link : graph.links)
        if (link_would_cycle(graph, link.from, link.to)) return std::nullopt;
    return p;
}

bool save_preset(const std::filesystem::path& path, const Preset& p) {
    const std::string text = json::write(preset_to_json(p), /*pretty=*/true);
    return write_file_bytes(path, text.data(), text.size());
}

std::optional<Preset> load_preset(const std::filesystem::path& path) {
    auto bytes = read_file_bytes(path);
    if (!bytes) return std::nullopt;
    const std::string_view text(reinterpret_cast<const char*>(bytes->data()),
                                bytes->size());
    json::ParseResult parsed = json::parse(text);
    if (!parsed.value) return std::nullopt;
    auto p = preset_from_json(*parsed.value);
    if (p) p->path = path;
    return p;
}

std::vector<Preset> scan_presets(const std::filesystem::path& dir,
                                 int* failed) {
    std::vector<Preset> out;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(dir, ec), end;
    while (!ec && it != end) {
        // The depth bound keeps link loops and deep trees out of the walk.
        if (it.depth() > 3) it.disable_recursion_pending();
        std::error_code fec;
        if (it->is_regular_file(fec) &&
            it->path().extension() == ".json") {
            if (auto p = load_preset(it->path()))
                out.push_back(std::move(*p));
            else if (failed)
                ++*failed;   // a bad preset must not go away silently
        }
        it.increment(ec);
    }
    std::sort(out.begin(), out.end(),
              [](const Preset& a, const Preset& b) { return a.name < b.name; });
    return out;
}

Preset make_preset_from_group(const Look& look, uint64_t group_id) {
    Preset p;
    for (const Group& g : look.groups)
        if (g.id == group_id) {
            p.group = g;
            break;
        }
    p.name = p.group.name.empty() ? "preset" : p.group.name;
    for (const EffectInstance& fx : look.effects)
        if (fx.group_id == group_id) {
            p.effects.push_back(fx);
            p.effects.back().generated_path.clear();
            p.effects.back().generated_signature.clear();
        }
    std::unordered_set<uint64_t> ids(p.group.inputs.begin(), p.group.inputs.end());
    for (const EffectInstance& fx : p.effects) ids.insert(fx.id);
    for (const NodeLink& link : look.links)
        if (ids.count(link.from) && ids.count(link.to)) p.links.push_back(link);
    // Keep only the face params that point at a captured member.
    std::vector<ParamKey> kept;
    for (const ParamKey& k : p.group.exposed)
        for (const EffectInstance& fx : p.effects)
            if (fx.id == k.effect_id) {
                kept.push_back(k);
                break;
            }
    p.group.exposed = std::move(kept);
    return p;
}

Preset instantiate_preset(Document& doc, const Preset& p) {
    Preset out = p;
    std::unordered_map<uint64_t, uint64_t> remap;
    Group& group = out.group;
    group.id = doc.next_effect_id++;
    group.folded = true;
    for (uint64_t& slot : group.inputs) {
        const uint64_t fresh = doc.next_effect_id++;
        remap.emplace(slot, fresh);
        slot = fresh;
    }
    for (EffectInstance& fx : out.effects) {
        const uint64_t fresh = doc.next_effect_id++;
        remap[fx.id] = fresh;
        fx.id = fresh;
        fx.group_id = group.id;
    }
    std::vector<ParamKey> exposed;
    for (ParamKey k : group.exposed) {
        auto it = remap.find(k.effect_id);
        if (it == remap.end()) continue;
        k.effect_id = it->second;
        exposed.push_back(k);
    }
    group.exposed = std::move(exposed);
    if (auto it = remap.find(group.face_out); it != remap.end())
        group.face_out = it->second;
    else
        group.face_out = 0;
    for (NodeLink& link : out.links) {
        link.from = remap.at(link.from);
        link.to = remap.at(link.to);
    }
    out.path.clear();
    return out;
}

}  // namespace looks::doc

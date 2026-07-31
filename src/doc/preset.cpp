#include "doc/preset.h"

#include <algorithm>
#include <unordered_map>

#include "doc/serialize.h"
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
    return v;
}

std::optional<Preset> preset_from_json(const json::Value& v) {
    if (!v.is_object() || v.get("looks_preset").as_int(0) < 1)
        return std::nullopt;
    Preset p;
    p.name = v.get("name").as_string();
    if (p.name.empty()) p.name = "preset";
    for (const json::Value& t : v.get("tags").array())
        if (t.is_string()) p.tags.push_back(t.as_string());
    p.group = group_from_json(v.get("group"));
    for (const json::Value& fv : v.get("effects").array())
        if (auto fx = effect_from_json(fv)) p.effects.push_back(std::move(*fx));
    if (p.effects.empty()) return std::nullopt;
    // Normalize: members carry the group's file-local id, no mask refs.
    for (EffectInstance& fx : p.effects) {
        fx.group_id = p.group.id;
        fx.mask_id = 0;
    }
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

std::vector<Preset> scan_presets(const std::filesystem::path& dir) {
    std::vector<Preset> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() != ".json") continue;
        if (auto p = load_preset(entry.path())) out.push_back(std::move(*p));
    }
    std::sort(out.begin(), out.end(),
              [](const Preset& a, const Preset& b) { return a.name < b.name; });
    return out;
}

Preset make_preset_from_group(const Document& doc, size_t layer_index,
                              uint64_t group_id) {
    Preset p;
    const Layer& layer = doc.layers[layer_index];
    for (const Group& g : layer.groups)
        if (g.id == group_id) {
            p.group = g;
            break;
        }
    p.name = p.group.name.empty() ? "preset" : p.group.name;
    for (const EffectInstance& fx : layer.stack)
        if (fx.group_id == group_id) {
            p.effects.push_back(fx);
            p.effects.back().mask_id = 0;
        }
    // Keep only exposed face params that point at captured members.
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

void instantiate_preset(Document& doc, const Preset& p, Group* out_group,
                        std::vector<EffectInstance>* out_effects) {
    std::unordered_map<uint64_t, uint64_t> remap;
    Group group = p.group;
    group.id = doc.next_effect_id++;
    group.folded = true;   // presets land collapsed, macros up front

    std::vector<EffectInstance> effects = p.effects;
    for (EffectInstance& fx : effects) {
        const uint64_t fresh = doc.next_effect_id++;
        remap[fx.id] = fresh;
        fx.id = fresh;
        fx.group_id = group.id;
        fx.mask_id = 0;
    }
    std::vector<ParamKey> exposed;
    for (ParamKey k : group.exposed) {
        auto it = remap.find(k.effect_id);
        if (it == remap.end()) continue;
        k.effect_id = it->second;
        exposed.push_back(k);
    }
    group.exposed = std::move(exposed);
    // Boundary bindings remap too; default to the chain ends.
    if (auto it = remap.find(group.face_in); it != remap.end())
        group.face_in = it->second;
    else
        group.face_in = effects.empty() ? 0 : effects.front().id;
    if (auto it = remap.find(group.face_out); it != remap.end())
        group.face_out = it->second;
    else
        group.face_out = effects.empty() ? 0 : effects.back().id;
    *out_group = std::move(group);
    *out_effects = std::move(effects);
}

}  // namespace looks::doc

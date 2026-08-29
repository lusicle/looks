#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "doc/document.h"
#include "util/json.h"

namespace looks::doc {

inline constexpr int kPresetVersion = 1;

struct Preset {
    std::string name;
    std::vector<std::string> tags;
    Group group;                          // file-local ids
    std::vector<EffectInstance> effects;  // stack order, file-local ids
    // The member the In slot seeds to, a file-local id. 0 is the first.
    uint64_t face_in = 0;
    std::filesystem::path path;           // where the browser found it
};

json::Value preset_to_json(const Preset& p);
std::optional<Preset> preset_from_json(const json::Value& v);

bool save_preset(const std::filesystem::path& path, const Preset& p);
std::optional<Preset> load_preset(const std::filesystem::path& path);

// This scans *.json some levels deep and sorts the result by name.
// `failed` counts the preset files that do not read or do not parse.
std::vector<Preset> scan_presets(const std::filesystem::path& dir,
                                 int* failed = nullptr);

Preset make_preset_from_group(const Look& look, size_t layer_index,
                              uint64_t group_id);

// This mints fresh ids and rewrites group_id and the face keys.
void instantiate_preset(Document& doc, const Preset& p, Group* out_group,
                        std::vector<EffectInstance>* out_effects,
                        uint64_t* out_face_in);

}  // namespace looks::doc

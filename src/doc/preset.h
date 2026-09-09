#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "doc/document.h"
#include "util/json.h"

namespace looks::doc {

inline constexpr int kPresetVersion = 2;

struct Preset {
    std::string name;
    std::vector<std::string> tags;
    Group group;                          // file-local ids
    std::vector<EffectInstance> effects;
    std::vector<NodeLink> links;
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

Preset make_preset_from_group(const Look& look, uint64_t group_id);

Preset instantiate_preset(Document& doc, const Preset& p);

}  // namespace looks::doc

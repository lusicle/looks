// Preset files: a saved group IS an "era preset" —
// one JSON file holding a Group (exposed face included) plus its member
// effects, tagged and searchable in the browser. Instantiating mints
// fresh document ids and rewrites the face keys so a preset can be
// dropped into any project any number of times.

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
    std::filesystem::path path;           // where the browser found it
};

json::Value preset_to_json(const Preset& p);
std::optional<Preset> preset_from_json(const json::Value& v);

bool save_preset(const std::filesystem::path& path, const Preset& p);
std::optional<Preset> load_preset(const std::filesystem::path& path);

// All *.json presets under `dir`, subdirectories included a few levels
// deep (they are the preset browser's bins), sorted by name. `failed`
// (optional) counts unreadable/invalid preset files so the UI can
// surface them instead of silently dropping them.
std::vector<Preset> scan_presets(const std::filesystem::path& dir,
                                 int* failed = nullptr);

// Capture a live group as a preset.
Preset make_preset_from_group(const Look& look, size_t layer_index,
                              uint64_t group_id);

// Mint fresh ids from doc.next_effect_id and rewrite group_id / face
// keys; the results feed insert_group_command.
void instantiate_preset(Document& doc, const Preset& p, Group* out_group,
                        std::vector<EffectInstance>* out_effects);

}  // namespace looks::doc

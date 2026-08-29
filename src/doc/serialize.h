// Project serialization: the whole Document to/from a single
// JSON file. Presets reuse the effect/group encoders so a saved group is
// the same on-disk shape as a group inside a project.
//
// Loading is tolerant by construction: missing keys fall back to defaults
// (json::Value typed reads never throw), unknown effect types are skipped,
// and id counters are re-derived from the highest id seen so a hand-edited
// file can never mint duplicate ids.
//
// Version 4: two entities. Looks are timeless graphs (media nodes with a
// slip, generators, nested look/sequence refs - no placements, no audio
// of their own); sequences arrange placements on video lanes and audio
// tracks and carry the timeline region. Older files do not load - no
// userbase, no migration path.

#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "doc/document.h"
#include "util/json.h"

namespace looks::doc {

inline constexpr int kProjectVersion = 5;

json::Value doc_to_json(const Document& doc);
Document doc_from_json(const json::Value& v);

// Shared with preset files (a preset is a group + its member effects).
json::Value effect_to_json(const EffectInstance& fx);
std::optional<EffectInstance> effect_from_json(const json::Value& v);
json::Value group_to_json(const Group& g);
// Pre-slot files carried the In binding as face_in; the reader surfaces
// it so loaders can run the one-time slot migration.
struct GroupLegacy {
    bool migrate = false;   // no "inputs" key: route through normalize
    uint64_t face_in = 0;
};
Group group_from_json(const json::Value& v, GroupLegacy* legacy = nullptr);

bool save_document(const std::filesystem::path& path, const Document& doc);
std::optional<Document> load_document(const std::filesystem::path& path,
                                      std::string* error = nullptr);

}  // namespace looks::doc

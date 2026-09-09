// Loading is tolerant: a missing key uses a default, unknown types drop.
// Id counters re-derive from the highest id in the file.

#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "doc/document.h"
#include "util/json.h"

namespace looks::doc {

inline constexpr int kProjectVersion = 6;

json::Value doc_to_json(const Document& doc);
Document doc_from_json(const json::Value& v);
bool valid_look_graph(const Look& look);
json::Value link_to_json(const NodeLink& link);
NodeLink link_from_json(const json::Value& value);

// Preset files share this encoding. A change here changes preset files.
json::Value effect_to_json(const EffectInstance& fx);
std::optional<EffectInstance> effect_from_json(const json::Value& v);
json::Value group_to_json(const Group& g);
Group group_from_json(const json::Value& v);

bool save_document(const std::filesystem::path& path, const Document& doc);
std::optional<Document> load_document(const std::filesystem::path& path,
                                      std::string* error = nullptr);

}  // namespace looks::doc

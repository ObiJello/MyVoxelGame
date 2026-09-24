#pragma once

#include "external/json.hpp"
#include <string>

// Twilight Forest data files for the structure ports (cached, thread-safe).
// Paths are under the data root (MC_DATA_ROOT, else the nearest data/ above
// the working directory), the same discovery the structure loaders use.

namespace minecraft {
namespace levelgen {
namespace structure {
namespace twilight_data {

/**
 * data/twilightforest/<relativePath>, parsed (e.g.
 * "twilight/structure_speleothem_settings/small_hollow_hill.json",
 * "worldgen/structure/small_hollow_hill.json"). Throws std::runtime_error when
 * the file is missing or malformed: a silently empty config would be an
 * invisible hole.
 */
const nlohmann::json& file(const std::string& relativePath);

/** worldgen/structure/<path>.json for "twilightforest:<path>". */
const nlohmann::json& structure(const std::string& structureName);

/** True when data/twilightforest/<relativePath> exists. */
bool exists(const std::string& relativePath);

} // namespace twilight_data
} // namespace structure
} // namespace levelgen
} // namespace minecraft

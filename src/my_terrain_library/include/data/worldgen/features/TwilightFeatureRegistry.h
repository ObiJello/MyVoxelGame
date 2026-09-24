#pragma once

#include "levelgen/feature/Feature.h"
#include <memory>
#include <string>
#include <vector>

// Twilight Forest configured-feature registry (pass two).
//
// Every TF configured feature is registered here under its JSON id — the path
// of data/twilightforest/worldgen/configured_feature/<path>.json with the
// "twilightforest:" namespace, e.g. "twilightforest:hollow_log",
// "twilightforest:tree/mega_canopy_tree". Vanilla configured features the TF
// placed features name ("minecraft:grass", ...) are registered under their
// vanilla ids. TwilightPlacements then builds every placed feature straight
// from data/twilightforest/worldgen/placed_feature/**.json, resolving the
// "feature" field here — so a feature is placed exactly as the mod's JSON
// says, with no hand-copied modifier chains.
//
// Bootstrap order (TwilightPlacements::bootstrap): TwilightFeatures (pass one
// trees/flora/lakes/ores), TwilightTreeFeatures, TwilightDecorFeatures, then
// the placed-feature JSON. A configured feature whose blocks are missing is
// simply not registered: its placed feature is skipped with one warning.

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {
namespace twilight {

/** Register `feature` (not owned) under `id`. A null feature is ignored. */
void registerConfigured(const std::string& id, levelgen::ConfiguredFeature* feature);

/** Take ownership of `feature` and register it under `id`; returns the raw pointer. */
levelgen::ConfiguredFeature* registerOwned(const std::string& id,
                                           std::unique_ptr<levelgen::ConfiguredFeature> feature);

/** The configured feature registered under `id`, or null. */
levelgen::ConfiguredFeature* findConfigured(const std::string& id);

/** Every registered id, for diagnostics. */
std::vector<std::string> registeredIds();

} // namespace twilight
} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft

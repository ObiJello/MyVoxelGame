#pragma once

#include "levelgen/structure/TemplateEngine.h"
#include <string>

// Reference: net/minecraft/world/level/levelgen/structure/templatesystem/
// RuleProcessor.java + BlockRotProcessor.java, data-driven from
// data/minecraft/worldgen/processor_list/*.json (B7 jigsaw placement).

namespace minecraft {
namespace levelgen {
class WorldGenLevel;
namespace structure {
namespace ProcessorLists {

/**
 * Appends the named processor list's per-block lambdas to the settings, in
 * file order. Each rule/block_rot processor gets its OWN
 * LegacyRandomSource(Mth.getSeed(worldPos)) per block (identically seeded -
 * the ruined-portal-verified convention). `level` is captured for location
 * predicates (world-block reads) - it must outlive the settings.
 * Throws on unknown list ids or unsupported processor/predicate types.
 */
void appendProcessors(const std::string& listId, TemplatePlaceSettings& settings,
                      WorldGenLevel* level);

} // namespace ProcessorLists
} // namespace structure
} // namespace levelgen
} // namespace minecraft

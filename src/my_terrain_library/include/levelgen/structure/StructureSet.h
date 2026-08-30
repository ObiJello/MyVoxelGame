#pragma once

#include "levelgen/structure/StructurePlacement.h"
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

// Reference: net/minecraft/world/level/levelgen/structure/StructureSet.java
// plus the datapack JSON under data/minecraft/worldgen/structure_set/ (20 files)
// and data/minecraft/worldgen/structure/ (34 files).
//
// Registry iteration order is ALPHABETICAL by full "minecraft:<name>" id -
// Java's RegistryDataLoader sorts loaded entries by Identifier (Comparable),
// and both createStructures (structure sets) and applyBiomeDecoration's
// structuresByStep grouping (structures) iterate that order. Parity-load-bearing.

namespace minecraft {
namespace levelgen {
namespace structure {

/**
 * Loaded worldgen/structure/<name>.json - the fields the placement layer and
 * later batches need. Grows into the behavioral Structure class in B1.2+.
 */
// Reference: RuinedPortalStructure.Setup (record) - from the JSON "setups" list.
struct RuinedPortalSetup {
    std::string placement;  // on_land_surface/in_mountain/underground/partly_buried/on_ocean_floor/in_nether
    float airPocketProbability = 0.0f;
    float mossiness = 0.0f;
    bool overgrown = false;
    bool vines = false;
    bool canBeCold = false;
    bool replaceWithBlackstone = false;
    float weight = 1.0f;
};

struct StructureInfo {
    std::string name;               // "minecraft:village_plains"
    std::string type;               // "minecraft:jigsaw", "minecraft:mineshaft", ...
    std::string biomesTag;          // "#minecraft:has_structure/village_plains" (all 34 use tags)
    std::string step;               // GenerationStep.Decoration serialized name
    std::string terrainAdaptation;  // "none", "beard_thin", "beard_box", "bury", "encapsulate"
    std::string mineshaftType;      // "normal"/"mesa" (mineshaft JSONs only; else empty)
    std::vector<RuinedPortalSetup> portalSetups;  // ruined_portal JSONs only

    // Jigsaw fields (type == minecraft:jigsaw only).
    std::string jigsawStartPool;
    std::string jigsawStartJigsawName;      // "" when absent
    std::string jigsawProjectToHeightmap;   // "" when absent, else e.g. "WORLD_SURFACE_WG"
    int jigsawStartHeightAbsolute = 0;      // absolute provider value
    bool jigsawStartHeightUniform = false;  // trial_chambers: uniform(min,max)
    int jigsawStartHeightMin = 0;
    int jigsawStartHeightMax = 0;
    int jigsawMaxDepth = 0;                 // "size"
    bool jigsawExpansionHack = false;
    // "liquid_settings": apply_waterlogging (default) | ignore_waterlogging.
    std::string jigsawLiquidSettings = "apply_waterlogging";
    int jigsawMaxDistanceH = 80;
    int jigsawMaxDistanceV = 80;
    int jigsawPaddingBottom = 0;
    int jigsawPaddingTop = 0;
    bool jigsawHasAliases = false;          // pool_aliases present
    struct PoolAlias {
        std::string type;    // "direct" / "random" / "random_group"
        std::string alias;   // direct/random
        std::string target;  // direct
        std::vector<std::pair<std::string, int>> targets;             // random: (pool, weight)
        std::vector<std::pair<std::vector<PoolAlias>, int>> groups;   // random_group
    };
    std::vector<PoolAlias> jigsawAliases;
};

// Reference: StructureSet.StructureSelectionEntry (record)
struct StructureSelectionEntry {
    const StructureInfo* structure;
    int32_t weight;
};

// Reference: StructureSet.java (record: structures + placement)
struct StructureSet {
    std::string name;  // "minecraft:villages"
    std::unique_ptr<StructurePlacement> placement;
    std::vector<StructureSelectionEntry> structures;
};

namespace StructureSets {

/**
 * All structure sets from data/minecraft/worldgen/structure_set/, sorted
 * alphabetically by full id (registry order). Loaded once, cached; throws on
 * missing/malformed data (silent emptiness would be an invisible parity hole).
 */
const std::vector<const StructureSet*>& all();

/** Structure registry (alphabetical by id), loaded from worldgen/structure/. */
const std::vector<const StructureInfo*>& allStructures();

/** Lookup by full id; throws if unknown. */
const StructureSet& byName(const std::string& name);
const StructureInfo& structureByName(const std::string& name);

} // namespace StructureSets

namespace BiomeTags {

/**
 * Resolve a worldgen biome tag ("#minecraft:has_structure/village_plains" or
 * "minecraft:...") to its expanded set of biome ids. File order is irrelevant
 * here (used only for set-membership tests). Throws on missing tag files.
 * Files: data/<ns>/tags/worldgen/biome/<path>.json
 */
const std::unordered_set<std::string>& resolve(const std::string& tag);

} // namespace BiomeTags

} // namespace structure
} // namespace levelgen
} // namespace minecraft

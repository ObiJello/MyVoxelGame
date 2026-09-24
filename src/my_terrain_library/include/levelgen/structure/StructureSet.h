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
    // Reference: Structure.StructureSettings.spawnOverrides - true when the
    // JSON carries a non-empty "spawn_overrides" map. The library only needs
    // to know WHICH structures to record spawn areas for
    // (StructureGeneration::recordSpawnOverrideAreas); the per-category mob
    // lists are the engine's to read.
    bool hasSpawnOverrides = false;
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
    // Engine extension (not a vanilla codec field): "level_site" {radius,
    // max_spread}. A structure too large to follow the ground (Aurelith, the
    // Hush's city: one rigid platform ~220 blocks across) samples the
    // WORLD_SURFACE_WG height on a 7x7 grid spanning +/- radius around its
    // centre, refuses the site when the highest and lowest samples differ by
    // more than max_spread, and otherwise sets its start on the MEDIAN sample
    // instead of the centre column (the vanilla projection). Precedent:
    // WoodlandMansionStructure.findGenerationPoint's lowest-corner sample.
    // radius 0 = off (every vanilla structure).
    int jigsawSiteRadius = 0;
    int jigsawSiteMaxSpread = 0;
    // Engine extension: "biome_at_surface": true. MC's Structure.isValidBiome
    // samples the biome at the jigsaw stub, i.e. start_height below the
    // projected surface for a buried start. Under a biome source whose cave
    // biome spans the whole underground band (The Hush's Crystal Caverns,
    // depth 0.2..0.9), a stub a dozen blocks down lands in the cave biome on
    // about half the land, so a structure tagged by surface biome is refused
    // there. With this flag (only meaningful with project_start_to_heightmap)
    // the check samples the projected surface column instead. false = the
    // vanilla stub check (every vanilla structure).
    bool jigsawBiomeAtSurface = false;
    struct PoolAlias {
        std::string type;    // "direct" / "random" / "random_group"
        std::string alias;   // direct/random
        std::string target;  // direct
        std::vector<std::pair<std::string, int>> targets;             // random: (pool, weight)
        std::vector<std::pair<std::vector<PoolAlias>, int>> groups;   // random_group
    };
    std::vector<PoolAlias> jigsawAliases;

    // Mod structure types (non-minecraft namespaces, e.g. "aether:bronze_dungeon"):
    // the full structure JSON, serialized, for the type's own port to read
    // its codec fields from. Empty for vanilla structures.
    std::string modJson;
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

/**
 * True for the structures in vanilla Minecraft 26.3's own registry. The
 * engine adds structures under the minecraft namespace too (the Hush,
 * Aurelith); those must not take a vanilla structure's decoration index.
 */
bool isVanillaStructure(const std::string& name);

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

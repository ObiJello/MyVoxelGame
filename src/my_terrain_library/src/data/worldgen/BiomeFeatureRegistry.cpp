#include "data/worldgen/BiomeFeatureRegistry.h"
#include "data/worldgen/features/OreFeatures.h"
#include "data/worldgen/features/VegetationFeatures.h"
#include "data/worldgen/features/TreeFeatures.h"
#include "data/worldgen/features/AquaticFeatures.h"
#include "data/worldgen/features/CaveFeatures.h"
#include "data/worldgen/features/MiscOverworldFeatures.h"
#include "data/worldgen/placement/TreePlacements.h"
#include "data/worldgen/placement/AquaticPlacements.h"
#include "data/worldgen/placement/CavePlacements.h"
#include "data/worldgen/placement/MiscOverworldPlacements.h"
#include <set>
#include <iostream>

// Reference: BiomeDefaultFeatures.java and OverworldBiomes.java
// CRITICAL: Each biome must add ALL its features in the EXACT order Java does,
// one biome at a time. This ensures FeatureSorter produces identical results.

namespace minecraft {
namespace data {
namespace worldgen {

using namespace levelgen;
using namespace levelgen::placement;
using namespace placement;  // For OrePlacements, CavePlacements, MiscOverworldPlacements, etc.

// Static members
std::unordered_map<std::string, std::vector<std::vector<const PlacedFeature*>>> BiomeFeatureRegistry::s_biomeFeatures;
std::vector<std::string> BiomeFeatureRegistry::s_biomeKeyOrder;
bool BiomeFeatureRegistry::s_initialized = false;

// Empty vector for missing biomes/steps
static const std::vector<const PlacedFeature*> s_emptyFeatures;
static const std::vector<std::vector<const PlacedFeature*>> s_emptyBiomeFeatures(GenerationStep::DECORATION_COUNT);

static bool s_debugNullptrFeatures = true;
static bool s_debugIceFeatures = true;
void BiomeFeatureRegistry::addFeature(const std::string& biomeKey, int step, const PlacedFeature* feature) {
    if (step < 0 || step >= GenerationStep::DECORATION_COUNT) return;
    if (!feature) {
        if (s_debugNullptrFeatures) {
            std::cerr << "WARNING: nullptr PlacedFeature for biome " << biomeKey << " step " << step << std::endl;
        }
        return;
    }

    // Debug: track ICE features
    if (s_debugIceFeatures && feature->getName().find("ICE") != std::string::npos) {
        std::cerr << "DEBUG addFeature: biome=" << biomeKey << " step=" << step
                  << " feature=" << (void*)feature << " name='" << feature->getName() << "'\n";
    }

    auto& biome = s_biomeFeatures[biomeKey];
    if (biome.empty()) {
        biome.resize(GenerationStep::DECORATION_COUNT);
    }
    biome[step].push_back(feature);
}

// =============================================================================
// Helper functions matching BiomeDefaultFeatures.java EXACTLY
// =============================================================================

void BiomeFeatureRegistry::addDefaultCarversAndLakes(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 15-21
    // Carvers are handled elsewhere, just add lakes
    addFeature(biomeKey, static_cast<int>(GenerationStep::Decoration::LAKES), MiscOverworldPlacements::LAKE_LAVA_UNDERGROUND);
    addFeature(biomeKey, static_cast<int>(GenerationStep::Decoration::LAKES), MiscOverworldPlacements::LAKE_LAVA_SURFACE);
}

void BiomeFeatureRegistry::addDefaultCrystalFormations(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java line 424
    addFeature(biomeKey, static_cast<int>(GenerationStep::Decoration::LOCAL_MODIFICATIONS), CavePlacements::AMETHYST_GEODE);
}

void BiomeFeatureRegistry::addDefaultMonsterRoom(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 23-26
    addFeature(biomeKey, static_cast<int>(GenerationStep::Decoration::UNDERGROUND_STRUCTURES), CavePlacements::MONSTER_ROOM);
    addFeature(biomeKey, static_cast<int>(GenerationStep::Decoration::UNDERGROUND_STRUCTURES), CavePlacements::MONSTER_ROOM_DEEP);
}

void BiomeFeatureRegistry::addDefaultUndergroundVariety(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 28-39
    int step = GenerationStep::UNDERGROUND_ORES;
    addFeature(biomeKey, step, OrePlacements::ORE_DIRT);
    addFeature(biomeKey, step, OrePlacements::ORE_GRAVEL);
    addFeature(biomeKey, step, OrePlacements::ORE_GRANITE_UPPER);
    addFeature(biomeKey, step, OrePlacements::ORE_GRANITE_LOWER);
    addFeature(biomeKey, step, OrePlacements::ORE_DIORITE_UPPER);
    addFeature(biomeKey, step, OrePlacements::ORE_DIORITE_LOWER);
    addFeature(biomeKey, step, OrePlacements::ORE_ANDESITE_UPPER);
    addFeature(biomeKey, step, OrePlacements::ORE_ANDESITE_LOWER);
    addFeature(biomeKey, step, OrePlacements::ORE_TUFF);
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, CavePlacements::GLOW_LICHEN);
}

void BiomeFeatureRegistry::addDefaultSprings(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 388-391
    addFeature(biomeKey, static_cast<int>(GenerationStep::Decoration::FLUID_SPRINGS), MiscOverworldPlacements::SPRING_WATER);
    addFeature(biomeKey, static_cast<int>(GenerationStep::Decoration::FLUID_SPRINGS), MiscOverworldPlacements::SPRING_LAVA);
}

void BiomeFeatureRegistry::addSurfaceFreezing(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 406-408
    addFeature(biomeKey, static_cast<int>(GenerationStep::Decoration::TOP_LAYER_MODIFICATION), MiscOverworldPlacements::FREEZE_TOP_LAYER);
}

void BiomeFeatureRegistry::globalOverworldGeneration(const std::string& biomeKey) {
    // Reference: OverworldBiomes.java lines 42-49
    addDefaultCarversAndLakes(biomeKey);
    addDefaultCrystalFormations(biomeKey);
    addDefaultMonsterRoom(biomeKey);
    addDefaultUndergroundVariety(biomeKey);
    addDefaultSprings(biomeKey);
    addSurfaceFreezing(biomeKey);
}

void BiomeFeatureRegistry::addDefaultOres(const std::string& biomeKey, bool largeCopperBlobs) {
    // Reference: BiomeDefaultFeatures.java lines 56-74
    int step = GenerationStep::UNDERGROUND_ORES;
    addFeature(biomeKey, step, OrePlacements::ORE_COAL_UPPER);
    addFeature(biomeKey, step, OrePlacements::ORE_COAL_LOWER);
    addFeature(biomeKey, step, OrePlacements::ORE_IRON_UPPER);
    addFeature(biomeKey, step, OrePlacements::ORE_IRON_MIDDLE);
    addFeature(biomeKey, step, OrePlacements::ORE_IRON_SMALL);
    addFeature(biomeKey, step, OrePlacements::ORE_GOLD);
    addFeature(biomeKey, step, OrePlacements::ORE_GOLD_LOWER);
    addFeature(biomeKey, step, OrePlacements::ORE_REDSTONE);
    addFeature(biomeKey, step, OrePlacements::ORE_REDSTONE_LOWER);
    addFeature(biomeKey, step, OrePlacements::ORE_DIAMOND);
    addFeature(biomeKey, step, OrePlacements::ORE_DIAMOND_MEDIUM);
    addFeature(biomeKey, step, OrePlacements::ORE_DIAMOND_LARGE);
    addFeature(biomeKey, step, OrePlacements::ORE_DIAMOND_BURIED);
    addFeature(biomeKey, step, OrePlacements::ORE_LAPIS);
    addFeature(biomeKey, step, OrePlacements::ORE_LAPIS_BURIED);
    addFeature(biomeKey, step, largeCopperBlobs ? OrePlacements::ORE_COPPER_LARGE : OrePlacements::ORE_COPPER);
    addFeature(biomeKey, step, CavePlacements::UNDERWATER_MAGMA);
}

void BiomeFeatureRegistry::addExtraGold(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::UNDERGROUND_ORES, OrePlacements::ORE_GOLD_EXTRA);
}

void BiomeFeatureRegistry::addExtraEmeralds(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::UNDERGROUND_ORES, OrePlacements::ORE_EMERALD);
}

void BiomeFeatureRegistry::addInfestedStone(const std::string& biomeKey) {
    addFeature(biomeKey, static_cast<int>(GenerationStep::Decoration::UNDERGROUND_DECORATION), OrePlacements::ORE_INFESTED);
}

void BiomeFeatureRegistry::addDefaultSoftDisks(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 88-92
    int step = GenerationStep::UNDERGROUND_ORES;
    addFeature(biomeKey, step, MiscOverworldPlacements::DISK_SAND);
    addFeature(biomeKey, step, MiscOverworldPlacements::DISK_CLAY);
    addFeature(biomeKey, step, MiscOverworldPlacements::DISK_GRAVEL);
}

void BiomeFeatureRegistry::addSwampClayDisk(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::UNDERGROUND_ORES, MiscOverworldPlacements::DISK_CLAY);
}

void BiomeFeatureRegistry::addMushroomFieldVegetation(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 253-257
    int step = GenerationStep::VEGETAL_DECORATION;
    addFeature(biomeKey, step, VegetationPlacements::MUSHROOM_ISLAND_VEGETATION);
    addFeature(biomeKey, step, VegetationPlacements::BROWN_MUSHROOM_TAIGA);
    addFeature(biomeKey, step, VegetationPlacements::RED_MUSHROOM_TAIGA);
}

void BiomeFeatureRegistry::addNearWaterVegetation(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 325-328
    int step = GenerationStep::VEGETAL_DECORATION;
    addFeature(biomeKey, step, VegetationPlacements::PATCH_SUGAR_CANE);
    addFeature(biomeKey, step, VegetationPlacements::PATCH_FIREFLY_BUSH_NEAR_WATER);
}

void BiomeFeatureRegistry::addPlainGrass(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 308-310
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::PATCH_TALL_GRASS_2);
}

void BiomeFeatureRegistry::addBushes(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 111-113
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::PATCH_BUSH);
}

void BiomeFeatureRegistry::addPlainVegetation(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 259-263
    int step = GenerationStep::VEGETAL_DECORATION;
    addFeature(biomeKey, step, VegetationPlacements::TREES_PLAINS);
    addFeature(biomeKey, step, VegetationPlacements::FLOWER_PLAINS);
    addFeature(biomeKey, step, VegetationPlacements::PATCH_GRASS_PLAIN);
}

void BiomeFeatureRegistry::addDefaultMushrooms(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 312-315
    int step = GenerationStep::VEGETAL_DECORATION;
    addFeature(biomeKey, step, VegetationPlacements::BROWN_MUSHROOM_NORMAL);
    addFeature(biomeKey, step, VegetationPlacements::RED_MUSHROOM_NORMAL);
}

void BiomeFeatureRegistry::addDefaultExtraVegetation(const std::string& biomeKey, bool nearWater) {
    // Reference: BiomeDefaultFeatures.java lines 317-323
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::PATCH_PUMPKIN);
    if (nearWater) {
        addNearWaterVegetation(biomeKey);
    }
}

void BiomeFeatureRegistry::addSwampVegetation(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 236-244
    int step = GenerationStep::VEGETAL_DECORATION;
    addFeature(biomeKey, step, VegetationPlacements::TREES_SWAMP);
    addFeature(biomeKey, step, VegetationPlacements::FLOWER_SWAMP);
    addFeature(biomeKey, step, VegetationPlacements::PATCH_GRASS_NORMAL);
    addFeature(biomeKey, step, VegetationPlacements::PATCH_DEAD_BUSH);
    addFeature(biomeKey, step, VegetationPlacements::PATCH_WATERLILY);
    addFeature(biomeKey, step, VegetationPlacements::BROWN_MUSHROOM_SWAMP);
    addFeature(biomeKey, step, VegetationPlacements::RED_MUSHROOM_SWAMP);
}

void BiomeFeatureRegistry::addSwampExtraVegetation(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 359-364
    int step = GenerationStep::VEGETAL_DECORATION;
    addFeature(biomeKey, step, VegetationPlacements::PATCH_SUGAR_CANE_SWAMP);
    addFeature(biomeKey, step, VegetationPlacements::PATCH_PUMPKIN);
    addFeature(biomeKey, step, VegetationPlacements::PATCH_FIREFLY_BUSH_SWAMP);
    addFeature(biomeKey, step, VegetationPlacements::PATCH_FIREFLY_BUSH_NEAR_WATER_SWAMP);
}

void BiomeFeatureRegistry::addFossilDecoration(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 375-378
    int step = static_cast<int>(GenerationStep::Decoration::UNDERGROUND_STRUCTURES);
    addFeature(biomeKey, step, CavePlacements::FOSSIL_UPPER);
    addFeature(biomeKey, step, CavePlacements::FOSSIL_LOWER);
}

void BiomeFeatureRegistry::addDefaultFlowers(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::FLOWER_DEFAULT);
}

void BiomeFeatureRegistry::addDefaultGrass(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 298-300
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::PATCH_GRASS_BADLANDS);
}

void BiomeFeatureRegistry::addWaterTrees(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::TREES_WATER);
}

void BiomeFeatureRegistry::addColdOceanExtraVegetation(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, AquaticPlacements::KELP_COLD);
}

void BiomeFeatureRegistry::addLukeWarmKelp(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, AquaticPlacements::KELP_WARM);
}

void BiomeFeatureRegistry::addFerns(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::PATCH_LARGE_FERN);
}

void BiomeFeatureRegistry::addTaigaTrees(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::TREES_TAIGA);
}

void BiomeFeatureRegistry::addTaigaGrass(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 302-306
    int step = GenerationStep::VEGETAL_DECORATION;
    addFeature(biomeKey, step, VegetationPlacements::PATCH_GRASS_TAIGA_2);
    addFeature(biomeKey, step, VegetationPlacements::BROWN_MUSHROOM_TAIGA);
    addFeature(biomeKey, step, VegetationPlacements::RED_MUSHROOM_TAIGA);
}

void BiomeFeatureRegistry::addCommonBerryBushes(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::PATCH_BERRY_COMMON);
}

void BiomeFeatureRegistry::addRareBerryBushes(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::PATCH_BERRY_RARE);
}

void BiomeFeatureRegistry::addForestFlowers(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::FOREST_FLOWERS);
}

void BiomeFeatureRegistry::addBirchTrees(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::TREES_BIRCH);
}

void BiomeFeatureRegistry::addTallBirchTrees(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::BIRCH_TALL);
}

void BiomeFeatureRegistry::addOtherBirchTrees(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 148-150 - forest uses this
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::TREES_BIRCH_AND_OAK_LEAF_LITTER);
}

void BiomeFeatureRegistry::addBirchForestFlowers(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::WILDFLOWERS_BIRCH_FOREST);
}

void BiomeFeatureRegistry::addForestGrass(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::PATCH_GRASS_FOREST);
}

void BiomeFeatureRegistry::addSavannaTrees(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::TREES_SAVANNA);
}

void BiomeFeatureRegistry::addShatteredSavannaTrees(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::TREES_WINDSWEPT_SAVANNA);
}

void BiomeFeatureRegistry::addSavannaGrass(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::PATCH_TALL_GRASS);
}

void BiomeFeatureRegistry::addShatteredSavannaGrass(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::PATCH_GRASS_NORMAL);
}

void BiomeFeatureRegistry::addSavannaExtraGrass(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::PATCH_GRASS_SAVANNA);
}

void BiomeFeatureRegistry::addWarmFlowers(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::FLOWER_WARM);
}

void BiomeFeatureRegistry::addJungleTrees(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::TREES_JUNGLE);
}

void BiomeFeatureRegistry::addSparseJungleTrees(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::TREES_SPARSE_JUNGLE);
}

void BiomeFeatureRegistry::addJungleGrass(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::PATCH_GRASS_JUNGLE);
}

void BiomeFeatureRegistry::addJungleVines(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::VINES);
}

void BiomeFeatureRegistry::addJungleMelons(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::PATCH_MELON);
}

void BiomeFeatureRegistry::addSparseJungleMelons(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::PATCH_MELON_SPARSE);
}

void BiomeFeatureRegistry::addLightBambooVegetation(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::BAMBOO_LIGHT);
}

void BiomeFeatureRegistry::addBambooVegetation(const std::string& biomeKey) {
    int step = GenerationStep::VEGETAL_DECORATION;
    addFeature(biomeKey, step, VegetationPlacements::BAMBOO);
    addFeature(biomeKey, step, VegetationPlacements::BAMBOO_VEGETATION);
}

void BiomeFeatureRegistry::addBadlandsTrees(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::TREES_BADLANDS);
}

void BiomeFeatureRegistry::addBadlandGrass(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 222-226
    int step = GenerationStep::VEGETAL_DECORATION;
    addFeature(biomeKey, step, VegetationPlacements::PATCH_GRASS_BADLANDS);
    addFeature(biomeKey, step, VegetationPlacements::PATCH_DRY_GRASS_BADLANDS);
    addFeature(biomeKey, step, VegetationPlacements::PATCH_DEAD_BUSH_BADLANDS);
}

void BiomeFeatureRegistry::addBadlandExtraVegetation(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 334-339
    int step = GenerationStep::VEGETAL_DECORATION;
    addFeature(biomeKey, step, VegetationPlacements::PATCH_SUGAR_CANE_BADLANDS);
    addFeature(biomeKey, step, VegetationPlacements::PATCH_PUMPKIN);
    addFeature(biomeKey, step, VegetationPlacements::PATCH_CACTUS_DECORATED);
    addFeature(biomeKey, step, VegetationPlacements::PATCH_FIREFLY_BUSH_NEAR_WATER);
}

void BiomeFeatureRegistry::addDesertVegetation(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 265-268
    int step = GenerationStep::VEGETAL_DECORATION;
    addFeature(biomeKey, step, VegetationPlacements::PATCH_DRY_GRASS_DESERT);
    addFeature(biomeKey, step, VegetationPlacements::PATCH_DEAD_BUSH_2);
}

void BiomeFeatureRegistry::addDesertExtraVegetation(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 353-357
    int step = GenerationStep::VEGETAL_DECORATION;
    addFeature(biomeKey, step, VegetationPlacements::PATCH_SUGAR_CANE_DESERT);
    addFeature(biomeKey, step, VegetationPlacements::PATCH_PUMPKIN);
    addFeature(biomeKey, step, VegetationPlacements::PATCH_CACTUS_DESERT);
}

void BiomeFeatureRegistry::addDesertExtraDecoration(const std::string& biomeKey) {
    addFeature(biomeKey, static_cast<int>(GenerationStep::Decoration::SURFACE_STRUCTURES), MiscOverworldPlacements::DESERT_WELL);
}

void BiomeFeatureRegistry::addMountainTrees(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::TREES_WINDSWEPT_HILLS);
}

void BiomeFeatureRegistry::addMountainForestTrees(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::TREES_WINDSWEPT_FOREST);
}

void BiomeFeatureRegistry::addSnowyTrees(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::TREES_SNOWY);
}

void BiomeFeatureRegistry::addFrozenSprings(const std::string& biomeKey) {
    addFeature(biomeKey, static_cast<int>(GenerationStep::Decoration::FLUID_SPRINGS), MiscOverworldPlacements::SPRING_LAVA_FROZEN);
}

void BiomeFeatureRegistry::addGroveTrees(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::TREES_GROVE);
}

void BiomeFeatureRegistry::addCherryGroveVegetation(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 281-285
    int step = GenerationStep::VEGETAL_DECORATION;
    addFeature(biomeKey, step, VegetationPlacements::PATCH_GRASS_PLAIN);
    addFeature(biomeKey, step, VegetationPlacements::FLOWER_CHERRY);
    addFeature(biomeKey, step, VegetationPlacements::TREES_CHERRY);
}

void BiomeFeatureRegistry::addMeadowVegetation(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 287-292
    int step = GenerationStep::VEGETAL_DECORATION;
    addFeature(biomeKey, step, VegetationPlacements::PATCH_GRASS_MEADOW);
    addFeature(biomeKey, step, VegetationPlacements::FLOWER_MEADOW);
    addFeature(biomeKey, step, VegetationPlacements::TREES_MEADOW);
    addFeature(biomeKey, step, VegetationPlacements::WILDFLOWERS_MEADOW);
}

void BiomeFeatureRegistry::addIcebergs(const std::string& biomeKey) {
    int step = static_cast<int>(GenerationStep::Decoration::LOCAL_MODIFICATIONS);
    addFeature(biomeKey, step, MiscOverworldPlacements::ICEBERG_PACKED);
    addFeature(biomeKey, step, MiscOverworldPlacements::ICEBERG_BLUE);
}

void BiomeFeatureRegistry::addBlueIce(const std::string& biomeKey) {
    addFeature(biomeKey, static_cast<int>(GenerationStep::Decoration::SURFACE_STRUCTURES), MiscOverworldPlacements::BLUE_ICE);
}

void BiomeFeatureRegistry::addMossyStoneBlock(const std::string& biomeKey) {
    addFeature(biomeKey, static_cast<int>(GenerationStep::Decoration::LOCAL_MODIFICATIONS), MiscOverworldPlacements::FOREST_ROCK);
}

void BiomeFeatureRegistry::addGiantTaigaVegetation(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 270-275
    int step = GenerationStep::VEGETAL_DECORATION;
    addFeature(biomeKey, step, VegetationPlacements::PATCH_GRASS_TAIGA);
    addFeature(biomeKey, step, VegetationPlacements::PATCH_DEAD_BUSH);
    addFeature(biomeKey, step, VegetationPlacements::BROWN_MUSHROOM_OLD_GROWTH);
    addFeature(biomeKey, step, VegetationPlacements::RED_MUSHROOM_OLD_GROWTH);
}

void BiomeFeatureRegistry::addLeafLitterPatch(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::PATCH_LEAF_LITTER);
}

void BiomeFeatureRegistry::addDripstone(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 41-45
    addFeature(biomeKey, static_cast<int>(GenerationStep::Decoration::LOCAL_MODIFICATIONS), CavePlacements::LARGE_DRIPSTONE);
    addFeature(biomeKey, static_cast<int>(GenerationStep::Decoration::UNDERGROUND_DECORATION), CavePlacements::DRIPSTONE_CLUSTER);
    addFeature(biomeKey, static_cast<int>(GenerationStep::Decoration::UNDERGROUND_DECORATION), CavePlacements::POINTED_DRIPSTONE);
}

void BiomeFeatureRegistry::addSculk(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 47-50
    int step = static_cast<int>(GenerationStep::Decoration::UNDERGROUND_DECORATION);
    addFeature(biomeKey, step, CavePlacements::SCULK_VEIN);
    addFeature(biomeKey, step, CavePlacements::SCULK_PATCH_DEEP_DARK);
}

void BiomeFeatureRegistry::addLushCavesVegetationFeatures(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 168-176
    int step = GenerationStep::VEGETAL_DECORATION;
    addFeature(biomeKey, step, CavePlacements::LUSH_CAVES_CEILING_VEGETATION);
    addFeature(biomeKey, step, CavePlacements::CAVE_VINES);
    addFeature(biomeKey, step, CavePlacements::LUSH_CAVES_CLAY);
    addFeature(biomeKey, step, CavePlacements::LUSH_CAVES_VEGETATION);
    addFeature(biomeKey, step, CavePlacements::ROOTED_AZALEA_TREE);
    addFeature(biomeKey, step, CavePlacements::SPORE_BLOSSOM);
    addFeature(biomeKey, step, CavePlacements::CLASSIC_VINES);
}

void BiomeFeatureRegistry::addLushCavesSpecialOres(const std::string& biomeKey) {
    addFeature(biomeKey, GenerationStep::UNDERGROUND_ORES, OrePlacements::ORE_CLAY);
}

// =============================================================================
// Individual biome setups - Reference: OverworldBiomes.java
// Each biome MUST add features in the EXACT order Java does
// =============================================================================

void BiomeFeatureRegistry::setupMushroomFields(const std::string& biomeKey) {
    // Reference: OverworldBiomes.java mushroomFields() lines 211-221
    globalOverworldGeneration(biomeKey);
    addDefaultOres(biomeKey);
    addDefaultSoftDisks(biomeKey);
    addMushroomFieldVegetation(biomeKey);
    addNearWaterVegetation(biomeKey);
}

void BiomeFeatureRegistry::setupPlains(const std::string& biomeKey, bool sunflower, bool snowy, bool spikes) {
    // Reference: OverworldBiomes.java plains() lines 175-209
    globalOverworldGeneration(biomeKey);
    if (snowy) {
        if (spikes) {
            addFeature(biomeKey, static_cast<int>(GenerationStep::Decoration::SURFACE_STRUCTURES), MiscOverworldPlacements::ICE_SPIKE);
            addFeature(biomeKey, static_cast<int>(GenerationStep::Decoration::SURFACE_STRUCTURES), MiscOverworldPlacements::ICE_PATCH);
        }
    } else {
        addPlainGrass(biomeKey);
        if (sunflower) {
            addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::PATCH_SUNFLOWER);
        } else {
            addBushes(biomeKey);
        }
    }
    addDefaultOres(biomeKey);
    addDefaultSoftDisks(biomeKey);
    if (snowy) {
        addSnowyTrees(biomeKey);
        addDefaultFlowers(biomeKey);
        addDefaultGrass(biomeKey);
    } else {
        addPlainVegetation(biomeKey);
    }
    addDefaultMushrooms(biomeKey);
    addDefaultExtraVegetation(biomeKey, true);
}

void BiomeFeatureRegistry::setupSwamp(const std::string& biomeKey) {
    // Reference: OverworldBiomes.java swamp() lines 471-485
    addFossilDecoration(biomeKey);
    globalOverworldGeneration(biomeKey);
    addDefaultOres(biomeKey);
    addSwampClayDisk(biomeKey);
    addSwampVegetation(biomeKey);
    addDefaultMushrooms(biomeKey);
    addSwampExtraVegetation(biomeKey);
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, AquaticPlacements::SEAGRASS_SWAMP);
}

void BiomeFeatureRegistry::setupOcean(const std::string& biomeKey, bool deep) {
    // Reference: OverworldBiomes.java baseOceanGeneration() + ocean()
    globalOverworldGeneration(biomeKey);
    addDefaultOres(biomeKey);
    addDefaultSoftDisks(biomeKey);
    addWaterTrees(biomeKey);
    addDefaultFlowers(biomeKey);
    addDefaultGrass(biomeKey);
    addDefaultMushrooms(biomeKey);
    addDefaultExtraVegetation(biomeKey, true);
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION,
               deep ? AquaticPlacements::SEAGRASS_DEEP : AquaticPlacements::SEAGRASS_NORMAL);
    addColdOceanExtraVegetation(biomeKey);
}

void BiomeFeatureRegistry::setupColdOcean(const std::string& biomeKey, bool deep) {
    // Reference: OverworldBiomes.java coldOcean()
    globalOverworldGeneration(biomeKey);
    addDefaultOres(biomeKey);
    addDefaultSoftDisks(biomeKey);
    addWaterTrees(biomeKey);
    addDefaultFlowers(biomeKey);
    addDefaultGrass(biomeKey);
    addDefaultMushrooms(biomeKey);
    addDefaultExtraVegetation(biomeKey, true);
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION,
               deep ? AquaticPlacements::SEAGRASS_DEEP_COLD : AquaticPlacements::SEAGRASS_COLD);
    addColdOceanExtraVegetation(biomeKey);
}

void BiomeFeatureRegistry::setupLukewarmOcean(const std::string& biomeKey, bool deep) {
    // Reference: OverworldBiomes.java lukeWarmOcean()
    globalOverworldGeneration(biomeKey);
    addDefaultOres(biomeKey);
    addDefaultSoftDisks(biomeKey);
    addWaterTrees(biomeKey);
    addDefaultFlowers(biomeKey);
    addDefaultGrass(biomeKey);
    addDefaultMushrooms(biomeKey);
    addDefaultExtraVegetation(biomeKey, true);
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION,
               deep ? AquaticPlacements::SEAGRASS_DEEP_WARM : AquaticPlacements::SEAGRASS_WARM);
    addLukeWarmKelp(biomeKey);
}

void BiomeFeatureRegistry::setupWarmOcean(const std::string& biomeKey) {
    // Reference: OverworldBiomes.java warmOcean()
    globalOverworldGeneration(biomeKey);
    addDefaultOres(biomeKey);
    addDefaultSoftDisks(biomeKey);
    addWaterTrees(biomeKey);
    addDefaultFlowers(biomeKey);
    addDefaultGrass(biomeKey);
    addDefaultMushrooms(biomeKey);
    addDefaultExtraVegetation(biomeKey, true);
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, AquaticPlacements::WARM_OCEAN_VEGETATION);
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, AquaticPlacements::SEAGRASS_WARM);
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, AquaticPlacements::SEA_PICKLE);
}

void BiomeFeatureRegistry::setupFrozenOcean(const std::string& biomeKey, bool deep) {
    // Reference: OverworldBiomes.java frozenOcean()
    addIcebergs(biomeKey);
    globalOverworldGeneration(biomeKey);
    addBlueIce(biomeKey);
    addDefaultOres(biomeKey);
    addDefaultSoftDisks(biomeKey);
    addWaterTrees(biomeKey);
    addDefaultFlowers(biomeKey);
    addDefaultGrass(biomeKey);
    addDefaultMushrooms(biomeKey);
    addDefaultExtraVegetation(biomeKey, true);
}

void BiomeFeatureRegistry::setupTaiga(const std::string& biomeKey, bool snowy) {
    // Reference: OverworldBiomes.java taiga()
    globalOverworldGeneration(biomeKey);
    addFerns(biomeKey);
    addDefaultOres(biomeKey);
    addDefaultSoftDisks(biomeKey);
    addTaigaTrees(biomeKey);
    addDefaultFlowers(biomeKey);
    addTaigaGrass(biomeKey);
    addDefaultExtraVegetation(biomeKey, true);
    if (snowy) {
        addRareBerryBushes(biomeKey);
    } else {
        addCommonBerryBushes(biomeKey);
    }
}

void BiomeFeatureRegistry::setupForest(const std::string& biomeKey, bool birch, bool tall, bool flower) {
    // Reference: OverworldBiomes.java forest()
    globalOverworldGeneration(biomeKey);
    if (flower) {
        addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::FLOWER_FOREST_FLOWERS);
    } else {
        addForestFlowers(biomeKey);
    }
    addDefaultOres(biomeKey);
    addDefaultSoftDisks(biomeKey);
    if (flower) {
        addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::TREES_FLOWER_FOREST);
        addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::FLOWER_FLOWER_FOREST);
        addDefaultGrass(biomeKey);
    } else {
        if (birch) {
            addBirchForestFlowers(biomeKey);
            if (tall) {
                addTallBirchTrees(biomeKey);
            } else {
                addBirchTrees(biomeKey);
            }
        } else {
            addOtherBirchTrees(biomeKey);
        }
        addBushes(biomeKey);
        addDefaultFlowers(biomeKey);
        addForestGrass(biomeKey);
    }
    addDefaultMushrooms(biomeKey);
    addDefaultExtraVegetation(biomeKey, true);
}

void BiomeFeatureRegistry::setupDarkForest(const std::string& biomeKey, bool isPaleGarden) {
    // Reference: OverworldBiomes.java darkForest()
    globalOverworldGeneration(biomeKey);
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION,
               isPaleGarden ? VegetationPlacements::PALE_GARDEN_VEGETATION : VegetationPlacements::DARK_FOREST_VEGETATION);
    if (!isPaleGarden) {
        addForestFlowers(biomeKey);
    } else {
        addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::PALE_MOSS_PATCH);
        addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::PALE_GARDEN_FLOWERS);
    }
    addDefaultOres(biomeKey);
    addDefaultSoftDisks(biomeKey);
    if (!isPaleGarden) {
        addDefaultFlowers(biomeKey);
    } else {
        addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::FLOWER_PALE_GARDEN);
    }
    addForestGrass(biomeKey);
    if (!isPaleGarden) {
        addDefaultMushrooms(biomeKey);
        addLeafLitterPatch(biomeKey);
    }
    addDefaultExtraVegetation(biomeKey, true);
}

void BiomeFeatureRegistry::setupSavanna(const std::string& biomeKey, bool shattered, bool plateau) {
    // Reference: OverworldBiomes.java savanna()
    globalOverworldGeneration(biomeKey);
    if (!shattered) {
        addSavannaGrass(biomeKey);
    }
    addDefaultOres(biomeKey);
    addDefaultSoftDisks(biomeKey);
    if (shattered) {
        addShatteredSavannaTrees(biomeKey);
        addDefaultFlowers(biomeKey);
        addShatteredSavannaGrass(biomeKey);
    } else {
        addSavannaTrees(biomeKey);
        addWarmFlowers(biomeKey);
        addSavannaExtraGrass(biomeKey);
    }
    addDefaultMushrooms(biomeKey);
    addDefaultExtraVegetation(biomeKey, true);
}

void BiomeFeatureRegistry::setupBadlands(const std::string& biomeKey, bool wooded) {
    // Reference: OverworldBiomes.java badlands()
    globalOverworldGeneration(biomeKey);
    addDefaultOres(biomeKey);
    addExtraGold(biomeKey);
    addDefaultSoftDisks(biomeKey);
    if (wooded) {
        addBadlandsTrees(biomeKey);
    }
    addBadlandGrass(biomeKey);
    addDefaultMushrooms(biomeKey);
    addBadlandExtraVegetation(biomeKey);
}

void BiomeFeatureRegistry::setupDesert(const std::string& biomeKey) {
    // Reference: OverworldBiomes.java desert()
    addFossilDecoration(biomeKey);
    globalOverworldGeneration(biomeKey);
    addDefaultOres(biomeKey);
    addDefaultSoftDisks(biomeKey);
    addDefaultFlowers(biomeKey);
    addDefaultGrass(biomeKey);
    addDesertVegetation(biomeKey);
    addDefaultMushrooms(biomeKey);
    addDesertExtraVegetation(biomeKey);
    addDesertExtraDecoration(biomeKey);
}

void BiomeFeatureRegistry::setupWindsweptHills(const std::string& biomeKey, bool moreTrees) {
    // Reference: OverworldBiomes.java windsweptHills()
    globalOverworldGeneration(biomeKey);
    addDefaultOres(biomeKey);
    addDefaultSoftDisks(biomeKey);
    if (moreTrees) {
        addMountainForestTrees(biomeKey);
    } else {
        addMountainTrees(biomeKey);
    }
    addBushes(biomeKey);
    addDefaultFlowers(biomeKey);
    addDefaultGrass(biomeKey);
    addDefaultMushrooms(biomeKey);
    addDefaultExtraVegetation(biomeKey, true);
    addExtraEmeralds(biomeKey);
    addInfestedStone(biomeKey);
}

void BiomeFeatureRegistry::setupRiver(const std::string& biomeKey, bool frozen) {
    // Reference: OverworldBiomes.java river()
    globalOverworldGeneration(biomeKey);
    addDefaultOres(biomeKey);
    addDefaultSoftDisks(biomeKey);
    addWaterTrees(biomeKey);
    addBushes(biomeKey);
    addDefaultFlowers(biomeKey);
    addDefaultGrass(biomeKey);
    addDefaultMushrooms(biomeKey);
    addDefaultExtraVegetation(biomeKey, true);
    if (!frozen) {
        addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, AquaticPlacements::SEAGRASS_RIVER);
    }
}

void BiomeFeatureRegistry::setupBeach(const std::string& biomeKey, bool snowy, bool stony) {
    // Reference: OverworldBiomes.java beach()
    globalOverworldGeneration(biomeKey);
    addDefaultOres(biomeKey);
    addDefaultSoftDisks(biomeKey);
    addDefaultFlowers(biomeKey);
    addDefaultGrass(biomeKey);
    addDefaultMushrooms(biomeKey);
    addDefaultExtraVegetation(biomeKey, true);
}

void BiomeFeatureRegistry::setupMeadowOrCherryGrove(const std::string& biomeKey, bool cherryGrove) {
    // Reference: OverworldBiomes.java meadowOrCherryGrove()
    globalOverworldGeneration(biomeKey);
    addPlainGrass(biomeKey);
    addDefaultOres(biomeKey);
    addDefaultSoftDisks(biomeKey);
    if (cherryGrove) {
        addCherryGroveVegetation(biomeKey);
    } else {
        addMeadowVegetation(biomeKey);
    }
    addExtraEmeralds(biomeKey);
    addInfestedStone(biomeKey);
}

void BiomeFeatureRegistry::setupPeaks(const std::string& biomeKey, bool stony) {
    // Reference: OverworldBiomes.java basePeaks() / stonyPeaks()
    globalOverworldGeneration(biomeKey);
    if (!stony) {
        addFrozenSprings(biomeKey);
    }
    addDefaultOres(biomeKey);
    addDefaultSoftDisks(biomeKey);
    addExtraEmeralds(biomeKey);
    addInfestedStone(biomeKey);
}

void BiomeFeatureRegistry::setupSnowySlopes(const std::string& biomeKey) {
    // Reference: OverworldBiomes.java snowySlopes()
    globalOverworldGeneration(biomeKey);
    addFrozenSprings(biomeKey);
    addDefaultOres(biomeKey);
    addDefaultSoftDisks(biomeKey);
    addDefaultExtraVegetation(biomeKey, false);
    addExtraEmeralds(biomeKey);
    addInfestedStone(biomeKey);
}

void BiomeFeatureRegistry::setupGrove(const std::string& biomeKey) {
    // Reference: OverworldBiomes.java grove()
    globalOverworldGeneration(biomeKey);
    addFrozenSprings(biomeKey);
    addDefaultOres(biomeKey);
    addDefaultSoftDisks(biomeKey);
    addGroveTrees(biomeKey);
    addDefaultExtraVegetation(biomeKey, false);
    addExtraEmeralds(biomeKey);
    addInfestedStone(biomeKey);
}

void BiomeFeatureRegistry::setupJungle(const std::string& biomeKey, bool bamboo, bool sparse, bool core) {
    // Reference: OverworldBiomes.java baseJungle()
    globalOverworldGeneration(biomeKey);
    addDefaultOres(biomeKey);
    addDefaultSoftDisks(biomeKey);
    if (bamboo) {
        addBambooVegetation(biomeKey);
    } else {
        if (core) {
            addLightBambooVegetation(biomeKey);
        }
        if (sparse) {
            addSparseJungleTrees(biomeKey);
        } else {
            addJungleTrees(biomeKey);
        }
    }
    addWarmFlowers(biomeKey);
    addJungleGrass(biomeKey);
    addDefaultMushrooms(biomeKey);
    addDefaultExtraVegetation(biomeKey, true);
    addJungleVines(biomeKey);
    if (sparse) {
        addSparseJungleMelons(biomeKey);
    } else {
        addJungleMelons(biomeKey);
    }
}

void BiomeFeatureRegistry::setupOldGrowthTaiga(const std::string& biomeKey, bool spruce) {
    // Reference: OverworldBiomes.java oldGrowthTaiga()
    globalOverworldGeneration(biomeKey);
    addMossyStoneBlock(biomeKey);
    addFerns(biomeKey);
    addDefaultOres(biomeKey);
    addDefaultSoftDisks(biomeKey);
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION,
               spruce ? VegetationPlacements::TREES_OLD_GROWTH_SPRUCE_TAIGA : VegetationPlacements::TREES_OLD_GROWTH_PINE_TAIGA);
    addDefaultFlowers(biomeKey);
    addGiantTaigaVegetation(biomeKey);
    addDefaultMushrooms(biomeKey);
    addDefaultExtraVegetation(biomeKey, true);
    addCommonBerryBushes(biomeKey);
}

void BiomeFeatureRegistry::setupLushCaves(const std::string& biomeKey) {
    // Reference: OverworldBiomes.java lushCaves()
    globalOverworldGeneration(biomeKey);
    addPlainGrass(biomeKey);
    addDefaultOres(biomeKey);
    addLushCavesSpecialOres(biomeKey);
    addDefaultSoftDisks(biomeKey);
    addLushCavesVegetationFeatures(biomeKey);
}

void BiomeFeatureRegistry::setupDripstoneCaves(const std::string& biomeKey) {
    // Reference: OverworldBiomes.java dripstoneCaves()
    globalOverworldGeneration(biomeKey);
    addPlainGrass(biomeKey);
    addDefaultOres(biomeKey, true);  // largeCopperBlobs = true
    addDefaultSoftDisks(biomeKey);
    addPlainVegetation(biomeKey);
    addDefaultMushrooms(biomeKey);
    addDefaultExtraVegetation(biomeKey, false);
    addDripstone(biomeKey);
}

void BiomeFeatureRegistry::setupDeepDark(const std::string& biomeKey) {
    // Reference: OverworldBiomes.java deepDark()
    // NOTE: deepDark has special carvers, not globalOverworldGeneration
    addDefaultCrystalFormations(biomeKey);
    addDefaultMonsterRoom(biomeKey);
    addDefaultUndergroundVariety(biomeKey);
    addSurfaceFreezing(biomeKey);
    addPlainGrass(biomeKey);
    addDefaultOres(biomeKey);
    addDefaultSoftDisks(biomeKey);
    addPlainVegetation(biomeKey);
    addDefaultMushrooms(biomeKey);
    addDefaultExtraVegetation(biomeKey, false);
    addSculk(biomeKey);
}

void BiomeFeatureRegistry::setupMangroveSwamp(const std::string& biomeKey) {
    // Reference: OverworldBiomes.java mangroveSwamp()
    addFossilDecoration(biomeKey);
    globalOverworldGeneration(biomeKey);
    addDefaultOres(biomeKey);
    // addMangroveSwampDisks - DISK_GRASS, DISK_CLAY
    addFeature(biomeKey, GenerationStep::UNDERGROUND_ORES, MiscOverworldPlacements::DISK_GRASS);
    addFeature(biomeKey, GenerationStep::UNDERGROUND_ORES, MiscOverworldPlacements::DISK_CLAY);
    // addMangroveSwampVegetation
    int step = GenerationStep::VEGETAL_DECORATION;
    addFeature(biomeKey, step, VegetationPlacements::TREES_MANGROVE);
    addFeature(biomeKey, step, VegetationPlacements::PATCH_GRASS_NORMAL);
    addFeature(biomeKey, step, VegetationPlacements::PATCH_DEAD_BUSH);
    addFeature(biomeKey, step, VegetationPlacements::PATCH_WATERLILY);
    // addMangroveSwampExtraVegetation
    addFeature(biomeKey, step, AquaticPlacements::SEAGRASS_SWAMP);
    addFeature(biomeKey, step, VegetationPlacements::PATCH_FIREFLY_BUSH_NEAR_WATER);
}

// =============================================================================
// Bootstrap - sets up all biomes in the exact order Java processes them
// =============================================================================

void BiomeFeatureRegistry::bootstrap() {
    if (s_initialized) return;

    // Initialize feature registries first
    features::OreFeatures::bootstrap();
    features::VegetationFeatures::bootstrap();
    features::TreeFeatures::bootstrap();
    features::AquaticFeatures::bootstrap();
    features::CaveFeatures::bootstrap();
    features::MiscOverworldFeatures::bootstrap();
    placement::OrePlacements::bootstrap();
    placement::VegetationPlacements::bootstrap();
    placement::TreePlacements::bootstrap();
    placement::AquaticPlacements::bootstrap();
    placement::CavePlacements::bootstrap();
    placement::MiscOverworldPlacements::bootstrap();

    // All overworld biomes in EXACT order from Java's biomeSource.possibleBiomes()
    // Reference: MultiNoiseBiomeSource.createFromPreset(OVERWORLD).possibleBiomes()
    std::vector<std::string> overworldBiomes = {
        "minecraft:mushroom_fields",          // 0
        "minecraft:deep_frozen_ocean",        // 1
        "minecraft:frozen_ocean",             // 2
        "minecraft:deep_cold_ocean",          // 3
        "minecraft:cold_ocean",               // 4
        "minecraft:deep_ocean",               // 5
        "minecraft:ocean",                    // 6
        "minecraft:deep_lukewarm_ocean",      // 7
        "minecraft:lukewarm_ocean",           // 8
        "minecraft:warm_ocean",               // 9
        "minecraft:stony_shore",              // 10
        "minecraft:swamp",                    // 11
        "minecraft:mangrove_swamp",           // 12
        "minecraft:snowy_slopes",             // 13
        "minecraft:snowy_plains",             // 14
        "minecraft:snowy_beach",              // 15
        "minecraft:windswept_gravelly_hills", // 16
        "minecraft:grove",                    // 17
        "minecraft:windswept_hills",          // 18
        "minecraft:snowy_taiga",              // 19
        "minecraft:windswept_forest",         // 20
        "minecraft:taiga",                    // 21
        "minecraft:plains",                   // 22
        "minecraft:meadow",                   // 23
        "minecraft:beach",                    // 24
        "minecraft:forest",                   // 25
        "minecraft:old_growth_spruce_taiga",  // 26
        "minecraft:flower_forest",            // 27
        "minecraft:birch_forest",             // 28
        "minecraft:dark_forest",              // 29
        "minecraft:pale_garden",              // 30
        "minecraft:savanna_plateau",          // 31
        "minecraft:savanna",                  // 32
        "minecraft:jungle",                   // 33
        "minecraft:badlands",                 // 34
        "minecraft:desert",                   // 35
        "minecraft:wooded_badlands",          // 36
        "minecraft:jagged_peaks",             // 37
        "minecraft:stony_peaks",              // 38
        "minecraft:frozen_river",             // 39
        "minecraft:river",                    // 40
        "minecraft:ice_spikes",               // 41
        "minecraft:old_growth_pine_taiga",    // 42
        "minecraft:sunflower_plains",         // 43
        "minecraft:old_growth_birch_forest",  // 44
        "minecraft:sparse_jungle",            // 45
        "minecraft:bamboo_jungle",            // 46
        "minecraft:eroded_badlands",          // 47
        "minecraft:windswept_savanna",        // 48
        "minecraft:cherry_grove",             // 49
        "minecraft:frozen_peaks",             // 50
        "minecraft:dripstone_caves",          // 51
        "minecraft:lush_caves",               // 52
        "minecraft:deep_dark"                 // 53
    };

    s_biomeKeyOrder = overworldBiomes;

    // Setup each biome individually with its exact feature order
    // The order of biomes processed matters for FeatureSorter's global index assignment!

    // 0: mushroom_fields
    setupMushroomFields("minecraft:mushroom_fields");

    // 1-2: frozen oceans
    setupFrozenOcean("minecraft:deep_frozen_ocean", true);
    setupFrozenOcean("minecraft:frozen_ocean", false);

    // 3-4: cold oceans
    setupColdOcean("minecraft:deep_cold_ocean", true);
    setupColdOcean("minecraft:cold_ocean", false);

    // 5-6: regular oceans
    setupOcean("minecraft:deep_ocean", true);
    setupOcean("minecraft:ocean", false);

    // 7-8: lukewarm oceans
    setupLukewarmOcean("minecraft:deep_lukewarm_ocean", true);
    setupLukewarmOcean("minecraft:lukewarm_ocean", false);

    // 9: warm ocean
    setupWarmOcean("minecraft:warm_ocean");

    // 10: stony shore (uses beach)
    setupBeach("minecraft:stony_shore", false, true);

    // 11: swamp
    setupSwamp("minecraft:swamp");

    // 12: mangrove_swamp
    setupMangroveSwamp("minecraft:mangrove_swamp");

    // 13: snowy_slopes
    setupSnowySlopes("minecraft:snowy_slopes");

    // 14: snowy_plains
    setupPlains("minecraft:snowy_plains", false, true, false);

    // 15: snowy_beach
    setupBeach("minecraft:snowy_beach", true, false);

    // 16: windswept_gravelly_hills (windsweptHills with more trees = false, but it's gravelly)
    setupWindsweptHills("minecraft:windswept_gravelly_hills", false);

    // 17: grove
    setupGrove("minecraft:grove");

    // 18: windswept_hills
    setupWindsweptHills("minecraft:windswept_hills", false);

    // 19: snowy_taiga
    setupTaiga("minecraft:snowy_taiga", true);

    // 20: windswept_forest
    setupWindsweptHills("minecraft:windswept_forest", true);

    // 21: taiga
    setupTaiga("minecraft:taiga", false);

    // 22: plains
    setupPlains("minecraft:plains", false, false, false);

    // 23: meadow
    setupMeadowOrCherryGrove("minecraft:meadow", false);

    // 24: beach
    setupBeach("minecraft:beach", false, false);

    // 25: forest
    setupForest("minecraft:forest", false, false, false);

    // 26: old_growth_spruce_taiga
    setupOldGrowthTaiga("minecraft:old_growth_spruce_taiga", true);

    // 27: flower_forest
    setupForest("minecraft:flower_forest", false, false, true);

    // 28: birch_forest
    setupForest("minecraft:birch_forest", true, false, false);

    // 29: dark_forest
    setupDarkForest("minecraft:dark_forest", false);

    // 30: pale_garden
    setupDarkForest("minecraft:pale_garden", true);

    // 31: savanna_plateau
    setupSavanna("minecraft:savanna_plateau", false, true);

    // 32: savanna
    setupSavanna("minecraft:savanna", false, false);

    // 33: jungle
    setupJungle("minecraft:jungle", false, false, true);

    // 34: badlands
    setupBadlands("minecraft:badlands", false);

    // 35: desert
    setupDesert("minecraft:desert");

    // 36: wooded_badlands
    setupBadlands("minecraft:wooded_badlands", true);

    // 37: jagged_peaks
    setupPeaks("minecraft:jagged_peaks", false);

    // 38: stony_peaks
    setupPeaks("minecraft:stony_peaks", true);

    // 39: frozen_river
    setupRiver("minecraft:frozen_river", true);

    // 40: river
    setupRiver("minecraft:river", false);

    // 41: ice_spikes
    setupPlains("minecraft:ice_spikes", false, true, true);

    // 42: old_growth_pine_taiga
    setupOldGrowthTaiga("minecraft:old_growth_pine_taiga", false);

    // 43: sunflower_plains
    setupPlains("minecraft:sunflower_plains", true, false, false);

    // 44: old_growth_birch_forest
    setupForest("minecraft:old_growth_birch_forest", true, true, false);

    // 45: sparse_jungle
    setupJungle("minecraft:sparse_jungle", false, true, false);

    // 46: bamboo_jungle
    setupJungle("minecraft:bamboo_jungle", true, false, true);

    // 47: eroded_badlands (same as regular badlands)
    setupBadlands("minecraft:eroded_badlands", false);

    // 48: windswept_savanna
    setupSavanna("minecraft:windswept_savanna", true, false);

    // 49: cherry_grove
    setupMeadowOrCherryGrove("minecraft:cherry_grove", true);

    // 50: frozen_peaks
    setupPeaks("minecraft:frozen_peaks", false);

    // 51: dripstone_caves
    setupDripstoneCaves("minecraft:dripstone_caves");

    // 52: lush_caves
    setupLushCaves("minecraft:lush_caves");

    // 53: deep_dark
    setupDeepDark("minecraft:deep_dark");

    s_initialized = true;
}

const std::vector<const PlacedFeature*>& BiomeFeatureRegistry::getFeaturesForStep(
    const std::string& biomeKey,
    int step
) {
    if (!s_initialized) {
        bootstrap();
    }

    auto it = s_biomeFeatures.find(biomeKey);
    if (it == s_biomeFeatures.end()) {
        return s_emptyFeatures;
    }

    if (step < 0 || step >= static_cast<int>(it->second.size())) {
        return s_emptyFeatures;
    }

    return it->second[step];
}

const std::vector<std::vector<const PlacedFeature*>>& BiomeFeatureRegistry::getFeaturesForBiome(
    const std::string& biomeKey
) {
    if (!s_initialized) {
        bootstrap();
    }

    auto it = s_biomeFeatures.find(biomeKey);
    if (it == s_biomeFeatures.end()) {
        return s_emptyBiomeFeatures;
    }
    return it->second;
}

bool BiomeFeatureRegistry::hasFeature(const std::string& biomeKey, const PlacedFeature* feature) {
    if (!s_initialized) {
        bootstrap();
    }

    auto it = s_biomeFeatures.find(biomeKey);
    if (it == s_biomeFeatures.end()) {
        return false;
    }

    for (const auto& stepFeatures : it->second) {
        for (const auto* f : stepFeatures) {
            if (f == feature) {
                return true;
            }
        }
    }
    return false;
}

const std::vector<std::string>& BiomeFeatureRegistry::getAllBiomeKeys() {
    if (!s_initialized) {
        bootstrap();
    }
    return s_biomeKeyOrder;
}

} // namespace worldgen
} // namespace data
} // namespace minecraft

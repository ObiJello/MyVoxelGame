#include <unordered_set>
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
#include "data/worldgen/features/NetherFeatures.h"
#include "data/worldgen/placement/NetherPlacements.h"
#include "data/worldgen/features/EndFeatures.h"
#include "data/worldgen/placement/EndPlacements.h"
#include "data/worldgen/features/HushFeatures.h"
#include "data/worldgen/placement/HushPlacements.h"
#include "data/worldgen/features/AetherFeatures.h"
#include "data/worldgen/placement/AetherPlacements.h"
#include "data/worldgen/features/TwilightFeatures.h"
#include "data/worldgen/placement/TwilightPlacements.h"
#include <set>
#include <iostream>
#include <mutex>

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
std::atomic<bool> BiomeFeatureRegistry::s_initialized{false};

// Empty vector for missing biomes/steps
static const std::vector<const PlacedFeature*> s_emptyFeatures;
static const std::vector<std::vector<const PlacedFeature*>> s_emptyBiomeFeatures(GenerationStep::DECORATION_COUNT);

static bool s_debugNullptrFeatures = true;
static bool s_debugIceFeatures = false;
static std::once_flag s_bootstrapOnce;
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

void BiomeFeatureRegistry::setupSulfurCaves(const std::string& biomeKey) {
    // Reference: 26.3 OverworldBiomes.java sulfurCaves()
    globalOverworldGeneration(biomeKey);
    addPlainGrass(biomeKey);
    addDefaultOres(biomeKey);
    addDefaultSoftDisks(biomeKey);
    // BiomeDefaultFeatures.addSulfurCavesFeatures
    addFeature(biomeKey, GenerationStep::LAKES, CavePlacements::ROOTED_SULFUR_SPRING);
    addFeature(biomeKey, GenerationStep::LAKES, MiscOverworldPlacements::SULFUR_POOL);
    addFeature(biomeKey, GenerationStep::UNDERGROUND_DECORATION, CavePlacements::SULFUR_SPIKE_CLUSTER);
    addFeature(biomeKey, GenerationStep::UNDERGROUND_DECORATION, CavePlacements::SULFUR_SPIKE);
}

void BiomeFeatureRegistry::setupDappledForest(const std::string& biomeKey) {
    // Reference: 26.3 OverworldBiomes.java dappledForest()
    globalOverworldGeneration(biomeKey);
    addDefaultOres(biomeKey);
    addDefaultSoftDisks(biomeKey);
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::TREES_DAPPLED_FOREST);
    // BiomeDefaultFeatures.addDappledForestVegetation
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::BROWN_MUSHROOM_DAPPLED_FOREST);
    addFeature(biomeKey, GenerationStep::VEGETAL_DECORATION, VegetationPlacements::PATCH_RED_SHRUB);
    addForestGrass(biomeKey);
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

// =============================================================================
// Nether biomes - Reference: NetherBiomes.java (exact addFeature call order)
// =============================================================================

void BiomeFeatureRegistry::addNetherDefaultOres(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 410-416
    int step = static_cast<int>(GenerationStep::Decoration::UNDERGROUND_DECORATION);
    addFeature(biomeKey, step, placement::OrePlacements::ORE_GRAVEL_NETHER);
    addFeature(biomeKey, step, placement::OrePlacements::ORE_BLACKSTONE);
    addFeature(biomeKey, step, placement::OrePlacements::ORE_GOLD_NETHER);
    addFeature(biomeKey, step, placement::OrePlacements::ORE_QUARTZ_NETHER);
    addAncientDebris(biomeKey);
}

void BiomeFeatureRegistry::addAncientDebris(const std::string& biomeKey) {
    // Reference: BiomeDefaultFeatures.java lines 418-421
    int step = static_cast<int>(GenerationStep::Decoration::UNDERGROUND_DECORATION);
    addFeature(biomeKey, step, placement::OrePlacements::ORE_ANCIENT_DEBRIS_LARGE);
    addFeature(biomeKey, step, placement::OrePlacements::ORE_ANCIENT_DEBRIS_SMALL);
}

void BiomeFeatureRegistry::setupNetherWastes(const std::string& biomeKey) {
    // Reference: NetherBiomes.java netherWastes() lines 38-41
    int vegetal = static_cast<int>(GenerationStep::Decoration::VEGETAL_DECORATION);
    int underground = static_cast<int>(GenerationStep::Decoration::UNDERGROUND_DECORATION);
    addFeature(biomeKey, vegetal, MiscOverworldPlacements::SPRING_LAVA);
    addDefaultMushrooms(biomeKey);
    addFeature(biomeKey, underground, NetherPlacements::SPRING_OPEN);
    addFeature(biomeKey, underground, NetherPlacements::PATCH_FIRE);
    addFeature(biomeKey, underground, NetherPlacements::PATCH_SOUL_FIRE);
    addFeature(biomeKey, underground, NetherPlacements::GLOWSTONE_EXTRA);
    addFeature(biomeKey, underground, NetherPlacements::GLOWSTONE);
    addFeature(biomeKey, underground, VegetationPlacements::BROWN_MUSHROOM_NETHER);
    addFeature(biomeKey, underground, VegetationPlacements::RED_MUSHROOM_NETHER);
    addFeature(biomeKey, underground, placement::OrePlacements::ORE_MAGMA);
    addFeature(biomeKey, underground, NetherPlacements::SPRING_CLOSED);
    addNetherDefaultOres(biomeKey);
}

void BiomeFeatureRegistry::setupSoulSandValley(const std::string& biomeKey) {
    // Reference: NetherBiomes.java soulSandValley() line 49
    int vegetal = static_cast<int>(GenerationStep::Decoration::VEGETAL_DECORATION);
    int localMods = static_cast<int>(GenerationStep::Decoration::LOCAL_MODIFICATIONS);
    int underground = static_cast<int>(GenerationStep::Decoration::UNDERGROUND_DECORATION);
    addFeature(biomeKey, vegetal, MiscOverworldPlacements::SPRING_LAVA);
    addFeature(biomeKey, localMods, NetherPlacements::BASALT_PILLAR);
    addFeature(biomeKey, underground, NetherPlacements::SPRING_OPEN);
    addFeature(biomeKey, underground, NetherPlacements::PATCH_FIRE);
    addFeature(biomeKey, underground, NetherPlacements::PATCH_SOUL_FIRE);
    addFeature(biomeKey, underground, NetherPlacements::GLOWSTONE_EXTRA);
    addFeature(biomeKey, underground, NetherPlacements::GLOWSTONE);
    addFeature(biomeKey, underground, NetherPlacements::PATCH_CRIMSON_ROOTS);
    addFeature(biomeKey, underground, placement::OrePlacements::ORE_MAGMA);
    addFeature(biomeKey, underground, NetherPlacements::SPRING_CLOSED);
    addFeature(biomeKey, underground, placement::OrePlacements::ORE_SOUL_SAND);
    addNetherDefaultOres(biomeKey);
}

void BiomeFeatureRegistry::setupCrimsonForest(const std::string& biomeKey) {
    // Reference: NetherBiomes.java crimsonForest() lines 63-66
    int vegetal = static_cast<int>(GenerationStep::Decoration::VEGETAL_DECORATION);
    int underground = static_cast<int>(GenerationStep::Decoration::UNDERGROUND_DECORATION);
    addFeature(biomeKey, vegetal, MiscOverworldPlacements::SPRING_LAVA);
    addDefaultMushrooms(biomeKey);
    addFeature(biomeKey, underground, NetherPlacements::SPRING_OPEN);
    addFeature(biomeKey, underground, NetherPlacements::PATCH_FIRE);
    addFeature(biomeKey, underground, NetherPlacements::GLOWSTONE_EXTRA);
    addFeature(biomeKey, underground, NetherPlacements::GLOWSTONE);
    addFeature(biomeKey, underground, placement::OrePlacements::ORE_MAGMA);
    addFeature(biomeKey, underground, NetherPlacements::SPRING_CLOSED);
    addFeature(biomeKey, vegetal, NetherPlacements::WEEPING_VINES);
    addFeature(biomeKey, vegetal, TreePlacements::CRIMSON_FUNGI);
    addFeature(biomeKey, vegetal, NetherPlacements::CRIMSON_FOREST_VEGETATION);
    addNetherDefaultOres(biomeKey);
}

void BiomeFeatureRegistry::setupWarpedForest(const std::string& biomeKey) {
    // Reference: NetherBiomes.java warpedForest() lines 72-75
    int vegetal = static_cast<int>(GenerationStep::Decoration::VEGETAL_DECORATION);
    int underground = static_cast<int>(GenerationStep::Decoration::UNDERGROUND_DECORATION);
    addFeature(biomeKey, vegetal, MiscOverworldPlacements::SPRING_LAVA);
    addDefaultMushrooms(biomeKey);
    addFeature(biomeKey, underground, NetherPlacements::SPRING_OPEN);
    addFeature(biomeKey, underground, NetherPlacements::PATCH_FIRE);
    addFeature(biomeKey, underground, NetherPlacements::PATCH_SOUL_FIRE);
    addFeature(biomeKey, underground, NetherPlacements::GLOWSTONE_EXTRA);
    addFeature(biomeKey, underground, NetherPlacements::GLOWSTONE);
    addFeature(biomeKey, underground, placement::OrePlacements::ORE_MAGMA);
    addFeature(biomeKey, underground, NetherPlacements::SPRING_CLOSED);
    addFeature(biomeKey, vegetal, TreePlacements::WARPED_FUNGI);
    addFeature(biomeKey, vegetal, NetherPlacements::WARPED_FOREST_VEGETATION);
    addFeature(biomeKey, vegetal, NetherPlacements::NETHER_SPROUTS);
    addFeature(biomeKey, vegetal, NetherPlacements::TWISTING_VINES);
    addNetherDefaultOres(biomeKey);
}

void BiomeFeatureRegistry::setupBasaltDeltas(const std::string& biomeKey) {
    // Reference: NetherBiomes.java basaltDeltas() lines 56-57
    int surfaceStructures = static_cast<int>(GenerationStep::Decoration::SURFACE_STRUCTURES);
    int underground = static_cast<int>(GenerationStep::Decoration::UNDERGROUND_DECORATION);
    addFeature(biomeKey, surfaceStructures, NetherPlacements::DELTA);
    addFeature(biomeKey, surfaceStructures, NetherPlacements::SMALL_BASALT_COLUMNS);
    addFeature(biomeKey, surfaceStructures, NetherPlacements::LARGE_BASALT_COLUMNS);
    addFeature(biomeKey, underground, NetherPlacements::BASALT_BLOBS);
    addFeature(biomeKey, underground, NetherPlacements::BLACKSTONE_BLOBS);
    addFeature(biomeKey, underground, NetherPlacements::SPRING_DELTA);
    addFeature(biomeKey, underground, NetherPlacements::PATCH_FIRE);
    addFeature(biomeKey, underground, NetherPlacements::PATCH_SOUL_FIRE);
    addFeature(biomeKey, underground, NetherPlacements::GLOWSTONE_EXTRA);
    addFeature(biomeKey, underground, NetherPlacements::GLOWSTONE);
    addFeature(biomeKey, underground, VegetationPlacements::BROWN_MUSHROOM_NETHER);
    addFeature(biomeKey, underground, VegetationPlacements::RED_MUSHROOM_NETHER);
    addFeature(biomeKey, underground, placement::OrePlacements::ORE_MAGMA);
    addFeature(biomeKey, underground, NetherPlacements::SPRING_CLOSED_DOUBLE);
    addFeature(biomeKey, underground, placement::OrePlacements::ORE_GOLD_DELTAS);
    addFeature(biomeKey, underground, placement::OrePlacements::ORE_QUARTZ_DELTAS);
    addAncientDebris(biomeKey);
}

// =============================================================================
// End biomes - Reference: EndBiomes.java
// =============================================================================

void BiomeFeatureRegistry::setupTheEnd(const std::string& biomeKey) {
    // Reference: EndBiomes.java endBiome() line 27
    addFeature(biomeKey, static_cast<int>(GenerationStep::Decoration::SURFACE_STRUCTURES),
               EndPlacements::END_SPIKE);
    addFeature(biomeKey, static_cast<int>(GenerationStep::Decoration::TOP_LAYER_MODIFICATION),
               EndPlacements::END_PLATFORM);
}

void BiomeFeatureRegistry::setupEndHighlands(const std::string& biomeKey) {
    // Reference: EndBiomes.java line 37
    addFeature(biomeKey, static_cast<int>(GenerationStep::Decoration::SURFACE_STRUCTURES),
               EndPlacements::END_GATEWAY_RETURN);
    addFeature(biomeKey, static_cast<int>(GenerationStep::Decoration::VEGETAL_DECORATION),
               EndPlacements::CHORUS_PLANT);
}

void BiomeFeatureRegistry::setupSmallEndIslands(const std::string& biomeKey) {
    // Reference: EndBiomes.java line 42
    addFeature(biomeKey, static_cast<int>(GenerationStep::Decoration::RAW_GENERATION),
               EndPlacements::END_ISLAND_DECORATED);
}

void BiomeFeatureRegistry::setupEndBarrensOrMidlands(const std::string& biomeKey) {
    // Reference: EndBiomes.java - no features; register the biome with empty
    // step lists so lookups are well-defined.
    auto& biome = s_biomeFeatures[biomeKey];
    if (biome.empty()) {
        biome.resize(GenerationStep::DECORATION_COUNT);
    }
}

// =============================================================================
// The Hush biomes - engine-only dimension (DimensionId::Hush), no Java reference
// Step assignments:
//   meadows  UNDERGROUND_ORES: echo ore, resonite ore
//            VEGETAL:          small crystal formations (rare), blooms
//                              (meadow density), hush grass, moss, sparse
//                              trees
//   forest   UNDERGROUND_ORES: echo ore, resonite ore
//            VEGETAL:          forest trees (large/normal mix), blooms,
//                              hush grass, moss
//   barrens  UNDERGROUND_ORES: dense echo ore, resonite ore
//            VEGETAL:          crystal outcrops + small formations (the
//                              crystal fields), surface clusters
//   caverns  UNDERGROUND_ORES: echo ore, resonite ore, cavern resonite ore
//            UNDERGROUND_DECORATION: resonant clusters, crystal clumps
//            (the dripstone_caves slot: dripstone_cluster / pointed_dripstone
//            are UNDERGROUND_DECORATION too)
//   choir    LOCAL_MODIFICATIONS: sunken hushstone-brick ruins
//            UNDERGROUND_ORES: echo ore, resonite ore
//            VEGETAL:          kelp, sea pickles, shore grass
//   deep     LOCAL_MODIFICATIONS: rope bridges over the chasms
//            UNDERGROUND_ORES: echo ore, resonite ore
//            UNDERGROUND_DECORATION: resonant stalactites (hanging formations)
//            VEGETAL:          tall crystal formations, rim grass
//   steppe   LOCAL_MODIFICATIONS: polished hushstone boulders (the
//                              FOREST_ROCK step)
//            UNDERGROUND_ORES: echo ore, resonite ore
//            VEGETAL:          tall crystal formations, short hush grass,
//                              scattered blooms
// The crystal formations run first in VEGETAL, before the grass and blooms
// that would otherwise sit on their mound or under their shards; each
// biome has its own formation placement, so this adds no shared edge.
// Shared placements keep the relative order the first four biomes gave them
// (the feature sorter rejects a cycle): ORE_ECHO before ORE_RESONITE.
// =============================================================================

void BiomeFeatureRegistry::setupHushMeadows(const std::string& biomeKey) {
    int ores = static_cast<int>(GenerationStep::Decoration::UNDERGROUND_ORES);
    int vegetal = static_cast<int>(GenerationStep::Decoration::VEGETAL_DECORATION);
    addFeature(biomeKey, ores, HushPlacements::ORE_ECHO);
    addFeature(biomeKey, ores, HushPlacements::ORE_RESONITE);
    addFeature(biomeKey, vegetal, HushPlacements::CRYSTAL_FORMATIONS_MEADOWS);
    addFeature(biomeKey, vegetal, HushPlacements::PATCH_RESONANCE_BLOOM_MEADOWS);
    addFeature(biomeKey, vegetal, HushPlacements::PATCH_HUSH_GRASS_MEADOWS);
    addFeature(biomeKey, vegetal, HushPlacements::HUSH_MOSS_PATCH);
    addFeature(biomeKey, vegetal, HushPlacements::WHISPERWOOD_SPARSE);
}

void BiomeFeatureRegistry::setupWhisperwoodForest(const std::string& biomeKey) {
    int ores = static_cast<int>(GenerationStep::Decoration::UNDERGROUND_ORES);
    int vegetal = static_cast<int>(GenerationStep::Decoration::VEGETAL_DECORATION);
    addFeature(biomeKey, ores, HushPlacements::ORE_ECHO);
    addFeature(biomeKey, ores, HushPlacements::ORE_RESONITE);
    addFeature(biomeKey, vegetal, HushPlacements::WHISPERWOOD_FOREST);
    addFeature(biomeKey, vegetal, HushPlacements::PATCH_RESONANCE_BLOOM);
    addFeature(biomeKey, vegetal, HushPlacements::PATCH_HUSH_GRASS_FOREST);
    addFeature(biomeKey, vegetal, HushPlacements::HUSH_MOSS_PATCH);
}

void BiomeFeatureRegistry::setupResonantBarrens(const std::string& biomeKey) {
    int ores = static_cast<int>(GenerationStep::Decoration::UNDERGROUND_ORES);
    int vegetal = static_cast<int>(GenerationStep::Decoration::VEGETAL_DECORATION);
    addFeature(biomeKey, ores, HushPlacements::ORE_ECHO_DENSE);
    addFeature(biomeKey, ores, HushPlacements::ORE_RESONITE);
    addFeature(biomeKey, vegetal, HushPlacements::CRYSTAL_FORMATIONS_BARRENS);
    addFeature(biomeKey, vegetal, HushPlacements::CRYSTAL_SHARDS_BARRENS);
    addFeature(biomeKey, vegetal, HushPlacements::RESONANT_CLUSTER_SURFACE);
}

void BiomeFeatureRegistry::setupCrystalCaverns(const std::string& biomeKey) {
    // The Hush's dripstone_caves band (OverworldBiomes.dripstoneCaves shape:
    // the ores every biome gets, plus addDripstone's UNDERGROUND_DECORATION
    // pair). The underground body stays hushstone; the caverns only add
    // what grows on its surfaces.
    int ores = static_cast<int>(GenerationStep::Decoration::UNDERGROUND_ORES);
    int underground = static_cast<int>(GenerationStep::Decoration::UNDERGROUND_DECORATION);
    addFeature(biomeKey, ores, HushPlacements::ORE_ECHO);
    addFeature(biomeKey, ores, HushPlacements::ORE_RESONITE);
    addFeature(biomeKey, ores, HushPlacements::ORE_RESONITE_CAVERNS);
    addFeature(biomeKey, underground, HushPlacements::RESONANT_CLUSTERS);
    addFeature(biomeKey, underground, HushPlacements::RESONANT_CRYSTAL_CLUMPS);
}

void BiomeFeatureRegistry::setupSunkenChoir(const std::string& biomeKey) {
    // The warm_ocean / lukewarm_ocean shape at the Hush's scale: the ruins
    // take the LOCAL_MODIFICATIONS slot (vanilla's ocean ruins are a
    // structure; these are the small broken fragments), kelp and pickles the
    // VEGETAL one.
    int local = static_cast<int>(GenerationStep::Decoration::LOCAL_MODIFICATIONS);
    int ores = static_cast<int>(GenerationStep::Decoration::UNDERGROUND_ORES);
    int vegetal = static_cast<int>(GenerationStep::Decoration::VEGETAL_DECORATION);
    addFeature(biomeKey, local, HushPlacements::SUNKEN_RUINS);
    addFeature(biomeKey, ores, HushPlacements::ORE_ECHO);
    addFeature(biomeKey, ores, HushPlacements::ORE_RESONITE);
    addFeature(biomeKey, vegetal, HushPlacements::KELP_SUNKEN_CHOIR);
    addFeature(biomeKey, vegetal, HushPlacements::SEA_PICKLE_SUNKEN_CHOIR);
    addFeature(biomeKey, vegetal, HushPlacements::PATCH_HUSH_GRASS_SHORE);
}

void BiomeFeatureRegistry::setupHollowDeep(const std::string& biomeKey) {
    int local = static_cast<int>(GenerationStep::Decoration::LOCAL_MODIFICATIONS);
    int ores = static_cast<int>(GenerationStep::Decoration::UNDERGROUND_ORES);
    int underground = static_cast<int>(GenerationStep::Decoration::UNDERGROUND_DECORATION);
    int vegetal = static_cast<int>(GenerationStep::Decoration::VEGETAL_DECORATION);
    addFeature(biomeKey, local, HushPlacements::ROPE_BRIDGES);
    addFeature(biomeKey, ores, HushPlacements::ORE_ECHO);
    addFeature(biomeKey, ores, HushPlacements::ORE_RESONITE);
    addFeature(biomeKey, underground, HushPlacements::RESONANT_STALACTITES);
    addFeature(biomeKey, vegetal, HushPlacements::CRYSTAL_FORMATIONS_RIM);
    addFeature(biomeKey, vegetal, HushPlacements::PATCH_HUSH_GRASS_RIM);
}

void BiomeFeatureRegistry::setupAuroraSteppe(const std::string& biomeKey) {
    int local = static_cast<int>(GenerationStep::Decoration::LOCAL_MODIFICATIONS);
    int ores = static_cast<int>(GenerationStep::Decoration::UNDERGROUND_ORES);
    int vegetal = static_cast<int>(GenerationStep::Decoration::VEGETAL_DECORATION);
    addFeature(biomeKey, local, HushPlacements::HUSHSTONE_BOULDERS);
    addFeature(biomeKey, ores, HushPlacements::ORE_ECHO);
    addFeature(biomeKey, ores, HushPlacements::ORE_RESONITE);
    addFeature(biomeKey, vegetal, HushPlacements::CRYSTAL_FORMATIONS_STEPPE);
    addFeature(biomeKey, vegetal, HushPlacements::PATCH_HUSH_GRASS_STEPPE);
    addFeature(biomeKey, vegetal, HushPlacements::PATCH_RESONANCE_BLOOM_STEPPE);
}

// =============================================================================
// The Aether biomes - data/aether/worldgen/biome/skyroot_*.json "features".
// All four share one list; only the vegetal tree placement differs
// (skyroot_{meadow,forest,grove,woodland}_trees). Per step, in JSON order:
//   RAW_GENERATION          quicksoil_shelf
//   LAKES                   water_lake
//   UNDERGROUND_ORES        aether_dirt_ore, icestone_ore, ambrosium_ore,
//                           zanite_ore, gravitite_ore_buried, gravitite_ore
//   FLUID_SPRINGS           water_spring
//   VEGETAL_DECORATION      <biome>_trees, holiday_tree, grass_patch,
//                           tall_grass_patch, white_flower_patch,
//                           purple_flower_patch, berry_bush_patch
//   TOP_LAYER_MODIFICATION  crystal_island, cold_aercloud,
//                           blue_aercloud, golden_aercloud
// Every entry is present, so the FeatureSorter indices (and the feature
// seeds) match the mod's.
// =============================================================================
void BiomeFeatureRegistry::setupAetherBiome(const std::string& biomeKey,
                                            const PlacedFeature* trees) {
    const int raw = static_cast<int>(GenerationStep::Decoration::RAW_GENERATION);
    const int lakes = static_cast<int>(GenerationStep::Decoration::LAKES);
    const int ores = static_cast<int>(GenerationStep::Decoration::UNDERGROUND_ORES);
    const int springs = static_cast<int>(GenerationStep::Decoration::FLUID_SPRINGS);
    const int vegetal = static_cast<int>(GenerationStep::Decoration::VEGETAL_DECORATION);
    const int topLayer = static_cast<int>(GenerationStep::Decoration::TOP_LAYER_MODIFICATION);

    addFeature(biomeKey, raw, AetherPlacements::QUICKSOIL_SHELF);

    addFeature(biomeKey, lakes, AetherPlacements::WATER_LAKE);

    addFeature(biomeKey, ores, AetherPlacements::AETHER_DIRT_ORE);
    addFeature(biomeKey, ores, AetherPlacements::ICESTONE_ORE);
    addFeature(biomeKey, ores, AetherPlacements::AMBROSIUM_ORE);
    addFeature(biomeKey, ores, AetherPlacements::ZANITE_ORE);
    addFeature(biomeKey, ores, AetherPlacements::GRAVITITE_ORE_BURIED);
    addFeature(biomeKey, ores, AetherPlacements::GRAVITITE_ORE);

    addFeature(biomeKey, springs, AetherPlacements::WATER_SPRING);

    addFeature(biomeKey, vegetal, trees);
    addFeature(biomeKey, vegetal, AetherPlacements::HOLIDAY_TREE);
    addFeature(biomeKey, vegetal, AetherPlacements::GRASS_PATCH);
    addFeature(biomeKey, vegetal, AetherPlacements::TALL_GRASS_PATCH);
    addFeature(biomeKey, vegetal, AetherPlacements::WHITE_FLOWER_PATCH);
    addFeature(biomeKey, vegetal, AetherPlacements::PURPLE_FLOWER_PATCH);
    addFeature(biomeKey, vegetal, AetherPlacements::BERRY_BUSH_PATCH);

    addFeature(biomeKey, topLayer, AetherPlacements::CRYSTAL_ISLAND);
    addFeature(biomeKey, topLayer, AetherPlacements::COLD_AERCLOUD);
    addFeature(biomeKey, topLayer, AetherPlacements::BLUE_AERCLOUD);
    addFeature(biomeKey, topLayer, AetherPlacements::GOLDEN_AERCLOUD);
}

// =============================================================================
// The Twilight Forest biomes - data/twilightforest/worldgen/biome/*.json
// "features", every step in JSON order (generated from the JSON, so the
// FeatureSorter indices — and with them every feature seed — match the mod).
// TF placed features come from TwilightPlacements::get (built from the mod's
// placed_feature JSON); a feature whose configured feature could not be
// built (a block missing from the registry) is null and addFeature skips it.
// Every one of the 22 biomes gets an entry, empty or not.
// =============================================================================
void BiomeFeatureRegistry::ensureBiomeEntry(const std::string& biomeKey) {
    auto& biome = s_biomeFeatures[biomeKey];
    if (biome.empty()) {
        biome.resize(GenerationStep::DECORATION_COUNT);
    }
}

void BiomeFeatureRegistry::setupTwilightBiomes() {
    // Every placed feature by its placed_feature JSON id (TwilightPlacements
    // builds them from data/twilightforest/worldgen/placed_feature/).
    auto tf = [](const char* id) { return TwilightPlacements::get(id); };
    {
        const std::string biomeKey = "twilightforest:clearing";
        ensureBiomeEntry(biomeKey);
        addFeature(biomeKey, 1, tf("twilightforest:water_lake"));
        addFeature(biomeKey, 4, tf("twilightforest:druid_hut"));
        addFeature(biomeKey, 4, tf("twilightforest:well_placer"));
        addFeature(biomeKey, 4, tf("twilightforest:foundation"));
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_SAND);
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_CLAY);
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_GRAVEL);
        addFeature(biomeKey, 6, tf("twilightforest:wood_roots"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_coal_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_iron_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_gold_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_redstone_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_diamond_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_lapis_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_copper_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:small_andesite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_diorite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_granite"));
        addFeature(biomeKey, 7, tf("twilightforest:plant_roots"));
        addFeature(biomeKey, 7, tf("twilightforest:torch_berries"));
        addFeature(biomeKey, 7, tf("twilightforest:iron_oreberries"));
        addFeature(biomeKey, 7, tf("twilightforest:gold_oreberries"));
        addFeature(biomeKey, 7, tf("twilightforest:copper_oreberries"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_TALL_GRASS);
        addFeature(biomeKey, 9, tf("twilightforest:patch_grass_badlands"));
        addFeature(biomeKey, 9, tf("twilightforest:patch_grass_savanna"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_SUGAR_CANE);
        addFeature(biomeKey, 9, tf("twilightforest:raspberry_bushes"));
        addFeature(biomeKey, 9, tf("twilightforest:blueberry_bushes"));
        addFeature(biomeKey, 9, tf("twilightforest:patch_grass_forest"));
        addFeature(biomeKey, 9, tf("twilightforest:mayapple"));
        addFeature(biomeKey, 9, tf("twilightforest:flower_placer"));
        addFeature(biomeKey, 9, tf("twilightforest:grove_ruins"));
        addFeature(biomeKey, 9, tf("twilightforest:stone_circle"));
        addFeature(biomeKey, 9, tf("twilightforest:outside_stalagmite"));
        addFeature(biomeKey, 9, tf("twilightforest:monolith"));
        addFeature(biomeKey, 9, tf("twilightforest:hollow_stump"));
        addFeature(biomeKey, 9, tf("twilightforest:hollow_log"));
        addFeature(biomeKey, 9, VegetationPlacements::FLOWER_FOREST_FLOWERS);
        addFeature(biomeKey, 9, tf("twilightforest:default_fallen_logs"));
        addFeature(biomeKey, 10, MiscOverworldPlacements::FREEZE_TOP_LAYER);
    }
    {
        const std::string biomeKey = "twilightforest:dark_forest";
        ensureBiomeEntry(biomeKey);
        addFeature(biomeKey, 6, tf("twilightforest:legacy_coal_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_iron_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_gold_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_redstone_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_diamond_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_lapis_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_copper_ore"));
        addFeature(biomeKey, 7, tf("twilightforest:plant_roots"));
        addFeature(biomeKey, 7, tf("twilightforest:torch_berries"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_GRASS_NORMAL);
        addFeature(biomeKey, 9, tf("twilightforest:tree/selector/dark_forest_tree_mix"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/selector/dark_forest_trees"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/darkwood_tree"));
        addFeature(biomeKey, 9, tf("twilightforest:dark_grass"));
        addFeature(biomeKey, 9, tf("twilightforest:dark_dead_bushes"));
        addFeature(biomeKey, 9, tf("twilightforest:dark_pumpkins"));
        addFeature(biomeKey, 9, tf("twilightforest:dark_mushglooms"));
        addFeature(biomeKey, 9, tf("twilightforest:dark_brown_mushrooms"));
        addFeature(biomeKey, 9, tf("twilightforest:dark_red_mushrooms"));
    }
    {
        const std::string biomeKey = "twilightforest:dark_forest_center";
        ensureBiomeEntry(biomeKey);
        addFeature(biomeKey, 6, tf("twilightforest:legacy_coal_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_iron_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_gold_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_redstone_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_diamond_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_lapis_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_copper_ore"));
        addFeature(biomeKey, 7, tf("twilightforest:plant_roots"));
        addFeature(biomeKey, 7, tf("twilightforest:torch_berries"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_GRASS_NORMAL);
        addFeature(biomeKey, 9, tf("twilightforest:tree/selector/dark_forest_tree_mix"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/selector/dark_forest_trees"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/darkwood_tree"));
        addFeature(biomeKey, 9, tf("twilightforest:dark_grass"));
        addFeature(biomeKey, 9, tf("twilightforest:dark_dead_bushes"));
        addFeature(biomeKey, 9, tf("twilightforest:dark_pumpkins"));
        addFeature(biomeKey, 9, tf("twilightforest:dark_mushglooms"));
        addFeature(biomeKey, 9, tf("twilightforest:dark_brown_mushrooms"));
        addFeature(biomeKey, 9, tf("twilightforest:dark_red_mushrooms"));
    }
    {
        const std::string biomeKey = "twilightforest:dense_forest";
        ensureBiomeEntry(biomeKey);
        addFeature(biomeKey, 1, tf("twilightforest:water_lake"));
        addFeature(biomeKey, 4, tf("twilightforest:druid_hut"));
        addFeature(biomeKey, 4, tf("twilightforest:well_placer"));
        addFeature(biomeKey, 4, tf("twilightforest:foundation"));
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_SAND);
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_CLAY);
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_GRAVEL);
        addFeature(biomeKey, 6, tf("twilightforest:wood_roots"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_coal_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_iron_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_gold_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_redstone_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_diamond_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_lapis_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_copper_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:small_andesite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_diorite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_granite"));
        addFeature(biomeKey, 7, tf("twilightforest:plant_roots"));
        addFeature(biomeKey, 7, tf("twilightforest:torch_berries"));
        addFeature(biomeKey, 7, tf("twilightforest:iron_oreberries"));
        addFeature(biomeKey, 7, tf("twilightforest:gold_oreberries"));
        addFeature(biomeKey, 7, tf("twilightforest:copper_oreberries"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_TALL_GRASS);
        addFeature(biomeKey, 9, tf("twilightforest:patch_grass_badlands"));
        addFeature(biomeKey, 9, tf("twilightforest:patch_grass_savanna"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_SUGAR_CANE);
        addFeature(biomeKey, 9, tf("twilightforest:raspberry_bushes"));
        addFeature(biomeKey, 9, tf("twilightforest:blueberry_bushes"));
        addFeature(biomeKey, 9, tf("twilightforest:blackberry_bushes"));
        addFeature(biomeKey, 9, tf("twilightforest:patch_grass_jungle"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_LARGE_FERN);
        addFeature(biomeKey, 9, tf("twilightforest:mayapple"));
        addFeature(biomeKey, 9, tf("twilightforest:flower_placer_alt"));
        addFeature(biomeKey, 9, tf("twilightforest:grove_ruins"));
        addFeature(biomeKey, 9, tf("twilightforest:stone_circle"));
        addFeature(biomeKey, 9, tf("twilightforest:outside_stalagmite"));
        addFeature(biomeKey, 9, tf("twilightforest:monolith"));
        addFeature(biomeKey, 9, tf("twilightforest:hollow_stump"));
        addFeature(biomeKey, 9, tf("twilightforest:hollow_log"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/selector/dense_canopy_trees"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/selector/vanilla_trees"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/oak_bush_dense"));
        addFeature(biomeKey, 9, tf("twilightforest:default_fallen_logs"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/forest_mega_oak_tree"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/mega_canopy_tree"));
        addFeature(biomeKey, 10, MiscOverworldPlacements::FREEZE_TOP_LAYER);
    }
    {
        const std::string biomeKey = "twilightforest:dense_mushroom_forest";
        ensureBiomeEntry(biomeKey);
        addFeature(biomeKey, 1, tf("twilightforest:water_lake"));
        addFeature(biomeKey, 4, tf("twilightforest:druid_hut"));
        addFeature(biomeKey, 4, tf("twilightforest:well_placer"));
        addFeature(biomeKey, 4, tf("twilightforest:foundation"));
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_SAND);
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_CLAY);
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_GRAVEL);
        addFeature(biomeKey, 6, tf("twilightforest:wood_roots"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_coal_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_iron_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_gold_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_redstone_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_diamond_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_lapis_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_copper_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:small_andesite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_diorite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_granite"));
        addFeature(biomeKey, 7, tf("twilightforest:plant_roots"));
        addFeature(biomeKey, 7, tf("twilightforest:torch_berries"));
        addFeature(biomeKey, 7, tf("twilightforest:iron_oreberries"));
        addFeature(biomeKey, 7, tf("twilightforest:gold_oreberries"));
        addFeature(biomeKey, 7, tf("twilightforest:copper_oreberries"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_TALL_GRASS);
        addFeature(biomeKey, 9, tf("twilightforest:patch_grass_badlands"));
        addFeature(biomeKey, 9, tf("twilightforest:patch_grass_savanna"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_SUGAR_CANE);
        addFeature(biomeKey, 9, tf("twilightforest:blackberry_bushes"));
        addFeature(biomeKey, 9, tf("twilightforest:patch_grass_jungle"));
        addFeature(biomeKey, 9, tf("twilightforest:mayapple"));
        addFeature(biomeKey, 9, tf("twilightforest:flower_placer_alt"));
        addFeature(biomeKey, 9, tf("twilightforest:grove_ruins"));
        addFeature(biomeKey, 9, tf("twilightforest:stone_circle"));
        addFeature(biomeKey, 9, tf("twilightforest:outside_stalagmite"));
        addFeature(biomeKey, 9, tf("twilightforest:monolith"));
        addFeature(biomeKey, 9, tf("twilightforest:hollow_stump"));
        addFeature(biomeKey, 9, tf("twilightforest:hollow_log"));
        addFeature(biomeKey, 9, tf("twilightforest:mycelium_blob"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/selector/vanilla_trees"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/twilight_oak_tree"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/large_twilight_oak_tree"));
        addFeature(biomeKey, 9, VegetationPlacements::BROWN_MUSHROOM_NORMAL);
        addFeature(biomeKey, 9, VegetationPlacements::RED_MUSHROOM_NORMAL);
        addFeature(biomeKey, 9, VegetationPlacements::BROWN_MUSHROOM_TAIGA);
        addFeature(biomeKey, 9, VegetationPlacements::RED_MUSHROOM_TAIGA);
        addFeature(biomeKey, 9, tf("twilightforest:tree/selector/vanilla_mushrooms"));
        addFeature(biomeKey, 9, tf("twilightforest:mushroom/canopy_mushrooms_dense"));
        addFeature(biomeKey, 9, tf("twilightforest:mushgloom_cluster"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/selector/canopy_trees"));
        addFeature(biomeKey, 9, tf("twilightforest:default_fallen_logs"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/mega_canopy_tree"));
        addFeature(biomeKey, 10, MiscOverworldPlacements::FREEZE_TOP_LAYER);
    }
    {
        const std::string biomeKey = "twilightforest:enchanted_forest";
        ensureBiomeEntry(biomeKey);
        addFeature(biomeKey, 1, tf("twilightforest:dense_water_lake"));
        addFeature(biomeKey, 4, tf("twilightforest:druid_hut"));
        addFeature(biomeKey, 4, tf("twilightforest:well_placer"));
        addFeature(biomeKey, 4, tf("twilightforest:foundation"));
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_SAND);
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_CLAY);
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_GRAVEL);
        addFeature(biomeKey, 6, tf("twilightforest:wood_roots"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_coal_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_iron_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_gold_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_redstone_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_diamond_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_lapis_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_copper_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:small_andesite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_diorite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_granite"));
        addFeature(biomeKey, 7, tf("twilightforest:plant_roots"));
        addFeature(biomeKey, 7, tf("twilightforest:torch_berries"));
        addFeature(biomeKey, 7, tf("twilightforest:iron_oreberries_enchanted_forest"));
        addFeature(biomeKey, 7, tf("twilightforest:gold_oreberries_enchanted_forest"));
        addFeature(biomeKey, 7, tf("twilightforest:copper_oreberries_enchanted_forest"));
        addFeature(biomeKey, 7, tf("twilightforest:essence_oreberries_enchanted_forest"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_GRASS_BADLANDS);
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_SUGAR_CANE);
        addFeature(biomeKey, 9, tf("twilightforest:dense_ferns"));
        addFeature(biomeKey, 9, tf("twilightforest:dense_large_ferns"));
        addFeature(biomeKey, 9, tf("twilightforest:flower_placer"));
        addFeature(biomeKey, 9, tf("twilightforest:grove_ruins"));
        addFeature(biomeKey, 9, tf("twilightforest:stone_circle"));
        addFeature(biomeKey, 9, tf("twilightforest:outside_stalagmite"));
        addFeature(biomeKey, 9, tf("twilightforest:monolith"));
        addFeature(biomeKey, 9, tf("twilightforest:hollow_stump"));
        addFeature(biomeKey, 9, tf("twilightforest:hollow_log"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/selector/enchanted_forest_trees"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/selector/dense_canopy_trees"));
        addFeature(biomeKey, 9, tf("twilightforest:fiddlehead"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/selector/canopy_trees"));
        addFeature(biomeKey, 9, tf("twilightforest:default_fallen_logs"));
        addFeature(biomeKey, 10, MiscOverworldPlacements::FREEZE_TOP_LAYER);
        addFeature(biomeKey, 10, tf("twilightforest:enchanted_forest_vines"));
    }
    {
        const std::string biomeKey = "twilightforest:final_plateau";
        ensureBiomeEntry(biomeKey);
    }
    {
        const std::string biomeKey = "twilightforest:fire_swamp";
        ensureBiomeEntry(biomeKey);
        addFeature(biomeKey, 1, tf("twilightforest:lava_lake"));
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_SAND);
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_CLAY);
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_GRAVEL);
        addFeature(biomeKey, 6, tf("twilightforest:wood_roots"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_coal_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_iron_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_gold_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_redstone_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_diamond_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_lapis_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_copper_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:small_andesite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_diorite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_granite"));
        addFeature(biomeKey, 7, tf("twilightforest:plant_roots"));
        addFeature(biomeKey, 7, tf("twilightforest:torch_berries"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_TALL_GRASS);
        addFeature(biomeKey, 9, tf("twilightforest:patch_grass_badlands"));
        addFeature(biomeKey, 9, tf("twilightforest:patch_grass_savanna"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_SUGAR_CANE);
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_GRASS_TAIGA_2);
        addFeature(biomeKey, 9, tf("twilightforest:stone_circle"));
        addFeature(biomeKey, 9, tf("twilightforest:outside_stalagmite"));
        addFeature(biomeKey, 9, tf("twilightforest:monolith"));
        addFeature(biomeKey, 9, tf("twilightforest:hollow_stump"));
        addFeature(biomeKey, 9, tf("twilightforest:hollow_log"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/swampy_oak_tree"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/oak_bush"));
        addFeature(biomeKey, 9, tf("twilightforest:fire_jet"));
        addFeature(biomeKey, 9, tf("twilightforest:smoker"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_SUGAR_CANE_SWAMP);
        addFeature(biomeKey, 9, VegetationPlacements::VINES);
        addFeature(biomeKey, 9, VegetationPlacements::BROWN_MUSHROOM_SWAMP);
        addFeature(biomeKey, 10, MiscOverworldPlacements::FREEZE_TOP_LAYER);
    }
    {
        const std::string biomeKey = "twilightforest:firefly_forest";
        ensureBiomeEntry(biomeKey);
        addFeature(biomeKey, 1, tf("twilightforest:water_lake"));
        addFeature(biomeKey, 4, tf("twilightforest:druid_hut"));
        addFeature(biomeKey, 4, tf("twilightforest:well_placer"));
        addFeature(biomeKey, 4, tf("twilightforest:foundation"));
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_SAND);
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_CLAY);
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_GRAVEL);
        addFeature(biomeKey, 6, tf("twilightforest:wood_roots"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_coal_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_iron_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_gold_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_redstone_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_diamond_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_lapis_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_copper_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:small_andesite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_diorite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_granite"));
        addFeature(biomeKey, 7, tf("twilightforest:plant_roots"));
        addFeature(biomeKey, 7, tf("twilightforest:torch_berries"));
        addFeature(biomeKey, 7, tf("twilightforest:iron_oreberries"));
        addFeature(biomeKey, 7, tf("twilightforest:gold_oreberries"));
        addFeature(biomeKey, 7, tf("twilightforest:copper_oreberries"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_TALL_GRASS);
        addFeature(biomeKey, 9, tf("twilightforest:patch_grass_badlands"));
        addFeature(biomeKey, 9, tf("twilightforest:patch_grass_savanna"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_SUGAR_CANE);
        addFeature(biomeKey, 9, tf("twilightforest:blackberry_bushes"));
        addFeature(biomeKey, 9, tf("twilightforest:patch_grass_jungle"));
        addFeature(biomeKey, 9, tf("twilightforest:mayapple"));
        addFeature(biomeKey, 9, tf("twilightforest:flower_placer_alt"));
        addFeature(biomeKey, 9, tf("twilightforest:grove_ruins"));
        addFeature(biomeKey, 9, tf("twilightforest:stone_circle"));
        addFeature(biomeKey, 9, tf("twilightforest:outside_stalagmite"));
        addFeature(biomeKey, 9, tf("twilightforest:monolith"));
        addFeature(biomeKey, 9, tf("twilightforest:hollow_stump"));
        addFeature(biomeKey, 9, tf("twilightforest:hollow_log"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/selector/firefly_forest_trees"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/selector/vanilla_trees"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/twilight_oak_tree"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/large_twilight_oak_tree"));
        addFeature(biomeKey, 9, tf("twilightforest:lamppost_placer"));
        addFeature(biomeKey, 9, tf("twilightforest:mushgloom_cluster"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_PUMPKIN);
        addFeature(biomeKey, 9, VegetationPlacements::FLOWER_FOREST_FLOWERS);
        addFeature(biomeKey, 9, tf("twilightforest:default_fallen_logs"));
        addFeature(biomeKey, 10, MiscOverworldPlacements::FREEZE_TOP_LAYER);
    }
    {
        const std::string biomeKey = "twilightforest:forest";
        ensureBiomeEntry(biomeKey);
        addFeature(biomeKey, 1, tf("twilightforest:water_lake"));
        addFeature(biomeKey, 4, tf("twilightforest:druid_hut"));
        addFeature(biomeKey, 4, tf("twilightforest:well_placer"));
        addFeature(biomeKey, 4, tf("twilightforest:foundation"));
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_SAND);
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_CLAY);
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_GRAVEL);
        addFeature(biomeKey, 6, tf("twilightforest:wood_roots"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_coal_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_iron_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_gold_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_redstone_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_diamond_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_lapis_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_copper_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:small_andesite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_diorite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_granite"));
        addFeature(biomeKey, 7, tf("twilightforest:plant_roots"));
        addFeature(biomeKey, 7, tf("twilightforest:torch_berries"));
        addFeature(biomeKey, 7, tf("twilightforest:iron_oreberries"));
        addFeature(biomeKey, 7, tf("twilightforest:gold_oreberries"));
        addFeature(biomeKey, 7, tf("twilightforest:copper_oreberries"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_TALL_GRASS);
        addFeature(biomeKey, 9, tf("twilightforest:patch_grass_badlands"));
        addFeature(biomeKey, 9, tf("twilightforest:patch_grass_savanna"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_SUGAR_CANE);
        addFeature(biomeKey, 9, tf("twilightforest:blueberry_bushes"));
        addFeature(biomeKey, 9, tf("twilightforest:patch_grass_jungle"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_LARGE_FERN);
        addFeature(biomeKey, 9, tf("twilightforest:mayapple"));
        addFeature(biomeKey, 9, tf("twilightforest:flower_placer_alt"));
        addFeature(biomeKey, 9, tf("twilightforest:grove_ruins"));
        addFeature(biomeKey, 9, tf("twilightforest:stone_circle"));
        addFeature(biomeKey, 9, tf("twilightforest:outside_stalagmite"));
        addFeature(biomeKey, 9, tf("twilightforest:monolith"));
        addFeature(biomeKey, 9, tf("twilightforest:hollow_stump"));
        addFeature(biomeKey, 9, tf("twilightforest:hollow_log"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/selector/vanilla_trees"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/twilight_oak_tree"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/large_twilight_oak_tree"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/selector/canopy_trees"));
        addFeature(biomeKey, 9, tf("twilightforest:default_fallen_logs"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/mega_canopy_tree"));
        addFeature(biomeKey, 10, MiscOverworldPlacements::FREEZE_TOP_LAYER);
    }
    {
        const std::string biomeKey = "twilightforest:glacier";
        ensureBiomeEntry(biomeKey);
        addFeature(biomeKey, 6, tf("twilightforest:legacy_coal_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_iron_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_gold_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_redstone_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_diamond_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_lapis_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_copper_ore"));
        addFeature(biomeKey, 7, tf("twilightforest:plant_roots"));
        addFeature(biomeKey, 7, tf("twilightforest:torch_berries"));
        addFeature(biomeKey, 10, MiscOverworldPlacements::FREEZE_TOP_LAYER);
    }
    {
        const std::string biomeKey = "twilightforest:highlands";
        ensureBiomeEntry(biomeKey);
        addFeature(biomeKey, 4, tf("twilightforest:druid_hut"));
        addFeature(biomeKey, 4, tf("twilightforest:well_placer"));
        addFeature(biomeKey, 4, tf("twilightforest:foundation"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_coal_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_iron_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_gold_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_redstone_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_diamond_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_lapis_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_copper_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:small_andesite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_diorite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_granite"));
        addFeature(biomeKey, 7, tf("twilightforest:troll_roots"));
        addFeature(biomeKey, 9, tf("twilightforest:blueberry_bushes"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_GRASS_TAIGA);
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_LARGE_FERN);
        addFeature(biomeKey, 9, tf("twilightforest:grove_ruins"));
        addFeature(biomeKey, 9, tf("twilightforest:stone_circle"));
        addFeature(biomeKey, 9, tf("twilightforest:outside_stalagmite"));
        addFeature(biomeKey, 9, tf("twilightforest:monolith"));
        addFeature(biomeKey, 9, tf("twilightforest:hollow_stump"));
        addFeature(biomeKey, 9, tf("twilightforest:hollow_log"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/selector/highlands_trees"));
        addFeature(biomeKey, 9, MiscOverworldPlacements::FOREST_ROCK);
        addFeature(biomeKey, 9, tf("twilightforest:sparse_mushglooms"));
        addFeature(biomeKey, 9, tf("twilightforest:spruce_fallen_log"));
        addFeature(biomeKey, 9, tf("twilightforest:dark_ferns"));
        addFeature(biomeKey, 9, tf("twilightforest:troll_mushglooms"));
    }
    {
        const std::string biomeKey = "twilightforest:highlands_underground";
        ensureBiomeEntry(biomeKey);
        addFeature(biomeKey, 6, tf("twilightforest:legacy_coal_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_iron_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_gold_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_redstone_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_diamond_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_lapis_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_copper_ore"));
        addFeature(biomeKey, 7, tf("twilightforest:troll_roots"));
        addFeature(biomeKey, 9, tf("twilightforest:troll_mushglooms"));
    }
    {
        const std::string biomeKey = "twilightforest:lake";
        ensureBiomeEntry(biomeKey);
        addFeature(biomeKey, 6, tf("twilightforest:legacy_coal_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_iron_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_gold_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_redstone_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_diamond_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_lapis_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_copper_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:small_andesite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_diorite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_granite"));
        addFeature(biomeKey, 7, tf("twilightforest:iron_oreberries"));
        addFeature(biomeKey, 7, tf("twilightforest:gold_oreberries"));
        addFeature(biomeKey, 7, tf("twilightforest:copper_oreberries"));
        addFeature(biomeKey, 9, AquaticPlacements::SEAGRASS_DEEP);
        addFeature(biomeKey, 10, MiscOverworldPlacements::FREEZE_TOP_LAYER);
    }
    {
        const std::string biomeKey = "twilightforest:mushroom_forest";
        ensureBiomeEntry(biomeKey);
        addFeature(biomeKey, 1, tf("twilightforest:water_lake"));
        addFeature(biomeKey, 4, tf("twilightforest:druid_hut"));
        addFeature(biomeKey, 4, tf("twilightforest:well_placer"));
        addFeature(biomeKey, 4, tf("twilightforest:foundation"));
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_SAND);
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_CLAY);
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_GRAVEL);
        addFeature(biomeKey, 6, tf("twilightforest:wood_roots"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_coal_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_iron_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_gold_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_redstone_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_diamond_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_lapis_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_copper_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:small_andesite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_diorite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_granite"));
        addFeature(biomeKey, 7, tf("twilightforest:plant_roots"));
        addFeature(biomeKey, 7, tf("twilightforest:torch_berries"));
        addFeature(biomeKey, 7, tf("twilightforest:iron_oreberries"));
        addFeature(biomeKey, 7, tf("twilightforest:gold_oreberries"));
        addFeature(biomeKey, 7, tf("twilightforest:copper_oreberries"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_TALL_GRASS);
        addFeature(biomeKey, 9, tf("twilightforest:patch_grass_badlands"));
        addFeature(biomeKey, 9, tf("twilightforest:patch_grass_savanna"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_SUGAR_CANE);
        addFeature(biomeKey, 9, tf("twilightforest:raspberry_bushes"));
        addFeature(biomeKey, 9, tf("twilightforest:blueberry_bushes"));
        addFeature(biomeKey, 9, tf("twilightforest:blackberry_bushes"));
        addFeature(biomeKey, 9, tf("twilightforest:patch_grass_jungle"));
        addFeature(biomeKey, 9, tf("twilightforest:mayapple"));
        addFeature(biomeKey, 9, tf("twilightforest:flower_placer_alt"));
        addFeature(biomeKey, 9, tf("twilightforest:grove_ruins"));
        addFeature(biomeKey, 9, tf("twilightforest:stone_circle"));
        addFeature(biomeKey, 9, tf("twilightforest:outside_stalagmite"));
        addFeature(biomeKey, 9, tf("twilightforest:monolith"));
        addFeature(biomeKey, 9, tf("twilightforest:hollow_stump"));
        addFeature(biomeKey, 9, tf("twilightforest:hollow_log"));
        addFeature(biomeKey, 9, tf("twilightforest:mycelium_blob"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/selector/vanilla_trees"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/twilight_oak_tree"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/large_twilight_oak_tree"));
        addFeature(biomeKey, 9, VegetationPlacements::BROWN_MUSHROOM_NORMAL);
        addFeature(biomeKey, 9, VegetationPlacements::RED_MUSHROOM_NORMAL);
        addFeature(biomeKey, 9, VegetationPlacements::BROWN_MUSHROOM_TAIGA);
        addFeature(biomeKey, 9, VegetationPlacements::RED_MUSHROOM_TAIGA);
        addFeature(biomeKey, 9, tf("twilightforest:tree/selector/vanilla_mushrooms"));
        addFeature(biomeKey, 9, tf("twilightforest:mushroom/canopy_mushrooms_sparse"));
        addFeature(biomeKey, 9, tf("twilightforest:mushgloom_cluster"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/selector/canopy_trees"));
        addFeature(biomeKey, 9, tf("twilightforest:default_fallen_logs"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/mega_canopy_tree"));
        addFeature(biomeKey, 10, MiscOverworldPlacements::FREEZE_TOP_LAYER);
    }
    {
        const std::string biomeKey = "twilightforest:oak_savannah";
        ensureBiomeEntry(biomeKey);
        addFeature(biomeKey, 1, tf("twilightforest:water_lake"));
        addFeature(biomeKey, 4, tf("twilightforest:druid_hut"));
        addFeature(biomeKey, 4, tf("twilightforest:well_placer"));
        addFeature(biomeKey, 4, tf("twilightforest:foundation"));
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_SAND);
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_CLAY);
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_GRAVEL);
        addFeature(biomeKey, 6, tf("twilightforest:wood_roots"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_coal_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_iron_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_gold_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_redstone_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_diamond_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_lapis_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_copper_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:small_andesite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_diorite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_granite"));
        addFeature(biomeKey, 7, tf("twilightforest:plant_roots"));
        addFeature(biomeKey, 7, tf("twilightforest:torch_berries"));
        addFeature(biomeKey, 7, tf("twilightforest:iron_oreberries"));
        addFeature(biomeKey, 7, tf("twilightforest:gold_oreberries"));
        addFeature(biomeKey, 7, tf("twilightforest:copper_oreberries"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_TALL_GRASS);
        addFeature(biomeKey, 9, tf("twilightforest:patch_grass_badlands"));
        addFeature(biomeKey, 9, tf("twilightforest:patch_grass_savanna"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_SUGAR_CANE);
        addFeature(biomeKey, 9, tf("twilightforest:patch_grass_taiga_2"));
        addFeature(biomeKey, 9, tf("twilightforest:mayapple"));
        addFeature(biomeKey, 9, tf("twilightforest:flower_placer"));
        addFeature(biomeKey, 9, tf("twilightforest:grove_ruins"));
        addFeature(biomeKey, 9, tf("twilightforest:stone_circle"));
        addFeature(biomeKey, 9, tf("twilightforest:outside_stalagmite"));
        addFeature(biomeKey, 9, tf("twilightforest:monolith"));
        addFeature(biomeKey, 9, tf("twilightforest:hollow_stump"));
        addFeature(biomeKey, 9, tf("twilightforest:hollow_log"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/savannah_oak_tree"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/large_twilight_oak_tree"));
        addFeature(biomeKey, 9, tf("twilightforest:default_fallen_logs"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/savannah_mega_oak_tree"));
        addFeature(biomeKey, 10, MiscOverworldPlacements::FREEZE_TOP_LAYER);
    }
    {
        const std::string biomeKey = "twilightforest:snowy_forest";
        ensureBiomeEntry(biomeKey);
        addFeature(biomeKey, 1, tf("twilightforest:frozen_lake"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_coal_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_iron_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_gold_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_redstone_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_diamond_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_lapis_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_copper_ore"));
        addFeature(biomeKey, 7, tf("twilightforest:plant_roots"));
        addFeature(biomeKey, 7, tf("twilightforest:torch_berries"));
        addFeature(biomeKey, 9, tf("twilightforest:snowy_blueberry_bushes"));
        addFeature(biomeKey, 9, tf("twilightforest:maloberry_bushes"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/selector/snowy_forest_trees"));
        addFeature(biomeKey, 9, tf("twilightforest:spruce_fallen_log"));
        addFeature(biomeKey, 10, tf("twilightforest:snow_under_trees"));
        addFeature(biomeKey, 10, MiscOverworldPlacements::FREEZE_TOP_LAYER);
    }
    {
        const std::string biomeKey = "twilightforest:spooky_forest";
        ensureBiomeEntry(biomeKey);
        addFeature(biomeKey, 4, tf("twilightforest:graveyard"));
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_SAND);
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_CLAY);
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_GRAVEL);
        addFeature(biomeKey, 6, tf("twilightforest:wood_roots"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_coal_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_iron_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_gold_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_redstone_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_diamond_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_lapis_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_copper_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:small_andesite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_diorite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_granite"));
        addFeature(biomeKey, 7, tf("twilightforest:plant_roots"));
        addFeature(biomeKey, 7, tf("twilightforest:torch_berries"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_TALL_GRASS);
        addFeature(biomeKey, 9, tf("twilightforest:patch_grass_badlands"));
        addFeature(biomeKey, 9, tf("twilightforest:patch_grass_savanna"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_SUGAR_CANE);
        addFeature(biomeKey, 9, tf("twilightforest:patch_grass_jungle"));
        addFeature(biomeKey, 9, tf("twilightforest:mayapple"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/twilight_oak_tree"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/large_twilight_oak_tree"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/dead_canopy_tree"));
        addFeature(biomeKey, 9, tf("twilightforest:pumpkin_lamppost"));
        addFeature(biomeKey, 9, tf("twilightforest:tf_oak_fallen_log"));
        addFeature(biomeKey, 9, tf("twilightforest:canopy_fallen_log"));
        addFeature(biomeKey, 9, tf("twilightforest:webs"));
        addFeature(biomeKey, 9, tf("twilightforest:fallen_leaves"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_PUMPKIN);
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_DEAD_BUSH);
        addFeature(biomeKey, 10, MiscOverworldPlacements::FREEZE_TOP_LAYER);
    }
    {
        const std::string biomeKey = "twilightforest:stream";
        ensureBiomeEntry(biomeKey);
        addFeature(biomeKey, 6, tf("twilightforest:legacy_coal_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_iron_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_gold_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_redstone_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_diamond_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_lapis_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_copper_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:small_andesite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_diorite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_granite"));
        addFeature(biomeKey, 7, tf("twilightforest:iron_oreberries"));
        addFeature(biomeKey, 7, tf("twilightforest:gold_oreberries"));
        addFeature(biomeKey, 7, tf("twilightforest:copper_oreberries"));
        addFeature(biomeKey, 9, AquaticPlacements::SEAGRASS_NORMAL);
        addFeature(biomeKey, 10, MiscOverworldPlacements::FREEZE_TOP_LAYER);
    }
    {
        const std::string biomeKey = "twilightforest:swamp";
        ensureBiomeEntry(biomeKey);
        addFeature(biomeKey, 4, tf("twilightforest:druid_hut"));
        addFeature(biomeKey, 4, tf("twilightforest:well_placer"));
        addFeature(biomeKey, 4, tf("twilightforest:foundation"));
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_SAND);
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_CLAY);
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_GRAVEL);
        addFeature(biomeKey, 6, tf("twilightforest:wood_roots"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_coal_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_iron_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_gold_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_redstone_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_diamond_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_lapis_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_copper_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:small_andesite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_diorite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_granite"));
        addFeature(biomeKey, 7, tf("twilightforest:plant_roots"));
        addFeature(biomeKey, 7, tf("twilightforest:torch_berries"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_TALL_GRASS);
        addFeature(biomeKey, 9, tf("twilightforest:patch_grass_badlands"));
        addFeature(biomeKey, 9, tf("twilightforest:patch_grass_savanna"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_SUGAR_CANE);
        addFeature(biomeKey, 9, tf("twilightforest:swamp_raspberry_bushes"));
        addFeature(biomeKey, 9, tf("twilightforest:swamp_blackberry_bushes"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_GRASS_TAIGA_2);
        addFeature(biomeKey, 9, tf("twilightforest:grove_ruins"));
        addFeature(biomeKey, 9, tf("twilightforest:stone_circle"));
        addFeature(biomeKey, 9, tf("twilightforest:outside_stalagmite"));
        addFeature(biomeKey, 9, tf("twilightforest:monolith"));
        addFeature(biomeKey, 9, tf("twilightforest:hollow_stump"));
        addFeature(biomeKey, 9, tf("twilightforest:hollow_log"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/mangrove_tree"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/swampy_oak_tree"));
        addFeature(biomeKey, 9, tf("twilightforest:tree/oak_bush"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_SUGAR_CANE_SWAMP);
        addFeature(biomeKey, 9, VegetationPlacements::VINES);
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_DEAD_BUSH);
        addFeature(biomeKey, 9, tf("twilightforest:mangrove_fallen_log"));
        addFeature(biomeKey, 9, tf("twilightforest:huge_lily_pad"));
        addFeature(biomeKey, 9, tf("twilightforest:huge_water_lily"));
        addFeature(biomeKey, 9, VegetationPlacements::PATCH_WATERLILY);
        addFeature(biomeKey, 10, MiscOverworldPlacements::FREEZE_TOP_LAYER);
    }
    {
        const std::string biomeKey = "twilightforest:thornlands";
        ensureBiomeEntry(biomeKey);
        addFeature(biomeKey, 9, tf("twilightforest:stone_circle"));
        addFeature(biomeKey, 9, tf("twilightforest:outside_stalagmite"));
        addFeature(biomeKey, 9, tf("twilightforest:monolith"));
        addFeature(biomeKey, 9, tf("twilightforest:hollow_stump"));
        addFeature(biomeKey, 9, tf("twilightforest:hollow_log"));
        addFeature(biomeKey, 9, tf("twilightforest:thorns"));
    }
    {
        const std::string biomeKey = "twilightforest:underground";
        ensureBiomeEntry(biomeKey);
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_SAND);
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_CLAY);
        addFeature(biomeKey, 6, MiscOverworldPlacements::DISK_GRAVEL);
        addFeature(biomeKey, 6, tf("twilightforest:wood_roots"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_coal_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_iron_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_gold_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_redstone_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_diamond_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_lapis_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:legacy_copper_ore"));
        addFeature(biomeKey, 6, tf("twilightforest:small_andesite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_diorite"));
        addFeature(biomeKey, 6, tf("twilightforest:small_granite"));
        addFeature(biomeKey, 7, tf("twilightforest:plant_roots"));
        addFeature(biomeKey, 7, tf("twilightforest:torch_berries"));
        addFeature(biomeKey, 10, MiscOverworldPlacements::FREEZE_TOP_LAYER);
    }
}

void BiomeFeatureRegistry::bootstrap() {
    std::call_once(s_bootstrapOnce, []() {
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
        features::NetherFeatures::bootstrap();
        placement::NetherPlacements::bootstrap();
        features::EndFeatures::bootstrap();
        placement::EndPlacements::bootstrap();
        features::HushFeatures::bootstrap();
        placement::HushPlacements::bootstrap();
        features::AetherFeatures::bootstrap();
        placement::AetherPlacements::bootstrap();
        features::TwilightFeatures::bootstrap();
        placement::TwilightPlacements::bootstrap();

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
            "minecraft:dappled_forest",           // 42 (26.3)
            "minecraft:old_growth_pine_taiga",    // 43
            "minecraft:sunflower_plains",         // 44
            "minecraft:old_growth_birch_forest",  // 45
            "minecraft:sparse_jungle",            // 46
            "minecraft:bamboo_jungle",            // 47
            "minecraft:eroded_badlands",          // 48
            "minecraft:windswept_savanna",        // 49
            "minecraft:cherry_grove",             // 50
            "minecraft:frozen_peaks",             // 51
            "minecraft:dripstone_caves",          // 52
            "minecraft:lush_caves",               // 53
            "minecraft:sulfur_caves",             // 54 (26.3)
            "minecraft:deep_dark"                 // 55
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

        // 42: dappled_forest (26.3)
        setupDappledForest("minecraft:dappled_forest");

        // 43: old_growth_pine_taiga
        setupOldGrowthTaiga("minecraft:old_growth_pine_taiga", false);

        // 44: sunflower_plains
        setupPlains("minecraft:sunflower_plains", true, false, false);

        // 45: old_growth_birch_forest
        setupForest("minecraft:old_growth_birch_forest", true, true, false);

        // 46: sparse_jungle
        setupJungle("minecraft:sparse_jungle", false, true, false);

        // 47: bamboo_jungle
        setupJungle("minecraft:bamboo_jungle", true, false, true);

        // 48: eroded_badlands (same as regular badlands)
        setupBadlands("minecraft:eroded_badlands", false);

        // 49: windswept_savanna
        setupSavanna("minecraft:windswept_savanna", true, false);

        // 50: cherry_grove
        setupMeadowOrCherryGrove("minecraft:cherry_grove", true);

        // 51: frozen_peaks
        setupPeaks("minecraft:frozen_peaks", false);

        // 52: dripstone_caves
        setupDripstoneCaves("minecraft:dripstone_caves");

        // 53: lush_caves
        setupLushCaves("minecraft:lush_caves");

        // 54: sulfur_caves (26.3)
        setupSulfurCaves("minecraft:sulfur_caves");

        // 55: deep_dark
        setupDeepDark("minecraft:deep_dark");

        // Nether biomes - registered in the map but NOT in s_biomeKeyOrder
        // (the overworld featuresPerStep must not see them; the nether
        // generator builds its own from getNetherBiomeKeys()).
        setupNetherWastes("minecraft:nether_wastes");
        setupSoulSandValley("minecraft:soul_sand_valley");
        setupCrimsonForest("minecraft:crimson_forest");
        setupWarpedForest("minecraft:warped_forest");
        setupBasaltDeltas("minecraft:basalt_deltas");

        // End biomes (also NOT in s_biomeKeyOrder)
        setupTheEnd("minecraft:the_end");
        setupEndHighlands("minecraft:end_highlands");
        setupEndBarrensOrMidlands("minecraft:end_midlands");
        setupSmallEndIslands("minecraft:small_end_islands");
        setupEndBarrensOrMidlands("minecraft:end_barrens");

        // The Hush biomes (engine-only; also NOT in s_biomeKeyOrder — the
        // Hush generator builds its featuresPerStep from getHushBiomeKeys())
        setupHushMeadows("minecraft:hush_meadows");
        setupWhisperwoodForest("minecraft:whisperwood_forest");
        setupResonantBarrens("minecraft:resonant_barrens");
        setupCrystalCaverns("minecraft:crystal_caverns");
        setupSunkenChoir("minecraft:sunken_choir");
        setupHollowDeep("minecraft:hollow_deep");
        setupAuroraSteppe("minecraft:aurora_steppe");

        // The Aether biomes (also NOT in s_biomeKeyOrder — the Aether
        // generator builds its featuresPerStep from getAetherBiomeKeys())
        setupAetherBiome("aether:skyroot_meadow", AetherPlacements::SKYROOT_MEADOW_TREES);
        setupAetherBiome("aether:skyroot_forest", AetherPlacements::SKYROOT_FOREST_TREES);
        setupAetherBiome("aether:skyroot_grove", AetherPlacements::SKYROOT_GROVE_TREES);
        setupAetherBiome("aether:skyroot_woodland", AetherPlacements::SKYROOT_WOODLAND_TREES);

        // The Twilight Forest biomes (also NOT in s_biomeKeyOrder — the TF
        // generator builds its featuresPerStep from getTwilightBiomeKeys())
        setupTwilightBiomes();

        s_initialized.store(true, std::memory_order_release);
    });
}

const std::vector<std::string>& BiomeFeatureRegistry::getEndBiomeKeys() {
    // Java TheEndBiomeSource.collectPossibleBiomes() order:
    // end, highlands, midlands, islands, barrens.
    static const std::vector<std::string> s_endBiomeKeys = {
        "minecraft:the_end",
        "minecraft:end_highlands",
        "minecraft:end_midlands",
        "minecraft:small_end_islands",
        "minecraft:end_barrens"
    };
    if (!s_initialized.load(std::memory_order_acquire)) {
        bootstrap();
    }
    return s_endBiomeKeys;
}

const std::vector<std::string>& BiomeFeatureRegistry::getNetherBiomeKeys() {
    // Java: nether biome source possibleBiomes() - vanilla nether preset
    // parameter list order (multi_noise_biome_source_parameter_list/nether.json).
    static const std::vector<std::string> s_netherBiomeKeys = {
        "minecraft:nether_wastes",
        "minecraft:soul_sand_valley",
        "minecraft:crimson_forest",
        "minecraft:warped_forest",
        "minecraft:basalt_deltas"
    };
    if (!s_initialized.load(std::memory_order_acquire)) {
        bootstrap();
    }
    return s_netherBiomeKeys;
}

const std::vector<std::string>& BiomeFeatureRegistry::getHushBiomeKeys() {
    // MultiNoiseBiomeSource::buildHushParameters() order — keep the two in
    // sync, this is the Hush possibleBiomes() order that seeds features.
    static const std::vector<std::string> s_hushBiomeKeys = {
        "minecraft:hush_meadows",
        "minecraft:whisperwood_forest",
        "minecraft:resonant_barrens",
        "minecraft:crystal_caverns",
        "minecraft:sunken_choir",
        "minecraft:hollow_deep",
        "minecraft:aurora_steppe"
    };
    if (!s_initialized.load(std::memory_order_acquire)) {
        bootstrap();
    }
    return s_hushBiomeKeys;
}

const std::vector<std::string>& BiomeFeatureRegistry::getAetherBiomeKeys() {
    // MultiNoiseBiomeSource::buildAetherParameters() first-appearance order
    // (data/aether/dimension/the_aether.json) — keep the two in sync, this is
    // the Aether possibleBiomes() order that seeds features.
    static const std::vector<std::string> s_aetherBiomeKeys = {
        "aether:skyroot_meadow",
        "aether:skyroot_forest",
        "aether:skyroot_grove",
        "aether:skyroot_woodland"
    };
    if (!s_initialized.load(std::memory_order_acquire)) {
        bootstrap();
    }
    return s_aetherBiomeKeys;
}

const std::vector<std::string>& BiomeFeatureRegistry::getTwilightBiomeKeys() {
    // TF possibleBiomes() order (BiomeDensitySource.collectPossibleBiomes:
    // biome_grid.json columns sorted by key, biome_layers ascending, first
    // appearance kept) — the order that seeds every TF feature. The
    // TwilightBiomeSource's possibleBiomes() must list the same keys.
    static const std::vector<std::string> s_twilightBiomeKeys = {
        "twilightforest:underground",
        "twilightforest:clearing",
        "twilightforest:dark_forest",
        "twilightforest:dark_forest_center",
        "twilightforest:dense_forest",
        "twilightforest:dense_mushroom_forest",
        "twilightforest:enchanted_forest",
        "twilightforest:final_plateau",
        "twilightforest:fire_swamp",
        "twilightforest:firefly_forest",
        "twilightforest:forest",
        "twilightforest:glacier",
        "twilightforest:highlands_underground",
        "twilightforest:highlands",
        "twilightforest:lake",
        "twilightforest:mushroom_forest",
        "twilightforest:oak_savannah",
        "twilightforest:snowy_forest",
        "twilightforest:spooky_forest",
        "twilightforest:stream",
        "twilightforest:swamp",
        "twilightforest:thornlands"
    };
    if (!s_initialized.load(std::memory_order_acquire)) {
        bootstrap();
    }
    return s_twilightBiomeKeys;
}

const std::vector<const PlacedFeature*>& BiomeFeatureRegistry::getFeaturesForStep(
    const std::string& biomeKey,
    int step
) {
    if (!s_initialized.load(std::memory_order_acquire)) {
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
    if (!s_initialized.load(std::memory_order_acquire)) {
        bootstrap();
    }

    auto it = s_biomeFeatures.find(biomeKey);
    if (it == s_biomeFeatures.end()) {
        return s_emptyBiomeFeatures;
    }
    return it->second;
}

bool BiomeFeatureRegistry::hasFeature(const std::string& biomeKey, const PlacedFeature* feature) {
    if (!s_initialized.load(std::memory_order_acquire)) {
        bootstrap();
    }
    // Java: biome.getGenerationSettings().hasFeature(feature) is a HashSet
    // lookup. This was a linear scan over every step's feature list, called
    // once per BiomeFilter test.
    static std::mutex s_setMutex;
    static std::unordered_map<std::string, std::unordered_set<const PlacedFeature*>> s_sets;
    std::lock_guard<std::mutex> lock(s_setMutex);
    auto sit = s_sets.find(biomeKey);
    if (sit == s_sets.end()) {
        std::unordered_set<const PlacedFeature*> set;
        auto it = s_biomeFeatures.find(biomeKey);
        if (it != s_biomeFeatures.end()) {
            for (const auto& stepFeatures : it->second) {
                for (const auto* f : stepFeatures) set.insert(f);
            }
        }
        sit = s_sets.emplace(biomeKey, std::move(set)).first;
    }
    return sit->second.count(feature) != 0;
}

bool BiomeFeatureRegistry::isKnownBiomeKey(const std::string& biomeKey) {
    bootstrap();
    // the_void is a real vanilla biome (selectable for single-biome worlds)
    // with no features in this port (vanilla gives it only
    // void_start_platform); getFeaturesForBiome returns empty for it.
    if (biomeKey == "minecraft:the_void") return true;
    for (const auto& keys : {getAllBiomeKeys(), getNetherBiomeKeys(), getEndBiomeKeys(),
                             getHushBiomeKeys(), getAetherBiomeKeys(),
                             getTwilightBiomeKeys()}) {
        for (const auto& k : keys) {
            if (k == biomeKey) return true;
        }
    }
    return false;
}

const std::vector<std::string>& BiomeFeatureRegistry::getAllBiomeKeys() {
    if (!s_initialized.load(std::memory_order_acquire)) {
        bootstrap();
    }
    return s_biomeKeyOrder;
}

} // namespace worldgen
} // namespace data
} // namespace minecraft

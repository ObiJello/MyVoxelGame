#pragma once

#include "levelgen/feature/Feature.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/Blocks.h"
#include <memory>
#include <vector>

// Reference: net/minecraft/data/worldgen/features/VegetationFeatures.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

using namespace ::world;

/**
 * VegetationFeatures - Registry of configured vegetation features
 * Reference: VegetationFeatures.java
 *
 * This class creates ConfiguredFeature instances for grass, flowers, etc.
 */
class VegetationFeatures {
private:
    // Static feature instances
    static levelgen::RandomPatchFeature s_randomPatchFeature;
    static levelgen::SimpleBlockFeature s_simpleBlockFeature;

    static bool s_initialized;

public:
    // =========================================================================
    // GRASS PATCHES - Reference: VegetationFeatures.java
    // =========================================================================
    static levelgen::ConfiguredFeature* PATCH_GRASS;
    static levelgen::ConfiguredFeature* PATCH_TALL_GRASS;
    static levelgen::ConfiguredFeature* PATCH_FERN;
    static levelgen::ConfiguredFeature* PATCH_LARGE_FERN;
    static levelgen::ConfiguredFeature* PATCH_GRASS_MEADOW;     // Meadow grass
    static levelgen::ConfiguredFeature* PATCH_TAIGA_GRASS;      // Taiga grass + fern
    static levelgen::ConfiguredFeature* PATCH_GRASS_JUNGLE;     // Jungle grass
    static levelgen::ConfiguredFeature* SINGLE_PIECE_OF_GRASS;  // Single grass for bonemeal

    // =========================================================================
    // FLOWERS - Reference: VegetationFeatures.java
    // =========================================================================
    static levelgen::ConfiguredFeature* FLOWER_DEFAULT;   // Dandelion + Poppy
    static levelgen::ConfiguredFeature* FLOWER_PLAIN;     // Plains flowers
    static levelgen::ConfiguredFeature* FLOWER_MEADOW;    // Meadow flowers
    static levelgen::ConfiguredFeature* FLOWER_CHERRY;    // Cherry pink petals
    static levelgen::ConfiguredFeature* FLOWER_SWAMP;     // Blue orchid
    static levelgen::ConfiguredFeature* FLOWER_WARM;      // Warm biome flowers
    static levelgen::ConfiguredFeature* FLOWER_FOREST;    // Forest flowers
    static levelgen::ConfiguredFeature* FLOWER_FLOWER_FOREST;  // Flower forest flowers
    static levelgen::ConfiguredFeature* FLOWER_PALE_GARDEN;    // Pale garden eyeblossom
    static levelgen::ConfiguredFeature* WILDFLOWERS;      // Wildflowers
    static levelgen::ConfiguredFeature* WILDFLOWERS_BIRCH_FOREST; // Wildflowers for birch forest
    static levelgen::ConfiguredFeature* WILDFLOWERS_MEADOW;       // Wildflowers for meadow
    static levelgen::ConfiguredFeature* FOREST_FLOWERS;   // Forest flowers (lily of valley)

    // =========================================================================
    // MUSHROOMS - Reference: VegetationFeatures.java
    // =========================================================================
    static levelgen::ConfiguredFeature* PATCH_BROWN_MUSHROOM;
    static levelgen::ConfiguredFeature* PATCH_RED_MUSHROOM;
    static levelgen::ConfiguredFeature* HUGE_BROWN_MUSHROOM;
    static levelgen::ConfiguredFeature* HUGE_RED_MUSHROOM;
    static levelgen::ConfiguredFeature* MUSHROOM_ISLAND_VEGETATION;

    // =========================================================================
    // VEGETATION PATCHES - Reference: VegetationFeatures.java
    // =========================================================================
    static levelgen::ConfiguredFeature* PATCH_DEAD_BUSH;
    static levelgen::ConfiguredFeature* PATCH_DRY_GRASS;        // Dry grass for badlands/desert
    static levelgen::ConfiguredFeature* PATCH_SUNFLOWER;
    static levelgen::ConfiguredFeature* PATCH_PUMPKIN;
    static levelgen::ConfiguredFeature* PATCH_MELON;
    static levelgen::ConfiguredFeature* PATCH_CACTUS;
    static levelgen::ConfiguredFeature* PATCH_SUGAR_CANE;
    static levelgen::ConfiguredFeature* PATCH_BERRY_BUSH;       // Sweet berry bush
    static levelgen::ConfiguredFeature* PATCH_WATERLILY;        // Lily pad
    static levelgen::ConfiguredFeature* PATCH_FIREFLY_BUSH;     // Firefly bush
    static levelgen::ConfiguredFeature* PATCH_BUSH;             // Bush patch

    // =========================================================================
    // BAMBOO & VINES - Reference: VegetationFeatures.java
    // =========================================================================
    static levelgen::ConfiguredFeature* BAMBOO_NO_PODZOL;
    static levelgen::ConfiguredFeature* BAMBOO_SOME_PODZOL;
    static levelgen::ConfiguredFeature* VINES;
    static levelgen::ConfiguredFeature* BAMBOO_VEGETATION;

    // =========================================================================
    // LEAF LITTER & PALE GARDEN - Reference: VegetationFeatures.java
    // =========================================================================
    static levelgen::ConfiguredFeature* PATCH_LEAF_LITTER;
    static levelgen::ConfiguredFeature* TREES_BIRCH_AND_OAK_LEAF_LITTER;
    static levelgen::ConfiguredFeature* PALE_GARDEN_VEGETATION;
    static levelgen::ConfiguredFeature* PALE_MOSS_VEGETATION;
    static levelgen::ConfiguredFeature* PALE_MOSS_PATCH;
    static levelgen::ConfiguredFeature* PALE_MOSS_PATCH_BONEMEAL;
    static levelgen::ConfiguredFeature* PALE_GARDEN_FLOWERS;
    static levelgen::ConfiguredFeature* PALE_FOREST_FLOWERS;

    // =========================================================================
    // MANGROVE - Reference: VegetationFeatures.java line 112
    // =========================================================================
    static levelgen::ConfiguredFeature* MANGROVE_VEGETATION;

    // =========================================================================
    // DARK FOREST - Reference: VegetationFeatures.java
    // =========================================================================
    static levelgen::ConfiguredFeature* DARK_FOREST_VEGETATION;

    // =========================================================================
    // TREE SELECTORS - Reference: VegetationFeatures.java
    // =========================================================================
    static levelgen::ConfiguredFeature* TREES_PLAINS;
    static levelgen::ConfiguredFeature* TREES_TAIGA;
    static levelgen::ConfiguredFeature* TREES_GROVE;
    static levelgen::ConfiguredFeature* TREES_BADLANDS;
    static levelgen::ConfiguredFeature* TREES_SNOWY;
    static levelgen::ConfiguredFeature* TREES_SWAMP;
    static levelgen::ConfiguredFeature* TREES_WINDSWEPT_SAVANNA;
    static levelgen::ConfiguredFeature* TREES_SAVANNA;
    static levelgen::ConfiguredFeature* TREES_BIRCH;
    static levelgen::ConfiguredFeature* BIRCH_TALL;
    static levelgen::ConfiguredFeature* TREES_WINDSWEPT_FOREST;
    static levelgen::ConfiguredFeature* TREES_WINDSWEPT_HILLS;
    static levelgen::ConfiguredFeature* TREES_WATER;
    static levelgen::ConfiguredFeature* TREES_SPARSE_JUNGLE;
    static levelgen::ConfiguredFeature* TREES_OLD_GROWTH_SPRUCE_TAIGA;
    static levelgen::ConfiguredFeature* TREES_OLD_GROWTH_PINE_TAIGA;
    static levelgen::ConfiguredFeature* TREES_JUNGLE;
    static levelgen::ConfiguredFeature* TREES_FLOWER_FOREST;
    static levelgen::ConfiguredFeature* TREES_MEADOW;
    static levelgen::ConfiguredFeature* TREES_CHERRY;
    static levelgen::ConfiguredFeature* TREES_MANGROVE;
    static levelgen::ConfiguredFeature* MEADOW_TREES;

    /**
     * Bootstrap/initialize all vegetation features
     * Must be called before using any features
     */
    static void bootstrap();

    /**
     * Check if bootstrapped
     */
    static bool isInitialized() { return s_initialized; }

private:
    /**
     * Helper to check if a block can support grass/flowers
     * Reference: BlockPredicates.java - canSurvive checks
     */
    static bool canPlaceOnGround(levelgen::WorldGenLevel* level, const core::BlockPos& pos);

    /**
     * Helper to create a simple grass placer function
     */
    static std::function<bool(levelgen::WorldGenLevel*, levelgen::ChunkGenerator*, levelgen::WorldgenRandom&, const core::BlockPos&)>
    createGrassPlacer(BlockState* block);

    /**
     * Helper to create a flower placer that randomly picks from options
     */
    static std::function<bool(levelgen::WorldGenLevel*, levelgen::ChunkGenerator*, levelgen::WorldgenRandom&, const core::BlockPos&)>
    createFlowerPlacer(const std::vector<BlockState*>& flowers);
};

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft

#pragma once

#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/placement/PlacementModifiers.h"
#include "data/worldgen/features/VegetationFeatures.h"
#include "world/level/block/state/BlockState.h"
#include <vector>

// Reference: net/minecraft/data/worldgen/placement/VegetationPlacements.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace placement {

using namespace ::minecraft::levelgen::placement;

/**
 * VegetationPlacements - Registry of placed vegetation features
 * Reference: VegetationPlacements.java
 *
 * This class creates PlacedFeature instances that combine vegetation
 * ConfiguredFeatures with appropriate placement modifiers.
 */
class VegetationPlacements {
private:
    static bool s_initialized;

public:
    // =========================================================================
    // BAMBOO & VINES - Reference: VegetationPlacements.java lines 34-36
    // =========================================================================
    static const PlacedFeature* BAMBOO_LIGHT;
    static const PlacedFeature* BAMBOO;
    static const PlacedFeature* VINES;

    // =========================================================================
    // SUNFLOWER & PUMPKIN - Reference: VegetationPlacements.java lines 37-38
    // =========================================================================
    static const PlacedFeature* PATCH_SUNFLOWER;
    static const PlacedFeature* PATCH_PUMPKIN;

    // =========================================================================
    // GRASS PATCHES - Reference: VegetationPlacements.java lines 39-48
    // =========================================================================
    static const PlacedFeature* PATCH_GRASS_PLAIN;
    static const PlacedFeature* PATCH_GRASS_MEADOW;
    static const PlacedFeature* PATCH_GRASS_FOREST;
    static const PlacedFeature* PATCH_GRASS_BADLANDS;
    static const PlacedFeature* PATCH_GRASS_SAVANNA;
    static const PlacedFeature* PATCH_GRASS_NORMAL;
    static const PlacedFeature* PATCH_GRASS_TAIGA_2;
    static const PlacedFeature* PATCH_GRASS_TAIGA;
    static const PlacedFeature* PATCH_GRASS_JUNGLE;
    static const PlacedFeature* GRASS_BONEMEAL;

    // =========================================================================
    // DEAD BUSH & DRY GRASS - Reference: VegetationPlacements.java lines 49-53
    // =========================================================================
    static const PlacedFeature* PATCH_DEAD_BUSH_2;
    static const PlacedFeature* PATCH_DEAD_BUSH;
    static const PlacedFeature* PATCH_DEAD_BUSH_BADLANDS;
    static const PlacedFeature* PATCH_DRY_GRASS_BADLANDS;
    static const PlacedFeature* PATCH_DRY_GRASS_DESERT;

    // =========================================================================
    // MELON - Reference: VegetationPlacements.java lines 54-55
    // =========================================================================
    static const PlacedFeature* PATCH_MELON;
    static const PlacedFeature* PATCH_MELON_SPARSE;

    // =========================================================================
    // BERRY BUSH - Reference: VegetationPlacements.java lines 56-57
    // =========================================================================
    static const PlacedFeature* PATCH_BERRY_COMMON;
    static const PlacedFeature* PATCH_BERRY_RARE;

    // =========================================================================
    // WATERLILY - Reference: VegetationPlacements.java line 58
    // =========================================================================
    static const PlacedFeature* PATCH_WATERLILY;

    // =========================================================================
    // TALL GRASS & FERNS - Reference: VegetationPlacements.java lines 59-61
    // =========================================================================
    static const PlacedFeature* PATCH_TALL_GRASS_2;
    static const PlacedFeature* PATCH_TALL_GRASS;
    static const PlacedFeature* PATCH_LARGE_FERN;

    // =========================================================================
    // BUSH - Reference: VegetationPlacements.java line 62
    // =========================================================================
    static const PlacedFeature* PATCH_BUSH;

    // =========================================================================
    // LEAF LITTER - Reference: VegetationPlacements.java line 63
    // =========================================================================
    static const PlacedFeature* PATCH_LEAF_LITTER;

    // =========================================================================
    // CACTUS - Reference: VegetationPlacements.java lines 64-65
    // =========================================================================
    static const PlacedFeature* PATCH_CACTUS_DESERT;
    static const PlacedFeature* PATCH_CACTUS_DECORATED;

    // =========================================================================
    // SUGAR CANE - Reference: VegetationPlacements.java lines 66-69
    // =========================================================================
    static const PlacedFeature* PATCH_SUGAR_CANE_SWAMP;
    static const PlacedFeature* PATCH_SUGAR_CANE_DESERT;
    static const PlacedFeature* PATCH_SUGAR_CANE_BADLANDS;
    static const PlacedFeature* PATCH_SUGAR_CANE;

    // =========================================================================
    // FIREFLY BUSH - Reference: VegetationPlacements.java lines 70-72
    // =========================================================================
    static const PlacedFeature* PATCH_FIREFLY_BUSH_SWAMP;
    static const PlacedFeature* PATCH_FIREFLY_BUSH_NEAR_WATER_SWAMP;
    static const PlacedFeature* PATCH_FIREFLY_BUSH_NEAR_WATER;

    // =========================================================================
    // MUSHROOMS - Reference: VegetationPlacements.java lines 73-82
    // =========================================================================
    static const PlacedFeature* BROWN_MUSHROOM_NETHER;
    static const PlacedFeature* RED_MUSHROOM_NETHER;
    static const PlacedFeature* BROWN_MUSHROOM_NORMAL;
    static const PlacedFeature* RED_MUSHROOM_NORMAL;
    static const PlacedFeature* BROWN_MUSHROOM_TAIGA;
    static const PlacedFeature* RED_MUSHROOM_TAIGA;
    static const PlacedFeature* BROWN_MUSHROOM_OLD_GROWTH;
    static const PlacedFeature* RED_MUSHROOM_OLD_GROWTH;
    static const PlacedFeature* BROWN_MUSHROOM_SWAMP;
    static const PlacedFeature* RED_MUSHROOM_SWAMP;

    // =========================================================================
    // FLOWERS - Reference: VegetationPlacements.java lines 83-92
    // =========================================================================
    static const PlacedFeature* FLOWER_WARM;
    static const PlacedFeature* FLOWER_DEFAULT;
    static const PlacedFeature* FLOWER_FLOWER_FOREST;
    static const PlacedFeature* FLOWER_SWAMP;
    static const PlacedFeature* FLOWER_PLAINS;
    static const PlacedFeature* FLOWER_MEADOW;
    static const PlacedFeature* FLOWER_CHERRY;
    static const PlacedFeature* FLOWER_PALE_GARDEN;
    static const PlacedFeature* WILDFLOWERS_BIRCH_FOREST;
    static const PlacedFeature* WILDFLOWERS_MEADOW;

    // =========================================================================
    // TREES - Reference: VegetationPlacements.java lines 93-122
    // =========================================================================
    static const PlacedFeature* TREES_PLAINS;
    static const PlacedFeature* DARK_FOREST_VEGETATION;
    static const PlacedFeature* PALE_GARDEN_VEGETATION;
    static const PlacedFeature* FLOWER_FOREST_FLOWERS;
    static const PlacedFeature* FOREST_FLOWERS;
    static const PlacedFeature* PALE_GARDEN_FLOWERS;
    static const PlacedFeature* PALE_MOSS_PATCH;
    static const PlacedFeature* TREES_FLOWER_FOREST;
    static const PlacedFeature* TREES_MEADOW;
    static const PlacedFeature* TREES_CHERRY;
    static const PlacedFeature* TREES_TAIGA;
    static const PlacedFeature* TREES_GROVE;
    static const PlacedFeature* TREES_BADLANDS;
    static const PlacedFeature* TREES_SNOWY;
    static const PlacedFeature* TREES_SWAMP;
    static const PlacedFeature* TREES_WINDSWEPT_SAVANNA;
    static const PlacedFeature* TREES_SAVANNA;
    static const PlacedFeature* BIRCH_TALL;
    static const PlacedFeature* TREES_BIRCH;
    static const PlacedFeature* TREES_WINDSWEPT_FOREST;
    static const PlacedFeature* TREES_WINDSWEPT_HILLS;
    static const PlacedFeature* TREES_WATER;
    static const PlacedFeature* TREES_BIRCH_AND_OAK_LEAF_LITTER;
    static const PlacedFeature* TREES_SPARSE_JUNGLE;
    static const PlacedFeature* TREES_OLD_GROWTH_SPRUCE_TAIGA;
    static const PlacedFeature* TREES_OLD_GROWTH_PINE_TAIGA;
    static const PlacedFeature* TREES_JUNGLE;
    static const PlacedFeature* BAMBOO_VEGETATION;
    static const PlacedFeature* MUSHROOM_ISLAND_VEGETATION;
    static const PlacedFeature* TREES_MANGROVE;

    // Kept for backwards compatibility (old names)
    static const PlacedFeature* PATCH_FERN;       // Use PATCH_LARGE_FERN
    static const PlacedFeature* FLOWER_PLAIN;    // Use FLOWER_PLAINS
    static const PlacedFeature* FLOWER_FOREST;   // Use FLOWER_DEFAULT
    static const PlacedFeature* PATCH_CACTUS;    // Use PATCH_CACTUS_DESERT

    /**
     * Bootstrap/initialize all vegetation placements
     * Must be called after VegetationFeatures::bootstrap()
     */
    static void bootstrap();

    /**
     * Check if bootstrapped
     */
    static bool isInitialized() { return s_initialized; }

    // =========================================================================
    // HELPER METHODS - Reference: VegetationPlacements.java lines 125-155
    // =========================================================================

    /**
     * Helper to create common grass placement modifiers
     * Reference: VegetationPlacements.java worldSurfaceSquaredWithCount()
     * Returns: CountPlacement, InSquarePlacement, HEIGHTMAP_WORLD_SURFACE, BiomeFilter
     */
    static std::vector<PlacementModifier*> worldSurfaceSquaredWithCount(int count);

    /**
     * Helper for mushroom placements
     * Reference: VegetationPlacements.java getMushroomPlacement()
     */
    static std::vector<PlacementModifier*> getMushroomPlacement(int rarity, PlacementModifier* prefix);

    /**
     * Helper for tree placements (base)
     * Reference: VegetationPlacements.java treePlacementBase()
     */
    static std::vector<PlacementModifier*> treePlacementBase(PlacementModifier* frequency);

    /**
     * Helper for tree placements
     * Reference: VegetationPlacements.java treePlacement()
     */
    static std::vector<PlacementModifier*> treePlacement(PlacementModifier* frequency);

    /**
     * Helper for tree placements with sapling survival check
     * Reference: VegetationPlacements.java treePlacement(PlacementModifier, Block)
     */
    static std::vector<PlacementModifier*> treePlacementWithSapling(PlacementModifier* frequency, BlockState* sapling);

private:
    /**
     * Helper to create flower placement modifiers (deprecated, use direct registration)
     */
    static std::vector<PlacementModifier*> flowerPlacement(int rarity);
};

} // namespace placement
} // namespace worldgen
} // namespace data
} // namespace minecraft

#pragma once

#include "levelgen/feature/Feature.h"
#include "world/level/block/Blocks.h"
#include <memory>
#include <vector>

// Reference: net/minecraft/data/worldgen/features/CaveFeatures.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

using namespace ::world;

/**
 * CaveFeatures - Registry of configured cave features
 * Reference: CaveFeatures.java
 *
 * This class creates ConfiguredFeature instances for geodes, etc.
 */
class CaveFeatures {
private:
    // Static feature instances
    static levelgen::GeodeFeature s_geodeFeature;
    static levelgen::MultifaceGrowthFeature s_multifaceGrowthFeature;
    static levelgen::SimpleBlockFeature s_simpleBlockFeature;
    static levelgen::VegetationPatchFeature s_vegetationPatchFeature;
    static levelgen::BlockColumnFeature s_blockColumnFeature;
    static levelgen::SimpleRandomSelectorFeature s_simpleRandomSelectorFeature;
    static levelgen::UnderwaterMagmaFeature s_underwaterMagmaFeature;
    static levelgen::SculkPatchFeature s_sculkPatchFeature;
    static levelgen::MonsterRoomFeature s_monsterRoomFeature;
    static levelgen::FossilFeature s_fossilFeature;
    static levelgen::DripstoneClusterFeature s_dripstoneClusterFeature;
    static levelgen::LargeDripstoneFeature s_largeDripstoneFeature;
    static levelgen::PointedDripstoneFeature s_pointedDripstoneFeature;
    static levelgen::RootSystemFeature s_rootSystemFeature;
    static levelgen::RandomBooleanSelectorFeature s_randomBooleanSelectorFeature;
    static levelgen::WaterloggedVegetationPatchFeature s_waterloggedVegPatchFeature;

    static bool s_initialized;

public:
    // =========================================================================
    // DUNGEON - Reference: CaveFeatures.java line 59
    // =========================================================================
    static levelgen::ConfiguredFeature* MONSTER_ROOM;

    // =========================================================================
    // FOSSILS - Reference: CaveFeatures.java lines 60-61
    // =========================================================================
    static levelgen::ConfiguredFeature* FOSSIL_COAL;
    static levelgen::ConfiguredFeature* FOSSIL_DIAMONDS;

    // =========================================================================
    // DRIPSTONE - Reference: CaveFeatures.java lines 62-64
    // =========================================================================
    static levelgen::ConfiguredFeature* DRIPSTONE_CLUSTER;
    static levelgen::ConfiguredFeature* LARGE_DRIPSTONE;
    static levelgen::ConfiguredFeature* POINTED_DRIPSTONE;

    // =========================================================================
    // UNDERWATER - Reference: CaveFeatures.java line 65
    // =========================================================================
    static levelgen::ConfiguredFeature* UNDERWATER_MAGMA;

    // =========================================================================
    // MULTIFACE - Reference: CaveFeatures.java line 66
    // =========================================================================
    static levelgen::ConfiguredFeature* GLOW_LICHEN;

    // =========================================================================
    // AZALEA - Reference: CaveFeatures.java line 67
    // =========================================================================
    static levelgen::ConfiguredFeature* ROOTED_AZALEA_TREE;

    // =========================================================================
    // CAVE VINES - Reference: CaveFeatures.java lines 68-69
    // =========================================================================
    static levelgen::ConfiguredFeature* CAVE_VINE;
    static levelgen::ConfiguredFeature* CAVE_VINE_IN_MOSS;

    // =========================================================================
    // MOSS - Reference: CaveFeatures.java lines 70-72
    // =========================================================================
    static levelgen::ConfiguredFeature* MOSS_VEGETATION;
    static levelgen::ConfiguredFeature* MOSS_PATCH;
    static levelgen::ConfiguredFeature* MOSS_PATCH_BONEMEAL;

    // =========================================================================
    // DRIPLEAF - Reference: CaveFeatures.java lines 73-76
    // =========================================================================
    static levelgen::ConfiguredFeature* DRIPLEAF;
    static levelgen::ConfiguredFeature* CLAY_WITH_DRIPLEAVES;
    static levelgen::ConfiguredFeature* CLAY_POOL_WITH_DRIPLEAVES;
    static levelgen::ConfiguredFeature* LUSH_CAVES_CLAY;

    // =========================================================================
    // CEILING - Reference: CaveFeatures.java lines 77-78
    // =========================================================================
    static levelgen::ConfiguredFeature* MOSS_PATCH_CEILING;
    static levelgen::ConfiguredFeature* SPORE_BLOSSOM;

    // =========================================================================
    // GEODE - Reference: CaveFeatures.java line 79
    // =========================================================================
    static levelgen::ConfiguredFeature* AMETHYST_GEODE;

    // =========================================================================
    // SCULK - Reference: CaveFeatures.java lines 80-82
    // =========================================================================
    static levelgen::ConfiguredFeature* SCULK_PATCH_DEEP_DARK;
    static levelgen::ConfiguredFeature* SCULK_PATCH_ANCIENT_CITY;
    static levelgen::ConfiguredFeature* SCULK_VEIN;

    /**
     * Bootstrap/initialize all cave features
     * Must be called before using any features
     */
    static void bootstrap();

    /**
     * Check if bootstrapped
     */
    static bool isInitialized() { return s_initialized; }
};

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft

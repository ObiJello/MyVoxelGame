#pragma once

#include "levelgen/feature/Feature.h"
#include "world/level/block/Blocks.h"
#include <memory>
#include <vector>

// Reference: net/minecraft/data/worldgen/features/MiscOverworldFeatures.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

using namespace ::world;

/**
 * MiscOverworldFeatures - Registry of miscellaneous overworld configured features
 * Reference: MiscOverworldFeatures.java
 */
class MiscOverworldFeatures {
private:
    static levelgen::DiskFeature s_diskFeature;
    static levelgen::SnowAndFreezeFeature s_snowAndFreezeFeature;
    static levelgen::IceSpikeFeature s_iceSpikeFeature;
    static levelgen::ForestRockFeature s_forestRockFeature;
    static levelgen::IcebergFeature s_icebergFeature;
    static levelgen::BlueIceFeature s_blueIceFeature;
    static levelgen::LakeFeature s_lakeFeature;
    static levelgen::SpringFeature s_springFeature;
    static levelgen::DesertWellFeature s_desertWellFeature;
    static levelgen::VoidStartPlatformFeature s_voidStartPlatformFeature;
    static levelgen::BonusChestFeature s_bonusChestFeature;
    static bool s_initialized;

public:
    // =========================================================================
    // ICE FEATURES - Reference: MiscOverworldFeatures.java lines 23-28
    // =========================================================================
    static levelgen::ConfiguredFeature* ICE_SPIKE;
    static levelgen::ConfiguredFeature* ICE_PATCH;
    static levelgen::ConfiguredFeature* ICEBERG_PACKED;
    static levelgen::ConfiguredFeature* ICEBERG_BLUE;
    static levelgen::ConfiguredFeature* BLUE_ICE;

    // =========================================================================
    // MISC FEATURES - Reference: MiscOverworldFeatures.java lines 25, 29
    // =========================================================================
    static levelgen::ConfiguredFeature* FOREST_ROCK;
    static levelgen::ConfiguredFeature* LAKE_LAVA;

    // =========================================================================
    // DISK FEATURES - Reference: MiscOverworldFeatures.java lines 30-34
    // =========================================================================
    static levelgen::ConfiguredFeature* DISK_CLAY;
    static levelgen::ConfiguredFeature* DISK_GRAVEL;
    static levelgen::ConfiguredFeature* DISK_SAND;
    static levelgen::ConfiguredFeature* DISK_GRASS;

    // =========================================================================
    // SPECIAL FEATURES - Reference: MiscOverworldFeatures.java lines 35-40
    // =========================================================================
    static levelgen::ConfiguredFeature* FREEZE_TOP_LAYER;
    static levelgen::ConfiguredFeature* BONUS_CHEST;
    static levelgen::ConfiguredFeature* VOID_START_PLATFORM;
    static levelgen::ConfiguredFeature* DESERT_WELL;

    // =========================================================================
    // SPRING FEATURES - Reference: MiscOverworldFeatures.java lines 38-40
    // =========================================================================
    static levelgen::ConfiguredFeature* SPRING_LAVA_OVERWORLD;
    static levelgen::ConfiguredFeature* SPRING_LAVA_FROZEN;
    static levelgen::ConfiguredFeature* SPRING_WATER;

    /**
     * Bootstrap/initialize all miscellaneous overworld features
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

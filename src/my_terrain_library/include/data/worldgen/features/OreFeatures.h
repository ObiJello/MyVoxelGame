#pragma once

#include "levelgen/feature/Feature.h"
#include "levelgen/structure/templatesystem/RuleTest.h"
#include "world/level/block/Blocks.h"
#include <memory>
#include <vector>

// Reference: net/minecraft/data/worldgen/features/OreFeatures.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

/**
 * OreFeatures - Registry of configured ore features
 * Reference: OreFeatures.java
 *
 * This class creates ConfiguredFeature instances for all ore types.
 * Each feature pairs an OreFeature with an OreConfiguration.
 */
class OreFeatures {
private:
    // Static feature instance
    static levelgen::OreFeature s_oreFeature;

    // RuleTest instances (shared)
    static std::shared_ptr<levelgen::structure::templatesystem::TagMatchTest> s_stoneOreReplaceables;
    static std::shared_ptr<levelgen::structure::templatesystem::TagMatchTest> s_deepslateOreReplaceables;
    static std::shared_ptr<levelgen::structure::templatesystem::TagMatchTest> s_naturalStone;
    static std::shared_ptr<levelgen::structure::templatesystem::TagMatchTest> s_netherrack;
    static std::shared_ptr<levelgen::structure::templatesystem::TagMatchTest> s_netherOreReplaceables;

    static bool s_initialized;

public:
    // Configured features - pointers to static instances
    static levelgen::ConfiguredFeature* ORE_COAL;
    static levelgen::ConfiguredFeature* ORE_COAL_BURIED;
    static levelgen::ConfiguredFeature* ORE_IRON;
    static levelgen::ConfiguredFeature* ORE_IRON_SMALL;
    static levelgen::ConfiguredFeature* ORE_GOLD;
    static levelgen::ConfiguredFeature* ORE_GOLD_BURIED;
    static levelgen::ConfiguredFeature* ORE_REDSTONE;
    static levelgen::ConfiguredFeature* ORE_DIAMOND_SMALL;
    static levelgen::ConfiguredFeature* ORE_DIAMOND_MEDIUM;
    static levelgen::ConfiguredFeature* ORE_DIAMOND_LARGE;
    static levelgen::ConfiguredFeature* ORE_DIAMOND_BURIED;
    static levelgen::ConfiguredFeature* ORE_LAPIS;
    static levelgen::ConfiguredFeature* ORE_LAPIS_BURIED;
    static levelgen::ConfiguredFeature* ORE_EMERALD;
    static levelgen::ConfiguredFeature* ORE_COPPER_SMALL;
    static levelgen::ConfiguredFeature* ORE_COPPER_LARGE;
    static levelgen::ConfiguredFeature* ORE_INFESTED;
    static levelgen::ConfiguredFeature* ORE_DIRT;
    static levelgen::ConfiguredFeature* ORE_GRAVEL;
    static levelgen::ConfiguredFeature* ORE_GRANITE;
    static levelgen::ConfiguredFeature* ORE_DIORITE;
    static levelgen::ConfiguredFeature* ORE_ANDESITE;
    static levelgen::ConfiguredFeature* ORE_TUFF;
    static levelgen::ConfiguredFeature* ORE_CLAY;

    /**
     * Bootstrap/initialize all ore features
     * Must be called before using any features
     */
    static void bootstrap();

    /**
     * Check if bootstrapped
     */
    static bool isInitialized() { return s_initialized; }

private:
    /**
     * Helper to create ore configuration with stone + deepslate targets
     */
    static levelgen::OreConfiguration createOreConfig(
        BlockState* stoneOre,
        BlockState* deepslateOre,
        int32_t size,
        float discardChance = 0.0f
    );

    /**
     * Helper to create ore configuration with single target
     */
    static levelgen::OreConfiguration createSingleOreConfig(
        std::shared_ptr<levelgen::structure::templatesystem::RuleTest> target,
        BlockState* ore,
        int32_t size,
        float discardChance = 0.0f
    );
};

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft

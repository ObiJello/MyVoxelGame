#pragma once

#include "world/biome/Biome.h"
#include "levelgen/GenerationStep.h"
#include "data/worldgen/placement/OrePlacements.h"

// Reference: net/minecraft/data/worldgen/BiomeDefaultFeatures.java

namespace minecraft {
namespace data {
namespace worldgen {

/**
 * BiomeDefaultFeatures - Helper methods to add default features to biomes
 * Reference: BiomeDefaultFeatures.java
 *
 * These methods populate BiomeGenerationSettings with PlacedFeature instances.
 */
class BiomeDefaultFeatures {
public:
    /**
     * Add default ores to overworld biomes
     * Reference: BiomeDefaultFeatures.java addDefaultOres() lines 303-327
     *
     * Adds: coal, iron, gold, redstone, diamond, lapis, copper, emerald
     * Also adds: dirt, gravel, granite, diorite, andesite, tuff
     */
    static void addDefaultOres(::world::biome::BiomeGenerationSettings& settings) {
        // Ensure placements are initialized
        if (!placement::OrePlacements::isInitialized()) {
            placement::OrePlacements::bootstrap();
        }

        // Stone variants (scattered through stone)
        // Reference: BiomeDefaultFeatures.java lines 304-313
        settings.addFeature(GenerationStep::UNDERGROUND_ORES, placement::OrePlacements::ORE_DIRT);
        settings.addFeature(GenerationStep::UNDERGROUND_ORES, placement::OrePlacements::ORE_GRAVEL);
        settings.addFeature(GenerationStep::UNDERGROUND_ORES, placement::OrePlacements::ORE_GRANITE_UPPER);
        settings.addFeature(GenerationStep::UNDERGROUND_ORES, placement::OrePlacements::ORE_GRANITE_LOWER);
        settings.addFeature(GenerationStep::UNDERGROUND_ORES, placement::OrePlacements::ORE_DIORITE_UPPER);
        settings.addFeature(GenerationStep::UNDERGROUND_ORES, placement::OrePlacements::ORE_DIORITE_LOWER);
        settings.addFeature(GenerationStep::UNDERGROUND_ORES, placement::OrePlacements::ORE_ANDESITE_UPPER);
        settings.addFeature(GenerationStep::UNDERGROUND_ORES, placement::OrePlacements::ORE_ANDESITE_LOWER);
        settings.addFeature(GenerationStep::UNDERGROUND_ORES, placement::OrePlacements::ORE_TUFF);

        // Ores (actual valuable resources)
        // Reference: BiomeDefaultFeatures.java lines 314-327
        settings.addFeature(GenerationStep::UNDERGROUND_ORES, placement::OrePlacements::ORE_COAL_UPPER);
        settings.addFeature(GenerationStep::UNDERGROUND_ORES, placement::OrePlacements::ORE_COAL_LOWER);
        settings.addFeature(GenerationStep::UNDERGROUND_ORES, placement::OrePlacements::ORE_IRON_UPPER);
        settings.addFeature(GenerationStep::UNDERGROUND_ORES, placement::OrePlacements::ORE_IRON_MIDDLE);
        settings.addFeature(GenerationStep::UNDERGROUND_ORES, placement::OrePlacements::ORE_IRON_SMALL);
        settings.addFeature(GenerationStep::UNDERGROUND_ORES, placement::OrePlacements::ORE_GOLD);
        settings.addFeature(GenerationStep::UNDERGROUND_ORES, placement::OrePlacements::ORE_GOLD_LOWER);
        settings.addFeature(GenerationStep::UNDERGROUND_ORES, placement::OrePlacements::ORE_REDSTONE);
        settings.addFeature(GenerationStep::UNDERGROUND_ORES, placement::OrePlacements::ORE_REDSTONE_LOWER);
        settings.addFeature(GenerationStep::UNDERGROUND_ORES, placement::OrePlacements::ORE_DIAMOND);
        settings.addFeature(GenerationStep::UNDERGROUND_ORES, placement::OrePlacements::ORE_DIAMOND_MEDIUM);
        settings.addFeature(GenerationStep::UNDERGROUND_ORES, placement::OrePlacements::ORE_DIAMOND_LARGE);
        settings.addFeature(GenerationStep::UNDERGROUND_ORES, placement::OrePlacements::ORE_DIAMOND_BURIED);
        settings.addFeature(GenerationStep::UNDERGROUND_ORES, placement::OrePlacements::ORE_LAPIS);
        settings.addFeature(GenerationStep::UNDERGROUND_ORES, placement::OrePlacements::ORE_LAPIS_BURIED);
        settings.addFeature(GenerationStep::UNDERGROUND_ORES, placement::OrePlacements::ORE_COPPER);
    }

    /**
     * Add extra gold for badlands biomes
     * Reference: BiomeDefaultFeatures.java addExtraGold()
     */
    static void addExtraGold(::world::biome::BiomeGenerationSettings& settings) {
        // Badlands have extra gold in mesa layers
        // TODO: Add ORE_GOLD_EXTRA placement
    }

    /**
     * Add emerald ore for mountain biomes
     * Reference: BiomeDefaultFeatures.java addDefaultExtraVegetation() includes emerald
     */
    static void addEmeraldOre(::world::biome::BiomeGenerationSettings& settings) {
        if (!placement::OrePlacements::isInitialized()) {
            placement::OrePlacements::bootstrap();
        }
        settings.addFeature(GenerationStep::UNDERGROUND_ORES, placement::OrePlacements::ORE_EMERALD);
    }

    /**
     * Bootstrap all feature registries
     * Call this before using any features
     */
    static void bootstrap() {
        features::OreFeatures::bootstrap();
        placement::OrePlacements::bootstrap();
    }
};

} // namespace worldgen
} // namespace data
} // namespace minecraft

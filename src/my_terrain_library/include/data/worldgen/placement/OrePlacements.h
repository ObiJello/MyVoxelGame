#pragma once

#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/placement/PlacementModifiers.h"
#include "data/worldgen/features/OreFeatures.h"
#include <vector>
#include <memory>

// Reference: net/minecraft/data/worldgen/placement/OrePlacements.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace placement {

using namespace levelgen;
using namespace levelgen::placement;

/**
 * OrePlacements - Registry of placed ore features
 * Reference: OrePlacements.java
 *
 * Creates PlacedFeature instances that combine ConfiguredFeatures
 * with placement modifiers (count, spread, height range, biome filter).
 */
class OrePlacements {
private:
    static bool s_initialized;

public:
    // Key overworld ore placements
    static PlacedFeature* ORE_COAL_UPPER;
    static PlacedFeature* ORE_COAL_LOWER;
    static PlacedFeature* ORE_IRON_UPPER;
    static PlacedFeature* ORE_IRON_MIDDLE;
    static PlacedFeature* ORE_IRON_SMALL;
    static PlacedFeature* ORE_GOLD;
    static PlacedFeature* ORE_GOLD_LOWER;
    static PlacedFeature* ORE_REDSTONE;
    static PlacedFeature* ORE_REDSTONE_LOWER;
    static PlacedFeature* ORE_DIAMOND;
    static PlacedFeature* ORE_DIAMOND_MEDIUM;
    static PlacedFeature* ORE_DIAMOND_LARGE;
    static PlacedFeature* ORE_DIAMOND_BURIED;
    static PlacedFeature* ORE_LAPIS;
    static PlacedFeature* ORE_LAPIS_BURIED;
    static PlacedFeature* ORE_COPPER;
    static PlacedFeature* ORE_COPPER_LARGE;   // For dripstone caves
    static PlacedFeature* ORE_GOLD_EXTRA;     // Extra gold for badlands
    static PlacedFeature* ORE_EMERALD;
    static PlacedFeature* ORE_INFESTED;
    static PlacedFeature* ORE_CLAY;           // For lush caves

    // Stone variant placements
    static PlacedFeature* ORE_DIRT;
    static PlacedFeature* ORE_GRAVEL;
    static PlacedFeature* ORE_GRANITE_UPPER;
    static PlacedFeature* ORE_GRANITE_LOWER;
    static PlacedFeature* ORE_DIORITE_UPPER;
    static PlacedFeature* ORE_DIORITE_LOWER;
    static PlacedFeature* ORE_ANDESITE_UPPER;
    static PlacedFeature* ORE_ANDESITE_LOWER;
    static PlacedFeature* ORE_TUFF;

    /**
     * Bootstrap/initialize all ore placements
     * Must be called after OreFeatures::bootstrap()
     */
    static void bootstrap();

    /**
     * Check if bootstrapped
     */
    static bool isInitialized() { return s_initialized; }

private:
    /**
     * Common ore placement modifiers
     * Reference: OrePlacements.java commonOrePlacement()
     */
    static std::vector<PlacementModifier*> commonOrePlacement(
        int32_t count,
        PlacementModifier* heightRange
    );

    /**
     * Rare ore placement modifiers
     * Reference: OrePlacements.java rareOrePlacement()
     */
    static std::vector<PlacementModifier*> rareOrePlacement(
        int32_t rarity,
        PlacementModifier* heightRange
    );
};

} // namespace placement
} // namespace worldgen
} // namespace data
} // namespace minecraft

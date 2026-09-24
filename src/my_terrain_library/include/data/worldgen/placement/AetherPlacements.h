#pragma once

#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/placement/PlacementModifier.h"
#include "levelgen/placement/PlacementModifiers.h"
#include "levelgen/placement/PlacementContext.h"
#include "levelgen/carver/CarverConfiguration.h"
#include "levelgen/Heightmap.h"
#include "data/worldgen/features/AetherFeatures.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The Aether 1.5.10 — placed features from data/aether/worldgen/placed_feature/
// *.json (AetherPlacedFeatures.java), plus the mod's one placement modifier the
// vegetation needs (aether:improved_layer_placement).

namespace minecraft {
namespace data {
namespace worldgen {
namespace placement {

using namespace levelgen;
using namespace levelgen::placement;

/**
 * ImprovedLayerPlacement — world/placementmodifier/ImprovedLayerPlacementModifier
 * .java, the mod's [CODE COPY] of CountOnEveryLayerPlacement with a chosen
 * heightmap and a vertical-bounds check: layer by layer, `count` random
 * columns (the provider re-sampled in the loop condition, as in Java) each
 * yield the i-th air-over-solid position scanning down from the heightmap,
 * whose `verticalBounds` blocks from that position up hold nothing solid;
 * stops after the first layer that yields nothing. "Solid" is anything but
 * air, water and lava; bedrock never counts as ground.
 */
class ImprovedLayerPlacement : public PlacementModifier {
private:
    Heightmap::Types m_heightmap;
    const carver::IntProvider* m_count;
    int32_t m_verticalBounds;

public:
    ImprovedLayerPlacement(Heightmap::Types heightmap, const carver::IntProvider* count,
                           int32_t verticalBounds)
        : m_heightmap(heightmap), m_count(count), m_verticalBounds(verticalBounds) {}

    void appendPositions(
        PlacementContext& context,
        WorldgenRandom& random,
        const core::BlockPos& origin,
        std::vector<core::BlockPos>& out
    ) override;

    std::string getTypeName() const override { return "ImprovedLayerPlacement"; }

    std::string describeTrace(
        PlacementContext& context,
        const core::BlockPos& origin,
        const std::vector<core::BlockPos>& results
    ) const override {
        (void)context;
        (void)origin;
        return "count=" + std::to_string(results.size());
    }

private:
    bool findOnGroundPosition(PlacementContext& context, int32_t x, int32_t y, int32_t z,
                              int32_t layer, core::BlockPos& out) const;
    bool checkVerticalBounds(PlacementContext& context, int32_t x, int32_t y, int32_t z) const;
};

/**
 * HolidayFilter — world/placementmodifier/HolidayFilter.java. Passes when the
 * server config generates holiday trees always (default false) or seasonally
 * (default true) and the local calendar month is December or January.
 */
class HolidayFilter : public PlacementFilter {
public:
    std::string getTypeName() const override { return "HolidayFilter"; }

protected:
    bool shouldPlace(PlacementContext& context, WorldgenRandom& random,
                     const core::BlockPos& origin) override;
};

/**
 * DungeonBlacklistFilter — world/placementmodifier/DungeonBlacklistFilter.java.
 * Rejects a position inside the bounding box of any start of a structure in
 * #aether:dungeons (bronze, silver, gold dungeon) referenced by the chunk it
 * lies in (StructureManager.getStructureAt). Draws nothing.
 */
class DungeonBlacklistFilter : public PlacementFilter {
public:
    std::string getTypeName() const override { return "DungeonBlacklistFilter"; }

protected:
    bool shouldPlace(PlacementContext& context, WorldgenRandom& random,
                     const core::BlockPos& origin) override;
};

/**
 * AetherPlacements - Registry of The Aether's placed features
 *
 *   AETHER_DIRT_ORE       count 20, in_square, uniform(bottom, above_bottom 128)
 *   ICESTONE_ORE          count 10, in_square, uniform(bottom, above_bottom 128)
 *   AMBROSIUM_ORE         count 20, in_square, uniform(bottom, above_bottom 128)
 *   ZANITE_ORE            count 14, in_square, uniform(bottom, above_bottom 75)
 *   GRAVITITE_ORE_BURIED  count 5,  in_square, uniform(bottom, above_bottom 74)
 *   GRAVITITE_ORE         count 7,  in_square, trapezoid(above_bottom -58, 74)
 *   SKYROOT_{MEADOW,FOREST,GROVE,WOODLAND}_TREES
 *                         rarity 1 / weighted count (6|7, 2|3, 5|6), surface
 *                         water depth 0, improved layer (OCEAN_FLOOR, 0..1, 4)
 *   GRASS_PATCH           noise threshold (-0.8, 5, 10), improved layer (MOTION_BLOCKING)
 *   TALL_GRASS_PATCH      noise threshold (-0.8, 0, 7), rarity 32, improved layer
 *   WHITE_FLOWER_PATCH    rarity 8,  improved layer
 *   PURPLE_FLOWER_PATCH   rarity 16, improved layer
 *   BERRY_BUSH_PATCH      rarity 8,  improved layer
 *   COLD_AERCLOUD         uniform(32, 96),  rarity 7,  in_square
 *   BLUE_AERCLOUD         uniform(32, 96),  rarity 24, in_square
 *   GOLDEN_AERCLOUD       uniform(96, 128), rarity 75, in_square
 *   QUICKSOIL_SHELF       rarity 5, heightmap WORLD_SURFACE_WG
 *   CRYSTAL_ISLAND        rarity 50, in_square, uniform(32, 96), biome, dungeon blacklist
 *   HOLIDAY_TREE          rarity 75, in_square, water depth 0, heightmap OCEAN_FLOOR,
 *                         biome, holiday filter, would_survive(skyroot_sapling)
 *   WATER_LAKE            rarity 15, heightmap WORLD_SURFACE_WG, biome
 *   WATER_SPRING          count 30, in_square, uniform(above_bottom 8, 128),
 *                         biome, dungeon blacklist
 * every list ending with biome (plus aether:dungeon_blacklist_filter where the
 * JSON has it: trees, grass, tall grass, aerclouds, shelf, crystal island,
 * spring). The mod's config_filter (tall grass on by default) always passes
 * and is left out.
 */
class AetherPlacements {
private:
    static bool s_initialized;

public:
    static PlacedFeature* AETHER_DIRT_ORE;
    static PlacedFeature* ICESTONE_ORE;
    static PlacedFeature* AMBROSIUM_ORE;
    static PlacedFeature* ZANITE_ORE;
    static PlacedFeature* GRAVITITE_ORE_BURIED;
    static PlacedFeature* GRAVITITE_ORE;

    static PlacedFeature* SKYROOT_MEADOW_TREES;
    static PlacedFeature* SKYROOT_FOREST_TREES;
    static PlacedFeature* SKYROOT_GROVE_TREES;
    static PlacedFeature* SKYROOT_WOODLAND_TREES;

    static PlacedFeature* GRASS_PATCH;
    static PlacedFeature* TALL_GRASS_PATCH;
    static PlacedFeature* WHITE_FLOWER_PATCH;
    static PlacedFeature* PURPLE_FLOWER_PATCH;
    static PlacedFeature* BERRY_BUSH_PATCH;

    static PlacedFeature* COLD_AERCLOUD;
    static PlacedFeature* BLUE_AERCLOUD;
    static PlacedFeature* GOLDEN_AERCLOUD;

    static PlacedFeature* QUICKSOIL_SHELF;

    static PlacedFeature* CRYSTAL_ISLAND;
    static PlacedFeature* HOLIDAY_TREE;
    static PlacedFeature* WATER_LAKE;
    static PlacedFeature* WATER_SPRING;

    /**
     * Bootstrap/initialize all Aether placements (bootstraps AetherFeatures
     * first if needed). A null configured feature yields a null placement.
     */
    static void bootstrap();

    static bool isInitialized() { return s_initialized; }
};

} // namespace placement
} // namespace worldgen
} // namespace data
} // namespace minecraft

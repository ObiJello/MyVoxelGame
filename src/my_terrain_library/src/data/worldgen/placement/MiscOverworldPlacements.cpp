#include "data/worldgen/placement/MiscOverworldPlacements.h"
#include "data/worldgen/features/MiscOverworldFeatures.h"
#include "levelgen/carver/CarverConfiguration.h"
#include "world/level/block/state/BlockState.h"
#include <deque>
#include <climits>

// Reference: net/minecraft/data/worldgen/placement/MiscOverworldPlacements.java
// CRITICAL: Implement EXACTLY as Java does for 100% bit parity

namespace minecraft {
namespace data {
namespace worldgen {
namespace placement {

using namespace ::minecraft::levelgen::placement;
using namespace ::minecraft::data::worldgen::features;
using ::minecraft::levelgen::Heightmap;
using ::minecraft::levelgen::carver::UniformHeight;
using ::minecraft::levelgen::VerticalAnchor;

// Static members
bool MiscOverworldPlacements::s_initialized = false;

// =========================================================================
// STATIC MEMBER DEFINITIONS - All PlacedFeature pointers initialized to nullptr
// Reference: MiscOverworldPlacements.java lines 33-50
// =========================================================================

// Ice features
const PlacedFeature* MiscOverworldPlacements::ICE_SPIKE = nullptr;
const PlacedFeature* MiscOverworldPlacements::ICE_PATCH = nullptr;
const PlacedFeature* MiscOverworldPlacements::ICEBERG_PACKED = nullptr;
const PlacedFeature* MiscOverworldPlacements::ICEBERG_BLUE = nullptr;
const PlacedFeature* MiscOverworldPlacements::BLUE_ICE = nullptr;

// Forest rock
const PlacedFeature* MiscOverworldPlacements::FOREST_ROCK = nullptr;

// Lakes
const PlacedFeature* MiscOverworldPlacements::LAKE_LAVA_UNDERGROUND = nullptr;
const PlacedFeature* MiscOverworldPlacements::LAKE_LAVA_SURFACE = nullptr;

// Disks
const PlacedFeature* MiscOverworldPlacements::DISK_CLAY = nullptr;
const PlacedFeature* MiscOverworldPlacements::DISK_GRAVEL = nullptr;
const PlacedFeature* MiscOverworldPlacements::DISK_SAND = nullptr;
const PlacedFeature* MiscOverworldPlacements::DISK_GRASS = nullptr;

// Top layer / void
const PlacedFeature* MiscOverworldPlacements::FREEZE_TOP_LAYER = nullptr;
const PlacedFeature* MiscOverworldPlacements::VOID_START_PLATFORM = nullptr;

// Desert well
const PlacedFeature* MiscOverworldPlacements::DESERT_WELL = nullptr;

// Springs
const PlacedFeature* MiscOverworldPlacements::SPRING_LAVA = nullptr;
const PlacedFeature* MiscOverworldPlacements::SPRING_LAVA_FROZEN = nullptr;
const PlacedFeature* MiscOverworldPlacements::SPRING_WATER = nullptr;

// =========================================================================
// STORAGE - Use deque to avoid pointer invalidation
// =========================================================================
static std::deque<PlacedFeature> s_placedFeatures;
static std::deque<CountPlacement> s_countPlacements;
static std::deque<HeightmapPlacement> s_heightmapPlacements;
static std::deque<HeightRangePlacement> s_heightRangePlacements;
static std::deque<RarityFilter> s_rarityFilters;
static std::deque<UniformHeight> s_uniformHeights;
static std::deque<BlockPredicateFilter> s_blockPredicateFilters;
static std::deque<levelgen::carver::UniformInt> s_uniformInts;
static std::deque<EnvironmentScanPlacement> s_envScanPlacements;
static std::deque<SurfaceRelativeThresholdFilter> s_surfaceFilters;

// Static predicate functions for EnvironmentScanPlacement
// Reference: BlockPredicate.not(BlockPredicate.ONLY_IN_AIR_PREDICATE) = any non-air block
static bool notAirBlockCheck(BlockState* state) {
    return state && !state->isAir();
}

// Reference: BlockPredicate.alwaysTrue() - always returns true
static bool alwaysTrueCheck(BlockState* state) {
    return true;
}

void MiscOverworldPlacements::bootstrap() {
    if (s_initialized) return;

    // Ensure features are bootstrapped first
    if (!MiscOverworldFeatures::isInitialized()) {
        MiscOverworldFeatures::bootstrap();
    }

    // Helper to create PlacedFeature and store it (with name for logging)
    auto createPlaced = [](levelgen::ConfiguredFeature* feature, std::vector<PlacementModifier*> modifiers, const std::string& name = "") -> const PlacedFeature* {
        if (feature == nullptr) {
            return nullptr;
        }
        s_placedFeatures.emplace_back(feature, modifiers, name);
        return &s_placedFeatures.back();
    };

    // Helper for HEIGHTMAP (MOTION_BLOCKING)
    auto heightmapMotionBlocking = []() -> PlacementModifier* {
        s_heightmapPlacements.push_back(HeightmapPlacement::onHeightmap(Heightmap::Types::MOTION_BLOCKING));
        return &s_heightmapPlacements.back();
    };

    // Helper for HEIGHTMAP_TOP_SOLID (OCEAN_FLOOR_WG)
    auto heightmapOceanFloor = []() -> PlacementModifier* {
        s_heightmapPlacements.push_back(HeightmapPlacement::onHeightmap(Heightmap::Types::OCEAN_FLOOR_WG));
        return &s_heightmapPlacements.back();
    };

    // Helper for BlockPredicateFilter that checks for water fluid at position
    // Reference: BlockPredicateFilter.forPredicate(BlockPredicate.matchesFluids(Fluids.WATER))
    auto waterPredicateFilter = []() -> PlacementModifier* {
        s_blockPredicateFilters.push_back(BlockPredicateFilter::hasSturdyFace(
            [](const PlacementContext& context, const core::BlockPos& pos) -> bool {
                BlockState* state = context.getBlockState(pos);
                if (state == nullptr) return false;
                // Check if the block is water
                return state->getIdentifier() == "minecraft:water";
            }
        ));
        return &s_blockPredicateFilters.back();
    };

    // =========================================================================
    // ICE_SPIKE PLACEMENT
    // Reference: Java line 71
    // CountPlacement.of(3), InSquarePlacement.spread(), HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::of(3));
        ICE_SPIKE = createPlaced(
            MiscOverworldFeatures::ICE_SPIKE,
            { &s_countPlacements.back(), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
            "ICE_SPIKE"
        );
    }

    // =========================================================================
    // ICE_PATCH PLACEMENT
    // Reference: Java line 72
    // CountPlacement.of(2), InSquarePlacement.spread(), HEIGHTMAP, RandomOffsetPlacement, BlockPredicateFilter, BiomeFilter
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::of(2));
        ICE_PATCH = createPlaced(
            MiscOverworldFeatures::ICE_PATCH,
            { &s_countPlacements.back(), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
            "ICE_PATCH"
        );
    }

    // =========================================================================
    // FOREST_ROCK PLACEMENT
    // Reference: Java line 73
    // CountPlacement.of(2), InSquarePlacement.spread(), HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::of(2));
        FOREST_ROCK = createPlaced(
            MiscOverworldFeatures::FOREST_ROCK,
            { &s_countPlacements.back(), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
            "FOREST_ROCK"
        );
    }

    // =========================================================================
    // ICEBERG_BLUE PLACEMENT
    // Reference: Java line 74
    // RarityFilter.onAverageOnceEvery(200), InSquarePlacement.spread(), BiomeFilter.biome()
    // =========================================================================
    {
        s_rarityFilters.push_back(RarityFilter::onAverageOnceEvery(200));
        ICEBERG_BLUE = createPlaced(
            MiscOverworldFeatures::ICEBERG_BLUE,
            { &s_rarityFilters.back(), &InSquarePlacement::spread(), &BiomeFilter::biome() },
            "ICEBERG_BLUE"
        );
    }

    // =========================================================================
    // ICEBERG_PACKED PLACEMENT
    // Reference: Java line 75
    // RarityFilter.onAverageOnceEvery(16), InSquarePlacement.spread(), BiomeFilter.biome()
    // =========================================================================
    {
        s_rarityFilters.push_back(RarityFilter::onAverageOnceEvery(16));
        ICEBERG_PACKED = createPlaced(
            MiscOverworldFeatures::ICEBERG_PACKED,
            { &s_rarityFilters.back(), &InSquarePlacement::spread(), &BiomeFilter::biome() },
            "ICEBERG_PACKED"
        );
    }

    // =========================================================================
    // BLUE_ICE PLACEMENT
    // Reference: Java line 76
    // CountPlacement.of(UniformInt.of(0, 19)), InSquarePlacement.spread(),
    // HeightRangePlacement.uniform(absolute(30), absolute(61)), BiomeFilter
    // =========================================================================
    {
        // CountPlacement: 0-19 attempts (random range, not fixed average)
        // Reference: CountPlacement.of(UniformInt.of(0, 19))
        s_uniformInts.push_back(levelgen::carver::UniformInt::of(0, 19));
        s_countPlacements.push_back(CountPlacement::of(&s_uniformInts.back()));
        s_uniformHeights.push_back(UniformHeight(
            VerticalAnchor::absolute(30),
            VerticalAnchor::absolute(61)
        ));
        s_heightRangePlacements.push_back(HeightRangePlacement::of(&s_uniformHeights.back()));

        BLUE_ICE = createPlaced(
            MiscOverworldFeatures::BLUE_ICE,
            { &s_countPlacements.back(), &InSquarePlacement::spread(), &s_heightRangePlacements.back(), &BiomeFilter::biome() },
            "BLUE_ICE"
        );
    }

    // =========================================================================
    // LAKE_LAVA_UNDERGROUND PLACEMENT
    // Reference: Java line 77
    // RarityFilter.onAverageOnceEvery(9), InSquarePlacement.spread(),
    // HeightRangePlacement.of(UniformHeight.of(absolute(0), top())),
    // EnvironmentScanPlacement.scanningFor(DOWN, allOf(not(ONLY_IN_AIR), insideWorld(0,-5,0)), 32),
    // SurfaceRelativeThresholdFilter.of(OCEAN_FLOOR_WG, MIN_VALUE, -5),
    // BiomeFilter.biome()
    // =========================================================================
    {
        s_rarityFilters.push_back(RarityFilter::onAverageOnceEvery(9));

        // HeightRangePlacement.of(UniformHeight.of(VerticalAnchor.absolute(0), VerticalAnchor.top()))
        // top() = Y level 320 (max build height)
        s_uniformHeights.push_back(UniformHeight(
            VerticalAnchor::absolute(0),
            VerticalAnchor::top()
        ));
        s_heightRangePlacements.push_back(HeightRangePlacement::of(&s_uniformHeights.back()));

        // EnvironmentScanPlacement.scanningFor(Direction.DOWN,
        //     BlockPredicate.allOf(not(ONLY_IN_AIR_PREDICATE), insideWorld(BlockPos(0, -5, 0))), 32)
        // Target: any non-air block (insideWorld check is handled by bounds checking in getPositions)
        // Allowed: alwaysTrue() (default for 3-param version)
        s_envScanPlacements.push_back(EnvironmentScanPlacement::scanningFor(
            EnvironmentScanPlacement::Direction::DOWN,
            notAirBlockCheck,
            alwaysTrueCheck,
            32
        ));

        // SurfaceRelativeThresholdFilter.of(Heightmap.Types.OCEAN_FLOOR_WG, Integer.MIN_VALUE, -5)
        // Only place if Y is at least 5 blocks below ocean floor
        s_surfaceFilters.push_back(SurfaceRelativeThresholdFilter::of(
            Heightmap::Types::OCEAN_FLOOR_WG,
            INT32_MIN,
            -5
        ));

        LAKE_LAVA_UNDERGROUND = createPlaced(
            MiscOverworldFeatures::LAKE_LAVA,
            {
                &s_rarityFilters.back(),
                &InSquarePlacement::spread(),
                &s_heightRangePlacements.back(),
                &s_envScanPlacements.back(),
                &s_surfaceFilters.back(),
                &BiomeFilter::biome()
            },
            "LAKE_LAVA_UNDERGROUND"
        );
    }

    // =========================================================================
    // LAKE_LAVA_SURFACE PLACEMENT
    // Reference: Java line 78
    // RarityFilter.onAverageOnceEvery(200), InSquarePlacement.spread(), HEIGHTMAP_WORLD_SURFACE, BiomeFilter
    // =========================================================================
    {
        s_rarityFilters.push_back(RarityFilter::onAverageOnceEvery(200));
        s_heightmapPlacements.push_back(HeightmapPlacement::onHeightmap(Heightmap::Types::WORLD_SURFACE_WG));

        LAKE_LAVA_SURFACE = createPlaced(
            MiscOverworldFeatures::LAKE_LAVA,
            { &s_rarityFilters.back(), &InSquarePlacement::spread(), &s_heightmapPlacements.back(), &BiomeFilter::biome() },
            "LAKE_LAVA_SURFACE"
        );
    }

    // =========================================================================
    // DISK_CLAY PLACEMENT
    // Reference: Java line 79
    // InSquarePlacement.spread(), HEIGHTMAP_TOP_SOLID, BlockPredicateFilter(water), BiomeFilter
    // =========================================================================
    {
        DISK_CLAY = createPlaced(
            MiscOverworldFeatures::DISK_CLAY,
            { &InSquarePlacement::spread(), heightmapOceanFloor(), waterPredicateFilter(), &BiomeFilter::biome() },
            "DISK_CLAY"
        );
    }

    // =========================================================================
    // DISK_GRAVEL PLACEMENT
    // Reference: Java line 80
    // InSquarePlacement.spread(), HEIGHTMAP_TOP_SOLID, BlockPredicateFilter(water), BiomeFilter
    // =========================================================================
    {
        DISK_GRAVEL = createPlaced(
            MiscOverworldFeatures::DISK_GRAVEL,
            { &InSquarePlacement::spread(), heightmapOceanFloor(), waterPredicateFilter(), &BiomeFilter::biome() },
            "DISK_GRAVEL"
        );
    }

    // =========================================================================
    // DISK_SAND PLACEMENT
    // Reference: Java line 81
    // CountPlacement.of(3), InSquarePlacement.spread(), HEIGHTMAP_TOP_SOLID, BlockPredicateFilter(water), BiomeFilter
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::of(3));
        DISK_SAND = createPlaced(
            MiscOverworldFeatures::DISK_SAND,
            { &s_countPlacements.back(), &InSquarePlacement::spread(), heightmapOceanFloor(), waterPredicateFilter(), &BiomeFilter::biome() },
            "DISK_SAND"
        );
    }

    // =========================================================================
    // DISK_GRASS PLACEMENT
    // Reference: Java line 82
    // CountPlacement.of(1), InSquarePlacement.spread(), HEIGHTMAP_TOP_SOLID, RandomOffsetPlacement, BlockPredicateFilter(mud), BiomeFilter
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::of(1));
        DISK_GRASS = createPlaced(
            MiscOverworldFeatures::DISK_GRASS,
            { &s_countPlacements.back(), &InSquarePlacement::spread(), heightmapOceanFloor(), &BiomeFilter::biome() },
            "DISK_GRASS"
        );
    }

    // =========================================================================
    // FREEZE_TOP_LAYER PLACEMENT
    // Reference: Java line 83
    // Just BiomeFilter - runs once per chunk
    // =========================================================================
    {
        FREEZE_TOP_LAYER = createPlaced(
            MiscOverworldFeatures::FREEZE_TOP_LAYER,
            { &BiomeFilter::biome() },
            "FREEZE_TOP_LAYER"
        );
    }

    // =========================================================================
    // VOID_START_PLATFORM PLACEMENT
    // Reference: Java line 84
    // Just BiomeFilter
    // =========================================================================
    {
        VOID_START_PLATFORM = createPlaced(
            MiscOverworldFeatures::VOID_START_PLATFORM,
            { &BiomeFilter::biome() },
            "VOID_START_PLATFORM"
        );
    }

    // =========================================================================
    // DESERT_WELL PLACEMENT
    // Reference: Java line 85
    // RarityFilter.onAverageOnceEvery(1000), InSquarePlacement.spread(), HEIGHTMAP, BiomeFilter
    // =========================================================================
    {
        s_rarityFilters.push_back(RarityFilter::onAverageOnceEvery(1000));
        DESERT_WELL = createPlaced(
            MiscOverworldFeatures::DESERT_WELL,
            { &s_rarityFilters.back(), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
            "DESERT_WELL"
        );
    }

    // =========================================================================
    // SPRING_LAVA PLACEMENT
    // Reference: Java line 86
    // CountPlacement.of(20), InSquarePlacement.spread(),
    // HeightRangePlacement.of(VeryBiasedToBottomHeight...), BiomeFilter
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::of(20));
        // VeryBiasedToBottomHeight - using uniform for now
        s_uniformHeights.push_back(UniformHeight(
            VerticalAnchor::bottom(),
            VerticalAnchor::belowTop(8)
        ));
        s_heightRangePlacements.push_back(HeightRangePlacement::of(&s_uniformHeights.back()));

        SPRING_LAVA = createPlaced(
            MiscOverworldFeatures::SPRING_LAVA_OVERWORLD,
            { &s_countPlacements.back(), &InSquarePlacement::spread(), &s_heightRangePlacements.back(), &BiomeFilter::biome() },
            "SPRING_LAVA"
        );
    }

    // =========================================================================
    // SPRING_LAVA_FROZEN PLACEMENT
    // Reference: Java line 87
    // Same as SPRING_LAVA but for frozen biomes
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::of(20));
        s_uniformHeights.push_back(UniformHeight(
            VerticalAnchor::bottom(),
            VerticalAnchor::belowTop(8)
        ));
        s_heightRangePlacements.push_back(HeightRangePlacement::of(&s_uniformHeights.back()));

        SPRING_LAVA_FROZEN = createPlaced(
            MiscOverworldFeatures::SPRING_LAVA_FROZEN,
            { &s_countPlacements.back(), &InSquarePlacement::spread(), &s_heightRangePlacements.back(), &BiomeFilter::biome() },
            "SPRING_LAVA_FROZEN"
        );
    }

    // =========================================================================
    // SPRING_WATER PLACEMENT
    // Reference: Java line 88
    // CountPlacement.of(25), InSquarePlacement.spread(),
    // HeightRangePlacement.uniform(bottom(), absolute(192)), BiomeFilter
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::of(25));
        s_uniformHeights.push_back(UniformHeight(
            VerticalAnchor::bottom(),
            VerticalAnchor::absolute(192)
        ));
        s_heightRangePlacements.push_back(HeightRangePlacement::of(&s_uniformHeights.back()));

        SPRING_WATER = createPlaced(
            MiscOverworldFeatures::SPRING_WATER,
            { &s_countPlacements.back(), &InSquarePlacement::spread(), &s_heightRangePlacements.back(), &BiomeFilter::biome() },
            "SPRING_WATER"
        );
    }

    s_initialized = true;

    // Debug: verify pointers at end of bootstrap
}

} // namespace placement
} // namespace worldgen
} // namespace data
} // namespace minecraft

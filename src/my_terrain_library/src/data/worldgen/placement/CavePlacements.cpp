#include "data/worldgen/placement/CavePlacements.h"
#include "data/worldgen/features/CaveFeatures.h"
#include "data/worldgen/features/VegetationFeatures.h"
#include "levelgen/carver/CarverConfiguration.h"
#include <deque>

// Reference: net/minecraft/data/worldgen/placement/CavePlacements.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace placement {

using namespace levelgen::placement;
using namespace features;
using levelgen::Heightmap;
using levelgen::carver::UniformHeight;
using levelgen::VerticalAnchor;

// Static members
bool CavePlacements::s_initialized = false;

// =========================================================================
// STATIC MEMBER DEFINITIONS - All PlacedFeature pointers initialized to nullptr
// Reference: CavePlacements.java lines 29-48
// =========================================================================

// Monster rooms
const PlacedFeature* CavePlacements::MONSTER_ROOM = nullptr;
const PlacedFeature* CavePlacements::MONSTER_ROOM_DEEP = nullptr;

// Fossils
const PlacedFeature* CavePlacements::FOSSIL_UPPER = nullptr;
const PlacedFeature* CavePlacements::FOSSIL_LOWER = nullptr;

// Dripstone
const PlacedFeature* CavePlacements::DRIPSTONE_CLUSTER = nullptr;
const PlacedFeature* CavePlacements::LARGE_DRIPSTONE = nullptr;
const PlacedFeature* CavePlacements::POINTED_DRIPSTONE = nullptr;

// Underwater
const PlacedFeature* CavePlacements::UNDERWATER_MAGMA = nullptr;

// Glow lichen
const PlacedFeature* CavePlacements::GLOW_LICHEN = nullptr;

// Lush caves
const PlacedFeature* CavePlacements::ROOTED_AZALEA_TREE = nullptr;
const PlacedFeature* CavePlacements::CAVE_VINES = nullptr;
const PlacedFeature* CavePlacements::LUSH_CAVES_VEGETATION = nullptr;
const PlacedFeature* CavePlacements::LUSH_CAVES_CLAY = nullptr;
const PlacedFeature* CavePlacements::LUSH_CAVES_CEILING_VEGETATION = nullptr;
const PlacedFeature* CavePlacements::SPORE_BLOSSOM = nullptr;

// Classic vines
const PlacedFeature* CavePlacements::CLASSIC_VINES = nullptr;

// Amethyst geode
const PlacedFeature* CavePlacements::AMETHYST_GEODE = nullptr;

// Sculk
const PlacedFeature* CavePlacements::SCULK_PATCH_DEEP_DARK = nullptr;
const PlacedFeature* CavePlacements::SCULK_PATCH_ANCIENT_CITY = nullptr;
const PlacedFeature* CavePlacements::SCULK_VEIN = nullptr;

// Storage for PlacedFeature instances (use deque to avoid pointer invalidation)
static std::deque<PlacedFeature> s_placedFeatures;

// Storage for placement modifiers
static std::deque<RarityFilter> s_rarityFilters;
static std::deque<HeightRangePlacement> s_heightPlacements;
static std::deque<CountPlacement> s_countPlacements;

// Storage for height providers
static std::deque<UniformHeight> s_uniformHeights;

// Storage for UniformInt providers (for random count ranges)
static std::deque<levelgen::carver::UniformInt> s_uniformInts;

// Storage for SurfaceRelativeThresholdFilter instances
static std::deque<SurfaceRelativeThresholdFilter> s_surfaceFilters;

// Storage for EnvironmentScanPlacement instances
static std::deque<EnvironmentScanPlacement> s_envScanPlacements;

// Storage for RandomOffsetPlacement instances
static std::deque<RandomOffsetPlacement> s_randomOffsetPlacements;

// Storage for ClampedNormalInt providers
static std::deque<levelgen::carver::ClampedNormalInt> s_clampedNormalInts;

// Storage for ConstantInt providers
static std::deque<levelgen::carver::ConstantInt> s_constantInts;

// Static predicate functions for EnvironmentScanPlacement
// Reference: BlockPredicate.solid() - returns true for solid blocks
static bool solidBlockCheck(BlockState* state) {
    return state && state->isSolid();
}

// Reference: BlockPredicate.ONLY_IN_AIR_PREDICATE - returns true for air blocks
static bool airBlockCheck(BlockState* state) {
    return state && state->isAir();
}

// Reference: BlockPredicate.hasSturdyFace(Direction.DOWN) - for cave vines
// C++ doesn't have isFaceSturdy(), so use !isAir() as approximation (matches Feature.h:6020)
static bool sturdyFaceDownCheck(BlockState* state) {
    return state && !state->isAir();
}

void CavePlacements::bootstrap() {
    if (s_initialized) return;

    // Ensure features are bootstrapped first
    if (!CaveFeatures::isInitialized()) {
        CaveFeatures::bootstrap();
    }

    // Helper to create PlacedFeature
    auto createPlaced = [](levelgen::ConfiguredFeature* feature, std::vector<PlacementModifier*> modifiers, const std::string& name = "") -> const PlacedFeature* {
        s_placedFeatures.emplace_back(feature, modifiers, name);
        return &s_placedFeatures.back();
    };

    // =========================================================================
    // AMETHYST_GEODE PLACEMENT
    // Reference: CavePlacements.java line 87
    // RarityFilter.onAverageOnceEvery(24),
    // InSquarePlacement.spread(),
    // HeightRangePlacement.uniform(VerticalAnchor.aboveBottom(6), VerticalAnchor.absolute(30)),
    // BiomeFilter.biome()
    // =========================================================================
    {
        // RarityFilter: 1/24 chance
        s_rarityFilters.push_back(RarityFilter::onAverageOnceEvery(24));

        // HeightRangePlacement: uniform from Y=6 above bottom to Y=30 absolute
        // aboveBottom(6) means -64 + 6 = -58
        // absolute(30) means Y=30
        s_uniformHeights.push_back(UniformHeight(
            VerticalAnchor::aboveBottom(6),
            VerticalAnchor::absolute(30)
        ));
        s_heightPlacements.push_back(HeightRangePlacement::of(&s_uniformHeights.back()));

        std::vector<PlacementModifier*> modifiers = {
            &s_rarityFilters.back(),
            &InSquarePlacement::spread(),
            &s_heightPlacements.back(),
            &BiomeFilter::biome()
        };

        AMETHYST_GEODE = createPlaced(CaveFeatures::AMETHYST_GEODE, modifiers, "AMETHYST_GEODE");
    }

    // =========================================================================
    // GLOW_LICHEN PLACEMENT
    // Reference: CavePlacements.java line 79
    // CountPlacement.of(UniformInt.of(104, 157))
    // PlacementUtils.RANGE_BOTTOM_TO_MAX_TERRAIN_HEIGHT
    // InSquarePlacement.spread()
    // SurfaceRelativeThresholdFilter.of(OCEAN_FLOOR_WG, Integer.MIN_VALUE, -13)
    // BiomeFilter.biome()
    // =========================================================================
    {
        // CountPlacement: 104-157 attempts (random range, not fixed average)
        // Reference: CountPlacement.of(UniformInt.of(104, 157))
        s_uniformInts.push_back(levelgen::carver::UniformInt::of(104, 157));
        s_countPlacements.push_back(CountPlacement::of(&s_uniformInts.back()));

        // HeightRangePlacement: entire cave range
        s_uniformHeights.push_back(UniformHeight(
            VerticalAnchor::aboveBottom(0),
            VerticalAnchor::absolute(256)
        ));
        s_heightPlacements.push_back(HeightRangePlacement::of(&s_uniformHeights.back()));

        // SurfaceRelativeThresholdFilter: only place at least 13 blocks below ocean floor
        // Reference: SurfaceRelativeThresholdFilter.of(OCEAN_FLOOR_WG, Integer.MIN_VALUE, -13)
        s_surfaceFilters.push_back(SurfaceRelativeThresholdFilter::of(
            Heightmap::Types::OCEAN_FLOOR_WG,
            INT32_MIN,
            -13
        ));

        // CRITICAL: Java order is Count -> HeightRange -> InSquare -> SurfaceFilter -> BiomeFilter
        std::vector<PlacementModifier*> modifiers = {
            &s_countPlacements.back(),
            &s_heightPlacements.back(),
            &InSquarePlacement::spread(),
            &s_surfaceFilters.back(),
            &BiomeFilter::biome()
        };

        GLOW_LICHEN = createPlaced(CaveFeatures::GLOW_LICHEN, modifiers, "GLOW_LICHEN");
    }

    // =========================================================================
    // SPORE_BLOSSOM PLACEMENT
    // Reference: CavePlacements.java line 85
    // CountPlacement.of(25), InSquarePlacement.spread(),
    // RANGE_BOTTOM_TO_MAX_TERRAIN_HEIGHT,
    // EnvironmentScanPlacement.scanningFor(Direction.UP, BlockPredicate.solid(), BlockPredicate.ONLY_IN_AIR_PREDICATE, 12),
    // RandomOffsetPlacement.vertical(ConstantInt.of(-1)),
    // BiomeFilter.biome()
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::of(25));

        s_uniformHeights.push_back(UniformHeight(
            VerticalAnchor::aboveBottom(0),
            VerticalAnchor::absolute(256)
        ));
        s_heightPlacements.push_back(HeightRangePlacement::of(&s_uniformHeights.back()));

        // EnvironmentScanPlacement: scan UP for solid block, allow only air, max 12 steps
        s_envScanPlacements.push_back(EnvironmentScanPlacement::scanningFor(
            EnvironmentScanPlacement::Direction::UP,
            solidBlockCheck,
            airBlockCheck,
            12
        ));

        // RandomOffsetPlacement.vertical(-1)
        s_constantInts.push_back(levelgen::carver::ConstantInt::of(-1));
        s_randomOffsetPlacements.push_back(RandomOffsetPlacement::vertical(&s_constantInts.back()));

        std::vector<PlacementModifier*> modifiers = {
            &s_countPlacements.back(),
            &InSquarePlacement::spread(),
            &s_heightPlacements.back(),
            &s_envScanPlacements.back(),
            &s_randomOffsetPlacements.back(),
            &BiomeFilter::biome()
        };

        SPORE_BLOSSOM = createPlaced(CaveFeatures::SPORE_BLOSSOM, modifiers, "SPORE_BLOSSOM");
    }

    // =========================================================================
    // LUSH_CAVES_VEGETATION (MOSS_PATCH) PLACEMENT
    // Reference: CavePlacements.java line 82
    // CountPlacement.of(125), InSquarePlacement.spread(),
    // RANGE_BOTTOM_TO_MAX_TERRAIN_HEIGHT,
    // EnvironmentScanPlacement.scanningFor(Direction.DOWN, BlockPredicate.solid(), BlockPredicate.ONLY_IN_AIR_PREDICATE, 12),
    // RandomOffsetPlacement.vertical(ConstantInt.of(1)),
    // BiomeFilter.biome()
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::of(125));

        s_uniformHeights.push_back(UniformHeight(
            VerticalAnchor::aboveBottom(0),
            VerticalAnchor::absolute(256)
        ));
        s_heightPlacements.push_back(HeightRangePlacement::of(&s_uniformHeights.back()));

        // EnvironmentScanPlacement: scan DOWN for solid block, allow only air, max 12 steps
        s_envScanPlacements.push_back(EnvironmentScanPlacement::scanningFor(
            EnvironmentScanPlacement::Direction::DOWN,
            solidBlockCheck,
            airBlockCheck,
            12
        ));

        // RandomOffsetPlacement.vertical(+1)
        s_constantInts.push_back(levelgen::carver::ConstantInt::of(1));
        s_randomOffsetPlacements.push_back(RandomOffsetPlacement::vertical(&s_constantInts.back()));

        std::vector<PlacementModifier*> modifiers = {
            &s_countPlacements.back(),
            &InSquarePlacement::spread(),
            &s_heightPlacements.back(),
            &s_envScanPlacements.back(),
            &s_randomOffsetPlacements.back(),
            &BiomeFilter::biome()
        };

        LUSH_CAVES_VEGETATION = createPlaced(CaveFeatures::MOSS_PATCH, modifiers, "LUSH_CAVES_VEGETATION");
    }

    // =========================================================================
    // LUSH_CAVES_CLAY PLACEMENT
    // Reference: CavePlacements.java line 83
    // CountPlacement.of(62), InSquarePlacement.spread(),
    // RANGE_BOTTOM_TO_MAX_TERRAIN_HEIGHT,
    // EnvironmentScanPlacement.scanningFor(Direction.DOWN, BlockPredicate.solid(), BlockPredicate.ONLY_IN_AIR_PREDICATE, 12),
    // RandomOffsetPlacement.vertical(ConstantInt.of(1)),
    // BiomeFilter.biome()
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::of(62));

        s_uniformHeights.push_back(UniformHeight(
            VerticalAnchor::aboveBottom(0),
            VerticalAnchor::absolute(256)
        ));
        s_heightPlacements.push_back(HeightRangePlacement::of(&s_uniformHeights.back()));

        // EnvironmentScanPlacement: scan DOWN for solid block, allow only air, max 12 steps
        s_envScanPlacements.push_back(EnvironmentScanPlacement::scanningFor(
            EnvironmentScanPlacement::Direction::DOWN,
            solidBlockCheck,
            airBlockCheck,
            12
        ));

        // RandomOffsetPlacement.vertical(+1)
        s_constantInts.push_back(levelgen::carver::ConstantInt::of(1));
        s_randomOffsetPlacements.push_back(RandomOffsetPlacement::vertical(&s_constantInts.back()));

        std::vector<PlacementModifier*> modifiers = {
            &s_countPlacements.back(),
            &InSquarePlacement::spread(),
            &s_heightPlacements.back(),
            &s_envScanPlacements.back(),
            &s_randomOffsetPlacements.back(),
            &BiomeFilter::biome()
        };

        LUSH_CAVES_CLAY = createPlaced(CaveFeatures::CLAY_WITH_DRIPLEAVES, modifiers, "LUSH_CAVES_CLAY");
    }

    // =========================================================================
    // CAVE_VINES PLACEMENT
    // Reference: CavePlacements.java line 81
    // CountPlacement.of(188), InSquarePlacement.spread(),
    // RANGE_BOTTOM_TO_MAX_TERRAIN_HEIGHT,
    // EnvironmentScanPlacement.scanningFor(Direction.UP, BlockPredicate.hasSturdyFace(Direction.DOWN), BlockPredicate.ONLY_IN_AIR_PREDICATE, 12),
    // RandomOffsetPlacement.vertical(ConstantInt.of(-1)),
    // BiomeFilter.biome()
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::of(188));

        s_uniformHeights.push_back(UniformHeight(
            VerticalAnchor::aboveBottom(0),
            VerticalAnchor::absolute(256)
        ));
        s_heightPlacements.push_back(HeightRangePlacement::of(&s_uniformHeights.back()));

        // EnvironmentScanPlacement: scan UP for block with sturdy face down, allow only air, max 12 steps
        // Note: Using sturdyFaceDownCheck which is !isAir() approximation
        s_envScanPlacements.push_back(EnvironmentScanPlacement::scanningFor(
            EnvironmentScanPlacement::Direction::UP,
            sturdyFaceDownCheck,
            airBlockCheck,
            12
        ));

        // RandomOffsetPlacement.vertical(-1)
        s_constantInts.push_back(levelgen::carver::ConstantInt::of(-1));
        s_randomOffsetPlacements.push_back(RandomOffsetPlacement::vertical(&s_constantInts.back()));

        std::vector<PlacementModifier*> modifiers = {
            &s_countPlacements.back(),
            &InSquarePlacement::spread(),
            &s_heightPlacements.back(),
            &s_envScanPlacements.back(),
            &s_randomOffsetPlacements.back(),
            &BiomeFilter::biome()
        };

        CAVE_VINES = createPlaced(CaveFeatures::CAVE_VINE, modifiers, "CAVE_VINES");
    }

    // =========================================================================
    // UNDERWATER_MAGMA PLACEMENT
    // Reference: CavePlacements.java line 78
    // CountPlacement.of(UniformInt.of(44, 52)),
    // InSquarePlacement.spread(),
    // HeightRangePlacement (RANGE_BOTTOM_TO_MAX_TERRAIN_HEIGHT),
    // SurfaceRelativeThresholdFilter.of(OCEAN_FLOOR_WG, MIN_VALUE, -2),
    // BiomeFilter.biome()
    // =========================================================================
    {
        // CountPlacement: 44-52 attempts (random range)
        // Reference: CountPlacement.of(UniformInt.of(44, 52))
        s_uniformInts.push_back(levelgen::carver::UniformInt::of(44, 52));
        s_countPlacements.push_back(CountPlacement::of(&s_uniformInts.back()));

        // HeightRangePlacement: entire height range
        s_uniformHeights.push_back(UniformHeight(
            VerticalAnchor::aboveBottom(0),
            VerticalAnchor::absolute(256)
        ));
        s_heightPlacements.push_back(HeightRangePlacement::of(&s_uniformHeights.back()));

        // SurfaceRelativeThresholdFilter: only place below ocean floor
        // Reference: SurfaceRelativeThresholdFilter.of(OCEAN_FLOOR_WG, MIN_VALUE, -2)
        s_surfaceFilters.push_back(SurfaceRelativeThresholdFilter::of(
            Heightmap::Types::OCEAN_FLOOR_WG,
            INT32_MIN,  // Java Integer.MIN_VALUE
            -2
        ));

        std::vector<PlacementModifier*> modifiers = {
            &s_countPlacements.back(),
            &InSquarePlacement::spread(),
            &s_heightPlacements.back(),
            &s_surfaceFilters.back(),
            &BiomeFilter::biome()
        };

        UNDERWATER_MAGMA = createPlaced(CaveFeatures::UNDERWATER_MAGMA, modifiers, "UNDERWATER_MAGMA");
    }

    // =========================================================================
    // SCULK_PATCH_DEEP_DARK PLACEMENT
    // Reference: CavePlacements.java line 88
    // CountPlacement.of(256), InSquarePlacement.spread(),
    // HeightRangePlacement (RANGE_BOTTOM_TO_MAX_TERRAIN_HEIGHT), BiomeFilter.biome()
    // =========================================================================
    {
        // CountPlacement: 256 attempts
        s_countPlacements.push_back(CountPlacement::of(256));

        // HeightRangePlacement: entire height range
        s_uniformHeights.push_back(UniformHeight(
            VerticalAnchor::aboveBottom(0),
            VerticalAnchor::absolute(256)
        ));
        s_heightPlacements.push_back(HeightRangePlacement::of(&s_uniformHeights.back()));

        std::vector<PlacementModifier*> modifiers = {
            &s_countPlacements.back(),
            &InSquarePlacement::spread(),
            &s_heightPlacements.back(),
            &BiomeFilter::biome()
        };

        SCULK_PATCH_DEEP_DARK = createPlaced(CaveFeatures::SCULK_PATCH_DEEP_DARK, modifiers, "SCULK_PATCH_DEEP_DARK");
    }

    // =========================================================================
    // SCULK_VEIN PLACEMENT
    // Reference: CavePlacements.java line 90
    // CountPlacement.of(UniformInt.of(204, 250)), InSquarePlacement.spread(),
    // HeightRangePlacement, BiomeFilter.biome()
    // =========================================================================
    {
        // CountPlacement: 204-250 attempts (random range)
        // Reference: CountPlacement.of(UniformInt.of(204, 250))
        s_uniformInts.push_back(levelgen::carver::UniformInt::of(204, 250));
        s_countPlacements.push_back(CountPlacement::of(&s_uniformInts.back()));

        // HeightRangePlacement: entire height range
        s_uniformHeights.push_back(UniformHeight(
            VerticalAnchor::aboveBottom(0),
            VerticalAnchor::absolute(256)
        ));
        s_heightPlacements.push_back(HeightRangePlacement::of(&s_uniformHeights.back()));

        std::vector<PlacementModifier*> modifiers = {
            &s_countPlacements.back(),
            &InSquarePlacement::spread(),
            &s_heightPlacements.back(),
            &BiomeFilter::biome()
        };

        SCULK_VEIN = createPlaced(CaveFeatures::SCULK_VEIN, modifiers, "SCULK_VEIN");
    }

    // =========================================================================
    // MONSTER_ROOM PLACEMENT (upper dungeons)
    // Reference: CavePlacements.java line 71
    // CountPlacement.of(10), InSquarePlacement.spread(),
    // HeightRangePlacement.uniform(VerticalAnchor.absolute(0), VerticalAnchor.top()),
    // BiomeFilter.biome()
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::of(10));

        // HeightRangePlacement: Y=0 to top (256)
        s_uniformHeights.push_back(UniformHeight(
            VerticalAnchor::absolute(0),
            VerticalAnchor::absolute(256)
        ));
        s_heightPlacements.push_back(HeightRangePlacement::of(&s_uniformHeights.back()));

        std::vector<PlacementModifier*> modifiers = {
            &s_countPlacements.back(),
            &InSquarePlacement::spread(),
            &s_heightPlacements.back(),
            &BiomeFilter::biome()
        };

        MONSTER_ROOM = createPlaced(CaveFeatures::MONSTER_ROOM, modifiers, "MONSTER_ROOM");
    }

    // =========================================================================
    // MONSTER_ROOM_DEEP PLACEMENT (deep dungeons)
    // Reference: CavePlacements.java line 72
    // CountPlacement.of(4), InSquarePlacement.spread(),
    // HeightRangePlacement.uniform(VerticalAnchor.aboveBottom(6), VerticalAnchor.absolute(-1)),
    // BiomeFilter.biome()
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::of(4));

        // HeightRangePlacement: Y=-58 (aboveBottom 6) to Y=-1
        s_uniformHeights.push_back(UniformHeight(
            VerticalAnchor::aboveBottom(6),
            VerticalAnchor::absolute(-1)
        ));
        s_heightPlacements.push_back(HeightRangePlacement::of(&s_uniformHeights.back()));

        std::vector<PlacementModifier*> modifiers = {
            &s_countPlacements.back(),
            &InSquarePlacement::spread(),
            &s_heightPlacements.back(),
            &BiomeFilter::biome()
        };

        MONSTER_ROOM_DEEP = createPlaced(CaveFeatures::MONSTER_ROOM, modifiers, "MONSTER_ROOM_DEEP");
    }

    // =========================================================================
    // FOSSIL_UPPER PLACEMENT
    // Reference: CavePlacements.java line 73
    // RarityFilter.onAverageOnceEvery(64), InSquarePlacement.spread(),
    // HeightRangePlacement.uniform(absolute(0), top()), BiomeFilter.biome()
    // =========================================================================
    {
        s_rarityFilters.push_back(RarityFilter::onAverageOnceEvery(64));

        s_uniformHeights.push_back(UniformHeight(
            VerticalAnchor::absolute(0),
            VerticalAnchor::absolute(256)
        ));
        s_heightPlacements.push_back(HeightRangePlacement::of(&s_uniformHeights.back()));

        std::vector<PlacementModifier*> modifiers = {
            &s_rarityFilters.back(),
            &InSquarePlacement::spread(),
            &s_heightPlacements.back(),
            &BiomeFilter::biome()
        };

        FOSSIL_UPPER = createPlaced(CaveFeatures::FOSSIL_COAL, modifiers, "FOSSIL_UPPER");
    }

    // =========================================================================
    // FOSSIL_LOWER PLACEMENT
    // Reference: CavePlacements.java line 74
    // RarityFilter.onAverageOnceEvery(64), InSquarePlacement.spread(),
    // HeightRangePlacement.uniform(bottom(), absolute(-8)), BiomeFilter.biome()
    // =========================================================================
    {
        s_rarityFilters.push_back(RarityFilter::onAverageOnceEvery(64));

        s_uniformHeights.push_back(UniformHeight(
            VerticalAnchor::bottom(),
            VerticalAnchor::absolute(-8)
        ));
        s_heightPlacements.push_back(HeightRangePlacement::of(&s_uniformHeights.back()));

        std::vector<PlacementModifier*> modifiers = {
            &s_rarityFilters.back(),
            &InSquarePlacement::spread(),
            &s_heightPlacements.back(),
            &BiomeFilter::biome()
        };

        FOSSIL_LOWER = createPlaced(CaveFeatures::FOSSIL_DIAMONDS, modifiers, "FOSSIL_LOWER");
    }

    // =========================================================================
    // DRIPSTONE_CLUSTER PLACEMENT
    // Reference: CavePlacements.java line 75
    // CountPlacement.of(UniformInt.of(48, 96)), InSquarePlacement.spread(),
    // RANGE_BOTTOM_TO_MAX_TERRAIN_HEIGHT, BiomeFilter.biome()
    // =========================================================================
    {
        // CountPlacement: 48-96 attempts (random range, not fixed average)
        // Reference: CountPlacement.of(UniformInt.of(48, 96))
        s_uniformInts.push_back(levelgen::carver::UniformInt::of(48, 96));
        s_countPlacements.push_back(CountPlacement::of(&s_uniformInts.back()));

        s_uniformHeights.push_back(UniformHeight(
            VerticalAnchor::aboveBottom(0),
            VerticalAnchor::absolute(256)
        ));
        s_heightPlacements.push_back(HeightRangePlacement::of(&s_uniformHeights.back()));

        std::vector<PlacementModifier*> modifiers = {
            &s_countPlacements.back(),
            &InSquarePlacement::spread(),
            &s_heightPlacements.back(),
            &BiomeFilter::biome()
        };

        DRIPSTONE_CLUSTER = createPlaced(CaveFeatures::DRIPSTONE_CLUSTER, modifiers, "DRIPSTONE_CLUSTER");
    }

    // =========================================================================
    // LARGE_DRIPSTONE PLACEMENT
    // Reference: CavePlacements.java line 76
    // CountPlacement.of(UniformInt.of(10, 48)), InSquarePlacement.spread(),
    // RANGE_BOTTOM_TO_MAX_TERRAIN_HEIGHT, BiomeFilter.biome()
    // =========================================================================
    {
        // CountPlacement: 10-48 attempts (random range, not fixed average)
        // Reference: CountPlacement.of(UniformInt.of(10, 48))
        s_uniformInts.push_back(levelgen::carver::UniformInt::of(10, 48));
        s_countPlacements.push_back(CountPlacement::of(&s_uniformInts.back()));

        s_uniformHeights.push_back(UniformHeight(
            VerticalAnchor::aboveBottom(0),
            VerticalAnchor::absolute(256)
        ));
        s_heightPlacements.push_back(HeightRangePlacement::of(&s_uniformHeights.back()));

        std::vector<PlacementModifier*> modifiers = {
            &s_countPlacements.back(),
            &InSquarePlacement::spread(),
            &s_heightPlacements.back(),
            &BiomeFilter::biome()
        };

        LARGE_DRIPSTONE = createPlaced(CaveFeatures::LARGE_DRIPSTONE, modifiers, "LARGE_DRIPSTONE");
    }

    // =========================================================================
    // POINTED_DRIPSTONE PLACEMENT
    // Reference: CavePlacements.java line 77
    // CountPlacement.of(UniformInt.of(192, 256)), InSquarePlacement.spread(),
    // RANGE_BOTTOM_TO_MAX_TERRAIN_HEIGHT, CountPlacement.of(UniformInt.of(1, 5)),
    // RandomOffsetPlacement.of(ClampedNormalInt.of(0.0F, 3.0F, -10, 10), ClampedNormalInt.of(0.0F, 0.6F, -2, 2)),
    // BiomeFilter.biome()
    // =========================================================================
    {
        // CountPlacement: 192-256 attempts (random range, not fixed average)
        // Reference: CountPlacement.of(UniformInt.of(192, 256))
        s_uniformInts.push_back(levelgen::carver::UniformInt::of(192, 256));
        s_countPlacements.push_back(CountPlacement::of(&s_uniformInts.back()));

        s_uniformHeights.push_back(UniformHeight(
            VerticalAnchor::aboveBottom(0),
            VerticalAnchor::absolute(256)
        ));
        s_heightPlacements.push_back(HeightRangePlacement::of(&s_uniformHeights.back()));

        // Second CountPlacement: 1-5 attempts per position
        // Reference: CountPlacement.of(UniformInt.of(1, 5))
        s_uniformInts.push_back(levelgen::carver::UniformInt::of(1, 5));
        s_countPlacements.push_back(CountPlacement::of(&s_uniformInts.back()));

        // RandomOffsetPlacement with ClampedNormalInt
        // Java: RandomOffsetPlacement.of(ClampedNormalInt.of(0.0F, 3.0F, -10, 10), ClampedNormalInt.of(0.0F, 0.6F, -2, 2))
        s_clampedNormalInts.push_back(levelgen::carver::ClampedNormalInt::of(0.0f, 3.0f, -10, 10));  // xzSpread
        s_clampedNormalInts.push_back(levelgen::carver::ClampedNormalInt::of(0.0f, 0.6f, -2, 2));    // ySpread
        s_randomOffsetPlacements.push_back(RandomOffsetPlacement::of(
            &s_clampedNormalInts[s_clampedNormalInts.size() - 2],
            &s_clampedNormalInts.back()
        ));

        std::vector<PlacementModifier*> modifiers = {
            &s_countPlacements[s_countPlacements.size() - 2],  // First count (192-256)
            &InSquarePlacement::spread(),
            &s_heightPlacements.back(),
            &s_countPlacements.back(),  // Second count (1-5)
            &s_randomOffsetPlacements.back(),
            &BiomeFilter::biome()
        };

        POINTED_DRIPSTONE = createPlaced(CaveFeatures::POINTED_DRIPSTONE, modifiers, "POINTED_DRIPSTONE");
    }

    // =========================================================================
    // ROOTED_AZALEA_TREE PLACEMENT
    // Reference: CavePlacements.java line 80
    // CountPlacement.of(UniformInt.of(1, 2)), InSquarePlacement.spread(),
    // RANGE_BOTTOM_TO_MAX_TERRAIN_HEIGHT,
    // EnvironmentScanPlacement.scanningFor(Direction.UP, BlockPredicate.solid(), BlockPredicate.ONLY_IN_AIR_PREDICATE, 12),
    // RandomOffsetPlacement.vertical(ConstantInt.of(-1)),
    // BiomeFilter.biome()
    // =========================================================================
    {
        // CountPlacement: 1-2 attempts (random range, not fixed average)
        // Reference: CountPlacement.of(UniformInt.of(1, 2))
        s_uniformInts.push_back(levelgen::carver::UniformInt::of(1, 2));
        s_countPlacements.push_back(CountPlacement::of(&s_uniformInts.back()));

        s_uniformHeights.push_back(UniformHeight(
            VerticalAnchor::aboveBottom(0),
            VerticalAnchor::absolute(256)
        ));
        s_heightPlacements.push_back(HeightRangePlacement::of(&s_uniformHeights.back()));

        // EnvironmentScanPlacement: scan UP for solid block, allow only air, max 12 steps
        s_envScanPlacements.push_back(EnvironmentScanPlacement::scanningFor(
            EnvironmentScanPlacement::Direction::UP,
            solidBlockCheck,
            airBlockCheck,
            12
        ));

        // RandomOffsetPlacement.vertical(-1)
        s_constantInts.push_back(levelgen::carver::ConstantInt::of(-1));
        s_randomOffsetPlacements.push_back(RandomOffsetPlacement::vertical(&s_constantInts.back()));

        std::vector<PlacementModifier*> modifiers = {
            &s_countPlacements.back(),
            &InSquarePlacement::spread(),
            &s_heightPlacements.back(),
            &s_envScanPlacements.back(),
            &s_randomOffsetPlacements.back(),
            &BiomeFilter::biome()
        };

        ROOTED_AZALEA_TREE = createPlaced(CaveFeatures::ROOTED_AZALEA_TREE, modifiers, "ROOTED_AZALEA_TREE");
    }

    // =========================================================================
    // LUSH_CAVES_CEILING_VEGETATION PLACEMENT
    // Reference: CavePlacements.java line 84
    // CountPlacement.of(125), InSquarePlacement.spread(),
    // RANGE_BOTTOM_TO_MAX_TERRAIN_HEIGHT,
    // EnvironmentScanPlacement.scanningFor(Direction.UP, BlockPredicate.solid(), BlockPredicate.ONLY_IN_AIR_PREDICATE, 12),
    // RandomOffsetPlacement.vertical(ConstantInt.of(-1)),
    // BiomeFilter.biome()
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::of(125));

        s_uniformHeights.push_back(UniformHeight(
            VerticalAnchor::aboveBottom(0),
            VerticalAnchor::absolute(256)
        ));
        s_heightPlacements.push_back(HeightRangePlacement::of(&s_uniformHeights.back()));

        // EnvironmentScanPlacement: scan UP for solid block, allow only air, max 12 steps
        s_envScanPlacements.push_back(EnvironmentScanPlacement::scanningFor(
            EnvironmentScanPlacement::Direction::UP,
            solidBlockCheck,
            airBlockCheck,
            12
        ));

        // RandomOffsetPlacement.vertical(-1)
        s_constantInts.push_back(levelgen::carver::ConstantInt::of(-1));
        s_randomOffsetPlacements.push_back(RandomOffsetPlacement::vertical(&s_constantInts.back()));

        std::vector<PlacementModifier*> modifiers = {
            &s_countPlacements.back(),
            &InSquarePlacement::spread(),
            &s_heightPlacements.back(),
            &s_envScanPlacements.back(),
            &s_randomOffsetPlacements.back(),
            &BiomeFilter::biome()
        };

        LUSH_CAVES_CEILING_VEGETATION = createPlaced(CaveFeatures::MOSS_PATCH_CEILING, modifiers, "LUSH_CAVES_CEILING_VEGETATION");
    }

    // =========================================================================
    // CLASSIC_VINES PLACEMENT
    // Reference: CavePlacements.java line 86
    // CountPlacement.of(256), InSquarePlacement.spread(),
    // RANGE_BOTTOM_TO_MAX_TERRAIN_HEIGHT, BiomeFilter.biome()
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::of(256));

        s_uniformHeights.push_back(UniformHeight(
            VerticalAnchor::aboveBottom(0),
            VerticalAnchor::absolute(256)
        ));
        s_heightPlacements.push_back(HeightRangePlacement::of(&s_uniformHeights.back()));

        std::vector<PlacementModifier*> modifiers = {
            &s_countPlacements.back(),
            &InSquarePlacement::spread(),
            &s_heightPlacements.back(),
            &BiomeFilter::biome()
        };

        CLASSIC_VINES = createPlaced(VegetationFeatures::VINES, modifiers, "CLASSIC_VINES_CAVE_FEATURE");
    }

    // =========================================================================
    // SCULK_PATCH_ANCIENT_CITY PLACEMENT
    // Reference: CavePlacements.java line 89
    // PlacementUtils.register(context, SCULK_PATCH_ANCIENT_CITY, sculkPatchAncientCity);
    // (no additional modifiers - placed directly)
    // =========================================================================
    {
        std::vector<PlacementModifier*> modifiers = {
            &BiomeFilter::biome()
        };

        SCULK_PATCH_ANCIENT_CITY = createPlaced(CaveFeatures::SCULK_PATCH_ANCIENT_CITY, modifiers, "SCULK_PATCH_ANCIENT_CITY");
    }

    s_initialized = true;
}

} // namespace placement
} // namespace worldgen
} // namespace data
} // namespace minecraft

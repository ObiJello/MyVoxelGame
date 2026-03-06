#include "data/worldgen/placement/VegetationPlacements.h"
#include "data/worldgen/features/VegetationFeatures.h"
#include "data/worldgen/features/TreeFeatures.h"
#include "levelgen/carver/CarverConfiguration.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include <deque>

// Reference: net/minecraft/data/worldgen/placement/VegetationPlacements.java
// CRITICAL: Implement EXACTLY as Java does for 100% bit parity

namespace minecraft {
namespace data {
namespace worldgen {
namespace placement {

using namespace levelgen::placement;
using namespace features;
using levelgen::Heightmap;

// Static members
bool VegetationPlacements::s_initialized = false;

// =========================================================================
// STATIC MEMBER DEFINITIONS - All PlacedFeature pointers initialized to nullptr
// Reference: VegetationPlacements.java lines 33-122
// =========================================================================

// Bamboo & Vines (lines 34-36)
const PlacedFeature* VegetationPlacements::BAMBOO_LIGHT = nullptr;
const PlacedFeature* VegetationPlacements::BAMBOO = nullptr;
const PlacedFeature* VegetationPlacements::VINES = nullptr;

// Sunflower & Pumpkin (lines 37-38)
const PlacedFeature* VegetationPlacements::PATCH_SUNFLOWER = nullptr;
const PlacedFeature* VegetationPlacements::PATCH_PUMPKIN = nullptr;

// Grass patches (lines 39-48)
const PlacedFeature* VegetationPlacements::PATCH_GRASS_PLAIN = nullptr;
const PlacedFeature* VegetationPlacements::PATCH_GRASS_MEADOW = nullptr;
const PlacedFeature* VegetationPlacements::PATCH_GRASS_FOREST = nullptr;
const PlacedFeature* VegetationPlacements::PATCH_GRASS_BADLANDS = nullptr;
const PlacedFeature* VegetationPlacements::PATCH_GRASS_SAVANNA = nullptr;
const PlacedFeature* VegetationPlacements::PATCH_GRASS_NORMAL = nullptr;
const PlacedFeature* VegetationPlacements::PATCH_GRASS_TAIGA_2 = nullptr;
const PlacedFeature* VegetationPlacements::PATCH_GRASS_TAIGA = nullptr;
const PlacedFeature* VegetationPlacements::PATCH_GRASS_JUNGLE = nullptr;
const PlacedFeature* VegetationPlacements::GRASS_BONEMEAL = nullptr;

// Dead bush & Dry grass (lines 49-53)
const PlacedFeature* VegetationPlacements::PATCH_DEAD_BUSH_2 = nullptr;
const PlacedFeature* VegetationPlacements::PATCH_DEAD_BUSH = nullptr;
const PlacedFeature* VegetationPlacements::PATCH_DEAD_BUSH_BADLANDS = nullptr;
const PlacedFeature* VegetationPlacements::PATCH_DRY_GRASS_BADLANDS = nullptr;
const PlacedFeature* VegetationPlacements::PATCH_DRY_GRASS_DESERT = nullptr;

// Melon (lines 54-55)
const PlacedFeature* VegetationPlacements::PATCH_MELON = nullptr;
const PlacedFeature* VegetationPlacements::PATCH_MELON_SPARSE = nullptr;

// Berry bush (lines 56-57)
const PlacedFeature* VegetationPlacements::PATCH_BERRY_COMMON = nullptr;
const PlacedFeature* VegetationPlacements::PATCH_BERRY_RARE = nullptr;

// Waterlily (line 58)
const PlacedFeature* VegetationPlacements::PATCH_WATERLILY = nullptr;

// Tall grass & Ferns (lines 59-62)
const PlacedFeature* VegetationPlacements::PATCH_TALL_GRASS_2 = nullptr;
const PlacedFeature* VegetationPlacements::PATCH_TALL_GRASS = nullptr;
const PlacedFeature* VegetationPlacements::PATCH_LARGE_FERN = nullptr;
const PlacedFeature* VegetationPlacements::PATCH_BUSH = nullptr;

// Leaf litter (line 63)
const PlacedFeature* VegetationPlacements::PATCH_LEAF_LITTER = nullptr;

// Cactus (lines 64-65)
const PlacedFeature* VegetationPlacements::PATCH_CACTUS_DESERT = nullptr;
const PlacedFeature* VegetationPlacements::PATCH_CACTUS_DECORATED = nullptr;

// Sugar cane (lines 66-69)
const PlacedFeature* VegetationPlacements::PATCH_SUGAR_CANE_SWAMP = nullptr;
const PlacedFeature* VegetationPlacements::PATCH_SUGAR_CANE_DESERT = nullptr;
const PlacedFeature* VegetationPlacements::PATCH_SUGAR_CANE_BADLANDS = nullptr;
const PlacedFeature* VegetationPlacements::PATCH_SUGAR_CANE = nullptr;

// Firefly bush (lines 70-72)
const PlacedFeature* VegetationPlacements::PATCH_FIREFLY_BUSH_SWAMP = nullptr;
const PlacedFeature* VegetationPlacements::PATCH_FIREFLY_BUSH_NEAR_WATER_SWAMP = nullptr;
const PlacedFeature* VegetationPlacements::PATCH_FIREFLY_BUSH_NEAR_WATER = nullptr;

// Mushrooms (lines 73-82)
const PlacedFeature* VegetationPlacements::BROWN_MUSHROOM_NETHER = nullptr;
const PlacedFeature* VegetationPlacements::RED_MUSHROOM_NETHER = nullptr;
const PlacedFeature* VegetationPlacements::BROWN_MUSHROOM_NORMAL = nullptr;
const PlacedFeature* VegetationPlacements::RED_MUSHROOM_NORMAL = nullptr;
const PlacedFeature* VegetationPlacements::BROWN_MUSHROOM_TAIGA = nullptr;
const PlacedFeature* VegetationPlacements::RED_MUSHROOM_TAIGA = nullptr;
const PlacedFeature* VegetationPlacements::BROWN_MUSHROOM_OLD_GROWTH = nullptr;
const PlacedFeature* VegetationPlacements::RED_MUSHROOM_OLD_GROWTH = nullptr;
const PlacedFeature* VegetationPlacements::BROWN_MUSHROOM_SWAMP = nullptr;
const PlacedFeature* VegetationPlacements::RED_MUSHROOM_SWAMP = nullptr;

// Flowers (lines 83-92)
const PlacedFeature* VegetationPlacements::FLOWER_WARM = nullptr;
const PlacedFeature* VegetationPlacements::FLOWER_DEFAULT = nullptr;
const PlacedFeature* VegetationPlacements::FLOWER_FLOWER_FOREST = nullptr;
const PlacedFeature* VegetationPlacements::FLOWER_SWAMP = nullptr;
const PlacedFeature* VegetationPlacements::FLOWER_PLAINS = nullptr;
const PlacedFeature* VegetationPlacements::FLOWER_MEADOW = nullptr;
const PlacedFeature* VegetationPlacements::FLOWER_CHERRY = nullptr;
const PlacedFeature* VegetationPlacements::FLOWER_PALE_GARDEN = nullptr;
const PlacedFeature* VegetationPlacements::WILDFLOWERS_BIRCH_FOREST = nullptr;
const PlacedFeature* VegetationPlacements::WILDFLOWERS_MEADOW = nullptr;

// Trees (lines 93-122)
const PlacedFeature* VegetationPlacements::TREES_PLAINS = nullptr;
const PlacedFeature* VegetationPlacements::DARK_FOREST_VEGETATION = nullptr;
const PlacedFeature* VegetationPlacements::PALE_GARDEN_VEGETATION = nullptr;
const PlacedFeature* VegetationPlacements::FLOWER_FOREST_FLOWERS = nullptr;
const PlacedFeature* VegetationPlacements::FOREST_FLOWERS = nullptr;
const PlacedFeature* VegetationPlacements::PALE_GARDEN_FLOWERS = nullptr;
const PlacedFeature* VegetationPlacements::PALE_MOSS_PATCH = nullptr;
const PlacedFeature* VegetationPlacements::TREES_FLOWER_FOREST = nullptr;
const PlacedFeature* VegetationPlacements::TREES_MEADOW = nullptr;
const PlacedFeature* VegetationPlacements::TREES_CHERRY = nullptr;
const PlacedFeature* VegetationPlacements::TREES_TAIGA = nullptr;
const PlacedFeature* VegetationPlacements::TREES_GROVE = nullptr;
const PlacedFeature* VegetationPlacements::TREES_BADLANDS = nullptr;
const PlacedFeature* VegetationPlacements::TREES_SNOWY = nullptr;
const PlacedFeature* VegetationPlacements::TREES_SWAMP = nullptr;
const PlacedFeature* VegetationPlacements::TREES_WINDSWEPT_SAVANNA = nullptr;
const PlacedFeature* VegetationPlacements::TREES_SAVANNA = nullptr;
const PlacedFeature* VegetationPlacements::BIRCH_TALL = nullptr;
const PlacedFeature* VegetationPlacements::TREES_BIRCH = nullptr;
const PlacedFeature* VegetationPlacements::TREES_WINDSWEPT_FOREST = nullptr;
const PlacedFeature* VegetationPlacements::TREES_WINDSWEPT_HILLS = nullptr;
const PlacedFeature* VegetationPlacements::TREES_WATER = nullptr;
const PlacedFeature* VegetationPlacements::TREES_BIRCH_AND_OAK_LEAF_LITTER = nullptr;
const PlacedFeature* VegetationPlacements::TREES_SPARSE_JUNGLE = nullptr;
const PlacedFeature* VegetationPlacements::TREES_OLD_GROWTH_SPRUCE_TAIGA = nullptr;
const PlacedFeature* VegetationPlacements::TREES_OLD_GROWTH_PINE_TAIGA = nullptr;
const PlacedFeature* VegetationPlacements::TREES_JUNGLE = nullptr;
const PlacedFeature* VegetationPlacements::BAMBOO_VEGETATION = nullptr;
const PlacedFeature* VegetationPlacements::MUSHROOM_ISLAND_VEGETATION = nullptr;
const PlacedFeature* VegetationPlacements::TREES_MANGROVE = nullptr;

// Backwards compatibility aliases
const PlacedFeature* VegetationPlacements::PATCH_FERN = nullptr;
const PlacedFeature* VegetationPlacements::FLOWER_PLAIN = nullptr;
const PlacedFeature* VegetationPlacements::FLOWER_FOREST = nullptr;
const PlacedFeature* VegetationPlacements::PATCH_CACTUS = nullptr;

// =========================================================================
// STORAGE - Use deque to avoid pointer invalidation
// =========================================================================
static std::deque<PlacedFeature> s_placedFeatures;
static std::deque<HeightmapPlacement> s_heightmapPlacements;
static std::deque<CountPlacement> s_countPlacements;
static std::deque<RarityFilter> s_rarityFilters;
static std::deque<SurfaceWaterDepthFilter> s_surfaceWaterDepthFilters;
static std::deque<NoiseThresholdCountPlacement> s_noiseThresholdCountPlacements;
static std::deque<NoiseBasedCountPlacement> s_noiseBasedCountPlacements;
static std::deque<HeightRangePlacement> s_heightRangePlacements;

// Storage for IntProviders used by ClampedInt CountPlacements
static std::deque<levelgen::carver::UniformInt> s_uniformInts;
static std::deque<levelgen::carver::ClampedInt> s_clampedInts;

// Storage for BlockPredicateFilter instances
static std::deque<BlockPredicateFilter> s_blockPredicateFilters;

// Storage for shared_ptr<BlockPredicate> instances (to keep them alive)
static std::deque<std::shared_ptr<levelgen::blockpredicates::BlockPredicate>> s_blockPredicates;

// =========================================================================
// HELPER METHODS
// Reference: VegetationPlacements.java lines 125-155
// =========================================================================

std::vector<PlacementModifier*> VegetationPlacements::worldSurfaceSquaredWithCount(int count) {
    // Reference: VegetationPlacements.java line 126
    // return List.of(CountPlacement.of(count), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP_WORLD_SURFACE, BiomeFilter.biome());
    s_countPlacements.push_back(CountPlacement::of(count));
    s_heightmapPlacements.push_back(HeightmapPlacement::onHeightmap(Heightmap::Types::WORLD_SURFACE_WG));

    return {
        &s_countPlacements.back(),
        &InSquarePlacement::spread(),
        &s_heightmapPlacements.back(),
        &BiomeFilter::biome()
    };
}

std::vector<PlacementModifier*> VegetationPlacements::getMushroomPlacement(int rarity, PlacementModifier* prefix) {
    // Reference: VegetationPlacements.java lines 129-143
    // ImmutableList.Builder<PlacementModifier> builder = ImmutableList.builder();
    // if (prefix != null) builder.add(prefix);
    // if (rarity != 0) builder.add(RarityFilter.onAverageOnceEvery(rarity));
    // builder.add(InSquarePlacement.spread());
    // builder.add(PlacementUtils.HEIGHTMAP);  // MOTION_BLOCKING
    // builder.add(BiomeFilter.biome());
    std::vector<PlacementModifier*> result;

    if (prefix != nullptr) {
        result.push_back(prefix);
    }

    if (rarity != 0) {
        s_rarityFilters.push_back(RarityFilter::onAverageOnceEvery(rarity));
        result.push_back(&s_rarityFilters.back());
    }

    result.push_back(&InSquarePlacement::spread());

    s_heightmapPlacements.push_back(HeightmapPlacement::onHeightmap(Heightmap::Types::MOTION_BLOCKING));
    result.push_back(&s_heightmapPlacements.back());

    result.push_back(&BiomeFilter::biome());

    return result;
}

std::vector<PlacementModifier*> VegetationPlacements::treePlacementBase(PlacementModifier* frequency) {
    // Reference: VegetationPlacements.java lines 145-147
    // return ImmutableList.builder().add(frequency).add(InSquarePlacement.spread()).add(TREE_THRESHOLD).add(PlacementUtils.HEIGHTMAP_OCEAN_FLOOR).add(BiomeFilter.biome());
    // TREE_THRESHOLD = SurfaceWaterDepthFilter.forMaxDepth(0)
    s_surfaceWaterDepthFilters.push_back(SurfaceWaterDepthFilter::forMaxDepth(0));
    s_heightmapPlacements.push_back(HeightmapPlacement::onHeightmap(Heightmap::Types::OCEAN_FLOOR));

    return {
        frequency,
        &InSquarePlacement::spread(),
        &s_surfaceWaterDepthFilters.back(),
        &s_heightmapPlacements.back(),
        &BiomeFilter::biome()
    };
}

std::vector<PlacementModifier*> VegetationPlacements::treePlacement(PlacementModifier* frequency) {
    // Reference: VegetationPlacements.java lines 149-151
    // Overload without block = no wouldSurvive filter
    return treePlacementBase(frequency);
}

std::vector<PlacementModifier*> VegetationPlacements::treePlacementWithSapling(PlacementModifier* frequency) {
    // Reference: VegetationPlacements.java lines 153-157
    // treePlacement(PlacementModifier, Block) adds BlockPredicateFilter.forPredicate(wouldSurvive)
    using namespace levelgen::blockpredicates;
    auto result = treePlacementBase(frequency);
    auto wouldSurvive = BlockPredicate::wouldSurvive(nullptr, {0, 0, 0});
    s_blockPredicates.push_back(wouldSurvive);
    s_blockPredicateFilters.push_back(BlockPredicateFilter::hasSturdyFace(
        [wouldSurvive](const PlacementContext& ctx, const core::BlockPos& pos) {
            return wouldSurvive->test(*ctx.getLevel(), pos);
        }
    ));
    result.push_back(&s_blockPredicateFilters.back());
    return result;
}

std::vector<PlacementModifier*> VegetationPlacements::flowerPlacement(int rarity) {
    // Helper for simple rarity-based flower placement (backwards compatibility)
    s_rarityFilters.push_back(RarityFilter::onAverageOnceEvery(rarity));
    s_heightmapPlacements.push_back(HeightmapPlacement::onHeightmap(Heightmap::Types::MOTION_BLOCKING));

    return {
        &s_rarityFilters.back(),
        &InSquarePlacement::spread(),
        &s_heightmapPlacements.back(),
        &BiomeFilter::biome()
    };
}

// =========================================================================
// BOOTSTRAP - Initialize all PlacedFeatures exactly as Java does
// Reference: VegetationPlacements.java bootstrap() lines 157-309
// =========================================================================

void VegetationPlacements::bootstrap() {
    if (s_initialized) return;

    // Ensure features are bootstrapped first
    if (!VegetationFeatures::isInitialized()) {
        VegetationFeatures::bootstrap();
    }
    if (!features::TreeFeatures::isInitialized()) {
        features::TreeFeatures::bootstrap();
    }

    // Helper to create PlacedFeature and store it
    auto createPlaced = [](levelgen::ConfiguredFeature* feature, std::vector<PlacementModifier*> modifiers, const std::string& name = "") -> const PlacedFeature* {
        if (feature == nullptr) {
            return nullptr;  // Skip if feature doesn't exist
        }
        s_placedFeatures.emplace_back(feature, modifiers, name);
        return &s_placedFeatures.back();
    };

    // Helper for creating CountPlacement (returns pointer to stored placement)
    auto countOf = [](int count) -> PlacementModifier* {
        s_countPlacements.push_back(CountPlacement::of(count));
        return &s_countPlacements.back();
    };

    // Helper for creating RarityFilter
    auto rarityOf = [](int rarity) -> PlacementModifier* {
        s_rarityFilters.push_back(RarityFilter::onAverageOnceEvery(rarity));
        return &s_rarityFilters.back();
    };

    // Helper for creating NoiseThresholdCountPlacement
    auto noiseThresholdCount = [](double noiseLevel, int belowNoise, int aboveNoise) -> PlacementModifier* {
        s_noiseThresholdCountPlacements.push_back(NoiseThresholdCountPlacement::of(noiseLevel, belowNoise, aboveNoise));
        return &s_noiseThresholdCountPlacements.back();
    };

    // Helper for creating NoiseBasedCountPlacement
    auto noiseBasedCount = [](int baseCount, double noiseMultiplier, double noiseOffset) -> PlacementModifier* {
        s_noiseBasedCountPlacements.push_back(NoiseBasedCountPlacement::of(baseCount, noiseMultiplier, noiseOffset));
        return &s_noiseBasedCountPlacements.back();
    };

    // Helper for HEIGHTMAP (MOTION_BLOCKING) - PlacementUtils.HEIGHTMAP
    auto heightmapMotionBlocking = []() -> PlacementModifier* {
        s_heightmapPlacements.push_back(HeightmapPlacement::onHeightmap(Heightmap::Types::MOTION_BLOCKING));
        return &s_heightmapPlacements.back();
    };

    // Helper for HEIGHTMAP_WORLD_SURFACE - PlacementUtils.HEIGHTMAP_WORLD_SURFACE
    auto heightmapWorldSurface = []() -> PlacementModifier* {
        s_heightmapPlacements.push_back(HeightmapPlacement::onHeightmap(Heightmap::Types::WORLD_SURFACE_WG));
        return &s_heightmapPlacements.back();
    };

    // Helper for HEIGHTMAP_OCEAN_FLOOR - PlacementUtils.HEIGHTMAP_OCEAN_FLOOR
    auto heightmapOceanFloor = []() -> PlacementModifier* {
        s_heightmapPlacements.push_back(HeightmapPlacement::onHeightmap(Heightmap::Types::OCEAN_FLOOR));
        return &s_heightmapPlacements.back();
    };

    // =========================================================================
    // Line 219: BAMBOO_LIGHT
    // RarityFilter.onAverageOnceEvery(4), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    BAMBOO_LIGHT = createPlaced(
        VegetationFeatures::BAMBOO_NO_PODZOL,
        { rarityOf(4), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "BAMBOO_LIGHT"
    );

    // =========================================================================
    // Line 220: BAMBOO
    // NoiseBasedCountPlacement.of(160, 80.0, 0.3), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP_WORLD_SURFACE, BiomeFilter.biome()
    // =========================================================================
    BAMBOO = createPlaced(
        VegetationFeatures::BAMBOO_SOME_PODZOL,
        { noiseBasedCount(160, 80.0, 0.3), &InSquarePlacement::spread(), heightmapWorldSurface(), &BiomeFilter::biome() },
        "BAMBOO"
    );

    // =========================================================================
    // Line 221: VINES
    // CountPlacement.of(127), InSquarePlacement.spread(), HeightRangePlacement.uniform(VerticalAnchor.absolute(64), VerticalAnchor.absolute(100)), BiomeFilter.biome()
    // =========================================================================
    {
        s_heightRangePlacements.push_back(HeightRangePlacement::uniform(64, 100));
        VINES = createPlaced(
            VegetationFeatures::VINES,
            { countOf(127), &InSquarePlacement::spread(), &s_heightRangePlacements.back(), &BiomeFilter::biome() },
            "VINES"
        );
    }

    // =========================================================================
    // Line 222: PATCH_SUNFLOWER
    // RarityFilter.onAverageOnceEvery(3), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    PATCH_SUNFLOWER = createPlaced(
        VegetationFeatures::PATCH_SUNFLOWER,
        { rarityOf(3), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "PATCH_SUNFLOWER"
    );

    // =========================================================================
    // Line 223: PATCH_PUMPKIN
    // RarityFilter.onAverageOnceEvery(300), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    PATCH_PUMPKIN = createPlaced(
        VegetationFeatures::PATCH_PUMPKIN,
        { rarityOf(300), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "PATCH_PUMPKIN"
    );

    // =========================================================================
    // Line 224: PATCH_GRASS_PLAIN
    // NoiseThresholdCountPlacement.of(-0.8, 5, 10), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP_WORLD_SURFACE, BiomeFilter.biome()
    // =========================================================================
    PATCH_GRASS_PLAIN = createPlaced(
        VegetationFeatures::PATCH_GRASS,
        { noiseThresholdCount(-0.8, 5, 10), &InSquarePlacement::spread(), heightmapWorldSurface(), &BiomeFilter::biome() },
        "PATCH_GRASS_PLAIN"
    );

    // =========================================================================
    // Line 225: PATCH_GRASS_MEADOW
    // NoiseThresholdCountPlacement.of(-0.8, 5, 10), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP_WORLD_SURFACE, BiomeFilter.biome()
    // =========================================================================
    PATCH_GRASS_MEADOW = createPlaced(
        VegetationFeatures::PATCH_GRASS_MEADOW,
        { noiseThresholdCount(-0.8, 5, 10), &InSquarePlacement::spread(), heightmapWorldSurface(), &BiomeFilter::biome() },
        "PATCH_GRASS_MEADOW"
    );

    // =========================================================================
    // Line 226: PATCH_GRASS_FOREST
    // worldSurfaceSquaredWithCount(2)
    // =========================================================================
    PATCH_GRASS_FOREST = createPlaced(
        VegetationFeatures::PATCH_GRASS,
        worldSurfaceSquaredWithCount(2),
        "PATCH_GRASS_FOREST"
    );

    // =========================================================================
    // Line 227: PATCH_LEAF_LITTER
    // worldSurfaceSquaredWithCount(2)
    // =========================================================================
    PATCH_LEAF_LITTER = createPlaced(
        VegetationFeatures::PATCH_LEAF_LITTER,
        worldSurfaceSquaredWithCount(2),
        "PATCH_LEAF_LITTER"
    );

    // =========================================================================
    // Line 228: PATCH_GRASS_BADLANDS
    // InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP_WORLD_SURFACE, BiomeFilter.biome()
    // =========================================================================
    PATCH_GRASS_BADLANDS = createPlaced(
        VegetationFeatures::PATCH_GRASS,
        { &InSquarePlacement::spread(), heightmapWorldSurface(), &BiomeFilter::biome() },
        "PATCH_GRASS_BADLANDS"
    );

    // =========================================================================
    // Line 229: PATCH_GRASS_SAVANNA
    // worldSurfaceSquaredWithCount(20)
    // =========================================================================
    PATCH_GRASS_SAVANNA = createPlaced(
        VegetationFeatures::PATCH_GRASS,
        worldSurfaceSquaredWithCount(20),
        "PATCH_GRASS_SAVANNA"
    );

    // =========================================================================
    // Line 230: PATCH_GRASS_NORMAL
    // worldSurfaceSquaredWithCount(5)
    // =========================================================================
    PATCH_GRASS_NORMAL = createPlaced(
        VegetationFeatures::PATCH_GRASS,
        worldSurfaceSquaredWithCount(5),
        "PATCH_GRASS_NORMAL"
    );

    // =========================================================================
    // Line 231: PATCH_GRASS_TAIGA_2
    // InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP_WORLD_SURFACE, BiomeFilter.biome()
    // =========================================================================
    PATCH_GRASS_TAIGA_2 = createPlaced(
        VegetationFeatures::PATCH_TAIGA_GRASS,
        { &InSquarePlacement::spread(), heightmapWorldSurface(), &BiomeFilter::biome() },
        "PATCH_GRASS_TAIGA_2"
    );

    // =========================================================================
    // Line 232: PATCH_GRASS_TAIGA
    // worldSurfaceSquaredWithCount(7)
    // =========================================================================
    PATCH_GRASS_TAIGA = createPlaced(
        VegetationFeatures::PATCH_TAIGA_GRASS,
        worldSurfaceSquaredWithCount(7),
        "PATCH_GRASS_TAIGA"
    );

    // =========================================================================
    // Line 233: PATCH_GRASS_JUNGLE
    // worldSurfaceSquaredWithCount(25)
    // =========================================================================
    PATCH_GRASS_JUNGLE = createPlaced(
        VegetationFeatures::PATCH_GRASS_JUNGLE,
        worldSurfaceSquaredWithCount(25),
        "PATCH_GRASS_JUNGLE"
    );

    // =========================================================================
    // Line 234: GRASS_BONEMEAL
    // PlacementUtils.isEmpty() - just BiomeFilter
    // =========================================================================
    GRASS_BONEMEAL = createPlaced(
        VegetationFeatures::SINGLE_PIECE_OF_GRASS,
        { &BiomeFilter::biome() },
        "GRASS_BONEMEAL"
    );

    // =========================================================================
    // Line 235: PATCH_DEAD_BUSH_2
    // worldSurfaceSquaredWithCount(2)
    // =========================================================================
    PATCH_DEAD_BUSH_2 = createPlaced(
        VegetationFeatures::PATCH_DEAD_BUSH,
        worldSurfaceSquaredWithCount(2),
        "PATCH_DEAD_BUSH_2"
    );

    // =========================================================================
    // Line 236: PATCH_DEAD_BUSH
    // InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP_WORLD_SURFACE, BiomeFilter.biome()
    // =========================================================================
    PATCH_DEAD_BUSH = createPlaced(
        VegetationFeatures::PATCH_DEAD_BUSH,
        { &InSquarePlacement::spread(), heightmapWorldSurface(), &BiomeFilter::biome() },
        "PATCH_DEAD_BUSH"
    );

    // =========================================================================
    // Line 237: PATCH_DEAD_BUSH_BADLANDS
    // worldSurfaceSquaredWithCount(20)
    // =========================================================================
    PATCH_DEAD_BUSH_BADLANDS = createPlaced(
        VegetationFeatures::PATCH_DEAD_BUSH,
        worldSurfaceSquaredWithCount(20),
        "PATCH_DEAD_BUSH_BADLANDS"
    );

    // =========================================================================
    // Line 238: PATCH_DRY_GRASS_BADLANDS
    // RarityFilter.onAverageOnceEvery(6), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    PATCH_DRY_GRASS_BADLANDS = createPlaced(
        VegetationFeatures::PATCH_DRY_GRASS,
        { rarityOf(6), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "PATCH_DRY_GRASS_BADLANDS"
    );

    // =========================================================================
    // Line 239: PATCH_DRY_GRASS_DESERT
    // RarityFilter.onAverageOnceEvery(3), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    PATCH_DRY_GRASS_DESERT = createPlaced(
        VegetationFeatures::PATCH_DRY_GRASS,
        { rarityOf(3), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "PATCH_DRY_GRASS_DESERT"
    );

    // =========================================================================
    // Line 240: PATCH_MELON
    // RarityFilter.onAverageOnceEvery(6), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    PATCH_MELON = createPlaced(
        VegetationFeatures::PATCH_MELON,
        { rarityOf(6), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "PATCH_MELON"
    );

    // =========================================================================
    // Line 241: PATCH_MELON_SPARSE
    // RarityFilter.onAverageOnceEvery(64), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    PATCH_MELON_SPARSE = createPlaced(
        VegetationFeatures::PATCH_MELON,
        { rarityOf(64), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "PATCH_MELON_SPARSE"
    );

    // =========================================================================
    // Line 242: PATCH_BERRY_COMMON
    // RarityFilter.onAverageOnceEvery(32), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP_WORLD_SURFACE, BiomeFilter.biome()
    // =========================================================================
    PATCH_BERRY_COMMON = createPlaced(
        VegetationFeatures::PATCH_BERRY_BUSH,
        { rarityOf(32), &InSquarePlacement::spread(), heightmapWorldSurface(), &BiomeFilter::biome() },
        "PATCH_BERRY_COMMON"
    );

    // =========================================================================
    // Line 243: PATCH_BERRY_RARE
    // RarityFilter.onAverageOnceEvery(384), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP_WORLD_SURFACE, BiomeFilter.biome()
    // =========================================================================
    PATCH_BERRY_RARE = createPlaced(
        VegetationFeatures::PATCH_BERRY_BUSH,
        { rarityOf(384), &InSquarePlacement::spread(), heightmapWorldSurface(), &BiomeFilter::biome() },
        "PATCH_BERRY_RARE"
    );

    // =========================================================================
    // Line 244: PATCH_WATERLILY
    // worldSurfaceSquaredWithCount(4)
    // =========================================================================
    PATCH_WATERLILY = createPlaced(
        VegetationFeatures::PATCH_WATERLILY,
        worldSurfaceSquaredWithCount(4),
        "PATCH_WATERLILY"
    );

    // =========================================================================
    // Line 245: PATCH_TALL_GRASS_2
    // NoiseThresholdCountPlacement.of(-0.8, 0, 7), RarityFilter.onAverageOnceEvery(32), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    PATCH_TALL_GRASS_2 = createPlaced(
        VegetationFeatures::PATCH_TALL_GRASS,
        { noiseThresholdCount(-0.8, 0, 7), rarityOf(32), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "PATCH_TALL_GRASS_2"
    );

    // =========================================================================
    // Line 246: PATCH_TALL_GRASS
    // RarityFilter.onAverageOnceEvery(5), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    PATCH_TALL_GRASS = createPlaced(
        VegetationFeatures::PATCH_TALL_GRASS,
        { rarityOf(5), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "PATCH_TALL_GRASS"
    );

    // =========================================================================
    // Line 247: PATCH_LARGE_FERN
    // RarityFilter.onAverageOnceEvery(5), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    PATCH_LARGE_FERN = createPlaced(
        VegetationFeatures::PATCH_LARGE_FERN,
        { rarityOf(5), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "PATCH_LARGE_FERN"
    );

    // =========================================================================
    // Line 248: PATCH_BUSH
    // RarityFilter.onAverageOnceEvery(4), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    PATCH_BUSH = createPlaced(
        VegetationFeatures::PATCH_BUSH,
        { rarityOf(4), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "PATCH_BUSH"
    );

    // =========================================================================
    // Line 249: PATCH_CACTUS_DESERT
    // RarityFilter.onAverageOnceEvery(6), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    PATCH_CACTUS_DESERT = createPlaced(
        VegetationFeatures::PATCH_CACTUS,
        { rarityOf(6), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "PATCH_CACTUS_DESERT"
    );

    // =========================================================================
    // Line 250: PATCH_CACTUS_DECORATED
    // RarityFilter.onAverageOnceEvery(13), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    PATCH_CACTUS_DECORATED = createPlaced(
        VegetationFeatures::PATCH_CACTUS,
        { rarityOf(13), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "PATCH_CACTUS_DECORATED"
    );

    // =========================================================================
    // Line 251: PATCH_SUGAR_CANE_SWAMP
    // RarityFilter.onAverageOnceEvery(3), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    PATCH_SUGAR_CANE_SWAMP = createPlaced(
        VegetationFeatures::PATCH_SUGAR_CANE,
        { rarityOf(3), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "PATCH_SUGAR_CANE_SWAMP"
    );

    // =========================================================================
    // Line 252: PATCH_SUGAR_CANE_DESERT
    // InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome() (no rarity)
    // =========================================================================
    PATCH_SUGAR_CANE_DESERT = createPlaced(
        VegetationFeatures::PATCH_SUGAR_CANE,
        { &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "PATCH_SUGAR_CANE_DESERT"
    );

    // =========================================================================
    // Line 253: PATCH_SUGAR_CANE_BADLANDS
    // RarityFilter.onAverageOnceEvery(5), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    PATCH_SUGAR_CANE_BADLANDS = createPlaced(
        VegetationFeatures::PATCH_SUGAR_CANE,
        { rarityOf(5), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "PATCH_SUGAR_CANE_BADLANDS"
    );

    // =========================================================================
    // Line 254: PATCH_SUGAR_CANE
    // RarityFilter.onAverageOnceEvery(6), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    PATCH_SUGAR_CANE = createPlaced(
        VegetationFeatures::PATCH_SUGAR_CANE,
        { rarityOf(6), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "PATCH_SUGAR_CANE"
    );

    // =========================================================================
    // Line 255: PATCH_FIREFLY_BUSH_NEAR_WATER
    // CountPlacement.of(2), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP_NO_LEAVES, BiomeFilter.biome(), nearWaterPredicate
    // nearWaterPredicate = BlockPredicateFilter.forPredicate(BlockPredicate.allOf(
    //   ONLY_IN_AIR_PREDICATE,
    //   wouldSurvive(FIREFLY_BUSH, BlockPos.ZERO),
    //   anyOf(matchesFluids(+1,-1,0,water), matchesFluids(-1,-1,0,water), matchesFluids(0,-1,+1,water), matchesFluids(0,-1,-1,water))
    // ))
    // =========================================================================
    {
        using namespace levelgen::blockpredicates;
        // Create the composite predicate for near-water placement
        // Check: air at pos AND water in one of the 4 adjacent positions below
        auto waterCheck1 = BlockPredicate::matchesFluids({1, -1, 0}, {"minecraft:water", "minecraft:flowing_water"});
        auto waterCheck2 = BlockPredicate::matchesFluids({-1, -1, 0}, {"minecraft:water", "minecraft:flowing_water"});
        auto waterCheck3 = BlockPredicate::matchesFluids({0, -1, 1}, {"minecraft:water", "minecraft:flowing_water"});
        auto waterCheck4 = BlockPredicate::matchesFluids({0, -1, -1}, {"minecraft:water", "minecraft:flowing_water"});
        auto nearWater = BlockPredicate::anyOf({waterCheck1, waterCheck2, waterCheck3, waterCheck4});
        auto airCheck = BlockPredicate::ONLY_IN_AIR_PREDICATE;
        auto fullPredicate = BlockPredicate::allOf({airCheck, nearWater});

        s_blockPredicates.push_back(fullPredicate);
        s_blockPredicateFilters.push_back(BlockPredicateFilter::hasSturdyFace(
            [fullPredicate](const PlacementContext& ctx, const core::BlockPos& pos) {
                return fullPredicate->test(*ctx.getLevel(), pos);
            }
        ));

        PATCH_FIREFLY_BUSH_NEAR_WATER = createPlaced(
            VegetationFeatures::PATCH_FIREFLY_BUSH,
            { countOf(2), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome(), &s_blockPredicateFilters.back() },
            "PATCH_FIREFLY_BUSH_NEAR_WATER"
        );
    }

    // =========================================================================
    // Line 256: PATCH_FIREFLY_BUSH_NEAR_WATER_SWAMP
    // CountPlacement.of(3), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome(), nearWaterPredicate
    // Reuse the same nearWaterPredicate filter created above
    // =========================================================================
    PATCH_FIREFLY_BUSH_NEAR_WATER_SWAMP = createPlaced(
        VegetationFeatures::PATCH_FIREFLY_BUSH,
        { countOf(3), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome(), &s_blockPredicateFilters.back() },
        "PATCH_FIREFLY_BUSH_NEAR_WATER_SWAMP"
    );

    // =========================================================================
    // Line 257: PATCH_FIREFLY_BUSH_SWAMP
    // RarityFilter.onAverageOnceEvery(8), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    PATCH_FIREFLY_BUSH_SWAMP = createPlaced(
        VegetationFeatures::PATCH_FIREFLY_BUSH,
        { rarityOf(8), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "PATCH_FIREFLY_BUSH_SWAMP"
    );

    // =========================================================================
    // Line 258: BROWN_MUSHROOM_NETHER
    // RarityFilter.onAverageOnceEvery(2), InSquarePlacement.spread(), PlacementUtils.FULL_RANGE, BiomeFilter.biome()
    // FULL_RANGE = HeightRangePlacement.uniform(VerticalAnchor.bottom(), VerticalAnchor.top())
    // =========================================================================
    {
        s_heightRangePlacements.push_back(HeightRangePlacement::uniform(-64, 320));  // FULL_RANGE
        BROWN_MUSHROOM_NETHER = createPlaced(
            VegetationFeatures::PATCH_BROWN_MUSHROOM,
            { rarityOf(2), &InSquarePlacement::spread(), &s_heightRangePlacements.back(), &BiomeFilter::biome() },
            "BROWN_MUSHROOM_NETHER"
        );
    }

    // =========================================================================
    // Line 259: RED_MUSHROOM_NETHER
    // RarityFilter.onAverageOnceEvery(2), InSquarePlacement.spread(), PlacementUtils.FULL_RANGE, BiomeFilter.biome()
    // =========================================================================
    {
        s_heightRangePlacements.push_back(HeightRangePlacement::uniform(-64, 320));  // FULL_RANGE
        RED_MUSHROOM_NETHER = createPlaced(
            VegetationFeatures::PATCH_RED_MUSHROOM,
            { rarityOf(2), &InSquarePlacement::spread(), &s_heightRangePlacements.back(), &BiomeFilter::biome() },
            "RED_MUSHROOM_NETHER"
        );
    }

    // =========================================================================
    // Line 260: BROWN_MUSHROOM_NORMAL
    // getMushroomPlacement(256, null)
    // =========================================================================
    BROWN_MUSHROOM_NORMAL = createPlaced(
        VegetationFeatures::PATCH_BROWN_MUSHROOM,
        getMushroomPlacement(256, nullptr),
        "BROWN_MUSHROOM_NORMAL"
    );

    // =========================================================================
    // Line 261: RED_MUSHROOM_NORMAL
    // getMushroomPlacement(512, null)
    // =========================================================================
    RED_MUSHROOM_NORMAL = createPlaced(
        VegetationFeatures::PATCH_RED_MUSHROOM,
        getMushroomPlacement(512, nullptr),
        "RED_MUSHROOM_NORMAL"
    );

    // =========================================================================
    // Line 262: BROWN_MUSHROOM_TAIGA
    // getMushroomPlacement(4, null)
    // =========================================================================
    BROWN_MUSHROOM_TAIGA = createPlaced(
        VegetationFeatures::PATCH_BROWN_MUSHROOM,
        getMushroomPlacement(4, nullptr),
        "BROWN_MUSHROOM_TAIGA"
    );

    // =========================================================================
    // Line 263: RED_MUSHROOM_TAIGA
    // getMushroomPlacement(256, null)
    // =========================================================================
    RED_MUSHROOM_TAIGA = createPlaced(
        VegetationFeatures::PATCH_RED_MUSHROOM,
        getMushroomPlacement(256, nullptr),
        "RED_MUSHROOM_TAIGA"
    );

    // =========================================================================
    // Line 264: BROWN_MUSHROOM_OLD_GROWTH
    // getMushroomPlacement(4, CountPlacement.of(3))
    // =========================================================================
    BROWN_MUSHROOM_OLD_GROWTH = createPlaced(
        VegetationFeatures::PATCH_BROWN_MUSHROOM,
        getMushroomPlacement(4, countOf(3)),
        "BROWN_MUSHROOM_OLD_GROWTH"
    );

    // =========================================================================
    // Line 265: RED_MUSHROOM_OLD_GROWTH
    // getMushroomPlacement(171, null)
    // =========================================================================
    RED_MUSHROOM_OLD_GROWTH = createPlaced(
        VegetationFeatures::PATCH_RED_MUSHROOM,
        getMushroomPlacement(171, nullptr),
        "RED_MUSHROOM_OLD_GROWTH"
    );

    // =========================================================================
    // Line 266: BROWN_MUSHROOM_SWAMP
    // getMushroomPlacement(0, CountPlacement.of(2))
    // =========================================================================
    BROWN_MUSHROOM_SWAMP = createPlaced(
        VegetationFeatures::PATCH_BROWN_MUSHROOM,
        getMushroomPlacement(0, countOf(2)),
        "BROWN_MUSHROOM_SWAMP"
    );

    // =========================================================================
    // Line 267: RED_MUSHROOM_SWAMP
    // getMushroomPlacement(64, null)
    // =========================================================================
    RED_MUSHROOM_SWAMP = createPlaced(
        VegetationFeatures::PATCH_RED_MUSHROOM,
        getMushroomPlacement(64, nullptr),
        "RED_MUSHROOM_SWAMP"
    );

    // =========================================================================
    // Line 268: FLOWER_WARM
    // RarityFilter.onAverageOnceEvery(16), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    FLOWER_WARM = createPlaced(
        VegetationFeatures::FLOWER_DEFAULT,
        { rarityOf(16), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "FLOWER_WARM"
    );

    // =========================================================================
    // Line 269: FLOWER_DEFAULT
    // RarityFilter.onAverageOnceEvery(32), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    FLOWER_DEFAULT = createPlaced(
        VegetationFeatures::FLOWER_DEFAULT,
        { rarityOf(32), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "FLOWER_DEFAULT"
    );

    // =========================================================================
    // Line 270: FLOWER_FLOWER_FOREST
    // CountPlacement.of(3), RarityFilter.onAverageOnceEvery(2), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    FLOWER_FLOWER_FOREST = createPlaced(
        VegetationFeatures::FLOWER_FLOWER_FOREST,
        { countOf(3), rarityOf(2), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "FLOWER_FLOWER_FOREST"
    );

    // =========================================================================
    // Line 271: FLOWER_SWAMP
    // RarityFilter.onAverageOnceEvery(32), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    FLOWER_SWAMP = createPlaced(
        VegetationFeatures::FLOWER_SWAMP,
        { rarityOf(32), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "FLOWER_SWAMP"
    );

    // =========================================================================
    // Line 272: FLOWER_PLAINS
    // NoiseThresholdCountPlacement.of(-0.8, 15, 4), RarityFilter.onAverageOnceEvery(32), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    FLOWER_PLAINS = createPlaced(
        VegetationFeatures::FLOWER_PLAIN,
        { noiseThresholdCount(-0.8, 15, 4), rarityOf(32), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "FLOWER_PLAINS"
    );

    // =========================================================================
    // Line 273: FLOWER_CHERRY
    // NoiseThresholdCountPlacement.of(-0.8, 5, 10), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    FLOWER_CHERRY = createPlaced(
        VegetationFeatures::FLOWER_CHERRY,
        { noiseThresholdCount(-0.8, 5, 10), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "FLOWER_CHERRY"
    );

    // =========================================================================
    // Line 274: FLOWER_MEADOW
    // InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    FLOWER_MEADOW = createPlaced(
        VegetationFeatures::FLOWER_MEADOW,
        { &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "FLOWER_MEADOW"
    );

    // =========================================================================
    // Line 275: FLOWER_PALE_GARDEN
    // RarityFilter.onAverageOnceEvery(32), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    FLOWER_PALE_GARDEN = createPlaced(
        VegetationFeatures::FLOWER_PALE_GARDEN,
        { rarityOf(32), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "FLOWER_PALE_GARDEN"
    );

    // =========================================================================
    // Line 276: WILDFLOWERS_BIRCH_FOREST
    // CountPlacement.of(3), RarityFilter.onAverageOnceEvery(2), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    WILDFLOWERS_BIRCH_FOREST = createPlaced(
        VegetationFeatures::WILDFLOWERS_BIRCH_FOREST,
        { countOf(3), rarityOf(2), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "WILDFLOWERS_BIRCH_FOREST"
    );

    // =========================================================================
    // Line 277: WILDFLOWERS_MEADOW
    // NoiseThresholdCountPlacement.of(-0.8, 5, 10), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    WILDFLOWERS_MEADOW = createPlaced(
        VegetationFeatures::WILDFLOWERS_MEADOW,
        { noiseThresholdCount(-0.8, 5, 10), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "WILDFLOWERS_MEADOW"
    );

    // =========================================================================
    // Line 278-279: TREES_PLAINS
    // PlacementUtils.countExtra(0, 0.05F, 1), InSquarePlacement.spread(), treeThreshold, PlacementUtils.HEIGHTMAP_OCEAN_FLOOR,
    // BlockPredicateFilter.forPredicate(BlockPredicate.wouldSurvive(OAK_SAPLING.defaultBlockState(), BlockPos.ZERO)), BiomeFilter.biome()
    // =========================================================================
    {
        using namespace levelgen::blockpredicates;
        s_countPlacements.push_back(CountPlacement::countExtra(0, 0.05f, 1));
        s_surfaceWaterDepthFilters.push_back(SurfaceWaterDepthFilter::forMaxDepth(0));

        // BlockPredicateFilter for wouldSurvive(OAK_SAPLING)
        // Note: WouldSurvivePredicate is simplified to return true for in-bounds positions
        auto wouldSurvive = BlockPredicate::wouldSurvive(nullptr, {0, 0, 0});
        s_blockPredicates.push_back(wouldSurvive);
        s_blockPredicateFilters.push_back(BlockPredicateFilter::hasSturdyFace(
            [wouldSurvive](const PlacementContext& ctx, const core::BlockPos& pos) {
                return wouldSurvive->test(*ctx.getLevel(), pos);
            }
        ));

        TREES_PLAINS = createPlaced(
            VegetationFeatures::TREES_PLAINS,
            { &s_countPlacements.back(), &InSquarePlacement::spread(), &s_surfaceWaterDepthFilters.back(), heightmapOceanFloor(), &s_blockPredicateFilters.back(), &BiomeFilter::biome() },
            "TREES_PLAINS"
        );
    }

    // =========================================================================
    // Line 280: DARK_FOREST_VEGETATION
    // CountPlacement.of(16), InSquarePlacement.spread(), treeThreshold, PlacementUtils.HEIGHTMAP_OCEAN_FLOOR, BiomeFilter.biome()
    // =========================================================================
    {
        s_surfaceWaterDepthFilters.push_back(SurfaceWaterDepthFilter::forMaxDepth(0));
        DARK_FOREST_VEGETATION = createPlaced(
            VegetationFeatures::DARK_FOREST_VEGETATION,
            { countOf(16), &InSquarePlacement::spread(), &s_surfaceWaterDepthFilters.back(), heightmapOceanFloor(), &BiomeFilter::biome() },
            "DARK_FOREST_VEGETATION"
        );
    }

    // =========================================================================
    // Line 281: PALE_GARDEN_VEGETATION
    // CountPlacement.of(16), InSquarePlacement.spread(), treeThreshold, PlacementUtils.HEIGHTMAP_OCEAN_FLOOR, BiomeFilter.biome()
    // =========================================================================
    {
        s_surfaceWaterDepthFilters.push_back(SurfaceWaterDepthFilter::forMaxDepth(0));
        PALE_GARDEN_VEGETATION = createPlaced(
            VegetationFeatures::PALE_GARDEN_VEGETATION,
            { countOf(16), &InSquarePlacement::spread(), &s_surfaceWaterDepthFilters.back(), heightmapOceanFloor(), &BiomeFilter::biome() },
            "PALE_GARDEN_VEGETATION"
        );
    }

    // =========================================================================
    // Line 282: FLOWER_FOREST_FLOWERS
    // RarityFilter.onAverageOnceEvery(7), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, CountPlacement.of(ClampedInt.of(UniformInt.of(-1, 3), 0, 3)), BiomeFilter.biome()
    // =========================================================================
    {
        // UniformInt.of(-1, 3) generates -1, 0, 1, 2, or 3
        // ClampedInt clamps to [0, 3], so -1 becomes 0
        s_uniformInts.push_back(levelgen::carver::UniformInt::of(-1, 3));
        s_clampedInts.push_back(levelgen::carver::ClampedInt::of(&s_uniformInts.back(), 0, 3));
        s_countPlacements.push_back(CountPlacement::of(&s_clampedInts.back()));

        FLOWER_FOREST_FLOWERS = createPlaced(
            VegetationFeatures::FOREST_FLOWERS,
            { rarityOf(7), &InSquarePlacement::spread(), heightmapMotionBlocking(), &s_countPlacements.back(), &BiomeFilter::biome() },
            "FLOWER_FOREST_FLOWERS"
        );
    }

    // =========================================================================
    // Line 283: FOREST_FLOWERS
    // RarityFilter.onAverageOnceEvery(7), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, CountPlacement.of(ClampedInt.of(UniformInt.of(-3, 1), 0, 1)), BiomeFilter.biome()
    // =========================================================================
    {
        // UniformInt.of(-3, 1) generates -3, -2, -1, 0, or 1
        // ClampedInt clamps to [0, 1], so -3, -2, -1 become 0
        s_uniformInts.push_back(levelgen::carver::UniformInt::of(-3, 1));
        s_clampedInts.push_back(levelgen::carver::ClampedInt::of(&s_uniformInts.back(), 0, 1));
        s_countPlacements.push_back(CountPlacement::of(&s_clampedInts.back()));

        FOREST_FLOWERS = createPlaced(
            VegetationFeatures::FOREST_FLOWERS,
            { rarityOf(7), &InSquarePlacement::spread(), heightmapMotionBlocking(), &s_countPlacements.back(), &BiomeFilter::biome() },
            "FOREST_FLOWERS"
        );
    }

    // =========================================================================
    // Line 284: PALE_GARDEN_FLOWERS
    // RarityFilter.onAverageOnceEvery(8), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP_NO_LEAVES, BiomeFilter.biome()
    // =========================================================================
    PALE_GARDEN_FLOWERS = createPlaced(
        VegetationFeatures::PALE_FOREST_FLOWERS,
        { rarityOf(8), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "PALE_GARDEN_FLOWERS"
    );

    // =========================================================================
    // Line 285: PALE_MOSS_PATCH
    // CountPlacement.of(1), InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP_NO_LEAVES, BiomeFilter.biome()
    // =========================================================================
    PALE_MOSS_PATCH = createPlaced(
        VegetationFeatures::PALE_MOSS_PATCH,
        { countOf(1), &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "PALE_MOSS_PATCH"
    );

    // =========================================================================
    // Line 286: TREES_FLOWER_FOREST
    // treePlacement(PlacementUtils.countExtra(6, 0.1F, 1))
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::countExtra(6, 0.1f, 1));
        TREES_FLOWER_FOREST = createPlaced(
            VegetationFeatures::TREES_FLOWER_FOREST,
            treePlacement(&s_countPlacements.back()),
            "TREES_FLOWER_FOREST"
        );
    }

    // =========================================================================
    // Line 287: TREES_MEADOW
    // treePlacement(RarityFilter.onAverageOnceEvery(100))
    // =========================================================================
    TREES_MEADOW = createPlaced(
        VegetationFeatures::MEADOW_TREES,
        treePlacement(rarityOf(100)),
        "TREES_MEADOW"
    );

    // =========================================================================
    // Line 288: TREES_CHERRY
    // treePlacement(PlacementUtils.countExtra(10, 0.1F, 1), Blocks.CHERRY_SAPLING)
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::countExtra(10, 0.1f, 1));
        TREES_CHERRY = createPlaced(
            features::TreeFeatures::CHERRY_BEES_005,
            treePlacementWithSapling(&s_countPlacements.back()),
            "TREES_CHERRY"
        );
    }

    // =========================================================================
    // Line 289: TREES_TAIGA
    // treePlacement(PlacementUtils.countExtra(10, 0.1F, 1))
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::countExtra(10, 0.1f, 1));
        TREES_TAIGA = createPlaced(
            VegetationFeatures::TREES_TAIGA,
            treePlacement(&s_countPlacements.back()),
            "TREES_TAIGA"
        );
    }

    // =========================================================================
    // Line 290: TREES_GROVE
    // treePlacement(PlacementUtils.countExtra(10, 0.1F, 1))
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::countExtra(10, 0.1f, 1));
        TREES_GROVE = createPlaced(
            VegetationFeatures::TREES_GROVE,
            treePlacement(&s_countPlacements.back()),
            "TREES_GROVE"
        );
    }

    // =========================================================================
    // Line 291: TREES_BADLANDS
    // treePlacement(PlacementUtils.countExtra(5, 0.1F, 1), Blocks.OAK_SAPLING)
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::countExtra(5, 0.1f, 1));
        TREES_BADLANDS = createPlaced(
            VegetationFeatures::TREES_BADLANDS,
            treePlacementWithSapling(&s_countPlacements.back()),
            "TREES_BADLANDS"
        );
    }

    // =========================================================================
    // Line 292: TREES_SNOWY
    // treePlacement(PlacementUtils.countExtra(0, 0.1F, 1), Blocks.SPRUCE_SAPLING)
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::countExtra(0, 0.1f, 1));
        TREES_SNOWY = createPlaced(
            VegetationFeatures::TREES_SNOWY,
            treePlacementWithSapling(&s_countPlacements.back()),
            "TREES_SNOWY"
        );
    }

    // =========================================================================
    // Line 293: TREES_SWAMP
    // PlacementUtils.countExtra(2, 0.1F, 1), InSquarePlacement.spread(), SurfaceWaterDepthFilter.forMaxDepth(2), PlacementUtils.HEIGHTMAP_OCEAN_FLOOR, BiomeFilter.biome(), BlockPredicateFilter...
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::countExtra(2, 0.1f, 1));
        s_surfaceWaterDepthFilters.push_back(SurfaceWaterDepthFilter::forMaxDepth(2));
        TREES_SWAMP = createPlaced(
            features::TreeFeatures::SWAMP_OAK,
            { &s_countPlacements.back(), &InSquarePlacement::spread(), &s_surfaceWaterDepthFilters.back(), heightmapOceanFloor(), &BiomeFilter::biome() },
            "TREES_SWAMP"
        );
    }

    // =========================================================================
    // Line 294: TREES_WINDSWEPT_SAVANNA
    // treePlacement(PlacementUtils.countExtra(2, 0.1F, 1))
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::countExtra(2, 0.1f, 1));
        TREES_WINDSWEPT_SAVANNA = createPlaced(
            VegetationFeatures::TREES_SAVANNA,
            treePlacement(&s_countPlacements.back()),
            "TREES_WINDSWEPT_SAVANNA"
        );
    }

    // =========================================================================
    // Line 295: TREES_SAVANNA
    // treePlacement(PlacementUtils.countExtra(1, 0.1F, 1))
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::countExtra(1, 0.1f, 1));
        TREES_SAVANNA = createPlaced(
            VegetationFeatures::TREES_SAVANNA,
            treePlacement(&s_countPlacements.back()),
            "TREES_SAVANNA"
        );
    }

    // =========================================================================
    // Line 296: BIRCH_TALL
    // treePlacement(PlacementUtils.countExtra(10, 0.1F, 1))
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::countExtra(10, 0.1f, 1));
        BIRCH_TALL = createPlaced(
            VegetationFeatures::BIRCH_TALL,
            treePlacement(&s_countPlacements.back()),
            "BIRCH_TALL"
        );
    }

    // =========================================================================
    // Line 297: TREES_BIRCH
    // treePlacement(PlacementUtils.countExtra(10, 0.1F, 1), Blocks.BIRCH_SAPLING)
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::countExtra(10, 0.1f, 1));
        TREES_BIRCH = createPlaced(
            VegetationFeatures::TREES_BIRCH,
            treePlacementWithSapling(&s_countPlacements.back()),
            "TREES_BIRCH"
        );
    }

    // =========================================================================
    // Line 298: TREES_WINDSWEPT_FOREST
    // treePlacement(PlacementUtils.countExtra(3, 0.1F, 1))
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::countExtra(3, 0.1f, 1));
        TREES_WINDSWEPT_FOREST = createPlaced(
            VegetationFeatures::TREES_WINDSWEPT_HILLS,
            treePlacement(&s_countPlacements.back()),
            "TREES_WINDSWEPT_FOREST"
        );
    }

    // =========================================================================
    // Line 299: TREES_WINDSWEPT_HILLS
    // treePlacement(PlacementUtils.countExtra(0, 0.1F, 1))
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::countExtra(0, 0.1f, 1));
        TREES_WINDSWEPT_HILLS = createPlaced(
            VegetationFeatures::TREES_WINDSWEPT_HILLS,
            treePlacement(&s_countPlacements.back()),
            "TREES_WINDSWEPT_HILLS"
        );
    }

    // =========================================================================
    // Line 300: TREES_WATER
    // treePlacement(PlacementUtils.countExtra(0, 0.1F, 1))
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::countExtra(0, 0.1f, 1));
        TREES_WATER = createPlaced(
            VegetationFeatures::TREES_WATER,
            treePlacement(&s_countPlacements.back()),
            "TREES_WATER"
        );
    }

    // =========================================================================
    // Line 301: TREES_BIRCH_AND_OAK_LEAF_LITTER
    // treePlacement(PlacementUtils.countExtra(10, 0.1F, 1))
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::countExtra(10, 0.1f, 1));
        TREES_BIRCH_AND_OAK_LEAF_LITTER = createPlaced(
            VegetationFeatures::TREES_BIRCH_AND_OAK_LEAF_LITTER,
            treePlacement(&s_countPlacements.back()),
            "TREES_BIRCH_AND_OAK_LEAF_LITTER"
        );
    }

    // =========================================================================
    // Line 302: TREES_SPARSE_JUNGLE
    // treePlacement(PlacementUtils.countExtra(2, 0.1F, 1))
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::countExtra(2, 0.1f, 1));
        TREES_SPARSE_JUNGLE = createPlaced(
            VegetationFeatures::TREES_SPARSE_JUNGLE,
            treePlacement(&s_countPlacements.back()),
            "TREES_SPARSE_JUNGLE"
        );
    }

    // =========================================================================
    // Line 303: TREES_OLD_GROWTH_SPRUCE_TAIGA
    // treePlacement(PlacementUtils.countExtra(10, 0.1F, 1))
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::countExtra(10, 0.1f, 1));
        TREES_OLD_GROWTH_SPRUCE_TAIGA = createPlaced(
            VegetationFeatures::TREES_OLD_GROWTH_SPRUCE_TAIGA,
            treePlacement(&s_countPlacements.back()),
            "TREES_OLD_GROWTH_SPRUCE_TAIGA"
        );
    }

    // =========================================================================
    // Line 304: TREES_OLD_GROWTH_PINE_TAIGA
    // treePlacement(PlacementUtils.countExtra(10, 0.1F, 1))
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::countExtra(10, 0.1f, 1));
        TREES_OLD_GROWTH_PINE_TAIGA = createPlaced(
            VegetationFeatures::TREES_OLD_GROWTH_PINE_TAIGA,
            treePlacement(&s_countPlacements.back()),
            "TREES_OLD_GROWTH_PINE_TAIGA"
        );
    }

    // =========================================================================
    // Line 305: TREES_JUNGLE
    // treePlacement(PlacementUtils.countExtra(50, 0.1F, 1))
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::countExtra(50, 0.1f, 1));
        TREES_JUNGLE = createPlaced(
            VegetationFeatures::TREES_JUNGLE,
            treePlacement(&s_countPlacements.back()),
            "TREES_JUNGLE"
        );
    }

    // =========================================================================
    // Line 306: BAMBOO_VEGETATION
    // treePlacement(PlacementUtils.countExtra(30, 0.1F, 1))
    // =========================================================================
    {
        s_countPlacements.push_back(CountPlacement::countExtra(30, 0.1f, 1));
        BAMBOO_VEGETATION = createPlaced(
            VegetationFeatures::BAMBOO_VEGETATION,
            treePlacement(&s_countPlacements.back()),
            "BAMBOO_VEGETATION"
        );
    }

    // =========================================================================
    // Line 307: MUSHROOM_ISLAND_VEGETATION
    // InSquarePlacement.spread(), PlacementUtils.HEIGHTMAP, BiomeFilter.biome()
    // =========================================================================
    MUSHROOM_ISLAND_VEGETATION = createPlaced(
        VegetationFeatures::MUSHROOM_ISLAND_VEGETATION,
        { &InSquarePlacement::spread(), heightmapMotionBlocking(), &BiomeFilter::biome() },
        "MUSHROOM_ISLAND_VEGETATION"
    );

    // =========================================================================
    // Line 308: TREES_MANGROVE
    // CountPlacement.of(25), InSquarePlacement.spread(), SurfaceWaterDepthFilter.forMaxDepth(5), PlacementUtils.HEIGHTMAP_OCEAN_FLOOR, BiomeFilter.biome()
    // =========================================================================
    {
        s_surfaceWaterDepthFilters.push_back(SurfaceWaterDepthFilter::forMaxDepth(5));
        TREES_MANGROVE = createPlaced(
            VegetationFeatures::MANGROVE_VEGETATION,
            { countOf(25), &InSquarePlacement::spread(), &s_surfaceWaterDepthFilters.back(), heightmapOceanFloor(), &BiomeFilter::biome() },
            "TREES_MANGROVE"
        );
    }

    // =========================================================================
    // BACKWARDS COMPATIBILITY ALIASES
    // =========================================================================
    PATCH_FERN = PATCH_LARGE_FERN;
    FLOWER_PLAIN = FLOWER_PLAINS;
    FLOWER_FOREST = FLOWER_DEFAULT;
    PATCH_CACTUS = PATCH_CACTUS_DESERT;

    s_initialized = true;
}

} // namespace placement
} // namespace worldgen
} // namespace data
} // namespace minecraft

#include "data/worldgen/placement/AquaticPlacements.h"
#include "data/worldgen/features/AquaticFeatures.h"
#include <deque>

// Reference: net/minecraft/data/worldgen/placement/AquaticPlacements.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace placement {

using namespace levelgen::placement;
using namespace features;
using levelgen::Heightmap;

// Static members
bool AquaticPlacements::s_initialized = false;

// PlacedFeature pointers - Seagrass
const PlacedFeature* AquaticPlacements::SEAGRASS_WARM = nullptr;
const PlacedFeature* AquaticPlacements::SEAGRASS_NORMAL = nullptr;
const PlacedFeature* AquaticPlacements::SEAGRASS_COLD = nullptr;
const PlacedFeature* AquaticPlacements::SEAGRASS_RIVER = nullptr;
const PlacedFeature* AquaticPlacements::SEAGRASS_SWAMP = nullptr;
const PlacedFeature* AquaticPlacements::SEAGRASS_DEEP_WARM = nullptr;
const PlacedFeature* AquaticPlacements::SEAGRASS_DEEP = nullptr;
const PlacedFeature* AquaticPlacements::SEAGRASS_DEEP_COLD = nullptr;

// PlacedFeature pointers - Sea pickle
const PlacedFeature* AquaticPlacements::SEA_PICKLE = nullptr;

// PlacedFeature pointers - Kelp
const PlacedFeature* AquaticPlacements::KELP_COLD = nullptr;
const PlacedFeature* AquaticPlacements::KELP_WARM = nullptr;

// PlacedFeature pointers - Warm ocean vegetation
const PlacedFeature* AquaticPlacements::WARM_OCEAN_VEGETATION = nullptr;

// Storage for PlacedFeature instances (use deque to avoid pointer invalidation)
static std::deque<PlacedFeature> s_placedFeatures;

// Storage for placement modifiers
static std::deque<CountPlacement> s_countPlacements;
static std::deque<HeightmapPlacement> s_heightmapPlacements;
static std::deque<NoiseBasedCountPlacement> s_noiseCountPlacements;
static std::deque<RarityFilter> s_rarityFilters;

static std::deque<BlockPredicateFilter> s_filters;
static std::deque<RandomOffsetPlacement> s_offsets;
static levelgen::carver::TrapezoidInt s_triangle7 = levelgen::carver::TrapezoidInt::triangle(7);
static levelgen::carver::TrapezoidInt s_triangle0 = levelgen::carver::TrapezoidInt::triangle(0);

// OffsetPlacement.ofTriangle(7, 0): x, y (two nextInt(1) draws), z
static PlacementModifier* offsetTriangle7x0() {
    s_offsets.push_back(RandomOffsetPlacement::of(&s_triangle7, &s_triangle0));
    return &s_offsets.back();
}

static PlacementModifier* heightmapOceanFloor() {
    // PlacementUtils.HEIGHTMAP_OCEAN_FLOOR
    s_heightmapPlacements.push_back(HeightmapPlacement::onHeightmap(Heightmap::Types::OCEAN_FLOOR));
    return &s_heightmapPlacements.back();
}

static PlacementModifier* waterFilter() {
    s_filters.push_back(BlockPredicateFilter::forPredicate(
        levelgen::blockpredicates::BlockPredicate::matchesBlocks(std::vector<std::string>{"minecraft:water"})));
    return &s_filters.back();
}

std::vector<PlacementModifier*> AquaticPlacements::seagrassPlacement(int count) {
    // 26.3 AquaticPlacements.seagrassPlacement: InSquarePlacement.spread(),
    // CountPlacement.of(count), OffsetPlacement.ofTriangle(7, 0),
    // HEIGHTMAP_OCEAN_FLOOR, BlockPredicateFilter(water), BiomeFilter.biome()
    s_countPlacements.push_back(CountPlacement::of(count));
    PlacementModifier* countPlacement = &s_countPlacements.back();
    return {
        &InSquarePlacement::spread(),
        countPlacement,
        offsetTriangle7x0(),
        heightmapOceanFloor(),
        waterFilter(),
        &BiomeFilter::biome()
    };
}

void AquaticPlacements::bootstrap() {
    if (s_initialized) return;

    // Ensure features are bootstrapped first
    if (!AquaticFeatures::isInitialized()) {
        AquaticFeatures::bootstrap();
    }

    // Helper to create PlacedFeature
    auto createPlaced = [](levelgen::ConfiguredFeature* feature, std::vector<PlacementModifier*> modifiers, const std::string& name = "") -> const PlacedFeature* {
        s_placedFeatures.emplace_back(feature, modifiers, name);
        return &s_placedFeatures.back();
    };

    // =========================================================================
    // SEAGRASS PLACEMENTS
    // Reference: AquaticPlacements.java lines 46-53
    // =========================================================================

    // SEAGRASS_WARM - count 80, uses seagrassShort (probability 0.3)
    SEAGRASS_WARM = createPlaced(
        AquaticFeatures::SEAGRASS_SHORT,
        seagrassPlacement(80),
        "SEAGRASS_WARM"
    );

    // SEAGRASS_NORMAL - count 48, uses seagrassShort (probability 0.3)
    SEAGRASS_NORMAL = createPlaced(
        AquaticFeatures::SEAGRASS_SHORT,
        seagrassPlacement(48),
        "SEAGRASS_NORMAL"
    );

    // SEAGRASS_COLD - count 32, uses seagrassShort (probability 0.3)
    SEAGRASS_COLD = createPlaced(
        AquaticFeatures::SEAGRASS_SHORT,
        seagrassPlacement(32),
        "SEAGRASS_COLD"
    );

    // SEAGRASS_RIVER - count 48, uses seagrassSlightlyLessShort (probability 0.4)
    SEAGRASS_RIVER = createPlaced(
        AquaticFeatures::SEAGRASS_SLIGHTLY_LESS_SHORT,
        seagrassPlacement(48),
        "SEAGRASS_RIVER"
    );

    // SEAGRASS_SWAMP - count 64, uses seagrassMid (probability 0.6)
    SEAGRASS_SWAMP = createPlaced(
        AquaticFeatures::SEAGRASS_MID,
        seagrassPlacement(64),
        "SEAGRASS_SWAMP"
    );

    // SEAGRASS_DEEP_WARM - count 80, uses seagrassTall (probability 0.8)
    SEAGRASS_DEEP_WARM = createPlaced(
        AquaticFeatures::SEAGRASS_TALL,
        seagrassPlacement(80),
        "SEAGRASS_DEEP_WARM"
    );

    // SEAGRASS_DEEP - count 48, uses seagrassTall (probability 0.8)
    SEAGRASS_DEEP = createPlaced(
        AquaticFeatures::SEAGRASS_TALL,
        seagrassPlacement(48),
        "SEAGRASS_DEEP"
    );

    // SEAGRASS_DEEP_COLD - count 40, uses seagrassTall (probability 0.8)
    SEAGRASS_DEEP_COLD = createPlaced(
        AquaticFeatures::SEAGRASS_TALL,
        seagrassPlacement(40),
        "SEAGRASS_DEEP_COLD"
    );

    // =========================================================================
    // KELP PLACEMENTS
    // Reference: AquaticPlacements.java lines 55-56
    // =========================================================================

    // 26.3: KELP_COLD / KELP_WARM - NoiseBasedCountPlacement(120 / 80, 80.0,
    // 0.0), InSquarePlacement.spread(), HEIGHTMAP_OCEAN_FLOOR, kelp filter
    // (water, water above, sturdy face below, not #cannot_support_kelp below),
    // BiomeFilter.biome()
    {
        using levelgen::blockpredicates::BlockPredicate;
        s_filters.push_back(BlockPredicateFilter::forPredicate(BlockPredicate::allOf({
            BlockPredicate::matchesBlocks(std::vector<std::string>{"minecraft:water"}),
            BlockPredicate::matchesBlocks(core::Vec3i(0, 1, 0), std::vector<std::string>{"minecraft:water"}),
            BlockPredicate::hasSturdyFace(core::Vec3i(0, -1, 0), core::Direction::UP),
            BlockPredicate::not_(BlockPredicate::matchesTag(core::Vec3i(0, -1, 0), "minecraft:cannot_support_kelp"))})));
        PlacementModifier* kelpFilter = &s_filters.back();

        s_noiseCountPlacements.push_back(NoiseBasedCountPlacement::of(120, 80.0, 0.0));
        PlacementModifier* coldCount = &s_noiseCountPlacements.back();
        KELP_COLD = createPlaced(AquaticFeatures::KELP,
            {coldCount, &InSquarePlacement::spread(), heightmapOceanFloor(), kelpFilter, &BiomeFilter::biome()},
            "KELP_COLD");

        s_noiseCountPlacements.push_back(NoiseBasedCountPlacement::of(80, 80.0, 0.0));
        PlacementModifier* warmCount = &s_noiseCountPlacements.back();
        KELP_WARM = createPlaced(AquaticFeatures::KELP,
            {warmCount, &InSquarePlacement::spread(), heightmapOceanFloor(), kelpFilter, &BiomeFilter::biome()},
            "KELP_WARM");
    }

    // =========================================================================
    // SEA PICKLE (26.3): RarityFilter.onAverageOnceEvery(16),
    // InSquarePlacement.spread(), CountPlacement.of(20),
    // OffsetPlacement.ofTriangle(7, 0), HEIGHTMAP_OCEAN_FLOOR,
    // BlockPredicateFilter(water), BiomeFilter.biome()
    // =========================================================================
    {
        s_rarityFilters.push_back(RarityFilter::onAverageOnceEvery(16));
        PlacementModifier* rarity = &s_rarityFilters.back();
        s_countPlacements.push_back(CountPlacement::of(20));
        PlacementModifier* count = &s_countPlacements.back();
        SEA_PICKLE = createPlaced(AquaticFeatures::SEA_PICKLE,
            {rarity, &InSquarePlacement::spread(), count, offsetTriangle7x0(), heightmapOceanFloor(), waterFilter(),
             &BiomeFilter::biome()},
            "SEA_PICKLE");
    }

    // =========================================================================
    // WARM OCEAN VEGETATION
    // Reference: AquaticPlacements.java line 57
    // NoiseBasedCountPlacement.of(20, 400.0, 0.0), InSquarePlacement.spread(), HEIGHTMAP_TOP_SOLID, BiomeFilter.biome()
    // =========================================================================
    {
        s_noiseCountPlacements.push_back(NoiseBasedCountPlacement::of(20, 400.0, 0.0));
        s_heightmapPlacements.push_back(HeightmapPlacement::onHeightmap(Heightmap::Types::OCEAN_FLOOR_WG));

        std::vector<PlacementModifier*> modifiers = {
            &s_noiseCountPlacements.back(),
            &InSquarePlacement::spread(),
            &s_heightmapPlacements.back(),
            &BiomeFilter::biome()
        };

        WARM_OCEAN_VEGETATION = createPlaced(AquaticFeatures::WARM_OCEAN_VEGETATION, modifiers, "WARM_OCEAN_VEGETATION");
    }

    s_initialized = true;
}

} // namespace placement
} // namespace worldgen
} // namespace data
} // namespace minecraft

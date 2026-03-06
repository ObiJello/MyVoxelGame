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

std::vector<PlacementModifier*> AquaticPlacements::seagrassPlacement(int count) {
    // Reference: AquaticPlacements.java line 33-35
    // InSquarePlacement.spread(), HEIGHTMAP_TOP_SOLID, CountPlacement.of(count), BiomeFilter.biome()

    s_countPlacements.push_back(CountPlacement::of(count));
    // Use OCEAN_FLOOR for underwater placement (HEIGHTMAP_TOP_SOLID equivalent for ocean floor)
    s_heightmapPlacements.push_back(HeightmapPlacement::onHeightmap(Heightmap::Types::OCEAN_FLOOR));

    return {
        &InSquarePlacement::spread(),
        &s_heightmapPlacements.back(),
        &s_countPlacements.back(),
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

    // KELP_COLD - NoiseBasedCountPlacement(120, 80.0, 0.0)
    // Reference: AquaticPlacements.java line 55
    {
        s_noiseCountPlacements.push_back(NoiseBasedCountPlacement::of(120, 80.0, 0.0));
        s_heightmapPlacements.push_back(HeightmapPlacement::onHeightmap(Heightmap::Types::OCEAN_FLOOR));

        std::vector<PlacementModifier*> modifiers = {
            &s_noiseCountPlacements.back(),
            &InSquarePlacement::spread(),
            &s_heightmapPlacements.back(),
            &BiomeFilter::biome()
        };

        KELP_COLD = createPlaced(AquaticFeatures::KELP, modifiers, "KELP_COLD");
    }

    // KELP_WARM - NoiseBasedCountPlacement(80, 80.0, 0.0)
    // Reference: AquaticPlacements.java line 56
    {
        s_noiseCountPlacements.push_back(NoiseBasedCountPlacement::of(80, 80.0, 0.0));
        s_heightmapPlacements.push_back(HeightmapPlacement::onHeightmap(Heightmap::Types::OCEAN_FLOOR));

        std::vector<PlacementModifier*> modifiers = {
            &s_noiseCountPlacements.back(),
            &InSquarePlacement::spread(),
            &s_heightmapPlacements.back(),
            &BiomeFilter::biome()
        };

        KELP_WARM = createPlaced(AquaticFeatures::KELP, modifiers, "KELP_WARM");
    }

    // =========================================================================
    // SEA PICKLE
    // Reference: AquaticPlacements.java line 54
    // RarityFilter.onAverageOnceEvery(16), InSquarePlacement.spread(), HEIGHTMAP_TOP_SOLID, BiomeFilter.biome()
    // =========================================================================
    {
        s_rarityFilters.push_back(RarityFilter::onAverageOnceEvery(16));
        s_heightmapPlacements.push_back(HeightmapPlacement::onHeightmap(Heightmap::Types::OCEAN_FLOOR));

        std::vector<PlacementModifier*> modifiers = {
            &s_rarityFilters.back(),
            &InSquarePlacement::spread(),
            &s_heightmapPlacements.back(),
            &BiomeFilter::biome()
        };

        SEA_PICKLE = createPlaced(AquaticFeatures::SEA_PICKLE, modifiers, "SEA_PICKLE");
    }

    // =========================================================================
    // WARM OCEAN VEGETATION
    // Reference: AquaticPlacements.java line 57
    // NoiseBasedCountPlacement.of(20, 400.0, 0.0), InSquarePlacement.spread(), HEIGHTMAP_TOP_SOLID, BiomeFilter.biome()
    // =========================================================================
    {
        s_noiseCountPlacements.push_back(NoiseBasedCountPlacement::of(20, 400.0, 0.0));
        s_heightmapPlacements.push_back(HeightmapPlacement::onHeightmap(Heightmap::Types::OCEAN_FLOOR));

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

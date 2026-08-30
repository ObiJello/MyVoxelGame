#include "data/worldgen/placement/EndPlacements.h"

// Reference: net/minecraft/data/worldgen/placement/EndPlacements.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace placement {

bool EndPlacements::s_initialized = false;

PlacedFeature* EndPlacements::END_PLATFORM = nullptr;
PlacedFeature* EndPlacements::END_SPIKE = nullptr;
PlacedFeature* EndPlacements::END_GATEWAY_RETURN = nullptr;
PlacedFeature* EndPlacements::CHORUS_PLANT = nullptr;
PlacedFeature* EndPlacements::END_ISLAND_DECORATED = nullptr;

static std::vector<std::unique_ptr<PlacementModifier>> s_modifiers;
static std::vector<std::unique_ptr<PlacedFeature>> s_placedFeatures;
static std::deque<carver::UniformInt> s_uniformInts;

void EndPlacements::bootstrap() {
    if (s_initialized) return;

    using features::EndFeatures;
    if (!EndFeatures::isInitialized()) {
        EndFeatures::bootstrap();
    }

    auto storeModifier = [](std::unique_ptr<PlacementModifier> mod) -> PlacementModifier* {
        PlacementModifier* raw = mod.get();
        s_modifiers.push_back(std::move(mod));
        return raw;
    };

    auto createPlaced = [](ConfiguredFeature* config,
                           const std::vector<PlacementModifier*>& modifiers,
                           const std::string& name) -> PlacedFeature* {
        auto feature = std::make_unique<PlacedFeature>(config, modifiers, name);
        PlacedFeature* ptr = feature.get();
        s_placedFeatures.push_back(std::move(feature));
        return ptr;
    };

    PlacementModifier* inSquare = &InSquarePlacement::spread();
    PlacementModifier* biome = &BiomeFilter::biome();
    auto heightmapMotionBlocking = [&]() -> PlacementModifier* {
        return storeModifier(std::make_unique<HeightmapPlacement>(
            HeightmapPlacement::onHeightmap(Heightmap::Types::MOTION_BLOCKING)));
    };

    // Reference: EndPlacements.java bootstrap(), in order.
    // END_PLATFORM: FixedPlacement.of(END_SPAWN_POINT(100,50,0).below()), biome
    END_PLATFORM = createPlaced(EndFeatures::END_PLATFORM,
        {storeModifier(std::make_unique<FixedPlacement>(FixedPlacement::of(
             {core::BlockPos(100, 49, 0)}))),
         biome},
        "END_PLATFORM");

    // END_SPIKE: biome only
    END_SPIKE = createPlaced(EndFeatures::END_SPIKE, {biome}, "END_SPIKE");

    // END_GATEWAY_RETURN: rarity 700, in_square, HEIGHTMAP,
    // RandomOffsetPlacement.vertical(UniformInt(3,9)), biome
    {
        static std::deque<RarityFilter> s_rarity;
        s_rarity.push_back(RarityFilter::onAverageOnceEvery(700));
        s_uniformInts.push_back(carver::UniformInt(3, 9));
        END_GATEWAY_RETURN = createPlaced(EndFeatures::END_GATEWAY_RETURN,
            {&s_rarity.back(), inSquare, heightmapMotionBlocking(),
             storeModifier(std::make_unique<RandomOffsetPlacement>(
                 RandomOffsetPlacement::vertical(&s_uniformInts.back()))),
             biome},
            "END_GATEWAY_RETURN");
    }

    // CHORUS_PLANT: count(UniformInt(0,4)), in_square, HEIGHTMAP, biome
    {
        s_uniformInts.push_back(carver::UniformInt(0, 4));
        CHORUS_PLANT = createPlaced(EndFeatures::CHORUS_PLANT,
            {storeModifier(std::make_unique<CountPlacement>(
                 CountPlacement::of(&s_uniformInts.back()))),
             inSquare, heightmapMotionBlocking(), biome},
            "CHORUS_PLANT");
    }

    // END_ISLAND_DECORATED: rarity 14, countExtra(1, 0.25, 1), in_square,
    // uniform(absolute(55), absolute(70)), biome
    {
        static std::deque<RarityFilter> s_rarity;
        s_rarity.push_back(RarityFilter::onAverageOnceEvery(14));
        END_ISLAND_DECORATED = createPlaced(EndFeatures::END_ISLAND,
            {&s_rarity.back(),
             storeModifier(std::make_unique<CountPlacement>(
                 CountPlacement::countExtra(1, 0.25f, 1))),
             inSquare,
             storeModifier(std::make_unique<HeightRangePlacement>(
                 HeightRangePlacement::uniform(VerticalAnchor::absolute(55),
                                               VerticalAnchor::absolute(70)))),
             biome},
            "END_ISLAND_DECORATED");
    }

    s_initialized = true;
}

} // namespace placement
} // namespace worldgen
} // namespace data
} // namespace minecraft

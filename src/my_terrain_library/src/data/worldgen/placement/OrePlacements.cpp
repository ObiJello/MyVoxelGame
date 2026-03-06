#include "data/worldgen/placement/OrePlacements.h"
#include "levelgen/carver/CarverConfiguration.h"

// Reference: net/minecraft/data/worldgen/placement/OrePlacements.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace placement {

using namespace levelgen;
using namespace levelgen::placement;
using namespace levelgen::carver;
using namespace features;

// Static member initialization
bool OrePlacements::s_initialized = false;

PlacedFeature* OrePlacements::ORE_COAL_UPPER = nullptr;
PlacedFeature* OrePlacements::ORE_COAL_LOWER = nullptr;
PlacedFeature* OrePlacements::ORE_IRON_UPPER = nullptr;
PlacedFeature* OrePlacements::ORE_IRON_MIDDLE = nullptr;
PlacedFeature* OrePlacements::ORE_IRON_SMALL = nullptr;
PlacedFeature* OrePlacements::ORE_GOLD = nullptr;
PlacedFeature* OrePlacements::ORE_GOLD_LOWER = nullptr;
PlacedFeature* OrePlacements::ORE_REDSTONE = nullptr;
PlacedFeature* OrePlacements::ORE_REDSTONE_LOWER = nullptr;
PlacedFeature* OrePlacements::ORE_DIAMOND = nullptr;
PlacedFeature* OrePlacements::ORE_DIAMOND_MEDIUM = nullptr;
PlacedFeature* OrePlacements::ORE_DIAMOND_LARGE = nullptr;
PlacedFeature* OrePlacements::ORE_DIAMOND_BURIED = nullptr;
PlacedFeature* OrePlacements::ORE_LAPIS = nullptr;
PlacedFeature* OrePlacements::ORE_LAPIS_BURIED = nullptr;
PlacedFeature* OrePlacements::ORE_COPPER = nullptr;
PlacedFeature* OrePlacements::ORE_COPPER_LARGE = nullptr;
PlacedFeature* OrePlacements::ORE_GOLD_EXTRA = nullptr;
PlacedFeature* OrePlacements::ORE_EMERALD = nullptr;
PlacedFeature* OrePlacements::ORE_INFESTED = nullptr;
PlacedFeature* OrePlacements::ORE_CLAY = nullptr;
PlacedFeature* OrePlacements::ORE_DIRT = nullptr;
PlacedFeature* OrePlacements::ORE_GRAVEL = nullptr;
PlacedFeature* OrePlacements::ORE_GRANITE_UPPER = nullptr;
PlacedFeature* OrePlacements::ORE_GRANITE_LOWER = nullptr;
PlacedFeature* OrePlacements::ORE_DIORITE_UPPER = nullptr;
PlacedFeature* OrePlacements::ORE_DIORITE_LOWER = nullptr;
PlacedFeature* OrePlacements::ORE_ANDESITE_UPPER = nullptr;
PlacedFeature* OrePlacements::ORE_ANDESITE_LOWER = nullptr;
PlacedFeature* OrePlacements::ORE_TUFF = nullptr;

// Storage for placement modifiers and features
static std::vector<std::unique_ptr<PlacementModifier>> s_modifiers;
static std::vector<std::unique_ptr<PlacedFeature>> s_placedFeatures;

std::vector<PlacementModifier*> OrePlacements::commonOrePlacement(
    int32_t count,
    PlacementModifier* heightRange
) {
    // Reference: OrePlacements.java commonOrePlacement()
    // List.of(CountPlacement.of(count), InSquarePlacement.spread(), heightRange, BiomeFilter.biome())
    auto countMod = std::make_unique<CountPlacement>(CountPlacement::of(count));
    PlacementModifier* countPtr = countMod.get();
    s_modifiers.push_back(std::move(countMod));

    return {
        countPtr,
        &InSquarePlacement::spread(),
        heightRange,
        &BiomeFilter::biome()
    };
}

std::vector<PlacementModifier*> OrePlacements::rareOrePlacement(
    int32_t rarity,
    PlacementModifier* heightRange
) {
    // Reference: OrePlacements.java rareOrePlacement()
    // List.of(RarityFilter.onAverageOnceEvery(rarity), InSquarePlacement.spread(), heightRange, BiomeFilter.biome())
    auto rarityMod = std::make_unique<RarityFilter>(RarityFilter::onAverageOnceEvery(rarity));
    PlacementModifier* rarityPtr = rarityMod.get();
    s_modifiers.push_back(std::move(rarityMod));

    return {
        rarityPtr,
        &InSquarePlacement::spread(),
        heightRange,
        &BiomeFilter::biome()
    };
}

void OrePlacements::bootstrap() {
    if (s_initialized) return;

    // Ensure OreFeatures is initialized
    if (!OreFeatures::isInitialized()) {
        OreFeatures::bootstrap();
    }

    // Helper to create and store height range modifiers
    auto uniformHeight = [](int32_t min, int32_t max) -> PlacementModifier* {
        auto mod = std::make_unique<HeightRangePlacement>(
            HeightRangePlacement::uniform(
                VerticalAnchor::absolute(min),
                VerticalAnchor::absolute(max)
            )
        );
        PlacementModifier* ptr = mod.get();
        s_modifiers.push_back(std::move(mod));
        return ptr;
    };

    auto triangleHeight = [](int32_t min, int32_t max) -> PlacementModifier* {
        auto mod = std::make_unique<HeightRangePlacement>(
            HeightRangePlacement::triangle(
                VerticalAnchor::absolute(min),
                VerticalAnchor::absolute(max)
            )
        );
        PlacementModifier* ptr = mod.get();
        s_modifiers.push_back(std::move(mod));
        return ptr;
    };

    auto aboveBottomTriangle = [](int32_t minOffset, int32_t maxOffset) -> PlacementModifier* {
        auto mod = std::make_unique<HeightRangePlacement>(
            HeightRangePlacement::triangle(
                VerticalAnchor::aboveBottom(minOffset),
                VerticalAnchor::aboveBottom(maxOffset)
            )
        );
        PlacementModifier* ptr = mod.get();
        s_modifiers.push_back(std::move(mod));
        return ptr;
    };

    auto uniformBottomToTop = []() -> PlacementModifier* {
        auto mod = std::make_unique<HeightRangePlacement>(
            HeightRangePlacement::uniform(
                VerticalAnchor::bottom(),
                VerticalAnchor::top()
            )
        );
        PlacementModifier* ptr = mod.get();
        s_modifiers.push_back(std::move(mod));
        return ptr;
    };

    // Helper to create and store placed features
    auto createPlaced = [](ConfiguredFeature* config, std::vector<PlacementModifier*> modifiers, const std::string& name = "") -> PlacedFeature* {
        auto feature = std::make_unique<PlacedFeature>(config, modifiers, name);
        PlacedFeature* ptr = feature.get();
        s_placedFeatures.push_back(std::move(feature));
        return ptr;
    };

    // Reference: OrePlacements.java bootstrap() lines 126-148

    // Coal - line 126-127
    ORE_COAL_UPPER = createPlaced(OreFeatures::ORE_COAL,
        commonOrePlacement(30, uniformHeight(136, 320)), "ORE_COAL_UPPER");
    ORE_COAL_LOWER = createPlaced(OreFeatures::ORE_COAL_BURIED,
        commonOrePlacement(20, triangleHeight(0, 192)), "ORE_COAL_LOWER");

    // Iron - line 128-130
    ORE_IRON_UPPER = createPlaced(OreFeatures::ORE_IRON,
        commonOrePlacement(90, triangleHeight(80, 384)), "ORE_IRON_UPPER");
    ORE_IRON_MIDDLE = createPlaced(OreFeatures::ORE_IRON,
        commonOrePlacement(10, triangleHeight(-24, 56)), "ORE_IRON_MIDDLE");
    ORE_IRON_SMALL = createPlaced(OreFeatures::ORE_IRON_SMALL,
        commonOrePlacement(10, uniformHeight(-64, 72)), "ORE_IRON_SMALL");

    // Gold - line 132-133
    ORE_GOLD = createPlaced(OreFeatures::ORE_GOLD_BURIED,
        commonOrePlacement(4, triangleHeight(-64, 32)), "ORE_GOLD");
    // Java: CountPlacement.of(UniformInt.of(0, 1)) - random count 0 or 1!
    // Reference: OrePlacements.java line 133
    {
        // Use static storage for IntProvider to keep it alive
        static std::deque<carver::UniformInt> s_intProviders;
        s_intProviders.push_back(carver::UniformInt(0, 1));
        auto countMod = std::make_unique<CountPlacement>(CountPlacement::of(&s_intProviders.back()));
        PlacementModifier* countPtr = countMod.get();
        s_modifiers.push_back(std::move(countMod));
        ORE_GOLD_LOWER = createPlaced(OreFeatures::ORE_GOLD_BURIED,
            {countPtr, &InSquarePlacement::spread(), uniformHeight(-64, -48), &BiomeFilter::biome()},
            "ORE_GOLD_LOWER");
    }

    // Redstone - line 134-135
    ORE_REDSTONE = createPlaced(OreFeatures::ORE_REDSTONE,
        commonOrePlacement(4, uniformHeight(-64, 15)), "ORE_REDSTONE");
    ORE_REDSTONE_LOWER = createPlaced(OreFeatures::ORE_REDSTONE,
        commonOrePlacement(8, aboveBottomTriangle(-32, 32)), "ORE_REDSTONE_LOWER");

    // Diamond - line 136-139
    ORE_DIAMOND = createPlaced(OreFeatures::ORE_DIAMOND_SMALL,
        commonOrePlacement(7, aboveBottomTriangle(-80, 80)), "ORE_DIAMOND");
    ORE_DIAMOND_MEDIUM = createPlaced(OreFeatures::ORE_DIAMOND_MEDIUM,
        commonOrePlacement(2, uniformHeight(-64, -4)), "ORE_DIAMOND_MEDIUM");
    ORE_DIAMOND_LARGE = createPlaced(OreFeatures::ORE_DIAMOND_LARGE,
        rareOrePlacement(9, aboveBottomTriangle(-80, 80)), "ORE_DIAMOND_LARGE");
    ORE_DIAMOND_BURIED = createPlaced(OreFeatures::ORE_DIAMOND_BURIED,
        commonOrePlacement(4, aboveBottomTriangle(-80, 80)), "ORE_DIAMOND_BURIED");

    // Lapis - line 140-141
    ORE_LAPIS = createPlaced(OreFeatures::ORE_LAPIS,
        commonOrePlacement(2, triangleHeight(-32, 32)), "ORE_LAPIS");
    ORE_LAPIS_BURIED = createPlaced(OreFeatures::ORE_LAPIS_BURIED,
        commonOrePlacement(4, uniformHeight(-64, 64)), "ORE_LAPIS_BURIED");

    // Copper - line 146
    ORE_COPPER = createPlaced(OreFeatures::ORE_COPPER_SMALL,
        commonOrePlacement(16, triangleHeight(-16, 112)), "ORE_COPPER");

    // Copper Large - for dripstone caves
    // Reference: OrePlacements.java line 148
    ORE_COPPER_LARGE = createPlaced(OreFeatures::ORE_COPPER_LARGE,
        commonOrePlacement(16, triangleHeight(-16, 112)), "ORE_COPPER_LARGE");

    // Extra Gold - for badlands/mesa biomes
    // Reference: OrePlacements.java line 133
    ORE_GOLD_EXTRA = createPlaced(OreFeatures::ORE_GOLD_BURIED,
        commonOrePlacement(50, uniformHeight(32, 256)), "ORE_GOLD_EXTRA");

    // Emerald - line 143
    ORE_EMERALD = createPlaced(OreFeatures::ORE_EMERALD,
        commonOrePlacement(100, triangleHeight(-16, 480)), "ORE_EMERALD");

    // Infested stone - line 142
    // Reference: commonOrePlacement(14, HeightRangePlacement.uniform(aboveBottom(0), absolute(63)))
    ORE_INFESTED = createPlaced(OreFeatures::ORE_INFESTED,
        commonOrePlacement(14, uniformHeight(-64, 63)), "ORE_INFESTED");

    // Clay - for lush caves
    // Reference: OrePlacements.java line 148 - uses RANGE_BOTTOM_TO_MAX_TERRAIN_HEIGHT
    // RANGE_BOTTOM_TO_MAX_TERRAIN_HEIGHT = uniform(bottom(), absolute(256))
    ORE_CLAY = createPlaced(OreFeatures::ORE_CLAY,
        commonOrePlacement(46, uniformHeight(-64, 256)), "ORE_CLAY");

    // Stone variants - line 117-125
    ORE_DIRT = createPlaced(OreFeatures::ORE_DIRT,
        commonOrePlacement(7, uniformHeight(0, 160)), "ORE_DIRT");
    ORE_GRAVEL = createPlaced(OreFeatures::ORE_GRAVEL,
        commonOrePlacement(14, uniformBottomToTop()), "ORE_GRAVEL");
    ORE_GRANITE_UPPER = createPlaced(OreFeatures::ORE_GRANITE,
        rareOrePlacement(6, uniformHeight(64, 128)), "ORE_GRANITE_UPPER");
    ORE_GRANITE_LOWER = createPlaced(OreFeatures::ORE_GRANITE,
        commonOrePlacement(2, uniformHeight(0, 60)), "ORE_GRANITE_LOWER");
    ORE_DIORITE_UPPER = createPlaced(OreFeatures::ORE_DIORITE,
        rareOrePlacement(6, uniformHeight(64, 128)), "ORE_DIORITE_UPPER");
    ORE_DIORITE_LOWER = createPlaced(OreFeatures::ORE_DIORITE,
        commonOrePlacement(2, uniformHeight(0, 60)), "ORE_DIORITE_LOWER");
    ORE_ANDESITE_UPPER = createPlaced(OreFeatures::ORE_ANDESITE,
        rareOrePlacement(6, uniformHeight(64, 128)), "ORE_ANDESITE_UPPER");
    ORE_ANDESITE_LOWER = createPlaced(OreFeatures::ORE_ANDESITE,
        commonOrePlacement(2, uniformHeight(0, 60)), "ORE_ANDESITE_LOWER");
    ORE_TUFF = createPlaced(OreFeatures::ORE_TUFF,
        commonOrePlacement(2, uniformHeight(-64, 0)), "ORE_TUFF");

    s_initialized = true;
}

} // namespace placement
} // namespace worldgen
} // namespace data
} // namespace minecraft

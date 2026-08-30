#include "data/worldgen/placement/NetherPlacements.h"

// Reference: net/minecraft/data/worldgen/placement/NetherPlacements.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace placement {

bool NetherPlacements::s_initialized = false;

PlacedFeature* NetherPlacements::DELTA = nullptr;
PlacedFeature* NetherPlacements::SMALL_BASALT_COLUMNS = nullptr;
PlacedFeature* NetherPlacements::LARGE_BASALT_COLUMNS = nullptr;
PlacedFeature* NetherPlacements::BASALT_BLOBS = nullptr;
PlacedFeature* NetherPlacements::BLACKSTONE_BLOBS = nullptr;
PlacedFeature* NetherPlacements::GLOWSTONE_EXTRA = nullptr;
PlacedFeature* NetherPlacements::GLOWSTONE = nullptr;
PlacedFeature* NetherPlacements::CRIMSON_FOREST_VEGETATION = nullptr;
PlacedFeature* NetherPlacements::WARPED_FOREST_VEGETATION = nullptr;
PlacedFeature* NetherPlacements::NETHER_SPROUTS = nullptr;
PlacedFeature* NetherPlacements::TWISTING_VINES = nullptr;
PlacedFeature* NetherPlacements::WEEPING_VINES = nullptr;
PlacedFeature* NetherPlacements::PATCH_CRIMSON_ROOTS = nullptr;
PlacedFeature* NetherPlacements::BASALT_PILLAR = nullptr;
PlacedFeature* NetherPlacements::SPRING_DELTA = nullptr;
PlacedFeature* NetherPlacements::SPRING_CLOSED = nullptr;
PlacedFeature* NetherPlacements::SPRING_CLOSED_DOUBLE = nullptr;
PlacedFeature* NetherPlacements::SPRING_OPEN = nullptr;
PlacedFeature* NetherPlacements::PATCH_SOUL_FIRE = nullptr;
PlacedFeature* NetherPlacements::PATCH_FIRE = nullptr;

// Owned storage
static std::vector<std::unique_ptr<PlacementModifier>> s_modifiers;
static std::vector<std::unique_ptr<PlacedFeature>> s_placedFeatures;
static std::deque<carver::BiasedToBottomInt> s_biasedInts;
static std::deque<carver::UniformInt> s_uniformInts;

void NetherPlacements::bootstrap() {
    if (s_initialized) return;

    using features::NetherFeatures;
    if (!NetherFeatures::isInitialized()) {
        NetherFeatures::bootstrap();
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

    auto countOnEveryLayer = [&](int32_t count) -> PlacementModifier* {
        return storeModifier(std::make_unique<CountOnEveryLayerPlacement>(
            CountOnEveryLayerPlacement::of(count)));
    };

    auto count = [&](int32_t c) -> PlacementModifier* {
        return storeModifier(std::make_unique<CountPlacement>(CountPlacement::of(c)));
    };

    auto heightUniform = [&](const VerticalAnchor& min, const VerticalAnchor& max) -> PlacementModifier* {
        return storeModifier(std::make_unique<HeightRangePlacement>(
            HeightRangePlacement::uniform(min, max)));
    };

    // PlacementUtils constants (anchor-relative; resolve per placement context)
    auto fullRange = [&]() -> PlacementModifier* {
        return heightUniform(VerticalAnchor::bottom(), VerticalAnchor::top());
    };
    auto range44 = [&]() -> PlacementModifier* {
        return heightUniform(VerticalAnchor::aboveBottom(4), VerticalAnchor::belowTop(4));
    };
    auto range1010 = [&]() -> PlacementModifier* {
        return heightUniform(VerticalAnchor::aboveBottom(10), VerticalAnchor::belowTop(10));
    };

    PlacementModifier* inSquare = &InSquarePlacement::spread();
    PlacementModifier* biome = &BiomeFilter::biome();

    // Reference: NetherPlacements.java bootstrap(), in order
    DELTA = createPlaced(NetherFeatures::DELTA,
        {countOnEveryLayer(40), biome}, "DELTA");
    SMALL_BASALT_COLUMNS = createPlaced(NetherFeatures::SMALL_BASALT_COLUMNS,
        {countOnEveryLayer(4), biome}, "SMALL_BASALT_COLUMNS");
    LARGE_BASALT_COLUMNS = createPlaced(NetherFeatures::LARGE_BASALT_COLUMNS,
        {countOnEveryLayer(2), biome}, "LARGE_BASALT_COLUMNS");
    BASALT_BLOBS = createPlaced(NetherFeatures::BASALT_BLOBS,
        {count(75), inSquare, fullRange(), biome}, "BASALT_BLOBS");
    BLACKSTONE_BLOBS = createPlaced(NetherFeatures::BLACKSTONE_BLOBS,
        {count(25), inSquare, fullRange(), biome}, "BLACKSTONE_BLOBS");
    {
        s_biasedInts.push_back(carver::BiasedToBottomInt(0, 9));
        PlacementModifier* biasedCount = storeModifier(std::make_unique<CountPlacement>(
            CountPlacement::of(&s_biasedInts.back())));
        GLOWSTONE_EXTRA = createPlaced(NetherFeatures::GLOWSTONE_EXTRA,
            {biasedCount, inSquare, range44(), biome}, "GLOWSTONE_EXTRA");
    }
    // GLOWSTONE reuses the glowstone_extra configured feature
    GLOWSTONE = createPlaced(NetherFeatures::GLOWSTONE_EXTRA,
        {count(10), inSquare, fullRange(), biome}, "GLOWSTONE");
    CRIMSON_FOREST_VEGETATION = createPlaced(NetherFeatures::CRIMSON_FOREST_VEGETATION,
        {countOnEveryLayer(6), biome}, "CRIMSON_FOREST_VEGETATION");
    WARPED_FOREST_VEGETATION = createPlaced(NetherFeatures::WARPED_FOREST_VEGETATION,
        {countOnEveryLayer(5), biome}, "WARPED_FOREST_VEGETATION");
    NETHER_SPROUTS = createPlaced(NetherFeatures::NETHER_SPROUTS,
        {countOnEveryLayer(4), biome}, "NETHER_SPROUTS");
    TWISTING_VINES = createPlaced(NetherFeatures::TWISTING_VINES,
        {count(10), inSquare, fullRange(), biome}, "TWISTING_VINES");
    WEEPING_VINES = createPlaced(NetherFeatures::WEEPING_VINES,
        {count(10), inSquare, fullRange(), biome}, "WEEPING_VINES");
    PATCH_CRIMSON_ROOTS = createPlaced(NetherFeatures::PATCH_CRIMSON_ROOTS,
        {fullRange(), biome}, "PATCH_CRIMSON_ROOTS");
    BASALT_PILLAR = createPlaced(NetherFeatures::BASALT_PILLAR,
        {count(10), inSquare, fullRange(), biome}, "BASALT_PILLAR");
    SPRING_DELTA = createPlaced(NetherFeatures::SPRING_LAVA_NETHER,
        {count(16), inSquare, range44(), biome}, "SPRING_DELTA");
    SPRING_CLOSED = createPlaced(NetherFeatures::SPRING_NETHER_CLOSED,
        {count(16), inSquare, range1010(), biome}, "SPRING_CLOSED");
    SPRING_CLOSED_DOUBLE = createPlaced(NetherFeatures::SPRING_NETHER_CLOSED,
        {count(32), inSquare, range1010(), biome}, "SPRING_CLOSED_DOUBLE");
    SPRING_OPEN = createPlaced(NetherFeatures::SPRING_NETHER_OPEN,
        {count(8), inSquare, range44(), biome}, "SPRING_OPEN");
    // firePlacement = count(UniformInt(0,5)), in_square, RANGE_4_4, biome.
    // Java builds ONE list used by both; each C++ placed feature gets its own
    // equivalent modifiers (stateless - identical draws).
    auto fireCount = [&]() -> PlacementModifier* {
        s_uniformInts.push_back(carver::UniformInt(0, 5));
        return storeModifier(std::make_unique<CountPlacement>(
            CountPlacement::of(&s_uniformInts.back())));
    };
    PATCH_SOUL_FIRE = createPlaced(NetherFeatures::PATCH_SOUL_FIRE,
        {fireCount(), inSquare, range44(), biome}, "PATCH_SOUL_FIRE");
    PATCH_FIRE = createPlaced(NetherFeatures::PATCH_FIRE,
        {fireCount(), inSquare, range44(), biome}, "PATCH_FIRE");

    s_initialized = true;
}

} // namespace placement
} // namespace worldgen
} // namespace data
} // namespace minecraft

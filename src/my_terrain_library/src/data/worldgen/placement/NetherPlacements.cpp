#include "data/worldgen/placement/NetherPlacements.h"
#include <deque>
#include <string>

// Reference: 26.3 net/minecraft/data/worldgen/placement/NetherPlacements.java

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
static std::deque<carver::TrapezoidInt> s_trapezoidInts;
static std::deque<carver::ConstantInt> s_constantInts;

void NetherPlacements::bootstrap() {
    if (s_initialized) return;

    using features::NetherFeatures;
    using blockpredicates::BlockPredicate;
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

    auto filter = [&](std::shared_ptr<BlockPredicate> predicate) -> PlacementModifier* {
        return storeModifier(std::make_unique<BlockPredicateFilter>(
            BlockPredicateFilter::forPredicate(std::move(predicate))));
    };

    auto constantInt = [&](int32_t value) -> const carver::IntProvider* {
        s_constantInts.emplace_back(value);
        return &s_constantInts.back();
    };

    // OffsetPlacement.of(x, y, z)
    auto offsetOf = [&](int32_t x, int32_t y, int32_t z) -> PlacementModifier* {
        return storeModifier(std::make_unique<OffsetPlacement>(constantInt(x), constantInt(y), constantInt(z)));
    };

    // OffsetPlacement.of(xz, y): one provider sampled for x and again for z
    auto offsetXzY = [&](const carver::IntProvider* xz, const carver::IntProvider* y) -> PlacementModifier* {
        return storeModifier(std::make_unique<OffsetPlacement>(xz, y, xz));
    };

    // OffsetPlacement.ofTriangle(xz, y)
    auto offsetTriangle = [&](int32_t xzRange, int32_t yRange) -> PlacementModifier* {
        s_trapezoidInts.push_back(carver::TrapezoidInt::triangle(xzRange));
        const carver::IntProvider* xz = &s_trapezoidInts.back();
        s_trapezoidInts.push_back(carver::TrapezoidInt::triangle(yRange));
        return offsetXzY(xz, &s_trapezoidInts.back());
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
    auto onlyInAir = [&]() { return filter(BlockPredicate::ONLY_IN_AIR_PREDICATE); };

    // BlockPredicate.matchesBlocks(Direction.UP / DOWN, blocks...)
    auto above = [](std::vector<std::string> blocks) {
        return BlockPredicate::matchesBlocks(core::Vec3i(0, 1, 0), blocks);
    };
    auto below = [](std::vector<std::string> blocks) {
        return BlockPredicate::matchesBlocks(core::Vec3i(0, -1, 0), blocks);
    };

    // Reference: 26.3 NetherPlacements.java bootstrap(), in order
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

    // GLOWSTONE_EXTRA / GLOWSTONE: the ceiling check moved from the feature
    // into a filter - ONLY_IN_AIR && above in [netherrack, basalt, blackstone]
    auto glowstoneCeiling = [&]() {
        return filter(BlockPredicate::allOf(BlockPredicate::ONLY_IN_AIR_PREDICATE,
            above({"minecraft:netherrack", "minecraft:basalt", "minecraft:blackstone"})));
    };
    {
        s_biasedInts.push_back(carver::BiasedToBottomInt(0, 9));
        PlacementModifier* biasedCount = storeModifier(std::make_unique<CountPlacement>(
            CountPlacement::of(&s_biasedInts.back())));
        GLOWSTONE_EXTRA = createPlaced(NetherFeatures::GLOWSTONE_EXTRA,
            {biasedCount, inSquare, range44(), glowstoneCeiling(), biome}, "GLOWSTONE_EXTRA");
    }
    // GLOWSTONE reuses the glowstone_extra configured feature
    GLOWSTONE = createPlaced(NetherFeatures::GLOWSTONE_EXTRA,
        {count(10), inSquare, fullRange(), glowstoneCeiling(), biome}, "GLOWSTONE");

    // Forest vegetation: CountOnEveryLayer(n), biome, on #nylium, Count(64),
    // Offset.ofTriangle(7, 3), ONLY_IN_AIR
    auto forestVegetation = [&](ConfiguredFeature* feature, int32_t perLayer, const std::string& name) {
        return createPlaced(feature,
            {countOnEveryLayer(perLayer), biome,
             filter(BlockPredicate::matchesTag(core::Vec3i(0, -1, 0), "minecraft:nylium")),
             count(64), offsetTriangle(7, 3), onlyInAir()},
            name);
    };
    CRIMSON_FOREST_VEGETATION = forestVegetation(NetherFeatures::CRIMSON_FOREST_VEGETATION, 6,
                                                 "CRIMSON_FOREST_VEGETATION");
    WARPED_FOREST_VEGETATION = forestVegetation(NetherFeatures::WARPED_FOREST_VEGETATION, 5,
                                                "WARPED_FOREST_VEGETATION");
    NETHER_SPROUTS = forestVegetation(NetherFeatures::NETHER_SPROUTS, 4, "NETHER_SPROUTS");

    // TWISTING_VINES: Count(10), InSquare, FULL_RANGE, biome + spreadTwistingVines(8, 4)
    {
        auto vineFilter = [&]() {
            return filter(BlockPredicate::allOf(BlockPredicate::ONLY_IN_AIR_PREDICATE,
                below({"minecraft:netherrack", "minecraft:warped_nylium", "minecraft:warped_wart_block"})));
        };
        const int32_t spreadWidth = 8;
        const int32_t spreadHeight = 4;
        s_uniformInts.push_back(carver::UniformInt(-spreadWidth, spreadWidth));
        const carver::IntProvider* spreadXz = &s_uniformInts.back();
        s_uniformInts.push_back(carver::UniformInt(-spreadHeight, spreadHeight));
        const carver::IntProvider* spreadY = &s_uniformInts.back();
        PlacementModifier* scanDown = storeModifier(std::make_unique<EnvironmentScanPlacement>(
            EnvironmentScanPlacement::scanningFor(EnvironmentScanPlacement::Direction::DOWN,
                BlockPredicate::not_(BlockPredicate::ONLY_IN_AIR_PREDICATE), 32)));
        TWISTING_VINES = createPlaced(NetherFeatures::TWISTING_VINES,
            {count(10), inSquare, fullRange(), biome,
             vineFilter(), count(spreadWidth * spreadWidth), offsetXzY(spreadXz, spreadY),
             offsetOf(0, -1, 0), scanDown, offsetOf(0, 1, 0), vineFilter()},
            "TWISTING_VINES");
    }

    WEEPING_VINES = createPlaced(NetherFeatures::WEEPING_VINES,
        {count(10), inSquare, fullRange(), biome,
         filter(BlockPredicate::allOf(BlockPredicate::ONLY_IN_AIR_PREDICATE,
             above({"minecraft:netherrack", "minecraft:nether_wart_block"})))},
        "WEEPING_VINES");
    PATCH_CRIMSON_ROOTS = createPlaced(NetherFeatures::CRIMSON_ROOTS,
        {fullRange(), biome, count(96), offsetTriangle(7, 3), onlyInAir()}, "PATCH_CRIMSON_ROOTS");
    BASALT_PILLAR = createPlaced(NetherFeatures::BASALT_PILLAR,
        {count(10), inSquare, fullRange(),
         filter(BlockPredicate::allOf(BlockPredicate::ONLY_IN_AIR_PREDICATE,
             BlockPredicate::not_(BlockPredicate::matchesTag(core::Vec3i(0, 1, 0), "minecraft:air")))),
         biome},
        "BASALT_PILLAR");
    SPRING_DELTA = createPlaced(NetherFeatures::SPRING_LAVA_NETHER,
        {count(16), inSquare, range44(), biome}, "SPRING_DELTA");
    SPRING_CLOSED = createPlaced(NetherFeatures::SPRING_NETHER_CLOSED,
        {count(16), inSquare, range1010(), biome}, "SPRING_CLOSED");
    SPRING_CLOSED_DOUBLE = createPlaced(NetherFeatures::SPRING_NETHER_CLOSED,
        {count(32), inSquare, range1010(), biome}, "SPRING_CLOSED_DOUBLE");
    SPRING_OPEN = createPlaced(NetherFeatures::SPRING_NETHER_OPEN,
        {count(8), inSquare, range44(), biome}, "SPRING_OPEN");

    // firePlacement(onlyOn) = Count(UniformInt(0, 5)), InSquare, RANGE_4_4,
    // biome, Count(96), Offset.ofTriangle(7, 3), ONLY_IN_AIR && below is onlyOn
    auto firePlacement = [&](ConfiguredFeature* feature, const char* onlyOn, const std::string& name) {
        s_uniformInts.push_back(carver::UniformInt(0, 5));
        PlacementModifier* fireCount = storeModifier(std::make_unique<CountPlacement>(
            CountPlacement::of(&s_uniformInts.back())));
        return createPlaced(feature,
            {fireCount, inSquare, range44(), biome, count(96), offsetTriangle(7, 3),
             filter(BlockPredicate::allOf(BlockPredicate::ONLY_IN_AIR_PREDICATE, below({onlyOn})))},
            name);
    };
    PATCH_SOUL_FIRE = firePlacement(NetherFeatures::SOUL_FIRE, "minecraft:soul_soil", "PATCH_SOUL_FIRE");
    PATCH_FIRE = firePlacement(NetherFeatures::FIRE, "minecraft:netherrack", "PATCH_FIRE");

    s_initialized = true;
}

} // namespace placement
} // namespace worldgen
} // namespace data
} // namespace minecraft

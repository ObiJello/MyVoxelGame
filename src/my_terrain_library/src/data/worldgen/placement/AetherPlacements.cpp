#include "data/worldgen/placement/AetherPlacements.h"
#include "levelgen/carver/CarverConfiguration.h"
#include "levelgen/Heightmap.h"
#include "core/BlockPos.h"
#include "levelgen/structure/StructureStartData.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include "world/IChunk.h"
#include "world/ChunkPos.h"
#include "world/level/block/Blocks.h"
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// The Aether 1.5.10 — data/aether/worldgen/placed_feature/*.json
// (AetherPlacedFeatures.java, AetherPlacedFeatureBuilders.java).

namespace minecraft {
namespace data {
namespace worldgen {
namespace placement {

// ============================================================================
// ImprovedLayerPlacement — ImprovedLayerPlacementModifier.java
// ============================================================================
namespace {

bool isAirState(BlockState* state) {
    return state == nullptr || state->isAir();
}

// ImprovedLayerPlacementModifier.isSolid: !air && !water && !lava.
bool isSolidState(BlockState* state) {
    if (isAirState(state)) return false;
    const std::string& name = state->getBlockName();
    return name != "minecraft:water" && name != "minecraft:lava";
}

} // namespace

void ImprovedLayerPlacement::appendPositions(
    PlacementContext& context,
    WorldgenRandom& random,
    const core::BlockPos& origin,
    std::vector<core::BlockPos>& out
) {
    int32_t layer = 0;
    bool foundAny;
    do {
        foundAny = false;
        // Java re-samples the count in the loop condition every iteration.
        for (int32_t j = 0; j < m_count->sample(random); ++j) {
            const int32_t x = random.nextInt(16) + origin.getX();
            const int32_t z = random.nextInt(16) + origin.getZ();
            const int32_t height = context.getHeight(m_heightmap, x, z);
            core::BlockPos found;
            if (findOnGroundPosition(context, x, height, z, layer, found)) {
                out.push_back(found);
                foundAny = true;
            }
        }
        ++layer;
    } while (foundAny);
}

bool ImprovedLayerPlacement::findOnGroundPosition(
    PlacementContext& context, int32_t x, int32_t y, int32_t z, int32_t layer,
    core::BlockPos& out
) const {
    int32_t seen = 0;
    for (int32_t j = y; j >= context.getMinY() + 1; --j) {
        const core::BlockPos pos(x, j, z);
        BlockState* state = context.getBlockState(pos);
        BlockState* below = context.getBlockState(pos.below());
        if (isAirState(state) && isSolidState(below)
            && below->getBlockName() != "minecraft:bedrock"
            && checkVerticalBounds(context, x, j, z)) {
            if (seen == layer) {
                out = pos;
                return true;
            }
            ++seen;
        }
    }
    return false;
}

bool ImprovedLayerPlacement::checkVerticalBounds(
    PlacementContext& context, int32_t x, int32_t y, int32_t z
) const {
    // Java loops the whole span without breaking; the reads have no side
    // effects, so stopping at the first solid block is equivalent.
    for (int32_t dy = y; dy < y + m_verticalBounds; ++dy) {
        if (isSolidState(context.getBlockState(core::BlockPos(x, dy, z)))) {
            return false;
        }
    }
    return true;
}

// ============================================================================
// HolidayFilter — HolidayFilter.java
// ============================================================================
bool HolidayFilter::shouldPlace(PlacementContext& context, WorldgenRandom& random,
                                const core::BlockPos& origin) {
    (void)context; (void)random; (void)origin;
    // AetherConfig.SERVER: generate_holiday_tree_always = false,
    // generate_holiday_tree_seasonally = true (the mod's defaults).
    constexpr bool kAlways = false;
    constexpr bool kSeasonally = true;
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    const bool isChristmas = local.tm_mon == 11 || local.tm_mon == 0;   // December, January
    return kAlways || (kSeasonally && isChristmas);
}

// ============================================================================
// DungeonBlacklistFilter — DungeonBlacklistFilter.java
// ============================================================================
bool DungeonBlacklistFilter::shouldPlace(PlacementContext& context, WorldgenRandom& random,
                                         const core::BlockPos& origin) {
    (void)random;
    // data/aether/tags/worldgen/structure/dungeons.json
    static const char* const kDungeons[] = {
        "aether:bronze_dungeon", "aether:silver_dungeon", "aether:gold_dungeon"};
    WorldGenLevel* level = context.getLevel();
    if (level == nullptr) return false;
    ::world::IChunk* chunk = level->getChunk(origin.getX() >> 4, origin.getZ() >> 4);
    if (chunk == nullptr) return true;
    for (const char* name : kDungeons) {
        for (int64_t ref : chunk->getReferencesForStructure(name)) {
            const ::world::ChunkPos refPos = ::world::ChunkPos::fromLong(ref);
            ::world::IChunk* refChunk = level->getChunk(refPos.x(), refPos.z());
            if (refChunk == nullptr) continue;
            const structure::StructureStartData* start = refChunk->getStartForStructure(name);
            if (start != nullptr && start->isValid()
                && start->boundingBox.isInside(origin.getX(), origin.getY(), origin.getZ())) {
                return false;
            }
        }
    }
    return true;
}

// ============================================================================
// Registry
// ============================================================================

bool AetherPlacements::s_initialized = false;

PlacedFeature* AetherPlacements::AETHER_DIRT_ORE = nullptr;
PlacedFeature* AetherPlacements::ICESTONE_ORE = nullptr;
PlacedFeature* AetherPlacements::AMBROSIUM_ORE = nullptr;
PlacedFeature* AetherPlacements::ZANITE_ORE = nullptr;
PlacedFeature* AetherPlacements::GRAVITITE_ORE_BURIED = nullptr;
PlacedFeature* AetherPlacements::GRAVITITE_ORE = nullptr;
PlacedFeature* AetherPlacements::SKYROOT_MEADOW_TREES = nullptr;
PlacedFeature* AetherPlacements::SKYROOT_FOREST_TREES = nullptr;
PlacedFeature* AetherPlacements::SKYROOT_GROVE_TREES = nullptr;
PlacedFeature* AetherPlacements::SKYROOT_WOODLAND_TREES = nullptr;
PlacedFeature* AetherPlacements::GRASS_PATCH = nullptr;
PlacedFeature* AetherPlacements::TALL_GRASS_PATCH = nullptr;
PlacedFeature* AetherPlacements::WHITE_FLOWER_PATCH = nullptr;
PlacedFeature* AetherPlacements::PURPLE_FLOWER_PATCH = nullptr;
PlacedFeature* AetherPlacements::BERRY_BUSH_PATCH = nullptr;
PlacedFeature* AetherPlacements::COLD_AERCLOUD = nullptr;
PlacedFeature* AetherPlacements::BLUE_AERCLOUD = nullptr;
PlacedFeature* AetherPlacements::GOLDEN_AERCLOUD = nullptr;
PlacedFeature* AetherPlacements::QUICKSOIL_SHELF = nullptr;
PlacedFeature* AetherPlacements::CRYSTAL_ISLAND = nullptr;
PlacedFeature* AetherPlacements::HOLIDAY_TREE = nullptr;
PlacedFeature* AetherPlacements::WATER_LAKE = nullptr;
PlacedFeature* AetherPlacements::WATER_SPRING = nullptr;

// Owned storage (unique_ptr / shared_ptr: pointers stay valid as vectors grow)
static std::vector<std::unique_ptr<PlacementModifier>> s_modifiers;
static std::vector<std::unique_ptr<PlacedFeature>> s_placedFeatures;
static std::vector<std::shared_ptr<carver::IntProvider>> s_intProviders;
static std::vector<std::shared_ptr<blockpredicates::BlockPredicate>> s_predicates;

void AetherPlacements::bootstrap() {
    if (s_initialized) return;

    using features::AetherFeatures;
    if (!AetherFeatures::isInitialized()) {
        AetherFeatures::bootstrap();
    }

    auto storeModifier = [](std::unique_ptr<PlacementModifier> mod) -> PlacementModifier* {
        PlacementModifier* raw = mod.get();
        s_modifiers.push_back(std::move(mod));
        return raw;
    };

    auto createPlaced = [](ConfiguredFeature* config,
                           const std::vector<PlacementModifier*>& modifiers,
                           const std::string& name) -> PlacedFeature* {
        // A null configured feature (its block not registered) gives a null
        // placement, which addFeature skips with a warning.
        if (config == nullptr) return nullptr;
        auto feature = std::make_unique<PlacedFeature>(config, modifiers, name);
        PlacedFeature* ptr = feature.get();
        s_placedFeatures.push_back(std::move(feature));
        return ptr;
    };

    auto count = [&](int32_t c) -> PlacementModifier* {
        return storeModifier(std::make_unique<CountPlacement>(CountPlacement::of(c)));
    };

    // minecraft:weighted_list count: {data: a, weight: wa}, {data: b, weight: wb}.
    auto weightedCount = [&](int32_t a, int32_t weightA, int32_t b, int32_t weightB) -> PlacementModifier* {
        auto provider = std::make_shared<carver::WeightedListInt>(std::vector<carver::WeightedIntEntry>{
            carver::WeightedIntEntry(std::make_shared<carver::ConstantInt>(a), weightA),
            carver::WeightedIntEntry(std::make_shared<carver::ConstantInt>(b), weightB)});
        s_intProviders.push_back(provider);
        return storeModifier(std::make_unique<CountPlacement>(CountPlacement::of(provider.get())));
    };

    auto rarityOf = [&](int32_t rarity) -> PlacementModifier* {
        return storeModifier(std::make_unique<RarityFilter>(RarityFilter::onAverageOnceEvery(rarity)));
    };

    auto noiseThresholdCount = [&](double noiseLevel, int32_t belowNoise, int32_t aboveNoise)
        -> PlacementModifier* {
        return storeModifier(std::make_unique<NoiseThresholdCountPlacement>(
            NoiseThresholdCountPlacement::of(noiseLevel, belowNoise, aboveNoise)));
    };

    auto heightUniform = [&](const VerticalAnchor& min, const VerticalAnchor& max) -> PlacementModifier* {
        return storeModifier(std::make_unique<HeightRangePlacement>(HeightRangePlacement::uniform(min, max)));
    };

    // minecraft:trapezoid with no plateau (plateau 0 = HeightRangePlacement.triangle).
    auto heightTriangle = [&](const VerticalAnchor& min, const VerticalAnchor& max) -> PlacementModifier* {
        return storeModifier(std::make_unique<HeightRangePlacement>(HeightRangePlacement::triangle(min, max)));
    };

    auto heightmap = [&](Heightmap::Types type) -> PlacementModifier* {
        return storeModifier(std::make_unique<HeightmapPlacement>(HeightmapPlacement::onHeightmap(type)));
    };

    auto waterDepthZero = [&]() -> PlacementModifier* {
        return storeModifier(std::make_unique<SurfaceWaterDepthFilter>(SurfaceWaterDepthFilter::forMaxDepth(0)));
    };

    // aether:improved_layer_placement {count: uniform(0, 1), verticalBounds: 4}
    // — every Aether use has that count and bound; only the heightmap varies.
    auto improvedLayer = [&](Heightmap::Types type) -> PlacementModifier* {
        auto provider = std::make_shared<carver::UniformInt>(0, 1);
        s_intProviders.push_back(provider);
        return storeModifier(std::make_unique<ImprovedLayerPlacement>(type, provider.get(), 4));
    };

    PlacementModifier* inSquare = &InSquarePlacement::spread();
    PlacementModifier* biome = &BiomeFilter::biome();
    // aether:dungeon_blacklist_filter — stateless, one shared instance.
    PlacementModifier* dungeonBlacklist = storeModifier(std::make_unique<DungeonBlacklistFilter>());

    // ---- ores: count, in_square, height_range, biome ----
    AETHER_DIRT_ORE = createPlaced(AetherFeatures::AETHER_DIRT_ORE,
        {count(20), inSquare, heightUniform(VerticalAnchor::aboveBottom(0), VerticalAnchor::aboveBottom(128)), biome},
        "aether:aether_dirt_ore");
    ICESTONE_ORE = createPlaced(AetherFeatures::ICESTONE_ORE,
        {count(10), inSquare, heightUniform(VerticalAnchor::aboveBottom(0), VerticalAnchor::aboveBottom(128)), biome},
        "aether:icestone_ore");
    AMBROSIUM_ORE = createPlaced(AetherFeatures::AMBROSIUM_ORE,
        {count(20), inSquare, heightUniform(VerticalAnchor::aboveBottom(0), VerticalAnchor::aboveBottom(128)), biome},
        "aether:ambrosium_ore");
    ZANITE_ORE = createPlaced(AetherFeatures::ZANITE_ORE,
        {count(14), inSquare, heightUniform(VerticalAnchor::aboveBottom(0), VerticalAnchor::aboveBottom(75)), biome},
        "aether:zanite_ore");
    GRAVITITE_ORE_BURIED = createPlaced(AetherFeatures::GRAVITITE_ORE_BURIED,
        {count(5), inSquare, heightUniform(VerticalAnchor::aboveBottom(0), VerticalAnchor::aboveBottom(74)), biome},
        "aether:gravitite_ore_buried");
    GRAVITITE_ORE = createPlaced(AetherFeatures::GRAVITITE_ORE,
        {count(7), inSquare, heightTriangle(VerticalAnchor::aboveBottom(-58), VerticalAnchor::aboveBottom(74)), biome},
        "aether:gravitite_ore");

    // ---- trees: frequency, surface_water_depth_filter(0),
    // improved_layer_placement(OCEAN_FLOOR), biome (no in_square: the layer
    // placement picks its own columns) ----
    auto treeList = [&](PlacementModifier* frequency) -> std::vector<PlacementModifier*> {
        return {frequency, waterDepthZero(), improvedLayer(Heightmap::Types::OCEAN_FLOOR), biome,
                dungeonBlacklist};
    };
    SKYROOT_MEADOW_TREES = createPlaced(AetherFeatures::TREES_SKYROOT_AND_GOLDEN_OAK,
        treeList(rarityOf(1)), "aether:skyroot_meadow_trees");
    SKYROOT_FOREST_TREES = createPlaced(AetherFeatures::TREES_SKYROOT_AND_GOLDEN_OAK,
        treeList(weightedCount(6, 9, 7, 1)), "aether:skyroot_forest_trees");
    SKYROOT_GROVE_TREES = createPlaced(AetherFeatures::TREES_SKYROOT_AND_GOLDEN_OAK,
        treeList(weightedCount(2, 9, 3, 1)), "aether:skyroot_grove_trees");
    SKYROOT_WOODLAND_TREES = createPlaced(AetherFeatures::TREES_SKYROOT_AND_GOLDEN_OAK,
        treeList(weightedCount(5, 9, 6, 1)), "aether:skyroot_woodland_trees");

    // ---- ground vegetation: ..., improved_layer_placement(MOTION_BLOCKING), biome ----
    GRASS_PATCH = createPlaced(AetherFeatures::GRASS_PATCH,
        {noiseThresholdCount(-0.8, 5, 10), improvedLayer(Heightmap::Types::MOTION_BLOCKING), biome,
         dungeonBlacklist},
        "aether:grass_patch");
    TALL_GRASS_PATCH = createPlaced(AetherFeatures::TALL_GRASS_PATCH,
        {noiseThresholdCount(-0.8, 0, 7), rarityOf(32), improvedLayer(Heightmap::Types::MOTION_BLOCKING), biome,
         dungeonBlacklist},
        "aether:tall_grass_patch");
    WHITE_FLOWER_PATCH = createPlaced(AetherFeatures::WHITE_FLOWER_PATCH,
        {rarityOf(8), improvedLayer(Heightmap::Types::MOTION_BLOCKING), biome},
        "aether:white_flower_patch");
    PURPLE_FLOWER_PATCH = createPlaced(AetherFeatures::PURPLE_FLOWER_PATCH,
        {rarityOf(16), improvedLayer(Heightmap::Types::MOTION_BLOCKING), biome},
        "aether:purple_flower_patch");
    BERRY_BUSH_PATCH = createPlaced(AetherFeatures::BERRY_BUSH_PATCH,
        {rarityOf(8), improvedLayer(Heightmap::Types::MOTION_BLOCKING), biome},
        "aether:berry_bush_patch");

    // ---- aerclouds: height_range first, then rarity, in_square, biome ----
    COLD_AERCLOUD = createPlaced(AetherFeatures::COLD_AERCLOUD,
        {heightUniform(VerticalAnchor::absolute(32), VerticalAnchor::absolute(96)), rarityOf(7), inSquare, biome, dungeonBlacklist},
        "aether:cold_aercloud");
    BLUE_AERCLOUD = createPlaced(AetherFeatures::BLUE_AERCLOUD,
        {heightUniform(VerticalAnchor::absolute(32), VerticalAnchor::absolute(96)), rarityOf(24), inSquare, biome, dungeonBlacklist},
        "aether:blue_aercloud");
    GOLDEN_AERCLOUD = createPlaced(AetherFeatures::GOLDEN_AERCLOUD,
        {heightUniform(VerticalAnchor::absolute(96), VerticalAnchor::absolute(128)), rarityOf(75), inSquare, biome, dungeonBlacklist},
        "aether:golden_aercloud");

    // ---- quicksoil shelf: rarity 5, heightmap WORLD_SURFACE_WG, biome (the
    // feature itself sweeps the 16x16 area from the chunk corner) ----
    QUICKSOIL_SHELF = createPlaced(AetherFeatures::QUICKSOIL_SHELF,
        {rarityOf(5), heightmap(Heightmap::Types::WORLD_SURFACE_WG), biome, dungeonBlacklist},
        "aether:quicksoil_shelf");

    // ---- pass two ----
    // crystal_island.json
    CRYSTAL_ISLAND = createPlaced(AetherFeatures::CRYSTAL_ISLAND,
        {rarityOf(50), inSquare,
         heightUniform(VerticalAnchor::absolute(32), VerticalAnchor::absolute(96)), biome,
         dungeonBlacklist},
        "aether:crystal_island");

    // holiday_tree.json — the would_survive state is skyroot_sapling stage 0
    // (its default state).
    {
        std::vector<PlacementModifier*> modifiers = {
            rarityOf(75), inSquare, waterDepthZero(), heightmap(Heightmap::Types::OCEAN_FLOOR), biome,
            storeModifier(std::make_unique<HolidayFilter>())};
        if (BlockState* sapling = world::level::block::Blocks::getDefaultState("minecraft:skyroot_sapling")) {
            auto predicate = blockpredicates::BlockPredicate::wouldSurvive(sapling, core::Vec3i::ZERO());
            s_predicates.push_back(predicate);
            modifiers.push_back(storeModifier(std::make_unique<BlockPredicateFilter>(
                BlockPredicateFilter::forPredicate(predicate))));
        } else {
            fprintf(stderr, "[AetherPlacements] minecraft:skyroot_sapling missing -"
                            " holiday_tree placed without its would_survive filter\n");
        }
        HOLIDAY_TREE = createPlaced(AetherFeatures::HOLIDAY_TREE, modifiers, "aether:holiday_tree");
    }

    // water_lake.json
    WATER_LAKE = createPlaced(AetherFeatures::WATER_LAKE,
        {rarityOf(15), heightmap(Heightmap::Types::WORLD_SURFACE_WG), biome},
        "aether:water_lake");

    // water_spring.json
    WATER_SPRING = createPlaced(AetherFeatures::WATER_SPRING,
        {count(30), inSquare,
         heightUniform(VerticalAnchor::aboveBottom(8), VerticalAnchor::aboveBottom(128)), biome,
         dungeonBlacklist},
        "aether:water_spring");

    s_initialized = true;
}

} // namespace placement
} // namespace worldgen
} // namespace data
} // namespace minecraft

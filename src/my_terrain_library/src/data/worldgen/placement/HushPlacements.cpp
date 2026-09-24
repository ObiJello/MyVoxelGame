#include "data/worldgen/placement/HushPlacements.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include "levelgen/carver/CarverConfiguration.h"
#include "levelgen/Heightmap.h"
#include "world/level/block/Blocks.h"
#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

// The Hush — engine-only dimension. No Java reference; each placement names
// the vanilla placement it copies its modifier list from.

namespace minecraft {
namespace data {
namespace worldgen {
namespace placement {

bool HushPlacements::s_initialized = false;

PlacedFeature* HushPlacements::ORE_ECHO = nullptr;
PlacedFeature* HushPlacements::ORE_ECHO_DENSE = nullptr;
PlacedFeature* HushPlacements::WHISPERWOOD_SPARSE = nullptr;
PlacedFeature* HushPlacements::WHISPERWOOD_FOREST = nullptr;
PlacedFeature* HushPlacements::PATCH_RESONANCE_BLOOM = nullptr;
PlacedFeature* HushPlacements::CRYSTAL_FORMATIONS_MEADOWS = nullptr;
PlacedFeature* HushPlacements::CRYSTAL_FORMATIONS_BARRENS = nullptr;
PlacedFeature* HushPlacements::CRYSTAL_SHARDS_BARRENS = nullptr;
PlacedFeature* HushPlacements::HUSH_MOSS_PATCH = nullptr;
PlacedFeature* HushPlacements::ORE_RESONITE = nullptr;
PlacedFeature* HushPlacements::ORE_RESONITE_CAVERNS = nullptr;
PlacedFeature* HushPlacements::RESONANT_CLUSTERS = nullptr;
PlacedFeature* HushPlacements::RESONANT_CRYSTAL_CLUMPS = nullptr;
PlacedFeature* HushPlacements::RESONANT_CLUSTER_SURFACE = nullptr;
PlacedFeature* HushPlacements::PATCH_HUSH_GRASS_MEADOWS = nullptr;
PlacedFeature* HushPlacements::PATCH_HUSH_GRASS_FOREST = nullptr;
PlacedFeature* HushPlacements::PATCH_RESONANCE_BLOOM_MEADOWS = nullptr;
PlacedFeature* HushPlacements::SUNKEN_RUINS = nullptr;
PlacedFeature* HushPlacements::KELP_SUNKEN_CHOIR = nullptr;
PlacedFeature* HushPlacements::SEA_PICKLE_SUNKEN_CHOIR = nullptr;
PlacedFeature* HushPlacements::PATCH_HUSH_GRASS_SHORE = nullptr;
PlacedFeature* HushPlacements::ROPE_BRIDGES = nullptr;
PlacedFeature* HushPlacements::RESONANT_STALACTITES = nullptr;
PlacedFeature* HushPlacements::PATCH_HUSH_GRASS_RIM = nullptr;
PlacedFeature* HushPlacements::CRYSTAL_FORMATIONS_RIM = nullptr;
PlacedFeature* HushPlacements::HUSHSTONE_BOULDERS = nullptr;
PlacedFeature* HushPlacements::PATCH_HUSH_GRASS_STEPPE = nullptr;
PlacedFeature* HushPlacements::PATCH_RESONANCE_BLOOM_STEPPE = nullptr;
PlacedFeature* HushPlacements::CRYSTAL_FORMATIONS_STEPPE = nullptr;

// Owned storage (unique_ptr: pointers stay valid as the vectors grow)
static std::vector<std::unique_ptr<PlacementModifier>> s_modifiers;
static std::vector<std::unique_ptr<PlacedFeature>> s_placedFeatures;
static std::vector<std::shared_ptr<blockpredicates::BlockPredicate>> s_blockPredicates;
static std::vector<std::shared_ptr<carver::IntProvider>> s_intProviders;

void HushPlacements::bootstrap() {
    if (s_initialized) return;

    using features::HushFeatures;
    if (!HushFeatures::isInitialized()) {
        HushFeatures::bootstrap();
    }

    auto storeModifier = [](std::unique_ptr<PlacementModifier> mod) -> PlacementModifier* {
        PlacementModifier* raw = mod.get();
        s_modifiers.push_back(std::move(mod));
        return raw;
    };

    auto createPlaced = [](ConfiguredFeature* config,
                           const std::vector<PlacementModifier*>& modifiers,
                           const std::string& name) -> PlacedFeature* {
        // HushFeatures leaves its features null when a Hush block is not
        // registered; a null placement is skipped by addFeature (with a
        // warning) instead of crashing at place time.
        if (config == nullptr) {
            return nullptr;
        }
        auto feature = std::make_unique<PlacedFeature>(config, modifiers, name);
        PlacedFeature* ptr = feature.get();
        s_placedFeatures.push_back(std::move(feature));
        return ptr;
    };

    auto count = [&](int32_t c) -> PlacementModifier* {
        return storeModifier(std::make_unique<CountPlacement>(CountPlacement::of(c)));
    };

    auto countExtra = [&](int32_t base, float chance, int32_t extra) -> PlacementModifier* {
        return storeModifier(std::make_unique<CountPlacement>(
            CountPlacement::countExtra(base, chance, extra)));
    };

    auto rarityOf = [&](int32_t rarity) -> PlacementModifier* {
        return storeModifier(std::make_unique<RarityFilter>(
            RarityFilter::onAverageOnceEvery(rarity)));
    };

    auto noiseThresholdCount = [&](double noiseLevel, int32_t belowNoise, int32_t aboveNoise)
        -> PlacementModifier* {
        return storeModifier(std::make_unique<NoiseThresholdCountPlacement>(
            NoiseThresholdCountPlacement::of(noiseLevel, belowNoise, aboveNoise)));
    };

    auto heightUniform = [&](const VerticalAnchor& min, const VerticalAnchor& max) -> PlacementModifier* {
        return storeModifier(std::make_unique<HeightRangePlacement>(
            HeightRangePlacement::uniform(min, max)));
    };

    auto heightTriangle = [&](const VerticalAnchor& min, const VerticalAnchor& max) -> PlacementModifier* {
        return storeModifier(std::make_unique<HeightRangePlacement>(
            HeightRangePlacement::triangle(min, max)));
    };

    auto heightmap = [&](Heightmap::Types type) -> PlacementModifier* {
        return storeModifier(std::make_unique<HeightmapPlacement>(
            HeightmapPlacement::onHeightmap(type)));
    };

    // PlacementUtils.filteredByBlockSurvival(OAK_SAPLING): the tree's
    // wouldSurvive check, which reads #minecraft:dirt (sculk loam and hush
    // moss are members).
    auto wouldSurviveOakSapling = [&]() -> PlacementModifier* {
        BlockState* sapling =
            minecraft::world::level::block::Blocks::getDefaultState("minecraft:oak_sapling");
        if (!sapling) {
            // Vanilla block; only a broken registry gets here. Fall back to
            // an unfiltered tree placement rather than fail every dimension.
            fprintf(stderr, "[HushPlacements] minecraft:oak_sapling missing - trees unfiltered\n");
            return nullptr;
        }
        auto predicate = blockpredicates::BlockPredicate::wouldSurvive(sapling, core::Vec3i::ZERO());
        s_blockPredicates.push_back(predicate);
        return storeModifier(std::make_unique<BlockPredicateFilter>(
            BlockPredicateFilter::forPredicate(predicate)));
    };

    // VegetationPlacements TREE_THRESHOLD = SurfaceWaterDepthFilter.forMaxDepth(0)
    auto treeThreshold = [&]() -> PlacementModifier* {
        return storeModifier(std::make_unique<SurfaceWaterDepthFilter>(
            SurfaceWaterDepthFilter::forMaxDepth(0)));
    };

    PlacementModifier* inSquare = &InSquarePlacement::spread();
    PlacementModifier* biome = &BiomeFilter::biome();

    // ---- ores: OrePlacements.commonOrePlacement(count, height) =
    // count, in_square, height, biome (the helper is private to OrePlacements,
    // so its list is spelled out here) ----

    // ORE_ECHO — the ORE_LAPIS shape (triangle -32..32) at twice its count.
    ORE_ECHO = createPlaced(HushFeatures::ORE_ECHO,
        {count(4), inSquare,
         heightTriangle(VerticalAnchor::absolute(-32), VerticalAnchor::absolute(32)),
         biome},
        "ORE_ECHO");

    // ORE_ECHO_DENSE — the ORE_LAPIS_BURIED shape (uniform bottom..64) at
    // twice its count; the resonant barrens' extra vein. No biome filter
    // (like RESONANT_STALACTITES): the barrens is a surface biome, and below
    // ~13 blocks under its surface the column is the Crystal Caverns (depth
    // 0.2..0.9), so a filter at the ore's origin refused ~80 % of these
    // veins (measured offline: 21 % of barrens origins passed). Unfiltered, it runs wherever a barrens chunk (or its 3x3
    // neighbourhood, MC applyBiomeDecoration) decorates.
    ORE_ECHO_DENSE = createPlaced(HushFeatures::ORE_ECHO,
        {count(8), inSquare,
         heightUniform(VerticalAnchor::bottom(), VerticalAnchor::absolute(64))},
        "ORE_ECHO_DENSE");

    // ORE_RESONITE — the deep tier ore in every Hush biome: the ORE_DIAMOND
    // triangle band (-64..0) at count 4.
    ORE_RESONITE = createPlaced(HushFeatures::ORE_RESONITE,
        {count(4), inSquare,
         heightTriangle(VerticalAnchor::absolute(-64), VerticalAnchor::absolute(0)),
         biome},
        "ORE_RESONITE");

    // ORE_RESONITE_CAVERNS — the Crystal Caverns' extra veins: uniform
    // bottom..16 at count 10 (the caverns are the resonite motherlode).
    ORE_RESONITE_CAVERNS = createPlaced(HushFeatures::ORE_RESONITE,
        {count(10), inSquare,
         heightUniform(VerticalAnchor::bottom(), VerticalAnchor::absolute(16)),
         biome},
        "ORE_RESONITE_CAVERNS");

    // ---- trees: VegetationPlacements.treePlacement(frequency, OAK_SAPLING) =
    // frequency, in_square, TREE_THRESHOLD, HEIGHTMAP_OCEAN_FLOOR,
    // wouldSurvive(sapling), biome ----

    auto treePlacement = [&](PlacementModifier* frequency) -> std::vector<PlacementModifier*> {
        std::vector<PlacementModifier*> modifiers = {
            frequency, inSquare, treeThreshold(), heightmap(Heightmap::Types::OCEAN_FLOOR)};
        if (PlacementModifier* survival = wouldSurviveOakSapling()) {
            modifiers.push_back(survival);
        }
        modifiers.push_back(biome);
        return modifiers;
    };

    // WHISPERWOOD_SPARSE — TREES_PLAINS' frequency: countExtra(0, 0.05, 1).
    WHISPERWOOD_SPARSE = createPlaced(HushFeatures::WHISPERWOOD,
        treePlacement(countExtra(0, 0.05f, 1)), "WHISPERWOOD_SPARSE");

    // WHISPERWOOD_FOREST — a forest density: CountPlacement.of(10) over the
    // 1 : 3 large/normal selector (TREES_BIRCH_AND_OAK shape).
    WHISPERWOOD_FOREST = createPlaced(HushFeatures::WHISPERWOOD_FOREST_TREES,
        treePlacement(count(10)), "WHISPERWOOD_FOREST");

    // ---- surface vegetation ----

    // PATCH_RESONANCE_BLOOM — the FLOWER_PLAINS list
    // (noiseThresholdCount(-0.8, 15, 4), rarity, in_square, HEIGHTMAP
    // (motion blocking), biome) with rarity 8 instead of 32.
    PATCH_RESONANCE_BLOOM = createPlaced(HushFeatures::PATCH_RESONANCE_BLOOM,
        {noiseThresholdCount(-0.8, 15, 4), rarityOf(8), inSquare,
         heightmap(Heightmap::Types::MOTION_BLOCKING), biome},
        "PATCH_RESONANCE_BLOOM");

    // ---- resonant crystal formations: the PATCH_CACTUS_DECORATED list
    // (rarity / count, in_square, HEIGHTMAP, biome) over one formation per
    // attempt. MOTION_BLOCKING puts the origin on the ground (the feature
    // rejects anything but hushstone, polished hushstone, sculk loam or hush
    // moss under it, so it never lands on a tree, a bridge or another
    // formation). ----

    // CRYSTAL_FORMATIONS_MEADOWS — a small accent every sixteenth chunk.
    CRYSTAL_FORMATIONS_MEADOWS = createPlaced(HushFeatures::CRYSTAL_FORMATION_SMALL,
        {rarityOf(16), inSquare, heightmap(Heightmap::Types::MOTION_BLOCKING), biome},
        "CRYSTAL_FORMATIONS_MEADOWS");

    // CRYSTAL_FORMATIONS_BARRENS — an outcrop every other chunk, and
    // CRYSTAL_SHARDS_BARRENS — a small one in every chunk between them: the
    // barrens are the crystal fields.
    CRYSTAL_FORMATIONS_BARRENS = createPlaced(HushFeatures::CRYSTAL_FORMATION_FIELD,
        {rarityOf(2), inSquare, heightmap(Heightmap::Types::MOTION_BLOCKING), biome},
        "CRYSTAL_FORMATIONS_BARRENS");
    CRYSTAL_SHARDS_BARRENS = createPlaced(HushFeatures::CRYSTAL_FORMATION_SMALL,
        {count(1), inSquare, heightmap(Heightmap::Types::MOTION_BLOCKING), biome},
        "CRYSTAL_SHARDS_BARRENS");

    // HUSH_MOSS_PATCH — surface moss: two attempts per chunk from the
    // ocean-floor heightmap (the FLOOR patch searches down from there).
    HUSH_MOSS_PATCH = createPlaced(HushFeatures::HUSH_MOSS_PATCH,
        {count(2), inSquare, heightmap(Heightmap::Types::OCEAN_FLOOR), biome},
        "HUSH_MOSS_PATCH");

    // PATCH_RESONANCE_BLOOM_MEADOWS — the meadows' bloom density: the same
    // list at rarity 6 (the shared placement stays at 8 for the forest).
    PATCH_RESONANCE_BLOOM_MEADOWS = createPlaced(HushFeatures::PATCH_RESONANCE_BLOOM,
        {noiseThresholdCount(-0.8, 15, 4), rarityOf(6), inSquare,
         heightmap(Heightmap::Types::MOTION_BLOCKING), biome},
        "PATCH_RESONANCE_BLOOM_MEADOWS");

    // PATCH_HUSH_GRASS_MEADOWS — the PATCH_GRASS_PLAIN list shape
    // (noiseThresholdCount, in_square, HEIGHTMAP_WORLD_SURFACE (=
    // WORLD_SURFACE_WG), biome) thinned to scattered tufts: 2 patches a
    // chunk, 4 where the grass noise is high (plains: 5 / 10, and this used
    // to double that).
    PATCH_HUSH_GRASS_MEADOWS = createPlaced(HushFeatures::PATCH_HUSH_GRASS,
        {noiseThresholdCount(-0.8, 2, 4), inSquare,
         heightmap(Heightmap::Types::WORLD_SURFACE_WG), biome},
        "PATCH_HUSH_GRASS_MEADOWS");

    // PATCH_HUSH_GRASS_FOREST — the PATCH_GRASS_FOREST list shape at one
    // patch a chunk (the forest floor is moss and blooms first).
    PATCH_HUSH_GRASS_FOREST = createPlaced(HushFeatures::PATCH_HUSH_GRASS,
        {count(1), inSquare, heightmap(Heightmap::Types::WORLD_SURFACE_WG), biome},
        "PATCH_HUSH_GRASS_FOREST");

    // RESONANT_CLUSTER_SURFACE — the barrens' occasional surface clusters:
    // the CRYSTAL_SHARDS_BARRENS list at count 2 (the patch's own filter
    // keeps them on bare stone floors).
    RESONANT_CLUSTER_SURFACE = createPlaced(HushFeatures::RESONANT_CLUSTER_SURFACE,
        {count(2), inSquare, heightmap(Heightmap::Types::MOTION_BLOCKING), biome},
        "RESONANT_CLUSTER_SURFACE");

    // ---- Crystal Caverns (UNDERGROUND_DECORATION) ----

    // RESONANT_CLUSTERS — the POINTED_DRIPSTONE list shape (count, in_square,
    // height range, biome; the scan/offset per facing live inside the
    // selector's inline placements): 48 attempts per chunk, uniform
    // bottom..50, caverns only.
    RESONANT_CLUSTERS = createPlaced(HushFeatures::RESONANT_CLUSTERS,
        {count(48), inSquare,
         heightUniform(VerticalAnchor::bottom(), VerticalAnchor::absolute(50)),
         biome},
        "RESONANT_CLUSTERS");

    // RESONANT_CRYSTAL_CLUMPS — six attempts per chunk, each scanning down
    // through air to a solid floor (12 steps) and stepping up one block
    // (the POINTED_DRIPSTONE_DOWN variant's scan + offset), caverns only.
    {
        auto plusOne = std::make_shared<carver::ConstantInt>(1);
        s_intProviders.push_back(plusOne);
        PlacementModifier* scanDown = storeModifier(std::make_unique<EnvironmentScanPlacement>(
            EnvironmentScanPlacement::scanningFor(
                EnvironmentScanPlacement::Direction::DOWN,
                blockpredicates::BlockPredicate::solid(),
                blockpredicates::BlockPredicate::ONLY_IN_AIR_PREDICATE,
                12)));
        PlacementModifier* stepUp = storeModifier(std::make_unique<RandomOffsetPlacement>(
            RandomOffsetPlacement::vertical(plusOne.get())));
        RESONANT_CRYSTAL_CLUMPS = createPlaced(HushFeatures::RESONANT_CRYSTAL_CLUMP,
            {count(6), inSquare,
             heightUniform(VerticalAnchor::bottom(), VerticalAnchor::absolute(50)),
             scanDown, stepUp, biome},
            "RESONANT_CRYSTAL_CLUMPS");
    }

    // ---- Sunken Choir ----

    // SUNKEN_RUINS — one fragment every other chunk, on the flooded floor
    // (the feature rejects a dry origin).
    SUNKEN_RUINS = createPlaced(HushFeatures::SUNKEN_RUIN,
        {rarityOf(2), inSquare, heightmap(Heightmap::Types::OCEAN_FLOOR), biome},
        "SUNKEN_RUINS");

    // KELP_SUNKEN_CHOIR — 26.3 AquaticPlacements.KELP_COLD's list
    // (noiseBasedCount, in_square, HEIGHTMAP_OCEAN_FLOOR, kelp filter, biome)
    // at a noise-to-count ratio of 40 (cold oceans use 120; the choir's water
    // is shallow).
    {
        using levelgen::blockpredicates::BlockPredicate;
        PlacementModifier* kelpFilter = storeModifier(std::make_unique<BlockPredicateFilter>(
            BlockPredicateFilter::forPredicate(BlockPredicate::allOf({
                BlockPredicate::matchesBlocks(std::vector<std::string>{"minecraft:water"}),
                BlockPredicate::matchesBlocks(core::Vec3i(0, 1, 0), std::vector<std::string>{"minecraft:water"}),
                BlockPredicate::hasSturdyFace(core::Vec3i(0, -1, 0), core::Direction::UP),
                BlockPredicate::not_(BlockPredicate::matchesTag(core::Vec3i(0, -1, 0), "minecraft:cannot_support_kelp"))}))));
        KELP_SUNKEN_CHOIR = createPlaced(HushFeatures::KELP,
            {storeModifier(std::make_unique<NoiseBasedCountPlacement>(
                 NoiseBasedCountPlacement::of(40, 80.0, 0.0))),
             inSquare, heightmap(Heightmap::Types::OCEAN_FLOOR), kelpFilter, biome},
            "KELP_SUNKEN_CHOIR");
    }

    // SEA_PICKLE_SUNKEN_CHOIR — 26.3 AquaticPlacements.SEA_PICKLE's list with
    // rarity 8 for 16 and 12 pickles for 20 (the choir's water is shallow and
    // small).
    {
        using levelgen::blockpredicates::BlockPredicate;
        static levelgen::carver::TrapezoidInt s_triangle7 = levelgen::carver::TrapezoidInt::triangle(7);
        static levelgen::carver::TrapezoidInt s_triangle0 = levelgen::carver::TrapezoidInt::triangle(0);
        PlacementModifier* spread = storeModifier(std::make_unique<RandomOffsetPlacement>(
            RandomOffsetPlacement::of(&s_triangle7, &s_triangle0)));
        PlacementModifier* waterFilter = storeModifier(std::make_unique<BlockPredicateFilter>(
            BlockPredicateFilter::forPredicate(
                BlockPredicate::matchesBlocks(std::vector<std::string>{"minecraft:water"}))));
        SEA_PICKLE_SUNKEN_CHOIR = createPlaced(HushFeatures::SEA_PICKLE,
            {rarityOf(8), inSquare, count(12), spread, heightmap(Heightmap::Types::OCEAN_FLOOR), waterFilter, biome},
            "SEA_PICKLE_SUNKEN_CHOIR");
    }

    // PATCH_HUSH_GRASS_SHORE — the dry banks between the pools.
    PATCH_HUSH_GRASS_SHORE = createPlaced(HushFeatures::PATCH_HUSH_GRASS,
        {count(1), inSquare, heightmap(Heightmap::Types::WORLD_SURFACE_WG), biome},
        "PATCH_HUSH_GRASS_SHORE");

    // ---- Hollow Deep ----

    // ROPE_BRIDGES — eight rim searches per chunk from the motion-blocking
    // surface (the _WG heightmaps predate the carvers, so they would float
    // the origin over the chasm); most find no chasm and place nothing.
    ROPE_BRIDGES = createPlaced(HushFeatures::ROPE_BRIDGE,
        {count(8), inSquare, heightmap(Heightmap::Types::MOTION_BLOCKING), biome},
        "ROPE_BRIDGES");

    // RESONANT_STALACTITES — the POINTED_DRIPSTONE list shape with the
    // ceiling variant's scan: an air start scans up through air (16 steps)
    // to a solid ceiling and steps back down one block. 32 attempts (each
    // one a hanging formation of ~10-40 blocks; the old single columns took
    // 48). Deliberately no biome filter (see the header).
    {
        auto minusOne = std::make_shared<carver::ConstantInt>(-1);
        s_intProviders.push_back(minusOne);
        PlacementModifier* scanUp = storeModifier(std::make_unique<EnvironmentScanPlacement>(
            EnvironmentScanPlacement::scanningFor(
                EnvironmentScanPlacement::Direction::UP,
                blockpredicates::BlockPredicate::solid(),
                blockpredicates::BlockPredicate::ONLY_IN_AIR_PREDICATE,
                16)));
        PlacementModifier* stepDown = storeModifier(std::make_unique<RandomOffsetPlacement>(
            RandomOffsetPlacement::vertical(minusOne.get())));
        RESONANT_STALACTITES = createPlaced(HushFeatures::RESONANT_STALACTITE,
            {count(32), inSquare,
             heightUniform(VerticalAnchor::absolute(-16), VerticalAnchor::absolute(180)),
             scanUp, stepDown},
            "RESONANT_STALACTITES");
    }

    // PATCH_HUSH_GRASS_RIM — the PATCH_GRASS_FOREST list shape on the rims,
    // one scattered patch a chunk.
    PATCH_HUSH_GRASS_RIM = createPlaced(HushFeatures::PATCH_HUSH_GRASS,
        {count(1), inSquare, heightmap(Heightmap::Types::WORLD_SURFACE_WG), biome},
        "PATCH_HUSH_GRASS_RIM");

    // CRYSTAL_FORMATIONS_RIM — a tall formation every fourth chunk; the
    // feature's level check keeps it off the chasm lip itself.
    CRYSTAL_FORMATIONS_RIM = createPlaced(HushFeatures::CRYSTAL_FORMATION_TALL,
        {rarityOf(4), inSquare, heightmap(Heightmap::Types::MOTION_BLOCKING), biome},
        "CRYSTAL_FORMATIONS_RIM");

    // ---- Aurora Steppe ----

    // HUSHSTONE_BOULDERS — MiscOverworldPlacements.FOREST_ROCK's list
    // (count, in_square, HEIGHTMAP, biome) thinned to one every third chunk:
    // the steppe is open ground with the odd boulder.
    HUSHSTONE_BOULDERS = createPlaced(HushFeatures::HUSHSTONE_BOULDER,
        {rarityOf(3), inSquare, heightmap(Heightmap::Types::MOTION_BLOCKING), biome},
        "HUSHSTONE_BOULDERS");

    // PATCH_HUSH_GRASS_STEPPE — the PATCH_GRASS_PLAIN list shape, a little
    // fuller than the meadows' (3 / 5): scattered tufts over the moss.
    PATCH_HUSH_GRASS_STEPPE = createPlaced(HushFeatures::PATCH_HUSH_GRASS,
        {noiseThresholdCount(-0.8, 3, 5), inSquare,
         heightmap(Heightmap::Types::WORLD_SURFACE_WG), biome},
        "PATCH_HUSH_GRASS_STEPPE");

    // PATCH_RESONANCE_BLOOM_STEPPE — scattered blooms: the bloom patch at
    // rarity 12 without the flower-noise count.
    PATCH_RESONANCE_BLOOM_STEPPE = createPlaced(HushFeatures::PATCH_RESONANCE_BLOOM,
        {rarityOf(12), inSquare, heightmap(Heightmap::Types::MOTION_BLOCKING), biome},
        "PATCH_RESONANCE_BLOOM_STEPPE");

    // CRYSTAL_FORMATIONS_STEPPE — a tall landmark every eighth chunk, seen
    // from far across the open plateau.
    CRYSTAL_FORMATIONS_STEPPE = createPlaced(HushFeatures::CRYSTAL_FORMATION_TALL,
        {rarityOf(8), inSquare, heightmap(Heightmap::Types::MOTION_BLOCKING), biome},
        "CRYSTAL_FORMATIONS_STEPPE");

    s_initialized = true;
}

} // namespace placement
} // namespace worldgen
} // namespace data
} // namespace minecraft

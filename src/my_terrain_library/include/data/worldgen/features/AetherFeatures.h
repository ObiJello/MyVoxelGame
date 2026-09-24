#pragma once

#include "levelgen/feature/Feature.h"
#include "levelgen/feature/TreeFeature.h"
#include "levelgen/feature/trunkplacers/TrunkPlacer.h"
#include "levelgen/feature/foliageplacers/FoliagePlacer.h"
#include "levelgen/feature/treedecorators/TreeDecorator.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/carver/CarverConfiguration.h"
#include "world/level/block/Blocks.h"
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// The Aether 1.5.10 (DimensionId::Aether) — configured features ported from
// data/aether/worldgen/configured_feature/*.json (AetherConfiguredFeatures.java).
// The engine registers every Aether block as minecraft:<slug>, so each
// "aether:<name>" state in the JSON resolves as "minecraft:<name>" here.

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

// ============================================================================
// AercloudFeature — world/feature/AercloudFeature.java
// ============================================================================

/**
 * AercloudConfiguration — world/configuration/AercloudConfiguration.java
 * (int bounds, BlockStateProvider blocks). Every Aether aercloud uses a
 * simple_state_provider, which consumes no randomness, so the state is held
 * directly.
 */
class AercloudConfiguration : public levelgen::FeatureConfiguration {
public:
    int32_t bounds;
    BlockState* block;

    AercloudConfiguration(int32_t bounds, BlockState* block)
        : bounds(bounds), block(block) {}
};

/**
 * AercloudFeature — a random walk of `bounds` steps, each stamping a small
 * diamond of cloud blocks into air. The Java for-loop bounds re-draw the
 * random every iteration (random.nextInt in the loop condition); the port
 * keeps that call pattern exactly.
 */
class AercloudFeature : public levelgen::Feature<AercloudConfiguration> {
public:
    bool place(levelgen::FeaturePlaceContext<AercloudConfiguration>& context) override;
};

// ============================================================================
// ShelfFeature — world/feature/ShelfFeature.java
// ============================================================================

/**
 * ShelfConfiguration — world/configuration/ShelfConfiguration.java
 * (BlockStateProvider block, FloatProvider radius, UniformInt y_range,
 * HolderSet<Block> valid_blocks). The quicksoil shelf's provider is simple and
 * its radius a constant float, neither of which consumes randomness; only
 * y_range's bounds are read (getMinValue / getMaxValue).
 */
class ShelfConfiguration : public levelgen::FeatureConfiguration {
public:
    BlockState* block;
    float radius;
    int32_t yMinInclusive;
    int32_t yMaxInclusive;
    std::vector<std::string> validBlocks;

    ShelfConfiguration(BlockState* block, float radius, int32_t yMinInclusive,
                       int32_t yMaxInclusive, std::vector<std::string> validBlocks)
        : block(block)
        , radius(radius)
        , yMinInclusive(yMinInclusive)
        , yMaxInclusive(yMaxInclusive)
        , validBlocks(std::move(validBlocks)) {}
};

/**
 * ShelfFeature — for every column of the 16x16 area east/south of the origin,
 * finds the lowest y in [min, max) with air, a valid block above and air two
 * above, and places a disk there (BlockPlacementUtil.placeDisk).
 */
class ShelfFeature : public levelgen::Feature<ShelfConfiguration> {
public:
    bool place(levelgen::FeaturePlaceContext<ShelfConfiguration>& context) override;
};

// ============================================================================
// Golden oak tree placers
// ============================================================================

/**
 * GoldenOakTrunkPlacer — world/trunkplacer/GoldenOakTrunkPlacer.java.
 * A straight trunk of the full height; between heights 5 and 8 each log has
 * a 2-in-3 chance of sprouting a 1-2 log branch in a random horizontal
 * direction, rising by (i / 4 - 1) per log. One foliage attachment at the top.
 */
class GoldenOakTrunkPlacer : public levelgen::feature::trunkplacers::TrunkPlacer {
public:
    GoldenOakTrunkPlacer(int baseHeight, int heightRandA, int heightRandB)
        : TrunkPlacer(baseHeight, heightRandA, heightRandB) {}

    std::vector<levelgen::feature::foliageplacers::FoliageAttachment> placeTrunk(
        levelgen::feature::trunkplacers::LevelReader& level,
        levelgen::feature::trunkplacers::TrunkSetter trunkSetter,
        levelgen::WorldgenRandom& random,
        int treeHeight,
        const core::BlockPos& origin,
        std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> trunkProvider,
        std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> dirtProvider,
        bool forceDirt
    ) override;

private:
    void branch(
        levelgen::feature::trunkplacers::LevelReader& level,
        levelgen::WorldgenRandom& random,
        levelgen::feature::trunkplacers::TrunkSetter trunkSetter,
        int x, int y, int z, int slant,
        std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> trunkProvider
    );
};

/**
 * GoldenOakFoliagePlacer — world/foliageplacer/GoldenOakFoliagePlacer.java.
 * Rows from `offset` down to `offset - foliageHeight`, each of range 4, with
 * a leaf skipped when x^2 + (y + 2)^2 + z^2 > 12 + nextInt(5): a ragged
 * sphere of radius ~3.5 centred two below the attachment row. foliageHeight
 * is a constant 7; the codec's trunk_height is carried but unused, as in Java.
 */
class GoldenOakFoliagePlacer : public levelgen::feature::foliageplacers::FoliagePlacer {
private:
    std::shared_ptr<levelgen::carver::IntProvider> m_trunkHeight;

public:
    GoldenOakFoliagePlacer(std::shared_ptr<levelgen::carver::IntProvider> radius,
                           std::shared_ptr<levelgen::carver::IntProvider> offset,
                           std::shared_ptr<levelgen::carver::IntProvider> trunkHeight)
        : FoliagePlacer(std::move(radius), std::move(offset))
        , m_trunkHeight(std::move(trunkHeight)) {}

    // The codec field (golden_oak_tree.json "trunk_height": 7); unused by
    // placement, as in Java.
    const std::shared_ptr<levelgen::carver::IntProvider>& trunkHeight() const { return m_trunkHeight; }

    int foliageHeight(
        levelgen::WorldgenRandom& random,
        int treeHeight,
        const levelgen::feature::configurations::TreeConfiguration& config
    ) const override {
        (void)random; (void)treeHeight; (void)config;
        return 7;
    }

protected:
    void createFoliageImpl(
        levelgen::feature::foliageplacers::FoliageSetter& foliageSetter,
        levelgen::WorldgenRandom& random,
        std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> foliageProvider,
        int treeHeight,
        const levelgen::feature::foliageplacers::FoliageAttachment& attachment,
        int foliageHeight,
        int leafRadius,
        int offset
    ) override;

    bool shouldSkipLocation(
        levelgen::WorldgenRandom& random,
        int dx, int y, int dz,
        int currentRadius,
        bool doubleTrunk
    ) const override {
        (void)currentRadius; (void)doubleTrunk;
        return dx * dx + (y + 2) * (y + 2) + dz * dz > 12 + random.nextInt(5);
    }
};

// ============================================================================
// Crystal tree placers
// ============================================================================

/**
 * CrystalTreeTrunkPlacer — world/trunkplacer/CrystalTreeTrunkPlacer.java.
 * A StraightTrunkPlacer (its own setDirtAt first, then super's, which sets the
 * dirt again — kept, as in Java) plus four side logs at heights 2 and 5, their
 * directions from Mth.cos/Mth.sin of an angle stepping a quarter turn (float
 * lookup table, as 1.21.1's Mth). One foliage attachment at the top.
 */
class CrystalTreeTrunkPlacer : public levelgen::feature::trunkplacers::StraightTrunkPlacer {
public:
    CrystalTreeTrunkPlacer(int baseHeight, int heightRandA, int heightRandB)
        : StraightTrunkPlacer(baseHeight, heightRandA, heightRandB) {}

    std::vector<levelgen::feature::foliageplacers::FoliageAttachment> placeTrunk(
        levelgen::feature::trunkplacers::LevelReader& level,
        levelgen::feature::trunkplacers::TrunkSetter trunkSetter,
        levelgen::WorldgenRandom& random,
        int treeHeight,
        const core::BlockPos& origin,
        std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> trunkProvider,
        std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> dirtProvider,
        bool forceDirt
    ) override;
};

/**
 * CrystalFoliagePlacer — world/foliageplacer/CrystalFoliagePlacer.java. Six
 * rows from `offset` down: ranges 0,1,1(+diamond),1,1(+diamond),2; corners of
 * each square skipped. foliageHeight = max(4, height - trunk_height.sample).
 */
class CrystalFoliagePlacer : public levelgen::feature::foliageplacers::FoliagePlacer {
private:
    std::shared_ptr<levelgen::carver::IntProvider> m_trunkHeight;

public:
    CrystalFoliagePlacer(std::shared_ptr<levelgen::carver::IntProvider> radius,
                         std::shared_ptr<levelgen::carver::IntProvider> offset,
                         std::shared_ptr<levelgen::carver::IntProvider> trunkHeight)
        : FoliagePlacer(std::move(radius), std::move(offset))
        , m_trunkHeight(std::move(trunkHeight)) {}

    int foliageHeight(
        levelgen::WorldgenRandom& random,
        int treeHeight,
        const levelgen::feature::configurations::TreeConfiguration& config
    ) const override {
        (void)config;
        const int trunk = m_trunkHeight->sample(random);
        return treeHeight - trunk > 4 ? treeHeight - trunk : 4;
    }

protected:
    void createFoliageImpl(
        levelgen::feature::foliageplacers::FoliageSetter& foliageSetter,
        levelgen::WorldgenRandom& random,
        std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> foliageProvider,
        int treeHeight,
        const levelgen::feature::foliageplacers::FoliageAttachment& attachment,
        int foliageHeight,
        int leafRadius,
        int offset
    ) override;

    bool shouldSkipLocation(
        levelgen::WorldgenRandom& random,
        int dx, int y, int dz,
        int currentRadius,
        bool doubleTrunk
    ) const override {
        (void)random; (void)y; (void)doubleTrunk;
        return dx == currentRadius && dz == currentRadius && currentRadius > 0;
    }
};

// ============================================================================
// Holiday tree
// ============================================================================

/**
 * HolidayFoliagePlacer — world/foliageplacer/HolidayFoliagePlacer.java. Eight
 * rows from `offset` down, each a combination of leaf rows and disk360 rings
 * in a Christmas-tree silhouette; corners skipped as CrystalFoliagePlacer.
 */
class HolidayFoliagePlacer : public levelgen::feature::foliageplacers::FoliagePlacer {
private:
    std::shared_ptr<levelgen::carver::IntProvider> m_trunkHeight;

public:
    HolidayFoliagePlacer(std::shared_ptr<levelgen::carver::IntProvider> radius,
                         std::shared_ptr<levelgen::carver::IntProvider> offset,
                         std::shared_ptr<levelgen::carver::IntProvider> trunkHeight)
        : FoliagePlacer(std::move(radius), std::move(offset))
        , m_trunkHeight(std::move(trunkHeight)) {}

    int foliageHeight(
        levelgen::WorldgenRandom& random,
        int treeHeight,
        const levelgen::feature::configurations::TreeConfiguration& config
    ) const override {
        (void)config;
        const int trunk = m_trunkHeight->sample(random);
        return treeHeight - trunk > 4 ? treeHeight - trunk : 4;
    }

protected:
    void createFoliageImpl(
        levelgen::feature::foliageplacers::FoliageSetter& foliageSetter,
        levelgen::WorldgenRandom& random,
        std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> foliageProvider,
        int treeHeight,
        const levelgen::feature::foliageplacers::FoliageAttachment& attachment,
        int foliageHeight,
        int leafRadius,
        int offset
    ) override;

    bool shouldSkipLocation(
        levelgen::WorldgenRandom& random,
        int dx, int y, int dz,
        int currentRadius,
        bool doubleTrunk
    ) const override {
        (void)random; (void)y; (void)doubleTrunk;
        return dx == currentRadius && dz == currentRadius && currentRadius > 0;
    }

private:
    void disk360(levelgen::feature::foliageplacers::FoliageSetter& foliageSetter,
                 levelgen::WorldgenRandom& random,
                 const std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider>& provider,
                 bool doubleTrunk, const core::BlockPos& pos, int height, int distance, int range);
};

/**
 * HolidayTreeDecorator — world/treedecorator/HolidayTreeDecorator.java. Around
 * every lowest-row log, a radius-10 disc of columns (y +9 down to -4) gets
 * snow on leaves, or a provider block (snow 10 : present 1) on grass/dirt,
 * with a falloff chance by distance.
 */
class HolidayTreeDecorator : public levelgen::feature::treedecorators::TreeDecorator {
private:
    std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> m_provider;
    BlockState* m_snow;

public:
    HolidayTreeDecorator(std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> provider,
                         BlockState* snow)
        : m_provider(std::move(provider)), m_snow(snow) {}

    void place(levelgen::feature::treedecorators::DecoratorContext& context) override;

private:
    void placeCircle(levelgen::feature::treedecorators::DecoratorContext& context,
                     const core::BlockPos& pos);
    void placeBlockAt(levelgen::feature::treedecorators::DecoratorContext& context,
                      const core::BlockPos& pos, float distance);
};

// ============================================================================
// CrystalIslandFeature — world/feature/CrystalIslandFeature.java
// ============================================================================

/**
 * Places the crystal tree (an inline placed feature, no modifiers) one block
 * above the origin; when it grows, a three-layer island under it — aether
 * grass on top, holystone below — each block only inside the 3x3 chunk write
 * window (BlockLogicUtil.isOutOfBounds).
 */
class CrystalIslandFeature : public levelgen::Feature<levelgen::NoneFeatureConfiguration> {
public:
    levelgen::placement::PlacedFeature* crystalTree = nullptr;
    BlockState* grass = nullptr;
    BlockState* holystone = nullptr;

    bool place(levelgen::FeaturePlaceContext<levelgen::NoneFeatureConfiguration>& context) override;

private:
    void setIslandBlock(levelgen::WorldGenLevel* level, const core::BlockPos& pos, BlockState* state);
};

// ============================================================================
// AetherLakeFeature — world/feature/AetherLakeFeature.java
// ============================================================================

/**
 * AetherLakeConfiguration — world/configuration/AetherLakeConfiguration.java
 * (BlockStateProvider fluid, BlockStateProvider top). Both simple providers.
 */
class AetherLakeConfiguration : public levelgen::FeatureConfiguration {
public:
    BlockState* fluid;
    BlockState* top;

    AetherLakeConfiguration(BlockState* fluid, BlockState* top) : fluid(fluid), top(top) {}
};

/**
 * [CODE COPY] of the pre-1.18 LakeFeature, water only: the ellipsoid grid,
 * the liquid/wall checks, fluid below the waterline and cave air above, then
 * `top` on every dirt block the carved air exposes, then ice where the biome
 * freezes. level.getBrightness(SKY, …) > 0 is always true during worldgen
 * (the column is not yet lit and SkyLightSectionStorage reports 15), so the
 * port leaves that test out.
 */
class AetherLakeFeature : public levelgen::Feature<AetherLakeConfiguration> {
public:
    bool place(levelgen::FeaturePlaceContext<AetherLakeConfiguration>& context) override;
};

/**
 * AetherFeatures - Registry of The Aether's configured features
 *
 *   SKYROOT_TREE                 skyroot_tree.json: straight trunk 4 + [0,2],
 *                                blob foliage r2 h3, two-layers size (1, 0, 1)
 *   GOLDEN_OAK_TREE              golden_oak_tree.json: GoldenOakTrunkPlacer
 *                                (10, 0, 0), GoldenOakFoliagePlacer (radius 3,
 *                                offset 1, trunk_height 7), two-layers size
 *                                (0, 0, 0, min clipped height 10)
 *   TREES_SKYROOT_AND_GOLDEN_OAK random_selector: golden oak 0.01, default
 *                                skyroot, each behind its sapling's
 *                                would_survive filter
 *   COLD_AERCLOUD / BLUE_AERCLOUD / GOLDEN_AERCLOUD   aercloud, bounds 16 / 8 / 4
 *   QUICKSOIL_SHELF              shelf: quicksoil, radius 3.4641016, y 0..48,
 *                                under aether grass
 *   AETHER_DIRT_ORE / ICESTONE_ORE / AMBROSIUM_ORE / ZANITE_ORE /
 *   GRAVITITE_ORE / GRAVITITE_ORE_BURIED   ore in holystone
 *   GRASS_PATCH / TALL_GRASS_PATCH / WHITE_FLOWER_PATCH /
 *   PURPLE_FLOWER_PATCH / BERRY_BUSH_PATCH  random patches, only in air
 *   CRYSTAL_TREE                 crystal_tree.json: CrystalTreeTrunkPlacer (7, 0, 0),
 *                                CrystalFoliagePlacer (0, 0, trunk 6), crystal
 *                                leaves 4 : crystal fruit leaves 1
 *   CRYSTAL_ISLAND               aether:crystal_island (crystal tree + island)
 *   HOLIDAY_TREE                 holiday_tree.json: straight trunk 9, holiday
 *                                foliage (trunk 8), holiday leaves 4 : decorated
 *                                1, HolidayTreeDecorator (snow 10 : present 1)
 *   WATER_LAKE                   aether:lake (water, aether grass top)
 *   WATER_SPRING                 spring_feature: water in holystone / aether dirt
 *
 * The four pass-two features are never null: a missing block falls back to a
 * named stand-in (logged once) so their FeatureSorter indices — and with them
 * every later feature's seed — always match the mod.
 *
 * Every feature resolves its own blocks: a block the registry does not have
 * leaves that feature null (logged once), never throws — this bootstrap runs
 * inside BiomeFeatureRegistry::bootstrap() for every dimension.
 */
class AetherFeatures {
private:
    static levelgen::OreFeature s_oreFeature;
    static std::shared_ptr<levelgen::feature::TreeFeature> s_treeFeature;
    static levelgen::RandomPatchFeature s_randomPatchFeature;
    static levelgen::SimpleBlockFeature s_simpleBlockFeature;
    static levelgen::RandomSelectorFeature s_randomSelectorFeature;
    static AercloudFeature s_aercloudFeature;
    static ShelfFeature s_shelfFeature;
    static CrystalIslandFeature s_crystalIslandFeature;
    static AetherLakeFeature s_lakeFeature;
    static levelgen::SpringFeature s_springFeature;

    static bool s_initialized;

public:
    static levelgen::ConfiguredFeature* SKYROOT_TREE;
    static levelgen::ConfiguredFeature* GOLDEN_OAK_TREE;
    static levelgen::ConfiguredFeature* TREES_SKYROOT_AND_GOLDEN_OAK;

    static levelgen::ConfiguredFeature* COLD_AERCLOUD;
    static levelgen::ConfiguredFeature* BLUE_AERCLOUD;
    static levelgen::ConfiguredFeature* GOLDEN_AERCLOUD;

    static levelgen::ConfiguredFeature* QUICKSOIL_SHELF;

    static levelgen::ConfiguredFeature* AETHER_DIRT_ORE;
    static levelgen::ConfiguredFeature* ICESTONE_ORE;
    static levelgen::ConfiguredFeature* AMBROSIUM_ORE;
    static levelgen::ConfiguredFeature* ZANITE_ORE;
    static levelgen::ConfiguredFeature* GRAVITITE_ORE_BURIED;
    static levelgen::ConfiguredFeature* GRAVITITE_ORE;

    static levelgen::ConfiguredFeature* GRASS_PATCH;
    static levelgen::ConfiguredFeature* TALL_GRASS_PATCH;
    static levelgen::ConfiguredFeature* WHITE_FLOWER_PATCH;
    static levelgen::ConfiguredFeature* PURPLE_FLOWER_PATCH;
    static levelgen::ConfiguredFeature* BERRY_BUSH_PATCH;

    static levelgen::ConfiguredFeature* CRYSTAL_TREE;
    static levelgen::ConfiguredFeature* CRYSTAL_ISLAND;
    static levelgen::ConfiguredFeature* HOLIDAY_TREE;
    static levelgen::ConfiguredFeature* WATER_LAKE;
    static levelgen::ConfiguredFeature* WATER_SPRING;

    /**
     * Bootstrap/initialize all Aether configured features. A missing block
     * leaves only the features that use it null.
     */
    static void bootstrap();

    static bool isInitialized() { return s_initialized; }
};

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft

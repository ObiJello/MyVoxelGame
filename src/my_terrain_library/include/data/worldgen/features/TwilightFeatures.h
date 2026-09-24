#pragma once

#include "levelgen/feature/Feature.h"
#include "levelgen/feature/TreeFeature.h"
#include "levelgen/feature/configurations/TreeConfiguration.h"
#include "levelgen/feature/trunkplacers/TrunkPlacer.h"
#include "levelgen/feature/foliageplacers/FoliagePlacer.h"
#include "levelgen/feature/treedecorators/TreeDecorator.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "levelgen/carver/CarverConfiguration.h"
#include "core/BlockPos.h"
#include "world/level/block/Blocks.h"
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Twilight Forest 4.9 (DimensionId::TwilightForest) — configured features
// ported from data/twilightforest/worldgen/configured_feature/**.json and the
// Java classes named at each piece (world/components/feature/**,
// util/features/{FeatureLogic,FeaturePlacers}.java). Pass one: trees and
// their placers/decorators, canopy mushrooms, flora, small lakes, ores.
// The engine registers every ported TF block as minecraft:<slug>, so each
// "twilightforest:<name>" state in the JSON resolves as "minecraft:<name>";
// TF blocks not ported yet are replaced by the stand-ins listed in the .cpp.

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

// ============================================================================
// Shared helpers — util/iterators/VoxelBresenhamIterator.java,
// util/features/FeatureLogic.java, util/features/FeaturePlacers.java
// ============================================================================
namespace twilight {

/**
 * VoxelBresenhamIterator — a stateful 3D Bresenham walk from `from` to `to`
 * (both inclusive). Stateful on purpose: FeaturePlacers.traceExposedRoot hands
 * the SAME iterator on to traceRoot, which continues from where it stopped.
 */
class VoxelBresenhamIterator {
public:
    VoxelBresenhamIterator(const core::BlockPos& from, const core::BlockPos& to);

    bool hasNext() const { return m_i < m_length; }
    core::BlockPos next();

private:
    enum class Axis { X, Y, Z };
    int32_t m_xInc, m_yInc, m_zInc;
    int32_t m_doubleAbsDx, m_doubleAbsDy, m_doubleAbsDz;
    int32_t m_length;
    int32_t m_x, m_y, m_z;
    Axis m_direction;
    int32_t m_i = 0;
    int32_t m_err1 = 0;
    int32_t m_err2 = 0;
};

/**
 * FeatureLogic.translate — moves `distance` along a vector whose angle is a
 * fraction of a full turn (0..1) and whose tilt is 0 = up, 0.5 = out, 1 = down.
 */
core::BlockPos translate(const core::BlockPos& pos, double distance, double angle, double tilt);

/** FeatureLogic.isReplaceable(state, includeFlowers). */
bool isReplaceable(BlockState* state, bool includeFlowers);

/** FeaturePlacers.validTreePos: TreeFeature.validTreePos or #minecraft:flowers. */
bool validTreePos(levelgen::WorldGenLevel& level, const core::BlockPos& pos);

} // namespace twilight

// ============================================================================
// Tree placers — world/components/feature/trees/treeplacers/*.java
// ============================================================================

/**
 * BranchesConfig — treeplacers/BranchesConfig.java. `randomAddLength` is part
 * of the codec but BranchingTrunkPlacer never reads it (as in the mod).
 */
struct BranchesConfig {
    std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> branchProvider;
    int32_t branchCount;
    int32_t randomAddBranches;
    double length;
    double randomAddLength;
    double spacingYaw;
    double downwardsPitch;
};

/**
 * BranchingTrunkPlacer — treeplacers/BranchingTrunkPlacer.java. A straight
 * trunk to the full height (shortened where a log cannot be placed), then
 * count + nextInt(randomAdd + 1) Bresenham branches spiralling out from
 * `height - branchDownwardOffset + b`, each ending in a 4-wood cross and a
 * foliage attachment. No dirt below the trunk: only the optional
 * prevent-exposed-root log under the base.
 */
class BranchingTrunkPlacer : public levelgen::feature::trunkplacers::TrunkPlacer {
public:
    BranchingTrunkPlacer(int baseHeight, int heightRandA, int heightRandB,
                         int branchDownwardOffset, BranchesConfig branchesConfig,
                         bool perpendicularBranches, bool preventExposedRoot)
        : TrunkPlacer(baseHeight, heightRandA, heightRandB)
        , m_branchDownwardOffset(branchDownwardOffset)
        , m_branchesConfig(std::move(branchesConfig))
        , m_perpendicularBranches(perpendicularBranches)
        , m_preventExposedRoot(preventExposedRoot) {}

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
    void buildBranch(levelgen::feature::trunkplacers::LevelReader& level,
                     levelgen::feature::trunkplacers::TrunkSetter& trunkSetter,
                     const core::BlockPos& pos,
                     std::vector<levelgen::feature::foliageplacers::FoliageAttachment>& leafBlocks,
                     int height, double length, double angle, double tilt,
                     levelgen::WorldgenRandom& random);
    bool placeWood(levelgen::feature::trunkplacers::LevelReader& level,
                   levelgen::feature::trunkplacers::TrunkSetter& trunkSetter,
                   levelgen::WorldgenRandom& random, const core::BlockPos& pos);

    int m_branchDownwardOffset;
    BranchesConfig m_branchesConfig;
    bool m_perpendicularBranches;
    bool m_preventExposedRoot;
};

/**
 * TrunkRiser — treeplacers/TrunkRiser.java ("twilightforest:trunk_mover_upper").
 * Takes the wrapped placer's heights and runs it `offset` blocks higher.
 */
class TrunkRiser : public levelgen::feature::trunkplacers::TrunkPlacer {
public:
    TrunkRiser(int offset, int innerBaseHeight, int innerRandA, int innerRandB,
               std::shared_ptr<levelgen::feature::trunkplacers::TrunkPlacer> placer)
        : TrunkPlacer(innerBaseHeight, innerRandA, innerRandB)
        , m_offset(offset)
        , m_placer(std::move(placer)) {}

    std::vector<levelgen::feature::foliageplacers::FoliageAttachment> placeTrunk(
        levelgen::feature::trunkplacers::LevelReader& level,
        levelgen::feature::trunkplacers::TrunkSetter trunkSetter,
        levelgen::WorldgenRandom& random,
        int treeHeight,
        const core::BlockPos& origin,
        std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> trunkProvider,
        std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> dirtProvider,
        bool forceDirt
    ) override {
        return m_placer->placeTrunk(level, std::move(trunkSetter), random, treeHeight,
                                    origin.above(m_offset), std::move(trunkProvider),
                                    std::move(dirtProvider), forceDirt);
    }

private:
    int m_offset;
    std::shared_ptr<levelgen::feature::trunkplacers::TrunkPlacer> m_placer;
};

/**
 * LeafSpheroidFoliagePlacer — treeplacers/LeafSpheroidFoliagePlacer.java
 * ("twilightforest:spheroid_foliage_placer"). A biased spheroid of leaves
 * (FeaturePlacers.placeSpheroid) plus `shagFactor` 2x2 leaf clusters on its
 * surface. foliageHeight is 0 and the radius provider is the constant
 * (int) horizontalRadius, as the mod's super() call builds it.
 */
class LeafSpheroidFoliagePlacer : public levelgen::feature::foliageplacers::FoliagePlacer {
public:
    LeafSpheroidFoliagePlacer(float horizontalRadius, float verticalRadius,
                              std::shared_ptr<levelgen::carver::IntProvider> yOffset,
                              int randomHorizontal, int randomVertical,
                              float verticalBias, int shagFactor);

    int foliageHeight(levelgen::WorldgenRandom& random, int treeHeight,
                      const levelgen::feature::configurations::TreeConfiguration& config) const override {
        (void)random; (void)treeHeight; (void)config;
        return 0;
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

    bool shouldSkipLocation(levelgen::WorldgenRandom& random, int dx, int y, int dz,
                            int currentRadius, bool doubleTrunk) const override {
        (void)random; (void)dx; (void)y; (void)dz; (void)currentRadius; (void)doubleTrunk;
        return false;
    }

private:
    float m_horizontalRadius;
    float m_verticalRadius;
    float m_verticalBias;
    int m_randomHorizontal;
    int m_randomVertical;
    int m_shagFactor;
};

/**
 * TreeRootsDecorator — treeplacers/TreeRootsDecorator.java
 * ("twilightforest:tree_roots"). strands + nextInt(extra + 1) roots traced
 * from under the lowest log, spiralling down (tilt 0.8). With a surface
 * provider (the mangrove's exposed roots) each root runs exposed above
 * ground and switches to ground roots once buried.
 */
class TreeRootsDecorator : public levelgen::feature::treedecorators::TreeDecorator {
public:
    TreeRootsDecorator(int strands, int addExtraStrands, int length, int yOffset,
                       std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> surfaceBlock,
                       std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> rootBlock,
                       int rootPenetrability)
        : m_strands(strands)
        , m_addExtraStrands(addExtraStrands)
        , m_length(length)
        , m_yOffset(yOffset)
        , m_surfaceBlock(std::move(surfaceBlock))
        , m_rootBlock(std::move(rootBlock))
        , m_rootPenetrability(rootPenetrability) {}

    void place(levelgen::feature::treedecorators::DecoratorContext& context) override;

private:
    int m_strands;
    int m_addExtraStrands;
    int m_length;
    int m_yOffset;
    std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> m_surfaceBlock;   // null = no surface roots
    std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> m_rootBlock;
    int m_rootPenetrability;
};

/**
 * TrunkSideDecorator — treeplacers/TrunkSideDecorator.java
 * ("twilightforest:trunkside_decorator"): `count` tries, each at `probability`,
 * putting the decoration beside a random log in a random horizontal
 * direction, facing that direction when the state has FACING.
 */
class TrunkSideDecorator : public levelgen::feature::treedecorators::TreeDecorator {
public:
    TrunkSideDecorator(int count, float probability,
                       std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> decoration)
        : m_count(count), m_probability(probability), m_decoration(std::move(decoration)) {}

    void place(levelgen::feature::treedecorators::DecoratorContext& context) override;

private:
    int m_count;
    float m_probability;
    std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> m_decoration;
};

/**
 * DangleFromTreeDecorator — treeplacers/DangleFromTreeDecorator.java
 * ("twilightforest:dangle_from_tree_decorator"): ropes hanging from random
 * leaves with a piece of baggage on the end.
 */
class DangleFromTreeDecorator : public levelgen::feature::treedecorators::TreeDecorator {
public:
    DangleFromTreeDecorator(int count, int randomAddCount, int minimumRequiredLength,
                            int baseLength, int randomAddLength,
                            std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> rope,
                            std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> baggage)
        : m_count(count), m_randomAddCount(randomAddCount)
        , m_minimumRequiredLength(minimumRequiredLength)
        , m_baseLength(baseLength), m_randomAddLength(randomAddLength)
        , m_rope(std::move(rope)), m_baggage(std::move(baggage)) {}

    void place(levelgen::feature::treedecorators::DecoratorContext& context) override;

private:
    int m_count;
    int m_randomAddCount;
    int m_minimumRequiredLength;
    int m_baseLength;
    int m_randomAddLength;
    std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> m_rope;
    std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> m_baggage;
};

// ============================================================================
// Features — world/components/feature/**
// ============================================================================

/**
 * DarkCanopyTreeFeature — trees/DarkCanopyTreeFeature.java
 * ("twilightforest:dark_canopy_tree"). Seeks down through leaves to the first
 * substrate block, requires four free blocks above and no log beside the
 * base, then runs the vanilla tree placement there (the mod's doPlace is a
 * [VanillaCopy] of TreeFeature's, so the library's TreeFeature does it).
 */
class DarkCanopyTreeFeature {
public:
    explicit DarkCanopyTreeFeature(std::shared_ptr<levelgen::feature::TreeFeature> treeFeature)
        : m_treeFeature(std::move(treeFeature)) {}

    bool place(levelgen::FeaturePlaceContext<levelgen::feature::configurations::TreeConfiguration>& context);

private:
    std::shared_ptr<levelgen::feature::TreeFeature> m_treeFeature;
};

/**
 * CanopyMushroomFeature — trees/CanopyMushroomFeature.java with
 * BrownCanopyMushroomFeature / RedCanopyMushroomFeature (cap styles 0 vanilla,
 * 1 smooth, 2 spheroid, 3 flat). The place() flow is 26.1's
 * AbstractHugeMushroomFeature.place: height, isValidPosition, cap, trunk —
 * and the trunk grows branches with their own small caps and fireflies.
 */
class CanopyMushroomFeature : public levelgen::Feature<levelgen::HugeMushroomFeatureConfiguration> {
public:
    enum class Kind { BROWN, RED_VANILLA, RED_SMOOTH, RED_SPHEROID, RED_FLAT };

    explicit CanopyMushroomFeature(Kind kind) : m_kind(kind) {}

    bool place(levelgen::FeaturePlaceContext<levelgen::HugeMushroomFeatureConfiguration>& context) override;

private:
    int getTreeHeight(levelgen::WorldgenRandom& random) const;
    int getBranches(levelgen::WorldgenRandom& random) const;
    double getLength(levelgen::WorldgenRandom& random) const;
    bool isValidPosition(levelgen::WorldGenLevel* level, const core::BlockPos& origin, int treeHeight,
                         const levelgen::HugeMushroomFeatureConfiguration& config) const;
    void placeTrunk(levelgen::WorldGenLevel* level, levelgen::WorldgenRandom& random,
                    const core::BlockPos& pos, const levelgen::HugeMushroomFeatureConfiguration& config,
                    int height);
    int buildABranch(levelgen::WorldGenLevel* level, const core::BlockPos& pos, int height,
                     double length, double angle, levelgen::WorldgenRandom& random,
                     const levelgen::HugeMushroomFeatureConfiguration& config, int bugsLeft);
    bool addFirefly(levelgen::WorldGenLevel* level, const core::BlockPos& pos, levelgen::WorldgenRandom& random);
    void makeCap(levelgen::WorldGenLevel* level, levelgen::WorldgenRandom& random, const core::BlockPos& pos,
                 int height, const levelgen::HugeMushroomFeatureConfiguration& config);
    void makeFlatCap(levelgen::WorldGenLevel* level, levelgen::WorldgenRandom& random, const core::BlockPos& pos,
                     int height, const levelgen::HugeMushroomFeatureConfiguration& config);
    void makeVanillaCap(levelgen::WorldGenLevel* level, levelgen::WorldgenRandom& random, const core::BlockPos& pos,
                        int height, const levelgen::HugeMushroomFeatureConfiguration& config);
    void makeSmoothCap(levelgen::WorldGenLevel* level, levelgen::WorldgenRandom& random, const core::BlockPos& pos,
                       int height, const levelgen::HugeMushroomFeatureConfiguration& config);
    void makeSpheroidCap(levelgen::WorldGenLevel* level, levelgen::WorldgenRandom& random, const core::BlockPos& pos,
                         int height, const levelgen::HugeMushroomFeatureConfiguration& config);
    static bool isReplaceableAt(levelgen::WorldGenLevel* level, const core::BlockPos& pos);

    Kind m_kind;
};

/** WeightedListFeatureConfig — feature/config/WeightedListFeatureConfig.java. */
struct WeightedListFeatureConfig {
    std::vector<std::pair<levelgen::placement::PlacedFeature*, int32_t>> entries;
};

/**
 * WeightedListFeature — feature/WeightedListFeature.java
 * ("twilightforest:weighted_list"): one WeightedList.getRandom draw.
 */
class WeightedListFeature : public levelgen::Feature<WeightedListFeatureConfig> {
public:
    bool place(levelgen::FeaturePlaceContext<WeightedListFeatureConfig>& context) override;
};

/** TFSmallLakeFeature.Configuration — fluid, optional barrier, optional ice. */
struct SmallLakeConfiguration {
    BlockState* fluid = nullptr;
    BlockState* barrier = nullptr;   // null = none
    BlockState* ice = nullptr;       // null = none
};

/**
 * TFSmallLakeFeature — feature/TFSmallLakeFeature.java ("twilightforest:
 * small_lake"): the legacy 16x8x16 blob lake, air above y 4 of the blob,
 * an optional ice layer at y 3 and an optional solid barrier around it.
 */
class TFSmallLakeFeature : public levelgen::Feature<SmallLakeConfiguration> {
public:
    bool place(levelgen::FeaturePlaceContext<SmallLakeConfiguration>& context) override;
};

/**
 * FallenLeavesFeature — feature/FallenLeavesFeature.java
 * ("twilightforest:fallen_leaves"): a mound of fallen_leaves layers, or a
 * flat scatter over water.
 */
class FallenLeavesFeature : public levelgen::Feature<levelgen::NoneFeatureConfiguration> {
public:
    bool place(levelgen::FeaturePlaceContext<levelgen::NoneFeatureConfiguration>& context) override;

private:
    bool canPlace(const core::BlockPos& pos, levelgen::WorldGenLevel* level) const;
    void generateFlatPileOnWater(levelgen::WorldGenLevel* level, const core::BlockPos& pos,
                                 levelgen::WorldgenRandom& random, BlockState* leaves);
    void generateCircleOfLeaves(levelgen::WorldGenLevel* level, const core::BlockPos& origin,
                                levelgen::WorldgenRandom& random, int radius, int height, BlockState* leaves);
    void checkAndGenerateLeafPile(levelgen::WorldGenLevel* level, const core::BlockPos& pos,
                                  int pileLayer, BlockState* leaves);
};

/**
 * UndergroundPlantFeature — feature/UndergroundPlantFeature.java
 * ("twilightforest:underground_plants"): walks down the column from the
 * origin, jittering sideways on solid blocks (or 1 in 6), placing the plant
 * wherever it survives. Landmark exclusion is pass two (no structures yet).
 */
class UndergroundPlantFeature : public levelgen::Feature<levelgen::BlockStateConfiguration> {
public:
    bool place(levelgen::FeaturePlaceContext<levelgen::BlockStateConfiguration>& context) override;
};

// ============================================================================
// Registry
// ============================================================================

/**
 * TwilightFeatures - Registry of the Twilight Forest's pass-one configured
 * features. Every feature resolves its own blocks: a block the registry does
 * not have leaves that feature null (logged once), never throws — this
 * bootstrap runs inside BiomeFeatureRegistry::bootstrap() for every
 * dimension.
 */
class TwilightFeatures {
private:
    static bool s_initialized;

public:
    // ---- trees (configured_feature/tree/*.json) ----
    static levelgen::ConfiguredFeature* TWILIGHT_OAK_TREE;
    static levelgen::ConfiguredFeature* LARGE_TWILIGHT_OAK_TREE;
    static levelgen::ConfiguredFeature* SWAMPY_OAK_TREE;
    static levelgen::ConfiguredFeature* OAK_BUSH;
    static levelgen::ConfiguredFeature* CANOPY_TREE;
    static levelgen::ConfiguredFeature* FIREFLY_CANOPY_TREE;
    static levelgen::ConfiguredFeature* DEAD_CANOPY_TREE;
    static levelgen::ConfiguredFeature* MANGROVE_TREE;
    static levelgen::ConfiguredFeature* DARKWOOD_TREE;
    static levelgen::ConfiguredFeature* DARK_FOREST_OAK_TREE;
    static levelgen::ConfiguredFeature* DARK_FOREST_BIRCH_TREE;
    static levelgen::ConfiguredFeature* DARK_OAK_BUSH;
    static levelgen::ConfiguredFeature* VANILLA_OAK_TREE;
    static levelgen::ConfiguredFeature* VANILLA_BIRCH_TREE;
    static levelgen::ConfiguredFeature* RAINBOW_OAK;
    static levelgen::ConfiguredFeature* LARGE_RAINBOW_OAK;
    static levelgen::ConfiguredFeature* MEGA_SPRUCE_TREE;
    static levelgen::ConfiguredFeature* SNOWY_SPRUCE_TREE;

    // ---- tree selectors (configured_feature/tree/selector/**.json) ----
    static levelgen::ConfiguredFeature* CANOPY_TREES;
    static levelgen::ConfiguredFeature* DENSE_CANOPY_TREES;
    static levelgen::ConfiguredFeature* FIREFLY_FOREST_TREES;
    static levelgen::ConfiguredFeature* ENCHANTED_FOREST_TREES;
    static levelgen::ConfiguredFeature* HIGHLANDS_TREES;
    static levelgen::ConfiguredFeature* SNOWY_FOREST_TREES;
    static levelgen::ConfiguredFeature* VANILLA_TREES;
    static levelgen::ConfiguredFeature* DARK_FOREST_TREES;
    static levelgen::ConfiguredFeature* VANILLA_MUSHROOMS;

    // ---- mushrooms (configured_feature/mushroom/*.json) ----
    static levelgen::ConfiguredFeature* BROWN_CANOPY_MUSHROOM;
    static levelgen::ConfiguredFeature* RED_CANOPY_MUSHROOM;
    static levelgen::ConfiguredFeature* CANOPY_MUSHROOMS_DENSE;
    static levelgen::ConfiguredFeature* CANOPY_MUSHROOMS_SPARSE;

    // ---- flora (simple_block; placed with the 26.1 patch modifier chain) ----
    static levelgen::ConfiguredFeature* MUSHGLOOM_CLUSTER;
    static levelgen::ConfiguredFeature* DARK_MUSHGLOOMS;
    static levelgen::ConfiguredFeature* DARK_BROWN_MUSHROOMS;
    static levelgen::ConfiguredFeature* DARK_RED_MUSHROOMS;
    static levelgen::ConfiguredFeature* DARK_DEAD_BUSHES;
    static levelgen::ConfiguredFeature* DARK_PUMPKINS;
    static levelgen::ConfiguredFeature* DARK_GRASS;
    static levelgen::ConfiguredFeature* DARK_FERNS;
    static levelgen::ConfiguredFeature* FIDDLEHEAD;
    static levelgen::ConfiguredFeature* MAYAPPLE;
    static levelgen::ConfiguredFeature* FLOWER_PLACER;
    static levelgen::ConfiguredFeature* FLOWER_PLACER_ALT;
    static levelgen::ConfiguredFeature* DENSE_FERNS;
    static levelgen::ConfiguredFeature* DENSE_LARGE_FERNS;
    // 26.1 vanilla configured features the TF patch_grass_* placements name
    // (VegetationFeatures GRASS / GRASS_JUNGLE / TAIGA_GRASS: bare
    // simple_block, unlike the library's 1.21-style random-patch versions).
    static levelgen::ConfiguredFeature* GRASS;
    static levelgen::ConfiguredFeature* GRASS_JUNGLE;
    static levelgen::ConfiguredFeature* TAIGA_GRASS;

    // ---- misc ----
    static levelgen::ConfiguredFeature* TORCH_BERRIES;
    static levelgen::ConfiguredFeature* FALLEN_LEAVES;
    static levelgen::ConfiguredFeature* WATER_LAKE;
    static levelgen::ConfiguredFeature* LAVA_LAKE;
    static levelgen::ConfiguredFeature* WATER_FROZEN;

    // ---- ores (minecraft:ore in #minecraft:stone_ore_replaceables) ----
    static levelgen::ConfiguredFeature* LEGACY_COAL_ORE;
    static levelgen::ConfiguredFeature* LEGACY_IRON_ORE;
    static levelgen::ConfiguredFeature* LEGACY_GOLD_ORE;
    static levelgen::ConfiguredFeature* LEGACY_REDSTONE_ORE;
    static levelgen::ConfiguredFeature* LEGACY_DIAMOND_ORE;
    static levelgen::ConfiguredFeature* LEGACY_LAPIS_ORE;
    static levelgen::ConfiguredFeature* LEGACY_COPPER_ORE;
    static levelgen::ConfiguredFeature* SMALL_ANDESITE;
    static levelgen::ConfiguredFeature* SMALL_DIORITE;
    static levelgen::ConfiguredFeature* SMALL_GRANITE;

    /** Bootstrap all TF configured features; a missing block nulls only its features. */
    static void bootstrap();

    static bool isInitialized() { return s_initialized; }
};

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft

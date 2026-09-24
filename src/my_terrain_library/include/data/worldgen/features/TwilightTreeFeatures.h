#pragma once

#include "levelgen/feature/Feature.h"
#include "levelgen/feature/treedecorators/TreeDecorator.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/WorldgenRandom.h"
#include "util/IntProvider.h"
#include "core/BlockPos.h"
#include "world/level/block/state/BlockState.h"
#include <array>
#include <climits>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

// Twilight Forest 4.9 (DimensionId::TwilightForest) — pass two: the giant
// trees, the fallen/hollow logs and stumps, the underground roots and the
// small ground features the TF biomes place. Every class mirrors the Java
// class named at it (world/components/feature/**, util/features/
// {FeatureLogic,FeaturePlacers,FeatureUtil}.java) with its random draws in
// the same order. Configured features are registered under their JSON ids
// (data/twilightforest/worldgen/configured_feature/<path>.json) through
// TwilightFeatureRegistry; TwilightPlacements builds the placed features.
// TF blocks resolve through levelgen/TwilightBlocks.h (real slug first, else
// a logged stand-in) once, at bootstrap.

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

namespace twilight {

/** A BiConsumer<BlockPos, BlockState> placer. */
using TreeBlockSetter = std::function<void(const core::BlockPos&, BlockState*)>;

/**
 * RootPlacer — util/RootPlacer.java: the placer that root tracing writes
 * through, plus how many blocks below a root position must also admit roots.
 */
struct RootPlacer {
    TreeBlockSetter placer;
    int rootPenetrability = 1;
};

} // namespace twilight

// ============================================================================
// Tree features — world/components/feature/trees/*.java
// ============================================================================

/**
 * TFTreeFeatureConfig — feature/config/TFTreeFeatureConfig.java. The codec's
 * orElse defaults are 20 / 1 / 1 / true / false; both chances are clamped
 * to at least 1 by the constructor, as in the mod.
 */
struct TFTreeFeatureConfig {
    std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> trunkProvider;
    std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> leavesProvider;
    std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> branchProvider;
    std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> rootsProvider;
    int minHeight = 20;
    int chanceAddFiveFirst = 1;
    int chanceAddFiveSecond = 1;
    bool hasLeaves = true;
    bool checkWater = false;
    std::vector<std::shared_ptr<levelgen::feature::treedecorators::TreeDecorator>> decorators;

    TFTreeFeatureConfig() = default;
    TFTreeFeatureConfig(std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> trunk,
                        std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> leaves,
                        std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> branch,
                        std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> roots,
                        int height, int chanceFiveFirst, int chanceFiveSecond,
                        bool leavesFlag, bool water,
                        std::vector<std::shared_ptr<levelgen::feature::treedecorators::TreeDecorator>> decos)
        : trunkProvider(std::move(trunk))
        , leavesProvider(std::move(leaves))
        , branchProvider(std::move(branch))
        , rootsProvider(std::move(roots))
        , minHeight(height)
        , chanceAddFiveFirst(chanceFiveFirst < 1 ? 1 : chanceFiveFirst)
        , chanceAddFiveSecond(chanceFiveSecond < 1 ? 1 : chanceFiveSecond)
        , hasLeaves(leavesFlag)
        , checkWater(water)
        , decorators(std::move(decos)) {}
};

/**
 * TFTreeFeature — trees/TFTreeFeature.java. place() is the mod's
 * [VanillaCopy] of TreeFeature.place: four tracking setters (trunk, leaves,
 * roots, decorations; flags UPDATE_KNOWN_SHAPE | UPDATE_ALL), generate(),
 * the decorators, then TreeFeature.updateLeaves + updateShapeAtEdge over the
 * bounding box — with the mod's own argument order (its leaves set is handed
 * to updateLeaves as the "logs" and its trunk set as the "roots").
 */
class TFTreeFeature : public levelgen::Feature<TFTreeFeatureConfig> {
public:
    bool place(levelgen::FeaturePlaceContext<TFTreeFeatureConfig>& context) final;

protected:
    struct Placers {
        twilight::TreeBlockSetter trunk;
        twilight::TreeBlockSetter leaves;
        twilight::RootPlacer decoration;
    };

    virtual bool generate(levelgen::WorldGenLevel& level, levelgen::WorldgenRandom& random,
                          const core::BlockPos& pos, const Placers& placers,
                          const TFTreeFeatureConfig& config) = 0;
};

/**
 * MegaCanopyTreeFeature — trees/MegaCanopyTreeFeature.java
 * ("twilightforest:mega_canopy"): a 2x2 canopy trunk, 6-8 double branches
 * ending in flat legacy-distance leaf discs with wood spokes, roots under
 * each trunk column.
 */
class MegaCanopyTreeFeature : public TFTreeFeature {
protected:
    bool generate(levelgen::WorldGenLevel& level, levelgen::WorldgenRandom& random,
                  const core::BlockPos& pos, const Placers& placers,
                  const TFTreeFeatureConfig& config) override;

private:
    void buildTrunk(levelgen::WorldGenLevel& level, std::vector<core::BlockPos>& leaves,
                    const Placers& placers, levelgen::WorldgenRandom& random,
                    const core::BlockPos& pos, int treeHeight, const TFTreeFeatureConfig& config);
    void buildBranch(levelgen::WorldGenLevel& level, std::vector<core::BlockPos>& leaves,
                     const core::BlockPos& pos, const Placers& placers, int height,
                     double length, double angle, double tilt,
                     levelgen::WorldgenRandom& random, const TFTreeFeatureConfig& config);
    void makeLeafBlob(levelgen::WorldGenLevel& level, const Placers& placers,
                      levelgen::WorldgenRandom& random, const core::BlockPos& leafPos,
                      const TFTreeFeatureConfig& config);
};

/**
 * MegaOakTreeFeature — trees/MegaOakTreeFeature.java
 * ("twilightforest:mega_oak"): a 2x2 oak trunk with 12-20 short branches,
 * each ending in a ragged 2.5 leaf spheroid, roots under each column.
 */
class MegaOakTreeFeature : public TFTreeFeature {
protected:
    bool generate(levelgen::WorldGenLevel& level, levelgen::WorldgenRandom& random,
                  const core::BlockPos& pos, const Placers& placers,
                  const TFTreeFeatureConfig& config) override;

private:
    void buildTrunk(levelgen::WorldGenLevel& level, std::vector<core::BlockPos>& leaves,
                    const Placers& placers, levelgen::WorldgenRandom& random,
                    const core::BlockPos& pos, int treeHeight, const TFTreeFeatureConfig& config);
    void buildBranch(levelgen::WorldGenLevel& level, std::vector<core::BlockPos>& leaves,
                     const core::BlockPos& pos, const Placers& placers, int height,
                     double length, double angle, double tilt,
                     levelgen::WorldgenRandom& random, const TFTreeFeatureConfig& config);
};

/**
 * LargeWinterTreeFeature — trees/LargeWinterTreeFeature.java
 * ("twilightforest:large_winter_tree"): a 35-54 block 2x2 spruce with
 * stacked even-width leaf circles and alternating pine branches.
 */
class LargeWinterTreeFeature : public TFTreeFeature {
public:
    explicit LargeWinterTreeFeature(BlockState* dirt = nullptr) : m_dirt(dirt) {}
    void setDirt(BlockState* dirt) { m_dirt = dirt; }

protected:
    bool generate(levelgen::WorldGenLevel& level, levelgen::WorldgenRandom& random,
                  const core::BlockPos& pos, const Placers& placers,
                  const TFTreeFeatureConfig& config) override;

private:
    void buildTrunk(levelgen::WorldGenLevel& level, const Placers& placers,
                    levelgen::WorldgenRandom& random, const core::BlockPos& pos,
                    int treeHeight, const TFTreeFeatureConfig& config);
    void makeLeaves(levelgen::WorldGenLevel& level, const Placers& placers,
                    levelgen::WorldgenRandom& random, const core::BlockPos& pos,
                    int treeHeight, const TFTreeFeatureConfig& config);
    void makePineBranches(levelgen::WorldGenLevel& level, const Placers& placers,
                          levelgen::WorldgenRandom& random, const core::BlockPos& pos,
                          int radius, const TFTreeFeatureConfig& config);

    BlockState* m_dirt;
};

/**
 * HollowStumpFeature — trees/HollowStumpFeature.java (with the parts of
 * HollowTreeFeature it uses: buildBranchRing / makeRoot)
 * ("twilightforest:hollow_stump"): a short hollow trunk ring on a suitable
 * flat area, with exposed roots wrapping down into the ground.
 */
class HollowStumpFeature : public TFTreeFeature {
protected:
    bool generate(levelgen::WorldGenLevel& level, levelgen::WorldgenRandom& random,
                  const core::BlockPos& pos, const Placers& placers,
                  const TFTreeFeatureConfig& config) override;

private:
    void buildSmallTrunk(levelgen::WorldGenLevel& level, const Placers& placers,
                         levelgen::WorldgenRandom& random, const core::BlockPos& pos,
                         int diameter, int maxHeight, const TFTreeFeatureConfig& config);
    // HollowTreeFeature.buildBranchRing for size 3 (roots, not leafy) — the
    // only size HollowStumpFeature passes; the other sizes grow the hollow
    // tree's crown.
    static void buildRootRing(levelgen::WorldGenLevel& level, levelgen::WorldgenRandom& random,
                              const core::BlockPos& pos, int radius, int branchHeight,
                              int heightVar, int length, double tilt, int minBranches,
                              int maxBranches, const TFTreeFeatureConfig& config);
    static void makeRoot(levelgen::WorldGenLevel& level, levelgen::WorldgenRandom& random,
                         const core::BlockPos& pos, int diameter, int branchHeight,
                         double length, double angle, double tilt,
                         const TFTreeFeatureConfig& config);
};

// ============================================================================
// Logs — world/components/feature/{FallenHollowLog,SmallFallenLog}Feature.java
// ============================================================================

/**
 * The blocks FallenHollowLogFeature keeps as final fields, resolved once at
 * bootstrap (the feature's JSON config is empty).
 */
struct FallenHollowLogConfig {
    BlockState* mossPatch = nullptr;
    BlockState* oakLeaves = nullptr;        // twilight_oak_leaves, persistent=true
    BlockState* oakLogWithZAxis = nullptr;  // twilight_oak_log, axis=z
    BlockState* oakLogWithXAxis = nullptr;  // twilight_oak_log, axis=x
    BlockState* grass = nullptr;            // minecraft:grass_block
    BlockState* firefly = nullptr;          // twilightforest:firefly (default: facing up)
};

/**
 * FallenHollowLogFeature — feature/FallenHollowLogFeature.java
 * ("twilightforest:fallen_hollow_log"): a 4x4 hollow twilight-oak log lying
 * along +X or +Z with jagged ends, a mossy floor, leaves and a firefly.
 */
class FallenHollowLogFeature : public levelgen::Feature<FallenHollowLogConfig> {
public:
    bool place(levelgen::FeaturePlaceContext<FallenHollowLogConfig>& context) override;

private:
    bool makeLog4Z(levelgen::WorldGenLevel& level, levelgen::WorldgenRandom& random,
                   const core::BlockPos& pos, const FallenHollowLogConfig& config);
    bool makeLog4X(levelgen::WorldGenLevel& level, levelgen::WorldgenRandom& random,
                   const core::BlockPos& pos, const FallenHollowLogConfig& config);
};

/**
 * HollowLogConfig — feature/config/HollowLogConfig.java: the normal log and
 * the hollow horizontal log. Both are pre-resolved per axis (and the hollow
 * one per HollowLogVariants.Horizontal) at bootstrap, so a stand-in keeps
 * what it can of axis/variant. A null hollow is the mod's "air" hollow.
 */
struct HollowLogConfig {
    enum Variant { EMPTY = 0, MOSS = 1, MOSS_AND_GRASS = 2, SNOW = 3, WATERLOGGED = 4 };

    BlockState* normalX = nullptr;
    BlockState* normalZ = nullptr;
    std::array<BlockState*, 5> hollowX{};   // indexed by Variant; null = no hollow log
    std::array<BlockState*, 5> hollowZ{};
    BlockState* mossPatch = nullptr;
    BlockState* seagrass = nullptr;
    BlockState* air = nullptr;

    bool hasHollow() const { return hollowX[EMPTY] != nullptr && hollowZ[EMPTY] != nullptr; }
};

/**
 * SmallFallenLogFeature — feature/SmallFallenLogFeature.java
 * ("twilightforest:fallen_small_log"): a 3-6 block log (sometimes hollow,
 * sometimes floating), moss or seagrass on top and an optional stub branch.
 */
class SmallFallenLogFeature : public levelgen::Feature<HollowLogConfig> {
public:
    bool place(levelgen::FeaturePlaceContext<HollowLogConfig>& context) override;
};

// ============================================================================
// Roots — WoodRootFeature, UndergroundPlantFeature
// ============================================================================

/** RootConfig — feature/config/RootConfig.java (root_block, root_ore). */
struct RootConfig {
    std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> blockRoot;
    std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> oreRoot;
};

/**
 * WoodRootFeature — feature/WoodRootFeature.java ("twilightforest:wood_roots"):
 * a root traced through stone between two stone endpoints, splitting in half
 * or growing a small ore ball when long enough.
 */
class WoodRootFeature : public levelgen::Feature<RootConfig> {
public:
    bool place(levelgen::FeaturePlaceContext<RootConfig>& context) override;

private:
    bool drawRoot(levelgen::WorldGenLevel& level, levelgen::WorldgenRandom& random,
                  const core::BlockPos& oPos, const core::BlockPos& pos, float length,
                  float angle, float tilt, const RootConfig& config);
    static bool placeRootBlock(levelgen::WorldGenLevel& level, const core::BlockPos& pos,
                               const levelgen::feature::stateproviders::BlockStateProvider& state,
                               levelgen::WorldgenRandom& random);
};

/**
 * The hanging plant a TF underground_plants / troll_vines feature places,
 * with the canSurvive of the mod's block class applied by name (a stand-in
 * block would carry the wrong rule, e.g. a bush's look-down check).
 */
struct UndergroundPlantConfig {
    enum class Survival {
        VANILLA,      // the block's own canSurvive (minecraft:hanging_roots, ...)
        ROOT_STRAND,  // RootStrandBlock: TFPlantBlock.canPlaceRootAt or root strand above
        TROLL_ROOT,   // TrollRootBlock / UnripeTorchClusterBlock: canPlaceRootBelow(above)
    };

    BlockState* state = nullptr;
    Survival survival = Survival::VANILLA;
    // state.is(TROLLVIDR): 1 in 10 becomes an unripe trollber (the draw is
    // made whenever the configured block is trollvidr, stand-in or not).
    bool isTrollvidr = false;
    BlockState* unripeTrollber = nullptr;
    int maxCount = INT_MAX;
    bool spawnInStructure = false;
};

/**
 * UndergroundPlantFeature — feature/UndergroundPlantFeature.java
 * ("twilightforest:underground_plants", and "twilightforest:troll_vines" =
 * the same class with spawnInStructure). Walks the column down from the
 * origin, jittering sideways on solid blocks (or 1 in 6) and placing the
 * plant wherever it survives.
 */
class TwilightUndergroundPlantFeature : public levelgen::Feature<UndergroundPlantConfig> {
public:
    bool place(levelgen::FeaturePlaceContext<UndergroundPlantConfig>& context) override;

private:
    static bool canSurvive(const UndergroundPlantConfig& config, BlockState* state,
                           levelgen::WorldGenLevel& level, const core::BlockPos& pos);
};

// ============================================================================
// Patches and snow — CheckAbovePatchFeature, SnowUnderTreeFeature
// ============================================================================

/**
 * DiskConfiguration as 26.1 declares it (state_provider is a plain
 * BlockStateProvider there, not the 1.20 rule-based provider the library's
 * DiskConfiguration carries).
 */
struct CheckAbovePatchConfig {
    std::shared_ptr<levelgen::feature::stateproviders::BlockStateProvider> stateProvider;
    std::shared_ptr<levelgen::blockpredicates::BlockPredicate> target;
    std::shared_ptr<util::IntProvider> radius;
    int halfHeight = 0;
};

/**
 * CheckAbovePatchFeature — feature/CheckAbovePatchFeature.java
 * ("twilightforest:mycelium_blob"): the mod's [VanillaCopy] of the disk
 * feature that only converts a target block whose block above can be
 * replaced, and marks every converted block for post-processing.
 */
class CheckAbovePatchFeature : public levelgen::Feature<CheckAbovePatchConfig> {
public:
    bool place(levelgen::FeaturePlaceContext<CheckAbovePatchConfig>& context) override;

private:
    bool placeColumn(const CheckAbovePatchConfig& config, levelgen::WorldGenLevel* level,
                     levelgen::WorldgenRandom& random, int start, int end, int x, int z);
};

/** The snow layer SnowUnderTreeFeature places (Blocks.SNOW). */
struct SnowUnderTreeConfig {
    BlockState* snow = nullptr;
};

/**
 * SnowUnderTreeFeature — trees/SnowUnderTreeFeature.java
 * ("twilightforest:snow_under_trees"): for each column of the 16x16 from the
 * origin whose top motion-blocking block is leaves, a snow layer on the
 * first sturdy ground below the canopy (and snowy=true on that ground).
 */
class SnowUnderTreeFeature : public levelgen::Feature<SnowUnderTreeConfig> {
public:
    bool place(levelgen::FeaturePlaceContext<SnowUnderTreeConfig>& context) override;
};

// ============================================================================
// Registry
// ============================================================================

/**
 * TwilightTreeFeatures — registers the configured features above under
 * their JSON ids. Blocks resolve through TwilightBlocks once; a feature
 * whose blocks cannot be resolved at all is not registered (logged once).
 * Idempotent; bootstraps TwilightFeatures first (the snowy forest selector
 * reuses its spruces).
 */
class TwilightTreeFeatures {
public:
    static void bootstrap();
    static bool isInitialized();
};

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft

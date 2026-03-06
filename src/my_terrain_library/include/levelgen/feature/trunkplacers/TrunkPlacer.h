#pragma once

#include "core/BlockPos.h"
#include "core/Direction.h"
#include "world/level/block/state/BlockState.h"
#include "levelgen/WorldgenRandom.h"
#include "levelgen/feature/foliageplacers/FoliagePlacer.h"
#include "levelgen/carver/CarverConfiguration.h"
#include <vector>
#include <memory>
#include <functional>

// Reference: net/minecraft/world/level/levelgen/feature/trunkplacers/TrunkPlacer.java
// Reference: net/minecraft/world/level/levelgen/feature/trunkplacers/StraightTrunkPlacer.java
// Reference: net/minecraft/world/level/levelgen/feature/trunkplacers/ForkingTrunkPlacer.java
// Reference: net/minecraft/world/level/levelgen/feature/trunkplacers/GiantTrunkPlacer.java
// Reference: net/minecraft/world/level/levelgen/feature/trunkplacers/MegaJungleTrunkPlacer.java
// Reference: net/minecraft/world/level/levelgen/feature/trunkplacers/DarkOakTrunkPlacer.java
// Reference: net/minecraft/world/level/levelgen/feature/trunkplacers/FancyTrunkPlacer.java
// Reference: net/minecraft/world/level/levelgen/feature/trunkplacers/BendingTrunkPlacer.java

namespace minecraft {

// Forward declarations
namespace levelgen::feature::stateproviders { class BlockStateProvider; }

namespace levelgen {
namespace feature {
namespace trunkplacers {

// Forward declaration
class TreeConfiguration;

/**
 * TrunkSetter - Function to set trunk blocks
 * Reference: BiConsumer<BlockPos, BlockState> in Java
 */
using TrunkSetter = std::function<void(const core::BlockPos&, BlockState*)>;

/**
 * LevelReader - Interface for reading level state
 * Reference: LevelSimulatedReader.java
 */
class LevelReader {
public:
    virtual ~LevelReader() = default;
    virtual bool isStateAtPosition(const core::BlockPos& pos, std::function<bool(BlockState*)> predicate) const = 0;
    virtual BlockState* getBlockState(const core::BlockPos& pos) const = 0;
};

/**
 * TrunkPlacer - Abstract base class for trunk placement
 * Reference: TrunkPlacer.java
 */
class TrunkPlacer {
protected:
    static constexpr int MAX_BASE_HEIGHT = 32;
    static constexpr int MAX_RAND = 24;

    int m_baseHeight;
    int m_heightRandA;
    int m_heightRandB;

public:
    static constexpr int MAX_HEIGHT = 80;

    TrunkPlacer(int baseHeight, int heightRandA, int heightRandB)
        : m_baseHeight(baseHeight)
        , m_heightRandA(heightRandA)
        , m_heightRandB(heightRandB)
    {}

    virtual ~TrunkPlacer() = default;

    /**
     * Place trunk and return foliage attachment points
     * Reference: TrunkPlacer.java line 42
     */
    virtual std::vector<foliageplacers::FoliageAttachment> placeTrunk(
        LevelReader& level,
        TrunkSetter trunkSetter,
        WorldgenRandom& random,
        int treeHeight,
        const core::BlockPos& origin,
        std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider,
        std::shared_ptr<stateproviders::BlockStateProvider> dirtProvider,
        bool forceDirt
    ) = 0;

    /**
     * Get tree height
     * Reference: TrunkPlacer.java lines 44-46
     */
    int getTreeHeight(WorldgenRandom& random) const {
        return m_baseHeight + random.nextInt(m_heightRandA + 1) + random.nextInt(m_heightRandB + 1);
    }

protected:
    /**
     * Place a log block
     * Reference: TrunkPlacer.java lines 59-70
     */
    bool placeLog(
        LevelReader& level,
        TrunkSetter trunkSetter,
        WorldgenRandom& random,
        const core::BlockPos& pos,
        std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider
    );

    /**
     * Check if position is valid for tree
     * Reference: TrunkPlacer.java lines 79-81
     */
    bool validTreePos(LevelReader& level, const core::BlockPos& pos) const;

    /**
     * Set dirt below trunk
     * Reference: TrunkPlacer.java lines 52-57
     */
    static void setDirtAt(
        LevelReader& level,
        TrunkSetter trunkSetter,
        WorldgenRandom& random,
        const core::BlockPos& pos,
        std::shared_ptr<stateproviders::BlockStateProvider> dirtProvider,
        bool forceDirt
    );
};

/**
 * StraightTrunkPlacer - Simple straight trunk
 * Reference: StraightTrunkPlacer.java
 */
class StraightTrunkPlacer : public TrunkPlacer {
public:
    StraightTrunkPlacer(int baseHeight, int heightRandA, int heightRandB)
        : TrunkPlacer(baseHeight, heightRandA, heightRandB)
    {}

    std::vector<foliageplacers::FoliageAttachment> placeTrunk(
        LevelReader& level,
        TrunkSetter trunkSetter,
        WorldgenRandom& random,
        int treeHeight,
        const core::BlockPos& origin,
        std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider,
        std::shared_ptr<stateproviders::BlockStateProvider> dirtProvider,
        bool forceDirt
    ) override;
};

/**
 * ForkingTrunkPlacer - Trunk that forks into branches (acacia)
 * Reference: ForkingTrunkPlacer.java
 */
class ForkingTrunkPlacer : public TrunkPlacer {
public:
    ForkingTrunkPlacer(int baseHeight, int heightRandA, int heightRandB)
        : TrunkPlacer(baseHeight, heightRandA, heightRandB)
    {}

    std::vector<foliageplacers::FoliageAttachment> placeTrunk(
        LevelReader& level,
        TrunkSetter trunkSetter,
        WorldgenRandom& random,
        int treeHeight,
        const core::BlockPos& origin,
        std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider,
        std::shared_ptr<stateproviders::BlockStateProvider> dirtProvider,
        bool forceDirt
    ) override;
};

/**
 * GiantTrunkPlacer - 2x2 trunk (mega spruce, mega jungle)
 * Reference: GiantTrunkPlacer.java
 */
class GiantTrunkPlacer : public TrunkPlacer {
public:
    GiantTrunkPlacer(int baseHeight, int heightRandA, int heightRandB)
        : TrunkPlacer(baseHeight, heightRandA, heightRandB)
    {}

    std::vector<foliageplacers::FoliageAttachment> placeTrunk(
        LevelReader& level,
        TrunkSetter trunkSetter,
        WorldgenRandom& random,
        int treeHeight,
        const core::BlockPos& origin,
        std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider,
        std::shared_ptr<stateproviders::BlockStateProvider> dirtProvider,
        bool forceDirt
    ) override;

protected:
    void placeLogIfFreeWithOffset(
        LevelReader& level,
        TrunkSetter trunkSetter,
        WorldgenRandom& random,
        std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider,
        const core::BlockPos& treePos,
        int x, int y, int z
    );

    bool isFree(LevelReader& level, const core::BlockPos& pos) const;
};

/**
 * MegaJungleTrunkPlacer - Giant jungle tree with vines
 * Reference: MegaJungleTrunkPlacer.java
 */
class MegaJungleTrunkPlacer : public GiantTrunkPlacer {
public:
    MegaJungleTrunkPlacer(int baseHeight, int heightRandA, int heightRandB)
        : GiantTrunkPlacer(baseHeight, heightRandA, heightRandB)
    {}

    std::vector<foliageplacers::FoliageAttachment> placeTrunk(
        LevelReader& level,
        TrunkSetter trunkSetter,
        WorldgenRandom& random,
        int treeHeight,
        const core::BlockPos& origin,
        std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider,
        std::shared_ptr<stateproviders::BlockStateProvider> dirtProvider,
        bool forceDirt
    ) override;
};

/**
 * DarkOakTrunkPlacer - 2x2 dark oak trunk
 * Reference: DarkOakTrunkPlacer.java
 */
class DarkOakTrunkPlacer : public TrunkPlacer {
public:
    DarkOakTrunkPlacer(int baseHeight, int heightRandA, int heightRandB)
        : TrunkPlacer(baseHeight, heightRandA, heightRandB)
    {}

    std::vector<foliageplacers::FoliageAttachment> placeTrunk(
        LevelReader& level,
        TrunkSetter trunkSetter,
        WorldgenRandom& random,
        int treeHeight,
        const core::BlockPos& origin,
        std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider,
        std::shared_ptr<stateproviders::BlockStateProvider> dirtProvider,
        bool forceDirt
    ) override;
};

/**
 * FancyTrunkPlacer - Branching oak tree trunk
 * Reference: FancyTrunkPlacer.java
 */
class FancyTrunkPlacer : public TrunkPlacer {
public:
    /**
     * FoliageCoords - Helper struct for tracking foliage positions
     * Reference: FancyTrunkPlacer.FoliageCoords
     */
    struct FoliageCoords {
        foliageplacers::FoliageAttachment attachment;
        int branchBase;

        FoliageCoords(const core::BlockPos& pos, int base)
            : attachment(pos, 0, false)
            , branchBase(base)
        {}
    };

    FancyTrunkPlacer(int baseHeight, int heightRandA, int heightRandB)
        : TrunkPlacer(baseHeight, heightRandA, heightRandB)
    {}

    std::vector<foliageplacers::FoliageAttachment> placeTrunk(
        LevelReader& level,
        TrunkSetter trunkSetter,
        WorldgenRandom& random,
        int treeHeight,
        const core::BlockPos& origin,
        std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider,
        std::shared_ptr<stateproviders::BlockStateProvider> dirtProvider,
        bool forceDirt
    ) override;

private:
    bool makeLimb(
        LevelReader& level,
        TrunkSetter trunkSetter,
        WorldgenRandom& random,
        const core::BlockPos& startPos,
        const core::BlockPos& endPos,
        bool doPlace,
        std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider
    );

    int getSteps(const core::BlockPos& pos) const;
    core::Axis getLogAxis(const core::BlockPos& startPos, const core::BlockPos& blockPos) const;
    bool trimBranches(int height, int localY) const;
    bool isFree(LevelReader& level, const core::BlockPos& pos) const;

    void makeBranches(
        LevelReader& level,
        TrunkSetter trunkSetter,
        WorldgenRandom& random,
        int height,
        const core::BlockPos& origin,
        const std::vector<FoliageCoords>& foliageCoords,
        std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider
    );
};

/**
 * BendingTrunkPlacer - Trunk that bends to the side (mangrove)
 * Reference: BendingTrunkPlacer.java
 */
class BendingTrunkPlacer : public TrunkPlacer {
private:
    int m_minHeightForLeaves;
    std::shared_ptr<carver::IntProvider> m_bendLength;

public:
    BendingTrunkPlacer(
        int baseHeight,
        int heightRandA,
        int heightRandB,
        int minHeightForLeaves,
        std::shared_ptr<carver::IntProvider> bendLength
    )
        : TrunkPlacer(baseHeight, heightRandA, heightRandB)
        , m_minHeightForLeaves(minHeightForLeaves)
        , m_bendLength(bendLength)
    {}

    std::vector<foliageplacers::FoliageAttachment> placeTrunk(
        LevelReader& level,
        TrunkSetter trunkSetter,
        WorldgenRandom& random,
        int treeHeight,
        const core::BlockPos& origin,
        std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider,
        std::shared_ptr<stateproviders::BlockStateProvider> dirtProvider,
        bool forceDirt
    ) override;
};

/**
 * UpwardsBranchingTrunkPlacer - Cherry tree branching trunk
 * Reference: UpwardsBranchingTrunkPlacer.java
 */
class UpwardsBranchingTrunkPlacer : public TrunkPlacer {
private:
    std::shared_ptr<carver::IntProvider> m_extraBranchSteps;
    float m_placeBranchPerLogProbability;
    std::shared_ptr<carver::IntProvider> m_extraBranchLength;

public:
    UpwardsBranchingTrunkPlacer(
        int baseHeight,
        int heightRandA,
        int heightRandB,
        std::shared_ptr<carver::IntProvider> extraBranchSteps,
        float placeBranchPerLogProbability,
        std::shared_ptr<carver::IntProvider> extraBranchLength
    )
        : TrunkPlacer(baseHeight, heightRandA, heightRandB)
        , m_extraBranchSteps(extraBranchSteps)
        , m_placeBranchPerLogProbability(placeBranchPerLogProbability)
        , m_extraBranchLength(extraBranchLength)
    {}

    std::vector<foliageplacers::FoliageAttachment> placeTrunk(
        LevelReader& level,
        TrunkSetter trunkSetter,
        WorldgenRandom& random,
        int treeHeight,
        const core::BlockPos& origin,
        std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider,
        std::shared_ptr<stateproviders::BlockStateProvider> dirtProvider,
        bool forceDirt
    ) override;

private:
    void placeBranch(
        LevelReader& level,
        TrunkSetter trunkSetter,
        WorldgenRandom& random,
        int treeHeight,
        std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider,
        std::vector<foliageplacers::FoliageAttachment>& attachments,
        const core::BlockPos& logPos,
        int currentHeight,
        core::Direction branchDir,
        int branchPos,
        int branchSteps
    );
};

/**
 * CherryTrunkPlacer - Cherry tree trunk with branching arms
 * Reference: CherryTrunkPlacer.java
 */
class CherryTrunkPlacer : public TrunkPlacer {
private:
    std::shared_ptr<carver::IntProvider> m_branchCount;
    std::shared_ptr<carver::IntProvider> m_branchHorizontalLength;
    int m_branchStartOffsetFromTopMin;
    int m_branchStartOffsetFromTopMax;
    std::shared_ptr<carver::IntProvider> m_branchEndOffsetFromTop;

public:
    /**
     * Constructor
     * Reference: CherryTrunkPlacer.java lines 31-38
     */
    CherryTrunkPlacer(
        int baseHeight,
        int heightRandA,
        int heightRandB,
        std::shared_ptr<carver::IntProvider> branchCount,
        std::shared_ptr<carver::IntProvider> branchHorizontalLength,
        int branchStartOffsetFromTopMin,
        int branchStartOffsetFromTopMax,
        std::shared_ptr<carver::IntProvider> branchEndOffsetFromTop
    )
        : TrunkPlacer(baseHeight, heightRandA, heightRandB)
        , m_branchCount(branchCount)
        , m_branchHorizontalLength(branchHorizontalLength)
        , m_branchStartOffsetFromTopMin(branchStartOffsetFromTopMin)
        , m_branchStartOffsetFromTopMax(branchStartOffsetFromTopMax)
        , m_branchEndOffsetFromTop(branchEndOffsetFromTop)
    {}

    std::vector<foliageplacers::FoliageAttachment> placeTrunk(
        LevelReader& level,
        TrunkSetter trunkSetter,
        WorldgenRandom& random,
        int treeHeight,
        const core::BlockPos& origin,
        std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider,
        std::shared_ptr<stateproviders::BlockStateProvider> dirtProvider,
        bool forceDirt
    ) override;

private:
    /**
     * Generate a branch from the trunk
     * Reference: CherryTrunkPlacer.java lines 84-109
     */
    foliageplacers::FoliageAttachment generateBranch(
        LevelReader& level,
        TrunkSetter trunkSetter,
        WorldgenRandom& random,
        int treeHeight,
        const core::BlockPos& origin,
        std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider,
        core::Direction branchDirection,
        int offsetFromOrigin,
        bool middleContinuesUpwards,
        core::BlockPos::MutableBlockPos& logPos
    );

    /**
     * Sample from uniform int range
     * Reference: UniformInt.sample()
     */
    int sampleUniform(WorldgenRandom& random, int min, int max) const {
        if (min >= max) return min;
        return min + random.nextInt(max - min + 1);
    }
};

} // namespace trunkplacers
} // namespace feature
} // namespace levelgen
} // namespace minecraft

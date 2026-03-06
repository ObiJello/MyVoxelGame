#pragma once

#include "core/BlockPos.h"
#include "core/Direction.h"
#include "world/level/block/state/BlockState.h"
#include "levelgen/WorldgenRandom.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include <vector>
#include <memory>
#include <functional>
#include <set>

// Reference: net/minecraft/world/level/levelgen/feature/treedecorators/TreeDecorator.java
// Reference: net/minecraft/world/level/levelgen/feature/treedecorators/LeaveVineDecorator.java
// Reference: net/minecraft/world/level/levelgen/feature/treedecorators/TrunkVineDecorator.java
// Reference: net/minecraft/world/level/levelgen/feature/treedecorators/CocoaDecorator.java
// Reference: net/minecraft/world/level/levelgen/feature/treedecorators/BeehiveDecorator.java
// Reference: net/minecraft/world/level/levelgen/feature/treedecorators/AlterGroundDecorator.java
// Reference: net/minecraft/world/level/levelgen/feature/treedecorators/AttachedToLeavesDecorator.java

namespace minecraft {
namespace levelgen {
namespace feature {
namespace treedecorators {

/**
 * TreeDecorator Context - Data passed to decorators
 * Reference: TreeDecorator.Context
 */
class DecoratorContext {
public:
    using BlockSetter = std::function<void(const core::BlockPos&, BlockState*)>;
    using BlockGetter = std::function<BlockState*(const core::BlockPos&)>;
    using HeightGetter = std::function<int(int, int)>;  // Returns heightmap Y at x, z

private:
    std::vector<core::BlockPos> m_logs;
    std::vector<core::BlockPos> m_leaves;
    std::vector<core::BlockPos> m_roots;
    BlockSetter m_blockSetter;
    BlockGetter m_blockGetter;
    HeightGetter m_heightGetter;
    WorldgenRandom* m_random;

public:
    DecoratorContext(
        const std::vector<core::BlockPos>& logs,
        const std::vector<core::BlockPos>& leaves,
        const std::vector<core::BlockPos>& roots,
        BlockSetter blockSetter,
        BlockGetter blockGetter,
        HeightGetter heightGetter,
        WorldgenRandom* random
    )
        : m_logs(logs)
        , m_leaves(leaves)
        , m_roots(roots)
        , m_blockSetter(blockSetter)
        , m_blockGetter(blockGetter)
        , m_heightGetter(heightGetter)
        , m_random(random)
    {}

    const std::vector<core::BlockPos>& logs() const { return m_logs; }
    const std::vector<core::BlockPos>& leaves() const { return m_leaves; }
    const std::vector<core::BlockPos>& roots() const { return m_roots; }

    WorldgenRandom& random() { return *m_random; }

    void setBlock(const core::BlockPos& pos, BlockState* blockState) {
        m_blockSetter(pos, blockState);
    }

    BlockState* getBlockState(const core::BlockPos& pos) const {
        return m_blockGetter(pos);
    }

    bool isAir(const core::BlockPos& pos) const {
        BlockState* state = m_blockGetter(pos);
        return state && state->isAir();
    }

    /**
     * Get heightmap Y for MOTION_BLOCKING_NO_LEAVES at (x, z)
     * Reference: PlaceOnGroundDecorator.java line 68
     * Used to prevent placing leaf litter under tree canopies or in caves
     */
    int getHeightNoLeaves(int x, int z) const {
        return m_heightGetter(x, z);
    }
};

/**
 * TreeDecorator - Abstract base class for tree decorations
 * Reference: TreeDecorator.java
 */
class TreeDecorator {
public:
    virtual ~TreeDecorator() = default;

    /**
     * Place decorations on the tree
     * Reference: TreeDecorator.java line 22
     */
    virtual void place(DecoratorContext& context) = 0;
};

/**
 * LeaveVineDecorator - Places vines on leaves
 * Reference: LeaveVineDecorator.java
 */
class LeaveVineDecorator : public TreeDecorator {
private:
    float m_probability;

    void addHangingVine(const core::BlockPos& pos, core::Direction direction, DecoratorContext& context);
    void placeVine(DecoratorContext& context, const core::BlockPos& pos, core::Direction direction);

public:
    explicit LeaveVineDecorator(float probability)
        : m_probability(probability)
    {}

    void place(DecoratorContext& context) override;
};

/**
 * TrunkVineDecorator - Places vines on trunk
 * Reference: TrunkVineDecorator.java
 */
class TrunkVineDecorator : public TreeDecorator {
private:
    void placeVine(DecoratorContext& context, const core::BlockPos& pos, core::Direction direction);

public:
    void place(DecoratorContext& context) override;
};

/**
 * CocoaDecorator - Places cocoa beans on jungle trees
 * Reference: CocoaDecorator.java
 */
class CocoaDecorator : public TreeDecorator {
private:
    float m_probability;

public:
    explicit CocoaDecorator(float probability)
        : m_probability(probability)
    {}

    void place(DecoratorContext& context) override;
};

/**
 * BeehiveDecorator - Places beehives on trees
 * Reference: BeehiveDecorator.java
 */
class BeehiveDecorator : public TreeDecorator {
private:
    float m_probability;

public:
    explicit BeehiveDecorator(float probability)
        : m_probability(probability)
    {}

    void place(DecoratorContext& context) override;
};

/**
 * AlterGroundDecorator - Modifies ground around tree (mega spruce)
 * Reference: AlterGroundDecorator.java
 */
class AlterGroundDecorator : public TreeDecorator {
private:
    std::shared_ptr<feature::stateproviders::BlockStateProvider> m_provider;

    void placeCircle(DecoratorContext& context, const core::BlockPos& pos);
    void placeBlockAt(DecoratorContext& context, const core::BlockPos& pos);

public:
    explicit AlterGroundDecorator(std::shared_ptr<feature::stateproviders::BlockStateProvider> provider)
        : m_provider(provider)
    {}

    void place(DecoratorContext& context) override;
};

/**
 * AttachedToLeavesDecorator - Places blocks attached to leaves (cherry petals)
 * Reference: AttachedToLeavesDecorator.java
 */
class AttachedToLeavesDecorator : public TreeDecorator {
private:
    float m_probability;
    int m_exclusionRadiusXZ;
    int m_exclusionRadiusY;
    BlockState* m_blockState;
    int m_requiredEmptyBlocks;

    bool hasRequiredEmptyBlocks(DecoratorContext& context, const core::BlockPos& leafPos, core::Direction direction) const;

public:
    AttachedToLeavesDecorator(
        float probability,
        int exclusionRadiusXZ,
        int exclusionRadiusY,
        BlockState* blockState,
        int requiredEmptyBlocks
    )
        : m_probability(probability)
        , m_exclusionRadiusXZ(exclusionRadiusXZ)
        , m_exclusionRadiusY(exclusionRadiusY)
        , m_blockState(blockState)
        , m_requiredEmptyBlocks(requiredEmptyBlocks)
    {}

    void place(DecoratorContext& context) override;
};

/**
 * CreakingHeartDecorator - Places a creaking heart inside a tree trunk
 * Reference: CreakingHeartDecorator.java
 */
class CreakingHeartDecorator : public TreeDecorator {
private:
    float m_probability;

public:
    explicit CreakingHeartDecorator(float probability)
        : m_probability(probability)
    {}

    void place(DecoratorContext& context) override;
};

/**
 * PaleMossDecorator - Places pale hanging moss on tree
 * Reference: PaleMossDecorator.java
 */
class PaleMossDecorator : public TreeDecorator {
private:
    float m_leavesProbability;
    float m_trunkProbability;
    float m_groundProbability;

public:
    PaleMossDecorator(float leavesProbability, float trunkProbability, float groundProbability)
        : m_leavesProbability(leavesProbability)
        , m_trunkProbability(trunkProbability)
        , m_groundProbability(groundProbability)
    {}

    void place(DecoratorContext& context) override;

private:
    static void addMossHanger(const core::BlockPos& pos, DecoratorContext& context);
};

/**
 * PlaceOnGroundDecorator - Places blocks on ground around tree
 * Reference: PlaceOnGroundDecorator.java
 */
class PlaceOnGroundDecorator : public TreeDecorator {
private:
    int m_tries;
    int m_radius;
    int m_height;
    std::shared_ptr<feature::stateproviders::BlockStateProvider> m_blockStateProvider;

public:
    PlaceOnGroundDecorator(
        int tries,
        int radius,
        int height,
        std::shared_ptr<feature::stateproviders::BlockStateProvider> blockStateProvider
    )
        : m_tries(tries)
        , m_radius(radius)
        , m_height(height)
        , m_blockStateProvider(blockStateProvider)
    {}

    void place(DecoratorContext& context) override;

private:
    void attemptToPlaceBlockAbove(DecoratorContext& context, const core::BlockPos& pos);
};

/**
 * AttachedToLogsDecorator - Attaches blocks to log sides
 * Reference: AttachedToLogsDecorator.java
 */
class AttachedToLogsDecorator : public TreeDecorator {
private:
    float m_probability;
    std::shared_ptr<feature::stateproviders::BlockStateProvider> m_blockProvider;
    std::vector<core::Direction> m_directions;

public:
    AttachedToLogsDecorator(
        float probability,
        std::shared_ptr<feature::stateproviders::BlockStateProvider> blockProvider,
        const std::vector<core::Direction>& directions
    )
        : m_probability(probability)
        , m_blockProvider(blockProvider)
        , m_directions(directions)
    {}

    void place(DecoratorContext& context) override;
};

} // namespace treedecorators
} // namespace feature
} // namespace levelgen
} // namespace minecraft

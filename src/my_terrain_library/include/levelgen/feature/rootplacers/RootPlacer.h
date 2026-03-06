#pragma once

#include "core/BlockPos.h"
#include "core/Direction.h"
#include "world/level/block/state/BlockState.h"
#include "levelgen/WorldgenRandom.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "levelgen/carver/CarverConfiguration.h"
#include <vector>
#include <memory>
#include <functional>
#include <optional>

// Reference: net/minecraft/world/level/levelgen/feature/rootplacers/RootPlacer.java
// Reference: net/minecraft/world/level/levelgen/feature/rootplacers/MangroveRootPlacer.java
// Reference: net/minecraft/world/level/levelgen/feature/rootplacers/AboveRootPlacement.java

namespace minecraft {
namespace levelgen {
namespace feature {
namespace rootplacers {

/**
 * RootSetter - Function to set root blocks
 * Reference: BiConsumer<BlockPos, BlockState> in Java
 */
using RootSetter = std::function<void(const core::BlockPos&, BlockState*)>;

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
 * AboveRootPlacement - Configuration for above-ground root placement
 * Reference: AboveRootPlacement.java
 */
struct AboveRootPlacement {
    std::shared_ptr<stateproviders::BlockStateProvider> aboveRootProvider;
    float aboveRootPlacementChance;

    AboveRootPlacement(
        std::shared_ptr<stateproviders::BlockStateProvider> provider,
        float chance
    )
        : aboveRootProvider(provider)
        , aboveRootPlacementChance(chance)
    {}
};

/**
 * RootPlacer - Abstract base class for tree root placement
 * Reference: RootPlacer.java
 */
class RootPlacer {
protected:
    std::shared_ptr<carver::IntProvider> m_trunkOffsetY;
    std::shared_ptr<stateproviders::BlockStateProvider> m_rootProvider;
    std::optional<AboveRootPlacement> m_aboveRootPlacement;

public:
    RootPlacer(
        std::shared_ptr<carver::IntProvider> trunkOffsetY,
        std::shared_ptr<stateproviders::BlockStateProvider> rootProvider,
        std::optional<AboveRootPlacement> aboveRootPlacement = std::nullopt
    )
        : m_trunkOffsetY(trunkOffsetY)
        , m_rootProvider(rootProvider)
        , m_aboveRootPlacement(aboveRootPlacement)
    {}

    virtual ~RootPlacer() = default;

    /**
     * Place roots for the tree
     * Reference: RootPlacer.java placeRoots()
     *
     * @return true if roots were successfully placed
     */
    virtual bool placeRoots(
        LevelReader& level,
        RootSetter rootSetter,
        WorldgenRandom& random,
        const core::BlockPos& origin,
        const core::BlockPos& trunkOrigin,
        std::vector<core::BlockPos>& rootPositions
    ) = 0;

    /**
     * Get trunk offset Y
     * Reference: RootPlacer.java getTrunkOrigin()
     */
    core::BlockPos getTrunkOrigin(const core::BlockPos& origin, WorldgenRandom& random) const {
        return origin.above(m_trunkOffsetY->sample(random));
    }

protected:
    /**
     * Place a root block
     * Reference: RootPlacer.java placeRoot()
     */
    void placeRoot(
        LevelReader& level,
        RootSetter rootSetter,
        WorldgenRandom& random,
        const core::BlockPos& pos,
        std::vector<core::BlockPos>& rootPositions
    );

    /**
     * Check if position can have root
     * Reference: RootPlacer.java canPlaceRoot()
     */
    virtual bool canPlaceRoot(LevelReader& level, const core::BlockPos& pos) const;

    /**
     * Get potentially waterlogged state
     * Reference: RootPlacer.java getPotentiallyWaterloggedState()
     */
    BlockState* getPotentiallyWaterloggedState(
        LevelReader& level,
        const core::BlockPos& pos,
        BlockState* blockState
    ) const;
};

/**
 * MangroveRootPlacer - Mangrove tree root system
 * Reference: MangroveRootPlacer.java
 */
class MangroveRootPlacer : public RootPlacer {
private:
    int m_maxRootWidth;
    int m_maxRootLength;
    float m_randomSkewChance;

public:
    MangroveRootPlacer(
        std::shared_ptr<carver::IntProvider> trunkOffsetY,
        std::shared_ptr<stateproviders::BlockStateProvider> rootProvider,
        std::optional<AboveRootPlacement> aboveRootPlacement,
        int maxRootWidth,
        int maxRootLength,
        float randomSkewChance
    )
        : RootPlacer(trunkOffsetY, rootProvider, aboveRootPlacement)
        , m_maxRootWidth(maxRootWidth)
        , m_maxRootLength(maxRootLength)
        , m_randomSkewChance(randomSkewChance)
    {}

    bool placeRoots(
        LevelReader& level,
        RootSetter rootSetter,
        WorldgenRandom& random,
        const core::BlockPos& origin,
        const core::BlockPos& trunkOrigin,
        std::vector<core::BlockPos>& rootPositions
    ) override;

    bool canPlaceRoot(LevelReader& level, const core::BlockPos& pos) const override;

private:
    /**
     * Simulate root growth
     * Reference: MangroveRootPlacer.java simulateRoots()
     */
    bool simulateRoots(
        LevelReader& level,
        WorldgenRandom& random,
        const core::BlockPos& rootPos,
        core::Direction dir,
        const core::BlockPos& rootOrigin,
        std::vector<core::BlockPos>& positions,
        int layer
    );

    /**
     * Get potential root positions
     * Reference: MangroveRootPlacer.java potentialRootPositions()
     */
    std::vector<core::BlockPos> potentialRootPositions(
        const core::BlockPos& pos,
        core::Direction prevDir,
        WorldgenRandom& random,
        const core::BlockPos& rootOrigin
    ) const;

    /**
     * Place root at position (handles muddy roots)
     * Reference: MangroveRootPlacer.java placeRoot() override
     */
    void placeRootAtPos(
        LevelReader& level,
        RootSetter rootSetter,
        WorldgenRandom& random,
        const core::BlockPos& pos,
        std::vector<core::BlockPos>& rootPositions
    );

    /**
     * Place root column
     * Reference: MangroveRootPlacer.java placeRootColumn()
     */
    void placeRootColumn(
        LevelReader& level,
        RootSetter rootSetter,
        WorldgenRandom& random,
        const core::BlockPos& columnStart,
        const core::BlockPos& trunkOrigin,
        std::vector<core::BlockPos>& rootPositions
    );
};

} // namespace rootplacers
} // namespace feature
} // namespace levelgen
} // namespace minecraft

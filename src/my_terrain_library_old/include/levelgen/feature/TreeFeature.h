#pragma once

#include "levelgen/feature/Feature.h"
#include "levelgen/feature/configurations/TreeConfiguration.h"
#include "levelgen/feature/foliageplacers/FoliagePlacer.h"
#include "levelgen/feature/trunkplacers/TrunkPlacer.h"
#include "levelgen/feature/treedecorators/TreeDecorator.h"
#include "core/BlockPos.h"
#include "world/level/block/state/BlockState.h"
#include "random/XoroshiroRandomSource.h"
#include <set>
#include <vector>
#include <functional>
#include <optional>

// Reference: net/minecraft/world/level/levelgen/feature/TreeFeature.java

namespace minecraft {
namespace levelgen {
namespace feature {

/**
 * WorldGenLevel - Interface for world generation
 * Reference: WorldGenLevel.java
 */
class WorldGenLevel {
public:
    virtual ~WorldGenLevel() = default;

    virtual void setBlock(const core::BlockPos& pos, BlockState* blockState, int flags) = 0;
    virtual BlockState* getBlockState(const core::BlockPos& pos) const = 0;
    virtual bool isStateAtPosition(const core::BlockPos& pos,
        std::function<bool(BlockState*)> predicate) const = 0;
    virtual int getMinY() const = 0;
    virtual int getMaxY() const = 0;
    virtual bool isFluidAtPosition(const core::BlockPos& pos,
        std::function<bool(BlockState*)> predicate) const = 0;
};

/**
 * BoundingBox - Axis-aligned bounding box
 * Reference: BoundingBox.java
 */
class BoundingBox {
private:
    int m_minX, m_minY, m_minZ;
    int m_maxX, m_maxY, m_maxZ;

public:
    BoundingBox(int minX, int minY, int minZ, int maxX, int maxY, int maxZ)
        : m_minX(minX), m_minY(minY), m_minZ(minZ)
        , m_maxX(maxX), m_maxY(maxY), m_maxZ(maxZ)
    {}

    int minX() const { return m_minX; }
    int minY() const { return m_minY; }
    int minZ() const { return m_minZ; }
    int maxX() const { return m_maxX; }
    int maxY() const { return m_maxY; }
    int maxZ() const { return m_maxZ; }

    int getXSpan() const { return m_maxX - m_minX + 1; }
    int getYSpan() const { return m_maxY - m_minY + 1; }
    int getZSpan() const { return m_maxZ - m_minZ + 1; }

    bool isInside(const core::BlockPos& pos) const {
        return pos.getX() >= m_minX && pos.getX() <= m_maxX &&
               pos.getY() >= m_minY && pos.getY() <= m_maxY &&
               pos.getZ() >= m_minZ && pos.getZ() <= m_maxZ;
    }

    static std::optional<BoundingBox> encapsulatingPositions(const std::vector<core::BlockPos>& positions) {
        if (positions.empty()) {
            return std::nullopt;
        }
        int minX = positions[0].getX(), maxX = positions[0].getX();
        int minY = positions[0].getY(), maxY = positions[0].getY();
        int minZ = positions[0].getZ(), maxZ = positions[0].getZ();

        for (const auto& pos : positions) {
            minX = std::min(minX, pos.getX());
            maxX = std::max(maxX, pos.getX());
            minY = std::min(minY, pos.getY());
            maxY = std::max(maxY, pos.getY());
            minZ = std::min(minZ, pos.getZ());
            maxZ = std::max(maxZ, pos.getZ());
        }

        return BoundingBox(minX, minY, minZ, maxX, maxY, maxZ);
    }
};

/**
 * TreeFeature - Places trees in the world
 * Reference: TreeFeature.java
 */
class TreeFeature {
public:
    static constexpr int BLOCK_UPDATE_FLAGS = 19;

    /**
     * Check if position has vine
     * Reference: TreeFeature.java isVine() lines 42-44
     */
    static bool isVine(WorldGenLevel& level, const core::BlockPos& pos) {
        return level.isStateAtPosition(pos, [](BlockState* state) {
            return state && state->getIdentifier() == "minecraft:vine";
        });
    }

    /**
     * Check if position is air or leaves
     * Reference: TreeFeature.java isAirOrLeaves() lines 46-48
     */
    static bool isAirOrLeaves(WorldGenLevel& level, const core::BlockPos& pos) {
        return level.isStateAtPosition(pos, [](BlockState* state) {
            return state && (state->isAir() || state->isLeaves());
        });
    }

    /**
     * Check if position is valid for tree
     * Reference: TreeFeature.java validTreePos() lines 54-56
     */
    static bool validTreePos(WorldGenLevel& level, const core::BlockPos& pos) {
        return level.isStateAtPosition(pos, [](BlockState* state) {
            // TODO: Check REPLACEABLE_BY_TREES tag
            return state && state->isAir();
        });
    }

    /**
     * Place tree at origin
     * Reference: TreeFeature.java place() lines 108-158
     */
    bool place(
        WorldGenLevel& level,
        XoroshiroRandomSource& random,
        const core::BlockPos& origin,
        const configurations::TreeConfiguration& config
    );

private:
    /**
     * Internal placement logic
     * Reference: TreeFeature.java doPlace() lines 58-83
     */
    bool doPlace(
        WorldGenLevel& level,
        XoroshiroRandomSource& random,
        const core::BlockPos& origin,
        std::function<void(const core::BlockPos&, BlockState*)> rootSetter,
        std::function<void(const core::BlockPos&, BlockState*)> trunkSetter,
        foliageplacers::FoliageSetter& foliageSetter,
        const configurations::TreeConfiguration& config
    );

    /**
     * Get maximum free tree height
     * Reference: TreeFeature.java getMaxFreeTreeHeight() lines 85-102
     */
    int getMaxFreeTreeHeight(
        WorldGenLevel& level,
        int maxTreeHeight,
        const core::BlockPos& treePos,
        const configurations::TreeConfiguration& config
    );
};

/**
 * SimpleFoliageSetter - Simple implementation of FoliageSetter that tracks placed blocks
 */
class SimpleFoliageSetter : public foliageplacers::FoliageSetter {
private:
    WorldGenLevel& m_level;
    std::set<core::BlockPos>& m_foliagePositions;

public:
    SimpleFoliageSetter(WorldGenLevel& level, std::set<core::BlockPos>& foliagePositions)
        : m_level(level)
        , m_foliagePositions(foliagePositions)
    {}

    void set(const core::BlockPos& pos, BlockState* blockState) override {
        m_foliagePositions.insert(pos);
        m_level.setBlock(pos, blockState, TreeFeature::BLOCK_UPDATE_FLAGS);
    }

    bool isSet(const core::BlockPos& pos) const override {
        return m_foliagePositions.count(pos) > 0;
    }
};

} // namespace feature
} // namespace levelgen
} // namespace minecraft

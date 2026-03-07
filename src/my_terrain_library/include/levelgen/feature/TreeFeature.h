#pragma once

#include "levelgen/feature/Feature.h"
#include "levelgen/feature/configurations/TreeConfiguration.h"
#include "levelgen/feature/foliageplacers/FoliagePlacer.h"
#include "levelgen/feature/trunkplacers/TrunkPlacer.h"
#include "levelgen/feature/treedecorators/TreeDecorator.h"
#include "levelgen/feature/BlockChangeTrace.h"
#include "levelgen/WorldGenLevel.h"
#include "core/BlockPos.h"
#include "world/level/block/state/BlockState.h"
#include "levelgen/WorldgenRandom.h"
#include <set>
#include <vector>
#include <functional>
#include <optional>

// Reference: net/minecraft/world/level/levelgen/feature/TreeFeature.java

namespace minecraft {
namespace levelgen {
namespace feature {

// Use the WorldGenLevel from levelgen namespace
using ::minecraft::levelgen::WorldGenLevel;

/**
 * ChunkWorldGenLevel - Concrete WorldGenLevel implementation wrapping single IChunk
 * This is used when only single-chunk access is available.
 * For multi-chunk access, use WorldGenRegionLevel instead.
 */
class ChunkWorldGenLevel : public WorldGenLevel {
public:
    ::world::IChunk* m_chunk;  // Public for direct access in tree placement

private:
    int64_t m_seed = 0;

public:
    explicit ChunkWorldGenLevel(::world::IChunk* chunk) : m_chunk(chunk), m_seed(0) {}

    void setSeed(int64_t seed) { m_seed = seed; }

    bool setBlock(const core::BlockPos& pos, BlockState* state, int flags) override {
        if (m_chunk && state) {
            BlockState* oldState = nullptr;
            if (feature::BlockChangeTrace::isEnabled()) {
                oldState = m_chunk->getBlockState(pos.getX() & 15, pos.getY(), pos.getZ() & 15);
            }
            m_chunk->setBlockState(pos.getX() & 15, pos.getY(), pos.getZ() & 15, state, false);
            if (oldState) {
                feature::BlockChangeTrace::log(
                    pos.getX(),
                    pos.getY(),
                    pos.getZ(),
                    oldState->getIdentifier(),
                    state->getIdentifier()
                );
            }
            return true;
        }
        return false;
    }

    BlockState* getBlockState(const core::BlockPos& pos) const override {
        if (m_chunk) {
            return m_chunk->getBlockState(pos.getX() & 15, pos.getY(), pos.getZ() & 15);
        }
        return nullptr;
    }

    bool isStateAtPosition(const core::BlockPos& pos,
                          std::function<bool(BlockState*)> predicate) const override {
        if (m_chunk) {
            auto* blockType = m_chunk->getBlockState(pos.getX() & 15, pos.getY(), pos.getZ() & 15);
            return predicate(blockType);
        }
        return predicate(nullptr);
    }

    bool isFluidAtPosition(const core::BlockPos& pos,
                          std::function<bool(BlockState*)> predicate) const override {
        BlockState* state = getBlockState(pos);
        return state && state->isFluid() && predicate(state);
    }

    ::world::IChunk* getChunk(int chunkX, int chunkZ) override {
        // Single-chunk implementation - only return if it's our chunk
        if (m_chunk && m_chunk->getPos().x() == chunkX && m_chunk->getPos().z() == chunkZ) {
            return m_chunk;
        }
        return nullptr;
    }

    int getHeight(Heightmap::Types type, int x, int z) const override {
        if (m_chunk) {
            // Add +1 to match Java's WorldGenRegion.getHeight() behavior
            // Reference: WorldGenRegion.java line 367 - returns air Y, not ground Y
            return m_chunk->getHeight(static_cast<int>(type), x & 15, z & 15) + 1;
        }
        return 0;
    }

    int getMinY() const override {
        return m_chunk ? m_chunk->getMinBuildHeight() : -64;
    }

    int getMaxY() const override {
        return m_chunk ? m_chunk->getMaxBuildHeight() : 320;
    }

    const world::biome::Biome* getBiome(const core::BlockPos& pos) const override {
        if (m_chunk) {
            return m_chunk->getBiome(pos);
        }
        return nullptr;
    }

    int64_t getSeed() const override {
        return m_seed;
    }

    // Override ensureCanWrite to check chunk bounds
    // Single-chunk implementation can only write to its own chunk
    bool ensureCanWrite(const core::BlockPos& pos) const override {
        if (!m_chunk) return false;
        int chunkX = pos.getX() >> 4;
        int chunkZ = pos.getZ() >> 4;
        return (chunkX == m_chunk->getPos().x() && chunkZ == m_chunk->getPos().z());
    }
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
            // Reference: TreeFeature.java validTreePos() -> isAir || REPLACEABLE_BY_TREES
            return state && (state->isAir() || state->isReplaceableByTrees());
        });
    }

    /**
     * Place tree using FeaturePlaceContext
     * Reference: Feature.java place() - compatible with ConfiguredFeatureImpl
     */
    bool place(FeaturePlaceContext<configurations::TreeConfiguration>& context) {
        // Use WorldGenLevel directly from context (supports multi-chunk access)
        WorldGenLevel* level = context.level();
        if (!level) {
            return false;
        }
        return place(*level, context.random(), context.origin(), context.config());
    }

    /**
     * Place tree at origin
     * Reference: TreeFeature.java place() lines 108-158
     */
    bool place(
        WorldGenLevel& level,
        WorldgenRandom& random,
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
        WorldgenRandom& random,
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

    bool canPlace(const core::BlockPos& pos) const override {
        // Reference: FoliagePlacer.java tryPlaceLeaf() -> TreeFeature.validTreePos()
        BlockState* state = m_level.getBlockState(pos);
        return state && (state->isAir() || state->isReplaceableByTrees());
    }
};

} // namespace feature
} // namespace levelgen
} // namespace minecraft

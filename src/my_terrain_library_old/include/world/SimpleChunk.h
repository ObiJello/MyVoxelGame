#pragma once

#include "IChunk.h"
#include "MinecraftBlockType.h"
#include <vector>
#include <unordered_set>

namespace world {

/**
 * Simple in-memory implementation of IChunk for testing.
 * Stores blocks in a flat array.
 */
class SimpleChunk : public IChunk {
private:
    ChunkPos m_pos;
    int m_minY;
    int m_maxY;
    int m_height;
    std::vector<IBlockType*> m_blocks;  // Flat array: index = (y - minY) * 256 + z * 16 + x
    std::unordered_set<uint64_t> m_postProcessing;  // Positions marked for post-processing

    // Convert (x, y, z) to array index
    int getIndex(int x, int y, int z) const {
        return (y - m_minY) * 256 + z * 16 + x;
    }

    // Pack position into uint64_t for post-processing set
    static uint64_t packPos(int x, int y, int z) {
        return ((uint64_t)(x & 0xFFFF) << 48) |
               ((uint64_t)(y & 0xFFFF) << 32) |
               ((uint64_t)(z & 0xFFFF) << 16);
    }

public:
    SimpleChunk(ChunkPos pos, int minY, int maxY)
        : m_pos(pos), m_minY(minY), m_maxY(maxY)
    {
        m_height = maxY - minY;
        m_blocks.resize(16 * m_height * 16, MinecraftBlocks::AIR());
    }

    ChunkPos getPos() const override {
        return m_pos;
    }

    int getMinBuildHeight() const override {
        return m_minY;
    }

    int getMaxBuildHeight() const override {
        return m_maxY;
    }

    IBlockType* getBlockState(const BlockPos& pos) override {
        int localX = pos.getX() & 15;  // pos.getX() % 16
        int localZ = pos.getZ() & 15;  // pos.getZ() % 16
        return getBlockState(localX, pos.getY(), localZ);
    }

    IBlockType* getBlockState(int localX, int y, int localZ) override {
        if (localX < 0 || localX >= 16 || localZ < 0 || localZ >= 16) {
            return MinecraftBlocks::AIR();
        }
        if (y < m_minY || y >= m_maxY) {
            return MinecraftBlocks::AIR();
        }

        int index = getIndex(localX, y, localZ);
        return m_blocks[index];
    }

    IBlockType* setBlockState(const BlockPos& pos, IBlockType* state, bool moved) override {
        int localX = pos.getX() & 15;
        int localZ = pos.getZ() & 15;
        return setBlockState(localX, pos.getY(), localZ, state, moved);
    }

    IBlockType* setBlockState(int localX, int y, int localZ, IBlockType* state, bool moved) override {
        if (localX < 0 || localX >= 16 || localZ < 0 || localZ >= 16) {
            return MinecraftBlocks::AIR();
        }
        if (y < m_minY || y >= m_maxY) {
            return MinecraftBlocks::AIR();
        }

        int index = getIndex(localX, y, localZ);
        IBlockType* oldState = m_blocks[index];
        m_blocks[index] = state;
        return oldState;
    }

    void markPosForPostprocessing(const BlockPos& pos) override {
        m_postProcessing.insert(packPos(pos.getX(), pos.getY(), pos.getZ()));
    }

    // Helper: Get all positions marked for post-processing
    const std::unordered_set<uint64_t>& getPostProcessingPositions() const {
        return m_postProcessing;
    }

    // Helper: Count non-air blocks
    int countNonAirBlocks() const {
        int count = 0;
        for (IBlockType* block : m_blocks) {
            if (!block->isAir()) {
                count++;
            }
        }
        return count;
    }

};

} // namespace world

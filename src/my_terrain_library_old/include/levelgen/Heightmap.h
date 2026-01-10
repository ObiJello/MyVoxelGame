#pragma once

#include "util/SimpleBitStorage.h"
#include "core/BlockPos.h"
#include "world/IBlockType.h"
#include "math/Mth.h"
#include <functional>
#include <string>
#include <cstdint>

namespace minecraft {
namespace levelgen {

/**
 * Heightmap - Tracks surface heights for a chunk
 *
 * Stores height values for a 16x16 grid of columns using compact bit storage.
 * Each heightmap type uses a different predicate to determine which blocks count as "surface".
 *
 * Reference: net/minecraft/world/level/levelgen/Heightmap.java
 */
class Heightmap {
public:
    /**
     * Heightmap types - different criteria for what counts as "surface"
     * Reference: Heightmap.java lines 156-203
     */
    enum class Types {
        // Worldgen heightmaps - used during generation
        WORLD_SURFACE_WG,      // Any non-air block (worldgen)
        OCEAN_FLOOR_WG,        // Motion-blocking blocks (worldgen)

        // Runtime heightmaps - kept after worldgen
        WORLD_SURFACE,         // Any non-air block (client)
        OCEAN_FLOOR,           // Motion-blocking blocks (live world)
        MOTION_BLOCKING,       // Motion-blocking or fluid (client)
        MOTION_BLOCKING_NO_LEAVES  // Motion-blocking/fluid, excluding leaves (client)
    };

    /**
     * Usage category for heightmap types
     * Reference: Heightmap.java lines 144-154
     */
    enum class Usage {
        WORLDGEN,      // Only during world generation
        LIVE_WORLD,    // Kept in live world
        CLIENT         // Sent to client
    };

    using OpaquePredicate = std::function<bool(const ::world::IBlockType*)>;
    using BlockGetter = std::function<const ::world::IBlockType*(int32_t x, int32_t y, int32_t z)>;

private:
    util::SimpleBitStorage m_data;     // Packed height data (256 columns)
    OpaquePredicate m_isOpaque;        // Predicate to test if block is opaque
    BlockGetter m_blockGetter;         // Function to get block at position
    int32_t m_minY;                    // Chunk minimum Y coordinate
    int32_t m_height;                  // Chunk height (max - min)

    /**
     * Get array index for x, z coordinates
     * Reference: Heightmap.java lines 140-142
     */
    static int32_t getIndex(int32_t x, int32_t z) {
        return x + z * 16;
    }

    /**
     * Get first available (non-solid) Y coordinate by index
     * Reference: Heightmap.java lines 118-120
     */
    int32_t getFirstAvailable(int32_t index) const {
        return m_data.get(index) + m_minY;
    }

public:
    /**
     * Constructor
     * Reference: Heightmap.java lines 35-40
     *
     * @param minY - Chunk minimum Y coordinate
     * @param height - Chunk height (e.g., 384 for Overworld)
     * @param type - Heightmap type
     * @param blockGetter - Function to get block at (x, y, z) for scan-downward case
     */
    Heightmap(int32_t minY, int32_t height, Types type, BlockGetter blockGetter = nullptr)
        : m_data(Mth::ceillog2(height + 1), 256)  // 256 columns, log2(height+1) bits each
        , m_isOpaque(getOpaquePredicate(type))
        , m_blockGetter(blockGetter)
        , m_minY(minY)
        , m_height(height)
    {
    }

    /**
     * Set the block getter function (for use after construction)
     */
    void setBlockGetter(BlockGetter blockGetter) {
        m_blockGetter = blockGetter;
    }

    /**
     * Get first available (air) Y coordinate at position
     * Reference: Heightmap.java lines 110-112
     */
    int32_t getFirstAvailable(int32_t x, int32_t z) const {
        return getFirstAvailable(getIndex(x, z));
    }

    /**
     * Get highest taken (solid) Y coordinate at position
     * Reference: Heightmap.java lines 114-116
     */
    int32_t getHighestTaken(int32_t x, int32_t z) const {
        return getFirstAvailable(getIndex(x, z)) - 1;
    }

    /**
     * Set height for a column
     * Reference: Heightmap.java lines 122-124
     *
     * @param x - Local X coordinate (0-15)
     * @param z - Local Z coordinate (0-15)
     * @param height - Absolute Y coordinate
     */
    void setHeight(int32_t x, int32_t z, int32_t height) {
        m_data.set(getIndex(x, z), height - m_minY);
    }

    /**
     * Update heightmap when a block is set
     * Reference: Heightmap.java lines 82-109
     *
     * @param localX - Local X coordinate (0-15)
     * @param localY - Absolute Y coordinate
     * @param localZ - Local Z coordinate (0-15)
     * @param state - New block state
     * @return true if heightmap was updated
     */
    bool update(int32_t localX, int32_t localY, int32_t localZ, const ::world::IBlockType* state) {
        int32_t firstAvailable = getFirstAvailable(localX, localZ);

        // If block is too far below current surface, no update needed
        // Reference: Heightmap.java line 84
        if (localY <= firstAvailable - 2) {
            return false;
        }

        // If block is opaque and at or above current surface, update height
        // Reference: Heightmap.java lines 87-91
        if (m_isOpaque(state)) {
            if (localY >= firstAvailable) {
                setHeight(localX, localZ, localY + 1);
                return true;
            }
        }
        // If block became non-opaque at current surface level, scan downward
        // Reference: Heightmap.java lines 92-105
        else if (firstAvailable - 1 == localY) {
            // Scan down to find new surface
            if (m_blockGetter) {
                for (int32_t y = localY - 1; y >= m_minY; --y) {
                    const ::world::IBlockType* blockBelow = m_blockGetter(localX, y, localZ);
                    if (m_isOpaque(blockBelow)) {
                        setHeight(localX, localZ, y + 1);
                        return true;
                    }
                }
            }

            // No opaque block found, set to minimum
            setHeight(localX, localZ, m_minY);
            return true;
        }

        return false;
    }

    /**
     * Get raw data array
     * Reference: Heightmap.java lines 136-138
     */
    const util::SimpleBitStorage& getData() const {
        return m_data;
    }

    util::SimpleBitStorage& getData() {
        return m_data;
    }

    /**
     * Set raw data
     * Reference: Heightmap.java lines 126-134
     */
    void setRawData(const std::vector<int64_t>& data) {
        if (data.size() == m_data.getRaw().size()) {
            std::copy(data.begin(), data.end(), m_data.getRaw().begin());
        }
        // Note: In real implementation, should log warning and reprime if sizes don't match
    }

    /**
     * Get raw data for serialization
     */
    std::vector<int64_t> getRawData() const {
        return m_data.getRaw();
    }

    /**
     * Get opaque predicate for heightmap type
     * Reference: Heightmap.java lines 156-177 (Types enum)
     */
    static OpaquePredicate getOpaquePredicate(Types type);

    /**
     * Get usage category for heightmap type
     * Reference: Heightmap.java lines 183-189
     */
    static Usage getUsage(Types type) {
        switch (type) {
            case Types::WORLD_SURFACE_WG:
            case Types::OCEAN_FLOOR_WG:
                return Usage::WORLDGEN;
            case Types::OCEAN_FLOOR:
                return Usage::LIVE_WORLD;
            case Types::WORLD_SURFACE:
            case Types::MOTION_BLOCKING:
            case Types::MOTION_BLOCKING_NO_LEAVES:
                return Usage::CLIENT;
        }
        return Usage::CLIENT;
    }

    /**
     * Check if heightmap type should be kept after worldgen
     * Reference: Heightmap.java lines 187-189
     */
    static bool keepAfterWorldgen(Types type) {
        return getUsage(type) != Usage::WORLDGEN;
    }

    /**
     * Check if heightmap type should be sent to client
     * Reference: Heightmap.java lines 183-185
     */
    static bool sendToClient(Types type) {
        return getUsage(type) == Usage::CLIENT;
    }
};

} // namespace levelgen
} // namespace minecraft

#pragma once

#include "world/LevelChunkSection.h"
#include "world/IBlockType.h"
#include <cstdint>

namespace minecraft {
namespace server {
namespace level {

/**
 * ServerLevel - Server-side world level
 * Reference: net/minecraft/server/level/ServerLevel.java
 *
 * This class provides access to world-level services needed for chunk operations.
 * For terrain generation parity, we only implement the minimal interface needed
 * for chunk serialization.
 */
class ServerLevel {
private:
    int32_t m_minY;
    int32_t m_height;
    world::BlockRegistry* m_blockRegistry;
    world::IBlockType* m_airBlock;
    world::IBlockType* m_defaultBlock;
    int64_t m_gameTime;

public:
    /**
     * Constructor
     * Reference: ServerLevel.java constructor
     */
    ServerLevel(
        int32_t minY,
        int32_t height,
        world::BlockRegistry* blockRegistry,
        world::IBlockType* airBlock,
        world::IBlockType* defaultBlock = nullptr
    )
        : m_minY(minY)
        , m_height(height)
        , m_blockRegistry(blockRegistry)
        , m_airBlock(airBlock)
        , m_defaultBlock(defaultBlock ? defaultBlock : airBlock)
        , m_gameTime(0)
    {}

    // =========================================================================
    // Height accessor methods
    // Reference: LevelHeightAccessor interface
    // =========================================================================

    /**
     * Get minimum build height (Y coordinate)
     * Reference: LevelHeightAccessor.getMinBuildHeight()
     */
    int32_t getMinBuildHeight() const { return m_minY; }

    /**
     * Get maximum build height (Y coordinate)
     * Reference: LevelHeightAccessor.getMaxBuildHeight()
     */
    int32_t getMaxBuildHeight() const { return m_minY + m_height; }

    /**
     * Get world height (max - min)
     * Reference: LevelHeightAccessor.getHeight()
     */
    int32_t getHeight() const { return m_height; }

    // =========================================================================
    // Registry access
    // Reference: ServerLevel.registryAccess()
    // =========================================================================

    /**
     * Get the block registry
     */
    world::BlockRegistry* getBlockRegistry() const { return m_blockRegistry; }

    /**
     * Get the air block type
     */
    world::IBlockType* getAirBlock() const { return m_airBlock; }

    /**
     * Get the default block type (used for unknown blocks)
     */
    world::IBlockType* getDefaultBlock() const { return m_defaultBlock; }

    // =========================================================================
    // Time methods
    // Reference: ServerLevel.getGameTime()
    // =========================================================================

    /**
     * Get the current game time (in ticks)
     * Reference: ServerLevel.getGameTime()
     */
    int64_t getGameTime() const { return m_gameTime; }

    /**
     * Set the current game time
     */
    void setGameTime(int64_t time) { m_gameTime = time; }

    /**
     * Advance game time by ticks
     */
    void advanceGameTime(int64_t ticks) { m_gameTime += ticks; }
};

} // namespace level
} // namespace server
} // namespace minecraft

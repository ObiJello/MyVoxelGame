#pragma once

#include "core/BlockPos.h"
#include "core/SectionPos.h"
#include "world/LevelChunkSection.h"
#include "world/level/block/state/BlockState.h"
#include "world/level/block/Blocks.h"
#include "levelgen/WorldGenLevel.h"
#include <unordered_map>
#include <cstdint>
#include <limits>

namespace minecraft {
namespace world {
namespace level {
namespace chunk {

/**
 * BulkSectionAccess - Provides cross-chunk block access with section caching
 * Reference: net/minecraft/world/level/chunk/BulkSectionAccess.java
 *
 * Key behaviors (matching Java exactly):
 * - Caches accessed sections for performance
 * - Returns null for sections when Y is out of bounds
 * - getBlockState() returns AIR when section is null (Y out of bounds)
 * - Throws exception if chunk is not available (via WorldGenRegion.getChunk())
 * - Must call close() to release section references
 *
 * This class is critical for ore placement parity. When checking if an ore
 * position is adjacent to air, Java uses this class to get blocks from
 * neighboring chunks. The C++ IChunk::getBlockState() wraps coordinates
 * via & 15, returning the wrong block from the current chunk.
 *
 * During FEATURES phase, neighbors at distance 1 are available at CARVERS
 * status, so getChunk() should not throw for valid neighbor access.
 */
class BulkSectionAccess {
private:
    levelgen::WorldGenLevel* m_level;
    std::unordered_map<int64_t, LevelChunkSection*> m_acquiredSections;
    LevelChunkSection* m_lastSection = nullptr;
    int64_t m_lastSectionKey = std::numeric_limits<int64_t>::min();

public:
    /**
     * Constructor
     * Reference: BulkSectionAccess.java constructor line 19-21
     */
    explicit BulkSectionAccess(levelgen::WorldGenLevel* level)
        : m_level(level)
    {}

    /**
     * Destructor - automatically releases sections (RAII)
     */
    ~BulkSectionAccess() {
        close();
    }

    // Non-copyable
    BulkSectionAccess(const BulkSectionAccess&) = delete;
    BulkSectionAccess& operator=(const BulkSectionAccess&) = delete;

    // Movable
    BulkSectionAccess(BulkSectionAccess&& other) noexcept
        : m_level(other.m_level)
        , m_acquiredSections(std::move(other.m_acquiredSections))
        , m_lastSection(other.m_lastSection)
        , m_lastSectionKey(other.m_lastSectionKey)
    {
        other.m_level = nullptr;
        other.m_lastSection = nullptr;
    }

    /**
     * Get section at block position
     * Reference: BulkSectionAccess.java getSection() lines 23-41
     *
     * @param pos Block position (absolute world coordinates)
     * @return Section pointer, or nullptr if chunk not loaded or out of bounds
     */
    LevelChunkSection* getSection(const core::BlockPos& pos) {
        // Calculate section index from Y coordinate
        // Reference: LevelHeightAccessor.java getSectionIndex()
        int32_t sectionY = core::SectionPos::blockToSectionCoord(pos.getY());
        int32_t minSectionY = core::SectionPos::blockToSectionCoord(m_level->getMinY());
        int32_t maxSectionY = core::SectionPos::blockToSectionCoord(m_level->getMaxY());
        int32_t sectionIndex = sectionY - minSectionY;

        // Bounds check - section index must be valid
        // Reference: BulkSectionAccess.java line 24-25
        if (sectionIndex < 0 || sectionY > maxSectionY) {
            return nullptr;
        }

        // Calculate section key for caching
        // Reference: BulkSectionAccess.java line 26
        int64_t sectionKey = core::SectionPos::asLong(pos);

        // Check last-accessed cache (optimization)
        // Reference: BulkSectionAccess.java line 27
        if (m_lastSection != nullptr && m_lastSectionKey == sectionKey) {
            return m_lastSection;
        }

        // Check acquired sections map
        // Reference: BulkSectionAccess.java line 28 (computeIfAbsent)
        auto it = m_acquiredSections.find(sectionKey);
        if (it != m_acquiredSections.end()) {
            m_lastSection = it->second;
            m_lastSectionKey = sectionKey;
            return m_lastSection;
        }

        // Need to load section from chunk
        // Reference: BulkSectionAccess.java line 29
        int32_t chunkX = core::SectionPos::blockToSectionCoord(pos.getX());
        int32_t chunkZ = core::SectionPos::blockToSectionCoord(pos.getZ());

        // Get chunk from level
        // Reference: BulkSectionAccess.java lines 29-31
        //
        // PARITY NOTE: In Java, FEATURES phase has chunks available at radius 8
        // (STRUCTURE_STARTS status), but only radius 1 has terrain (CARVERS).
        // Chunks at distance 2-8 are empty (no terrain) - getBlockState returns AIR.
        // Our C++ cache only has radius 1. For chunks outside the cache:
        // - WorldGenRegion.getChunk() throws (matching Java's behavior for truly
        //   non-existent chunks)
        // - But Java would have empty chunks at distance 2-8 returning AIR
        //
        // For parity, we catch exceptions and return nullptr, leading to AIR
        // being returned - matching what Java sees from empty STRUCTURE_STARTS chunks.
        ::world::IChunk* chunk = nullptr;
        try {
            chunk = m_level->getChunk(chunkX, chunkZ);
        } catch (...) {
            // Chunk not in cache - treat as empty chunk (return AIR)
            // This matches Java behavior where distance 2-8 chunks are empty
            return nullptr;
        }

        if (!chunk) {
            // Fallback for WorldGenLevel implementations that return nullptr
            return nullptr;
        }

        // Get section and acquire reference
        // Reference: BulkSectionAccess.java lines 30-31
        LevelChunkSection* section = &chunk->getSection(sectionIndex);
        section->acquire();

        // Cache the section
        // Reference: BulkSectionAccess.java line 32-34
        m_acquiredSections[sectionKey] = section;
        m_lastSection = section;
        m_lastSectionKey = sectionKey;

        return section;
    }

    /**
     * Get block state at position
     * Reference: BulkSectionAccess.java getBlockState() lines 43-52
     *
     * @param pos Block position (absolute world coordinates)
     * @return Block state, or AIR if section is null (non-loaded chunk)
     *
     * CRITICAL: This method returns AIR for positions in non-loaded chunks.
     * This is the key difference from IChunk::getBlockState() which wraps
     * coordinates via & 15 and returns the wrong block.
     */
    block::state::BlockState* getBlockState(const core::BlockPos& pos) {
        LevelChunkSection* section = getSection(pos);

        if (section == nullptr) {
            // Non-loaded chunk or out of bounds - return AIR
            // Reference: BulkSectionAccess.java line 46
            // This matches Java's behavior exactly
            return block::Blocks::AIR->defaultBlockState();
        }

        // Convert to section-relative coordinates (0-15)
        // Reference: BulkSectionAccess.java lines 48-50
        int32_t relX = core::SectionPos::sectionRelative(pos.getX());
        int32_t relY = core::SectionPos::sectionRelative(pos.getY());
        int32_t relZ = core::SectionPos::sectionRelative(pos.getZ());

        // Reference: BulkSectionAccess.java line 51
        return section->getBlockState(relX, relY, relZ);
    }

    /**
     * Release all acquired sections
     * Reference: BulkSectionAccess.java close() lines 55-62
     *
     * Called automatically by destructor (RAII pattern).
     * In Java this is called via try-with-resources.
     */
    void close() {
        // Reference: BulkSectionAccess.java lines 56-60
        for (auto& [key, section] : m_acquiredSections) {
            if (section) {
                section->release();
            }
        }
        m_acquiredSections.clear();
        m_lastSection = nullptr;
        m_lastSectionKey = std::numeric_limits<int64_t>::min();
    }
};

} // namespace chunk
} // namespace level
} // namespace world
} // namespace minecraft

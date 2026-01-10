#pragma once

#include "server/level/GenerationChunkHolder.h"
#include "server/level/ChunkResult.h"
#include "server/level/FullChunkStatus.h"
#include "server/level/ChunkLevel.h"
#include "util/CompletableFuture.h"
#include "world/ChunkPos.h"
#include <memory>
#include <functional>
#include <atomic>
#include <vector>
#include <bitset>

// Reference: net/minecraft/server/level/ChunkHolder.java

namespace minecraft {

// Forward declarations
namespace server { namespace level {
    class ChunkMap;
}}

namespace server {
namespace level {

// Forward declaration for LevelChunk (would be the actual chunk type)
using LevelChunk = ::world::IChunk;

/**
 * ChunkHolder - Server-side chunk holder that extends GenerationChunkHolder
 * Reference: ChunkHolder.java
 *
 * This class manages a chunk's full lifecycle including:
 * - Three levels of chunk futures (full, ticking, entity ticking)
 * - Block and light change tracking
 * - Ticket level management
 * - Save synchronization
 */
class ChunkHolder : public GenerationChunkHolder {
public:
    // Type aliases for LevelChunk results
    using LevelChunkResult = std::shared_ptr<ChunkResult<LevelChunk*>>;
    using LevelChunkFuture = std::shared_ptr<util::CompletableFuture<LevelChunkResult>>;

    // Static sentinels
    // Reference: ChunkHolder.java lines 32-33
    static LevelChunkResult UNLOADED_LEVEL_CHUNK;
    static LevelChunkFuture UNLOADED_LEVEL_CHUNK_FUTURE;

    /**
     * Initialize static members
     */
    static void initializeStatics();

    /**
     * LevelChangeListener - Callback for level changes
     * Reference: ChunkHolder.java lines 337-340
     */
    using LevelChangeListener = std::function<void(
        const world::ChunkPos& pos,
        std::function<int()> oldLevel,
        int newLevel,
        std::function<void(int)> setQueueLevel
    )>;

    /**
     * PlayerProvider - Interface for getting players near a chunk
     * Reference: ChunkHolder.java lines 342-344
     */
    using PlayerProvider = std::function<std::vector<void*>(const world::ChunkPos& pos, bool borderOnly)>;

    /**
     * Constructor
     * Reference: ChunkHolder.java lines 53-72
     */
    ChunkHolder(
        const world::ChunkPos& pos,
        int ticketLevel,
        int levelHeight,
        int minY,
        LevelChangeListener onLevelChange,
        PlayerProvider playerProvider
    );

    /**
     * Mark the chunk as unsaved (needs to be written to disk)
     * Reference: ChunkHolder.java markUnsaved delegates to chunk
     */
    void markUnsaved() {
        // TODO: Delegate to chunk when chunk saving is implemented
    }

    /**
     * Get the ticking chunk future
     * Reference: ChunkHolder.java lines 74-76
     */
    LevelChunkFuture getTickingChunkFuture();

    /**
     * Get the entity ticking chunk future
     * Reference: ChunkHolder.java lines 78-80
     */
    LevelChunkFuture getEntityTickingChunkFuture();

    /**
     * Get the full chunk future
     * Reference: ChunkHolder.java lines 82-84
     */
    LevelChunkFuture getFullChunkFuture();

    /**
     * Get the ticking chunk if available
     * Reference: ChunkHolder.java lines 86-88
     */
    LevelChunk* getTickingChunk();

    /**
     * Get the chunk to send (if send sync is done)
     * Reference: ChunkHolder.java lines 90-92
     */
    LevelChunk* getChunkToSend();

    /**
     * Get the send synchronization future
     * Reference: ChunkHolder.java lines 94-96
     */
    std::shared_ptr<util::CompletableFuture<void>> getSendSyncFuture();

    /**
     * Add a send dependency
     * Reference: ChunkHolder.java lines 98-105
     */
    void addSendDependency(std::shared_ptr<util::CompletableFuture<void>> sync);

    /**
     * Get the save synchronization future
     * Reference: ChunkHolder.java lines 107-109
     */
    std::shared_ptr<util::CompletableFuture<void>> getSaveSyncFuture();

    /**
     * Check if ready for saving
     * Reference: ChunkHolder.java lines 111-113
     */
    bool isReadyForSaving() const;

    /**
     * Add a save dependency
     * Reference: ChunkHolder.java lines 115-122
     */
    void addSaveDependency(std::shared_ptr<util::CompletableFuture<void>> sync) override;

    /**
     * Mark a block as changed
     * Reference: ChunkHolder.java lines 124-141
     *
     * @return true if this is the first change in this tick
     */
    bool blockChanged(int x, int y, int z);

    /**
     * Mark a light section as changed
     * Reference: ChunkHolder.java lines 143-169
     *
     * @param layer 0 for block, 1 for sky
     * @param chunkY The Y coordinate of the section
     * @return true if this section wasn't already marked
     */
    bool sectionLightChanged(int layer, int chunkY);

    /**
     * Check if there are changes to broadcast
     * Reference: ChunkHolder.java lines 171-173
     */
    bool hasChangesToBroadcast() const;

    /**
     * Get the ticket level
     * Reference: ChunkHolder.java lines 241-243
     */
    int getTicketLevel() const override;

    /**
     * Get the queue level
     * Reference: ChunkHolder.java lines 245-247
     */
    int getQueueLevel() const override;

    /**
     * Set the ticket level
     * Reference: ChunkHolder.java lines 253-255
     */
    void setTicketLevel(int ticketLevel);

    /**
     * Update futures based on ticket level change
     * Reference: ChunkHolder.java lines 270-323
     */
    void updateFutures(ChunkMap& scheduler, std::function<void(std::function<void()>)> mainThreadExecutor);

    /**
     * Check if was accessible since last save
     * Reference: ChunkHolder.java lines 325-327
     */
    bool wasAccessibleSinceLastSave() const;

    /**
     * Refresh accessibility flag
     * Reference: ChunkHolder.java lines 329-331
     */
    void refreshAccessibility();

private:
    /**
     * Set the queue level
     * Reference: ChunkHolder.java lines 249-251
     */
    void setQueueLevel(int queueLevel);

    /**
     * Schedule full chunk promotion
     * Reference: ChunkHolder.java lines 257-263
     */
    void scheduleFullChunkPromotion(
        ChunkMap& scheduler,
        LevelChunkFuture task,
        std::function<void(std::function<void()>)> mainThreadExecutor,
        FullChunkStatus status
    );

    /**
     * Demote full chunk
     * Reference: ChunkHolder.java lines 265-268
     */
    void demoteFullChunk(ChunkMap& scheduler, FullChunkStatus status);

    /**
     * Helper to get section index from Y coordinate
     */
    int getSectionIndex(int y) const;

    // Level height info
    int m_levelHeight;
    int m_minY;
    int m_sectionsCount;

    // Chunk futures for different states
    LevelChunkFuture m_fullChunkFuture;
    LevelChunkFuture m_tickingChunkFuture;
    LevelChunkFuture m_entityTickingChunkFuture;

    // Ticket levels
    int m_oldTicketLevel;
    std::atomic<int> m_ticketLevel;
    std::atomic<int> m_queueLevel;

    // Change tracking
    std::atomic<bool> m_hasChangedSections{false};
    std::vector<std::set<short>> m_changedBlocksPerSection;
    std::bitset<64> m_blockChangedLightSectionFilter;
    std::bitset<64> m_skyChangedLightSectionFilter;

    // Callbacks
    LevelChangeListener m_onLevelChange;
    PlayerProvider m_playerProvider;

    // State flags
    std::atomic<bool> m_wasAccessibleSinceLastSave{false};

    // Synchronization futures
    std::shared_ptr<util::CompletableFuture<void>> m_pendingFullStateConfirmation;
    std::shared_ptr<util::CompletableFuture<void>> m_sendSync;
    std::shared_ptr<util::CompletableFuture<void>> m_saveSync;

    // Mutex for non-atomic operations
    mutable std::mutex m_mutex;
};

} // namespace level
} // namespace server
} // namespace minecraft

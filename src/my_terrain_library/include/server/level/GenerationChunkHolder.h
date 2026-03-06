#pragma once

#include "server/level/ChunkResult.h"
#include "server/level/ChunkLevel.h"
#include "server/level/FullChunkStatus.h"
#include "util/CompletableFuture.h"
#include "util/StaticCache2D.h"
#include "world/ChunkPos.h"
#include "world/chunk/status/ChunkStatus.h"
#include "world/chunk/status/ChunkStep.h"
#include "world/IChunk.h"
#include <atomic>
#include <array>
#include <memory>
#include <vector>
#include <utility>

// Reference: net/minecraft/server/level/GenerationChunkHolder.java

namespace minecraft {

// Forward declarations
namespace server { namespace level {
    class ChunkMap;
    class ChunkGenerationTask;
    class GeneratingChunkMap;
}}

namespace server {
namespace level {

/**
 * GenerationChunkHolder - Base class for chunk holders during generation
 * Reference: GenerationChunkHolder.java
 *
 * This class manages the generation state of a chunk, including:
 * - Futures for each generation status
 * - Task scheduling for generation
 * - Reference counting for generation phase
 */
class GenerationChunkHolder {
public:
    // Type aliases
    using ChunkAccess = ::world::IChunk;
    using ChunkResultType = std::shared_ptr<ChunkResult<ChunkAccess*>>;
    using FutureType = std::shared_ptr<util::CompletableFuture<ChunkResultType>>;

    // Static sentinels
    // Reference: GenerationChunkHolder.java lines 26-28
    static ChunkResultType NOT_DONE_YET;
    static ChunkResultType UNLOADED_CHUNK;
    static FutureType UNLOADED_CHUNK_FUTURE;

    /**
     * Initialize static members
     * Call once at startup
     */
    static void initializeStatics();

    /**
     * Constructor
     * Reference: GenerationChunkHolder.java lines 37-46
     */
    explicit GenerationChunkHolder(const world::ChunkPos& pos);

    virtual ~GenerationChunkHolder() = default;

    /**
     * Schedule chunk generation to reach the given status
     * Reference: GenerationChunkHolder.java lines 48-64
     */
    FutureType scheduleChunkGenerationTask(
        const world::chunk::status::ChunkStatus& status,
        ChunkMap& scheduler
    );

    /**
     * Apply a generation step
     * Reference: GenerationChunkHolder.java lines 66-81
     */
    FutureType applyStep(
        const world::chunk::status::ChunkStep& step,
        GeneratingChunkMap& chunkMap,
        util::StaticCache2D<GenerationChunkHolder*>& cache
    );

    /**
     * Update the highest allowed status based on ticket level
     * Reference: GenerationChunkHolder.java lines 83-95
     */
    void updateHighestAllowedStatus(ChunkMap& scheduler);

    /**
     * Replace proto chunks with an imposter chunk
     * Reference: GenerationChunkHolder.java lines 97-113
     */
    void replaceProtoChunk(ChunkAccess* imposterChunk);

    /**
     * Remove a generation task
     * Reference: GenerationChunkHolder.java lines 115-117
     */
    void removeTask(ChunkGenerationTask* task);

    /**
     * Increase generation reference count
     * Reference: GenerationChunkHolder.java lines 241-247
     */
    void increaseGenerationRefCount();

    /**
     * Decrease generation reference count
     * Reference: GenerationChunkHolder.java lines 249-259
     */
    void decreaseGenerationRefCount();

    /**
     * Get chunk if present, unchecked (no status validation)
     * Reference: GenerationChunkHolder.java lines 261-264
     */
    virtual ChunkAccess* getChunkIfPresentUnchecked(const world::chunk::status::ChunkStatus& status) const;

    /**
     * Get chunk if present and allowed
     * Reference: GenerationChunkHolder.java lines 266-268
     */
    ChunkAccess* getChunkIfPresent(const world::chunk::status::ChunkStatus& status) const;

    /**
     * Get the latest chunk available
     * Reference: GenerationChunkHolder.java lines 270-278
     */
    ChunkAccess* getLatestChunk() const;

    /**
     * Get the persisted status from the EMPTY future
     * Reference: GenerationChunkHolder.java lines 280-284
     */
    const world::chunk::status::ChunkStatus* getPersistedStatus() const;

    /**
     * Get the chunk position
     * Reference: GenerationChunkHolder.java lines 286-288
     */
    const world::ChunkPos& getPos() const { return m_pos; }

    /**
     * Get the full chunk status based on ticket level
     * Reference: GenerationChunkHolder.java lines 290-292
     */
    FullChunkStatus getFullStatus() const;

    /**
     * Get the ticket level (abstract - implemented by ChunkHolder)
     * Reference: GenerationChunkHolder.java line 294
     */
    virtual int getTicketLevel() const = 0;

    /**
     * Get the queue level (abstract - implemented by ChunkHolder)
     * Reference: GenerationChunkHolder.java line 296
     */
    virtual int getQueueLevel() const = 0;

    /**
     * Get all futures for debugging
     * Reference: GenerationChunkHolder.java lines 299-307
     */
    std::vector<std::pair<const world::chunk::status::ChunkStatus*, FutureType>> getAllFutures() const;

    /**
     * Get the latest status for debugging
     * Reference: GenerationChunkHolder.java lines 310-318
     */
    const world::chunk::status::ChunkStatus* getLatestStatus() const;

protected:
    /**
     * Add a save dependency (abstract - implemented by ChunkHolder)
     * Reference: GenerationChunkHolder.java line 239
     */
    virtual void addSaveDependency(std::shared_ptr<util::CompletableFuture<void>> sync) = 0;

    world::ChunkPos m_pos;

private:
    /**
     * Reschedule chunk task
     * Reference: GenerationChunkHolder.java lines 119-132
     */
    void rescheduleChunkTask(ChunkMap& scheduler, const world::chunk::status::ChunkStatus* status);

    /**
     * Get or create a future for a status
     * Reference: GenerationChunkHolder.java lines 134-156
     */
    FutureType getOrCreateFuture(const world::chunk::status::ChunkStatus& status);

    /**
     * Fail and clear pending futures between two statuses
     * Reference: GenerationChunkHolder.java lines 158-169
     */
    void failAndClearPendingFuturesBetween(
        const world::chunk::status::ChunkStatus* fromExclusive,
        const world::chunk::status::ChunkStatus& toInclusive
    );

    /**
     * Fail and clear a single pending future
     * Reference: GenerationChunkHolder.java lines 171-175
     */
    void failAndClearPendingFuture(int index, FutureType& previous);

    /**
     * Fail and clear a pending future (must be called with m_futuresMutex held)
     */
    void failAndClearPendingFutureUnlocked(int index, FutureType& previous);

    /**
     * Complete a future with a chunk
     * Reference: GenerationChunkHolder.java lines 177-199
     */
    void completeFuture(const world::chunk::status::ChunkStatus& status, ChunkAccess* chunk);

    /**
     * Find the highest status with a pending future
     * Reference: GenerationChunkHolder.java lines 201-219
     */
    const world::chunk::status::ChunkStatus* findHighestStatusWithPendingFuture(
        const world::chunk::status::ChunkStatus* newStatus
    );

    /**
     * Acquire the right to bump to a new status
     * Reference: GenerationChunkHolder.java lines 221-232
     */
    bool acquireStatusBump(const world::chunk::status::ChunkStatus& status);

    /**
     * Check if a status is disallowed
     * Reference: GenerationChunkHolder.java lines 234-237
     */
    bool isStatusDisallowed(const world::chunk::status::ChunkStatus& status) const;

    // The highest allowed status based on ticket level
    std::atomic<const world::chunk::status::ChunkStatus*> m_highestAllowedStatus{nullptr};

    // The status we've started working on
    std::atomic<const world::chunk::status::ChunkStatus*> m_startedWork{nullptr};

    // Array of futures, one for each status
    // Protected by m_futuresMutex since Apple's libc++ doesn't support std::atomic<shared_ptr>
    static constexpr int STATUS_COUNT = 12;  // Number of ChunkStatus values
    mutable std::mutex m_futuresMutex;
    std::array<FutureType, STATUS_COUNT> m_futures;

    // Current generation task (protected by m_taskMutex)
    // Uses shared_ptr for Java-like GC semantics
    mutable std::mutex m_taskMutex;
    std::shared_ptr<ChunkGenerationTask> m_task;

    // Reference count for generation phase
    std::atomic<int> m_generationRefCount{0};

    // Future for generation save synchronization
    std::shared_ptr<util::CompletableFuture<void>> m_generationSaveSyncFuture;

    // Mutex for atomic operations that can't be done lock-free
    mutable std::mutex m_mutex;
};

/**
 * SimpleGenerationChunkHolder - Concrete implementation for sequential chunk generation
 *
 * In Java, ChunkHolder is the concrete subclass of GenerationChunkHolder.
 * This is a simplified version for sequential/test use cases where we don't
 * need the full async chunk loading machinery.
 */
class SimpleGenerationChunkHolder : public GenerationChunkHolder {
private:
    ChunkAccess* m_chunk;

public:
    /**
     * Constructor with chunk
     */
    explicit SimpleGenerationChunkHolder(ChunkAccess* chunk)
        : GenerationChunkHolder(chunk ? chunk->getPos() : world::ChunkPos(0, 0))
        , m_chunk(chunk)
    {
    }

    /**
     * Get the wrapped chunk
     */
    ChunkAccess* getChunk() const { return m_chunk; }

    /**
     * Get chunk if present - returns our wrapped chunk regardless of status
     */
    ChunkAccess* getChunkIfPresentUnchecked(const world::chunk::status::ChunkStatus& /*status*/) const override {
        return m_chunk;
    }

    // Abstract method implementations (no-op for sequential generation)
    int getTicketLevel() const override { return 0; }
    int getQueueLevel() const override { return 0; }

protected:
    void addSaveDependency(std::shared_ptr<util::CompletableFuture<void>> /*sync*/) override {
        // No-op for sequential generation
    }
};

} // namespace level
} // namespace server
} // namespace minecraft

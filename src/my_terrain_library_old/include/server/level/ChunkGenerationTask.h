#pragma once

#include "server/level/GenerationChunkHolder.h"
#include "server/level/ChunkResult.h"
#include "util/CompletableFuture.h"
#include "util/StaticCache2D.h"
#include "world/ChunkPos.h"
#include "world/chunk/status/ChunkStatus.h"
#include "world/chunk/status/ChunkPyramid.h"
#include <vector>
#include <atomic>
#include <memory>

// Reference: net/minecraft/server/level/ChunkGenerationTask.java

namespace minecraft {
namespace server {
namespace level {

// Forward declarations
class GeneratingChunkMap;

/**
 * ChunkGenerationTask - Main orchestrator for generating a chunk through all phases
 * Reference: ChunkGenerationTask.java
 *
 * This class manages the progression of a chunk through generation statuses,
 * handling dependencies on neighboring chunks and scheduling work appropriately.
 */
class ChunkGenerationTask {
public:
    // Type aliases
    using ChunkAccess = ::world::IChunk;
    using ChunkResultType = std::shared_ptr<ChunkResult<ChunkAccess*>>;
    using FutureType = std::shared_ptr<util::CompletableFuture<ChunkResultType>>;

    /**
     * Factory method to create a generation task
     * Reference: ChunkGenerationTask.java lines 34-38
     *
     * @param chunkMap The chunk map for acquiring holders
     * @param targetStatus The status to generate to
     * @param pos The chunk position
     * @return A new ChunkGenerationTask (shared_ptr for Java-like GC semantics)
     */
    static std::shared_ptr<ChunkGenerationTask> create(
        GeneratingChunkMap& chunkMap,
        const world::chunk::status::ChunkStatus& targetStatus,
        const world::ChunkPos& pos
    );

    /**
     * Run the task until it needs to wait for dependencies
     * Reference: ChunkGenerationTask.java lines 40-54
     *
     * @return A future to wait on, or nullptr if done
     */
    std::shared_ptr<util::CompletableFuture<void>> runUntilWait();

    /**
     * Mark this task for cancellation
     * Reference: ChunkGenerationTask.java lines 71-73
     */
    void markForCancellation();

    /**
     * Get the center chunk holder
     * Reference: ChunkGenerationTask.java lines 111-113
     */
    GenerationChunkHolder* getCenter();

    /**
     * Get the target status
     */
    const world::chunk::status::ChunkStatus& getTargetStatus() const { return m_targetStatus; }

    /**
     * Get the chunk position
     */
    const world::ChunkPos& getPos() const { return m_pos; }

private:
    /**
     * Private constructor - use create() factory method
     * Reference: ChunkGenerationTask.java lines 27-32
     */
    ChunkGenerationTask(
        GeneratingChunkMap& chunkMap,
        const world::chunk::status::ChunkStatus& targetStatus,
        const world::ChunkPos& pos,
        util::StaticCache2D<GenerationChunkHolder*> cache
    );

    /**
     * Schedule the next layer of generation
     * Reference: ChunkGenerationTask.java lines 56-69
     */
    void scheduleNextLayer();

    /**
     * Release the claim on chunk holders
     * Reference: ChunkGenerationTask.java lines 75-82
     */
    void releaseClaim();

    /**
     * Check if the chunk can be loaded without generation
     * Reference: ChunkGenerationTask.java lines 84-109
     */
    bool canLoadWithoutGeneration();

    /**
     * Schedule all chunks in a layer
     * Reference: ChunkGenerationTask.java lines 115-131
     */
    void scheduleLayer(const world::chunk::status::ChunkStatus& status, bool needsGeneration);

    /**
     * Get the radius for a layer
     * Reference: ChunkGenerationTask.java lines 133-136
     */
    int getRadiusForLayer(const world::chunk::status::ChunkStatus& status, bool needsGeneration);

    /**
     * Schedule a single chunk in a layer
     * Reference: ChunkGenerationTask.java lines 138-157
     */
    bool scheduleChunkInLayer(
        const world::chunk::status::ChunkStatus& status,
        bool needsGeneration,
        GenerationChunkHolder* chunkHolder
    );

    /**
     * Wait for the scheduled layer to complete
     * Reference: ChunkGenerationTask.java lines 159-174
     */
    std::shared_ptr<util::CompletableFuture<void>> waitForScheduledLayer();

    GeneratingChunkMap* m_chunkMap;
    world::ChunkPos m_pos;
    const world::chunk::status::ChunkStatus* m_scheduledStatus{nullptr};
    const world::chunk::status::ChunkStatus& m_targetStatus;
    std::atomic<bool> m_markedForCancellation{false};
    std::vector<FutureType> m_scheduledLayer;
    util::StaticCache2D<GenerationChunkHolder*> m_cache;
    bool m_needsGeneration{false};
};

} // namespace level
} // namespace server
} // namespace minecraft

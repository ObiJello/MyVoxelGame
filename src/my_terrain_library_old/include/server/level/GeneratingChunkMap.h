#pragma once

#include "server/level/GenerationChunkHolder.h"
#include "server/level/ChunkResult.h"
#include "util/CompletableFuture.h"
#include "util/StaticCache2D.h"
#include "world/chunk/status/ChunkStatus.h"
#include "world/chunk/status/ChunkStep.h"
#include "world/ChunkPos.h"
#include <cstdint>

// Reference: net/minecraft/server/level/GeneratingChunkMap.java

namespace minecraft {
namespace server {
namespace level {

// Forward declarations
class ChunkGenerationTask;

/**
 * GeneratingChunkMap - Interface for chunk generation coordination
 * Reference: GeneratingChunkMap.java
 *
 * This interface defines the operations needed by ChunkGenerationTask
 * to manage chunk generation.
 */
class GeneratingChunkMap {
public:
    // Type aliases
    using ChunkAccess = ::world::IChunk;
    using ChunkResultType = std::shared_ptr<ChunkResult<ChunkAccess*>>;
    using FutureType = std::shared_ptr<util::CompletableFuture<ChunkResultType>>;

    virtual ~GeneratingChunkMap() = default;

    /**
     * Acquire a chunk holder for generation
     * Reference: GeneratingChunkMap.java - acquireGeneration
     *
     * Increments the generation reference count on the holder.
     *
     * @param chunkPos The chunk position as a long
     * @return The GenerationChunkHolder for that position
     */
    virtual GenerationChunkHolder* acquireGeneration(int64_t chunkPos) = 0;

    /**
     * Release a chunk holder from generation
     * Reference: GeneratingChunkMap.java - releaseGeneration
     *
     * Decrements the generation reference count on the holder.
     *
     * @param chunkHolder The holder to release
     */
    virtual void releaseGeneration(GenerationChunkHolder* chunkHolder) = 0;

    /**
     * Apply a generation step to a chunk
     * Reference: GeneratingChunkMap.java - applyStep
     *
     * @param chunkHolder The chunk holder
     * @param step The step to apply
     * @param cache The cache of neighboring chunk holders
     * @return A future that resolves to the chunk when complete
     */
    virtual std::shared_ptr<util::CompletableFuture<ChunkAccess*>> applyStep(
        GenerationChunkHolder* chunkHolder,
        const world::chunk::status::ChunkStep& step,
        util::StaticCache2D<GenerationChunkHolder*>& cache
    ) = 0;

    /**
     * Schedule a generation task
     * Reference: GeneratingChunkMap.java - scheduleGenerationTask
     *
     * Creates and schedules a new ChunkGenerationTask.
     * Returns shared_ptr for Java-like GC semantics (lambdas keep tasks alive).
     *
     * @param targetStatus The status to generate to
     * @param pos The chunk position
     * @return The scheduled task
     */
    virtual std::shared_ptr<ChunkGenerationTask> scheduleGenerationTask(
        const world::chunk::status::ChunkStatus& targetStatus,
        const world::ChunkPos& pos
    ) = 0;

    /**
     * Run pending generation tasks
     * Reference: GeneratingChunkMap.java - runGenerationTasks
     */
    virtual void runGenerationTasks() = 0;
};

} // namespace level
} // namespace server
} // namespace minecraft

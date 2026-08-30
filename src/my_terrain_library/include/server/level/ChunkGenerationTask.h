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
#include <string>

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
    FutureType runUntilWait();
    // Drop every generation ref this task holds (normally done by runUntilWait on completion).
    void releaseClaim();

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
    std::string debugString() const {
        std::string s = "runs=" + std::to_string(m_runs.load()) + " target=" + m_targetStatus.getName();
        s += " scheduled=" + (m_scheduledStatus ? m_scheduledStatus->getName() : std::string("none"));
        s += " layer=" + std::to_string(m_scheduledLayer.size());
        if (!m_scheduledLayerInfo.empty()) {
            world::ChunkPos w(m_scheduledLayerInfo.back().first);
            s += " waitsOn=" + w.toString() + "@" + std::to_string(m_scheduledLayerInfo.back().second);
        }
        if (!m_scheduledLayer.empty()) s += m_scheduledLayer.back()->isDone() ? "(done)" : "(waiting)";
        s += m_markedForCancellation.load() ? " cancelled" : "";
        return s;
    }

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
    FutureType waitForScheduledLayer();

    GeneratingChunkMap* m_chunkMap;
    world::ChunkPos m_pos;
    const world::chunk::status::ChunkStatus* m_scheduledStatus{nullptr};
    const world::chunk::status::ChunkStatus& m_targetStatus;
    std::atomic<int> m_runs{0};   // diagnostics: how often runUntilWait ran
public:
    void noteRun() { m_runs.fetch_add(1, std::memory_order_relaxed); }
    int runs() const { return m_runs.load(); }
private:
    std::atomic<bool> m_markedForCancellation{false};
    // Set by the first runUntilWait (started) and by a cancellation of a task
    // that never started; whoever flips it first owns releaseClaim.
    std::atomic<bool> m_started{false};
    std::atomic<bool> m_claimReleased{false};
    std::vector<FutureType> m_scheduledLayer;
    // Parallel to m_scheduledLayer: which (chunk key, status index) each
    // pending future belongs to — diagnostics for stuck-task chains.
    std::vector<std::pair<int64_t, int>> m_scheduledLayerInfo;
public:
    // Key/status the task is currently blocked on (last pending layer future), or {0,-1}.
    std::pair<int64_t, int> waitingOn() const {
        return m_scheduledLayerInfo.empty() ? std::make_pair(int64_t(0), -1) : m_scheduledLayerInfo.back();
    }
private:
    util::StaticCache2D<GenerationChunkHolder*> m_cache;
    bool m_needsGeneration{false};
};

} // namespace level
} // namespace server
} // namespace minecraft

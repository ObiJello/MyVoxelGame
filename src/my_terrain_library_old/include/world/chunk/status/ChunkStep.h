#pragma once

#include "world/chunk/status/ChunkStatus.h"
#include "world/chunk/status/ChunkDependencies.h"
#include "world/chunk/status/WorldGenContext.h"
#include "world/IChunk.h"
#include "levelgen/Blender.h"
#include "util/CompletableFuture.h"
#include <vector>
#include <functional>
#include <memory>
#include <array>
#include <algorithm>

// Reference: net/minecraft/world/level/chunk/status/ChunkStep.java

namespace minecraft {

// Forward declarations
namespace levelgen {
    class ChunkGenerator;
    class RandomState;
}

namespace world {
namespace chunk {
namespace status {

// Forward declaration
class ChunkStep;
struct WorldGenContext;

/**
 * ChunkStatusTask - Function type for generation tasks
 * Reference: ChunkStatusTasks.java method signatures
 *
 * @param context - World generation context
 * @param step - Current chunk step
 * @param chunks - 2D cache of neighboring chunks
 * @param chunk - The center chunk to process
 * @return A CompletableFuture that completes with the processed chunk
 *
 * Note: Most tasks return CompletableFuture::completed() for synchronous work.
 * The NOISE task uses supplyAsync() for parallel chunk generation.
 * Reference: Java returns CompletableFuture<ChunkAccess>
 */
using ChunkStatusTask = std::function<std::shared_ptr<util::CompletableFuture<::world::IChunk*>>(
    WorldGenContext& context,
    const ChunkStep& step,
    const std::vector<std::vector<::world::IChunk*>>& chunks,
    ::world::IChunk* chunk
)>;

// WorldGenContext is defined in WorldGenContext.h
// Forward declare here for use in ChunkStatusTask
// Include WorldGenContext.h where the full definition is needed

/**
 * ChunkStep - Represents a single step in chunk generation with dependencies
 *
 * Each step:
 * - Targets a specific ChunkStatus
 * - Has direct dependencies (what neighboring chunks need at this step)
 * - Has accumulated dependencies (considering parent steps)
 * - Has a task to execute
 *
 * Reference: ChunkStep.java
 */
class ChunkStep {
private:
    const ChunkStatus* m_targetStatus;
    ChunkDependencies m_directDependencies;
    ChunkDependencies m_accumulatedDependencies;
    int32_t m_blockStateWriteRadius;
    ChunkStatusTask m_task;

public:
    /**
     * Constructor
     * Reference: ChunkStep.java line 14 (record definition)
     */
    ChunkStep(
        const ChunkStatus* targetStatus,
        ChunkDependencies directDependencies,
        ChunkDependencies accumulatedDependencies,
        int32_t blockStateWriteRadius,
        ChunkStatusTask task
    )
        : m_targetStatus(targetStatus)
        , m_directDependencies(std::move(directDependencies))
        , m_accumulatedDependencies(std::move(accumulatedDependencies))
        , m_blockStateWriteRadius(blockStateWriteRadius)
        , m_task(std::move(task))
    {
    }

    /**
     * Get the target status for this step
     */
    const ChunkStatus& targetStatus() const { return *m_targetStatus; }

    /**
     * Get direct dependencies
     */
    const ChunkDependencies& directDependencies() const { return m_directDependencies; }

    /**
     * Get accumulated dependencies
     */
    const ChunkDependencies& accumulatedDependencies() const { return m_accumulatedDependencies; }

    /**
     * Get block state write radius
     */
    int32_t blockStateWriteRadius() const { return m_blockStateWriteRadius; }

    /**
     * Get the task function
     */
    const ChunkStatusTask& task() const { return m_task; }

    /**
     * Get the accumulated radius for a given status
     * Reference: ChunkStep.java lines 15-17
     *
     * @param status - The status to check
     * @return 0 if status equals target, otherwise accumulated radius
     */
    int32_t getAccumulatedRadiusOf(const ChunkStatus& status) const {
        if (status == *m_targetStatus) {
            return 0;
        }
        return m_accumulatedDependencies.getRadiusOf(status);
    }

    /**
     * Apply this step to a chunk
     * Reference: ChunkStep.java lines 19-26
     *
     * @param context - World generation context
     * @param cache - 2D grid of neighboring chunks
     * @param chunk - The center chunk
     * @return A future that completes with the processed chunk
     */
    std::shared_ptr<util::CompletableFuture<::world::IChunk*>> apply(
        WorldGenContext& context,
        const std::vector<std::vector<::world::IChunk*>>& cache,
        ::world::IChunk* chunk
    ) const;
};

/**
 * ChunkStep::Builder - Builder pattern for constructing ChunkSteps
 * Reference: ChunkStep.java lines 42-137
 */
class ChunkStepBuilder {
private:
    const ChunkStatus* m_status;
    const ChunkStep* m_parent;
    std::vector<const ChunkStatus*> m_directDependenciesByRadius;
    int32_t m_blockStateWriteRadius;
    ChunkStatusTask m_task;

    /**
     * Get radius at which parent status is required
     * Reference: ChunkStep.java lines 128-136
     */
    int32_t getRadiusOfParent(const ChunkStatus& status) const {
        for (int32_t i = static_cast<int32_t>(m_directDependenciesByRadius.size()) - 1; i >= 0; --i) {
            if (m_directDependenciesByRadius[i]->isOrAfter(status)) {
                return i;
            }
        }
        return 0;
    }

    /**
     * Build accumulated dependencies considering parent chain
     * Reference: ChunkStep.java lines 103-126
     */
    std::vector<const ChunkStatus*> buildAccumulatedDependencies() const {
        if (m_parent == nullptr) {
            return m_directDependenciesByRadius;
        }

        int32_t radiusOfParent = getRadiusOfParent(m_parent->targetStatus());
        const ChunkDependencies& parentDeps = m_parent->accumulatedDependencies();

        int32_t newSize = std::max(
            radiusOfParent + parentDeps.size(),
            static_cast<int32_t>(m_directDependenciesByRadius.size())
        );

        std::vector<const ChunkStatus*> accumulated(newSize);

        for (int32_t distance = 0; distance < newSize; ++distance) {
            int32_t distanceInParent = distance - radiusOfParent;

            if (distanceInParent >= 0 && distanceInParent < parentDeps.size()) {
                if (distance >= static_cast<int32_t>(m_directDependenciesByRadius.size())) {
                    accumulated[distance] = &parentDeps.get(distanceInParent);
                } else {
                    accumulated[distance] = &ChunkStatus::max(
                        *m_directDependenciesByRadius[distance],
                        parentDeps.get(distanceInParent)
                    );
                }
            } else if (distance < static_cast<int32_t>(m_directDependenciesByRadius.size())) {
                accumulated[distance] = m_directDependenciesByRadius[distance];
            }
        }

        return accumulated;
    }

public:
    /**
     * Constructor for first status (EMPTY)
     * Reference: ChunkStep.java lines 49-57
     */
    explicit ChunkStepBuilder(const ChunkStatus& status)
        : m_status(&status)
        , m_parent(nullptr)
        , m_blockStateWriteRadius(-1)
        , m_task(nullptr)
    {
        if (&status.getParent() != &status) {
            throw std::invalid_argument("Not starting with the first status");
        }
    }

    /**
     * Constructor for subsequent statuses
     * Reference: ChunkStep.java lines 59-67
     */
    ChunkStepBuilder(const ChunkStatus& status, const ChunkStep& parent)
        : m_status(&status)
        , m_parent(&parent)
        , m_directDependenciesByRadius{&parent.targetStatus()}
        , m_blockStateWriteRadius(-1)
        , m_task(nullptr)
    {
        if (parent.targetStatus().getIndex() != status.getIndex() - 1) {
            throw std::invalid_argument("Out of order status");
        }
    }

    /**
     * Add a dependency requirement at a specific radius
     * Reference: ChunkStep.java lines 69-86
     *
     * @param status - Required status for neighboring chunks
     * @param radius - Maximum distance at which this applies
     */
    ChunkStepBuilder& addRequirement(const ChunkStatus& status, int32_t radius) {
        if (status.isOrAfter(*m_status)) {
            throw std::invalid_argument("Status cannot require itself or later status");
        }

        int32_t newLength = radius + 1;
        if (newLength > static_cast<int32_t>(m_directDependenciesByRadius.size())) {
            // Expand and fill with the new status
            std::vector<const ChunkStatus*> previous = std::move(m_directDependenciesByRadius);
            m_directDependenciesByRadius.resize(newLength, &status);

            // Merge with previous requirements
            for (int32_t i = 0; i < std::min(newLength, static_cast<int32_t>(previous.size())); ++i) {
                m_directDependenciesByRadius[i] = &ChunkStatus::max(*previous[i], status);
            }
        } else {
            // Just update existing entries
            for (int32_t i = 0; i < newLength; ++i) {
                m_directDependenciesByRadius[i] = &ChunkStatus::max(
                    *m_directDependenciesByRadius[i], status
                );
            }
        }

        return *this;
    }

    /**
     * Set the block state write radius
     * Reference: ChunkStep.java lines 88-91
     */
    ChunkStepBuilder& setBlockStateWriteRadius(int32_t radius) {
        m_blockStateWriteRadius = radius;
        return *this;
    }

    /**
     * Set the task function
     * Reference: ChunkStep.java lines 93-96
     */
    ChunkStepBuilder& setTask(ChunkStatusTask task) {
        m_task = std::move(task);
        return *this;
    }

    /**
     * Build the ChunkStep
     * Reference: ChunkStep.java lines 98-101
     */
    ChunkStep build() const {
        return ChunkStep(
            m_status,
            ChunkDependencies(m_directDependenciesByRadius),
            ChunkDependencies(buildAccumulatedDependencies()),
            m_blockStateWriteRadius,
            m_task
        );
    }
};

/**
 * ChunkPipeline - Manages the complete generation pipeline
 * Contains all ChunkSteps in order
 */
class ChunkPipeline {
private:
    std::vector<ChunkStep> m_steps;

public:
    /**
     * Create the default Minecraft generation pipeline
     * Call after static initialization
     */
    static ChunkPipeline createDefault();

    /**
     * Get step for a status
     */
    const ChunkStep& getStep(const ChunkStatus& status) const {
        return m_steps[status.getIndex()];
    }

    /**
     * Get step by index
     */
    const ChunkStep& getStep(int32_t index) const {
        return m_steps[index];
    }

    /**
     * Get all steps
     */
    const std::vector<ChunkStep>& getSteps() const {
        return m_steps;
    }

    /**
     * Add a step to the pipeline
     */
    void addStep(ChunkStep step) {
        m_steps.push_back(std::move(step));
    }
};

} // namespace status
} // namespace chunk
} // namespace world
} // namespace minecraft

#pragma once

#include "world/chunk/status/ChunkStatus.h"
#include "world/chunk/status/ChunkStep.h"
#include "world/chunk/status/ChunkStatusTasks.h"
#include <vector>
#include <functional>

// Reference: net/minecraft/world/level/chunk/status/ChunkPyramid.java

namespace minecraft {
namespace world {
namespace chunk {
namespace status {

/**
 * ChunkPyramid - Defines the generation/loading pipeline as a series of steps
 * Reference: ChunkPyramid.java
 *
 * Contains two static pyramids:
 * - GENERATION_PYRAMID: For generating new chunks (includes all tasks)
 * - LOADING_PYRAMID: For loading existing chunks (mostly passthroughs)
 */
class ChunkPyramid {
public:
    /**
     * The two global pyramids
     * Reference: ChunkPyramid.java lines 9-10
     */
    static const ChunkPyramid& getGenerationPyramid();
    static const ChunkPyramid& getLoadingPyramid();

    /**
     * Initialize the static pyramids
     * Must be called after ChunkStatus is initialized
     */
    static void initialize();

    /**
     * Check if initialized
     */
    static bool isInitialized();

    /**
     * Get the step to reach a given status
     * Reference: ChunkPyramid.java lines 12-14
     */
    const ChunkStep& getStepTo(const ChunkStatus& status) const;

    /**
     * Get all steps
     */
    const std::vector<ChunkStep>& getSteps() const { return m_steps; }

    /**
     * Builder class for constructing pyramids
     * Reference: ChunkPyramid.java lines 21-39
     */
    class Builder {
    public:
        Builder() = default;

        /**
         * Add a step for a status
         * Reference: ChunkPyramid.java lines 28-38
         */
        Builder& step(
            const ChunkStatus& status,
            std::function<ChunkStepBuilder&(ChunkStepBuilder&)> configurator
        );

        /**
         * Build the pyramid
         * Reference: ChunkPyramid.java lines 24-26
         */
        ChunkPyramid build();

    private:
        std::vector<ChunkStep> m_steps;
    };

private:
    explicit ChunkPyramid(std::vector<ChunkStep> steps);

    std::vector<ChunkStep> m_steps;

    // Static instances
    static bool s_initialized;
    static ChunkPyramid s_generationPyramid;
    static ChunkPyramid s_loadingPyramid;
};

} // namespace status
} // namespace chunk
} // namespace world
} // namespace minecraft

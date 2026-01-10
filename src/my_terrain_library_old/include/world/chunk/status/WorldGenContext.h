#pragma once

#include <functional>
#include <cstdint>

// Reference: net/minecraft/world/level/chunk/status/WorldGenContext.java

// Forward declarations
namespace minecraft {
    namespace levelgen {
        class ChunkGenerator;
        class RandomState;
    }
}

namespace minecraft {
namespace world {
namespace chunk {
namespace status {

/**
 * Executor type - function that takes a task and schedules it for execution
 * Reference: Java's Executor interface
 */
using Executor = std::function<void(std::function<void()>)>;

/**
 * WorldGenContext - Context for world generation operations
 * Reference: WorldGenContext.java
 *
 * This record-like struct contains all the context needed for chunk generation:
 * - level: The server level being generated
 * - generator: The chunk generator
 * - structureManager: Manager for structure templates
 * - lightEngine: The threaded light engine
 * - mainThreadExecutor: Executor for main thread tasks
 * - backgroundExecutor: Executor for background thread pool (like Util.backgroundExecutor())
 * - unsavedListener: Callback for marking chunks unsaved
 */
struct WorldGenContext {
    // Core generation components
    levelgen::ChunkGenerator* generator = nullptr;
    levelgen::RandomState* randomState = nullptr;
    int64_t seed = 0;

    // Server-level components (placeholders for future implementation)
    void* level = nullptr;                     // ServerLevel*
    void* structureManager = nullptr;          // StructureTemplateManager*
    void* lightEngine = nullptr;               // ThreadedLevelLightEngine*

    // Executors and callbacks
    Executor mainThreadExecutor;
    Executor backgroundExecutor;  // For async operations like noise generation
    std::function<void(int, int)> unsavedListener;  // Called with chunk x, z when unsaved

    WorldGenContext() = default;

    WorldGenContext(
        levelgen::ChunkGenerator* gen,
        levelgen::RandomState* rs,
        int64_t s
    )
        : generator(gen)
        , randomState(rs)
        , seed(s)
    {}

    WorldGenContext(
        levelgen::ChunkGenerator* gen,
        levelgen::RandomState* rs,
        int64_t s,
        Executor bgExecutor
    )
        : generator(gen)
        , randomState(rs)
        , seed(s)
        , backgroundExecutor(std::move(bgExecutor))
    {}

    WorldGenContext(
        void* lvl,
        levelgen::ChunkGenerator* gen,
        void* structMgr,
        void* lightEng,
        Executor mainExec,
        Executor bgExec,
        std::function<void(int, int)> unsavedCb
    )
        : generator(gen)
        , level(lvl)
        , structureManager(structMgr)
        , lightEngine(lightEng)
        , mainThreadExecutor(std::move(mainExec))
        , backgroundExecutor(std::move(bgExec))
        , unsavedListener(std::move(unsavedCb))
    {}
};

} // namespace status
} // namespace chunk
} // namespace world
} // namespace minecraft

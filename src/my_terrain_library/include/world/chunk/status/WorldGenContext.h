#pragma once

#include <functional>
#include <atomic>
#include <memory>
#include <cstdlib>
#include <vector>
#include <condition_variable>
#include <mutex>
#include <cstdint>

// Reference: net/minecraft/world/level/chunk/status/WorldGenContext.java

// Forward declarations
namespace minecraft {
    namespace levelgen {
        class ChunkGenerator;
        class RandomState;
        namespace structure {
            class ChunkGeneratorStructureState;
        }
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
// Spatial exclusion for feature decoration run off the worldgen lane (see
// ChunkStatusTasks::generateFeatures). A features step writes only its 3x3
// neighbourhood (blockStateWriteRadius 1), so two steps whose centres are >= 3
// chunks apart touch disjoint blocks; centres within Chebyshev distance 2
// must not run at the same time.
struct FeatureClaims {
    std::mutex mutex;
    std::vector<std::pair<int, int>> running;
    // Tasks that found a conflict. They are re-submitted (through `submit`)
    // when a claim they conflicted with is released — never by spinning:
    // an immediate re-queue turned into a retry storm that ate the pool's
    // CPU whenever neighbours clustered in the queue (measured 2026-08-30:
    // per-stage times 2-3x, run-to-run bimodal generation speed).
    struct Waiter { int x, z; std::function<void()> retry; };
    std::vector<Waiter> waiters;
    std::atomic<size_t> retries{0};   // diagnostics

    bool conflictsLocked(int x, int z) const {
        for (const auto& [cx, cz] : running) {
            if (std::abs(cx - x) <= 2 && std::abs(cz - z) <= 2) return true;
        }
        return false;
    }
    // Acquire, or park `retry` to be re-submitted later. Returns true when acquired.
    bool acquireOrWait(int x, int z, std::function<void()> retry) {
        std::lock_guard<std::mutex> lock(mutex);
        if (conflictsLocked(x, z)) {
            waiters.push_back(Waiter{x, z, std::move(retry)});
            retries.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        running.emplace_back(x, z);
        return true;
    }
    void release(int x, int z, const std::function<void(std::function<void()>)>& submit) {
        std::vector<std::function<void()>> wake;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (size_t i = 0; i < running.size(); ++i) {
                if (running[i].first == x && running[i].second == z) {
                    running[i] = running.back(); running.pop_back(); break;
                }
            }
            // Wake every waiter that no longer conflicts with what is running.
            for (size_t i = 0; i < waiters.size();) {
                if (!conflictsLocked(waiters[i].x, waiters[i].z)) {
                    wake.push_back(std::move(waiters[i].retry));
                    waiters[i] = std::move(waiters.back()); waiters.pop_back();
                } else {
                    ++i;
                }
            }
        }
        for (auto& fn : wake) submit(std::move(fn));
    }
};

struct WorldGenContext {
    // Core generation components
    levelgen::ChunkGenerator* generator = nullptr;
    levelgen::RandomState* randomState = nullptr;
    int64_t seed = 0;

    // Structure placement/start state (non-owning). nullptr = structures
    // disabled: generateStructureStarts/References stay no-ops, exactly the
    // historical structures-off behavior.
    levelgen::structure::ChunkGeneratorStructureState* structureState = nullptr;

    // Server-level components (placeholders for future implementation)
    void* level = nullptr;                     // ServerLevel*
    void* structureManager = nullptr;          // StructureTemplateManager*
    void* lightEngine = nullptr;               // ThreadedLevelLightEngine*

    // Executors and callbacks
    Executor mainThreadExecutor;
    Executor backgroundExecutor;  // For async operations like noise generation
    std::shared_ptr<FeatureClaims> featureClaims;   // null = run features on the lane
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

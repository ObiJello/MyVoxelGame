// File: src/server/world/MyTerrainGenerator.hpp
#pragma once

#include "common/world/gen/IChunkGenerator.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/core/Log.hpp"

// Terrain library includes
#include "levelgen/NoiseRegistry.h"
#include "levelgen/DensityFunctionRegistry.h"
#include "levelgen/RandomState.h"
#include "levelgen/NoiseGeneratorSettings.h"
#include "levelgen/NoiseRouterData.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/FluidPicker.h"
#include "levelgen/SurfaceSystem.h"
#include "levelgen/SurfaceRuleData.h"
#include "levelgen/placement/PlacedFeature.h"
#include "world/ProtoChunk.h"
#include "world/level/block/Blocks.h"
#include "world/biome/MultiNoiseBiomeSource.h"
#include "world/chunk/status/ChunkStatus.h"

// Server-level includes (the async pipeline)
#include "server/level/ServerChunkCache.h"

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <functional>

namespace Game {

    /**
     * Background thread pool executor - simulates Minecraft's Util.backgroundExecutor()
     * Reference: Minecraft uses ForkJoinPool.commonPool() for background work
     */
    class BackgroundExecutor {
    public:
        using Task = std::function<void()>;

        BackgroundExecutor(size_t numThreads = std::thread::hardware_concurrency())
            : m_running(true)
        {
            for (size_t i = 0; i < numThreads; ++i) {
                m_workers.emplace_back([this]() { workerLoop(); });
            }
        }

        ~BackgroundExecutor() { shutdown(); }

        void shutdown() {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_running = false;
            }
            m_cv.notify_all();
            for (auto& worker : m_workers) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
        }

        void submit(Task task) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_tasks.push(std::move(task));
            }
            m_cv.notify_one();
        }

        std::function<void(std::function<void()>)> getExecutor() {
            return [this](std::function<void()> task) {
                this->submit(std::move(task));
            };
        }

    private:
        void workerLoop() {
            while (true) {
                Task task;
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_cv.wait(lock, [this]() { return !m_running || !m_tasks.empty(); });
                    if (!m_running && m_tasks.empty()) return;
                    if (!m_tasks.empty()) {
                        task = std::move(m_tasks.front());
                        m_tasks.pop();
                    }
                }
                if (task) {
                    try { task(); } catch (...) {}
                }
            }
        }

        std::vector<std::thread> m_workers;
        std::queue<Task> m_tasks;
        std::mutex m_mutex;
        std::condition_variable m_cv;
        std::atomic<bool> m_running;
    };

    /**
     * Main thread executor for tasks that must run on the main thread
     */
    class MainThreadExecutor {
    public:
        using Task = std::function<void()>;

        void submit(Task task) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_tasks.push(std::move(task));
        }

        void runPendingTasks() {
            std::queue<Task> tasksToRun;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                std::swap(tasksToRun, m_tasks);
            }
            while (!tasksToRun.empty()) {
                tasksToRun.front()();
                tasksToRun.pop();
            }
        }

        bool hasPendingTasks() {
            std::lock_guard<std::mutex> lock(m_mutex);
            return !m_tasks.empty();
        }

        std::function<void(std::function<void()>)> getExecutor() {
            return [this](std::function<void()> task) {
                this->submit(std::move(task));
            };
        }

    private:
        std::queue<Task> m_tasks;
        std::mutex m_mutex;
    };

    /**
     * Wrapper generator that integrates the terrain library with the game's
     * IChunkGenerator interface using the FULL ServerChunkCache async pipeline.
     *
     * Pipeline: ServerChunkCache -> ChunkMap -> DistanceManager ->
     *           ChunkGenerationTask -> Worker Threads
     *
     * This matches the exact same flow as Minecraft's DedicatedServer and
     * the async_chunk_test parity test.
     */
    class MyTerrainGenerator : public IChunkGenerator {
    public:
        explicit MyTerrainGenerator(const GenerationConfig& config);
        ~MyTerrainGenerator() override;

        // IChunkGenerator interface implementation
        bool Initialize() override;
        void Shutdown() override;
        ChunkGenerationResult GenerateChunk(Math::ChunkPos position) override;
        std::future<ChunkGenerationResult> GenerateChunkAsync(Math::ChunkPos position) override;
        std::vector<int> GenerateHeightMap(Math::ChunkPos position) override;
        std::string GenerateBiome(Math::ChunkPos position) override;

        void SetConfig(const GenerationConfig& config) override;
        GenerationConfig GetConfig() const override;
        void SetSeed(int32_t seed) override;
        int32_t GetSeed() const override;
        void SetWorldType(const std::string& worldType) override;
        std::string GetWorldType() const override;

        void SetPassEnabled(GenerationPass pass, bool enabled) override;
        bool IsPassEnabled(GenerationPass pass) const override;
        ChunkGenerationResult GenerateWithPasses(Math::ChunkPos position, const std::vector<GenerationPass>& passes) override;

        bool IsReady() const override;

        GeneratorStats GetStats() const override;
        void ResetStats() override;
        void SetMaxGenerationTime(float maxTimeMs) override;
        float GetMaxGenerationTime() const override;

        void RegisterTerrainFunction(const std::string& name, TerrainFunction func) override;
        void RegisterFeatureFunction(const std::string& name, FeatureFunction func) override;
        void SetTerrainFunction(const std::string& name) override;
        void AddFeatureFunction(const std::string& name) override;

        DebugInfo GetDebugInfo(Math::ChunkPos position) override;
        void SetDebugMode(bool enabled) override;
        bool IsDebugMode() const override;

        std::string GetLastError() const override;
        void ClearErrors() override;

    private:
        GenerationConfig m_config;
        GeneratorStats m_stats;
        bool m_initialized = false;

        // Terrain library components
        minecraft::levelgen::NoiseGeneratorSettings* m_settings = nullptr;
        minecraft::levelgen::RandomState* m_randomState = nullptr;
        minecraft::levelgen::FluidPicker* m_fluidPicker = nullptr;
        minecraft::levelgen::NoiseBasedChunkGenerator* m_generator = nullptr;
        std::unique_ptr<minecraft::world::biome::MultiNoiseBiomeSource> m_biomeSource;
        minecraft::world::BlockRegistry* m_blockRegistry = nullptr;
        minecraft::BlockState* m_airBlock = nullptr;
        minecraft::BlockState* m_stoneBlock = nullptr;

        // Full async pipeline components (same as async_chunk_test)
        std::unique_ptr<BackgroundExecutor> m_backgroundExecutor;
        std::unique_ptr<MainThreadExecutor> m_mainThreadExecutor;
        std::unique_ptr<minecraft::server::level::ServerChunkCache> m_chunkCache;

        // Target chunk status for generation
        const minecraft::world::chunk::status::ChunkStatus* m_targetStatus = nullptr;

        // Helper to map block types to game BlockIDs
        BlockID MapBlockType(minecraft::world::BlockState* blockState) const;
    };

} // namespace Game

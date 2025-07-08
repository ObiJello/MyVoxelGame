// File: src/engine/world/ChunkProvider.cpp (Updated for Layered Meshing)
#include "ChunkProvider.hpp"
#include "Log.hpp"
#include <memory>
#include <algorithm>
#include <shared_mutex>
#include "JobSystem.hpp"
#include <unordered_set>

#include "MinecraftChunkLoader.hpp"
#include "SectionDataUnpacker.hpp"

namespace Game {

    // Single global noise generator (kept for fallback generation)
    static FastNoiseLite s_noise;

    // Thread-safe chunk registry - now accessible from other files
    std::unordered_map<uint64_t, std::unique_ptr<ChunkData>> s_chunkRegistry;
    std::shared_mutex s_registryMutex;

    // Neighbor offset table for 4-directional neighbors (X, Z)
    static constexpr std::array<std::pair<int, int>, 4> NEIGHBOR_OFFSETS = {{
        {-1,  0}, // West
        { 1,  0}, // East
        { 0, -1}, // North
        { 0,  1}  // South
    }};

    // Helper function to create a unique key from chunk coordinates
    static uint64_t MakeChunkKey(int32_t x, int32_t z) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) |
               static_cast<uint32_t>(z);
    }

    // Helper function to get chunk coordinates from key
    static Math::ChunkPos KeyToChunkPos(uint64_t key) {
        int32_t x = static_cast<int32_t>(key >> 32);
        int32_t z = static_cast<int32_t>(key & 0xFFFFFFFF);
        return {x, z};
    }

    // Thread-safe neighbor lookup with bounds checking
    static std::shared_ptr<Chunk> GetNeighborChunk(Math::ChunkPos pos, int dx, int dz) {
        uint64_t key = MakeChunkKey(pos.x + dx, pos.z + dz);

        std::shared_lock<std::shared_mutex> lock(s_registryMutex);
        auto it = s_chunkRegistry.find(key);
        if (it != s_chunkRegistry.end() && it->second->isGenerated.load()) {
            return it->second->chunk;
        }
        return nullptr;
    }

    // Forward declarations for enhanced meshing
    static void LayeredMeshingJob(std::shared_ptr<Chunk> chunk, Math::ChunkPos pos);
    static void StandardLayeredMeshingJob(std::shared_ptr<Chunk> chunk, Math::ChunkPos pos);

    // Create neighbor context for inter-chunk meshing
    static NeighborContext CreateNeighborContext(std::shared_ptr<Chunk> centerChunk, Math::ChunkPos pos) {
        NeighborContext ctx(centerChunk);
        ctx.hasAllNeighbors = true;

        for (size_t i = 0; i < 4; ++i) {
            auto [dx, dz] = NEIGHBOR_OFFSETS[i];
            ctx.neighbors[i] = GetNeighborChunk(pos, dx, dz);
            if (!ctx.neighbors[i]) {
                ctx.hasAllNeighbors = false;
            }
        }

        return ctx;
    }

    // Register dependency relationships between chunks
    static void RegisterDependency(uint64_t sourceKey, uint64_t dependentKey) {
        std::unique_lock<std::shared_mutex> lock(s_registryMutex);
        auto sourceIt = s_chunkRegistry.find(sourceKey);
        if (sourceIt != s_chunkRegistry.end()) {
            sourceIt->second->dependents.insert(dependentKey);
        }
    }

    // Update neighbor counts and trigger remeshing when appropriate
    static void UpdateNeighborCounts(Math::ChunkPos pos) {
        uint64_t centerKey = MakeChunkKey(pos.x, pos.z);

        std::unique_lock<std::shared_mutex> lock(s_registryMutex);
        auto centerIt = s_chunkRegistry.find(centerKey);
        if (centerIt == s_chunkRegistry.end() || !centerIt->second->isGenerated.load()) {
            return;
        }

        // Count available neighbors for center chunk
        int neighborCount = 0;
        for (auto [dx, dz] : NEIGHBOR_OFFSETS) {
            uint64_t neighborKey = MakeChunkKey(pos.x + dx, pos.z + dz);
            auto neighborIt = s_chunkRegistry.find(neighborKey);
            if (neighborIt != s_chunkRegistry.end() && neighborIt->second->isGenerated.load()) {
                neighborCount++;
                // Register bidirectional dependencies
                centerIt->second->dependents.insert(neighborKey);
                neighborIt->second->dependents.insert(centerKey);
            }
        }

        centerIt->second->neighborCount.store(neighborCount);

        // If we have all neighbors and no pending mesh, trigger layered meshing
        if (neighborCount == 4 && !centerIt->second->hasPendingMesh.exchange(true)) {
            auto chunk = centerIt->second->chunk;

            Log::Debug("Triggering layered inter-chunk meshing for chunk (%d, %d) with all 4 neighbors",
                      pos.x, pos.z);

            // Schedule enhanced layered meshing job
            JobSystem::g_ThreadPool.Enqueue([chunk, pos]() {
                LayeredMeshingJob(chunk, pos);
            });
        }
    }

    // ENHANCED: Layered meshing job with full inter-chunk face culling and fluid support
    static void LayeredMeshingJob(std::shared_ptr<Chunk> chunk, Math::ChunkPos pos) {
        Log::Debug("Starting layered inter-chunk meshing for chunk (%d, %d)", pos.x, pos.z);

        // Create neighbor context
        NeighborContext ctx = CreateNeighborContext(chunk, pos);

        if (!ctx.hasAllNeighbors) {
            Log::Warning("Layered meshing called without all neighbors for chunk (%d, %d)", pos.x, pos.z);
            // Fall back to standard layered meshing without inter-chunk culling
            StandardLayeredMeshingJob(chunk, pos);
            return;
        }

        int totalMeshJobs = 0;

        // Process each non-empty section with neighbor context using LAYERED meshing
        for (int s = 0; s < Math::SECTIONS_PER_CHUNK; ++s) {
            if (!chunk->sections[s]) {
                continue; // Skip empty sections
            }

            // ENHANCED: Allocate LayeredMeshData for this section
            auto* layeredMeshData = new LayeredMeshData();
            layeredMeshData->chunkXZ = { pos.x, pos.z };
            layeredMeshData->sectionIndex = s;

            ChunkSection* sectionPtr = chunk->sections[s].get();

            Log::Debug("Enqueueing layered mesher for chunk (%d, %d) section %d",
                      pos.x, pos.z, s);

            // ENHANCED: Layered meshing job with neighbor context and fluid support
            JobSystem::g_ThreadPool.Enqueue([sectionPtr, layeredMeshData, ctx, pos, s]() {
                try {
                    Log::Debug("Executing layered mesher for chunk (%d, %d) section %d",
                              pos.x, pos.z, s);

                    // Call the enhanced layered mesher with neighbor context
                    LayeredMesherJob(sectionPtr, layeredMeshData, ctx);

                    Log::Debug("Layered mesher completed for chunk (%d, %d) section %d with "
                              "opaque: %zu verts, cutout: %zu verts, translucent: %zu verts",
                              pos.x, pos.z, s,
                              layeredMeshData->opaqueVertices.size(),
                              layeredMeshData->cutoutVertices.size(),
                              layeredMeshData->translucentVertices.size());

                } catch (const std::exception& e) {
                    Log::Error("Layered mesher failed for chunk (%d, %d) section %d: %s",
                              pos.x, pos.z, s, e.what());
                    delete layeredMeshData;
                } catch (...) {
                    Log::Error("Layered mesher failed for chunk (%d, %d) section %d with unknown exception",
                              pos.x, pos.z, s);
                    delete layeredMeshData;
                }
            });

            totalMeshJobs++;
        }

        Log::Info("Layered meshing for chunk (%d, %d) complete → %d layered meshing jobs enqueued",
                 pos.x, pos.z, totalMeshJobs);

        // Mark mesh as no longer pending
        {
            std::shared_lock<std::shared_mutex> lock(s_registryMutex);
            uint64_t key = MakeChunkKey(pos.x, pos.z);
            auto it = s_chunkRegistry.find(key);
            if (it != s_chunkRegistry.end()) {
                it->second->hasPendingMesh.store(false);
            }
        }
    }

    // ENHANCED: Standard layered meshing fallback for chunks without all neighbors
    static void StandardLayeredMeshingJob(std::shared_ptr<Chunk> chunk, Math::ChunkPos pos) {
        Log::Debug("Starting standard layered meshing for chunk (%d, %d)", pos.x, pos.z);

        int meshJobsEnqueued = 0;
        for (int s = 0; s < Math::SECTIONS_PER_CHUNK; ++s) {
            if (!chunk->sections[s]) {
                continue;
            }

            // ENHANCED: Use LayeredMeshData instead of MeshData
            auto* layeredMeshData = new LayeredMeshData();
            layeredMeshData->chunkXZ = { pos.x, pos.z };
            layeredMeshData->sectionIndex = s;

            ChunkSection* sectionPtr = chunk->sections[s].get();

            JobSystem::g_ThreadPool.Enqueue([chunk, sectionPtr, layeredMeshData, pos, s]() {
                try {
                    // Create a neighbor context with just the center chunk
                    NeighborContext ctx(chunk);
                    ctx.hasAllNeighbors = false; // Mark that we don't have all neighbors

                    // Use layered meshing even without full neighbor context
                    LayeredMesherJob(sectionPtr, layeredMeshData, ctx);
                } catch (const std::exception& e) {
                    Log::Error("Standard layered mesher failed for chunk (%d, %d) section %d: %s",
                              pos.x, pos.z, s, e.what());
                    delete layeredMeshData;
                } catch (...) {
                    Log::Error("Standard layered mesher failed for chunk (%d, %d) section %d with unknown exception",
                              pos.x, pos.z, s);
                    delete layeredMeshData;
                }
            });

            meshJobsEnqueued++;
        }

        Log::Info("Standard layered meshing for chunk (%d, %d) complete → %d layered meshing jobs enqueued",
                 pos.x, pos.z, meshJobsEnqueued);
    }

    void ChunkProvider::RequestChunk(Math::ChunkPos pos) {
        // Initialize noise generator once (for fallback generation)
        static bool s_initialized = false;
        if (!s_initialized) {
            s_noise.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
            s_noise.SetSeed(1337);
            s_initialized = true;
        }

        uint64_t key = MakeChunkKey(pos.x, pos.z);

        // Check if chunk already exists
        {
            std::shared_lock<std::shared_mutex> lock(s_registryMutex);
            if (s_chunkRegistry.find(key) != s_chunkRegistry.end()) {
                return;
            }
        }

        // Create new chunk data entry
        auto chunkData = std::make_unique<ChunkData>(nullptr); // Will be set later

        {
            std::unique_lock<std::shared_mutex> lock(s_registryMutex);
            s_chunkRegistry[key] = std::move(chunkData);
        }

        Log::Debug("Requesting generation for chunk (%d, %d)", pos.x, pos.z);

        // Enqueue chunk loading job that tries Minecraft data first
        JobSystem::g_ThreadPool.Enqueue([pos, key]() {
            Log::Debug("Starting chunk loading for (%d, %d)", pos.x, pos.z);

            std::shared_ptr<Chunk> chunk = nullptr;

            try {
                // Try loading from Minecraft region files first
                chunk = MinecraftChunkLoader::LoadOrGenerateChunk(pos);

                if (!chunk) {
                    Log::Error("Both Minecraft loading and fallback generation failed for chunk (%d, %d)",
                              pos.x, pos.z);
                    // Remove failed chunk from registry
                    std::unique_lock<std::shared_mutex> lock(s_registryMutex);
                    s_chunkRegistry.erase(key);
                    return;
                }

                // Set chunk position (may have been loaded from different source)
                chunk->pos = pos;

            } catch (const std::exception& e) {
                Log::Error("Exception during chunk loading for (%d, %d): %s", pos.x, pos.z, e.what());

                // Remove failed chunk from registry
                std::unique_lock<std::shared_mutex> lock(s_registryMutex);
                s_chunkRegistry.erase(key);
                return;
            }

            Log::Debug("Chunk loading complete for (%d, %d)", pos.x, pos.z);

            // Update registry with loaded chunk
            {
                std::unique_lock<std::shared_mutex> lock(s_registryMutex);
                auto it = s_chunkRegistry.find(key);
                if (it != s_chunkRegistry.end()) {
                    it->second->chunk = chunk;
                    it->second->isGenerated.store(true);
                }
            }

            // Update neighbor counts for this chunk and its neighbors
            UpdateNeighborCounts(pos);
            for (auto [dx, dz] : NEIGHBOR_OFFSETS) {
                UpdateNeighborCounts({pos.x + dx, pos.z + dz});
            }

            Log::Info("Chunk (%d, %d) loading complete, neighbor relationships updated", pos.x, pos.z);
        });
    }

    void ChunkProvider::UnloadChunk(Math::ChunkPos pos) {
        uint64_t key = MakeChunkKey(pos.x, pos.z);

        std::unique_lock<std::shared_mutex> lock(s_registryMutex);
        auto it = s_chunkRegistry.find(key);
        if (it == s_chunkRegistry.end()) {
            return;
        }

        // Store dependent keys before erasing
        std::vector<uint64_t> dependentKeys;
        for (uint64_t dependentKey : it->second->dependents) {
            dependentKeys.push_back(dependentKey);
        }

        // Remove the chunk from registry FIRST before triggering neighbor updates
        s_chunkRegistry.erase(it);

        Log::Debug("Chunk (%d, %d) unloaded and removed from registry", pos.x, pos.z);
    }

    // Enhanced ChunkProvider functions for Minecraft world support
    void ChunkProvider::SetMinecraftWorldPath(const std::string& worldPath) {
        MinecraftChunkLoader::SetWorldPath(worldPath);
        Log::Info("Set Minecraft world path: %s", worldPath.c_str());

        // Check if the world path exists and has region files
        std::string regionPath = worldPath + "/region";
        if (std::filesystem::exists(regionPath)) {
            Log::Info("Found region directory: %s", regionPath.c_str());
        } else {
            Log::Warning("No region directory found at: %s (will use procedural generation)", regionPath.c_str());
        }
    }

    std::string ChunkProvider::GetMinecraftWorldPath() {
        return MinecraftChunkLoader::GetWorldPath();
    }

    bool ChunkProvider::IsMinecraftChunkAvailable(Math::ChunkPos pos) {
        std::string worldPath = MinecraftChunkLoader::GetWorldPath();
        if (worldPath.empty()) {
            return false;
        }
        return MinecraftChunkLoader::ChunkExistsInRegion(pos, worldPath);
    }

    size_t ChunkProvider::GetLoadedChunkCount() {
        std::shared_lock<std::shared_mutex> lock(s_registryMutex);
        return s_chunkRegistry.size();
    }

    void ChunkProvider::ClearAllChunks() {
        std::unique_lock<std::shared_mutex> lock(s_registryMutex);
        size_t clearedCount = s_chunkRegistry.size();
        s_chunkRegistry.clear();
        Log::Info("Cleared %zu chunks from registry", clearedCount);
    }

} // namespace Game
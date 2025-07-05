// File: src/game/WorldManager.cpp (Fixed Chunk Unloading)
#include "WorldManager.hpp"
#include "ChunkProvider.hpp"
#include "../render/ChunkRenderer.hpp"  // for g_chunkMeshes
#include "Mesher.hpp"
#include "Log.hpp"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <chrono>
#include <glad/glad.h>

#include "JobSystem.hpp"

namespace Game {

    std::unordered_set<Math::ChunkPos, ChunkPosHash> WorldManager::s_loaded;

    // Helper function to create a unique key from chunk coordinates
    static uint64_t MakeChunkKey(int32_t x, int32_t z) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) |
               static_cast<uint32_t>(z);
    }

    // Helper function to get neighbor chunk safely
    static std::shared_ptr<Chunk> GetNeighborChunk(Math::ChunkPos pos, int dx, int dz) {
        uint64_t key = MakeChunkKey(pos.x + dx, pos.z + dz);

        std::shared_lock<std::shared_mutex> lock(s_registryMutex);
        auto it = s_chunkRegistry.find(key);
        if (it != s_chunkRegistry.end() && it->second &&
            it->second->isGenerated.load()) {
            return it->second->chunk;
            }
        return nullptr;
    }

    // Create neighbor context for inter-chunk meshing
    static NeighborContext CreateNeighborContext(std::shared_ptr<Chunk> centerChunk, Math::ChunkPos pos) {
        NeighborContext ctx(centerChunk);
        ctx.hasAllNeighbors = true;

        // Neighbor offset table for 4-directional neighbors (X, Z)
        static constexpr std::array<std::pair<int, int>, 4> NEIGHBOR_OFFSETS = {{
            {-1,  0}, // West
            { 1,  0}, // East
            { 0, -1}, // North
            { 0,  1}  // South
        }};

        for (size_t i = 0; i < 4; ++i) {
            auto [dx, dz] = NEIGHBOR_OFFSETS[i];
            ctx.neighbors[i] = GetNeighborChunk(pos, dx, dz);
            if (!ctx.neighbors[i]) {
                ctx.hasAllNeighbors = false;
            }
        }

        return ctx;
    }

    void WorldManager::Update(const glm::vec3& cameraPos) {
        // 1) Calculate camera chunk position
        int cx = static_cast<int>(std::floor(cameraPos.x / Math::CHUNK_SIZE_X));
        int cz = static_cast<int>(std::floor(cameraPos.z / Math::CHUNK_SIZE_Z));
        Math::ChunkPos cam{cx, cz};

        // 2) Build desired set of chunks based on render radius (FIXED: Square pattern like Minecraft)
        std::unordered_set<Math::ChunkPos, ChunkPosHash> desiredChunks;

        // FIXED: Use square pattern instead of circular
        // For render distance N, we load chunks from (cx-N, cz-N) to (cx+N, cz+N)
        // This creates a (2*N+1) × (2*N+1) square centered on the player
        for (int dz = -RENDER_RADIUS; dz <= RENDER_RADIUS; ++dz) {
            for (int dx = -RENDER_RADIUS; dx <= RENDER_RADIUS; ++dx) {
                // No distance check - load all chunks in the square
                desiredChunks.insert({cx + dx, cz + dz});
            }
        }

        // Log the chunk loading pattern for verification
        static bool hasLoggedPattern = false;
        if (!hasLoggedPattern) {
            int totalChunks = (2 * RENDER_RADIUS + 1) * (2 * RENDER_RADIUS + 1);
            Log::Info("Using square chunk loading pattern: %d×%d grid (%d total chunks) around player",
                     2 * RENDER_RADIUS + 1, 2 * RENDER_RADIUS + 1, totalChunks);
            Log::Info("Render radius %d creates chunks from (%d,%d) to (%d,%d) relative to player",
                     RENDER_RADIUS, -RENDER_RADIUS, -RENDER_RADIUS, RENDER_RADIUS, RENDER_RADIUS);
            hasLoggedPattern = true;
        }

        // 3) Request loading of new chunks
        std::vector<Math::ChunkPos> newChunks;
        for (const auto& pos : desiredChunks) {
            if (s_loaded.find(pos) == s_loaded.end()) {
                newChunks.push_back(pos);
            }
        }

        // Sort new chunks by distance from camera for optimal loading order
        std::sort(newChunks.begin(), newChunks.end(),
            [cx, cz](const Math::ChunkPos& a, const Math::ChunkPos& b) {
                int distA = (a.x - cx) * (a.x - cx) + (a.z - cz) * (a.z - cz);
                int distB = (b.x - cx) * (b.x - cx) + (b.z - cz) * (b.z - cz);
                return distA < distB;
        });

        // Load new chunks (with modest throttling to prevent frame spikes)
        constexpr int MAX_LOADS_PER_FRAME = 8;
        int loadsThisFrame = 0;

        for (const auto& pos : newChunks) {
            if (loadsThisFrame >= MAX_LOADS_PER_FRAME) {
                break; // Defer remaining loads to next frame
            }

            LoadChunk(pos);
            s_loaded.insert(pos);
            loadsThisFrame++;
        }

        // 4) **SIMPLIFIED**: Identify and immediately unload chunks outside render distance
        std::vector<Math::ChunkPos> chunksToUnload;

        for (auto it = s_loaded.begin(); it != s_loaded.end();) {
            if (desiredChunks.find(*it) == desiredChunks.end()) {
                // Chunk is outside render distance - unload it
                chunksToUnload.push_back(*it);
                it = s_loaded.erase(it);
            } else {
                ++it;
            }
        }

        // 5) **SIMPLIFIED**: Unload all chunks immediately (no throttling or grace period)
        for (const auto& pos : chunksToUnload) {
            UnloadChunk(pos);
            //Log::Debug("Unloaded chunk (%d, %d)", pos.x, pos.z);
        }

        // 6) Performance monitoring
        static int frameCounter = 0;
        static auto lastStatsTime = std::chrono::steady_clock::now();

        frameCounter++;
        if (frameCounter % 300 == 0) { // Every 300 frames (~5 seconds at 60 FPS)
            auto currentTime = std::chrono::steady_clock::now();
            auto deltaTime = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - lastStatsTime);

            Log::Info("WorldManager stats: %zu chunks loaded, %zu mesh sections rendered, "
                     "camera at chunk (%d, %d), loaded %zu, unloaded %zu chunks this update",
                     s_loaded.size(), Render::g_chunkMeshes.size(), cx, cz,
                     loadsThisFrame, chunksToUnload.size());

            lastStatsTime = currentTime;
        }
    }

    void WorldManager::LoadChunk(Math::ChunkPos pos) {
        //Log::Debug("LoadChunk requested for (%d, %d)", pos.x, pos.z);

        // Request chunk generation through the ChunkProvider
        // The ChunkProvider will automatically handle neighbor detection and trigger appropriate meshing
        ChunkProvider::RequestChunk(pos);
    }

    void WorldManager::UnloadChunk(Math::ChunkPos pos) {
        //Log::Info("UnloadChunk starting for (%d, %d)", pos.x, pos.z);

        // 1) **FIXED**: Use the improved RemoveChunkMeshes function
        Render::RemoveChunkMeshes(pos);

        // 2) Unload from ChunkProvider (this will also trigger remeshing of dependent neighbors)
        ChunkProvider::UnloadChunk(pos);

        //Log::Info("UnloadChunk completed for (%d, %d)", pos.x, pos.z);
    }

    void WorldManager::ForceRemeshChunk(Math::ChunkPos pos) {
        Log::Info("Force remesh requested for chunk (%d, %d)", pos.x, pos.z);

        // **CRITICAL FIX**: Use the SAME meshing logic as initial generation
        // Don't delete GPU meshes immediately - let the system replace them naturally

        // Access the chunk registry to check if chunk exists and get neighbor context
        {
            std::shared_lock<std::shared_mutex> lock(s_registryMutex);
            uint64_t key = MakeChunkKey(pos.x, pos.z);
            auto it = s_chunkRegistry.find(key);

            if (it != s_chunkRegistry.end() && it->second && it->second->chunk) {
                // Mark the chunk as needing a new mesh
                it->second->chunk->needsMesh.store(true, std::memory_order_relaxed);
                it->second->chunk->hasMesh.store(false, std::memory_order_relaxed);
                it->second->hasPendingMesh.store(false, std::memory_order_relaxed);

                Log::Debug("Marked chunk (%d, %d) for remeshing", pos.x, pos.z);

                // **FIXED**: Check if we have all neighbors for meshing
                auto chunk = it->second->chunk;
                NeighborContext ctx = CreateNeighborContext(chunk, pos);

                // **NEW**: Use the SAME meshing strategy as initial generation
                if (ctx.hasAllNeighbors) {
                    // Meshing with full neighbor context (same as initial generation)
                    Log::Debug("Using inter-chunk remeshing for chunk (%d, %d)", pos.x, pos.z);

                    for (int s = 0; s < Math::SECTIONS_PER_CHUNK; ++s) {
                        if (!chunk->sections[s]) {
                            continue; // Skip empty sections
                        }

                        // Create mesh data for this section
                        auto* meshData = new MeshData();
                        meshData->chunkXZ = { pos.x, pos.z };
                        meshData->sectionIndex = s;

                        ChunkSection* sectionPtr = chunk->sections[s].get();

                        // **CRITICAL**: Use InterChunkMesherJob (same as initial generation)
                        JobSystem::g_ThreadPool.Enqueue([sectionPtr, meshData, ctx, pos, s]() {
                            try {
                                Log::Debug("remesh for chunk (%d, %d) section %d", pos.x, pos.z, s);

                                // Use the SAME mesher as initial generation
                                InterChunkMesherJob(sectionPtr, meshData, ctx);

                            } catch (const std::exception& e) {
                                Log::Error("remesh failed for chunk (%d, %d) section %d: %s",
                                          pos.x, pos.z, s, e.what());
                                delete meshData;
                            } catch (...) {
                                Log::Error("remesh failed for chunk (%d, %d) section %d with unknown exception",
                                          pos.x, pos.z, s);
                                delete meshData;
                            }
                        });
                    }
                } else {
                    // Standard meshing without all neighbors (fallback)
                    Log::Debug("Using standard remeshing for chunk (%d, %d) - not all neighbors available", pos.x, pos.z);

                    for (int s = 0; s < Math::SECTIONS_PER_CHUNK; ++s) {
                        if (!chunk->sections[s]) {
                            continue; // Skip empty sections
                        }

                        // Create mesh data for this section
                        auto* meshData = new MeshData();
                        meshData->chunkXZ = { pos.x, pos.z };
                        meshData->sectionIndex = s;

                        ChunkSection* sectionPtr = chunk->sections[s].get();

                        // **IMPROVED**: Use mesher with partial neighbor context
                        JobSystem::g_ThreadPool.Enqueue([chunk, sectionPtr, meshData, ctx, pos, s]() {
                            try {
                                Log::Debug("Standard remesh for chunk (%d, %d) section %d", pos.x, pos.z, s);

                                // **FIXED**: Use InterChunkMesherJob even without all neighbors
                                // This will still do better culling than MesherJob
                                InterChunkMesherJob(sectionPtr, meshData, ctx);

                            } catch (const std::exception& e) {
                                Log::Error("Standard remesh failed for chunk (%d, %d) section %d: %s",
                                          pos.x, pos.z, s, e.what());
                                delete meshData;
                            } catch (...) {
                                Log::Error("Standard remesh failed for chunk (%d, %d) section %d with unknown exception",
                                          pos.x, pos.z, s);
                                delete meshData;
                            }
                        });
                    }
                }
            } else {
                Log::Warning("Cannot remesh chunk (%d, %d) - chunk not found or not generated", pos.x, pos.z);
            }
        }
    }

    size_t WorldManager::GetLoadedChunkCount() {
        return s_loaded.size();
    }

    size_t WorldManager::GetRenderedSectionCount() {
        return Render::g_chunkMeshes.size();
    }

    bool WorldManager::IsChunkLoaded(Math::ChunkPos pos) {
        return s_loaded.find(pos) != s_loaded.end();
    }

    std::vector<Math::ChunkPos> WorldManager::GetLoadedChunks() {
        std::vector<Math::ChunkPos> result;
        result.reserve(s_loaded.size());
        for (const auto& pos : s_loaded) {
            result.push_back(pos);
        }
        return result;
    }

} // namespace Game
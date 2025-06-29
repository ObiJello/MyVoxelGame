// File: src/game/WorldManager.cpp (Fixed Remeshing Issues)
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
    std::unordered_map<Math::ChunkPos, std::chrono::steady_clock::time_point, ChunkPosHash> WorldManager::s_loadTimes;

    void WorldManager::Update(const glm::vec3& cameraPos) {
        // 1) Calculate camera chunk position
        int cx = static_cast<int>(std::floor(cameraPos.x / Math::CHUNK_SIZE_X));
        int cz = static_cast<int>(std::floor(cameraPos.z / Math::CHUNK_SIZE_Z));
        Math::ChunkPos cam{cx, cz};

        // 2) Build desired set of chunks based on render radius
        std::unordered_set<Math::ChunkPos, ChunkPosHash> desiredChunks;
        for (int dz = -RENDER_RADIUS; dz <= RENDER_RADIUS; ++dz) {
            for (int dx = -RENDER_RADIUS; dx <= RENDER_RADIUS; ++dx) {
                // Optional: Use circular culling instead of square
                if (dx * dx + dz * dz <= RENDER_RADIUS * RENDER_RADIUS) {
                    desiredChunks.insert({cx + dx, cz + dz});
                }
            }
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

        // Load new chunks with throttling to prevent frame drops
        constexpr int MAX_LOADS_PER_FRAME = 4;
        int loadsThisFrame = 0;

        for (const auto& pos : newChunks) {
            if (loadsThisFrame >= MAX_LOADS_PER_FRAME) {
                break; // Defer remaining loads to next frame
            }

            LoadChunk(pos);
            s_loaded.insert(pos);
            s_loadTimes[pos] = std::chrono::steady_clock::now();
            loadsThisFrame++;

            /*Log::Debug("Requested loading of chunk (%d, %d), distance from camera: %d",
                      pos.x, pos.z, (pos.x - cx) * (pos.x - cx) + (pos.z - cz) * (pos.z - cz));*/
        }

        // 4) Identify chunks to unload (outside render radius)
        std::vector<Math::ChunkPos> chunksToUnload;
        for (auto it = s_loaded.begin(); it != s_loaded.end();) {
            if (desiredChunks.find(*it) == desiredChunks.end()) {
                chunksToUnload.push_back(*it);
                it = s_loaded.erase(it);
            } else {
                ++it;
            }
        }

        // 5) Unload chunks with throttling and grace period
        constexpr int MAX_UNLOADS_PER_FRAME = 2;
        constexpr auto GRACE_PERIOD = std::chrono::seconds(5); // Keep chunks loaded for 5 seconds after they leave render distance

        auto now = std::chrono::steady_clock::now();
        int unloadsThisFrame = 0;

        for (const auto& pos : chunksToUnload) {
            if (unloadsThisFrame >= MAX_UNLOADS_PER_FRAME) {
                break; // Defer remaining unloads to next frame
            }

            auto loadTimeIt = s_loadTimes.find(pos);
            if (loadTimeIt != s_loadTimes.end()) {
                if (now - loadTimeIt->second < GRACE_PERIOD) {
                    continue; // Skip unloading, still in grace period
                }
                s_loadTimes.erase(loadTimeIt);
            }

            UnloadChunk(pos);
            unloadsThisFrame++;

            Log::Debug("Unloaded chunk (%d, %d)", pos.x, pos.z);
        }

        // 6) Performance monitoring
        static int frameCounter = 0;
        static auto lastStatsTime = std::chrono::steady_clock::now();

        frameCounter++;
        if (frameCounter % 300 == 0) { // Every 300 frames (~5 seconds at 60 FPS)
            auto currentTime = std::chrono::steady_clock::now();
            auto deltaTime = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - lastStatsTime);

            Log::Info("WorldManager stats: %zu chunks loaded, %zu mesh sections rendered, "
                     "camera at chunk (%d, %d), update time: %lld ms",
                     s_loaded.size(), Render::g_chunkMeshes.size(), cx, cz, deltaTime.count());

            lastStatsTime = currentTime;
        }
    }

    void WorldManager::LoadChunk(Math::ChunkPos pos) {
        //Log::Debug("LoadChunk requested for (%d, %d)", pos.x, pos.z);

        // Request chunk generation through the enhanced ChunkProvider
        // The ChunkProvider will automatically handle neighbor detection and
        // trigger appropriate meshing (standard or enhanced with inter-chunk culling)
        ChunkProvider::RequestChunk(pos);
    }

    void WorldManager::UnloadChunk(Math::ChunkPos pos) {
        Log::Debug("UnloadChunk requested for (%d, %d)", pos.x, pos.z);

        // 1) Remove GPU meshes for this chunk
        auto& meshes = Render::g_chunkMeshes;
        auto originalSize = meshes.size();

        meshes.erase(
            std::remove_if(meshes.begin(), meshes.end(),
                [&pos](const auto& cm) {
                    bool shouldRemove = (cm.chunkXZ.x == pos.x && cm.chunkXZ.z == pos.z);
                    if (shouldRemove) {
                        // Clean up OpenGL resources
                        glDeleteVertexArrays(1, &cm.vao);
                        glDeleteBuffers(1, &cm.vbo);
                        glDeleteBuffers(1, &cm.ebo);
                    }
                    return shouldRemove;
                }),
            meshes.end()
        );

        auto removedMeshes = originalSize - meshes.size();
        if (removedMeshes > 0) {
            Log::Debug("Removed %zu mesh sections for chunk (%d, %d)", removedMeshes, pos.x, pos.z);
        }

        // 2) Unload from ChunkProvider (this will also trigger remeshing of dependent neighbors)
        ChunkProvider::UnloadChunk(pos);
    }

    void WorldManager::ForceRemeshChunk(Math::ChunkPos pos) {
    Log::Info("Force remesh requested for chunk (%d, %d)", pos.x, pos.z);

    // **CRITICAL FIX**: Don't delete GPU meshes immediately!
    // Instead, just mark the chunk as needing a new mesh and let the system
    // generate new meshes. The old meshes will be replaced when new ones arrive.

    // Access the chunk registry to mark the chunk for remeshing
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

            // **NEW**: Directly trigger meshing for each section that exists
            auto chunk = it->second->chunk;
            for (int s = 0; s < Math::SECTIONS_PER_CHUNK; ++s) {
                if (!chunk->sections[s]) {
                    continue; // Skip empty sections
                }

                // Create mesh data for this section
                auto* meshData = new MeshData();
                meshData->chunkXZ = { pos.x, pos.z };
                meshData->sectionIndex = s;

                ChunkSection* sectionPtr = chunk->sections[s].get();

                // **CRITICAL**: Use the standard mesher for immediate remesh
                // This ensures we get a mesh quickly to replace the old one
                JobSystem::g_ThreadPool.Enqueue([chunk, sectionPtr, meshData, pos, s]() {
                    try {
                        Log::Debug("Remeshing chunk (%d, %d) section %d", pos.x, pos.z, s);

                        // Use enhanced mesher with chunk context
                        MesherJob(sectionPtr, meshData, chunk.get());

                        Log::Debug("Remesh completed for chunk (%d, %d) section %d", pos.x, pos.z, s);
                    } catch (const std::exception& e) {
                        Log::Error("Remesh failed for chunk (%d, %d) section %d: %s",
                                  pos.x, pos.z, s, e.what());
                        delete meshData;
                    } catch (...) {
                        Log::Error("Remesh failed for chunk (%d, %d) section %d with unknown exception",
                                  pos.x, pos.z, s);
                        delete meshData;
                    }
                });
            }
        } else {
            Log::Warning("Cannot remesh chunk (%d, %d) - chunk not found or not generated", pos.x, pos.z);
        }
    }
    }

    // **NEW HELPER FUNCTION**: Safely get chunk for remeshing
    std::shared_ptr<Chunk> WorldManager::GetChunkForRemesh(Math::ChunkPos pos) {
        std::shared_lock<std::shared_mutex> lock(s_registryMutex);
        uint64_t key = MakeChunkKey(pos.x, pos.z);
        auto it = s_chunkRegistry.find(key);

        if (it != s_chunkRegistry.end() && it->second &&
            it->second->chunk && it->second->isGenerated.load()) {
            return it->second->chunk;
        }

        return nullptr;
    }

    // **NEW HELPER FUNCTION**: Create neighbor context (moved from ChunkProvider for access)
    NeighborContext WorldManager::CreateNeighborContext(std::shared_ptr<Chunk> centerChunk, Math::ChunkPos pos) {
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

    // **NEW HELPER FUNCTION**: Get neighbor chunk safely
    std::shared_ptr<Chunk> WorldManager::GetNeighborChunk(Math::ChunkPos pos, int dx, int dz) {
        uint64_t key = MakeChunkKey(pos.x + dx, pos.z + dz);

        std::shared_lock<std::shared_mutex> lock(s_registryMutex);
        auto it = s_chunkRegistry.find(key);
        if (it != s_chunkRegistry.end() && it->second &&
            it->second->isGenerated.load()) {
            return it->second->chunk;
        }
        return nullptr;
    }

    // **NEW HELPER FUNCTION**: Update neighbor counts (moved from ChunkProvider for access)
    void WorldManager::UpdateNeighborCounts(Math::ChunkPos pos) {
        uint64_t centerKey = MakeChunkKey(pos.x, pos.z);

        std::unique_lock<std::shared_mutex> lock(s_registryMutex);
        auto centerIt = s_chunkRegistry.find(centerKey);
        if (centerIt == s_chunkRegistry.end() || !centerIt->second->isGenerated.load()) {
            return;
        }

        // Neighbor offset table
        static constexpr std::array<std::pair<int, int>, 4> NEIGHBOR_OFFSETS = {{
            {-1,  0}, {1,  0}, {0, -1}, {0,  1}
        }};

        // Count available neighbors for center chunk
        int neighborCount = 0;
        for (auto [dx, dz] : NEIGHBOR_OFFSETS) {
            uint64_t neighborKey = MakeChunkKey(pos.x + dx, pos.z + dz);
            auto neighborIt = s_chunkRegistry.find(neighborKey);
            if (neighborIt != s_chunkRegistry.end() && neighborIt->second->isGenerated.load()) {
                neighborCount++;
            }
        }

        centerIt->second->neighborCount.store(neighborCount);

        // If we have all neighbors and no pending mesh, trigger enhanced meshing
        if (neighborCount == 4 && !centerIt->second->hasPendingMesh.exchange(true)) {
            auto chunk = centerIt->second->chunk;

            Log::Debug("Triggering enhanced inter-chunk meshing for chunk (%d, %d) with all 4 neighbors",
                      pos.x, pos.z);

            // Schedule enhanced meshing job
            JobSystem::g_ThreadPool.Enqueue([chunk, pos]() {
                EnhancedMeshingJob(chunk, pos);
            });
        }
    }

    // **NEW HELPER FUNCTION**: Make chunk key
    uint64_t WorldManager::MakeChunkKey(int32_t x, int32_t z) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) |
               static_cast<uint32_t>(z);
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
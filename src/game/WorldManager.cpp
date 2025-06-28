// File: src/game/WorldManager.cpp (Enhanced with Inter-Chunk Mesh Coordination)
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

        // Remove existing GPU meshes for this chunk
        auto& meshes = Render::g_chunkMeshes;
        meshes.erase(
            std::remove_if(meshes.begin(), meshes.end(),
                [&pos](const auto& cm) {
                    bool shouldRemove = (cm.chunkXZ.x == pos.x && cm.chunkXZ.z == pos.z);
                    if (shouldRemove) {
                        glDeleteVertexArrays(1, &cm.vao);
                        glDeleteBuffers(1, &cm.vbo);
                        glDeleteBuffers(1, &cm.ebo);
                    }
                    return shouldRemove;
                }),
            meshes.end()
        );

        // Request regeneration through ChunkProvider
        // This will trigger the appropriate meshing strategy based on neighbor availability
        ChunkProvider::RequestChunk(pos);
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
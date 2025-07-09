// File: src/engine/world/ChunkProvider.hpp
#pragma once

#include "../../game/WorldMath.hpp"
#include "Chunk.hpp"
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <shared_mutex>
#include <atomic>
#include <string>
#include <filesystem>

// Path to FastNoiseLite.h (adjust if you put it somewhere else):
#include "../../../ext/FastNoiseLite.h"

namespace Game {

    // Forward declare ChunkData for external access
    struct ChunkData {
        std::shared_ptr<Chunk> chunk;
        std::unordered_set<uint64_t> dependents;  // Chunks that depend on this one for meshing
        std::atomic<bool> isGenerated{false};     // Block data is complete
        std::atomic<bool> hasPendingMesh{false};  // Mesh generation is queued
        std::atomic<int> neighborCount{0};        // Number of available neighbors (0-4)

        ChunkData(std::shared_ptr<Chunk> c) : chunk(std::move(c)) {}
    };

    // External declarations for chunk registry (defined in ChunkProvider.cpp)
    extern std::unordered_map<uint64_t, std::unique_ptr<ChunkData>> s_chunkRegistry;
    extern std::shared_mutex s_registryMutex;

    class ChunkProvider {
    public:
        // === CHUNK LIFECYCLE MANAGEMENT ===

        // Enqueue a background job to generate/load the chunk at chunk-coordinates (pos.x, pos.z).
        // **ENHANCED**: Now tries to load from Minecraft region files first, falls back to generation
        static void RequestChunk(Math::ChunkPos pos);

        // Unload a chunk and trigger remeshing of dependent neighbors
        static void UnloadChunk(Math::ChunkPos pos);

        // **NEW**: Get chunk if already loaded (no loading) - for World class
        static std::shared_ptr<Chunk> GetChunkIfLoaded(Math::ChunkPos pos);

        // **NEW**: Check if chunk is loaded and generated
        static bool IsChunkLoaded(Math::ChunkPos pos);

        // **NEW**: Overload for coordinate pair (for physics system compatibility)
        static bool IsChunkLoaded(int chunkX, int chunkZ) {
            return IsChunkLoaded({chunkX, chunkZ});
        }

        // **NEW**: Get chunk with optional loading request
        static std::shared_ptr<Chunk> GetChunkWithLoad(Math::ChunkPos pos, bool requestIfMissing = true);

        // === CHUNK AREA MANAGEMENT ===

        // Update chunks around player position (requests missing chunks)
        static void UpdateLoadedChunks(Math::ChunkPos playerChunk, int viewDistance);

        // Unload chunks outside view distance
        static void UnloadDistantChunks(Math::ChunkPos playerChunk, int unloadDistance);

        // **NEW**: Minecraft world support functions
        // Set the path to a Minecraft world directory (contains 'region' folder)
        static void SetMinecraftWorldPath(const std::string& worldPath);

        // Get the current Minecraft world path
        static std::string GetMinecraftWorldPath();

        // Check if a specific chunk exists in Minecraft region files
        static bool IsMinecraftChunkAvailable(Math::ChunkPos pos);

        // **NEW**: Registry management functions
        // Get number of currently loaded chunks
        static size_t GetLoadedChunkCount();

        // Clear all chunks from memory (useful for world switching)
        static void ClearAllChunks();

        // **NEW**: World loading utilities
        // Load a Minecraft world from a save directory
        static bool LoadMinecraftWorld(const std::string& savePath) {
            // Check if the path exists and has the expected structure
            if (!std::filesystem::exists(savePath)) {
                Log::Error("Minecraft world path does not exist: %s", savePath.c_str());
                return false;
            }

            std::string regionPath = savePath + "/region";
            if (!std::filesystem::exists(regionPath)) {
                Log::Error("No region directory found in world: %s", savePath.c_str());
                return false;
            }

            // Count region files
            int regionFileCount = 0;
            try {
                for (const auto& entry : std::filesystem::directory_iterator(regionPath)) {
                    if (entry.path().extension() == ".mca") {
                        regionFileCount++;
                    }
                }
            } catch (const std::exception& e) {
                Log::Error("Error scanning region directory: %s", e.what());
                return false;
            }

            if (regionFileCount == 0) {
                Log::Warning("No .mca files found in region directory: %s", regionPath.c_str());
                return false;
            }

            // Clear existing chunks and set new world path
            ClearAllChunks();
            SetMinecraftWorldPath(savePath);

            Log::Info("Successfully loaded Minecraft world: %s (%d region files)",
                     savePath.c_str(), regionFileCount);
            return true;
        }

        // **NEW**: Get world statistics
        struct WorldStats {
            size_t loadedChunks = 0;
            size_t minecraftChunks = 0;
            size_t generatedChunks = 0;
            std::string worldPath;
            bool hasMinecraftWorld = false;
        };

        static WorldStats GetWorldStats() {
            WorldStats stats;
            stats.worldPath = GetMinecraftWorldPath();
            stats.hasMinecraftWorld = !stats.worldPath.empty();
            stats.loadedChunks = GetLoadedChunkCount();

            // Note: Counting Minecraft vs generated chunks would require
            // additional tracking in ChunkData structure

            return stats;
        }

        // **NEW**: Registry access helpers (public for internal use)
        static uint64_t MakeChunkKey(Math::ChunkPos pos);
        static Math::ChunkPos KeyToChunkPos(uint64_t key);

    private:
        ChunkProvider() = delete; // Static class
    };

} // namespace Game
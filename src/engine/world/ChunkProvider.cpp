// File: src/engine/world/ChunkProvider.cpp (UPDATED - New access methods)
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

    // === INTERNAL HELPER FUNCTIONS ===

    // Helper function to create a unique key from chunk coordinates
    uint64_t ChunkProvider::MakeChunkKey(Math::ChunkPos pos) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(pos.x)) << 32) |
               static_cast<uint32_t>(pos.z);
    }

    // Helper function to get chunk coordinates from key
    Math::ChunkPos ChunkProvider::KeyToChunkPos(uint64_t key) {
        int32_t x = static_cast<int32_t>(key >> 32);
        int32_t z = static_cast<int32_t>(key & 0xFFFFFFFF);
        return {x, z};
    }

    // Thread-safe neighbor lookup with bounds checking
    static std::shared_ptr<Chunk> GetNeighborChunk(Math::ChunkPos pos, int dx, int dz) {
        uint64_t key = ChunkProvider::MakeChunkKey({pos.x + dx, pos.z + dz});

        std::shared_lock<std::shared_mutex> lock(s_registryMutex);
        auto it = s_chunkRegistry.find(key);
        if (it != s_chunkRegistry.end() && it->second->isGenerated.load()) {
            return it->second->chunk;
        }
        return nullptr;
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
        uint64_t centerKey = ChunkProvider::MakeChunkKey(pos);

        std::unique_lock<std::shared_mutex> lock(s_registryMutex);
        auto centerIt = s_chunkRegistry.find(centerKey);
        if (centerIt == s_chunkRegistry.end() || !centerIt->second->isGenerated.load()) {
            return;
        }

        // Count available neighbors for center chunk
        int neighborCount = 0;
        for (auto [dx, dz] : NEIGHBOR_OFFSETS) {
            uint64_t neighborKey = ChunkProvider::MakeChunkKey({pos.x + dx, pos.z + dz});
            auto neighborIt = s_chunkRegistry.find(neighborKey);
            if (neighborIt != s_chunkRegistry.end() && neighborIt->second->isGenerated.load()) {
                neighborCount++;
                // Register bidirectional dependencies
                centerIt->second->dependents.insert(neighborKey);
                neighborIt->second->dependents.insert(centerKey);
            }
        }

        centerIt->second->neighborCount.store(neighborCount);
    }

    // === PUBLIC CHUNK ACCESS METHODS ===

    std::shared_ptr<Chunk> ChunkProvider::GetChunkIfLoaded(Math::ChunkPos pos) {
        uint64_t key = MakeChunkKey(pos);

        std::shared_lock<std::shared_mutex> lock(s_registryMutex);
        auto it = s_chunkRegistry.find(key);

        if (it != s_chunkRegistry.end() && it->second->isGenerated.load()) {
            return it->second->chunk;
        }

        return nullptr; // Chunk not loaded or not generated
    }

    bool ChunkProvider::IsChunkLoaded(Math::ChunkPos pos) {
        return GetChunkIfLoaded(pos) != nullptr;
    }

    std::shared_ptr<Chunk> ChunkProvider::GetChunkWithLoad(Math::ChunkPos pos, bool requestIfMissing) {
        // First try to get chunk if already loaded
        auto chunk = GetChunkIfLoaded(pos);
        if (chunk) {
            return chunk;
        }

        // If not loaded and we should request it, do so
        if (requestIfMissing) {
            RequestChunk(pos);
        }

        // Return null for now - caller should check again later
        // In a more advanced implementation, we might wait for the chunk to load
        return nullptr;
    }

    // === CHUNK AREA MANAGEMENT ===

    void ChunkProvider::UpdateLoadedChunks(Math::ChunkPos playerChunk, int viewDistance) {

        int chunksRequested = 0;

        // Request chunks around player position
        for (int dx = -viewDistance; dx <= viewDistance; ++dx) {
            for (int dz = -viewDistance; dz <= viewDistance; ++dz) {
                Math::ChunkPos chunkPos = {playerChunk.x + dx, playerChunk.z + dz};

                // Check if chunk is within circular view distance
                float distance = std::sqrt(dx * dx + dz * dz);
                if (distance <= viewDistance) {
                    if (!IsChunkLoaded(chunkPos)) {
                        RequestChunk(chunkPos);
                        chunksRequested++;
                    }
                }
            }
        }

        if (chunksRequested > 0) {
            Log::Debug("Requested %d new chunks for loading", chunksRequested);
        }
    }

    void ChunkProvider::UnloadDistantChunks(Math::ChunkPos playerChunk, int unloadDistance) {
        std::vector<Math::ChunkPos> chunksToUnload;

        {
            std::shared_lock<std::shared_mutex> lock(s_registryMutex);

            for (const auto& [key, chunkData] : s_chunkRegistry) {
                Math::ChunkPos chunkPos = KeyToChunkPos(key);

                float distance = std::sqrt(
                    std::pow(chunkPos.x - playerChunk.x, 2) +
                    std::pow(chunkPos.z - playerChunk.z, 2)
                );

                if (distance > unloadDistance) {
                    chunksToUnload.push_back(chunkPos);
                }
            }
        }

        // Unload distant chunks
        for (const auto& chunkPos : chunksToUnload) {
            UnloadChunk(chunkPos);
        }

        if (!chunksToUnload.empty()) {
            Log::Debug("Unloaded %zu distant chunks", chunksToUnload.size());
        }
    }

    // === EXISTING METHODS (unchanged) ===

    void ChunkProvider::RequestChunk(Math::ChunkPos pos) {
        // Initialize noise generator once (for fallback generation)
        static bool s_initialized = false;
        if (!s_initialized) {
            s_noise.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
            s_noise.SetSeed(1337);
            s_initialized = true;
        }

        uint64_t key = MakeChunkKey(pos);

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
        uint64_t key = MakeChunkKey(pos);

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
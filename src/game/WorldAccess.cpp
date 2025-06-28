// File: src/game/WorldAccess.cpp
#include "WorldAccess.hpp"
#include "ChunkProvider.hpp"
#include "../core/Log.hpp"
#include <shared_mutex>
#include <unordered_map>
#include <cmath>
#include <algorithm>

namespace Game {

    // Static storage for callbacks
    static std::vector<ChunkModifiedCallback> s_modificationCallbacks;
    static std::shared_mutex s_callbackMutex;

    // External access to chunk registry (defined in ChunkProvider.cpp)
    extern std::unordered_map<uint64_t, std::unique_ptr<struct ChunkData>> s_chunkRegistry;
    extern std::shared_mutex s_registryMutex;

    // Helper to create chunk key
    static uint64_t MakeChunkKey(int32_t x, int32_t z) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) |
               static_cast<uint32_t>(z);
    }

    BlockID WorldAccess::GetBlock(int worldX, int worldY, int worldZ) {
        // Check world bounds
        if (!IsValidPosition(worldX, worldY, worldZ)) {
            return BlockID::Air;
        }

        // Convert to chunk and local coordinates
        Math::ChunkPos chunkPos;
        int localX, localY, localZ;
        WorldToLocal(worldX, worldY, worldZ, chunkPos, localX, localY, localZ);

        // Access chunk registry with read lock
        std::shared_lock<std::shared_mutex> lock(s_registryMutex);

        uint64_t key = MakeChunkKey(chunkPos.x, chunkPos.z);
        auto it = s_chunkRegistry.find(key);

        if (it == s_chunkRegistry.end() || !it->second || !it->second->chunk) {
            return BlockID::Air; // Chunk not loaded
        }

        // Get block from chunk
        return it->second->chunk->GetBlock(localX, localY, localZ);
    }

    bool WorldAccess::SetBlock(int worldX, int worldY, int worldZ, BlockID id) {
        // Check world bounds
        if (!IsValidPosition(worldX, worldY, worldZ)) {
            Log::Warning("Attempted to set block outside world bounds at (%d, %d, %d)",
                        worldX, worldY, worldZ);
            return false;
        }

        // Convert to chunk and local coordinates
        Math::ChunkPos chunkPos;
        int localX, localY, localZ;
        WorldToLocal(worldX, worldY, worldZ, chunkPos, localX, localY, localZ);

        // Access chunk registry with write lock
        {
            std::unique_lock<std::shared_mutex> lock(s_registryMutex);

            uint64_t key = MakeChunkKey(chunkPos.x, chunkPos.z);
            auto it = s_chunkRegistry.find(key);

            if (it == s_chunkRegistry.end() || !it->second || !it->second->chunk) {
                Log::Warning("Cannot set block - chunk (%d, %d) not loaded",
                            chunkPos.x, chunkPos.z);
                return false;
            }

            // Set the block
            it->second->chunk->SetBlock(localX, localY, localZ, id);
        }

        // Notify that this chunk was modified
        NotifyChunkModified(chunkPos);

        // Check if block is on chunk boundary and notify neighboring chunks
        std::vector<Math::ChunkPos> affectedChunks = GetAffectedChunks(worldX, worldZ);
        for (const auto& affected : affectedChunks) {
            if (affected.x != chunkPos.x || affected.z != chunkPos.z) {
                NotifyChunkModified(affected);
            }
        }

        Log::Debug("Set block at world (%d, %d, %d) to %s",
                  worldX, worldY, worldZ,
                  BlockRegistry::Get(id).name.c_str());

        return true;
    }

    int WorldAccess::SetBlocks(const std::vector<BlockModification>& modifications) {
        if (modifications.empty()) {
            return 0;
        }

        int successCount = 0;
        std::unordered_set<uint64_t> modifiedChunks;

        // Group modifications by chunk for efficiency
        std::unordered_map<uint64_t, std::vector<const BlockModification*>> chunkMods;

        for (const auto& mod : modifications) {
            if (!IsValidPosition(mod.worldX, mod.worldY, mod.worldZ)) {
                continue;
            }

            Math::ChunkPos chunkPos;
            int localX, localY, localZ;
            WorldToLocal(mod.worldX, mod.worldY, mod.worldZ,
                        chunkPos, localX, localY, localZ);

            uint64_t key = MakeChunkKey(chunkPos.x, chunkPos.z);
            chunkMods[key].push_back(&mod);
        }

        // Apply modifications chunk by chunk
        {
            std::unique_lock<std::shared_mutex> lock(s_registryMutex);

            for (const auto& [chunkKey, mods] : chunkMods) {
                auto it = s_chunkRegistry.find(chunkKey);
                if (it == s_chunkRegistry.end() || !it->second || !it->second->chunk) {
                    continue;
                }

                auto& chunk = it->second->chunk;

                for (const auto* mod : mods) {
                    Math::ChunkPos chunkPos;
                    int localX, localY, localZ;
                    WorldToLocal(mod->worldX, mod->worldY, mod->worldZ,
                                chunkPos, localX, localY, localZ);

                    chunk->SetBlock(localX, localY, localZ, mod->newId);
                    successCount++;

                    // Track modified chunks
                    modifiedChunks.insert(chunkKey);

                    // Check for boundary modifications
                    auto affected = GetAffectedChunks(mod->worldX, mod->worldZ);
                    for (const auto& pos : affected) {
                        modifiedChunks.insert(MakeChunkKey(pos.x, pos.z));
                    }
                }
            }
        }

        // Notify about all modified chunks
        for (uint64_t key : modifiedChunks) {
            int32_t x = static_cast<int32_t>(key >> 32);
            int32_t z = static_cast<int32_t>(key & 0xFFFFFFFF);
            NotifyChunkModified({x, z});
        }

        return successCount;
    }

    bool WorldAccess::IsValidPosition(int worldX, int worldY, int worldZ) {
        return worldY >= Math::MinY && worldY <= Math::MaxY;
    }

    bool WorldAccess::IsChunkLoadedAt(int worldX, int worldZ) {
        Math::ChunkPos chunkPos = WorldToChunkPos(worldX, worldZ);

        std::shared_lock<std::shared_mutex> lock(s_registryMutex);
        uint64_t key = MakeChunkKey(chunkPos.x, chunkPos.z);
        auto it = s_chunkRegistry.find(key);

        return it != s_chunkRegistry.end() && it->second &&
               it->second->chunk && it->second->isGenerated.load();
    }

    void WorldAccess::RegisterModificationCallback(ChunkModifiedCallback callback) {
        std::unique_lock<std::shared_mutex> lock(s_callbackMutex);
        s_modificationCallbacks.push_back(callback);
    }

    void WorldAccess::ClearModificationCallbacks() {
        std::unique_lock<std::shared_mutex> lock(s_callbackMutex);
        s_modificationCallbacks.clear();
    }

    Math::ChunkPos WorldAccess::WorldToChunkPos(int worldX, int worldZ) {
        // Handle negative coordinates properly
        int chunkX = (worldX >= 0) ? (worldX / Math::CHUNK_SIZE_X) :
                     ((worldX - Math::CHUNK_SIZE_X + 1) / Math::CHUNK_SIZE_X);
        int chunkZ = (worldZ >= 0) ? (worldZ / Math::CHUNK_SIZE_Z) :
                     ((worldZ - Math::CHUNK_SIZE_Z + 1) / Math::CHUNK_SIZE_Z);

        return {chunkX, chunkZ};
    }

    void WorldAccess::WorldToLocal(int worldX, int worldY, int worldZ,
                                  Math::ChunkPos& chunkPos,
                                  int& localX, int& localY, int& localZ) {
        chunkPos = WorldToChunkPos(worldX, worldZ);

        // Calculate local coordinates within chunk
        localX = ((worldX % Math::CHUNK_SIZE_X) + Math::CHUNK_SIZE_X) % Math::CHUNK_SIZE_X;
        localY = worldY; // Y is already in chunk space (0-255)
        localZ = ((worldZ % Math::CHUNK_SIZE_Z) + Math::CHUNK_SIZE_Z) % Math::CHUNK_SIZE_Z;
    }

    void WorldAccess::NotifyChunkModified(Math::ChunkPos pos) {
        std::shared_lock<std::shared_mutex> lock(s_callbackMutex);

        for (const auto& callback : s_modificationCallbacks) {
            if (callback) {
                callback(pos);
            }
        }
    }

    std::vector<Math::ChunkPos> WorldAccess::GetAffectedChunks(int worldX, int worldZ) {
        std::vector<Math::ChunkPos> affected;

        // Get the primary chunk
        Math::ChunkPos primary = WorldToChunkPos(worldX, worldZ);
        affected.push_back(primary);

        // Calculate local position within chunk
        int localX = ((worldX % Math::CHUNK_SIZE_X) + Math::CHUNK_SIZE_X) % Math::CHUNK_SIZE_X;
        int localZ = ((worldZ % Math::CHUNK_SIZE_Z) + Math::CHUNK_SIZE_Z) % Math::CHUNK_SIZE_Z;

        // Check if on chunk boundaries
        if (localX == 0) {
            affected.push_back({primary.x - 1, primary.z}); // West neighbor
        }
        if (localX == Math::CHUNK_SIZE_X - 1) {
            affected.push_back({primary.x + 1, primary.z}); // East neighbor
        }
        if (localZ == 0) {
            affected.push_back({primary.x, primary.z - 1}); // North neighbor
        }
        if (localZ == Math::CHUNK_SIZE_Z - 1) {
            affected.push_back({primary.x, primary.z + 1}); // South neighbor
        }

        // Check corners
        if (localX == 0 && localZ == 0) {
            affected.push_back({primary.x - 1, primary.z - 1}); // NW corner
        }
        if (localX == Math::CHUNK_SIZE_X - 1 && localZ == 0) {
            affected.push_back({primary.x + 1, primary.z - 1}); // NE corner
        }
        if (localX == 0 && localZ == Math::CHUNK_SIZE_Z - 1) {
            affected.push_back({primary.x - 1, primary.z + 1}); // SW corner
        }
        if (localX == Math::CHUNK_SIZE_X - 1 && localZ == Math::CHUNK_SIZE_Z - 1) {
            affected.push_back({primary.x + 1, primary.z + 1}); // SE corner
        }

        return affected;
    }

} // namespace Game
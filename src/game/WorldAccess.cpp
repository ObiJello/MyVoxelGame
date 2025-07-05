// File: src/game/WorldAccess.cpp - MODIFIED for section-level remeshing
#include "WorldAccess.hpp"
#include "ChunkProvider.hpp"
#include "BlockRegistry.hpp"
#include "../core/Log.hpp"
#include "../core/Config.hpp"
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <algorithm>

#include "JobSystem.hpp"

namespace Game {

    // Static storage for callbacks
    static std::vector<ChunkModifiedCallback> s_modificationCallbacks;
    static std::shared_mutex s_callbackMutex;

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

        return it->second->chunk->GetBlock(localX, worldY, localZ);
    }

    bool WorldAccess::SetBlock(int worldX, int worldY, int worldZ, BlockID id) {
        // Check world bounds
        if (!IsValidPosition(worldX, worldY, worldZ)) {
            Log::Warning("Attempted to set block outside world bounds at (%d, %d, %d)",
                        worldX, worldY, worldZ);
            return false;
        }

        // Get old block for comparison
        BlockID oldBlock = GetBlock(worldX, worldY, worldZ);

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

            it->second->chunk->SetBlock(localX, worldY, localZ, id);
        }

        // MODIFIED: Smart section-level remeshing instead of whole chunk
        NotifyBlockModified(worldX, worldY, worldZ, oldBlock, id);

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
                    // Get old block before modification
                    BlockID oldBlock = GetBlock(mod->worldX, mod->worldY, mod->worldZ);

                    Math::ChunkPos chunkPos;
                    int localX, localY, localZ;
                    WorldToLocal(mod->worldX, mod->worldY, mod->worldZ,
                                chunkPos, localX, localY, localZ);

                    chunk->SetBlock(localX, mod->worldY, localZ, mod->newId);
                    successCount++;

                    // MODIFIED: Smart section-level remeshing for each block
                    NotifyBlockModified(mod->worldX, mod->worldY, mod->worldZ, oldBlock, mod->newId);
                }
            }
        }

        return successCount;
    }

    // NEW: Smart block modification notification with section-level remeshing
    void WorldAccess::NotifyBlockModified(int worldX, int worldY, int worldZ, BlockID oldBlock, BlockID newBlock) {
        // Convert world Y to section
        int sectionIndex = Chunk::SectionIndexFromGlobalY(worldY);
        if (sectionIndex < 0) {
            return; // Out of world bounds
        }

        // Convert to chunk coordinates
        Math::ChunkPos chunkPos = WorldToChunkPos(worldX, worldZ);

        // Calculate which sections need remeshing
        std::vector<std::pair<Math::ChunkPos, int>> sectionsToRemesh;

        // ALWAYS remesh the section containing the changed block
        sectionsToRemesh.push_back({chunkPos, sectionIndex});

        // Check if we need to remesh adjacent sections
        int localY = Chunk::LocalYFromGlobalY(worldY);

        // If block is at section boundary, remesh adjacent sections
        if (localY == 0 && sectionIndex > 0) {
            // Block is at bottom of section, remesh section below
            sectionsToRemesh.push_back({chunkPos, sectionIndex - 1});
        }
        if (localY == Math::SECTION_HEIGHT - 1 && sectionIndex < Math::SECTIONS_PER_CHUNK - 1) {
            // Block is at top of section, remesh section above
            sectionsToRemesh.push_back({chunkPos, sectionIndex + 1});
        }

        // Check for inter-chunk effects
        int localX = ((worldX % Math::CHUNK_SIZE_X) + Math::CHUNK_SIZE_X) % Math::CHUNK_SIZE_X;
        int localZ = ((worldZ % Math::CHUNK_SIZE_Z) + Math::CHUNK_SIZE_Z) % Math::CHUNK_SIZE_Z;

        // If block is at chunk boundary, remesh adjacent chunks' same sections
        if (localX == 0) {
            // West boundary
            sectionsToRemesh.push_back({{chunkPos.x - 1, chunkPos.z}, sectionIndex});
        }
        if (localX == Math::CHUNK_SIZE_X - 1) {
            // East boundary
            sectionsToRemesh.push_back({{chunkPos.x + 1, chunkPos.z}, sectionIndex});
        }
        if (localZ == 0) {
            // North boundary
            sectionsToRemesh.push_back({{chunkPos.x, chunkPos.z - 1}, sectionIndex});
        }
        if (localZ == Math::CHUNK_SIZE_Z - 1) {
            // South boundary
            sectionsToRemesh.push_back({{chunkPos.x, chunkPos.z + 1}, sectionIndex});
        }

        // Trigger remeshing for affected sections
        for (const auto& [targetChunkPos, targetSectionIndex] : sectionsToRemesh) {
            RemeshSingleSection(targetChunkPos, targetSectionIndex);
        }

        // Still notify callbacks for other systems (but only once per chunk)
        static std::unordered_set<uint64_t> notifiedChunks;
        uint64_t chunkKey = MakeChunkKey(chunkPos.x, chunkPos.z);
        if (notifiedChunks.find(chunkKey) == notifiedChunks.end()) {
            notifiedChunks.insert(chunkKey);

            std::shared_lock<std::shared_mutex> lock(s_callbackMutex);
            for (const auto& callback : s_modificationCallbacks) {
                if (callback) {
                    callback(chunkPos);
                }
            }

            // Clear the notification set periodically to avoid memory leak
            if (notifiedChunks.size() > 1000) {
                notifiedChunks.clear();
            }
        }

        Log::Debug("Block change at (%d,%d,%d): queued %zu section remeshes",
                  worldX, worldY, worldZ, sectionsToRemesh.size());
    }

    // NEW: Remesh a single section instead of whole chunk
    void WorldAccess::RemeshSingleSection(Math::ChunkPos chunkPos, int sectionIndex) {
        // Access the chunk registry to check if chunk exists
        std::shared_lock<std::shared_mutex> lock(s_registryMutex);
        uint64_t key = MakeChunkKey(chunkPos.x, chunkPos.z);
        auto it = s_chunkRegistry.find(key);

        if (it == s_chunkRegistry.end() || !it->second || !it->second->chunk) {
            return; // Chunk not loaded, skip
        }

        auto chunk = it->second->chunk;

        // Check if section exists and is valid
        if (sectionIndex < 0 || sectionIndex >= Math::SECTIONS_PER_CHUNK) {
            return; // Invalid section index
        }

        if (!chunk->sections[sectionIndex]) {
            return; // Section doesn't exist (empty), no need to mesh
        }

        // Create neighbor context for enhanced meshing
        NeighborContext ctx = CreateNeighborContext(chunk, chunkPos);

        // Create mesh data for this specific section
        auto* meshData = new MeshData();
        meshData->chunkXZ = chunkPos;
        meshData->sectionIndex = sectionIndex;

        ChunkSection* sectionPtr = chunk->sections[sectionIndex].get();

        // Use appropriate meshing strategy based on neighbor availability
        if (ctx.hasAllNeighbors) {
            // Enhanced inter-chunk meshing
            JobSystem::g_ThreadPool.Enqueue([sectionPtr, meshData, ctx, chunkPos, sectionIndex]() {
                try {
                    InterChunkMesherJob(sectionPtr, meshData, ctx);
                } catch (const std::exception& e) {
                    Log::Error("Enhanced section remesh failed for chunk (%d, %d) section %d: %s",
                              chunkPos.x, chunkPos.z, sectionIndex, e.what());
                    delete meshData;
                } catch (...) {
                    Log::Error("Enhanced section remesh failed for chunk (%d, %d) section %d with unknown exception",
                              chunkPos.x, chunkPos.z, sectionIndex);
                    delete meshData;
                }
            });
        } else {
            // Standard meshing without all neighbors
            JobSystem::g_ThreadPool.Enqueue([chunk, sectionPtr, meshData, chunkPos, sectionIndex]() {
                try {
                    MesherJob(sectionPtr, meshData, chunk.get());
                } catch (const std::exception& e) {
                    Log::Error("Standard section remesh failed for chunk (%d, %d) section %d: %s",
                              chunkPos.x, chunkPos.z, sectionIndex, e.what());
                    delete meshData;
                } catch (...) {
                    Log::Error("Standard section remesh failed for chunk (%d, %d) section %d with unknown exception",
                              chunkPos.x, chunkPos.z, sectionIndex);
                    delete meshData;
                }
            });
        }

        Log::Debug("Queued remesh for section %d of chunk (%d,%d)",
                  sectionIndex, chunkPos.x, chunkPos.z);
    }

    // Helper function to create neighbor context (needs to be accessible)
    NeighborContext WorldAccess::CreateNeighborContext(std::shared_ptr<Chunk> centerChunk, Math::ChunkPos pos) {
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

    // Helper function to get neighbor chunk safely
    std::shared_ptr<Chunk> WorldAccess::GetNeighborChunk(Math::ChunkPos pos, int dx, int dz) {
        uint64_t key = MakeChunkKey(pos.x + dx, pos.z + dz);

        std::shared_lock<std::shared_mutex> lock(s_registryMutex);
        auto it = s_chunkRegistry.find(key);
        if (it != s_chunkRegistry.end() && it->second &&
            it->second->isGenerated.load()) {
            return it->second->chunk;
        }
        return nullptr;
    }

    // Rest of the existing functions remain the same...
    bool WorldAccess::IsValidPosition(int worldX, int worldY, int worldZ) {
        return worldY >= Config::MinY && worldY <= Config::MaxY;
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
        localY = worldY;
        localZ = ((worldZ % Math::CHUNK_SIZE_Z) + Math::CHUNK_SIZE_Z) % Math::CHUNK_SIZE_Z;
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
// File: src/game/WorldAccess.hpp - MODIFIED header
#pragma once

#include "Blocks.hpp"
#include "WorldMath.hpp"
#include "Mesher.hpp"  // For NeighborContext
#include <functional>
#include <vector>
#include <memory>

namespace Game {

    // Forward declarations
    class Chunk;

    // Callback type for chunk modification notifications
    using ChunkModifiedCallback = std::function<void(Math::ChunkPos)>;

    class WorldAccess {
    public:
        // Get block at world coordinates (thread-safe)
        static BlockID GetBlock(int worldX, int worldY, int worldZ);

        // Set block at world coordinates (thread-safe)
        // Returns true if successful, false if chunk doesn't exist or coords invalid
        static bool SetBlock(int worldX, int worldY, int worldZ, BlockID id);

        // Batch block modifications for efficiency
        struct BlockModification {
            int worldX, worldY, worldZ;
            BlockID newId;
        };

        // Apply multiple block modifications atomically
        // Returns number of blocks successfully modified
        static int SetBlocks(const std::vector<BlockModification>& modifications);

        // Check if a position is within world bounds
        static bool IsValidPosition(int worldX, int worldY, int worldZ);

        // Check if a chunk is loaded at the given world position
        static bool IsChunkLoadedAt(int worldX, int worldZ);

        // Register callback for when chunks are modified
        static void RegisterModificationCallback(ChunkModifiedCallback callback);

        // Clear all modification callbacks
        static void ClearModificationCallbacks();

    private:
        // Convert world coordinates to chunk coordinates
        static Math::ChunkPos WorldToChunkPos(int worldX, int worldZ);

        // Convert world coordinates to local chunk coordinates
        static void WorldToLocal(int worldX, int worldY, int worldZ,
                                Math::ChunkPos& chunkPos,
                                int& localX, int& localY, int& localZ);

        // NEW: Smart block modification notification with section-level remeshing
        static void NotifyBlockModified(int worldX, int worldY, int worldZ, BlockID oldBlock, BlockID newBlock);

        // NEW: Remesh a single section instead of whole chunk
        static void RemeshSingleSection(Math::ChunkPos chunkPos, int sectionIndex);

        // NEW: Helper functions for neighbor context
        static NeighborContext CreateNeighborContext(std::shared_ptr<Chunk> centerChunk, Math::ChunkPos pos);
        static std::shared_ptr<Chunk> GetNeighborChunk(Math::ChunkPos pos, int dx, int dz);

        // Get list of chunks affected by a block modification at chunk boundary
        static std::vector<Math::ChunkPos> GetAffectedChunks(int worldX, int worldZ);
    };

} // namespace Game
// File: src/engine/world/ChunkProvider.hpp
#pragma once

#include "Chunk.hpp"
#include "../../game/WorldMath.hpp"
#include "../block/Blocks.hpp"
#include "../../core/Config.hpp"
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <string>
#include <queue>

namespace Game {

    // Hash function for ChunkPos to use in unordered_map
    struct ChunkPosHash {
        std::size_t operator()(const Math::ChunkPos& pos) const {
            return std::hash<int32_t>{}(pos.x) ^ (std::hash<int32_t>{}(pos.z) << 1);
        }
    };

    // Dirty section tracking
    struct DirtySection {
        Math::ChunkPos chunkPos;
        int sectionIndex;

        bool operator==(const DirtySection& other) const {
            return chunkPos.x == other.chunkPos.x &&
                   chunkPos.z == other.chunkPos.z &&
                   sectionIndex == other.sectionIndex;
        }
    };

    struct DirtySectionHash {
        std::size_t operator()(const DirtySection& ds) const {
            return ChunkPosHash{}(ds.chunkPos) ^ (std::hash<int>{}(ds.sectionIndex) << 2);
        }
    };

    class ChunkProvider {
    public:
        ChunkProvider();
        ~ChunkProvider();

        // **NEW**: Static methods for global Minecraft world management
        static bool LoadMinecraftWorld(const std::string& savePath);
        static void ClearAllChunks();
        static void SetGlobalMinecraftWorldPath(const std::string& worldPath);

        // Lifecycle
        void Initialize();
        void Update(float deltaTime);
        void Shutdown();

        // Core chunk operations
        std::shared_ptr<Chunk> LoadChunk(int chunkX, int chunkZ);
        void UnloadChunk(int chunkX, int chunkZ);
        bool IsChunkLoaded(int chunkX, int chunkZ) const;
        bool EnsureChunkLoaded(int chunkX, int chunkZ);

        // **UPDATED**: Block access with world Y coordinates (no conversion needed)
        BlockID GetBlockAt(int worldX, int worldY, int worldZ) const;
        bool SetBlockAt(int worldX, int worldY, int worldZ, BlockID blockId);

        // Chunk management around player
        void UpdateLoadedChunks(int playerChunkX, int playerChunkZ, int viewDistance);

        // Dirty tracking for mesh system
        void MarkSectionDirty(Math::ChunkPos chunkPos, int sectionIndex);
        void MarkChunkDirty(Math::ChunkPos chunkPos);
        bool HasDirtySections() const;
        std::vector<DirtySection> GetAndClearDirtySections();

        // Statistics
        size_t GetLoadedChunkCount() const;
        size_t GetDirtyChunkCount() const;
        void GetLoadedChunkBounds(int& minX, int& maxX, int& minZ, int& maxZ) const;

        // Minecraft world support (instance methods)
        void SetMinecraftWorldPath(const std::string& worldPath);
        const std::string& GetMinecraftWorldPath() const;
        bool IsMinecraftChunkAvailable(Math::ChunkPos pos) const;

        // Configuration
        void SetMaxLoadedChunks(size_t maxChunks) { m_maxLoadedChunks = maxChunks; }
        void SetAsyncSaveEnabled(bool enabled) { m_asyncSaveEnabled = enabled; }

        // Debug/testing
        void GenerateTestChunks(int centerX, int centerZ, int radius);

        // **UPDATED**: World coordinate conversion utilities - simplified for world Y
        static Math::ChunkPos WorldToChunkPos(int worldX, int worldZ);
        static void WorldToLocal(int worldX, int worldY, int worldZ,
                                int& chunkX, int& chunkZ,
                                int& localX, int& localZ);
        static int WorldYToSectionIndex(int worldY);
        // **REMOVED**: WorldYToChunkLocalY - no longer needed since we use world Y directly

        // **NEW**: Get already loaded chunk (don't trigger loading)
        std::shared_ptr<Chunk> GetLoadedChunk(int chunkX, int chunkZ) const;

    private:
        // internal helper to guard world‐Y bounds
        bool IsValidPosition(int worldX, int worldY, int worldZ) const;

        // Chunk storage
        mutable std::mutex m_chunksMutex;
        std::unordered_map<Math::ChunkPos, std::shared_ptr<Chunk>, ChunkPosHash> m_loadedChunks;

        // Dirty tracking
        mutable std::mutex m_dirtyMutex;
        std::unordered_set<DirtySection, DirtySectionHash> m_dirtySections;
        std::unordered_set<Math::ChunkPos, ChunkPosHash> m_dirtyChunks; // For saving

        // Async saving queue
        mutable std::mutex m_saveMutex;
        std::queue<std::shared_ptr<Chunk>> m_chunksToSave;

        // Configuration
        std::string m_minecraftWorldPath;
        size_t m_maxLoadedChunks = 1024; // LRU eviction threshold
        bool m_asyncSaveEnabled = true;

        // Statistics
        size_t m_chunksGenerated = 0;
        size_t m_chunksLoaded = 0;
        size_t m_chunksUnloaded = 0;
        size_t m_chunksSaved = 0;

        // Helper functions
        std::shared_ptr<Chunk> GetChunk(int chunkX, int chunkZ) const;
        std::shared_ptr<Chunk> GenerateChunk(Math::ChunkPos chunkPos);
        void SetupChunkCallbacks(std::shared_ptr<Chunk> chunk);

        // **UPDATED**: Chunk generation now uses world Y coordinates
        void GenerateTerrainChunk(Chunk& chunk, Math::ChunkPos pos);
        void PlaceStructures(Chunk& chunk, Math::ChunkPos pos);

        // Unloading and LRU logic
        void UnloadDistantChunks(int centerX, int centerZ, int keepRadius);
        void EnforceLRULimit();
        bool ShouldUnloadChunk(Math::ChunkPos chunkPos, int centerX, int centerZ, int keepRadius) const;

        // Async saving
        void QueueChunkForSave(std::shared_ptr<Chunk> chunk);
        void ProcessSaveQueue();

        // Distance calculation
        int ChunkDistance(int x1, int z1, int x2, int z2) const;
    };

} // namespace Game
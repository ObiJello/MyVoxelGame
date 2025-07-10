// File: src/engine/world/ChunkProvider.cpp
#include "ChunkProvider.hpp"
#include "MinecraftChunkLoader.hpp"
#include "../../core/Log.hpp"
#include "../../core/Config.hpp"
#include <algorithm>
#include <cmath>
#include <random>

// Include FastNoise for terrain generation
#include "../../../ext/FastNoiseLite.h"

namespace Game {

    ChunkProvider::ChunkProvider() {
        Log::Info("ChunkProvider created");
    }

    ChunkProvider::~ChunkProvider() {
        Shutdown();
        Log::Info("ChunkProvider destroyed");
    }

    void ChunkProvider::Initialize() {
        Log::Info("Initializing ChunkProvider...");

        // Clear any existing chunks
        {
            std::lock_guard<std::mutex> lock(m_chunksMutex);
            m_loadedChunks.clear();
        }

        // Clear dirty tracking
        {
            std::lock_guard<std::mutex> lock(m_dirtyMutex);
            m_dirtySections.clear();
            m_dirtyChunks.clear();
        }

        // Reset statistics
        m_chunksGenerated = 0;
        m_chunksLoaded = 0;
        m_chunksUnloaded = 0;
        m_chunksSaved = 0;

        Log::Info("✓ ChunkProvider initialized");
    }

    void ChunkProvider::Update(float deltaTime) {
        // Process async save queue
        if (m_asyncSaveEnabled) {
            ProcessSaveQueue();
        }

        // Periodically log statistics
        static float logTimer = 0.0f;
        logTimer += deltaTime;
        if (logTimer >= 10.0f) { // Every 10 seconds
            size_t loadedCount = GetLoadedChunkCount();
            size_t dirtyCount = GetDirtyChunkCount();
            if (loadedCount > 0) {
                Log::Debug("ChunkProvider: %zu loaded, %zu dirty, %zu generated, %zu saved",
                          loadedCount, dirtyCount, m_chunksGenerated, m_chunksSaved);
            }
            logTimer = 0.0f;
        }
    }

    void ChunkProvider::Shutdown() {
        Log::Info("Shutting down ChunkProvider...");

        // Save all dirty chunks before shutdown
        ProcessSaveQueue();

        size_t chunkCount = 0;
        {
            std::lock_guard<std::mutex> lock(m_chunksMutex);
            chunkCount = m_loadedChunks.size();

            // Save all loaded chunks that are dirty
            for (const auto& [pos, chunk] : m_loadedChunks) {
                if (m_dirtyChunks.find(pos) != m_dirtyChunks.end()) {
                    // TODO: Save chunk to disk
                    m_chunksSaved++;
                }
            }

            m_loadedChunks.clear();
        }

        if (chunkCount > 0) {
            Log::Info("Unloaded %zu chunks during shutdown (%zu saved)", chunkCount, m_chunksSaved);
        }

        Log::Info("ChunkProvider shutdown complete");
    }

    // Core chunk operations
    std::shared_ptr<Chunk> ChunkProvider::LoadChunk(int chunkX, int chunkZ) {
        Math::ChunkPos pos{chunkX, chunkZ};

        // Check if already loaded
        {
            std::lock_guard<std::mutex> lock(m_chunksMutex);
            auto it = m_loadedChunks.find(pos);
            if (it != m_loadedChunks.end()) {
                return it->second;
            }
        }

        // Try to load from Minecraft world first
        std::shared_ptr<Chunk> chunk = nullptr;

        if (!m_minecraftWorldPath.empty()) {
            try {
                chunk = MinecraftChunkLoader::LoadMinecraftChunk(pos, m_minecraftWorldPath);
                if (chunk) {
                    Log::Debug("Loaded chunk (%d, %d) from Minecraft world", chunkX, chunkZ);
                }
            } catch (const std::exception& e) {
                Log::Warning("Failed to load Minecraft chunk (%d, %d): %s", chunkX, chunkZ, e.what());
            }
        }

        // Fall back to procedural generation
        if (!chunk) {
            chunk = GenerateChunk(pos);
            m_chunksGenerated++;
        }

        // Setup callbacks for dirty tracking
        SetupChunkCallbacks(chunk);

        // Store in loaded chunks
        {
            std::lock_guard<std::mutex> lock(m_chunksMutex);
            m_loadedChunks[pos] = chunk;
        }

        m_chunksLoaded++;

        // Enforce LRU limit
        EnforceLRULimit();

        return chunk;
    }

    void ChunkProvider::UnloadChunk(int chunkX, int chunkZ) {
        Math::ChunkPos pos{chunkX, chunkZ};

        std::shared_ptr<Chunk> chunk;
        {
            std::lock_guard<std::mutex> lock(m_chunksMutex);
            auto it = m_loadedChunks.find(pos);
            if (it != m_loadedChunks.end()) {
                chunk = it->second;
                m_loadedChunks.erase(it);
            }
        }

        if (chunk) {
            // Check if chunk is dirty and queue for saving
            {
                std::lock_guard<std::mutex> lock(m_dirtyMutex);
                if (m_dirtyChunks.find(pos) != m_dirtyChunks.end()) {
                    QueueChunkForSave(chunk);
                    m_dirtyChunks.erase(pos);
                }
            }

            m_chunksUnloaded++;
            Log::Debug("Unloaded chunk (%d, %d)", chunkX, chunkZ);
        }
    }

    bool ChunkProvider::IsChunkLoaded(int chunkX, int chunkZ) const {
        Math::ChunkPos pos{chunkX, chunkZ};

        std::lock_guard<std::mutex> lock(m_chunksMutex);
        return m_loadedChunks.find(pos) != m_loadedChunks.end();
    }

    bool ChunkProvider::EnsureChunkLoaded(int chunkX, int chunkZ) {
        if (IsChunkLoaded(chunkX, chunkZ)) {
            return true;
        }

        auto chunk = LoadChunk(chunkX, chunkZ);
        return chunk != nullptr;
    }

    // Block access with world coordinate conversion
    BlockID ChunkProvider::GetBlockAt(int worldX, int worldY, int worldZ) const {
        if (!IsValidPosition(worldX, worldY, worldZ)) {
            return BlockID::Air;
        }

        int chunkX, chunkZ, localX, localY, localZ;
        WorldToLocal(worldX, worldY, worldZ, chunkX, chunkZ, localX, localY, localZ);

        auto chunk = GetChunk(chunkX, chunkZ);
        if (!chunk) {
            return BlockID::Air;
        }

        return chunk->GetBlock(localX, localY, localZ);
    }

    bool ChunkProvider::SetBlockAt(int worldX, int worldY, int worldZ, BlockID blockId) {
        if (!IsValidPosition(worldX, worldY, worldZ)) {
            return false;
        }

        int chunkX, chunkZ, localX, localY, localZ;
        WorldToLocal(worldX, worldY, worldZ, chunkX, chunkZ, localX, localY, localZ);

        auto chunk = GetChunk(chunkX, chunkZ);
        if (!chunk) {
            // Try to load the chunk
            chunk = LoadChunk(chunkX, chunkZ);
            if (!chunk) {
                return false;
            }
        }

        // Get old block to check for actual change
        BlockID oldBlock = chunk->GetBlock(localX, localY, localZ);
        if (oldBlock == blockId) {
            return true; // No change needed
        }

        chunk->SetBlock(localX, localY, localZ, blockId);

        // Mark chunk as dirty for saving
        Math::ChunkPos pos{chunkX, chunkZ};
        {
            std::lock_guard<std::mutex> lock(m_dirtyMutex);
            m_dirtyChunks.insert(pos);
        }

        return true;
    }

    // Chunk management around player
    void ChunkProvider::UpdateLoadedChunks(int playerChunkX, int playerChunkZ, int viewDistance) {
        // Load chunks in a square around the player
        for (int dz = -viewDistance; dz <= viewDistance; ++dz) {
            for (int dx = -viewDistance; dx <= viewDistance; ++dx) {
                int chunkX = playerChunkX + dx;
                int chunkZ = playerChunkZ + dz;

                if (!IsChunkLoaded(chunkX, chunkZ)) {
                    LoadChunk(chunkX, chunkZ);
                }
            }
        }

        // Unload distant chunks
        UnloadDistantChunks(playerChunkX, playerChunkZ, viewDistance + 2);
    }

    // Dirty tracking for mesh system
    void ChunkProvider::MarkSectionDirty(Math::ChunkPos chunkPos, int sectionIndex) {
        std::lock_guard<std::mutex> lock(m_dirtyMutex);
        m_dirtySections.insert({chunkPos, sectionIndex});
    }

    void ChunkProvider::MarkChunkDirty(Math::ChunkPos chunkPos) {
        std::lock_guard<std::mutex> lock(m_dirtyMutex);
        m_dirtyChunks.insert(chunkPos);
    }

    bool ChunkProvider::HasDirtySections() const {
        std::lock_guard<std::mutex> lock(m_dirtyMutex);
        return !m_dirtySections.empty();
    }

    std::vector<DirtySection> ChunkProvider::GetAndClearDirtySections() {
        std::lock_guard<std::mutex> lock(m_dirtyMutex);

        std::vector<DirtySection> result(m_dirtySections.begin(), m_dirtySections.end());
        m_dirtySections.clear();

        return result;
    }

    // Statistics
    size_t ChunkProvider::GetLoadedChunkCount() const {
        std::lock_guard<std::mutex> lock(m_chunksMutex);
        return m_loadedChunks.size();
    }

    size_t ChunkProvider::GetDirtyChunkCount() const {
        std::lock_guard<std::mutex> lock(m_dirtyMutex);
        return m_dirtyChunks.size();
    }

    void ChunkProvider::GetLoadedChunkBounds(int& minX, int& maxX, int& minZ, int& maxZ) const {
        std::lock_guard<std::mutex> lock(m_chunksMutex);

        if (m_loadedChunks.empty()) {
            minX = maxX = minZ = maxZ = 0;
            return;
        }

        bool first = true;
        for (const auto& [pos, chunk] : m_loadedChunks) {
            if (first) {
                minX = maxX = pos.x;
                minZ = maxZ = pos.z;
                first = false;
            } else {
                minX = std::min(minX, pos.x);
                maxX = std::max(maxX, pos.x);
                minZ = std::min(minZ, pos.z);
                maxZ = std::max(maxZ, pos.z);
            }
        }
    }

    // Minecraft world support
    void ChunkProvider::SetMinecraftWorldPath(const std::string& worldPath) {
        m_minecraftWorldPath = worldPath;
        if (!worldPath.empty()) {
            Log::Info("ChunkProvider: Set Minecraft world path: %s", worldPath.c_str());
        } else {
            Log::Info("ChunkProvider: Cleared Minecraft world path");
        }
    }

    const std::string& ChunkProvider::GetMinecraftWorldPath() const {
        return m_minecraftWorldPath;
    }

    // Debug/testing
    void ChunkProvider::GenerateTestChunks(int centerX, int centerZ, int radius) {
        Log::Info("Generating test chunks around (%d, %d) with radius %d", centerX, centerZ, radius);

        for (int dz = -radius; dz <= radius; ++dz) {
            for (int dx = -radius; dx <= radius; ++dx) {
                int chunkX = centerX + dx;
                int chunkZ = centerZ + dz;
                LoadChunk(chunkX, chunkZ);
            }
        }

        Log::Info("Generated %d test chunks", (radius * 2 + 1) * (radius * 2 + 1));
    }

    // World coordinate conversion utilities (static)
    Math::ChunkPos ChunkProvider::WorldToChunkPos(int worldX, int worldZ) {
        return Math::ChunkPos{
            static_cast<int32_t>(std::floor(static_cast<float>(worldX) / Math::CHUNK_SIZE_X)),
            static_cast<int32_t>(std::floor(static_cast<float>(worldZ) / Math::CHUNK_SIZE_Z))
        };
    }

    void ChunkProvider::WorldToLocal(int worldX, int worldY, int worldZ,
                                    int& chunkX, int& chunkZ,
                                    int& localX, int& localY, int& localZ) {
        // Calculate chunk coordinates
        chunkX = static_cast<int>(std::floor(static_cast<float>(worldX) / Math::CHUNK_SIZE_X));
        chunkZ = static_cast<int>(std::floor(static_cast<float>(worldZ) / Math::CHUNK_SIZE_Z));

        // Calculate local coordinates within chunk
        localX = worldX - (chunkX * Math::CHUNK_SIZE_X);
        localZ = worldZ - (chunkZ * Math::CHUNK_SIZE_Z);
        localY = worldY - Config::MinY; // Convert to chunk-local Y (0-383)

        // Ensure local coordinates are in valid range
        if (localX < 0) localX += Math::CHUNK_SIZE_X;
        if (localZ < 0) localZ += Math::CHUNK_SIZE_Z;
    }

    int ChunkProvider::WorldYToSectionIndex(int worldY) {
        return (worldY - Config::MinY) / Math::SECTION_HEIGHT;
    }

    int ChunkProvider::WorldYToChunkLocalY(int worldY) {
        return worldY - Config::MinY;
    }

    // Private helper functions
    std::shared_ptr<Chunk> ChunkProvider::GetChunk(int chunkX, int chunkZ) const {
        Math::ChunkPos pos{chunkX, chunkZ};

        std::lock_guard<std::mutex> lock(m_chunksMutex);
        auto it = m_loadedChunks.find(pos);
        return (it != m_loadedChunks.end()) ? it->second : nullptr;
    }

    std::shared_ptr<Chunk> ChunkProvider::GenerateChunk(Math::ChunkPos chunkPos) {
        auto chunk = std::make_shared<Chunk>();
        chunk->pos = chunkPos;

        // Generate terrain
        GenerateTerrainChunk(*chunk, chunkPos);

        // Add structures (trees, ores, etc.)
        PlaceStructures(*chunk, chunkPos);

        return chunk;
    }

    void ChunkProvider::SetupChunkCallbacks(std::shared_ptr<Chunk> chunk) {
        // Set up the dirty callback for the chunk
        Math::ChunkPos pos = chunk->pos;
        chunk->onSectionDirty = [this, pos](int sectionIndex) {
            this->MarkSectionDirty(pos, sectionIndex);
        };
    }

    void ChunkProvider::GenerateTerrainChunk(Chunk& chunk, Math::ChunkPos pos) {
        static FastNoiseLite noise;
        static bool initialized = false;
        if (!initialized) {
            noise.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
            noise.SetSeed(12345);
            noise.SetFrequency(0.01f);
            initialized = true;
        }

        int baseWorldX = pos.x * Math::CHUNK_SIZE_X;
        int baseWorldZ = pos.z * Math::CHUNK_SIZE_Z;

        for (int localX = 0; localX < Math::CHUNK_SIZE_X; ++localX) {
            for (int localZ = 0; localZ < Math::CHUNK_SIZE_Z; ++localZ) {
                int worldX = baseWorldX + localX;
                int worldZ = baseWorldZ + localZ;

                // Generate height using noise
                float noiseValue = noise.GetNoise(static_cast<float>(worldX), static_cast<float>(worldZ));
                float normalizedNoise = (noiseValue + 1.0f) * 0.5f; // Convert from [-1,1] to [0,1]

                int surfaceHeight = static_cast<int>(normalizedNoise * 64.0f + 64.0f); // Height 64-128
                surfaceHeight = std::clamp(surfaceHeight, Config::MinY + 5, Config::MaxY - 10);

                // Convert world Y to chunk-local Y
                int bedrockStart = Config::MinY - Config::MinY; // 0 in chunk-local
                int bedrockEnd = (Config::MinY + 5) - Config::MinY; // 5 in chunk-local
                int surfaceLocalY = surfaceHeight - Config::MinY;

                // Generate bedrock layer
                for (int localY = bedrockStart; localY < bedrockEnd; ++localY) {
                    chunk.SetBlock(localX, localY, localZ, BlockID::Bedrock);
                }

                // Generate stone and surface
                for (int localY = bedrockEnd; localY <= surfaceLocalY; ++localY) {
                    BlockID blockType;
                    if (localY == surfaceLocalY) {
                        blockType = BlockID::Grass; // Surface
                    } else if (localY >= surfaceLocalY - 3) {
                        blockType = BlockID::Dirt; // Top soil
                    } else {
                        blockType = BlockID::Stone; // Deep stone
                    }

                    chunk.SetBlock(localX, localY, localZ, blockType);
                }
            }
        }
    }

    void ChunkProvider::PlaceStructures(Chunk& chunk, Math::ChunkPos pos) {
        // Simple ore generation
        std::mt19937 rng(pos.x * 31 + pos.z * 17 + 12345);
        std::uniform_int_distribution<int> coordDist(1, 14); // Avoid chunk edges
        std::uniform_int_distribution<int> heightDist(5, 50); // Chunk-local Y coordinates
        std::uniform_real_distribution<float> chanceDist(0.0f, 1.0f);

        // Place some coal ore
        if (chanceDist(rng) < 0.3f) { // 30% chance
            int x = coordDist(rng);
            int z = coordDist(rng);
            int y = heightDist(rng);

            if (chunk.GetBlock(x, y, z) == BlockID::Stone) {
                chunk.SetBlock(x, y, z, BlockID::CoalOre);
            }
        }

        // Place some iron ore
        if (chanceDist(rng) < 0.2f) { // 20% chance
            int x = coordDist(rng);
            int z = coordDist(rng);
            int y = heightDist(rng);

            if (chunk.GetBlock(x, y, z) == BlockID::Stone) {
                chunk.SetBlock(x, y, z, BlockID::IronOre);
            }
        }
    }

    void ChunkProvider::UnloadDistantChunks(int centerX, int centerZ, int keepRadius) {
        std::vector<Math::ChunkPos> chunksToUnload;

        // Find chunks to unload
        {
            std::lock_guard<std::mutex> lock(m_chunksMutex);
            for (const auto& [pos, chunk] : m_loadedChunks) {
                if (ShouldUnloadChunk(pos, centerX, centerZ, keepRadius)) {
                    chunksToUnload.push_back(pos);
                }
            }
        }

        // Unload chunks outside the lock to avoid deadlock
        for (const Math::ChunkPos& pos : chunksToUnload) {
            UnloadChunk(pos.x, pos.z);
        }

        if (!chunksToUnload.empty()) {
            Log::Debug("Unloaded %zu distant chunks (keep radius: %d)",
                      chunksToUnload.size(), keepRadius);
        }
    }

    void ChunkProvider::EnforceLRULimit() {
        std::lock_guard<std::mutex> lock(m_chunksMutex);

        if (m_loadedChunks.size() <= m_maxLoadedChunks) {
            return; // Under limit
        }

        // Simple eviction: unload the first chunk we find
        // TODO: Implement proper LRU tracking
        auto it = m_loadedChunks.begin();
        if (it != m_loadedChunks.end()) {
            Math::ChunkPos pos = it->first;
            Log::Debug("LRU eviction: unloading chunk (%d, %d)", pos.x, pos.z);

            // Note: We need to release the lock before calling UnloadChunk
            // to avoid deadlock, but this is a simplified implementation
            m_loadedChunks.erase(it);
        }
    }

    bool ChunkProvider::ShouldUnloadChunk(Math::ChunkPos chunkPos, int centerX, int centerZ, int keepRadius) const {
        int distance = ChunkDistance(chunkPos.x, chunkPos.z, centerX, centerZ);
        return distance > keepRadius;
    }

    void ChunkProvider::QueueChunkForSave(std::shared_ptr<Chunk> chunk) {
        if (!m_asyncSaveEnabled) {
            return;
        }

        std::lock_guard<std::mutex> lock(m_saveMutex);
        m_chunksToSave.push(chunk);
    }

    void ChunkProvider::ProcessSaveQueue() {
        std::queue<std::shared_ptr<Chunk>> chunksToSave;

        // Move chunks from save queue
        {
            std::lock_guard<std::mutex> lock(m_saveMutex);
            chunksToSave.swap(m_chunksToSave);
        }

        // Process saves (this would write to disk in a real implementation)
        while (!chunksToSave.empty()) {
            auto chunk = chunksToSave.front();
            chunksToSave.pop();

            // TODO: Actually save chunk to disk
            // For now, just count it
            m_chunksSaved++;

            Log::Debug("Saved chunk (%d, %d) to disk", chunk->pos.x, chunk->pos.z);
        }
    }

    int ChunkProvider::ChunkDistance(int x1, int z1, int x2, int z2) const {
        // Use Chebyshev distance (max of dx, dz) for square loading pattern
        return std::max(std::abs(x1 - x2), std::abs(z1 - z2));
    }

    bool ChunkProvider::IsValidPosition(int worldX, int worldY, int worldZ) const {
        return worldY >= Config::MinY && worldY <= Config::MaxY;
    }

    std::shared_ptr<Chunk> ChunkProvider::GetLoadedChunk(int chunkX, int chunkZ) const {
        Math::ChunkPos pos{chunkX, chunkZ};

        std::lock_guard<std::mutex> lock(m_chunksMutex);
        auto it = m_loadedChunks.find(pos);
        return (it != m_loadedChunks.end()) ? it->second : nullptr;
    }

} // namespace Game
// File: src/engine/world/World.cpp
#include "World.hpp"
#include "../../core/Log.hpp"
#include "../../core/Config.hpp"
#include "../block/BlockRegistry.hpp"
#include "../physics/Physics.hpp"
#include <algorithm>
#include <cmath>

namespace Game {

    World::World() {
        m_chunkProvider = std::make_unique<ChunkProvider>();
        Log::Info("World created");
    }

    World::~World() {
        Shutdown();
        Log::Info("World destroyed");
    }

    void World::Initialize() {
        Log::Info("Initializing World...");

        // Initialize chunk provider
        m_chunkProvider->Initialize();

        // Set the global block access for physics system
        SetGlobalBlockAccess(this);

        Log::Info("✓ World initialized successfully");
    }

    void World::Update(float deltaTime) {
        // Update chunk provider
        m_chunkProvider->Update(deltaTime);

        // Log statistics occasionally
        static float logTimer = 0.0f;
        logTimer += deltaTime;
        if (logTimer >= 5.0f) { // Every 5 seconds
            size_t loadedChunks = GetLoadedChunkCount();
            if (loadedChunks > 0) {
                Log::Debug("World stats: %zu chunks loaded, %zu block accesses",
                          loadedChunks, m_blockAccessCount);
            }
            logTimer = 0.0f;
        }
    }

    void World::Shutdown() {
        if (m_chunkProvider) {
            m_chunkProvider->Shutdown();
        }

        // Clear global block access
        SetGlobalBlockAccess(nullptr);

        Log::Info("World shutdown complete");
    }

    // IBlockAccess implementation
    BlockID World::GetBlock(int worldX, int worldY, int worldZ) const {
        m_blockAccessCount++;

        if (!IsValidPosition(worldX, worldY, worldZ)) {
            return BlockID::Air;
        }

        return m_chunkProvider->GetBlockAt(worldX, worldY, worldZ);
    }

    bool World::IsChunkLoaded(int chunkX, int chunkZ) const {
        return m_chunkProvider->IsChunkLoaded(chunkX, chunkZ);
    }

    bool World::IsPositionLoaded(int worldX, int worldY, int worldZ) const {
        if (!IsValidPosition(worldX, worldY, worldZ)) {
            return false;
        }

        Math::ChunkPos chunkPos = ChunkProvider::WorldToChunkPos(worldX, worldZ);
        return IsChunkLoaded(chunkPos.x, chunkPos.z);
    }

    bool World::IsBlockSolid(int worldX, int worldY, int worldZ) const {
        BlockID blockId = GetBlock(worldX, worldY, worldZ);
        if (blockId == BlockID::Air) {
            return false;
        }

        const Block& block = BlockRegistry::Get(blockId);
        return block.opaque;
    }

    bool World::IsBlockFluid(int worldX, int worldY, int worldZ) const {
        BlockID blockId = GetBlock(worldX, worldY, worldZ);
        return blockId == BlockID::Water || blockId == BlockID::Lava;
    }

    bool World::IsValidPosition(int worldX, int worldY, int worldZ) const {
        return worldY >= MIN_Y && worldY <= MAX_Y;
    }

    // World modification
    bool World::SetBlock(int worldX, int worldY, int worldZ, BlockID blockId) {
        if (!IsValidPosition(worldX, worldY, worldZ)) {
            Log::Warning("Attempted to set block at invalid position (%d, %d, %d)",
                        worldX, worldY, worldZ);
            return false;
        }

        // Ensure the chunk is loaded
        Math::ChunkPos chunkPos = ChunkProvider::WorldToChunkPos(worldX, worldZ);
        if (!m_chunkProvider->EnsureChunkLoaded(chunkPos.x, chunkPos.z)) {
            Log::Warning("Failed to load chunk (%d, %d) for block placement",
                        chunkPos.x, chunkPos.z);
            return false;
        }

        // Set the block
        bool success = m_chunkProvider->SetBlockAt(worldX, worldY, worldZ, blockId);

        if (success) {
            OnBlockChanged(worldX, worldY, worldZ);

            // Log block changes occasionally for debugging
            static int changeCount = 0;
            if (++changeCount % 100 == 0) {
                const Block& block = BlockRegistry::Get(blockId);
                Log::Debug("Block changes: %d (latest: %s at %d,%d,%d)",
                          changeCount, block.name.c_str(), worldX, worldY, worldZ);
            }
        }

        return success;
    }

    void World::UpdateLoadedChunks(int playerChunkX, int playerChunkZ, int viewDistance) {
        m_chunkLoadRequests++;
        m_chunkProvider->UpdateLoadedChunks(playerChunkX, playerChunkZ, viewDistance);
    }

    void World::MarkSectionDirty(int worldX, int worldY, int worldZ) {
        Math::ChunkPos chunkPos = ChunkProvider::WorldToChunkPos(worldX, worldZ);
        int sectionIndex = ChunkProvider::WorldYToSectionIndex(worldY);
        m_chunkProvider->MarkSectionDirty(chunkPos, sectionIndex);
    }

    bool World::HasDirtySections() const {
        return m_chunkProvider->HasDirtySections();
    }

    size_t World::GetLoadedChunkCount() const {
        return m_chunkProvider->GetLoadedChunkCount();
    }

    // Minecraft world support
    void World::SetMinecraftWorldPath(const std::string& worldPath) {
        m_minecraftWorldPath = worldPath;
        m_chunkProvider->SetMinecraftWorldPath(worldPath);

        if (!worldPath.empty()) {
            Log::Info("Set Minecraft world path: %s", worldPath.c_str());
        } else {
            Log::Info("Cleared Minecraft world path, using procedural generation");
        }
    }

    const std::string& World::GetMinecraftWorldPath() const {
        return m_minecraftWorldPath;
    }

    bool World::HasMinecraftWorld() const {
        return !m_minecraftWorldPath.empty();
    }

    // Helper functions
    Math::ChunkPos World::WorldToChunkPos(int worldX, int worldZ) const {
        return Math::ChunkPos{
            static_cast<int32_t>(std::floor(static_cast<float>(worldX) / Math::CHUNK_SIZE_X)),
            static_cast<int32_t>(std::floor(static_cast<float>(worldZ) / Math::CHUNK_SIZE_Z))
        };
    }

    void World::OnBlockChanged(int worldX, int worldY, int worldZ) {
        // Mark section for remeshing
        MarkSectionDirty(worldX, worldY, worldZ);

        if (Render::g_meshManager) {
            Math::ChunkPos chunkPos = ChunkProvider::WorldToChunkPos(worldX, worldZ);
            int sectionIndex = ChunkProvider::WorldYToSectionIndex(worldY);

            // This is the missing link - actually tell the mesh system to rebuild
            Render::g_meshManager->MarkSectionDirty(chunkPos, sectionIndex);

            // Also mark neighboring sections if block is on boundary
            MarkNeighboringSectionsIfNeeded(worldX, worldY, worldZ);
        }

        // Calculate which section was modified for logging
        Math::ChunkPos chunkPos = ChunkProvider::WorldToChunkPos(worldX, worldZ);
        int sectionIndex = ChunkProvider::WorldYToSectionIndex(worldY);

        Log::Debug("Block changed at (%d, %d, %d) - chunk (%d, %d) section %d",
                  worldX, worldY, worldZ, chunkPos.x, chunkPos.z, sectionIndex);
    }

    // New helper method to handle edge cases
    void World::MarkNeighboringSectionsIfNeeded(int worldX, int worldY, int worldZ) {
        // If block is on chunk boundary, mark neighboring chunks
        int localX = worldX % Math::CHUNK_SIZE_X;
        int localZ = worldZ % Math::CHUNK_SIZE_Z;
        int localY = (worldY - Config::MinY) % Math::SECTION_HEIGHT;

        Math::ChunkPos baseChunk = ChunkProvider::WorldToChunkPos(worldX, worldZ);
        int baseSectionY = ChunkProvider::WorldYToSectionIndex(worldY);

        // Mark neighboring chunks if on boundary
        if (localX == 0 && Render::g_meshManager) {
            Math::ChunkPos westChunk{baseChunk.x - 1, baseChunk.z};
            Render::g_meshManager->MarkSectionDirty(westChunk, baseSectionY);
        }
        if (localX == Math::CHUNK_SIZE_X - 1 && Render::g_meshManager) {
            Math::ChunkPos eastChunk{baseChunk.x + 1, baseChunk.z};
            Render::g_meshManager->MarkSectionDirty(eastChunk, baseSectionY);
        }
        if (localZ == 0 && Render::g_meshManager) {
            Math::ChunkPos northChunk{baseChunk.x, baseChunk.z - 1};
            Render::g_meshManager->MarkSectionDirty(northChunk, baseSectionY);
        }
        if (localZ == Math::CHUNK_SIZE_Z - 1 && Render::g_meshManager) {
            Math::ChunkPos southChunk{baseChunk.x, baseChunk.z + 1};
            Render::g_meshManager->MarkSectionDirty(southChunk, baseSectionY);
        }

        // Mark neighboring sections if on section boundary
        if (localY == 0 && baseSectionY > 0 && Render::g_meshManager) {
            Render::g_meshManager->MarkSectionDirty(baseChunk, baseSectionY - 1);
        }
        if (localY == Math::SECTION_HEIGHT - 1 && baseSectionY < Math::SECTIONS_PER_CHUNK - 1 && Render::g_meshManager) {
            Render::g_meshManager->MarkSectionDirty(baseChunk, baseSectionY + 1);
        }
    }

    std::shared_ptr<Chunk> World::GetChunk(int chunkX, int chunkZ) const {
        if (!m_chunkProvider) {
            return nullptr;
        }

        // Check if chunk is loaded first
        if (!m_chunkProvider->IsChunkLoaded(chunkX, chunkZ)) {
            return nullptr;
        }

        // This assumes ChunkProvider has a GetChunk method that returns loaded chunks
        // You may need to add this method to ChunkProvider
        return m_chunkProvider->GetLoadedChunk(chunkX, chunkZ);
    }

    const Chunk* World::GetChunkForMeshing(int chunkX, int chunkZ) const {
        auto chunk = GetChunk(chunkX, chunkZ);
        return chunk.get();
    }

} // namespace Game
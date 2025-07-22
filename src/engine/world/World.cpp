// File: src/engine/world/World.cpp
#include "World.hpp"
#include "../../core/Log.hpp"
#include "../../core/Config.hpp"
#include "../block/BlockRegistry.hpp"
#include "../physics/Physics.hpp"
#include "../../platform/GameDirectory.hpp"
#include <algorithm>
#include <cmath>

#include "mesh/MeshManager.hpp"
#include "physics/RayCast.hpp"

namespace Game {

    World::World() {
        LoadWorldSettings();
        Log::Info("World created (render distance: %d chunks)", m_renderDistance);
    }

    World::~World() {
        Shutdown();
        Log::Info("World destroyed");
    }

    void World::Initialize() {
        Log::Info("=== SIMPLIFIED WORLD INITIALIZATION START ===");

        // Load world settings from game settings
        LoadWorldSettings();
        Log::Info("World settings loaded - render distance: %d", m_renderDistance);

        // Create and configure chunk provider
        ChunkProviderConfig config = CreateDefaultConfig();

        // Configure based on world settings - square area calculation
        int squareSize = 2 * m_renderDistance + 1;
        config.maxLoadedChunks = squareSize * squareSize + 50; // Add buffer for loading/unloading
        config.enableFallbackGeneration = false;

        Log::Info("Config - Max chunks: %zu for %dx%d square, Fallback generation: %s",
                  config.maxLoadedChunks, squareSize, squareSize,
                  config.enableFallbackGeneration ? "enabled" : "disabled");

        // Set up generation config
        config.generationConfig.seed = 123764;
        config.generationConfig.worldType = "default";
        config.generationConfig.generateOres = true;
        config.generationConfig.generateCaves = true;
        config.generationConfig.generateStructures = true;
        config.generationConfig.generateVegetation = true;

        // Set up dirty tracking config
        config.dirtyConfig.enableNeighborInvalidation = true;

        // Set Minecraft world path if available
        if (!m_minecraftWorldPath.empty()) {
            config.minecraftWorldPath = m_minecraftWorldPath;
            Log::Info("Using Minecraft world path: %s", m_minecraftWorldPath.c_str());
        } else {
            Log::Info("No Minecraft world path set, using procedural generation only");
        }

        // Validate config before creating chunk provider
        if (!config.IsValid()) {
            Log::Error("ChunkProviderConfig validation failed!");
            return;
        }

        // Create chunk provider with config
        Log::Info("Creating ChunkProvider...");
        try {
            m_chunkProvider = std::make_unique<ChunkProvider>(config);
            Log::Info("ChunkProvider created successfully");
        } catch (const std::exception& e) {
            Log::Error("Failed to create ChunkProvider: %s", e.what());
            return;
        }

        // Initialize chunk provider
        Log::Info("Initializing ChunkProvider...");
        if (!m_chunkProvider->Initialize()) {
            Log::Error("Failed to initialize ChunkProvider");
            m_chunkProvider.reset();
            return;
        }

        // Set the global block access for physics system
        SetGlobalBlockAccess(this);

        Log::Info("✓ World initialized successfully (render distance: %d chunks)", m_renderDistance);
        Log::Info("=== SIMPLIFIED WORLD INITIALIZATION COMPLETE ===");
    }

    void World::Shutdown() {
        if (m_chunkProvider) {
            m_chunkProvider->Shutdown();
            m_chunkProvider.reset();
        }

        // Clear global block access
        SetGlobalBlockAccess(nullptr);

        Log::Info("World shutdown complete");
    }

    void World::RefreshSettings() {
        LoadWorldSettings();
        Log::Info("World settings refreshed (render distance: %d chunks)", m_renderDistance);
    }

    void World::LoadWorldSettings() {
        // Load render distance from game settings
        m_renderDistance = Platform::g_gameSettings.GetRenderDistance();

        // Clamp to reasonable values
        m_renderDistance = std::clamp(m_renderDistance, 4, 32);

        Log::Debug("Loaded world settings: render distance=%d chunks", m_renderDistance);
    }

    // IBlockAccess implementation
    BlockID World::GetBlock(int worldX, int worldY, int worldZ) const {
        m_blockAccessCount++;

        if (!IsValidPosition(worldX, worldY, worldZ)) {
            return BlockID::Air;
        }

        if (!m_chunkProvider) {
            return BlockID::Air;
        }

        return m_chunkProvider->GetBlock(worldX, worldY, worldZ);
    }

    bool World::IsChunkLoaded(int chunkX, int chunkZ) const {
        if (!m_chunkProvider) {
            return false;
        }

        return m_chunkProvider->IsChunkLoaded(chunkX, chunkZ);
    }

    bool World::IsPositionLoaded(int worldX, int worldY, int worldZ) const {
        if (!IsValidPosition(worldX, worldY, worldZ)) {
            return false;
        }

        Math::ChunkPos chunkPos = Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
        return IsChunkLoaded(chunkPos.x, chunkPos.z);
    }

    bool World::IsBlockSolid(int worldX, int worldY, int worldZ) const {
        if (!m_chunkProvider) {
            return false;
        }

        return m_chunkProvider->IsBlockSolid(worldX, worldY, worldZ);
    }

    bool World::IsBlockFluid(int worldX, int worldY, int worldZ) const {
        if (!m_chunkProvider) {
            return false;
        }

        return m_chunkProvider->IsBlockFluid(worldX, worldY, worldZ);
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

        if (!m_chunkProvider) {
            Log::Warning("No chunk provider available for block placement");
            return false;
        }

        // Set the block using the chunk provider
        m_chunkProvider->SetBlock(worldX, worldY, worldZ, blockId);

        // Notify about block change
        OnBlockChanged(worldX, worldY, worldZ);

        return true;
    }

    void World::UpdateLoadedChunks(int playerChunkX, int playerChunkZ, int viewDistance) {
        if (!m_chunkProvider) {
            return;
        }

        // Use setting-based view distance if not provided
        if (viewDistance <= 0) {
            viewDistance = m_renderDistance;
        }

        // Create list of all chunk positions in the square area
        std::vector<Math::ChunkPos> chunksToLoad;
        for (int dx = -viewDistance; dx <= viewDistance; ++dx) {
            for (int dz = -viewDistance; dz <= viewDistance; ++dz) {
                int chunkX = playerChunkX + dx;
                int chunkZ = playerChunkZ + dz;
                Math::ChunkPos chunkPos{chunkX, chunkZ};

                // Only add chunks that aren't already loaded
                if (!m_chunkProvider->IsChunkLoaded(chunkPos)) {
                    chunksToLoad.push_back(chunkPos);
                }
            }
        }

        // Sort chunks by distance from player (nearest first)
        std::sort(chunksToLoad.begin(), chunksToLoad.end(),
                 [playerChunkX, playerChunkZ](const Math::ChunkPos& a, const Math::ChunkPos& b) {
                     // Calculate squared distance (avoid sqrt for performance)
                     int distASq = (a.x - playerChunkX) * (a.x - playerChunkX) +
                                   (a.z - playerChunkZ) * (a.z - playerChunkZ);
                     int distBSq = (b.x - playerChunkX) * (b.x - playerChunkX) +
                                   (b.z - playerChunkZ) * (b.z - playerChunkZ);
                     return distASq < distBSq;
                 });

        Log::Debug("Loading %zu chunks in %dx%d square around player chunk (%d, %d) - nearest first",
                  chunksToLoad.size(), 2 * viewDistance + 1, 2 * viewDistance + 1,
                  playerChunkX, playerChunkZ);

        size_t chunksLoaded = 0;
        size_t totalChunksInArea = (2 * viewDistance + 1) * (2 * viewDistance + 1);
        size_t chunksSkipped = totalChunksInArea - chunksToLoad.size();

        // Load chunks in order of proximity to player
        for (const auto& chunkPos : chunksToLoad) {
            // Load the chunk
            auto chunk = m_chunkProvider->GetChunk(chunkPos);
            if (chunk) {
                chunksLoaded++;

                // Mark all sections in this chunk for meshing
                if (Render::g_meshManager) {
                    for (int sectionY = 0; sectionY < Math::SECTIONS_PER_CHUNK; ++sectionY) {
                        Render::g_meshManager->MarkSectionDirty(chunkPos, sectionY);
                    }
                }

                // Log progress for nearest chunks
                int distanceSq = (chunkPos.x - playerChunkX) * (chunkPos.x - playerChunkX) +
                                (chunkPos.z - playerChunkZ) * (chunkPos.z - playerChunkZ);
                if (distanceSq <= 4) { // Within 2 chunk radius
                    Log::Debug("Loaded nearby chunk (%d, %d) - distance: %.1f",
                              chunkPos.x, chunkPos.z, std::sqrt(distanceSq));
                }
            } else {
                Log::Warning("Failed to load chunk (%d, %d)", chunkPos.x, chunkPos.z);
            }
        }

        // Unload chunks outside the square area
        UnloadDistantChunks(playerChunkX, playerChunkZ, viewDistance + 2);

        if (chunksLoaded > 0) {
            Log::Info("Chunk loading complete: %zu new chunks loaded, %zu already loaded (nearest-first order)",
                     chunksLoaded, chunksSkipped);
        }
    }

    void World::MarkSectionDirty(int worldX, int worldY, int worldZ) {
        if (!m_chunkProvider) {
            return;
        }

        m_chunkProvider->MarkBlockDirty(worldX, worldY, worldZ);
    }

    bool World::HasDirtySections() const {
        if (!m_chunkProvider) {
            return false;
        }

        return m_chunkProvider->GetDirtyCount() > 0;
    }

    size_t World::GetLoadedChunkCount() const {
        if (!m_chunkProvider) {
            return 0;
        }

        return m_chunkProvider->GetLoadedChunkCount();
    }

    // Minecraft world support
    void World::SetMinecraftWorldPath(const std::string& worldPath) {
        m_minecraftWorldPath = worldPath;

        if (m_chunkProvider) {
            m_chunkProvider->SetWorldPath(worldPath);
        }

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
        return Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
    }

    void World::OnBlockChanged(int worldX, int worldY, int worldZ) {
        // Mark section for remeshing
        MarkSectionDirty(worldX, worldY, worldZ);

        // Use the global mesh manager
        if (Render::g_meshManager) {
            Math::ChunkPos chunkPos = Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
            int sectionIndex = Math::WorldCoordinates::WorldYToSectionIndex(worldY);

            // Mark the section dirty in the mesh system
            Render::g_meshManager->MarkSectionDirty(chunkPos, sectionIndex);

            // Also mark neighboring sections if block is on boundary
            MarkNeighboringSectionsIfNeeded(worldX, worldY, worldZ);
        }
    }

    void World::MarkNeighboringSectionsIfNeeded(int worldX, int worldY, int worldZ) {
        if (!Render::g_meshManager) {
            return;
        }

        // Convert to chunk coordinates
        Math::ChunkPos baseChunk = Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
        int baseSectionY = Math::WorldCoordinates::WorldYToSectionIndex(worldY);

        // Get local coordinates within chunk
        int localX = worldX - (baseChunk.x * Math::CHUNK_SIZE_X);
        int localZ = worldZ - (baseChunk.z * Math::CHUNK_SIZE_Z);
        int localY = (worldY - Math::WorldCoordinates::MIN_WORLD_Y) % Math::SECTION_HEIGHT;

        // Mark neighboring chunks if on boundary
        if (localX == 0) {
            Math::ChunkPos westChunk{baseChunk.x - 1, baseChunk.z};
            Render::g_meshManager->MarkSectionDirty(westChunk, baseSectionY);
        }
        if (localX == Math::CHUNK_SIZE_X - 1) {
            Math::ChunkPos eastChunk{baseChunk.x + 1, baseChunk.z};
            Render::g_meshManager->MarkSectionDirty(eastChunk, baseSectionY);
        }
        if (localZ == 0) {
            Math::ChunkPos northChunk{baseChunk.x, baseChunk.z - 1};
            Render::g_meshManager->MarkSectionDirty(northChunk, baseSectionY);
        }
        if (localZ == Math::CHUNK_SIZE_Z - 1) {
            Math::ChunkPos southChunk{baseChunk.x, baseChunk.z + 1};
            Render::g_meshManager->MarkSectionDirty(southChunk, baseSectionY);
        }

        // Mark neighboring sections if on section boundary
        if (localY == 0 && baseSectionY > 0) {
            Render::g_meshManager->MarkSectionDirty(baseChunk, baseSectionY - 1);
        }
        if (localY == Math::SECTION_HEIGHT - 1 && baseSectionY < Math::SECTIONS_PER_CHUNK - 1) {
            Render::g_meshManager->MarkSectionDirty(baseChunk, baseSectionY + 1);
        }
    }

    std::shared_ptr<Chunk> World::GetChunk(int chunkX, int chunkZ) const {
        if (!m_chunkProvider) {
            return nullptr;
        }

        Math::ChunkPos chunkPos{chunkX, chunkZ};
        return m_chunkProvider->GetChunk(chunkPos);
    }

    const Chunk* World::GetChunkForMeshing(int chunkX, int chunkZ) const {
        auto chunk = GetChunk(chunkX, chunkZ);
        return chunk.get();
    }

    void World::UnloadDistantChunks(int centerX, int centerZ, int keepDistance) {
        if (!m_chunkProvider) {
            return;
        }

        // Simple square-based unloading
        // Create a reasonable search area
        int searchRadius = keepDistance + 10;
        std::vector<Math::ChunkPos> chunksToUnload;

        for (int x = centerX - searchRadius; x <= centerX + searchRadius; ++x) {
            for (int z = centerZ - searchRadius; z <= centerZ + searchRadius; ++z) {
                Math::ChunkPos pos{x, z};

                if (m_chunkProvider->IsChunkLoaded(pos)) {
                    // Check if outside keep distance (square pattern)
                    int distanceX = std::abs(x - centerX);
                    int distanceZ = std::abs(z - centerZ);
                    int squareDistance = std::max(distanceX, distanceZ);

                    if (squareDistance > keepDistance) {
                        chunksToUnload.push_back(pos);
                    }
                }
            }
        }

        // Unload chunks
        size_t unloaded = 0;
        for (const Math::ChunkPos& pos : chunksToUnload) {
            if (m_chunkProvider->UnloadChunk(pos)) {
                unloaded++;
            }
        }

        if (unloaded > 0) {
            Log::Debug("Unloaded %zu distant chunks (total candidates: %zu, keep distance: %d)",
                      unloaded, chunksToUnload.size(), keepDistance);
        }
    }

    // Additional helper methods for integration with mesh system
    std::vector<DirtySection> World::GetDirtySections() {
        if (!m_chunkProvider) {
            return {};
        }

        return m_chunkProvider->GetDirtySections();
    }

    void World::ClearDirtySections(const std::vector<DirtySection>& sections) {
        if (!m_chunkProvider) {
            return;
        }

        m_chunkProvider->ClearDirtySections(sections);
    }

    void World::LogPerformanceStats() {
        if (!m_chunkProvider) {
            Log::Info("World: No chunk provider available for performance stats");
            return;
        }

        m_chunkProvider->LogPerformanceStats();
    }

    void World::SaveAllChunks() {
        if (!m_chunkProvider) {
            return;
        }

        Log::Info("Saving all loaded chunks...");
        m_chunkProvider->SaveAllDirtyChunks();
    }

    void World::SetGenerationSeed(int32_t seed) {
        if (!m_chunkProvider) {
            return;
        }

        m_chunkProvider->SetGenerationSeed(seed);
        Log::Info("Set world generation seed to: %d", seed);
    }

    int32_t World::GetGenerationSeed() const {
        if (!m_chunkProvider) {
            return 0;
        }

        return m_chunkProvider->GetGenerationSeed();
    }

    size_t World::GetMemoryUsage() const {
        if (!m_chunkProvider) {
            return sizeof(World);
        }

        return sizeof(World) + m_chunkProvider->GetMemoryUsage();
    }

    ChunkProviderStats World::GetChunkProviderStats() const {
        if (!m_chunkProvider) {
            return ChunkProviderStats{};
        }

        return m_chunkProvider->GetProviderStats();
    }

} // namespace Game
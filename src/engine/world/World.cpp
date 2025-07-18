// File: src/engine/world/World.cpp - Updated for new ChunkProvider integration
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
        Log::Info("World created (chunk loading distance: %d chunks)", m_chunkLoadingDistance);
    }

    World::~World() {
        Shutdown();
        Log::Info("World destroyed");
    }

    void World::Initialize() {
        Log::Info("=== WORLD INITIALIZATION START ===");

        // Load world settings from game settings
        LoadWorldSettings();
        Log::Info("World settings loaded - chunk loading distance: %d", m_chunkLoadingDistance);

        // Create and configure chunk provider
        ChunkProviderConfig config = CreateDefaultConfig();
        Log::Info("Created default config");

        // Configure based on world settings
        config.maxLoadedChunks = m_chunkLoadingDistance * m_chunkLoadingDistance * 4; // Square area with buffer
        config.enableLRUEviction = true;
        config.enableFallbackGeneration = true;
        config.enableAsyncSaving = true;
        config.enableAutoSave = true;
        config.autoSaveIntervalSeconds = 30.0f;

        Log::Info("Config - Max chunks: %zu, Fallback generation: %s",
                  config.maxLoadedChunks, config.enableFallbackGeneration ? "enabled" : "disabled");

        // Set up generation config
        config.generationConfig.seed = 12345;
        config.generationConfig.worldType = "default";
        config.generationConfig.generateOres = true;
        config.generationConfig.generateCaves = true;
        config.generationConfig.generateStructures = true;
        config.generationConfig.generateVegetation = true;

        Log::Info("Generation config - Seed: %d, Type: %s",
                  config.generationConfig.seed, config.generationConfig.worldType.c_str());

        // Set up dirty tracking config
        config.dirtyConfig.enableBatching = true;
        config.dirtyConfig.maxBatchSize = 50;
        config.dirtyConfig.batchTimeoutMs = 10.0f;
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
        Log::Info("Config validation passed");

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
            m_chunkProvider.reset(); // Clean up
            return;
        }
        Log::Info("ChunkProvider initialized successfully");

        // Verify initialization
        if (!m_chunkProvider->IsInitialized()) {
            Log::Error("ChunkProvider reports not initialized after Initialize() returned true!");
            return;
        }
        Log::Info("ChunkProvider initialization verified");

        // Set the global block access for physics system
        SetGlobalBlockAccess(this);

        Log::Info("✓ World initialized successfully (chunk loading distance: %d chunks)", m_chunkLoadingDistance);
        Log::Info("=== WORLD INITIALIZATION COMPLETE ===");
    }

    void World::Update(float deltaTime) {
        // Update chunk provider
        if (m_chunkProvider) {
            m_chunkProvider->Update(deltaTime);
        }

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
            m_chunkProvider.reset();
        }

        // Clear global block access
        SetGlobalBlockAccess(nullptr);

        Log::Info("World shutdown complete");
    }

    void World::RefreshSettings() {
        LoadWorldSettings();
        Log::Info("World settings refreshed (chunk loading distance: %d chunks)", m_chunkLoadingDistance);
    }

    void World::LoadWorldSettings() {
        // Load render distance from game settings and use it for chunk loading
        int renderDistanceChunks = Platform::g_gameSettings.GetRenderDistance();

        // Use render distance for chunk loading, but add a buffer for smooth loading
        m_chunkLoadingDistance = renderDistanceChunks + 2; // Load 2 extra chunks beyond render distance

        // Clamp to reasonable values
        m_chunkLoadingDistance = std::clamp(m_chunkLoadingDistance, 4, 64);

        Log::Debug("Loaded world settings: render distance=%d chunks, chunk loading distance=%d chunks",
                  renderDistanceChunks, m_chunkLoadingDistance);
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

        // Log block changes occasionally for debugging
        static int changeCount = 0;
        if (++changeCount % 100 == 0) {
            const Block& block = BlockRegistry::Get(blockId);
            Log::Debug("Block changes: %d (latest: %s at %d,%d,%d)",
                      changeCount, block.name.c_str(), worldX, worldY, worldZ);
        }

        return true;
    }

    void World::UpdateLoadedChunks(int playerChunkX, int playerChunkZ, int viewDistance) {
        if (!m_chunkProvider) {
            return;
        }

        m_chunkLoadRequests++;

        // Use setting-based view distance if not provided
        if (viewDistance <= 0) {
            viewDistance = m_chunkLoadingDistance;
        }

        // Track newly loaded chunks to trigger mesh updates
        std::vector<Math::ChunkPos> newlyLoadedChunks;

        // Load chunks in a square pattern
        int halfSize = viewDistance;

        for (int dz = -halfSize; dz <= halfSize; ++dz) {
            for (int dx = -halfSize; dx <= halfSize; ++dx) {
                int chunkX = playerChunkX + dx;
                int chunkZ = playerChunkZ + dz;

                Math::ChunkPos chunkPos{chunkX, chunkZ};

                // CRITICAL FIX: Check if chunk is already loaded FIRST
                bool wasLoaded = m_chunkProvider->IsChunkLoaded(chunkPos);

                if (!wasLoaded) {
                    // Only load if not already loaded
                    auto chunk = m_chunkProvider->GetChunk(chunkPos);
                    if (chunk) {
                        newlyLoadedChunks.push_back(chunkPos);
                        Log::Info("Newly loaded chunk (%d, %d)", chunkX, chunkZ);
                    } else {
                        Log::Warning("Failed to load chunk (%d, %d)", chunkX, chunkZ);
                    }
                }
                // If wasLoaded is true, do NOTHING - don't reload the chunk
            }
        }

        // Only mark truly NEW chunks for meshing
        if (!newlyLoadedChunks.empty() && Render::g_meshManager) {
            Log::Info("Marking %zu newly loaded chunks for meshing", newlyLoadedChunks.size());
            for (const auto& chunkPos : newlyLoadedChunks) {
                // Mark all sections in newly loaded chunks as dirty for meshing
                for (int sectionY = 0; sectionY < Math::SECTIONS_PER_CHUNK; ++sectionY) {
                    Render::g_meshManager->MarkSectionDirty(chunkPos, sectionY);
                }
            }
        }

        // Unload distant chunks
        UnloadDistantChunks(playerChunkX, playerChunkZ, viewDistance + 2);
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

        // Calculate which section was modified for logging
        Math::ChunkPos chunkPos = Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
        int sectionIndex = Math::WorldCoordinates::WorldYToSectionIndex(worldY);

        Log::Debug("Block changed at (%d, %d, %d) - chunk (%d, %d) section %d",
                  worldX, worldY, worldZ, chunkPos.x, chunkPos.z, sectionIndex);
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

        // Get loaded chunks and find those to unload
        std::vector<Math::ChunkPos> chunksToUnload;

        // Since we don't have a direct way to get loaded chunk bounds from the new system,
        // we'll use a different approach - get all loaded chunks and check distance
        auto cacheState = m_chunkProvider->GetCacheStats();

        // For now, we'll implement a simple distance-based unloading
        // In a full implementation, you'd want to get the actual loaded chunk list

        // This is a placeholder - in practice, you'd need to add a method to ChunkProvider
        // to get all loaded chunk positions
        for (int x = centerX - keepDistance - 10; x <= centerX + keepDistance + 10; ++x) {
            for (int z = centerZ - keepDistance - 10; z <= centerZ + keepDistance + 10; ++z) {
                Math::ChunkPos pos{x, z};
                if (m_chunkProvider->IsChunkLoaded(pos)) {
                    // Calculate square distance (Chebyshev distance)
                    int distanceX = std::abs(x - centerX);
                    int distanceZ = std::abs(z - centerZ);
                    int squareDistance = std::max(distanceX, distanceZ);

                    if (squareDistance > keepDistance) {
                        chunksToUnload.push_back(pos);
                    }
                }
            }
        }

        // Unload chunks outside the distance
        for (const Math::ChunkPos& pos : chunksToUnload) {
            m_chunkProvider->UnloadChunk(pos);
        }

        if (!chunksToUnload.empty()) {
            Log::Debug("Unloaded %zu distant chunks (keep distance: %d)",
                      chunksToUnload.size(), keepDistance);
        }
    }

    // Additional helper methods for integration with mesh system
    std::vector<DirtySection> World::GetDirtySections(size_t maxCount) {
        if (!m_chunkProvider) {
            return {};
        }

        return m_chunkProvider->GetDirtySections(maxCount);
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

    void World::PreloadArea(int centerChunkX, int centerChunkZ, int radius) {
        if (!m_chunkProvider) {
            return;
        }

        Math::ChunkPos center{centerChunkX, centerChunkZ};
        m_chunkProvider->PreloadArea(center, radius);
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
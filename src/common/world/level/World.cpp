// File: src/common/world/level/World.cpp
#include "World.hpp"
#include "../../core/Log.hpp"
#include "../block/BlockRegistry.hpp"
#include "../../physics/RayCast.hpp"
#include "server/IntegratedServer.hpp"
#include "server/world/tracking/SectionChangeAccumulator.hpp"
#include <algorithm>
#include <cmath>


namespace Game {

    World::World() {
        Log::Info("World created");
    }

    World::~World() {
        Shutdown();
        Log::Info("World destroyed");
    }

    void World::Initialize() {
        Log::Info("=== WORLD INITIALIZATION START ===");

        // Create and configure chunk provider
        ChunkProviderConfig config = CreateDefaultConfig();
        config.enableFallbackGeneration = false;

        // Set up generation config
        // NOTE: Seed comes from GenerationConfig default (IChunkGenerator.hpp)
        // Change the seed there for a single source of truth
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

        // NOTE: ChunkProvider::Initialize() is deferred to the server thread
        // via World::InitializeChunkProvider(). This ensures ServerChunkCache
        // captures the correct thread ID (matching Minecraft's architecture).

        Log::Info("✓ World created successfully");
        Log::Info("=== WORLD INITIALIZATION COMPLETE ===");
    }

    bool World::InitializeChunkProvider() {
        if (!m_chunkProvider) {
            Log::Error("InitializeChunkProvider: No ChunkProvider created");
            return false;
        }

        Log::Info("Initializing ChunkProvider on server thread...");
        if (!m_chunkProvider->Initialize()) {
            Log::Error("Failed to initialize ChunkProvider");
            m_chunkProvider.reset();
            return false;
        }

        SetGlobalBlockAccess(this);
        Log::Info("ChunkProvider initialized successfully on server thread");
        return true;
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
        // Default to all updates for backwards compatibility
        return SetBlock(worldX, worldY, worldZ, blockId, UpdateFlags::All);
    }
    
    bool World::SetBlock(int worldX, int worldY, int worldZ, BlockID blockId, uint32_t updateFlags) {
        if (!IsValidPosition(worldX, worldY, worldZ)) {
            Log::Warning("Attempted to set block at invalid position (%d, %d, %d)",
                        worldX, worldY, worldZ);
            return false;
        }

        if (!m_chunkProvider) {
            Log::Warning("No chunk provider available for block placement");
            return false;
        }

        // Get the old block for comparison
        BlockID oldBlockId = GetBlock(worldX, worldY, worldZ);
        
        // No change needed
        if (oldBlockId == blockId) {
            return true;
        }

        // Set the block using the chunk provider
        m_chunkProvider->SetBlock(worldX, worldY, worldZ, blockId);

        // Process update flags
        if (updateFlags & UpdateFlags::NotifyNeighbors) {
            // Notify all 6 neighboring blocks
            NotifyNeighborBlocks(worldX, worldY, worldZ);
        }
        
        if (updateFlags & UpdateFlags::UpdateShapes) {
            // TODO: Update connected block shapes (fences, walls, etc.)
        }
        
        if (updateFlags & UpdateFlags::RecomputeLight) {
            // TODO: Trigger light recalculation
        }
        
        if (updateFlags & UpdateFlags::UpdateHeightmap) {
            // TODO: Update chunk heightmap
        }
        
        if (updateFlags & UpdateFlags::MarkDirty) {
            // Mark section for remeshing
            OnBlockChanged(worldX, worldY, worldZ);
        }
        
        if (updateFlags) {
            // Queue block change for centralized broadcast
            // This happens on server thread during world simulation
            if (Server::g_integratedServer) {
                auto* accumulator = Server::g_integratedServer->GetChangeAccumulator();
                if (accumulator) {
                    // Calculate section position
                    Game::Math::SectionPos sp = Game::Math::SectionPos::fromWorldPos(worldX, worldY, worldZ);
                    
                    // Calculate local coordinates within section
                    uint8_t localX = worldX & 0xF;
                    uint8_t localY = (worldY + 64) & 0xF;  // Adjust for min Y of -64
                    uint8_t localZ = worldZ & 0xF;
                    
                    // Accumulate the change (will be broadcast at end of tick)
                    accumulator->accumulate(sp, localX, localY, localZ, blockId);
                }
            }
        }

        return true;
    }
    
    void World::NotifyNeighborBlocks(int worldX, int worldY, int worldZ) {
        // TODO: Implement block neighbor notification when BlockRegistry is fully implemented
        // For now, this is a placeholder that will be expanded later
        
        // Get block registry
        // auto& registry = BlockRegistry::getInstance();
        
        // Notify all 6 neighbors
        const glm::ivec3 offsets[6] = {
            {1, 0, 0}, {-1, 0, 0},  // +X, -X
            {0, 1, 0}, {0, -1, 0},  // +Y, -Y
            {0, 0, 1}, {0, 0, -1}   // +Z, -Z
        };
        
        glm::ivec3 blockPos(worldX, worldY, worldZ);
        
        for (const auto& offset : offsets) {
            glm::ivec3 neighborPos = blockPos + offset;
            
            // Skip invalid positions
            if (!IsValidPosition(neighborPos.x, neighborPos.y, neighborPos.z)) {
                continue;
            }
            
            // TODO: Get the neighbor block and notify it
            // BlockID neighborId = GetBlock(neighborPos.x, neighborPos.y, neighborPos.z);
            // auto* neighborBlock = registry.getBlock(neighborId);
            // 
            // if (neighborBlock) {
            //     // Notify the neighbor that this block changed
            //     neighborBlock->onNeighborChanged(this, neighborPos, blockPos);
            // }
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
        // Mark section for remeshing (server-side tracking)
        MarkSectionDirty(worldX, worldY, worldZ);

        // The server will send block change packets to clients
        // Clients will handle their own dirty tracking when they receive the packets
    }

    void World::MarkNeighboringSectionsIfNeeded(int worldX, int worldY, int worldZ) {
        // This function is no longer needed - clients handle their own dirty tracking
        // when they receive block change packets
        // Keeping empty function for now to avoid breaking other code that might call it
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

    // ========================================================================
    // SERVER WORLD LOOP IMPLEMENTATION
    // ========================================================================

    void World::WorldLoop(float deltaTime) {
        if (m_stopRequested.load()) return;

        // World simulation only — chunk loading is driven by the session system
        // (IntegratedServer::ProcessWatchSetChanges), NOT by World.

        // 1. Process any pending block updates
        ProcessBlockUpdates();

        // 2. Perform random block ticks (growth, decay, etc.)
        PerformRandomBlockTick();

        // 3. Process scheduled block events
        ProcessBlockEvents();

        // 4. Update tile entities
        TileEntityTick();

        // 5. Update entities
        EntityTick();

        // 6. Update world time and weather
        WorldTimeWeatherTick();
    }

    void World::ProcessBlockUpdates() {
        // Process any pending block updates
        // This would handle things like:
        // - Water/lava flow
        // - Sand/gravel falling
        // - Redstone updates
        // - Block state changes
        
        // TODO: Implement block update queue and processing
        // For now, this is a placeholder for future implementation
    }

    void World::PerformRandomBlockTick() {
        // Perform random block ticks for loaded chunks
        // This handles:
        // - Crop growth
        // - Grass spreading
        // - Ice melting/freezing
        // - Leaf decay
        // - Fire spreading
        
        // TODO: Implement random tick selection and processing
        // In Minecraft, 3 random blocks per chunk section are ticked per game tick
    }

    void World::ProcessBlockEvents() {
        // Process scheduled block events
        // This handles time-delayed block actions like:
        // - Piston extensions/retractions
        // - Door animations
        // - Note block sounds
        // - Dispenser/dropper actions
        
        // TODO: Implement block event queue and processing
    }

    void World::TileEntityTick() {
        // Update all tile entities in loaded chunks
        // This handles:
        // - Furnace smelting
        // - Chest animations
        // - Beacon effects
        // - Spawner logic
        // - Hopper item transfer
        
        // TODO: Implement tile entity system and ticking
    }

    void World::EntityTick() {
        // Update all entities in the world
        // This handles:
        // - Entity movement and physics
        // - AI behavior
        // - Entity collisions
        // - Entity spawning/despawning
        // - Item pickup/drop
        
        // TODO: Implement entity system and ticking
    }

    void World::WorldTimeWeatherTick() {
        // Update world time and weather
        // This handles:
        // - Day/night cycle
        // - Weather transitions (clear/rain/thunder)
        // - Moon phases
        // - Light level updates based on time
        
        // TODO: Implement time and weather system
    }

} // namespace Game
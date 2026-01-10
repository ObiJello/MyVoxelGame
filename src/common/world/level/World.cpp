// File: src/common/world/level/World.cpp
#include "World.hpp"
#include "../../core/Log.hpp"
#include "../block/BlockRegistry.hpp"
#include "../../physics/Physics.hpp"
#include "platform/GameDirectory.hpp"
#include "server/IntegratedServer.hpp"
#include "server/world/tracking/SectionChangeAccumulator.hpp"
#include "../../physics/RayCast.hpp"
#include <algorithm>
#include <cmath>


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
        // ChunkCache now manages its own size (defaults to 2048)
        config.enableFallbackGeneration = false;

        Log::Info("Config - %dx%d square area, Fallback generation: %s",
                  squareSize, squareSize,
                  config.enableFallbackGeneration ? "enabled" : "disabled");

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

        if (chunksToLoad.size() > 0) {
            Log::Debug("Loading %zu chunks in %dx%d square around player chunk (%d, %d) - nearest first",
                      chunksToLoad.size(), 2 * viewDistance + 1, 2 * viewDistance + 1,
                      playerChunkX, playerChunkZ);
        }

        size_t chunksLoaded = 0;
        size_t totalChunksInArea = (2 * viewDistance + 1) * (2 * viewDistance + 1);
        size_t chunksSkipped = totalChunksInArea - chunksToLoad.size();

        // Load chunks in order of proximity to player
        for (const auto& chunkPos : chunksToLoad) {
            // Load the chunk
            auto chunk = m_chunkProvider->GetChunk(chunkPos);
            if (chunk) {
                chunksLoaded++;

                // Server-side chunk loaded - client will handle its own dirty tracking
                // when it receives the chunk data packet

                // Log progress for nearest chunks
                int distanceSq = (chunkPos.x - playerChunkX) * (chunkPos.x - playerChunkX) +
                                (chunkPos.z - playerChunkZ) * (chunkPos.z - playerChunkZ);
                if (distanceSq <= 4) { // Within 2 chunk radius
                    /*Log::Debug("Loaded nearby chunk (%d, %d) - distance: %.1f",
                              chunkPos.x, chunkPos.z, std::sqrt(distanceSq));*/
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

    // ========================================================================
    // SERVER WORLD LOOP IMPLEMENTATION
    // ========================================================================

    void World::WorldLoop(float deltaTime, int maxChunksPerTick) {
        // Main world tick function - processes all world updates
        
        // 1. Chunk loading/unloading based on player positions
        ChunkLoadUnload();
        
        // 2. Process any pending block updates
        ProcessBlockUpdates();
        
        // 3. Perform random block ticks (growth, decay, etc.)
        PerformRandomBlockTick();
        
        // 4. Process scheduled block events
        ProcessBlockEvents();
        
        // 5. Update tile entities
        TileEntityTick();
        
        // 6. Update entities
        EntityTick();
        
        // 7. Update world time and weather
        WorldTimeWeatherTick();
        
        // 8. Send chunk and entity updates to clients
        ChunkEntityPacketDispatch(maxChunksPerTick);
    }

    void World::ChunkLoadUnload() {
        if (!m_chunkProvider) {
            return;
        }

        // Get player position from integrated server
        // In multiplayer, this would iterate through all connected players
        int playerChunkX = 0;
        int playerChunkZ = 0;
        
        if (Server::g_integratedServer) {
            const auto& playerState = Server::g_integratedServer->GetPlayerState();
            playerChunkX = playerState.currentChunk.x;
            playerChunkZ = playerState.currentChunk.z;
        }
        
        // Load chunks around player(s) synchronously
        UpdateLoadedChunks(playerChunkX, playerChunkZ, m_renderDistance);
        
        // Unload distant chunks
        UnloadDistantChunks(playerChunkX, playerChunkZ, m_renderDistance + 2);
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

    void World::ChunkEntityPacketDispatch(int maxChunksPerTick) {
        // Send chunk and entity updates to all connected clients
        // This handles:
        // - Chunk data packets for newly loaded chunks
        // - Block change packets for modified blocks
        // - Entity spawn/despawn packets
        // - Entity movement/update packets
        // - Tile entity update packets
        
        if (!m_chunkProvider) {
            return;
        }
        
        if (!Server::g_integratedServer) {
            return;
        }
        
        // Get player position to determine which chunks to send
        const auto& playerState = Server::g_integratedServer->GetPlayerState();
        int playerChunkX = playerState.currentChunk.x;
        int playerChunkZ = playerState.currentChunk.z;
        
        // Check if player moved significantly (more than render distance)
        // If so, clear the sent chunks cache as we need to resend for new area
        static Math::ChunkPos lastPlayerChunk{INT_MAX, INT_MAX};
        if (lastPlayerChunk.x != INT_MAX) {
            int dx = std::abs(playerChunkX - lastPlayerChunk.x);
            int dz = std::abs(playerChunkZ - lastPlayerChunk.z);
            if (dx > m_renderDistance || dz > m_renderDistance) {
                Log::Debug("[World] Player moved significantly, clearing sent chunks cache");
                m_sentChunks.clear();
            }
        }
        lastPlayerChunk = Math::ChunkPos{playerChunkX, playerChunkZ};
        
        // Build list of chunks that need to be sent (loaded but not sent yet)
        struct ChunkToSend {
            Math::ChunkPos pos;
            float distanceSq;
        };
        std::vector<ChunkToSend> chunksToSend;
        
        for (int dx = -m_renderDistance; dx <= m_renderDistance; ++dx) {
            for (int dz = -m_renderDistance; dz <= m_renderDistance; ++dz) {
                Math::ChunkPos chunkPos{playerChunkX + dx, playerChunkZ + dz};
                
                // Skip if already sent
                if (m_sentChunks.find(chunkPos) != m_sentChunks.end()) {
                    continue;
                }
                
                // Skip if not loaded
                if (!m_chunkProvider->IsChunkLoaded(chunkPos)) {
                    continue;
                }
                
                // Add to list with distance for priority sorting
                float distSq = static_cast<float>(dx * dx + dz * dz);
                chunksToSend.push_back({chunkPos, distSq});
            }
        }
        
        // Sort by distance (closest first)
        std::sort(chunksToSend.begin(), chunksToSend.end(),
                  [](const ChunkToSend& a, const ChunkToSend& b) {
                      return a.distanceSq < b.distanceSq;
                  });
        
        // Apply the chunk send limit
        int chunksToSendThisTick = static_cast<int>(chunksToSend.size());
        if (maxChunksPerTick > 0 && chunksToSendThisTick > maxChunksPerTick) {
            chunksToSendThisTick = maxChunksPerTick;
        }
        
        // Send chunks up to the limit
        int chunksSent = 0;
        for (int i = 0; i < chunksToSendThisTick; ++i) {
            const auto& chunkToSend = chunksToSend[i];
            auto chunk = m_chunkProvider->GetChunk(chunkToSend.pos);
            
            if (!chunk) {
                continue;
            }
            
            // Create ChunkDataS2CPacket
            Network::ChunkDataS2CPacket packet;
            packet.chunkX = chunkToSend.pos.x;
            packet.chunkZ = chunkToSend.pos.z;
            packet.groundUpContinuous = true;
            packet.primaryBitmask = 0;
            
            // Build section data for non-empty sections
            for (int sectionY = 0; sectionY < Math::SECTIONS_PER_CHUNK; ++sectionY) {
                const auto* section = chunk->GetSection(sectionY);
                if (section) {
                    // Check if section has any non-air blocks
                    uint16_t nonAirCount = 0;
                    for (size_t i = 0; i < section->blocks.size(); ++i) {
                        if (section->blocks[i] != static_cast<uint16_t>(BlockID::Air)) {
                            nonAirCount++;
                        }
                    }
                    
                    // Skip empty sections
                    if (nonAirCount == 0) {
                        continue;
                    }
                    
                    // Set bit in bitmask for this section
                    packet.primaryBitmask |= (1 << sectionY);
                    
                    // Create section data
                    Network::ChunkDataS2CPacket::SectionData sectionData;
                    sectionData.blockCount = nonAirCount;
                    
                    // For now, use direct block IDs (no palette)
                    sectionData.bitsPerEntry = 16; // Direct block IDs
                    
                    // Pack block data (16x16x16 blocks)
                    const size_t blocksPerSection = 16 * 16 * 16;
                    const size_t blocksPerLong = 64 / 16; // 4 blocks per uint64_t
                    sectionData.dataArray.resize((blocksPerSection + blocksPerLong - 1) / blocksPerLong);
                    
                    // Copy block data
                    for (size_t i = 0; i < blocksPerSection; ++i) {
                        uint16_t blockId = section->blocks[i];
                        size_t longIndex = i / blocksPerLong;
                        size_t bitOffset = (i % blocksPerLong) * 16;
                        sectionData.dataArray[longIndex] |= (static_cast<uint64_t>(blockId) << bitOffset);
                    }
                    
                    packet.sections.push_back(std::move(sectionData));
                }
            }
            
            // Send packet via IntegratedServer
            Server::g_integratedServer->SendChunkDataS2CPacket(std::move(packet));
            
            // Mark as sent
            m_sentChunks.insert(chunkToSend.pos);
            chunksSent++;
        }
        
        if (chunksSent > 0) {
            Log::Info("[World] Sent %d chunks to client (%.0f%% complete, %zu remaining)", 
                      chunksSent, 
                      (m_sentChunks.size() * 100.0f) / ((2 * m_renderDistance + 1) * (2 * m_renderDistance + 1)),
                      chunksToSend.size() - chunksSent);
        }
        
        // TODO: Implement entity spawn/despawn packets
        // - Track entities entering/leaving player's view distance
        // - Send EntitySpawn packets for new entities
        // - Send EntityDestroy packets for entities leaving view
        
        // TODO: Implement entity movement/update packets
        // - Track entity position changes
        // - Send EntityMove packets for entities that have moved
        // - Batch multiple entity movements into single packet if possible
        
        // TODO: Implement tile entity update packets
        // - Track tile entities that have changed state
        // - Send tile entity data packets (e.g., for chests, furnaces, etc.)
        // - Only send updates for tile entities in loaded chunks
        
        // TODO: Implement block change packet batching
        // - Block changes are already handled via ProcessBlockAction
        // - But we could batch multiple changes in the same chunk
        // - Use MultiBlockChangeS2CPacket for efficiency
    }

} // namespace Game
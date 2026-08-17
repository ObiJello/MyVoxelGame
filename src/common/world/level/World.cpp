// File: src/common/world/level/World.cpp
#include "World.hpp"
#include "common/world/biome/Biomes.hpp"
#include "../../core/Log.hpp"
#include "../../core/Profiling_Tracy.hpp"
#include "../block/BlockRegistry.hpp"
#include "../block/BlockPlacement.hpp"   // CanSurviveAt / HasModelledSurvivalRule
#include "../block/entity/BlockEntity.hpp"
#include "../block/entity/BlockEntityType.hpp"
#include "../block/entity/BlockEntityTypes.hpp"
#include "../chunk/Chunk.hpp"
#include "../../physics/RayCast.hpp"
#include "server/IntegratedServer.hpp"
#include "server/network/NetworkServer.hpp"
#include "server/world/tracking/SectionChangeAccumulator.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/network/packets/game/BlockEntityDataS2CPacket.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/world/loot/LootTables.hpp"
#include "WorldDrops.hpp"
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

        // Read-only worlds get no chunk saver at all (see ChunkProvider).
        config.readOnly = m_readOnly;
        if (m_readOnly) {
            Log::Info("World opened READ-ONLY — no chunk data will be written back");
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

    uint8_t World::GetBlockState(int worldX, int worldY, int worldZ) const {
        if (!IsValidPosition(worldX, worldY, worldZ) || !m_chunkProvider) {
            return 0;
        }
        return m_chunkProvider->GetBlockState(worldX, worldY, worldZ);
    }

    uint16_t World::GetBiome(int worldX, int worldY, int worldZ) const {
        if (!IsValidPosition(worldX, worldY, worldZ) || !m_chunkProvider) {
            return kFallbackBiomeId;
        }
        return m_chunkProvider->GetBiome(worldX, worldY, worldZ);
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

    int World::GetSurfaceHeight(int worldX, int worldZ, HeightmapType type) const {
        const auto chunkPos = Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
        // Cache-only: this is called per candidate position by the mob spawner,
        // which walks the ticket manager's chunk list -- and that list contains
        // chunks that have never been loaded. A blocking GetChunk here would
        // generate one synchronously on the server thread, mid-tick.
        auto chunk = GetLoadedChunk(chunkPos.x, chunkPos.z);
        if (!chunk) return MIN_Y;

        // An unprimed chunk means it came from a path that skipped both the
        // generator copy and the NBT restore. Prime it now rather than
        // answering MIN_Y — a wrong surface height silently misplaces every
        // spawn in the column, where a one-off scan is merely slow.
        if (!chunk->AreHeightmapsPrimed()) {
            chunk->PrimeHeightmaps();
        }

        const int localX = worldX - chunkPos.x * Math::CHUNK_SIZE_X;
        const int localZ = worldZ - chunkPos.z * Math::CHUNK_SIZE_Z;
        return chunk->GetSurfaceHeight(localX, localZ, type);
    }

    bool World::CanSeeSky(int worldX, int worldY, int worldZ) const {
        // MC Level.canSeeSky: nothing at or above this position blocks the sky.
        // WORLD_SURFACE stores the topmost non-air block, so the test is a
        // single comparison — no column walk, which is what the undead
        // daylight-burn check and every spawn light test used to pay for.
        return worldY > GetSurfaceHeight(worldX, worldZ, HeightmapType::WorldSurface);
    }

    bool World::IsValidPosition(int worldX, int worldY, int worldZ) const {
        return worldY >= MIN_Y && worldY <= MAX_Y;
    }

    // World modification
    bool World::SetBlock(int worldX, int worldY, int worldZ, BlockID blockId) {
        // Default to all updates for backwards compatibility
        return SetBlock(worldX, worldY, worldZ, blockId, UpdateFlags::All, 0);
    }

    bool World::SetBlock(int worldX, int worldY, int worldZ, BlockID blockId, uint32_t updateFlags) {
        return SetBlock(worldX, worldY, worldZ, blockId, updateFlags, 0);
    }

    bool World::SetBlock(int worldX, int worldY, int worldZ, BlockID blockId, uint32_t updateFlags,
                         uint8_t stateIndex) {
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
        const uint8_t oldState = GetBlockState(worldX, worldY, worldZ);

        // No change needed. The state comparison matters: re-orienting a block
        // in place (same id, new facing) must not be swallowed here.
        if (oldBlockId == blockId && oldState == stateIndex) {
            return true;
        }

        // Set the block using the chunk provider
        m_chunkProvider->SetBlock(worldX, worldY, worldZ, blockId, stateIndex);

        // ── BlockEntity lifecycle hook (mirrors MC Level.setBlock's
        //    setBlockEntity call). If the OLD block had a BE, destroy it.
        //    If the NEW block needs a BE, create one and broadcast it.
        //
        //    The chunk lookup is best-effort: a freshly-loaded chunk should
        //    always be available immediately after SetBlock since we just
        //    wrote into it via m_chunkProvider->SetBlock. If it's somehow
        //    not, we silently skip — the BE will be missing but the block
        //    update still goes out.
        {
            const auto chunkPos = Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
            auto chunk = m_chunkProvider->GetChunk(chunkPos);
            if (chunk) {
                const int localX = worldX - chunkPos.x * 16;
                const int localZ = worldZ - chunkPos.z * 16;

                // ONLY when the BLOCK changes. A state-only edit (same block,
                // new facing or chest `type`) must keep its block entity: MC's
                // setBlock only swaps the BE when the new state's block differs.
                //
                // Without this guard, re-typing a chest as it pairs or unpairs
                // destroys and recreates its block entity — silently emptying
                // the chest next to the one you just placed, and churning a
                // BlockEntityRemove + BlockEntityData pair at the client for a
                // block that never went away.
                const bool blockChanged = (oldBlockId != blockId);
                const bool oldHadBE = blockChanged && BlockEntityTypes::HasBlockEntity(oldBlockId);
                const bool newHasBE = blockChanged && BlockEntityTypes::HasBlockEntity(blockId);

                if (oldHadBE) {
                    chunk->RemoveBlockEntity(localX, worldY, localZ);
                    // Tell every watcher to drop their copy. The block change
                    // is already queued via the accumulator above, but a
                    // separate teardown packet keeps client-side lifecycle
                    // symmetric with the server (mirrors MC's implicit
                    // remove-via-new-blockstate by being explicit).
                    if (Server::g_integratedServer && Server::g_integratedServer->GetNetworkServer()) {
                        Network::BlockEntityRemoveS2CPacket pkt{worldX, worldY, worldZ};
                        auto data = Network::Serialization::Serialize(pkt);
                        Server::g_integratedServer->GetNetworkServer()->BroadcastPacket(
                            static_cast<uint8_t>(Network::PacketId::BlockEntityRemoveS2C),
                            data);
                    }
                }
                if (newHasBE) {
                    const auto* type = BlockEntityTypes::ForBlock(blockId);
                    if (type) {
                        auto be = type->Create(glm::ivec3(worldX, worldY, worldZ), blockId);
                        // Snapshot the freshly-created state for the broadcast
                        // BEFORE handing the BE to the chunk (the chunk owns
                        // it after SetBlockEntity).
                        Network::BlockEntityDataS2CPacket pkt(worldX, worldY, worldZ,
                                                              type->TypeId());
                        Network::PacketBuffer scratch;
                        be->Save(scratch);
                        pkt.dataBlob = scratch.GetData();

                        chunk->SetBlockEntity(localX, worldY, localZ, std::move(be));

                        if (Server::g_integratedServer && Server::g_integratedServer->GetNetworkServer()) {
                            auto data = Network::Serialization::Serialize(pkt);
                            Server::g_integratedServer->GetNetworkServer()->BroadcastPacket(
                                static_cast<uint8_t>(Network::PacketId::BlockEntityDataS2C),
                                data);
                        }
                    }
                }
            }
        }

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
                    
                    // Accumulate the change (will be broadcast at end of tick).
                    // Block and state travel together — a re-orientation is a
                    // real change that watchers must be told about.
                    accumulator->accumulate(sp, localX, localY, localZ,
                                            Game::BlockStateRef{blockId, stateIndex});
                }
            }
        }

        return true;
    }
    
    bool World::CanBlockSurviveAt(int worldX, int worldY, int worldZ) const {
        const BlockID id = GetBlock(worldX, worldY, worldZ);
        if (id == BlockID::Air) return true;
        if (!BlockRegistry::Get(id).needsSupportBelow) return true;

        // Where the block's real MC canSurvive rule is modelled, use it. This
        // is not an optimisation — the heuristic below is WRONG for anything
        // that stacks on itself. Sugar cane is `.noCollision()`, so a cane
        // resting on cane fails "the block below has collision" and the whole
        // column above the first segment gets destroyed by the next neighbour
        // update. That is what made a growing stalk lose its middle.
        //
        // Only asked for blocks with a modelled rule: CanSurviveAt answers
        // `true` for everything else, which would turn the support collapse
        // off entirely for flowers and torches.
        if (HasModelledSurvivalRule(id)) {
            return CanSurviveAt(*this, {worldX, worldY, worldZ}, id);
        }

        // MC LeafLitterBlock.canSurvive: isFaceSturdy(below, Direction.UP).
        // "Has collision" stands in for a sturdy top face — every full cube
        // and every slab-like block passes, and the non-colliding blocks
        // (another flower, a torch) correctly do not.
        //
        // Divergence, deliberate: MC's VegetationBlock is stricter still and
        // wants DIRT or farmland specifically, so vanilla flowers cannot sit on
        // stone. That only shows up in positions normal placement can't create,
        // and the looser rule fails SAFE — it never deletes a block MC would
        // have kept.
        const int belowY = worldY - 1;
        if (!IsValidPosition(worldX, belowY, worldZ)) return false;
        const BlockID below = GetBlock(worldX, belowY, worldZ);
        if (below == BlockID::Air) return false;
        return BlockRegistry::HasCollision(below);
    }

    void World::NotifyNeighborBlocks(int worldX, int worldY, int worldZ) {
        // Port of MC BlockState.updateNeighbourShapes → Block.updateOrDestroy
        // (Block.java): after a block changes, every neighbour re-checks
        // whether it can still exist, and one that cannot is DESTROYED WITH
        // DROPS rather than left floating.
        //
        // Recursion is real and wanted — breaking the dirt under a stack of
        // sugar cane has to collapse the whole column — so this re-enters
        // through SetBlock. MC bounds it with a recursionLeft counter starting
        // at 512; the same budget is kept here, as a thread_local because
        // SetBlock can be driven from either the server thread or a worker.
        static thread_local int s_updateDepth = 0;
        constexpr int kMaxUpdateDepth = 512;
        if (s_updateDepth >= kMaxUpdateDepth) return;

        // All six, not just the one above: each neighbour evaluates its OWN
        // rule, and blocks with no rule fall out in the first line of
        // CanBlockSurviveAt. Keeping the walk general means a side-attached
        // rule can be added later without revisiting this loop.
        // Paired with kOffsets: the direction pointing from the NEIGHBOUR back
        // at the block that changed, which is what an updateShape rule asks
        // about ("is the thing I'm attached to still there?").
        static constexpr glm::ivec3 kOffsets[6] = {
            {1, 0, 0}, {-1, 0, 0},
            {0, 1, 0}, {0, -1, 0},
            {0, 0, 1}, {0, 0, -1}
        };
        static constexpr Direction kFromNeighbour[6] = {
            Direction::West,  Direction::East,
            Direction::Down,  Direction::Up,
            Direction::North, Direction::South
        };

        const glm::ivec3 origin(worldX, worldY, worldZ);
        const BlockID originId = GetBlock(worldX, worldY, worldZ);

        // MC destroyBlock(pos, true) — drops, then clears. Shared by the
        // support rule and by an updateShape that answers AIR, because MC's
        // updateShape returning AIR is a destroy too, not a silent erase.
        auto destroyWithDrops = [&](const glm::ivec3& p, BlockID id) {
            // Roll the loot BEFORE clearing: the tables key on the block that
            // is still there, and on its state.
            const uint8_t state = GetBlockState(p.x, p.y, p.z);
            JavaRandom rng(static_cast<uint64_t>(
                (static_cast<int64_t>(p.x) * 3129871) ^
                (static_cast<int64_t>(p.z) * 116129781) ^
                 static_cast<int64_t>(p.y)));
            LootContext ctx;
            ctx.block      = id;
            ctx.blockState = state;
            ctx.world      = this;
            ctx.pos        = p;
            ctx.rng        = &rng;

            for (const ItemStack& drop : LootTables::GetDrops(ctx)) {
                // Pops out at the block as a real entity. The only way this
                // fails now is with no server to spawn into, in which case
                // nothing is simulating the collapse either.
                DropItemStackNear(p, drop);
            }
            SetBlock(p.x, p.y, p.z, BlockID::Air, UpdateFlags::All);
        };

        ++s_updateDepth;
        for (int oi = 0; oi < 6; ++oi) {
            const glm::ivec3 n = origin + kOffsets[oi];
            if (!IsValidPosition(n.x, n.y, n.z)) continue;

            const BlockID id = GetBlock(n.x, n.y, n.z);
            if (id == BlockID::Air) continue;
            const Block& neighbourDef = BlockRegistry::Get(id);

            // MC BlockState.updateShape — a neighbour may TRANSFORM rather than
            // just survive-or-die. Runs before the support rule below because
            // the two are alternatives: a block that transformed has already
            // answered for this change.
            if (neighbourDef.neighborChanged) {
                BlockID outBlock = BlockID::Air;
                uint8_t outState = 0;
                if (neighbourDef.neighborChanged(*this, n, id,
                                                 GetBlockState(n.x, n.y, n.z),
                                                 kFromNeighbour[oi], originId,
                                                 outBlock, outState)) {
                    // AIR from updateShape means "I cannot exist any more" —
                    // MC's RedStoneWireBlock and the face-attached family both
                    // return it when their support goes, and MC destroys with
                    // drops rather than erasing.
                    if (outBlock == BlockID::Air) destroyWithDrops(n, id);
                    else SetBlock(n.x, n.y, n.z, outBlock, UpdateFlags::All, outState);
                    continue;
                }
            }

            if (!neighbourDef.needsSupportBelow) continue;   // cheap reject
            if (CanBlockSurviveAt(n.x, n.y, n.z)) continue;

            destroyWithDrops(n, id);
        }
        --s_updateDepth;
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

    std::shared_ptr<Chunk> World::GetLoadedChunk(int chunkX, int chunkZ) const {
        if (!m_chunkProvider) {
            return nullptr;
        }
        return m_chunkProvider->GetLoadedChunk(Math::ChunkPos{chunkX, chunkZ});
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

    void World::SetGenerationSeed(int64_t seed) {
        if (!m_chunkProvider) {
            return;
        }

        m_chunkProvider->SetGenerationSeed(seed);
        Log::Info("Set world generation seed to: %d", seed);
    }

    int64_t World::GetGenerationSeed() const {
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
        PROFILE_ZONE_N("WorldLoop");
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

    // MC Level.getBlockRandomPos (Level.java:821-825), reproduced exactly:
    //
    //     this.randValue = this.randValue * 3 + 1013904223;
    //     int val = this.randValue >> 2;
    //     return new BlockPos(xo + (val & 15), yo + (val >> 16 & yMask), zo + (val >> 8 & 15));
    //
    // This is NOT three nextInt(16) calls on the world RNG, and substituting
    // them would change how sampled positions are distributed through a section
    // — the whole point of the shifts is that X, Y and Z come from different,
    // non-overlapping bit ranges of one cheap step.
    //
    // The multiply overflows int32 by design; MC relies on Java's wrapping
    // arithmetic, so this goes through uint32_t to keep it defined in C++.
    glm::ivec3 World::GetBlockRandomPos(int xo, int yo, int zo, int yMask) {
        m_randValue = static_cast<int32_t>(static_cast<uint32_t>(m_randValue) * 3u + 1013904223u);
        const int32_t val = m_randValue >> 2;
        return glm::ivec3(xo + (val & 15),
                          yo + ((val >> 16) & yMask),
                          zo + ((val >> 8) & 15));
    }

    // Port of MC ServerLevel.tickChunk (ServerLevel.java:452-492), block half.
    //
    // The precipitation half of tickChunk (ice and snow forming) is deliberately
    // absent — it needs biome temperature and weather, neither of which exists
    // here. When either arrives it belongs at the top of this function, in the
    // same `for i < tickSpeed` shape MC uses.
    void World::PerformRandomBlockTick() {
        PROFILE_ZONE_N("RandomTick");

        const int tickSpeed = m_randomTickSpeed;
        if (tickSpeed <= 0 || !m_chunkProvider) return;

        for (const Math::ChunkPos& cp : m_blockTickingChunks) {
            // Cache-only, and it MUST stay that way. m_blockTickingChunks comes
            // from the ticket manager's level cache, which lists every chunk
            // inside simulation distance whether or not it has ever been
            // loaded — 289 of them at the default distance of 8. Calling the
            // blocking GetChunk here made the first tick after a player joined
            // synchronously generate every one of them on the server thread:
            // measured at 7761 ms, during which no tick completed and not a
            // single chunk was delivered to the client, so the world stayed
            // empty even though the chunks were arriving.
            //
            // MC has no equivalent hazard — ChunkMap.forEachBlockTickingChunk
            // walks holders that already exist, and a chunk that is not loaded
            // simply is not ticked.
            auto chunk = m_chunkProvider->GetLoadedChunk(cp);
            if (!chunk) continue;

            const int minX = cp.x * Math::CHUNK_SIZE_X;
            const int minZ = cp.z * Math::CHUNK_SIZE_Z;

            for (int sectionIndex = 0; sectionIndex < Math::SECTIONS_PER_CHUNK; ++sectionIndex) {
                ChunkSection* section = chunk->GetSection(sectionIndex);
                // The whole reason ChunkSection keeps a census: nearly every
                // section in the world answers no here, for one comparison.
                if (!section || !section->IsRandomlyTicking()) continue;

                const int minYInSection = MIN_Y + sectionIndex * Math::SECTION_HEIGHT;

                for (int i = 0; i < tickSpeed; ++i) {
                    const glm::ivec3 pos = GetBlockRandomPos(minX, minYInSection, minZ, 15);

                    // Read straight out of the section we already have rather
                    // than going back through World::GetBlock, which would
                    // re-resolve the chunk and the section for a position we
                    // just constructed inside them.
                    const int lx = pos.x - minX;
                    const int ly = pos.y - minYInSection;
                    const int lz = pos.z - minZ;
                    const BlockID id = section->GetBlockID(lx, ly, lz);
                    if (id == BlockID::Air) continue;

                    const Block& def = BlockRegistry::Get(id);
                    if (!def.randomTick) continue;

                    const uint8_t state = section->GetState(lx, ly, lz);
                    if (def.isRandomlyTicking && !def.isRandomlyTicking(state)) continue;

                    // The callback may SetBlock anywhere — including into this
                    // same section, invalidating `section` if the write creates
                    // or destroys one. Nothing after the call touches `section`
                    // in this iteration, and the next iteration re-fetches
                    // nothing... which is exactly why the loop re-reads
                    // `section` below rather than caching a block pointer.
                    def.randomTick(*this, pos, id, state, m_tickRandom);

                    // Re-fetch: a growth callback that turned farmland to dirt
                    // (or a cane that grew into the section above) can have
                    // dropped or replaced this section.
                    section = chunk->GetSection(sectionIndex);
                    if (!section) break;
                }
            }
        }
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
        // Walk every loaded chunk and tick its BlockEntities. Mirrors MC's
        // Level.tickBlockEntities (Level.java:401-425). Per-BE Tick() is a
        // no-op by default; subclasses with timed behaviour (chest lid
        // auto-close, bell decay, conduit pulse, vault preview eject, etc.)
        // override. The NeedsTicking() filter is a cheap virtual that lets us
        // skip the vast majority of BEs that never tick — most placed BEs
        // are silent containers.
        //
        // deltaTime is a per-tick constant (50 ms / 0.05 s) since WorldLoop
        // runs at the server tick rate; passing it through keeps subclass
        // logic framerate-independent.
        if (!m_chunkProvider) return;
        constexpr float kTickDt = 1.0f / 20.0f;

        const auto positions = m_chunkProvider->GetLoadedChunkPositions();
        for (const auto& pos : positions) {
            auto chunk = m_chunkProvider->GetChunk(pos);
            if (!chunk) continue;
            for (auto& [localPos, be] : chunk->MutableBlockEntities()) {
                if (be && be->NeedsTicking()) {
                    be->Tick(this, kTickDt);
                }
            }
        }
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
        // Port of ServerLevel.tickTime: gameTime always advances, dayTime only
        // while the doDaylightCycle gamerule is enabled.
        m_gameTime++;
        if (m_doDaylightCycle) {
            m_dayTime++;
        }

        // TODO: Weather transitions (clear/rain/thunder)
    }

} // namespace Game
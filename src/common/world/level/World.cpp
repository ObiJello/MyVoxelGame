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
#include "server/level/ServerLevel.hpp"   // per-dimension nether-portal index
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

        // Which dimension's terrain this provider generates. ChunkProvider
        // copies it into the generation config at Initialize; setting the
        // inner field directly is what the provider warns about, because the
        // two would then be able to disagree.
        //
        // Getting this wrong is silent: a Nether level left at the default
        // generates a perfectly valid Overworld and nothing complains.
        config.dimension = std::string(DimensionGeneratorKey(m_dimension));

        // Where this dimension persists to, and which sub-folder it uses.
        // Empty savePath keeps the historical behaviour: regenerate from the
        // seed every session.
        config.savePath    = m_savePath;
        config.dimensionId = m_dimension;

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

        // Scheduled block ticks. The resolver is cache-ONLY (GetLoadedChunk,
        // never GetChunk): the drain runs inside the tick and must not block on
        // disk or terrain generation to find a queue, and a chunk that is not
        // resident has nothing to simulate anyway.
        m_blockTicks.SetContainerResolver(
            [this](int chunkX, int chunkZ) { return GetLoadedChunk(chunkX, chunkZ); });
        // A chunk loaded from disk with pending appointments has to announce
        // itself; the drain only visits chunks it already knows about.
        m_chunkProvider->SetChunkTicksLoadedCallback([this](Math::ChunkPos cp) {
            m_blockTicks.NoteChunkWithTicks(cp.x, cp.z);
        });
        // Same gate PerformRandomBlockTick uses: only chunks the server has
        // marked as simulating this tick. Appointments in a chunk that has
        // fallen out of range are kept, not dropped — they fire when a player
        // comes back, which is what stops a distant sand pillar from being
        // frozen mid-collapse forever.
        m_blockTicks.SetTickCheck([this](int chunkX, int chunkZ) {
            const uint64_t key =
                (static_cast<uint64_t>(static_cast<uint32_t>(chunkX)) << 32) |
                 static_cast<uint64_t>(static_cast<uint32_t>(chunkZ));
            return m_blockTickingKeys.count(key) != 0;
        });

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

        // NOT SetGlobalBlockAccess(this) any more. That global is a single
        // pointer feeding the raycast helpers, and with three Worlds the last
        // one constructed would win — a Nether level built after the Overworld
        // would silently become what every raycast reads.
        //
        // The overworld is registered explicitly by IntegratedServer instead,
        // and on a host process PlatformMain overwrites it with the CLIENT's
        // block access anyway, which is what the raycast consumers actually
        // want.
        Log::Info("ChunkProvider initialized successfully on server thread");
        return true;
    }

    void World::Shutdown() {
        if (m_chunkProvider) {
            m_chunkProvider->Shutdown();
            m_chunkProvider.reset();
        }

        // The global block access is NOT cleared here — see
        // InitializeChunkProvider. A non-overworld level shutting down must
        // not blank a pointer it never owned.

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

    BlockState World::GetBlockState(int worldX, int worldY, int worldZ) const {
        if (!IsValidPosition(worldX, worldY, worldZ) || !m_chunkProvider) {
            return BlockState{};      // air's default — nothing is there to read
        }
        return m_chunkProvider->GetBlockState(worldX, worldY, worldZ);
    }

    // Clamp to the dimension's build range, then hand whole (column, section)
    // tiles to the chunk provider. Cells outside the range, and cells in
    // columns that are not resident, keep the caller's zero — which is exactly
    // what GetBlockState answers for them today (BlockState{} = air, and air
    // has no collision), so the fast path and the generic path agree.
    bool World::IsRegionAllAir(const glm::ivec3& min, const glm::ivec3& max,
                               bool absentIsAir) const {
        if (!m_chunkProvider) return false;
        return m_chunkProvider->IsRegionAllAir(min, max, absentIsAir);
    }

    uint64_t World::RegionWriteStamp(const glm::ivec3& min, const glm::ivec3& max) const {
        return m_chunkProvider ? m_chunkProvider->RegionWriteStamp(min, max) : 0;
    }

    void World::GetBlockStatesInBox(const glm::ivec3& min, const glm::ivec3& max,
                                    BlockState* out) const {
        if (m_chunkProvider) m_chunkProvider->GetBlockStatesInBox(min, max, out);
        else IBlockAccess::GetBlockStatesInBox(min, max, out);
    }

    void World::FillCollisionMask(const glm::ivec3& origin, const glm::ivec3& size,
                                  int shiftZ, int shiftY, uint64_t* out) const {
        if (!m_chunkProvider) return;
        m_chunkProvider->FillCollisionMaskFast(origin, size, shiftZ, shiftY, out);
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

    // Re-entrancy budget for onPlace, which may write to the world and so may
    // re-enter SetBlock. MC bounds the analogous chain with a `recursionLeft`
    // counter starting at 512 (Block.updateOrDestroy); the same number is used
    // by NotifyNeighborBlocks below, and the two are deliberately separate
    // counters because they bound different chains.
    //
    // thread_local for the same reason as the neighbour counter: SetBlock can
    // be driven from the server thread or from a worker.
    static thread_local int s_setBlockDepth = 0;
    static constexpr int kMaxSetBlockDepth = 512;

    // World modification
    // The two short forms mean "this block, in its DEFAULT state" — MC's
    // `setBlockAndUpdate(pos, block.defaultBlockState())`. They used to forward
    // a literal 0, which was the same thing only while index 0 was the default.
    // It is not: state 0 is `StateDefinition.any()`, the first value of every
    // property, and BooleanProperty lists `true` first — so a plain
    // `SetBlock(pos, OakDoor)` would have placed an open, powered, upper half.
    bool World::SetBlock(int worldX, int worldY, int worldZ, BlockID blockId) {
        // Default to all updates for backwards compatibility
        return SetBlock(worldX, worldY, worldZ, blockId, UpdateFlags::All,
                        DefaultStateIndexOf(blockId));
    }

    bool World::SetBlock(int worldX, int worldY, int worldZ, BlockID blockId, uint32_t updateFlags) {
        return SetBlock(worldX, worldY, worldZ, blockId, updateFlags,
                        DefaultStateIndexOf(blockId));
    }

    bool World::SetBlock(int worldX, int worldY, int worldZ, BlockID blockId, uint32_t updateFlags,
                         BlockStateIndex stateIndex) {
        if (!IsValidPosition(worldX, worldY, worldZ)) {
            Log::Warning("Attempted to set block at invalid position (%d, %d, %d)",
                        worldX, worldY, worldZ);
            return false;
        }

        if (!m_chunkProvider) {
            Log::Warning("No chunk provider available for block placement");
            return false;
        }

        // Every accepted write bumps this, so anything holding a cached view
        // of the block field can tell that its snapshot went stale. Bumped
        // before the early-out below only would be wrong the other way — a
        // no-op write must NOT invalidate a valid snapshot — so it is bumped
        // after the change is known to be real, further down.
        //
        // Get the old block for comparison
        BlockID oldBlockId = GetBlock(worldX, worldY, worldZ);
        const BlockState oldState = GetBlockState(worldX, worldY, worldZ);

        // No change needed. The state comparison matters: re-orienting a block
        // in place (same id, new facing) must not be swallowed here.
        if (oldBlockId == blockId && oldState.Index() == stateIndex) {
            return true;
        }

        // The write is real from here on, so retire every cached view of the
        // block field. See World::BlockWriteEpoch.
        m_blockWriteEpoch.fetch_add(1, std::memory_order_release);

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
            // The two predicates below need NO chunk — HasBlockEntity is a flat
            // array lookup — so they are computed first and the whole block is
            // skipped when neither fires. That is every cell in a stone or dirt
            // crater, and it deletes one full chunk resolve per destroyed block
            // (the chunk is used for nothing else in this scope).
            const bool blockChanged = (oldBlockId != blockId);
            const bool oldHadBE = blockChanged && BlockEntityTypes::HasBlockEntity(oldBlockId);
            const bool newHasBE = blockChanged && BlockEntityTypes::HasBlockEntity(blockId);

            const auto chunkPos = Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
            auto chunk = (oldHadBE || newHasBE) ? m_chunkProvider->GetChunk(chunkPos) : nullptr;
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
                if (oldHadBE) {
                    chunk->RemoveBlockEntity(localX, worldY, localZ);
                    // Tell every watcher to drop their copy. The block change
                    // is already queued via the accumulator above, but a
                    // separate teardown packet keeps client-side lifecycle
                    // symmetric with the server (mirrors MC's implicit
                    // remove-via-new-blockstate by being explicit).
                    //
                    // Watcher-scoped, and scoped to THIS world's dimension: the
                    // packet is a bare x/y/z, so a broadcast would tell a
                    // player in another dimension to delete the block entity at
                    // those coordinates in the world they are actually in.
                    if (Server::g_integratedServer) {
                        Network::BlockEntityRemoveS2CPacket pkt{worldX, worldY, worldZ};
                        auto data = Network::Serialization::Serialize(pkt);
                        Server::g_integratedServer->SendToChunkWatchersAt(
                            GetDimension(), chunkPos,
                            Network::PacketId::BlockEntityRemoveS2C, data);
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

                        // Same scoping as the removal above — the payload is
                        // positional and carries no dimension of its own.
                        if (Server::g_integratedServer) {
                            auto data = Network::Serialization::Serialize(pkt);
                            Server::g_integratedServer->SendToChunkWatchersAt(
                                GetDimension(), chunkPos,
                                Network::PacketId::BlockEntityDataS2C, data);
                        }
                    }
                }
            }
        }

        // ── onPlace (MC BlockBehaviour.onPlace, reached from Level.setBlock)
        //    Runs after the chunk write so the callback sees the world as it
        //    now is, and BEFORE neighbour notification so a block that
        //    replaces itself here (fire becoming a nether portal) never gets
        //    a neighbour update for the state it is about to abandon.
        //
        //    Only on a real block change, matching the
        //    `!oldState.is(state.getBlock())` guard every MC implementation
        //    opens with. The callback may write to the world — including this
        //    very cell — so it is reached through the same recursion budget
        //    NotifyNeighborBlocks uses; see kMaxSetBlockDepth.
        if (oldBlockId != blockId) {
            const Block& newDef = BlockRegistry::Get(blockId);
            if (newDef.onPlace && s_setBlockDepth < kMaxSetBlockDepth) {
                ++s_setBlockDepth;
                newDef.onPlace(*this, glm::ivec3(worldX, worldY, worldZ),
                               BlockStates::FromIndex(blockId, stateIndex), oldState);
                --s_setBlockDepth;

                // MC Level.setBlock re-reads the cell after the chunk write
                // and only runs markAndNotifyBlock when what is there is still
                // what it wrote. That guard exists for exactly one case, and
                // it is the one that matters here: fire's onPlace turns the
                // whole frame into portal blocks INCLUDING this cell, so
                // carrying on would broadcast "fire" over the portal block
                // that just replaced it and would run neighbour updates for a
                // state that no longer exists. The callback's own writes have
                // already done both jobs.
                if (GetBlock(worldX, worldY, worldZ) != blockId) {
                    return true;
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
                // Resolved from THIS world, not "the" accumulator: there is one
                // per dimension, and a Nether edit accumulated into the
                // overworld's would be flushed to overworld watchers as a
                // change to their chunk at the same x/z. Returns null for the
                // client's predicted world, which the server does not own.
                auto* accumulator =
                    Server::g_integratedServer->GetChangeAccumulatorFor(*this);
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
                                            Game::BlockStates::FromIndex(blockId, stateIndex));
                }
            }
        }

        // Keep this dimension's nether-portal index in step.
        //
        // Here rather than at the individual call sites because EVERY route a
        // portal block can appear or vanish by funnels through SetBlock —
        // lighting one, PortalForcer building an exit, a player mining the
        // frame, and the updateShape cascade that collapse triggers. Missing
        // any one of them leaves an index entry pointing at a portal that is
        // not there, or loses one that is.
        if (oldBlockId != blockId &&
            (oldBlockId == BlockID::NetherPortal || blockId == BlockID::NetherPortal) &&
            Server::g_integratedServer) {
            if (auto* level = Server::g_integratedServer->GetLevel(m_dimension);
                level && level->World() == this) {
                const glm::ivec3 pos(worldX, worldY, worldZ);
                if (blockId == BlockID::NetherPortal) level->Portals().Add(pos);
                else                                  level->Portals().Remove(pos);
            }
        }

#if ENABLE_IMMERSIVE_PORTALS
        // An obsidian block removed may have been a nether portal's frame.
        // Cheap gate (obsidian only) here; the periodic sweep in
        // NetherPortalGeneration catches everything else.
        if (oldBlockId != blockId &&
            (oldBlockId == BlockID::Obsidian || oldBlockId == BlockID::CryingObsidian) &&
            Server::g_integratedServer) {
            Server::g_integratedServer->OnObsidianRemoved(m_dimension, glm::ivec3(worldX, worldY, worldZ));
        }
#endif

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
        // Moved to WorldDrops.hpp as DestroyBlockWithDrops — scaffolding and
        // dripstone need the identical "roll loot, pop it, clear the cell"
        // sequence, and three copies of a loot roll is three chances for them
        // to drift.
        auto destroyWithDrops = [&](const glm::ivec3& p, BlockID /*id*/) {
            DestroyBlockWithDrops(*this, p);
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
                BlockState outState;
                if (neighbourDef.neighborChanged(*this, n,
                                                 GetBlockState(n.x, n.y, n.z),
                                                 kFromNeighbour[oi], originId,
                                                 outState, &m_blockTicks)) {
                    const BlockID outBlock = outState.Block();
                    // AIR from updateShape means "I cannot exist any more" —
                    // MC's RedStoneWireBlock and the face-attached family both
                    // return it when their support goes, and MC destroys with
                    // drops rather than erasing.
                    if (outBlock == BlockID::Air) destroyWithDrops(n, id);
                    else SetBlock(n.x, n.y, n.z, outState, UpdateFlags::All);
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

    void World::SetGenerateStructures(bool enabled) {
        if (!m_chunkProvider) {
            return;
        }

        m_chunkProvider->SetGenerateStructures(enabled);
        Log::Info("Set world structure generation: %s", enabled ? "ON" : "OFF");
    }

    void World::SetWorldGenOptions(const std::string& worldType,
                                   const std::string& flatPreset,
                                   const std::string& flatLayers,
                                   const std::string& singleBiome) {
        if (!m_chunkProvider) {
            return;
        }

        m_chunkProvider->SetWorldGenOptions(worldType, flatPreset, flatLayers, singleBiome);
        Log::Info("Set world type: %s", worldType.c_str());
    }

    void World::SetWorldGenTweaks(const std::string& tweaksJson) {
        if (!m_chunkProvider) {
            return;
        }

        m_chunkProvider->SetWorldGenTweaks(tweaksJson);
        if (!tweaksJson.empty()) {
            Log::Info("World generation tweaks active (non-vanilla world)");
        }
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

        // Scheduled ticks are saved as delays relative to the world clock, so
        // the Anvil stack needs the current value before anything can save or
        // load this tick.
        if (m_chunkProvider) m_chunkProvider->SetGameTime(m_gameTime);

        // 1. Process any pending block updates (MC ServerLevel's tickPending
        //    phase — scheduled block ticks, which run BEFORE random ticks).
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

    // MC ServerLevel.tick's "tickPending" phase:
    //     this.blockTicks.tick(gameTime, 65536, this::tickBlock);
    //
    // Runs every scheduled block tick that has come due, in a globally
    // deterministic order (trigger time, then priority, then insertion order).
    // This is what drives falling blocks today and is where fluid flow and
    // redstone will hang when they land.
    //
    // PHASE NOTE: MC advances the clock (tickTime) BEFORE draining, and this
    // engine advances it at the END of WorldLoop, so the value read here is one
    // behind MC's for the same wall-clock tick. That is harmless and
    // deliberately left alone: scheduling and draining both read the same
    // m_gameTime, so a delay of 2 is still exactly two drains away. Moving the
    // advance to the front to match MC exactly would shift GetGameTime() by one
    // for every other subsystem in the tick, which is not this feature's call
    // to make.
    void World::ProcessBlockUpdates() {
        PROFILE_ZONE_N("BlockTicks");
        if (!m_chunkProvider) return;

        m_blockTicks.SetGameTime(m_gameTime);
        m_blockTicks.Tick(m_gameTime, kMaxBlockTicksPerTick,
                          [this](const glm::ivec3& pos, BlockID type) {
            // MC ServerLevel.tickBlock re-reads the cell and bails unless the
            // block is still the one the appointment was made for. Without this
            // a tick scheduled for sand fires on whatever the player put there
            // in the two ticks since — and FallingBlockTick would happily spawn
            // a falling stone.
            const BlockState state = GetBlockState(pos.x, pos.y, pos.z);
            if (!state.Is(type)) return;

            const Block& def = BlockRegistry::Get(type);
            if (!def.tick) return;
            def.tick(*this, pos, state, m_tickRandom);
        });
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

                    const BlockState state = section->StateAt(lx, ly, lz);
                    if (def.isRandomlyTicking && !def.isRandomlyTicking(state)) continue;

                    // The callback may SetBlock anywhere — including into this
                    // same section, invalidating `section` if the write creates
                    // or destroys one. Nothing after the call touches `section`
                    // in this iteration, and the next iteration re-fetches
                    // nothing... which is exactly why the loop re-reads
                    // `section` below rather than caching a block pointer.
                    def.randomTick(*this, pos, state, m_tickRandom);

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

            // The dirty bit is drained HERE, in the loop that already visits
            // every block entity, so it costs nothing extra.
            //
            // Container.setChanged has always called BlockEntity::MarkDirty,
            // but nothing ever READ it — and the only thing that marked a
            // chunk for saving was ChunkProvider::SetBlock. So moving an item
            // in a chest changed no block, left the chunk clean, and the
            // chest's contents were never written. Placing a block anywhere in
            // the same chunk would incidentally save them; that is the bug,
            // not the feature.
            bool anyDirty = false;
            for (auto& [localPos, be] : chunk->MutableBlockEntities()) {
                if (!be) continue;
                if (be->IsDirty()) {
                    be->ClearDirty();
                    anyDirty = true;
                }
                if (be->NeedsTicking()) {
                    be->Tick(this, kTickDt);
                }
            }
            if (anyDirty) m_chunkProvider->MarkChunkForSave(pos);
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
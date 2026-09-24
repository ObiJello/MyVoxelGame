// File: src/common/world/level/World.cpp
#include "World.hpp"
#include "common/world/lighting/LevelLightManager.hpp"
#include "common/world/lighting/BlockLightProperties.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/world/biome/BiomeZoom.hpp"
#include "../../core/Log.hpp"
#include "../../core/Profiling_Tracy.hpp"
#include "../block/BlockRegistry.hpp"
#include "../block/BlockPlacement.hpp"   // CanSurviveAt / HasModelledSurvivalRule
#include "../block/entity/BlockEntity.hpp"
#include "../block/entity/BlockEntityType.hpp"
#include "../block/entity/BlockEntityTypes.hpp"
#include "../chunk/Chunk.hpp"
#include "../math/WorldCoordinates.hpp"
#include "../../physics/RayCast.hpp"
#include "server/IntegratedServer.hpp"
#include "server/network/NetworkServer.hpp"
#include "server/world/tracking/SectionChangeAccumulator.hpp"
#include "server/level/ServerLevel.hpp"   // per-dimension portal indexes
#include "common/world/portal/PortalFamily.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/network/packets/game/BlockEntityDataS2CPacket.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/world/loot/LootTables.hpp"
#include "WorldDrops.hpp"
#include "../fluid/FlowingFluid.hpp"
#include "../block/piston/PistonBaseBlock.hpp"   // UpdateFromNeighbourShapes
#include "NeighborUpdater.hpp"
#include "../block/RedstoneComponents.hpp"
#include "../block/RedstoneStateUtil.hpp"
#include <unordered_set>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include "../block/RedstonePlus.hpp"
#include "../block/RedstoneSignal.hpp"
#include "../block/RedstoneFamilies.hpp"
#include "common/network/packets/game/BlockEntityDataS2CPacket.hpp"
#include <algorithm>
#include <cmath>


namespace Game {

    // ILevelWrite's default: a level that keeps no block entities drops the
    // one it was handed. Defined here, where BlockEntity is complete.
    void ILevelWrite::SetBlockEntity(const glm::ivec3& /*pos*/, std::unique_ptr<BlockEntity> entity) {
        entity.reset();
    }

    World::World()
        : m_neighborUpdater(std::make_unique<CollectingNeighborUpdater>(
              *this, CollectingNeighborUpdater::kDefaultMaxChainedNeighborUpdates)) {
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

        // The level light engine (MC LevelLightEngine, server thread). Its
        // registry is fed by IntegratedServer as chunks become resident and
        // looks the cache up (never loads) for the rest. A cache eviction on
        // any thread is queued to it, so an evicted chunk's layers stop
        // taking writes.
        m_light = std::make_unique<Lighting::LevelLightManager>(
            DimensionHasSkyLight(m_dimension),
            [this](Math::ChunkPos pos) -> std::shared_ptr<Chunk> {
                return m_chunkProvider ? m_chunkProvider->GetLoadedChunk(pos) : nullptr;
            });
        {
            Lighting::LevelLightManager* light = m_light.get();
            m_chunkProvider->SetLightEvictionHook(
                [light](Math::ChunkPos pos, const std::shared_ptr<Chunk>& chunk) {
                    light->NoteEvicted(pos, chunk);
                });
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
        // Likewise a chunk with worldgen post-processing still to apply.
        m_chunkProvider->SetChunkPostProcessCallback([this](Math::ChunkPos cp) {
            const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(cp.x)) << 32) |
                                  static_cast<uint64_t>(static_cast<uint32_t>(cp.z));
            std::lock_guard<std::mutex> lock(m_postProcessMutex);
            m_pendingPostProcess.insert(key);
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
            if (m_blockTickingKeys.count(key) == 0) return false;
            // MC's guarantee, made explicit: a chunk block-ticks only once
            // the chunks around it are resident (in vanilla the status
            // pyramid delivers a ticking chunk's neighbours as FULL first).
            // Here the ticket says "ticking" the moment it is placed, before
            // the loader has delivered the neighbours — and a component
            // ticking next to a hole reads air across the border: measured
            // 2026-09-11, a zero-delay gate flipped during load and the
            // counter behind it stepped before the machine was even started.
            // Cheap: asked only for chunks that have a tick due.
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx)
                    if ((dx || dz) && !IsChunkLoaded(chunkX + dx, chunkZ + dz)) return false;
            return true;
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
        // The generator now reports the seed it actually runs on (a world
        // that was never given one keeps the generator default).
        RefreshBiomeZoomSeed();
        Log::Info("ChunkProvider initialized successfully on server thread");
        return true;
    }

    void World::Shutdown() {
        // Idempotent and quiet the second time: ~ServerLevel shuts its world
        // down explicitly and ~World then calls this again.
        if (!m_light && !m_chunkProvider) return;

        // The light manager and the provider point at each other: the
        // provider's eviction hook holds the manager as a raw pointer
        // (NoteEvicted, callable from any thread that evicts), and the
        // manager's cache lookup reads m_chunkProvider. So: stop the provider
        // first (cache, saver and generator gone — nothing can evict any
        // more), then drop the manager, then the provider object. Resetting
        // the manager first left the hook naming a dead mutex for as long as
        // the provider could still evict.
        if (m_chunkProvider) {
            m_chunkProvider->Shutdown();
        }
        m_light.reset();
        m_chunkProvider.reset();

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

    int World::GetRawBrightness(int worldX, int worldY, int worldZ) const {
        // MC LevelReader.getRawBrightness(pos, 0).
        return std::max(GetBrightness(Lighting::LightLayer::Block, worldX, worldY, worldZ),
                        GetBrightness(Lighting::LightLayer::Sky, worldX, worldY, worldZ));
    }

    int World::GetBrightness(Lighting::LightLayer layer, int worldX, int worldY, int worldZ) const {
        // MC Level.getBrightness(layer, pos) — the level light engine's value.
        // Without one (a World that never initialised) the interface's
        // sky-column stand-in answers.
        if (!m_light) return IBlockAccess::GetBrightness(layer, worldX, worldY, worldZ);
        return m_light->GetBrightness(layer, worldX, worldY, worldZ);
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
        // MC LevelReader.getBiome -> BiomeManager.getBiome: the fuzzy zoom
        // picks which neighbouring quart this block shows, then the noise
        // biome is read there. Any Y is fine — the quart is clamped into the
        // column, as MC clamps it.
        if (!m_chunkProvider) {
            return kFallbackBiomeId;
        }
        const glm::ivec3 quart = BiomeZoom::NoiseQuartAt(
            m_biomeZoomSeed.load(std::memory_order_relaxed), worldX, worldY, worldZ);
        return m_chunkProvider->GetNoiseBiome(quart.x, quart.y, quart.z);
    }

    void World::RefreshBiomeZoomSeed() {
        m_biomeZoomSeed.store(BiomeZoom::ObfuscateSeed(GetGenerationSeed()),
                              std::memory_order_relaxed);
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
        return SetBlock(glm::ivec3(worldX, worldY, worldZ),
                        BlockStates::FromIndex(blockId, stateIndex), updateFlags, kUpdateLimit);
    }

    // Port of MC Level.setBlock(pos, state, flags, updateLimit) layered over
    // LevelChunk.setBlockState(pos, state, flags). The two are one function
    // here because the chunk half's side effects (block-entity teardown,
    // affectNeighborsAfterRemoval, onPlace) and the level half's (client
    // notification, neighbour updates, shape updates) have to interleave in
    // vanilla's exact order, and that order is what redstone timing is built
    // on:
    //
    //   1. write the cell
    //   2. old block entity goes (side effects unless flag 256)
    //   3. oldState.affectNeighborsAfterRemoval   (flag 1 or 64, block change)
    //   4. bail if step 3 replaced the cell again
    //   5. state.onPlace                          (unless flag 512)
    //   6. new block entity is created
    //   7. bail if step 5 changed the cell
    //   8. sendBlockUpdated                        (flag 2)
    //   9. updateNeighborsAt + updateNeighbourForOutputSignal   (flag 1)
    //  10. the three shape walks                   (unless flag 16, limit > 0)
    // Dev trace: OBEY_RS_TRACE="x,y,z;x,y,z;..." lists cells whose every
    // SetBlock is appended to OBEY_RS_TRACE_OUT as "gameTime x y z slug on"
    // (on = wire power > 0 / lit / powered). For lining the engine's redstone
    // event order up against the tick simulator's recorder (play2 --rec).
    namespace {
        struct RedstoneTrace {
            bool enabled = false;
            std::unordered_set<uint64_t> cells;
            FILE* out = nullptr;
            static uint64_t Key(int x, int y, int z) {
                return (static_cast<uint64_t>(static_cast<uint32_t>(x) & 0x3FFFFFFu) << 38) ^
                       (static_cast<uint64_t>(static_cast<uint32_t>(y) & 0xFFFu) << 26) ^
                       static_cast<uint64_t>(static_cast<uint32_t>(z) & 0x3FFFFFFu);
            }
            RedstoneTrace() {
                const char* spec = std::getenv("OBEY_RS_TRACE");
                const char* outPath = std::getenv("OBEY_RS_TRACE_OUT");
                if (!spec || !outPath) return;
                int x = 0, y = 0, z = 0;
                const char* p = spec;
                while (*p) {
                    if (std::sscanf(p, "%d,%d,%d", &x, &y, &z) == 3) cells.insert(Key(x, y, z));
                    const char* semi = std::strchr(p, ';');
                    if (!semi) break;
                    p = semi + 1;
                }
                out = std::fopen(outPath, "w");
                enabled = out && !cells.empty();
            }
        };
        RedstoneTrace& Trace() { static RedstoneTrace t; return t; }
    }

    bool World::SetBlock(const glm::ivec3& pos, BlockState state, uint32_t updateFlags,
                         int updateLimit) {
        const int worldX = pos.x, worldY = pos.y, worldZ = pos.z;
        if (Trace().enabled && Trace().cells.count(RedstoneTrace::Key(worldX, worldY, worldZ))) {
            const BlockID id = state.Block();
            int on = 0;
            if (id == BlockID::RedstoneWire) on = PowerOf(state) > 0;
            else if (state.HasProperty(PropertyId::LIT)) on = LitOf(state);
            else if (state.HasProperty(PropertyId::POWERED)) on = PoweredOf(state);
            if (id == BlockID::DisplayBlock) {          // colour bits: red 1, green 2, blue 4 (+8 when powered)
                on = (BoolOf(state, PropertyId::RED) ? 1 : 0) | (BoolOf(state, PropertyId::GREEN) ? 2 : 0) |
                     (BoolOf(state, PropertyId::BLUE) ? 4 : 0) | (PoweredOf(state) ? 8 : 0);
            }
            std::fprintf(Trace().out, "%lld %d %d %d %s %d\n", static_cast<long long>(m_gameTime), worldX, worldY, worldZ,
                         BlockRegistry::Get(id).registrySlug.c_str(), on);
            std::fflush(Trace().out);
        }
        if (!IsValidPosition(worldX, worldY, worldZ)) {
            Log::Warning("Attempted to set block at invalid position (%d, %d, %d)",
                        worldX, worldY, worldZ);
            return false;
        }

        if (!m_chunkProvider) {
            Log::Warning("No chunk provider available for block placement");
            return false;
        }

        const BlockID    blockId    = state.Block();
        const BlockState oldState   = GetBlockState(worldX, worldY, worldZ);
        const BlockID    oldBlockId = oldState.Block();

        // No change needed. MC's LevelChunk.setBlockState answers null here
        // and Level.setBlock returns false; this engine has always answered
        // true and callers treat false as a failure to report, so it stays.
        if (oldState == state) {
            return true;
        }

        // The write is real from here on, so retire every cached view of the
        // block field. See World::BlockWriteEpoch.
        m_blockWriteEpoch.fetch_add(1, std::memory_order_release);

        // Set the block using the chunk provider
        m_chunkProvider->SetBlock(worldX, worldY, worldZ, blockId, state.Index());

        // LevelChunk.setBlockState's light half: when the two states light
        // differently, the column's sky sources move and the cell is queued
        // for the light engine (drained once per tick, IntegratedServer).
        if (m_light && Lighting::BlockLightProperties::HasDifferentLightProperties(oldState, state)) {
            const auto cp = Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
            if (const auto lightChunk = m_chunkProvider->GetLoadedChunk(cp)) {
                m_light->OnBlockChanged(*lightChunk, worldX, worldY, worldZ, oldState, state);
            }
        }

        const bool blockChanged  = (oldBlockId != blockId);
        const bool movedByPiston = (updateFlags & UpdateFlags::MoveByPiston) != 0;
        const bool sideEffects   = (updateFlags & UpdateFlags::SkipBlockEntitySideEffects) == 0;
        const auto chunkPos = Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
        const int  localX = worldX - chunkPos.x * 16;
        const int  localZ = worldZ - chunkPos.z * 16;

        // ── Old block entity (LevelChunk.setBlockState step 2) ───────────
        //
        // ONLY when the BLOCK changes. A state-only edit (same block, new
        // facing or chest `type`) must keep its block entity: MC's setBlock
        // only swaps the BE when the new state's block differs.
        //
        // Without this guard, re-typing a chest as it pairs or unpairs
        // destroys and recreates its block entity — silently emptying the
        // chest next to the one you just placed, and churning a
        // BlockEntityRemove + BlockEntityData pair at the client for a
        // block that never went away.
        if (blockChanged && BlockEntityTypes::HasBlockEntity(oldBlockId)) {
            if (auto chunk = m_chunkProvider->GetChunk(chunkPos)) {
                // MC BlockEntity.preRemoveSideEffects, gated on flag 256. A
                // container's contents are dropped by whoever is breaking it
                // in this engine (PlayerSession / explosions); the piston's
                // moving-block entity finishes its move here, which may
                // re-write this very cell.
                if (sideEffects) {
                    if (BlockEntity* be = chunk->GetBlockEntity(localX, worldY, localZ)) {
                        be->PreRemoveSideEffects(*this, pos, oldState);
                    }
                }
                if (!chunk->GetBlockEntity(localX, worldY, localZ)) {
                    // Already gone (the side effect removed it).
                } else if (oldBlockId == BlockID::MovingPiston) {
                    // Never sent to clients — see SetBlockEntity.
                    chunk->RemoveBlockEntity(localX, worldY, localZ);
                } else {
                chunk->RemoveBlockEntity(localX, worldY, localZ);
                // Tell every watcher to drop their copy. Watcher-scoped, and
                // scoped to THIS world's dimension: the packet is a bare
                // x/y/z, so a broadcast would tell a player in another
                // dimension to delete the block entity at those coordinates
                // in the world they are actually in.
                if (Server::g_integratedServer) {
                    Network::BlockEntityRemoveS2CPacket pkt{worldX, worldY, worldZ};
                    auto data = Network::Serialization::Serialize(pkt);
                    Server::g_integratedServer->SendToChunkWatchersAt(
                        GetDimension(), chunkPos,
                        Network::PacketId::BlockEntityRemoveS2C, data);
                }
                }
            }
        }

        // ── affectNeighborsAfterRemoval (step 3) ────────────────────────
        //
        // `blockChanged || newBlock instanceof BaseRailBlock` — a rail
        // re-shaping counts as a removal for its neighbours, which is what
        // lets a detector rail's power follow a track edit.
        if (blockChanged || IsRailBlock(blockId)) {
            if ((updateFlags & UpdateFlags::NotifyNeighbors) || movedByPiston) {
                const Block& oldDef = BlockRegistry::Get(oldBlockId);
                if (oldDef.affectNeighborsAfterRemoval && s_setBlockDepth < kMaxSetBlockDepth) {
                    ++s_setBlockDepth;
                    oldDef.affectNeighborsAfterRemoval(*this, pos, oldState, movedByPiston);
                    --s_setBlockDepth;
                }
            }
        }

        // Step 4: `if (!section.getBlockState(...).is(newBlock)) return null`.
        if (GetBlock(worldX, worldY, worldZ) != blockId) {
            return false;
        }

        // ── onPlace (step 5) ─────────────────────────────────────────────
        //
        // Runs after the chunk write so the callback sees the world as it
        // now is, and BEFORE neighbour notification so a block that
        // replaces itself here (fire becoming a nether portal) never gets a
        // neighbour update for the state it is about to abandon.
        //
        // Every write reaches it, state-only edits included — see the note
        // on BlockOnPlaceFn. The callback may write to the world, including
        // this very cell, so it runs under the recursion budget.
        if ((updateFlags & UpdateFlags::SkipOnPlace) == 0) {
            const Block& newDef = BlockRegistry::Get(blockId);
            if (newDef.onPlace && s_setBlockDepth < kMaxSetBlockDepth) {
                ++s_setBlockDepth;
                newDef.onPlace(*this, pos, state, oldState, movedByPiston);
                --s_setBlockDepth;
            }
        }

        // ── New block entity (step 6) ────────────────────────────────────
        //
        // Created AFTER onPlace, as LevelChunk does, and only if the cell
        // still holds the block and nothing has installed one already (a
        // piston hands the moving-block cell a pre-built entity through
        // SetBlockEntity from inside its own write).
        // MC MovingPistonBlock.newBlockEntity returns null: the piston hands
        // the cell its carried state through SetBlockEntity itself, and an
        // empty default here would be broadcast and then replaced.
        if (blockChanged && BlockEntityTypes::HasBlockEntity(blockId) &&
            blockId != BlockID::MovingPiston &&
            GetBlock(worldX, worldY, worldZ) == blockId) {
            if (auto chunk = m_chunkProvider->GetChunk(chunkPos)) {
                if (!chunk->GetBlockEntity(localX, worldY, localZ)) {
                    if (const auto* type = BlockEntityTypes::ForBlock(blockId)) {
                        auto be = type->Create(pos, blockId);
                        be->SetLevel(this);
                        // Snapshot the freshly-created state for the broadcast
                        // BEFORE handing the BE to the chunk (the chunk owns
                        // it after SetBlockEntity).
                        BroadcastBlockEntity(pos, *be);
                        chunk->SetBlockEntity(localX, worldY, localZ, std::move(be));
                    }
                }
            }
        }

        // Step 7: MC Level.setBlock re-reads the cell after the chunk write
        // and only runs the notifications when what is there is still what
        // it wrote. That guard exists for exactly one case, and it is the
        // one that matters here: fire's onPlace turns the whole frame into
        // portal blocks INCLUDING this cell, so carrying on would broadcast
        // "fire" over the portal block that just replaced it and would run
        // neighbour updates for a state that no longer exists. The
        // callback's own writes have already done both jobs.
        if (GetBlockState(worldX, worldY, worldZ) != state) {
            return true;
        }

        // ── sendBlockUpdated (step 8, flag 2) ────────────────────────────
        if (updateFlags & UpdateFlags::UpdateClients) {
            // Mark section for remeshing
            OnBlockChanged(worldX, worldY, worldZ);

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
                    uint8_t lx = worldX & 0xF;
                    uint8_t ly = (worldY + 64) & 0xF;  // Adjust for min Y of -64
                    uint8_t lz = worldZ & 0xF;

                    // Accumulate the change (will be broadcast at end of tick).
                    // Block and state travel together — a re-orientation is a
                    // real change that watchers must be told about.
                    accumulator->accumulate(sp, lx, ly, lz, state);
                }
            }
        }

        // ── updateNeighborsAt (step 9, flag 1) ───────────────────────────
        if (updateFlags & UpdateFlags::NotifyNeighbors) {
            UpdateNeighborsAt(pos, oldBlockId);
            if (BlockRegistry::Get(blockId).hasAnalogOutputSignal) {
                UpdateNeighbourForOutputSignal(pos, blockId);
            }
        }

        // ── The shape walks (step 10, unless flag 16) ────────────────────
        //
        // `updateFlags & -34` — the neighbours' writes carry every flag but
        // UPDATE_NEIGHBORS and UPDATE_SUPPRESS_DROPS.
        if ((updateFlags & UpdateFlags::KnownShape) == 0 && updateLimit > 0) {
            const uint32_t neighbourFlags =
                updateFlags & ~(UpdateFlags::NotifyNeighbors | UpdateFlags::SuppressDrops);
            UpdateIndirectNeighbourShapes(oldState, pos, neighbourFlags, updateLimit - 1);
            UpdateNeighbourShapes(state, pos, neighbourFlags, updateLimit - 1);
            UpdateIndirectNeighbourShapes(state, pos, neighbourFlags, updateLimit - 1);
        }

        // Keep this dimension's per-family portal indexes in step.
        //
        // Here rather than at the individual call sites because EVERY route a
        // portal block can appear or vanish by funnels through SetBlock —
        // lighting one, PortalForcer building an exit, a player mining the
        // frame, and the updateShape cascade that collapse triggers. Missing
        // any one of them leaves an index entry pointing at a portal that is
        // not there, or loses one that is. Each family (nether_portal,
        // hush_portal) keeps its own index.
        if (blockChanged && Server::g_integratedServer) {
            const PortalFamily* wasFamily = FamilyOfPortalBlock(oldBlockId);
            const PortalFamily* nowFamily = FamilyOfPortalBlock(blockId);
            if (wasFamily || nowFamily) {
                if (auto* level = Server::g_integratedServer->GetLevel(m_dimension);
                    level && level->World() == this) {
                    if (wasFamily && wasFamily != nowFamily) level->Portals(wasFamily->id).Remove(pos);
                    if (nowFamily)                           level->Portals(nowFamily->id).Add(pos);
                }
            }
        }

        // MC ServerLevel.onBlockStateChange → PoiManager: a bed, job site or
        // bell appearing or vanishing. Same funnel, same reason, as the
        // portal indexes above; the cheap block test keeps the lookup off
        // every ordinary edit.
        if (oldState != state && Server::g_integratedServer &&
            (BlockMayHavePoi(oldBlockId) || BlockMayHavePoi(blockId))) {
            if (auto* level = Server::g_integratedServer->GetLevel(m_dimension);
                level && level->World() == this) {
                level->Poi().OnBlockStateChange(pos, oldState, state);
            }
        }

#if ENABLE_IMMERSIVE_PORTALS
        // A frame block removed (obsidian, reinforced deepslate) may have
        // been an immersive portal's frame. Cheap gate (frame blocks only)
        // here; the periodic sweep in NetherPortalGeneration catches
        // everything else.
        if (blockChanged && FamilyOfFrameBlock(oldBlockId) && Server::g_integratedServer) {
            Server::g_integratedServer->OnFrameBlockRemoved(m_dimension, pos, oldBlockId);
        }
#endif

        return true;
    }

    bool World::RemoveBlock(const glm::ivec3& pos, bool movedByPiston) {
        // MC: setBlock(pos, fluidState.createLegacyBlock(), 3 | (movedByPiston ? 64 : 0)).
        const BlockState old = GetBlockState(pos.x, pos.y, pos.z);
        const BlockState next = BlockRegistry::ContainsWater(old)
            ? BlockStates::Default(BlockID::Water) : BlockState{};
        return SetBlock(pos, next,
                        UpdateFlags::All | (movedByPiston ? UpdateFlags::MoveByPiston : 0u),
                        kUpdateLimit);
    }

    bool World::DestroyBlock(const glm::ivec3& pos, bool dropResources, int updateLimit) {
        const BlockState state = GetBlockState(pos.x, pos.y, pos.z);
        if (state.Block() == BlockID::Air) return false;
        // MC: spawnDestroyParticles — no level-event channel yet.
        if (dropResources) {
            DropBlockLoot(*this, pos, state);
        }
        const BlockState next = BlockRegistry::ContainsWater(state)
            ? BlockStates::Default(BlockID::Water) : BlockState{};
        return SetBlock(pos, next, UpdateFlags::All, updateLimit);
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

    // ── Neighbour notification ──────────────────────────────────────────────

    void World::UpdateNeighborsAt(const glm::ivec3& pos, BlockID sourceBlock) {
        m_neighborUpdater->UpdateNeighborsAtExceptFromFacing(pos, sourceBlock, std::nullopt);
    }

    void World::UpdateNeighborsAtExceptFromFacing(const glm::ivec3& pos, BlockID sourceBlock,
                                                  Direction skipDirection) {
        m_neighborUpdater->UpdateNeighborsAtExceptFromFacing(pos, sourceBlock, skipDirection);
    }

    void World::NeighborChanged(const glm::ivec3& pos, BlockID sourceBlock) {
        m_neighborUpdater->NeighborChanged(pos, sourceBlock);
    }

    void World::NeighborChanged(BlockState state, const glm::ivec3& pos, BlockID sourceBlock,
                                bool movedByPiston) {
        m_neighborUpdater->NeighborChanged(state, pos, sourceBlock, movedByPiston);
    }

    // MC Level.updateNeighbourForOutputSignal, verbatim: comparators beside
    // the block, or one CONDUCTOR away from it, get a neighborChanged.
    void World::UpdateNeighbourForOutputSignal(const glm::ivec3& pos, BlockID changedBlock) {
        for (Direction direction : {Direction::North, Direction::South,
                                    Direction::West,  Direction::East}) {
            glm::ivec3 relativePos(pos.x + StepX(direction), pos.y, pos.z + StepZ(direction));
            if (!IsPositionLoaded(relativePos.x, relativePos.y, relativePos.z)) continue;
            BlockState state = GetBlockState(relativePos.x, relativePos.y, relativePos.z);
            if (state.Is(BlockID::Comparator)) {
                NeighborChanged(state, relativePos, changedBlock, false);
            } else if (IsRedstoneConductor(*this, relativePos, state)) {
                relativePos += glm::ivec3(StepX(direction), 0, StepZ(direction));
                state = GetBlockState(relativePos.x, relativePos.y, relativePos.z);
                if (state.Is(BlockID::Comparator)) {
                    NeighborChanged(state, relativePos, changedBlock, false);
                }
            }
        }
    }

    void World::NeighborShapeChanged(Direction direction, const glm::ivec3& pos,
                                     const glm::ivec3& neighborPos, BlockState neighborState,
                                     uint32_t updateFlags, int updateLimit) {
        m_neighborUpdater->ShapeUpdate(direction, neighborState, pos, neighborPos,
                                       updateFlags, updateLimit);
    }

    void World::UpdateNeighbourShapes(BlockState state, const glm::ivec3& pos,
                                      uint32_t updateFlags, int updateLimit) {
        for (Direction direction : CollectingNeighborUpdater::kUpdateShapeOrder) {
            const glm::ivec3 neighbour(pos.x + StepX(direction), pos.y + StepY(direction),
                                       pos.z + StepZ(direction));
            NeighborShapeChanged(Opposite(direction), neighbour, pos, state,
                                 updateFlags, updateLimit);
        }
    }

    void World::UpdateIndirectNeighbourShapes(BlockState state, const glm::ivec3& pos,
                                              uint32_t updateFlags, int updateLimit) {
        const Block& def = BlockRegistry::Get(state.Block());
        if (def.updateIndirectNeighbourShapes) {
            def.updateIndirectNeighbourShapes(*this, pos, state, updateFlags, updateLimit);
        }
    }

    // MC NeighborUpdater.executeShapeUpdate + the engine's support rule.
    void World::ExecuteShapeUpdate(Direction direction, const glm::ivec3& pos,
                                   const glm::ivec3& neighborPos, BlockState neighborState,
                                   uint32_t updateFlags, int updateLimit) {
        if (!IsValidPosition(pos.x, pos.y, pos.z)) return;
        const BlockState currentState = GetBlockState(pos.x, pos.y, pos.z);
        const BlockID    id           = currentState.Block();
        if (id == BlockID::Air) return;   // air's updateShape is the identity

        // UPDATE_SKIP_SHAPE_UPDATE_ON_WIRE: the experimental evaluator writes
        // every wire itself and does not want the shape walk touching them.
        if ((updateFlags & UpdateFlags::SkipShapeUpdateOnWire) && id == BlockID::RedstoneWire) {
            return;
        }

        const Block& def = BlockRegistry::Get(id);
        BlockState newState = currentState;

        // MC BlockState.updateShape — a neighbour may TRANSFORM rather than
        // just survive-or-die.
        bool transformed = false;
        if (def.updateShape) {
            BlockState outState;
            if (def.updateShape(*this, pos, currentState, direction, neighborState.Block(),
                                outState, &m_blockTicks)) {
                newState    = outState;
                transformed = true;
            }
        }

        // MC SimpleWaterloggedBlock.updateShape's shared first line —
        //     if (WATERLOGGED) ticks.scheduleTick(pos, Fluids.WATER, delay);
        // — which every one of the 386 waterloggable blocks (and kelp,
        // seagrass, the bubble column) repeats verbatim. Done here, once,
        // rather than in each family's hook: the water a fence holds has to
        // start flowing the moment the block beside it is broken, and a
        // block that never got its own updateShape ported would otherwise
        // hold its water forever. Plain water/lava cells book their own
        // tick through LiquidBlock's updateShape above.
        if (id != BlockID::Water && id != BlockID::Lava && BlockRegistry::ContainsWater(currentState)) {
            Fluids::ScheduleTick(*this, pos, FluidType::Water);
        }

        // The engine's generic support rule, for blocks flagged
        // needsSupportBelow that have no updateShape of their own. MC's
        // VegetationBlock.updateShape is `direction == DOWN && !canSurvive
        // → AIR`, so only the change from below is consulted.
        if (!transformed && def.needsSupportBelow && direction == Direction::Down &&
            !CanBlockSurviveAt(pos.x, pos.y, pos.z)) {
            newState = BlockState{};
        }

        UpdateOrDestroy(currentState, newState, pos, updateFlags, updateLimit);
    }

    // MC Block.updateOrDestroy, verbatim: AIR from updateShape is a DESTROY
    // (with drops unless flag 32), anything else a write that clears the
    // suppress-drops bit.
    void World::UpdateOrDestroy(BlockState oldState, BlockState newState, const glm::ivec3& pos,
                                uint32_t updateFlags, int updateLimit) {
        if (newState == oldState) return;
        if (newState.Block() == BlockID::Air) {
            DestroyBlock(pos, (updateFlags & UpdateFlags::SuppressDrops) == 0, updateLimit);
        } else {
            SetBlock(pos, newState, updateFlags & ~UpdateFlags::SuppressDrops, updateLimit);
        }
    }

    // MC NeighborUpdater.executeUpdate → BlockState.handleNeighborChanged.
    void World::ExecuteNeighborUpdate(BlockState state, const glm::ivec3& pos, BlockID sourceBlock,
                                      bool movedByPiston) {
        if (!IsValidPosition(pos.x, pos.y, pos.z)) return;
        const Block& def = BlockRegistry::Get(state.Block());
        if (!def.neighborChanged) return;
        def.neighborChanged(*this, pos, state, sourceBlock, movedByPiston);
    }

    // ── Block events ────────────────────────────────────────────────────────

    void World::BlockEvent(const glm::ivec3& pos, BlockID block, int b0, int b1) {
        m_blockEvents.push_back(BlockEventData{pos, block, b0, b1});
    }

    bool World::ShouldTickBlocksAt(const glm::ivec3& pos) const {
        const auto cp = Math::WorldCoordinates::WorldToChunkPos(pos.x, pos.z);
        const uint64_t key =
            (static_cast<uint64_t>(static_cast<uint32_t>(cp.x)) << 32) |
             static_cast<uint64_t>(static_cast<uint32_t>(cp.z));
        return m_blockTickingKeys.count(key) != 0;
    }

    // ── Block entities ──────────────────────────────────────────────────────

    BlockEntity* World::GetBlockEntity(const glm::ivec3& pos) {
        if (!m_chunkProvider || !IsValidPosition(pos.x, pos.y, pos.z)) return nullptr;
        const auto cp = Math::WorldCoordinates::WorldToChunkPos(pos.x, pos.z);
        auto chunk = m_chunkProvider->GetLoadedChunk(cp);
        if (!chunk) return nullptr;
        return chunk->GetBlockEntity(pos.x - cp.x * 16, pos.y, pos.z - cp.z * 16);
    }

    void World::SetBlockEntity(const glm::ivec3& pos, std::unique_ptr<BlockEntity> entity) {
        if (!m_chunkProvider || !entity || !IsValidPosition(pos.x, pos.y, pos.z)) return;
        const auto cp = Math::WorldCoordinates::WorldToChunkPos(pos.x, pos.z);
        auto chunk = m_chunkProvider->GetLoadedChunk(cp);
        if (!chunk) return;
        entity->SetLevel(this);
        // MC BlockEntity.getUpdatePacket is null for PistonMovingBlockEntity:
        // the client builds its own from the block event, so the server's is
        // never sent (a chunk send still carries it, as vanilla's does).
        if (entity->GetBlockId() != BlockID::MovingPiston) BroadcastBlockEntity(pos, *entity);
        chunk->SetBlockEntity(pos.x - cp.x * 16, pos.y, pos.z - cp.z * 16, std::move(entity));
        m_chunkProvider->MarkChunkForSave(cp);
    }

    void World::RemoveBlockEntity(const glm::ivec3& pos) {
        if (!m_chunkProvider || !IsValidPosition(pos.x, pos.y, pos.z)) return;
        const auto cp = Math::WorldCoordinates::WorldToChunkPos(pos.x, pos.z);
        auto chunk = m_chunkProvider->GetLoadedChunk(cp);
        if (!chunk) return;
        BlockEntity* existing = chunk->GetBlockEntity(pos.x - cp.x * 16, pos.y, pos.z - cp.z * 16);
        if (!existing) return;
        const bool movingPiston = existing->GetBlockId() == BlockID::MovingPiston;
        chunk->RemoveBlockEntity(pos.x - cp.x * 16, pos.y, pos.z - cp.z * 16);
        // A moving-piston cell's entity was never sent; the client retires
        // its own copy when the final block arrives.
        if (Server::g_integratedServer && !movingPiston) {
            Network::BlockEntityRemoveS2CPacket pkt{pos.x, pos.y, pos.z};
            auto data = Network::Serialization::Serialize(pkt);
            Server::g_integratedServer->SendToChunkWatchersAt(
                GetDimension(), cp, Network::PacketId::BlockEntityRemoveS2C, data);
        }
    }

    void World::BlockEntityChanged(const glm::ivec3& pos) {
        BlockEntity* be = GetBlockEntity(pos);
        if (!be) return;
        be->MarkDirty();
        BroadcastBlockEntity(pos, *be);
    }

    void World::BroadcastBlockEntity(const glm::ivec3& pos, const BlockEntity& entity) {
        if (!Server::g_integratedServer || !entity.GetType()) return;
        Network::BlockEntityDataS2CPacket pkt(pos.x, pos.y, pos.z, entity.GetType()->TypeId());
        Network::PacketBuffer scratch;
        entity.Save(scratch);
        pkt.dataBlob = scratch.GetData();
        auto data = Network::Serialization::Serialize(pkt);
        // Same scoping as the removal — the payload is positional and
        // carries no dimension of its own.
        Server::g_integratedServer->SendToChunkWatchersAt(
            GetDimension(), Math::WorldCoordinates::WorldToChunkPos(pos.x, pos.z),
            Network::PacketId::BlockEntityDataS2C, data);
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

    void World::SaveAllChunks(bool wait) {
        if (!m_chunkProvider) {
            return;
        }

        Log::Info("Saving all loaded chunks...");
        m_chunkProvider->SaveAllDirtyChunks(wait);
    }

    size_t World::SaveDirtyChunksEagerly(size_t maxChunks, std::chrono::steady_clock::time_point deadline) {
        return m_chunkProvider ? m_chunkProvider->SaveDirtyChunksEagerly(maxChunks, deadline) : 0;
    }

    void World::SetGenerationSeed(int64_t seed) {
        if (!m_chunkProvider) {
            return;
        }

        m_chunkProvider->SetGenerationSeed(seed);
        RefreshBiomeZoomSeed();
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

        // MC runs a chunk's post-processing as it becomes a ticking chunk,
        // in the chunk source's part of the tick — before the level's.
        PostProcessGeneration();

        // MC ServerLevel.handlingTick spans tickPending through blockEvents.
        m_handlingTick = true;

        // 1. Process any pending block updates (MC ServerLevel's tickPending
        //    phase — scheduled block ticks, which run BEFORE random ticks).
        //    redstone_plus: the delayed components' re-checks deferred by
        //    everything since the last tick (player interactions) go first,
        //    then the ones this tick's cascades produce.
        if (!m_redstoneFrozen) {
            if (RedstonePlus::Enabled()) RedstoneFlushDeferredChecks(*this);
            ProcessBlockUpdates();
            if (RedstonePlus::Enabled()) RedstoneFlushDeferredChecks(*this);
        }

        // 2. Perform random block ticks (growth, decay, etc.)
        PerformRandomBlockTick();

        // 3. Process scheduled block events
        ProcessBlockEvents();

        m_handlingTick = false;

        // 4. Update tile entities
        TileEntityTick();

        // 5. Update entities
        EntityTick();

        // 6. Update world time and weather
        WorldTimeWeatherTick();
    }

    // MC ChunkMap.prepareTickingChunk waits for the chunk's 3x3 at FULL, then
    // LevelChunk.postProcessGeneration visits every marked cell:
    //
    //     FluidState fluidState = blockState.getFluidState();
    //     if (!fluidState.isEmpty()) fluidState.tick(level, pos, blockState);
    //     if (!(blockState.getBlock() instanceof LiquidBlock)) {
    //         BlockState newState = Block.updateFromNeighbourShapes(blockState, level, pos);
    //         if (newState != blockState) level.setBlock(pos, newState, 276);
    //     }
    //
    // What worldgen marks: fluids at aquifer and cave edges (they start
    // flowing), soul sand and magma under water (their bubble columns),
    // mushrooms (their footing), sculk veins. A chunk not ticking yet stays
    // pending; one that left the cache keeps its list in its save and is
    // announced again when it comes back.
    void World::PostProcessGeneration() {
        std::vector<uint64_t> pending;
        {
            std::lock_guard<std::mutex> lock(m_postProcessMutex);
            if (m_pendingPostProcess.empty()) return;
            pending.assign(m_pendingPostProcess.begin(), m_pendingPostProcess.end());
        }
        PROFILE_ZONE_N("PostProcessGeneration");
        for (const uint64_t key : pending) {
            if (m_blockTickingKeys.count(key) == 0) continue;
            const int chunkX = static_cast<int32_t>(static_cast<uint32_t>(key >> 32));
            const int chunkZ = static_cast<int32_t>(static_cast<uint32_t>(key));
            std::shared_ptr<Chunk> chunk = GetLoadedChunk(chunkX, chunkZ);
            if (!chunk) {
                std::lock_guard<std::mutex> lock(m_postProcessMutex);
                m_pendingPostProcess.erase(key);
                continue;
            }
            bool surrounded = true;
            for (int dz = -1; dz <= 1 && surrounded; ++dz) {
                for (int dx = -1; dx <= 1 && surrounded; ++dx) {
                    if ((dx != 0 || dz != 0) && !GetLoadedChunk(chunkX + dx, chunkZ + dz)) surrounded = false;
                }
            }
            if (!surrounded) continue;
            {
                std::lock_guard<std::mutex> lock(m_postProcessMutex);
                m_pendingPostProcess.erase(key);
            }

            std::vector<std::vector<int16_t>> sections;
            {
                const auto guard = chunk->LockExclusive();
                sections.swap(chunk->postProcessing);
            }
            if (sections.empty()) continue;
            // The drained list must not come back from the save.
            m_chunkProvider->MarkChunkForSave(Math::ChunkPos{chunkX, chunkZ});

            for (size_t si = 0; si < sections.size(); ++si) {
                const int baseY = Math::WorldCoordinates::MIN_WORLD_Y + static_cast<int>(si) * 16;
                for (const int16_t packed : sections[si]) {
                    const glm::ivec3 pos(chunkX * 16 + (packed & 15),
                                         baseY + ((packed >> 4) & 15),
                                         chunkZ * 16 + ((packed >> 8) & 15));
                    const BlockState blockState = GetBlockState(pos.x, pos.y, pos.z);
                    const FluidState fluidState = FluidStateOf(blockState);
                    if (!fluidState.IsEmpty()) {
                        Fluids::Tick(*this, pos, blockState, fluidState);
                    }
                    if (!blockState.Is(BlockID::Water) && !blockState.Is(BlockID::Lava)) {
                        const BlockState newState = UpdateFromNeighbourShapes(*this, blockState, pos);
                        if (newState.RawId() != blockState.RawId()) {
                            SetBlock(pos.x, pos.y, pos.z, newState,
                                     UpdateFlags::Invisible | UpdateFlags::KnownShape |
                                     UpdateFlags::SkipBlockEntitySideEffects);
                        }
                    }
                }
            }
        }
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

            // Fluid ticks share this queue (see FlowingFluid.hpp). MC's
            // ServerLevel.tickFluid guard is on the cell's FLUID, not its
            // block: a `water` appointment on a waterlogged fence must fire
            // on the fence's water, and one on a cell that has since dried
            // up must not.
            if (type == BlockID::Water || type == BlockID::Lava) {
                const FluidState fluid = FluidStateOf(state);
                if (!fluid.IsSame(type == BlockID::Water ? FluidType::Water : FluidType::Lava)) return;
                Fluids::Tick(*this, pos, state, fluid);
                return;
            }

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

        // Per section per tick, as MC — but no more samples than a section
        // has blocks. MC itself has no ceiling, and a rule set to millions
        // parks the server thread in this loop for good: no chunk goes out,
        // no command comes in to lower it again. At 4096 a section's every
        // block ticks about once, which is all a bigger number could mean.
        constexpr int kMaxSamplesPerSection = Math::CHUNK_SIZE_X * Math::SECTION_HEIGHT * Math::CHUNK_SIZE_Z;
        const int tickSpeed = std::min(m_randomTickSpeed, kMaxSamplesPerSection);
        if (tickSpeed <= 0 || !m_chunkProvider) return;

        for (const Math::ChunkPos& cp : m_randomTickingChunks) {
            // Cache-only, and it MUST stay that way. The list comes
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

    // MC ServerLevel.runBlockEvents, verbatim in shape. Events in a chunk
    // that is not simulating this tick are held over, not dropped — a piston
    // armed at the edge of the loaded area fires when someone comes back.
    void World::ProcessBlockEvents() {
        PROFILE_ZONE_N("BlockEvents");
        m_blockEventsToReschedule.clear();

        while (!m_blockEvents.empty()) {
            const BlockEventData eventData = m_blockEvents.front();
            m_blockEvents.pop_front();
            if (ShouldTickBlocksAt(eventData.pos)) {
                // doBlockEvent: only if the block is still the one that booked it.
                const BlockState state = GetBlockState(eventData.pos.x, eventData.pos.y, eventData.pos.z);
                if (!state.Is(eventData.block)) continue;
                const Block& def = BlockRegistry::Get(eventData.block);
                if (!def.triggerEvent) continue;
                const bool handled = def.triggerEvent(*this, eventData.pos, state,
                                                      eventData.b0, eventData.b1);
                if (handled && Server::g_integratedServer) {
                    // MC broadcasts a ClientboundBlockEventPacket to players
                    // within 64 blocks so the client can mirror the event
                    // (piston animation, note-block sound). The engine's
                    // clients render pistons from the block-entity stream
                    // instead, but the packet is sent for the ones that will
                    // want it.
                    Network::BlockEntityActionS2CPacket pkt;
                    pkt.worldX      = eventData.pos.x;
                    pkt.worldY      = eventData.pos.y;
                    pkt.worldZ      = eventData.pos.z;
                    pkt.actionType  = static_cast<uint8_t>(eventData.b0);
                    pkt.actionParam = static_cast<uint8_t>(eventData.b1);
                    pkt.blockId     = static_cast<uint16_t>(eventData.block);
                    auto data = Network::Serialization::Serialize(pkt);
                    Server::g_integratedServer->SendToChunkWatchers(
                        GetDimension(), glm::dvec3(eventData.pos) + glm::dvec3(0.5),
                        Network::PacketId::BlockEntityActionS2C, data);
                }
            } else {
                m_blockEventsToReschedule.push_back(eventData);
            }
        }

        for (const BlockEventData& e : m_blockEventsToReschedule) m_blockEvents.push_back(e);
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

        // Only chunks that hold a block entity, gathered in one pass under the
        // cache lock — walking every loaded chunk through GetChunk cost a lock
        // per chunk (~7,000 per level with a few dozen players) to find the
        // few with anything to tick. A chunk that gains its first block entity
        // during this walk is ticked from the next tick on, as MC's
        // pendingBlockEntityTickers are.
        const auto chunks = m_chunkProvider->GetChunksWithBlockEntities();
        for (const auto& [pos, chunk] : chunks) {

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
            // Snapshot first: a ticking entity may write blocks — a piston's
            // moving cell lands its block and removes itself — and that
            // mutates the very map being walked. Each entry is re-resolved
            // before it is ticked so a removed one is skipped.
            struct Entry { glm::ivec3 local; BlockEntity* be; };
            std::vector<Entry> entries;
            for (auto& [localPos, be] : chunk->MutableBlockEntities()) {
                if (be) entries.push_back(Entry{localPos, be.get()});
            }
            bool anyDirty = false;
            for (const Entry& e : entries) {
                BlockEntity* be = chunk->GetBlockEntity(e.local.x, e.local.y, e.local.z);
                if (be != e.be) continue;
                // An entity that arrived with its chunk has no level yet.
                if (!be->GetLevel()) be->SetLevel(this);
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
        if (m_doDaylightCycle && !m_fixedDayTime) {
            m_dayTime++;
        }

        // TODO: Weather transitions (clear/rain/thunder)
    }

} // namespace Game
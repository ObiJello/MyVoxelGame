// File: src/common/world/level/World.hpp
#pragma once

#include "../chunk/IBlockAccess.hpp"
#include "../chunk/Heightmap.hpp"
#include "ILevelWrite.hpp"
#include "server/world/ChunkProvider.hpp"
#include "../block/Blocks.hpp"
#include "../math/WorldMath.hpp"
#include "server/world/tracking/DirtyTracker.hpp"
#include "common/core/JavaRandom.hpp"
#include "../ticks/LevelTicks.hpp"
#include "common/entity/EntityLevel.hpp"   // Game::Difficulty
#include <memory>
#include <atomic>
#include <cstdint>
#include <vector>
#include <unordered_set>
#include <glm/glm.hpp>

namespace Game {

    class World : public ILevelWrite {
    public:
        // Update flags for SetBlock operations (bitfield)
        enum UpdateFlags : uint32_t {
            None              = 0,
            NotifyNeighbors   = 1 << 0,  // Notify neighboring blocks of change
            UpdateShapes      = 1 << 1,  // Update block shapes (for fences, walls, etc.) - TODO
            RecomputeLight    = 1 << 2,  // Recalculate lighting - TODO
            // Vestigial. Heightmaps are now maintained unconditionally by
            // Chunk::SetBlock, which is the single funnel every write reaches.
            // That matches MC: LevelChunk.setBlockState updates its heightmaps
            // regardless of the flags handed to Level.setBlock — those govern
            // neighbour updates, lighting and client notification, never the
            // heightmap. The bit is kept so the `All` combination and existing
            // call sites keep their numeric value.
            UpdateHeightmap   = 1 << 3,
            MarkDirty         = 1 << 4,  // Mark section dirty for mesh rebuild
            NoDrops           = 1 << 6,  // Don't drop items when breaking - TODO
            
            // Common flag combinations
            All = NotifyNeighbors | UpdateShapes | RecomputeLight | UpdateHeightmap | MarkDirty,
            AllNoDrops = All | NoDrops
        };
        
        World();
        ~World();

        // MC ChunkAccess.getHeight — topmost matching block in a column, or
        // MIN_Y when the column is empty or its chunk is not loaded.
        //
        // O(1): reads the chunk's maintained heightmap. This is the call that
        // replaced every "scan down the column to find the surface" loop.
        int GetSurfaceHeight(int worldX, int worldZ, HeightmapType type) const;

        // MC Level.canSeeSky — is anything between this position and the sky?
        // Answered from the WORLD_SURFACE heightmap rather than a walk.
        bool CanSeeSky(int worldX, int worldY, int worldZ) const;

        // Core world operations
        void Initialize();
        bool InitializeChunkProvider();
        void Shutdown();

        // IBlockAccess implementation
        BlockID GetBlock(int worldX, int worldY, int worldZ) const override;
        BlockState GetBlockState(int worldX, int worldY, int worldZ) const override;

        // Section-aware collision-mask fill. See IBlockAccess for the contract.
        void FillCollisionMask(const glm::ivec3& origin, const glm::ivec3& size,
                               int shiftZ, int shiftY, uint64_t* out) const override;
        bool IsRegionAllAir(const glm::ivec3& min, const glm::ivec3& max,
                            bool absentIsAir = false) const override;
        void GetBlockStatesInBox(const glm::ivec3& min, const glm::ivec3& max,
                                 BlockState* out) const override;
        uint64_t RegionWriteStamp(const glm::ivec3& min, const glm::ivec3& max) const override;

        // Monotonic counter bumped by every accepted block write. A cached view
        // of the block field (see CollisionGrid) stamps this at build and
        // re-checks it before use, so a write that lands mid-read demotes the
        // reader to the uncached path instead of silently answering stale.
        uint64_t BlockWriteEpoch() const {
            return m_blockWriteEpoch.load(std::memory_order_acquire);
        }
        uint16_t GetBiome(int worldX, int worldY, int worldZ) const override;
        bool IsChunkLoaded(int chunkX, int chunkZ) const override;
        bool IsPositionLoaded(int worldX, int worldY, int worldZ) const override;
        bool IsBlockSolid(int worldX, int worldY, int worldZ) const override;
        bool IsBlockFluid(int worldX, int worldY, int worldZ) const override;
        bool IsValidPosition(int worldX, int worldY, int worldZ) const override;

        // World modification
        bool SetBlock(int worldX, int worldY, int worldZ, BlockID blockId);
        // ILevelWrite — the interface item behaviours write through, so the
        // same code can target the client's predicted world.
        bool SetBlock(int worldX, int worldY, int worldZ, BlockID blockId,
                      uint32_t updateFlags) override;
        // Full form carrying the block-state index (MC BlockState.getId()).
        // The two overloads above forward here with stateIndex = 0, i.e. the
        // block's default state — which is what a caller that doesn't know
        // about states means.
        bool SetBlock(int worldX, int worldY, int worldZ, BlockID blockId,
                      uint32_t updateFlags, BlockStateIndex stateIndex) override;
        // Bring the interface's `setBlock(pos, state, flags)` convenience form
        // back into scope — declaring overloads here would otherwise hide it.
        using ILevelWrite::SetBlock;

        // The server's world is the authority — MC ServerLevel.isClientSide.
        bool IsClientSide() const override { return false; }

        // Which dimension this world IS. MUST be set before Initialize() —
        // that is where the chunk provider is built, and the provider hands
        // the dimension to the terrain generator, which uses it to pick the
        // noise router, biome source and surface rules.
        //
        // It is also what the portal rules read (`ILevelWrite::GetDimension`)
        // and what scopes the block-entity broadcasts to the right watchers,
        // so a world left at the default would light nether portals in the End
        // and send Nether block entities to Overworld players.
        void SetDimension(DimensionId dimension) { m_dimension = dimension; }
        DimensionId GetDimension() const override { return m_dimension; }

        // MC BlockBehaviour.canSurvive for the block currently at this cell.
        // True for anything with no support rule, so callers can ask blindly.
        bool CanBlockSurviveAt(int worldX, int worldY, int worldZ) const;

        // Mesh system integration
        void MarkSectionDirty(int worldX, int worldY, int worldZ);
        bool HasDirtySections() const;

        // Get dirty sections for mesh rebuilding
        std::vector<DirtySection> GetDirtySections();
        void ClearDirtySections(const std::vector<DirtySection>& sections);

        // Get loaded chunk count for debugging
        size_t GetLoadedChunkCount() const;

        // World bounds (from Config)
        static constexpr int MIN_Y = -64;
        static constexpr int MAX_Y = 319;
        static constexpr int WORLD_HEIGHT = MAX_Y - MIN_Y + 1;

        // Minecraft world support
        void SetMinecraftWorldPath(const std::string& worldPath);

        // This dimension's folder inside an ObeyCraft save. Non-empty means the
        // world persists. Distinct from the Minecraft world path above, which
        // is somebody else's save and is never written.
        void SetSavePath(const std::string& savePath) { m_savePath = savePath; }
        const std::string& GetSavePath() const { return m_savePath; }
        const std::string& GetMinecraftWorldPath() const;
        bool HasMinecraftWorld() const;

        // Open the world without any ability to write it back. MUST be called
        // before Initialize() — that is where the chunk provider is built, and
        // read-only is enforced by never giving it a chunk saver.
        void SetReadOnly(bool readOnly) { m_readOnly = readOnly; }
        bool IsReadOnly() const { return m_readOnly; }

        // Provide chunk access for mesh system
        // BLOCKS on a cache miss: goes to disk, then to the generator, and
        // waits for that chunk to reach FULL. Never call from per-tick
        // simulation — use GetLoadedChunk.
        std::shared_ptr<Chunk> GetChunk(int chunkX, int chunkZ) const;

        // Cache-only: null when the chunk is not resident. Never loads,
        // generates or blocks. MC ServerChunkCache.getChunkNow.
        std::shared_ptr<Chunk> GetLoadedChunk(int chunkX, int chunkZ) const;

        // Convenience method for mesh manager
        const Chunk* GetChunkForMeshing(int chunkX, int chunkZ) const;

        // Performance and debugging
        void LogPerformanceStats();
        void SaveAllChunks();
        size_t GetMemoryUsage() const;
        ChunkProviderStats GetChunkProviderStats() const;

        // World generation control
        void SetGenerationSeed(int64_t seed);
        int64_t GetGenerationSeed() const;
        // World-creation "Generate Structures" toggle; set alongside the seed
        // (after it — see ChunkProvider::SetGenerateStructures), before
        // generation starts.
        void SetGenerateStructures(bool enabled);
        // World type + superflat/single-biome customization (same timing).
        void SetWorldGenOptions(const std::string& worldType,
                                const std::string& flatPreset,
                                const std::string& flatLayers,
                                const std::string& singleBiome);
        // World Properties sandbox tweaks JSON ("" = vanilla; same timing).
        void SetWorldGenTweaks(const std::string& tweaksJson);

        // Direct access to chunk provider for advanced use cases
        ChunkProvider* GetChunkProvider() const { return m_chunkProvider.get(); }

        // Signal the world to stop all long-running operations (called from shutdown)
        void RequestStop() {
            m_stopRequested.store(true);
            // Abort blocking getChunk() loops in the terrain library
            if (m_chunkProvider && m_chunkProvider->GetGenerator()) {
                m_chunkProvider->GetGenerator()->RequestAbort();
            }
        }
        bool IsStopRequested() const { return m_stopRequested.load(); }

        // ========================================================================
        // SERVER WORLD LOOP
        // ========================================================================

        // Main world tick function called from server thread
        // Handles simulation only (block ticks, entities, etc.)
        // Chunk loading is driven by the session system, NOT by World.
        void WorldLoop(float deltaTime);

        // Block update processing
        void ProcessBlockUpdates();

        // Random block ticks (like crop growth, ice melting, etc.)
        void PerformRandomBlockTick();

        // Process scheduled block events
        void ProcessBlockEvents();

        // Update tile entities
        void TileEntityTick();

        // Update entities
        void EntityTick();

        // Update world time and weather
        void WorldTimeWeatherTick();

        // ========================================================================
        // WORLD TIME (day/night cycle)
        // ========================================================================
        // Mirrors ServerLevel.tickTime: gameTime always advances; dayTime only
        // advances when the doDaylightCycle gamerule is on (default OFF here,
        // unlike vanilla — worlds are frozen at noon unless enabled).
        int64_t GetGameTime() const { return m_gameTime; }
        int64_t GetDayTime() const { return m_dayTime; }
        void SetDayTime(int64_t dayTime) { m_dayTime = dayTime; }
        bool GetDoDaylightCycle() const { return m_doDaylightCycle; }
        void SetDoDaylightCycle(bool enabled) { m_doDaylightCycle = enabled; }

        // MC gamerule `mobGriefing`. Gates world edits made BY mobs — today
        // just a sheep eating grass; creepers and endermen want the same flag.
        bool GetDoMobGriefing() const { return m_doMobGriefing; }
        void SetDoMobGriefing(bool enabled) { m_doMobGriefing = enabled; }

        // MC gamerule `doMobSpawning`. Gates the natural spawner only —
        // despawning, AI and /summon are all unaffected, exactly as in vanilla,
        // so turning it off empties the world gradually rather than at once.
        bool GetDoMobSpawning() const { return m_doMobSpawning; }
        void SetDoMobSpawning(bool enabled) { m_doMobSpawning = enabled; }

        // MC LevelData.getDifficulty. Was hardcoded to Normal at
        // EntityLevel::GetDifficulty, which meant Peaceful was unreachable and
        // MC's difficulty scaling on damage could not be reproduced at all.
        Difficulty GetDifficulty() const { return m_difficulty; }
        void SetDifficulty(Difficulty d) { m_difficulty = d; }

        // ── Explosion gamerules (MC GameRules.java) ─────────────────────────
        //
        // MC gamerule `tnt_explodes`. Turns every TNT prime and detonation
        // into a no-op — the block still breaks and drops, it just never lights.
        bool GetTntExplodes() const { return m_tntExplodes; }
        void SetTntExplodes(bool enabled) { m_tntExplodes = enabled; }

        // MC gamerule `entity_drops`. Gates drops that come from an ENTITY
        // rather than a block break — including a falling block that could not
        // land and pops as an item.
        bool GetDoEntityDrops() const { return m_doEntityDrops; }
        void SetDoEntityDrops(bool enabled) { m_doEntityDrops = enabled; }

        // The three drop-decay rules pick DESTROY vs DESTROY_WITH_DECAY, which
        // is what decides whether a blasted block's loot survives at 1/radius.
        //
        // NOTE THE TNT DEFAULT IS FALSE. That is vanilla and it surprises
        // people: TNT drops EVERYTHING it breaks, while creeper and generic
        // block explosions decay. Someone will eventually "fix" this to true;
        // it is not a bug.
        bool GetTntExplosionDropDecay() const { return m_tntExplosionDropDecay; }
        void SetTntExplosionDropDecay(bool v) { m_tntExplosionDropDecay = v; }

        bool GetBlockExplosionDropDecay() const { return m_blockExplosionDropDecay; }
        void SetBlockExplosionDropDecay(bool v) { m_blockExplosionDropDecay = v; }

        bool GetMobExplosionDropDecay() const { return m_mobExplosionDropDecay; }
        void SetMobExplosionDropDecay(bool v) { m_mobExplosionDropDecay = v; }

        // ========================================================================
        // RANDOM TICKING (MC ServerLevel.tickChunk)
        // ========================================================================

        // MC gamerule `random_tick_speed`: how many random positions are sampled
        // per chunk SECTION per tick. Vanilla default 3, minimum 0 (which turns
        // random ticking off entirely). Raising it is the standard way to watch
        // crops grow without waiting — /gamerule random_tick_speed 1000.
        static constexpr int kDefaultRandomTickSpeed = 3;
        int  GetRandomTickSpeed() const { return m_randomTickSpeed; }
        void SetRandomTickSpeed(int speed) {
            m_randomTickSpeed = speed < 0 ? 0 : speed;
        }

        // Which chunks are close enough to a player to simulate. Set by
        // IntegratedServer each tick from its ChunkTicketManager; empty means
        // "nothing simulates", which is the correct answer for a world with no
        // players in it. World deliberately does not reach for the ticket
        // manager itself — it is a data container and knows nothing about
        // sessions (see the WorldLoop comment).
        void SetBlockTickingChunks(std::vector<Math::ChunkPos> chunks) {
            m_blockTickingChunks = std::move(chunks);
            // The random-tick walk iterates the vector; the scheduled-tick
            // drain needs to ASK "is this one chunk simulating?" per active
            // container, so it gets a set. Rebuilt here rather than lazily
            // because the vector is replaced wholesale every tick and a stale
            // set would silently freeze or resurrect block ticks.
            m_blockTickingKeys.clear();
            m_blockTickingKeys.reserve(m_blockTickingChunks.size());
            for (const Math::ChunkPos& cp : m_blockTickingChunks) {
                m_blockTickingKeys.insert(
                    (static_cast<uint64_t>(static_cast<uint32_t>(cp.x)) << 32) |
                     static_cast<uint64_t>(static_cast<uint32_t>(cp.z)));
            }
        }

        // MC Level.getBlockRandomPos — a dedicated LCG, NOT the world RNG.
        // Exposed for testing; the tick loop is its only real caller.
        glm::ivec3 GetBlockRandomPos(int xo, int yo, int zo, int yMask);

        // MC LevelReader.getRawBrightness. See IBlockAccess for what this
        // stands in for; World keeps the base implementation.
        //
        // MC Level.isRainingAt — no weather system, so always false. Named and
        // called anyway so the farmland rule reads like FarmBlock.java and a
        // future weather system has one obvious place to plug in.
        bool IsRainingAt(int /*worldX*/, int /*worldY*/, int /*worldZ*/) const { return false; }

        // ========================================================================
        // SCHEDULED BLOCK TICKS (MC ServerLevel.blockTicks)
        // ========================================================================
        //
        // The delayed-tick system behind falling blocks, and eventually behind
        // fluids and redstone. Distinct from random ticking: a random tick is a
        // probability, a scheduled tick is an appointment.
        //
        // The queues live on the chunks (Chunk::BlockTicks); this is the index
        // that merges them. Wired up in Initialize(), because the resolver has
        // to reach the chunk provider.
        ScheduledTickAccess* Ticks() override { return &m_blockTicks; }
        LevelTicks&          BlockTicks()     { return m_blockTicks; }

        // MC ServerLevel's per-tick cap on scheduled ticks (65536). Far above
        // anything a real world produces; it exists so a runaway feedback loop
        // degrades into lag instead of hanging the tick thread.
        static constexpr int kMaxBlockTicksPerTick = 65536;

        // MC Level.updateNeighborsAt. Public because the wither ritual mirrors
        // CarvedPumpkinBlock.updatePatternBlocks: the pattern cells are
        // cleared WITHOUT neighbour updates (MC flag 2), the boss is spawned,
        // and only then does each cleared cell notify its neighbours.
        void NotifyNeighborBlocks(int worldX, int worldY, int worldZ);

    private:
        std::unique_ptr<ChunkProvider> m_chunkProvider;
        std::string m_minecraftWorldPath;
        bool        m_readOnly = false;   // see SetReadOnly
        DimensionId m_dimension = DimensionId::Overworld;
        std::string m_savePath;   // see SetSavePath

        // Helper functions
        void OnBlockChanged(int worldX, int worldY, int worldZ);
        Math::ChunkPos WorldToChunkPos(int worldX, int worldZ) const;
        void MarkNeighboringSectionsIfNeeded(int worldX, int worldY, int worldZ);

        // Statistics
        mutable size_t m_blockAccessCount = 0;

        // Stop flag for early termination of long-running loops
        std::atomic<bool> m_stopRequested{false};

        // World time. dayTime defaults to 6000 (noon) so frozen worlds match
        // the pre-time-system look (MC new worlds start at 0/sunrise, but our
        // doDaylightCycle default is false so noon is the better freeze point).
        int64_t m_gameTime = 0;
        int64_t m_dayTime = 6000;
        bool m_doDaylightCycle = false;
        // Vanilla defaults, unlike doDaylightCycle above.
        bool m_doMobSpawning = true;
        bool m_doMobGriefing = true;
        // MC's default for a fresh world.
        Difficulty m_difficulty = Difficulty::Normal;
        // Explosion rules — see the accessors above. Every default is MC's,
        // including tntExplosionDropDecay being the odd one out at false.
        bool m_tntExplodes             = true;
        bool m_doEntityDrops           = true;
        bool m_tntExplosionDropDecay   = false;
        bool m_blockExplosionDropDecay = true;
        // See BlockWriteEpoch(). Atomic because SetBlock is documented as
        // callable from the server thread OR a worker; two racing plain
        // increments could advance the counter by one instead of two and let a
        // two-writes-stale snapshot compare equal to the current value.
        std::atomic<uint64_t> m_blockWriteEpoch{1};
        bool m_mobExplosionDropDecay   = true;

        // ── Random ticking ──────────────────────────────────────────────────
        int m_randomTickSpeed = kDefaultRandomTickSpeed;

        // Refreshed by the server every tick. Kept as a plain vector the server
        // moves in, rather than a set World queries, so the tick loop is a
        // straight walk with no hashing.
        std::vector<Math::ChunkPos> m_blockTickingChunks;
        // Lookup form of the above — see SetBlockTickingChunks.
        std::unordered_set<uint64_t> m_blockTickingKeys;

        // MC Level.randValue — the state of getBlockRandomPos's own LCG. It is
        // deliberately NOT the world RNG and deliberately not seeded: vanilla
        // leaves it at 0 and lets it walk, and the sequence is what spreads
        // sampled positions evenly through a section.
        int32_t m_randValue = 0;

        // ── Scheduled ticks ─────────────────────────────────────────────────
        // See Ticks() above. Resolver + tick-check are installed in Initialize.
        LevelTicks m_blockTicks;

        // Reused across every dispatch so a random tick allocates nothing. The
        // seed is irrelevant to correctness — MC passes the level's shared
        // RandomSource here too — but a fixed one makes a session reproducible
        // when debugging a growth rule.
        JavaRandom m_tickRandom{0};
    };

} // namespace Game
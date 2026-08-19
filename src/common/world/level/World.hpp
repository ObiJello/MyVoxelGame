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
#include <memory>
#include <atomic>
#include <cstdint>
#include <vector>
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

    private:
        std::unique_ptr<ChunkProvider> m_chunkProvider;
        std::string m_minecraftWorldPath;
        bool        m_readOnly = false;   // see SetReadOnly

        // Helper functions
        void OnBlockChanged(int worldX, int worldY, int worldZ);
        void NotifyNeighborBlocks(int worldX, int worldY, int worldZ);
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

        // ── Random ticking ──────────────────────────────────────────────────
        int m_randomTickSpeed = kDefaultRandomTickSpeed;

        // Refreshed by the server every tick. Kept as a plain vector the server
        // moves in, rather than a set World queries, so the tick loop is a
        // straight walk with no hashing.
        std::vector<Math::ChunkPos> m_blockTickingChunks;

        // MC Level.randValue — the state of getBlockRandomPos's own LCG. It is
        // deliberately NOT the world RNG and deliberately not seeded: vanilla
        // leaves it at 0 and lets it walk, and the sequence is what spreads
        // sampled positions evenly through a section.
        int32_t m_randValue = 0;

        // Reused across every dispatch so a random tick allocates nothing. The
        // seed is irrelevant to correctness — MC passes the level's shared
        // RandomSource here too — but a fixed one makes a session reproducible
        // when debugging a growth rule.
        JavaRandom m_tickRandom{0};
    };

} // namespace Game
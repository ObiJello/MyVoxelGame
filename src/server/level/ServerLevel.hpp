// File: src/server/level/ServerLevel.hpp
//
// One dimension's worth of server state — MC's `ServerLevel`.
//
// WHY THIS TYPE EXISTS
// --------------------
// Until portals, this engine had exactly one world and every world-scoped
// system was a single member of IntegratedServer: one ChunkTicketManager, one
// MobManager, one SectionChangeAccumulator, and so on. Every one of them is
// keyed by `ChunkPos` or by entity id, neither of which carries a dimension —
// so a second world sharing them would not merely be untidy, it would apply a
// Nether block edit to the Overworld chunk at the same x/z and hand a Nether
// mob's id to a player standing in the Overworld.
//
// So the bundle is the fix: everything keyed by a position or an entity gets
// one instance PER DIMENSION, and IntegratedServer holds an array of these
// instead of a field each.
//
// WHAT IS DELIBERATELY *NOT* HERE
// -------------------------------
// `PlayerSessionManager`, `SendScheduler`, `NetworkServer` and the tick-rate
// manager stay global. A player exists once regardless of where they are
// standing, connections are not per-world, and the send budget is a property
// of the socket. `ServerWorkerPool` is global too; its jobs carry a
// DimensionId instead.
//
// The block, item, blockstate, loot and terrain-library registries are all
// process-wide immutable tables — they are shared, and the terrain library's
// bootstraps are idempotent and seed-independent, which is what makes three
// generators safe.
//
// LIFETIME
// --------
// The Overworld is constructed eagerly at server start. The Nether and the
// End are constructed the first time something needs them, because each one
// costs a `MyTerrainGenerator` (noise router, random state, chunk cache) and
// most sessions never visit either. Once created, a level lives until
// shutdown — MC keeps its levels for the server's lifetime too, and tearing
// one down while a chunk job for it is in flight is not a problem worth
// having.
#pragma once
#include <deque>

#include "server/level/LevelEntityStore.hpp"

#include "NetherPortalIndex.hpp"
#include "common/entity/ai/village/PoiManager.hpp"
#include "common/world/portal/PortalFamily.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/World.hpp"
#include "common/world/math/WorldMath.hpp"

#include <array>
#include <memory>
#include <string>
#include <unordered_set>
#include <chrono>
#include <glm/glm.hpp>

namespace Game {
    class MyTerrainGenerator;
}

namespace Server {

    class ChunkTicketManager;
    class ChunkStatusManager;
    class SectionChangeAccumulator;
    class ChunkDeltaBroadcaster;
    class ItemEntityManager;
    class ExperienceOrbManager;
    class ServerLevelBridge;
    class MobManager;
    class EndDragonFight;
    class SilentWardenBossBars;
    class HushStillness;
    class AurelithCities;
    class ChunkKeeper;
    class FallingBlockStore;
    class ServerEntityTracker;
    class PlayerSessionManager;
    class IntegratedServer;

    // How this level is built. Everything the constructor cannot derive from
    // the DimensionId alone.
    struct ServerLevelConfig {
        Game::DimensionId dimension = Game::DimensionId::Overworld;

        // Anvil root for THIS dimension. Empty means "generate only, never
        // persist", which is the state procedurally-created worlds are in
        // today for every dimension including the overworld.
        //
        // For a world that does have a root, MC's layout puts the overworld at
        // the root and the others in a sub-folder — see DimensionSaveSubdir.
        std::string worldPath;

        bool    readOnly       = false;

        // The ObeyCraft save's world root — the same for every dimension.
        // Anvil::SaveRoot::Dimension(dim) adds DIM-1 / DIM1 where applicable;
        // nothing else may. Empty = do not persist.
        std::string savePath;
        int64_t seed           = 0;
        bool    generateStructures = true;

        // World-type customization. Overworld only — the Nether and the End
        // have no presets in vanilla, and passing "amplified" to a nether
        // generator would silently select the wrong noise router.
        std::string worldType     = "default";
        std::string flatPreset;
        std::string flatLayers;
        std::string singleBiome;
        std::string worldgenTweaks;

        // MC ServerChunkCache sizing. The overworld is where players spend
        // their time; a nether cache the same size is ~5000 chunks of resident
        // memory for a dimension somebody visits for two minutes.
        size_t maxLoadedChunks = 5120;
    };

    class ServerLevel {
    public:
        ServerLevel(const ServerLevelConfig& config, PlayerSessionManager* sessions,
                    IntegratedServer* server);
        ~ServerLevel();

        ServerLevel(const ServerLevel&) = delete;
        ServerLevel& operator=(const ServerLevel&) = delete;

        // Builds the chunk provider (and with it the terrain generator).
        //
        // MUST run on the server thread: ServerChunkCache captures
        // `this_thread::get_id()` at construction and takes a different code
        // path off it, so a level whose provider was built on the main thread
        // deadlocks the first time a worker asks it for a chunk.
        bool InitializeChunkProvider();

        Game::DimensionId Dimension() const { return m_config.dimension; }
        const ServerLevelConfig& Config() const { return m_config; }

        Game::World*              World()       const { return m_world.get(); }
        ChunkTicketManager*       Tickets()     const { return m_tickets.get(); }
        ChunkStatusManager*       Status()      const { return m_status.get(); }
        SectionChangeAccumulator* Changes()     const { return m_changes.get(); }
        ChunkDeltaBroadcaster*    Deltas()      const { return m_deltas.get(); }
        ItemEntityManager*        Items()       const { return m_items.get(); }
        ExperienceOrbManager*     Orbs()        const { return m_orbs.get(); }
        ServerLevelBridge*        MobLevel()    const { return m_mobLevel.get(); }
        MobManager*               Mobs()        const { return m_mobs.get(); }
        // The compact mass-falling-block rows — see FallingBlockStore.
        FallingBlockStore*        FallingBlocks() const { return m_fallingBlocks.get(); }
        // Entity persistence for this dimension. Null when the world does not
        // persist (imported read-only, or seed-only).
        LevelEntityStore*         Entities()    const { return m_entityStore.get(); }
        ServerEntityTracker*      MobTracker()  const { return m_mobTracker.get(); }
        // MC ServerLevel.dragonFight — non-null only for the End.
        EndDragonFight*           DragonFightController() const { return m_dragonFight.get(); }
        // The Silent Warden's boss bar (The Hush) — every level, since the
        // boss can be summoned anywhere. Null only during teardown.
        SilentWardenBossBars*     WardenBossBars() const { return m_wardenBossBars.get(); }
        // The Hush's stillness (HushStillness.hpp) — non-null only for the
        // Hush.
        HushStillness*            Stillness() const { return m_stillness.get(); }
        // Aurelith's cities and the quest to reawaken their Hearts
        // (AurelithCities.hpp) — non-null only for the Hush.
        AurelithCities*           Aurelith() const { return m_aurelith.get(); }
        // Force-loaded chunks and the redstone index (ChunkKeeper.hpp).
        ChunkKeeper*              Keeper()      const { return m_keeper.get(); }

        // The terrain generator behind the chunk provider, or null before the
        // provider is built. Server thread only.
        Game::MyTerrainGenerator* TerrainGenerator() const;

        // MC ServerChunkCache.getChunk(x, z, ChunkStatus.FULL, load = true).
        //
        // The chunk at FULL, loaded from disk or generated as needed,
        // BLOCKING the server thread until it exists. Null only when the
        // level has no world, generation failed, or the generator is
        // aborting (shutdown). The blocking is the terrain library's own
        // managedBlock port (ServerChunkCache::getChunk pumps the distance
        // manager and the main-thread task queue while it waits), reached
        // through World::GetChunk — the same call PortalTravel makes for a
        // portal's far side.
        //
        // MUST run on the server thread: the provider's ServerChunkCache
        // captured that thread's id when it was built (InitializeChunkProvider)
        // and only on it takes the self-pumping branch; anywhere else it
        // waits on a thread that may itself be waiting.
        //
        // Also places a temporary entity-ticking ticket at `pos` for
        // `ticketTicks` — MC's PLAYER_SPAWN / POST_TELEPORT ticket — so the
        // caller's use of the chunk is not raced by an unload.
        //
        // A cold chunk costs a full generation (tens of ms). One-shot,
        // player-visible decisions only — a respawn, a portal exit — never
        // per-tick simulation, which uses World::GetLoadedChunk.
        std::shared_ptr<Game::Chunk> GetChunkBlocking(Game::Math::ChunkPos pos,
                                                      int ticketTicks = 300);

        // Chunk loads in flight for THIS dimension. Dimension-scoped because
        // Overworld (0,0) and Nether (0,0) are the same ChunkPos and sharing
        // one set would make the second request look like a duplicate of the
        // first and silently never load.
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> pendingChunkLoads;
        // Redstone warm-up (redstone_plus): while the keeper's chunks are still
        // arriving, nothing redstone is evaluated — no border settles, no
        // deferred re-checks, no block ticks. A machine that spans dozens of
        // chunks would otherwise evaluate its gates against chunks not yet
        // in (dust nets cut at the border read as off), and instant torches
        // and latches REMEMBER such a transient: the Snake machine's clock
        // gate opened during loading and the snake died before the player
        // touched anything. Chunks that arrived during the warm-up are
        // settled together once every kept chunk is resident.
        bool redstoneWarmup = false;
        bool redstoneWarmupDone = false;     // once per level: later loads settle at arrival as before
        std::vector<Game::Math::ChunkPos> redstoneSettleLater;
        // The unload sweep, spread over ticks (UnloadUnwatchedChunks): the
        // snapshot being scanned a slice per tick, and the chunks found past
        // MC's reach (every loader's radius + 13), unloaded in the tick's
        // spare time (MC ChunkMap.unloadQueue). `unloadQueued` keeps a chunk
        // from being queued twice while it waits.
        std::vector<Game::Math::ChunkPos> unloadScan;
        size_t unloadScanCursor = 0;
        std::deque<Game::Math::ChunkPos> unloadQueue;
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> unloadQueued;
        // Ticket-driven generation: chunks the disk did not have, waiting to be
        // handed to the terrain library (IntegratedServer::ServiceGenerationQueues).
        std::vector<Game::Math::ChunkPos> generationBacklog;
        // The backlog is kept nearest-first (to the nearest of
        // generationBacklogAnchors: the player and portal-far-side centres in
        // THIS level) and consumed from the back; it is re-sorted only when
        // entries arrive or an anchor moves, never rescanned per tick — with a
        // wide simulation ring it holds tens of thousands of entries.
        bool generationBacklogSorted = false;
        int64_t generationBacklogSortTick = -1;   // m_currentServerTick of the last sort
        std::chrono::steady_clock::time_point generationLastStallCheck{};   // watchdog runs ~1/s
        std::vector<Game::Math::ChunkPos> generationBacklogAnchors;
        size_t generationInFlight = 0;
        // When each in-flight request was handed to the library, and when the
        // library last completed anything: the stall watchdog reads both.
        std::unordered_map<Game::Math::ChunkPos, std::chrono::steady_clock::time_point,
                           Game::Math::ChunkPosHash> generationIssued;
        std::chrono::steady_clock::time_point generationLastCompletion{};
        // Requests the watchdog gave up on, with the time they may be tried
        // again: a wedged holder wedges again at once, and re-picking it
        // (it is the nearest) would starve every other request.
        std::unordered_map<Game::Math::ChunkPos, std::chrono::steady_clock::time_point,
                           Game::Math::ChunkPosHash> generationQuarantine;
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> failedChunkLoads;
        // Pending chunks taken as the library announced them ready (MC
        // onChunkReadyToSend) rather than through their own request: their
        // backlog entries must not issue one. Pruned as they surface.
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> generationDelivered;
        // Pending chunks the disk did NOT have: handed to the generator
        // (TakeRequests) and waiting for generation. Only these may be taken
        // from a ready announcement — a chunk the game is loading from its own
        // save must never be replaced by the library's copy (the library reads
        // saved FULL chunks back as neighbours of new ones, and its copy is not
        // the save). Left when the load result lands or the load is cancelled.
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> generationWaiting;
        // Pipeline counters for the server-state log line: requests the
        // library refused outright, completions received (and how many came
        // back empty), failed loads re-requested, and requests the watchdog
        // gave up on and reissued.
        uint64_t genRefused = 0, genCompleted = 0, genCompletedEmpty = 0, genRetried = 0, genStuck = 0;
        uint64_t genDeliveredReady = 0;   // taken through the ready announcement

        // Where this dimension's portal blocks are, one index per portal
        // family — MC's PoiManager, cut down to the one query PortalForcer
        // asks. Per level for the same reason as everything else here: the
        // index is keyed by block position, and an Overworld portal at
        // (0,64,0) and a Nether portal at (0,64,0) are different portals.
        // Per family because a hush_portal is never a nether portal's exit.
        NetherPortalIndex&       Portals(Game::PortalFamilyId family)       { return m_portals[static_cast<size_t>(family)]; }
        const NetherPortalIndex& Portals(Game::PortalFamilyId family) const { return m_portals[static_cast<size_t>(family)]; }

        // MC ServerLevel.getPoiManager — villages' job sites, beds and bells.
        // Rebuilt from blocks (see PoiManager.hpp): fed by the chunk-ready
        // scan and World::SetBlock; lives as long as the level.
        Game::PoiManager&       Poi()       { return m_poi; }
        const Game::PoiManager& Poi() const { return m_poi; }

        // Block-centred feet position players arrive at with no other target.
        // Only the overworld's is computed from the generator; the others are
        // set by whatever placed the player (a portal exit, the End platform).
        glm::vec3 worldSpawn{0.5f, 67.0f, 0.5f};

        // MC ServerLevel.getSeaLevel — per-dimension, and read by the natural
        // spawner. Overworld 63, Nether 32, End 0.
        int SeaLevel() const;

        // Is anything keeping this level simulating? A level with no players
        // and no forced chunks still exists but does no work, which is what
        // stops an unvisited Nether costing a tick's budget forever.
        bool HasWork(const PlayerSessionManager& sessions) const;

    private:
        ServerLevelConfig     m_config;
        PlayerSessionManager* m_sessions = nullptr;
        IntegratedServer*     m_server   = nullptr;

        std::unique_ptr<Game::World>              m_world;
        std::unique_ptr<ChunkTicketManager>       m_tickets;
        std::unique_ptr<ChunkStatusManager>       m_status;
        std::unique_ptr<SectionChangeAccumulator> m_changes;
        std::unique_ptr<ChunkDeltaBroadcaster>    m_deltas;
        std::unique_ptr<ItemEntityManager>        m_items;
        std::unique_ptr<ExperienceOrbManager>     m_orbs;
        std::unique_ptr<ServerLevelBridge>        m_mobLevel;
        std::unique_ptr<MobManager>               m_mobs;
        std::unique_ptr<FallingBlockStore>        m_fallingBlocks;
        std::unique_ptr<LevelEntityStore> m_entityStore;
        std::unique_ptr<ServerEntityTracker>      m_mobTracker;
        // End only (MC ServerLevel.dragonFight).
        std::unique_ptr<EndDragonFight>           m_dragonFight;
        std::unique_ptr<SilentWardenBossBars>     m_wardenBossBars;
        // Hush only.
        std::unique_ptr<HushStillness>            m_stillness;
        std::unique_ptr<AurelithCities>           m_aurelith;
        // shared: the chunk cache's save observer holds a weak reference, and
        // the cache (inside m_world) outlives this member.
        std::shared_ptr<ChunkKeeper>              m_keeper;

        // By value: each is a pair of hash sets with no dependencies, and
        // they must survive every chunk in the level unloading (see the
        // header). Indexed by PortalFamilyId; built from the family table so
        // the block each one tracks cannot drift from the family's.
        std::array<NetherPortalIndex, Game::kPortalFamilyCount> m_portals{{
            NetherPortalIndex{ Game::Family(Game::PortalFamilyId::Nether).portalBlock },
            NetherPortalIndex{ Game::Family(Game::PortalFamilyId::Hush).portalBlock },
            NetherPortalIndex{ Game::Family(Game::PortalFamilyId::Aether).portalBlock },
        }};
        Game::PoiManager m_poi;
    };

} // namespace Server

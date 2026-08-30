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

#include "server/level/LevelEntityStore.hpp"

#include "NetherPortalIndex.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/World.hpp"
#include "common/world/math/WorldMath.hpp"

#include <memory>
#include <string>
#include <unordered_set>
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

        // This dimension's folder inside an ObeyCraft save (already including
        // DIM-1 / DIM1 where applicable). Empty = do not persist.
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

        // The terrain generator behind the chunk provider, or null before the
        // provider is built. Server thread only.
        Game::MyTerrainGenerator* TerrainGenerator() const;

        // Chunk loads in flight for THIS dimension. Dimension-scoped because
        // Overworld (0,0) and Nether (0,0) are the same ChunkPos and sharing
        // one set would make the second request look like a duplicate of the
        // first and silently never load.
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> pendingChunkLoads;
        // Ticket-driven generation: chunks the disk did not have, waiting to be
        // handed to the terrain library (IntegratedServer::ServiceGenerationQueues).
        std::vector<Game::Math::ChunkPos> generationBacklog;
        size_t generationInFlight = 0;
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> failedChunkLoads;

        // Where this dimension's nether portals are — MC's PoiManager, cut
        // down to the one query PortalForcer asks. Per level for the same
        // reason as everything else here: the index is keyed by block
        // position, and an Overworld portal at (0,64,0) and a Nether portal at
        // (0,64,0) are different portals.
        NetherPortalIndex&       Portals()       { return m_portals; }
        const NetherPortalIndex& Portals() const { return m_portals; }

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

        // By value: it is a pair of hash sets with no dependencies, and it
        // must survive every chunk in the level unloading (see its header).
        NetherPortalIndex m_portals;
    };

} // namespace Server

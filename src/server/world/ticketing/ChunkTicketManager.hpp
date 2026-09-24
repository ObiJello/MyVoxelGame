// File: src/server/world/ticketing/ChunkTicketManager.hpp
//
// MC net.minecraft.server.level.DistanceManager.
//
// ── The invariant this file exists to hold ──────────────────────────────────
//
// A chunk's level is DERIVED. Tickets are the only sources; every other
// chunk's level is a solve over them. Nothing outside this class can place,
// move, or centre a region — there is no region, only sources and a solve.
//
// That is not a stylistic preference, it is the fix for a specific bug. The
// previous design had callers enumerate a (2r+1)^2 square of individually-added
// tickets centred on a chunk THEY chose. PlayerSessionManager::OnPlayerJoin
// centred one on the world spawn; PlayerSession::AttachPlayer then moved the
// player to the chunk restored from their save; and the routine that would have
// relocated the square only ran when an anchor field changed during Tick(),
// which AttachPlayer had already pre-answered by writing the same field. The
// square stayed at spawn for the whole session and NOTHING within simulation
// distance of the player ticked: mobs frozen, hurtTime never decrementing so a
// mob could be hit exactly once, sand never falling, primed TNT stuck at a
// fixed fuse forever.
//
// With levels derived, "the region is centred on the wrong chunk" stops being
// a sentence anyone can write. A player contributes exactly one ticket at the
// chunk they are standing in (MC DistanceManager.addPlayer:111-117), and moving
// them is remove-then-add of that one ticket.
//
// ── Propagation ────────────────────────────────────────────────────────────
//
// MC's own solver: ChunkTracker over DynamicGraphMinFixedPoint (the terrain
// library's port, shared rather than duplicated), exactly as MC's
// LoadingChunkTracker / SimulationChunkTracker use it. A ticket change only
// queues its chunk (MC TicketStorage -> ChunkTracker.update); RunAllUpdates
// drains the queue once per tick, touching only the chunks whose level
// actually moves. The fixed point is
//
//     level(c) = min over tickets t of ( t.level + chebyshev(t.chunk, c) )
//
// This replaced a from-scratch flood over every ticket, which a level query
// forced after every ticket change: with 50 players flying, each chunk border
// crossed re-flooded ~90,000 chunks (a player ticket reaches MAX_LEVEL, 18
// chunks out at simulation distance 8), several times a tick — 29 ms of
// session ticking in the stress test (2026-09-23).
//
// ── When levels change ─────────────────────────────────────────────────────
//
// Only in RunAllUpdates (MC DistanceManager.runAllUpdates, run from
// ServerChunkCache.tick). Every query reads the levels as of the last call;
// a ticket added since is not visible until the next one. The server calls it
// for every level once a tick, after the sessions have moved their tickets.
#pragma once

#include "common/world/math/WorldMath.hpp"
#include "server/world/ticketing/ChunkLevel.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Server {

    // MC TicketType. The level a ticket carries is supplied by whoever creates
    // it; the type exists so tickets can be found and removed again, and so
    // expiry can be per-kind.
    enum class TicketType {
        PlayerSimulation,   // MC PLAYER_SIMULATION — one per occupied chunk
        PlayerSpawn,        // MC PLAYER_SPAWN — self-expiring pre-load at the join position
        Spawn,              // world spawn chunks
        Portal,
        Forced,             // command / API
        Temporary,          // short-lived, for chunk operations
        Unknown,
    };

    class ChunkTicketManager {
    public:
        // Re-exported so existing callers and log lines keep compiling against
        // one spelling of the thresholds. ChunkLevel.hpp is the definition.
        static constexpr int ENTITY_TICKING_LEVEL = ChunkLevel::ENTITY_TICKING;
        static constexpr int BLOCK_TICKING_LEVEL  = ChunkLevel::BLOCK_TICKING;
        static constexpr int FULL_LEVEL           = ChunkLevel::FULL;
        static constexpr int MAX_LEVEL            = ChunkLevel::MAX_LEVEL;

        ChunkTicketManager();
        ~ChunkTicketManager();

        // ── Players (MC DistanceManager.addPlayer / removePlayer) ───────────
        //
        // ONE ticket at the chunk the player occupies, refcounted by how many
        // players occupy it. `chunk` is always read live from the player's
        // position by the caller — never from a cached anchor.
        void AddPlayer(Game::Math::ChunkPos chunk, uint32_t playerId);
        void RemovePlayer(Game::Math::ChunkPos chunk, uint32_t playerId);

        // Drop this player's ticket wherever it currently is. Used by the
        // dimension-change teardown, which does not know (and must not have to
        // know) which chunk the player was last registered at.
        void RemoveAllPlayerTickets(uint32_t playerId);

        // MC DistanceManager.updateSimulationDistance — re-levels every
        // PLAYER_SIMULATION ticket in place.
        void SetSimulationDistance(int distance);
        int  GetSimulationDistance() const { return m_simulationDistance; }

        // ── Other ticket sources ────────────────────────────────────────────
        void AddSpawnTickets(Game::Math::ChunkPos spawnChunk, int radius);
        void AddForcedTicket(const std::string& identifier, Game::Math::ChunkPos chunk, int level);
        void RemoveForcedTicket(const std::string& identifier, Game::Math::ChunkPos chunk);
        void AddTemporaryTicket(Game::Math::ChunkPos chunk, int level, int lifespanTicks);

        // MC PLAYER_SPAWN: a short self-expiring LOADING ticket placed at the
        // position a joining player will occupy, so their terrain is on its way
        // before the entity exists. Deliberately a different type from
        // PlayerSimulation — it must not make anything tick, and it must go
        // away on its own if the join never completes.
        void AddPlayerSpawnTicket(Game::Math::ChunkPos chunk, int radius, int lifespanTicks);

        // ── Level queries (the only way to ask about a chunk) ───────────────
        //
        // MC DistanceManager.runAllUpdates: drain pending level changes at ONE
        // defined point in the tick. Everything after it can read levels
        // without taking the mutex, because the only writers of m_levels are
        // this call and Clear() — a ticket mutation merely queues its chunk.
        //
        // Also checks, whenever levels moved, that every player stands in
        // their own entity-ticking range (see the .cpp).
        void RunAllUpdates();

        // Lock-free entity-ticking test, asked once PER ENTITY PER TICK (at a
        // million entities a mutex here was 37 ns/entity). Returns the level
        // as of the last RunAllUpdates: a ticket changed afterwards reads one
        // tick stale, which is exactly MC's semantics — levels move at defined
        // points, not continuously. Never torn, because nothing structurally
        // modifies m_levels outside RunAllUpdates.
        bool IsEntityTickingAfterUpdates(Game::Math::ChunkPos chunk) const;

        // The same answers under the mutex, for callers off the tick's
        // defined points. Also as of the last RunAllUpdates — no query ever
        // propagates (MC reads the trackers' current levels the same way).
        int  GetChunkLevel(Game::Math::ChunkPos chunk) const;
        bool IsEntityTicking(Game::Math::ChunkPos chunk) const;
        bool IsBlockTicking(Game::Math::ChunkPos chunk) const;
        bool ShouldChunkBeLoaded(Game::Math::ChunkPos chunk) const;

        // Kept under their old names — several callers read as questions about
        // simulation rather than about levels, and that reads better at the
        // call site.
        bool ShouldChunkTickBlocks(Game::Math::ChunkPos chunk) const { return IsBlockTicking(chunk); }
        bool ShouldChunkTickEntities(Game::Math::ChunkPos chunk) const { return IsEntityTicking(chunk); }

        // Iterable forms. MC has these too — ServerChunkCache.tickChunks walks
        // a LIST of ticking chunks for random ticks — but the per-entity gate
        // does NOT use them: ServerLevel.tick re-reads inEntityTickingRange for
        // each entity from its own live chunk position every tick. Anything
        // gating a single entity should call IsEntityTicking, not search these.
        std::vector<Game::Math::ChunkPos> GetLoadedChunks() const;
        // Cached: the list is rebuilt only when the levels change (see
        // LevelsVersion), since with a large simulation distance it holds
        // tens of thousands of chunks and it is asked for every tick.
        std::vector<Game::Math::ChunkPos> GetBlockTickingChunks() const;
        // The block-ticking chunks that also RANDOM-tick: within vanilla's
        // largest simulation distance (32 chunks, plus the one-chunk block-
        // ticking margin) of a player, or reached by a non-player ticket.
        // Below a simulation distance of 33 this IS the block-ticking list,
        // exactly MC; beyond it — a range MC cannot express — the outer ring
        // keeps scheduled ticks (redstone, fluids, falling sand) and drops
        // random ticks, which is what makes a 128-chunk ring affordable
        // (measured: 66k chunks of random-ticking grass alone were 100 ms a
        // tick).
        std::vector<Game::Math::ChunkPos> GetRandomTickingChunks() const;
        // Bumps every time a RunAllUpdates changed any level. A caller holding
        // a copy of a list can skip its own rebuild while unchanged.
        uint64_t LevelsVersion() const;
        std::vector<Game::Math::ChunkPos> GetEntityTickingChunks() const;

        // ── Maintenance ─────────────────────────────────────────────────────
        void ProcessExpiredTickets(int64_t currentTick);
        void Clear();

        struct Stats {
            size_t totalTickets;
            size_t playerTickets;
            size_t loadedChunks;
            size_t blockTickingChunks;
            size_t entityTickingChunks;
        };
        Stats GetStats() const;

    private:
        struct Ticket {
            TicketType type;
            int        level;
            int64_t    createdTick;
            int        lifespan;      // -1 = permanent
            std::string identifier;   // forced tickets only; empty otherwise
            int        refCount;      // MC dedupes (type, level); we refcount instead

            bool IsExpired(int64_t now) const {
                return lifespan > 0 && (now - createdTick) >= lifespan;
            }
        };

        // The ONLY state. Everything else is derived from this.
        std::unordered_map<Game::Math::ChunkPos, std::vector<Ticket>,
                           Game::Math::ChunkPosHash> m_tickets;

        // MC playersPerChunk — who is standing where, so the shared
        // PlayerSimulation ticket is refcounted correctly.
        std::unordered_map<Game::Math::ChunkPos, std::unordered_set<uint32_t>,
                           Game::Math::ChunkPosHash> m_playersPerChunk;
        // Where each player's ticket currently sits. This is the engine's
        // equivalent of MC ServerPlayer.lastSectionPos, and like MC's it means
        // "where my registration is", NOT "where the player is". It is written
        // only by AddPlayer/RemovePlayer, in the same statement block as the
        // ticket mutation, so it cannot drift from the ticket it describes.
        std::unordered_map<uint32_t, Game::Math::ChunkPos> m_playerTicketChunk;

        // Derived: every chunk at a level <= MAX_LEVEL. Written only by the
        // tracker, inside RunAllUpdates.
        std::unordered_map<Game::Math::ChunkPos, int, Game::Math::ChunkPosHash> m_levels;
        // MC's ChunkTracker over m_tickets and m_levels (defined in the .cpp,
        // which is the only place that needs the library's headers).
        class LevelTracker;
        std::unique_ptr<LevelTracker> m_tracker;
        bool     m_levelsChanged = false;                 // set by the tracker
        uint64_t m_levelsVersion = 0;                     // bumped per RunAllUpdates that changed levels
        mutable uint64_t m_cacheVersion = ~uint64_t{0};   // version the caches were built from
        mutable std::vector<Game::Math::ChunkPos> m_cachedBlockTicking;
        mutable std::vector<Game::Math::ChunkPos> m_cachedRandomTicking;

        mutable std::mutex m_mutex;
        int64_t m_currentTick = 0;
        int     m_simulationDistance = 10;   // MC's default

        // Lock-free internals — callers already hold m_mutex.
        void AddTicketLocked(Game::Math::ChunkPos chunk, TicketType type, int level,
                             int lifespan, const std::string& identifier);
        void RemoveTicketLocked(Game::Math::ChunkPos chunk, TicketType type,
                                const std::string& identifier);
        // MC TicketStorage.getTicketLevelAt: the lowest level among a chunk's
        // tickets, UNLOADED without any.
        int  TicketLevelAtLocked(Game::Math::ChunkPos chunk) const;
        // MC TicketStorage's listener call after a chunk's tickets changed:
        // queue the chunk for the tracker when its source level moved.
        void OnTicketsChangedLocked(Game::Math::ChunkPos chunk, int oldSourceLevel);
        int  GetChunkLevelLocked(Game::Math::ChunkPos chunk) const;
        std::vector<Game::Math::ChunkPos> CollectAtMostLocked(int maxLevel) const;
        void RefreshTickingCachesLocked() const;
    };

} // namespace Server

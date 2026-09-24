// File: src/server/level/LevelEntityStore.hpp
//
// Entity persistence for ONE dimension.
//
// Per level, not per server, for the same reason pendingChunkLoads is: chunk
// (0,0) in the Overworld and chunk (0,0) in the Nether are the same ChunkPos
// and must not share state.
//
// The state machine exists to stop the one failure mode that matters. Between
// asking for a chunk's entities and receiving them, the managers are NOT
// authoritative for that chunk — writing it in that window would save an empty
// list over a herd of cows. Nothing may be written for a chunk that is Absent
// or Pending.
#pragma once

#include "common/world/math/WorldMath.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Game {
    namespace Anvil { struct EntityChunkContents; }
    class Mob;
    struct EntityLevel;
    enum class EntityTypeId : uint16_t;
}

namespace Server {

    class ServerLevel;
    class EntityIoWorker;   // LevelEntityStore.cpp — MC IOWorker for entities/*.mca

    // Defined in IntegratedServer.cpp beside the natural spawner and /summon.
    // Shared rather than duplicated: a second copy of that 71-case switch
    // would stop matching the first the next time a mob is added.
    std::unique_ptr<Game::Mob> MakeMobForLoad(Game::EntityTypeId type, Game::EntityLevel* level);

    class LevelEntityStore {
    public:
        enum class State : uint8_t {
            Absent,    // never read, or read and dropped — do NOT write
            Pending,   // a worker read is in flight — do NOT write
            Loaded,    // this level's managers own this chunk's entities
        };

        explicit LevelEntityStore(ServerLevel& level) : m_level(level) {}
        // Writes everything still queued, then refuses new writes.
        ~LevelEntityStore();

        // Ask for a chunk's entities. A no-op when already Pending or Loaded,
        // so the many places that make a chunk live can all call it freely.
        // Asynchronous, as MC's (PersistentEntitySectionManager.
        // requestChunkLoad): the chunk goes Pending, the read runs on the I/O
        // worker, and ProcessPendingLoads adopts the entities a tick or so
        // later.
        void RequestLoad(Game::Math::ChunkPos pos);

        // Server thread, once a tick (MC processPendingLoads): adopt every
        // finished read, then place the chunk's worldgen mobs.
        void ProcessPendingLoads();

        // Server thread: finish every read queued or in flight and adopt it.
        // The shutdown save calls it first (MC saveAll drains the loads
        // until every chunk can be stored); an autosave does not wait.
        void CompleteLoads();

        bool IsPending(Game::Math::ChunkPos pos) const;

        // Every chunk holding a live mob, item or orb: one pass over the
        // level's entities, for a batch of ReadyToUnload calls.
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> OccupiedChunks() const;

        // Whether the chunk may unload now (MC: storeChunkSections must pass
        // before processChunkUnload does). False while its read is Pending;
        // false for an unread chunk holding live entities, whose read this
        // starts — they merge with the file's on arrival and the unload
        // retries. `holdsEntities` from OccupiedChunks.
        bool ReadyToUnload(Game::Math::ChunkPos pos, bool holdsEntities);

        // Server thread. Hands the worker's bytes to the managers.
        void ApplyLoadResult(Game::Math::ChunkPos pos, const std::vector<uint8_t>& nbt,
                             bool readSucceeded);

        // Gather this chunk's entities and write them. A chunk that now holds
        // nothing saveable is CLEARED from the region file rather than written
        // as an empty list, matching vanilla.
        bool SaveChunk(Game::Math::ChunkPos pos, std::string& error);

        // Every chunk that currently holds a saveable entity, plus every chunk
        // that used to and no longer does. Walks the MANAGERS, so it is
        // O(entities) rather than O(loaded chunks).
        void SaveAllLoaded();

        // Save, then forget the state entry. Called on unload BEFORE the
        // managers destroy anything.
        void SaveAndForget(Game::Math::ChunkPos pos);
        // The same for several chunks with ONE pass over the level's
        // entities. Finding a chunk's entities walks every mob, item and orb
        // of the level; per chunk, that was O(entities x unloads) on the
        // server thread — seconds when a whole server's players left at once.
        void SaveAndForgetMany(const std::vector<Game::Math::ChunkPos>& chunks);

        // Block until every queued entity write has reached the region files.
        // Shutdown only: autosave and pause-save leave the writes to the
        // background worker, as MC's saveAll(flush=false) does.
        void Flush();

        size_t PendingCount() const;

    private:
        // MC ServerLevel.addWorldGenChunkEntities: the mobs world generation
        // placed in this chunk (structure templates, swamp huts, monuments,
        // mansions, end cities) join the level the first time the chunk's
        // entities are claimed — right after the entities/*.mca read, so the
        // managers are authoritative for the chunk and the new mobs save with
        // it. ChunkProvider::TakeWorldgenEntities hands each list out once,
        // and a chunk read back from disk carries none: they spawn exactly once.
        void AddWorldgenEntities(Game::Math::ChunkPos pos);

        // SaveChunk with the chunk's entities already gathered (null: gather).
        bool SaveChunkWith(Game::Math::ChunkPos pos, const Game::Anvil::EntityChunkContents* gathered,
                           std::string& error);

        // Created on first use; see EntityIoWorker.
        EntityIoWorker& Io();

        ServerLevel& m_level;
        std::shared_ptr<EntityIoWorker> m_io;

        std::unordered_map<Game::Math::ChunkPos, State, Game::Math::ChunkPosHash> m_state;
        // The read each Pending chunk is waiting for: a result for any other
        // (the chunk was forgotten, or forgotten and asked again) is dropped.
        std::unordered_map<Game::Math::ChunkPos, uint64_t, Game::Math::ChunkPosHash> m_loadTickets;
        uint64_t m_loadTicketSeq = 0;
        // Chunks whose last write was non-empty. Without this we could not
        // tell that a chunk which used to hold entities now holds none, and
        // its stale list would stay on disk forever.
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> m_onDisk;
    };

} // namespace Server

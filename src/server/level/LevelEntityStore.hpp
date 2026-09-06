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
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Game {
    class Mob;
    struct EntityLevel;
    enum class EntityTypeId : uint16_t;
}

namespace Server {

    class ServerLevel;

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

        // Ask for a chunk's entities. A no-op when already Pending or Loaded,
        // so the many places that make a chunk live can all call it freely.
        void RequestLoad(Game::Math::ChunkPos pos);

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

        size_t PendingCount() const;

    private:
        ServerLevel& m_level;

        std::unordered_map<Game::Math::ChunkPos, State, Game::Math::ChunkPosHash> m_state;
        // Chunks whose last write was non-empty. Without this we could not
        // tell that a chunk which used to hold entities now holds none, and
        // its stale list would stay on disk forever.
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> m_onDisk;
    };

} // namespace Server

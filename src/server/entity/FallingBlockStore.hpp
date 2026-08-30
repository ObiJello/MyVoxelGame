// File: src/server/entity/FallingBlockStore.hpp
//
// The compact representation of a mass-falling block: sand, red sand and
// gravel spawned by their own block tick (the /shape pyramid, a collapsing
// dune, the ceiling a blast just took the floor from). One 64-byte row in a
// struct-of-arrays instead of an 816-byte Game::Mob object.
//
// WHY. FallingBlockEntity already takes every shortcut a Mob can (NoAiTag: no
// navigator, no sensing, no controls, no attributes, exempt from the
// dying-mob reference sweep), and it is still 816 bytes with the ten fields
// a tick reads scattered over a dozen cache lines. At 1.37M of them every
// per-tick pass — physics, tracker, classify, the end-of-tick scans — was a
// cache miss per entity, ~290 ms a tick fully parallel, because the walk was
// memory-bound. Here the same passes are sequential scans.
//
// WHAT IS SHARED, so the two representations cannot drift: the mover is
// Physics::MoveEntity (the same call FallingBlockEntity::Move makes), the
// landing is Game::FallingBlockTryLand (the same function its TryLand calls),
// and the tick order — gravity, move, [land damp + try land], drag ALWAYS
// last — is FallingBlockEntity's, with the same split-tick + conflict-retry
// rule MobManager applies to its falling batch (physics across the pool,
// landing serial in insertion order, a later block re-runs its move if an
// earlier landing this tick wrote a cell in its swept region).
//
// WHAT STAYS A Mob: anything with behaviour beyond fall-and-land. Anvils and
// dripstone (they hurt what they land on), concrete powder (the swept water
// clip and solidify), suspicious blocks (cancelDrop), dragon eggs,
// scaffolding, anything /summon'd or loaded from NBT. Takes() is the gate.
//
// KNOWN DIVERGENCES of this representation, all deliberate and all small:
//   * not a victim of explosions (MC's blast push moves a falling block);
//   * does not travel through portals;
//   * not persisted on chunk unload or world save (MC writes the entity to
//     the chunk; here it is dropped — it would have landed within seconds);
//   * not addressable by @e selectors.
// Each is the next thing to add if it ever matters; none affects a plain
// fall.
#pragma once

#include "common/world/block/BlockState.hpp"
#include "common/world/math/WorldMath.hpp"
#include "server/entity/ServerEntityTracker.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Server {

    class ChunkTicketManager;
    class MobManager;
    class ServerLevelBridge;

    class FallingBlockStore {
    public:
        FallingBlockStore(ServerLevelBridge* level, MobManager* ids);

        // Is this carried state one the store represents? See the header.
        static bool Takes(Game::BlockState state);

        // MC FallingBlockEntity.fall: horizontally centred on the cell, feet at
        // its y, zero velocity. Returns the entity id (mob range).
        int32_t Spawn(const glm::ivec3& blockPos, Game::BlockState state);

        // One server tick of physics and landing. Runs where MobManager::Tick
        // ticks its own falling batch — after the mobs, before the blasts.
        void Tick(const ChunkTicketManager* tickets);

        // The tracker half: watch sets, adds, movement, and the removals for
        // everything Tick retired. Same contract as ServerEntityTracker::Tick.
        void Track(const std::vector<ServerEntityTracker::TrackedPlayer>& players,
                   const ChunkTicketManager* tickets,
                   std::vector<EntityPacketOut>& out);

        // Chunk unload: drop every block standing in one of `chunks`, telling
        // its watchers. See the persistence note in the header.
        void RemoveInChunks(const std::vector<Game::Math::ChunkPos>& chunks,
                            std::vector<EntityPacketOut>& out);

        void RemovePlayer(uint32_t connectionId);
        void Clear();

        size_t Count() const { return m_id.size(); }

    private:
        struct Snapshot {
            glm::dvec3 position;
            glm::dvec3 velocity;
            bool       onGround;
        };
        enum class Outcome : uint8_t { Skipped, Airborne, NeedsLanding };

        Outcome TickPhysics(size_t i);
        void    Compact();
        int     SlotOf(uint32_t connectionId);

        ServerLevelBridge* m_level;
        MobManager*        m_ids;

        // ── The rows. Parallel vectors; index = insertion (tick) order. ────
        std::vector<int32_t>    m_id;
        std::vector<glm::dvec3> m_pos;
        std::vector<glm::dvec3> m_oldPos;
        std::vector<glm::dvec3> m_vel;
        std::vector<uint32_t>   m_state;     // BlockState raw id
        std::vector<int32_t>    m_time;      // MC `time`
        std::vector<uint8_t>    m_onGround;
        std::vector<uint8_t>    m_dead;      // retired this tick; compacted after Track

        // ── Tracker state, one row per block ───────────────────────────────
        std::vector<int64_t>    m_baseX, m_baseY, m_baseZ;   // delta base (1/4096)
        std::vector<uint64_t>   m_watchers;                  // bit per player slot
        std::vector<int32_t>    m_trackTicks;
        std::vector<int32_t>    m_teleportDelay;
        std::vector<uint64_t>   m_lastChunkKey;
        std::vector<uint8_t>    m_wasOnGround;
        // slot -> connection id (0 = free). 64 slots: a bit per player.
        std::vector<uint32_t>   m_slots;

        // ── Per-tick scratch, members for capacity ─────────────────────────
        std::vector<Snapshot>   m_snap;
        std::vector<uint8_t>    m_outcome;
        std::unordered_map<uint64_t, std::pair<int, int>> m_writtenColumns;
    };

} // namespace Server

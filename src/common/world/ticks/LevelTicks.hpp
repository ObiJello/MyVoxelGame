// File: src/common/world/ticks/LevelTicks.hpp
//
// MC net.minecraft.world.ticks.LevelTicks — the world-level scheduled-tick
// index and drain loop.
//
// The queues themselves live on the chunks (LevelChunkTicks); this is the thing
// that knows which of them have work pending and merges them into one globally
// ordered stream each tick. Keeping the two apart is vanilla's split and it is
// what makes chunk unload a no-op here: the container goes with the chunk and
// this class notices on its next pass.
//
// COLLECT THEN RUN, always. Every eligible tick is pulled out of its container
// before any of them executes. That is not a micro-optimisation — a falling
// sand block's tick spawns an entity and clears its cell, which fires neighbour
// updates that schedule more ticks. If those landed in the same drain pass, a
// column of sand would collapse entirely within one game tick instead of
// cascading one block per tick the way vanilla does.
#pragma once

#include "common/world/ticks/LevelChunkTicks.hpp"
#include "common/world/ticks/ScheduledTickAccess.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace Game {

    class Chunk;

    class LevelTicks : public ScheduledTickAccess {
    public:
        // How a chunk is found. World points this at GetLoadedChunk, so an
        // unloaded chunk resolves to null and its appointments are simply not
        // run — correct, because an unloaded chunk is not simulating either.
        //
        // Resolving lazily rather than keeping an addContainer/removeContainer
        // registry is deliberate: a registry has to be kept in step with every
        // chunk load and unload, and a stale entry there is a dangling pointer.
        //
        // It hands back the CHUNK rather than its container because the
        // container has to be touched under the chunk's content lock — the
        // chunk saver walks a chunk from a worker thread while the tick thread
        // is scheduling into it, which is the same race Chunk::LockShared was
        // introduced for. Returning a bare pointer would leave the caller no
        // way to take that lock.
        using ContainerResolver = std::function<std::shared_ptr<Chunk>(int chunkX, int chunkZ)>;

        // MC LevelTicks' `tickCheck` predicate — is this chunk close enough to a
        // player to simulate? Mirrors the gate PerformRandomBlockTick already
        // uses. Null means "everything ticks", which is what tests want.
        using TickCheck = std::function<bool(int chunkX, int chunkZ)>;

        // Called once per due appointment, in drain order.
        using Runner = std::function<void(const glm::ivec3& pos, BlockID type)>;

        void SetContainerResolver(ContainerResolver resolver) {
            m_resolve = std::move(resolver);
        }
        void SetTickCheck(TickCheck check) { m_tickCheck = std::move(check); }

        // The current game time, which ScheduleTick adds its delay to. World
        // pushes this in each tick rather than LevelTicks reaching for it, so
        // this class stays free of any dependency on World.
        void SetGameTime(int64_t gameTime) { m_gameTime = gameTime; }

        // ── ScheduledTickAccess ────────────────────────────────────────────
        void ScheduleTick(const glm::ivec3& pos, BlockID block, int delay,
                          TickPriority priority = TickPriority::Normal) override;
        bool HasScheduledTick(const glm::ivec3& pos, BlockID block) const override;

        // MC ServerLevel: `blockTicks.tick(gameTime, 65536, this::tickBlock)`.
        // Returns how many appointments ran, for metrics.
        int Tick(int64_t gameTime, int maxToProcess, const Runner& run);

        // Monotonic insertion counter — the final tiebreak that makes drain
        // order reproducible. Exposed because chunk load restores saved ticks
        // straight into a container and must draw from the same sequence.
        int64_t NextSubTickOrder() { return m_subTickCounter++; }

        // A chunk that has just been loaded with saved ticks has to be
        // announced, because nothing scheduled into it through this class and
        // the drain only ever looks at chunks it already knows about.
        //
        // THREAD-SAFE, unlike everything else here: chunk loads finish on the
        // worker pool, so this one entry point takes a lock and parks the key
        // in an inbox that the next Tick() folds in on the server thread.
        void NoteChunkWithTicks(int chunkX, int chunkZ);

        void Clear() {
            m_active.clear();
            m_toRunSet.clear();
            m_collected.clear();
        }

        size_t ActiveChunkCount() const { return m_active.size(); }

    private:
        static uint64_t ChunkKey(int chunkX, int chunkZ) {
            return (static_cast<uint64_t>(static_cast<uint32_t>(chunkX)) << 32) |
                    static_cast<uint64_t>(static_cast<uint32_t>(chunkZ));
        }
        static int KeyX(uint64_t k) { return static_cast<int>(static_cast<int32_t>(k >> 32)); }
        static int KeyZ(uint64_t k) { return static_cast<int>(static_cast<int32_t>(k & 0xFFFFFFFFull)); }

        void CollectTicks(int64_t gameTime, int maxToProcess);

        ContainerResolver m_resolve;
        TickCheck         m_tickCheck;
        int64_t           m_gameTime      = 0;
        int64_t           m_subTickCounter = 0;
        // See ScheduleTick. ~0 = no memo.
        uint64_t          m_lastActiveKey = ~0ull;

        // Chunks believed to hold at least one appointment. Kept as a set of
        // keys rather than container pointers so an unloaded chunk leaves a
        // harmless stale key rather than a dangling pointer; the drain resolves
        // each key and prunes the ones that no longer answer.
        std::unordered_set<uint64_t> m_active;

        // See NoteChunkWithTicks. Drained into m_active at the top of Tick().
        std::mutex            m_inboxMutex;
        std::vector<uint64_t> m_inbox;

        // Reused across ticks so a drain allocates nothing in steady state.
        std::vector<ScheduledTick>   m_collected;
        std::vector<uint64_t>        m_activeScratch;
        // Appointments pulled from their containers but not yet run. Consulted
        // by HasScheduledTick so a block asking mid-drain gets the truth.
        std::unordered_set<TickIdentity, TickIdentityHash> m_toRunSet;
    };

} // namespace Game

// File: src/server/entity/ItemEntityManager.hpp
//
// Owns every dropped item in the world and drives it: physics, merging,
// player pickup and despawn. This is the server half of MC's ItemEntity —
// clients get read-only snapshots (see Client::ItemEntityManager).
//
// Why this lives under src/server/ rather than on Game::World: pickup has to
// reach ServerPlayer and the session list, and World is common code shared with
// the client. Ticking from here keeps that dependency pointing the right way.
// Game::World::EntityTick() stays the empty stub it has always been.
#pragma once

#include "common/entity/ItemEntity.hpp"
#include "common/entity/Item.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/math/WorldMath.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/world/block/Direction.hpp"

#include <glm/glm.hpp>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Game { class World; }

namespace Server {

    class PlayerSessionManager;

    // The id space itself lives in common (Game::kItemEntityIdBase) because the
    // client dispatches removals on it too.
    using Game::kItemEntityIdBase;

    // A player collected some of an item this tick. Drives the client's pickup
    // animation (MC ClientboundTakeItemEntityPacket).
    //
    // Namespace scope rather than nested in ItemEntityManager so IntegratedServer
    // can name it from a forward declaration instead of pulling this whole
    // header (and Physics, and ItemEntity) into its own.
    struct ItemPickupEvent {
        int32_t  itemEntityId;
        uint32_t playerId;
        int32_t  amount;
    };

    class ItemEntityManager {
    public:
        // ── Spawning ───────────────────────────────────────────────────────

        // Raw spawn. `pos` is the entity's feet, `vel` is blocks per TICK.
        // Returns the new entity id, or 0 if the stack was empty.
        int32_t Spawn(const glm::dvec3& pos, const glm::dvec3& vel,
                      const Game::ItemStack& stack, int pickupDelay);

        // MC Block.popResource — the standard "a block produced this" drop.
        // Scatters within the block cell and gives it a small upward hop.
        void PopResource(const glm::ivec3& blockPos, const Game::ItemStack& stack);

        // MC Block.popResourceFromFace — drop nudged out of one face, used
        // when the item logically comes off a particular side of a block.
        void PopResourceFromFace(const glm::ivec3& blockPos, Game::Direction face,
                                 const Game::ItemStack& stack);

        // MC LivingEntity.drop with randomly=false — a player throwing from the
        // hand, launched along their look direction with a little spread.
        //
        // Takes the look direction as a VECTOR rather than yaw/pitch on
        // purpose. The angles agree with MC's now, but a drop direction is a
        // direction — handing it a vector keeps this manager free of any angle
        // convention at all, so it cannot be broken by one again.
        //
        // `eyePos` is the thrower's eye position; `forward` need not be
        // normalised.
        int32_t DropFromPlayer(const glm::dvec3& eyePos, const glm::dvec3& forward,
                               const Game::ItemStack& stack);

        // MC LivingEntity.drop with randomly=true — scattered in a random
        // horizontal direction, ignoring where the entity is looking. What
        // death drops and "empty this container" use.
        int32_t DropScattered(const glm::dvec3& pos, const Game::ItemStack& stack);

        // ── Lifecycle ──────────────────────────────────────────────────────

        // One server tick: physics, merging, pickup, despawn.
        //
        // `outRemoved` gets the ids that died for reasons the client cannot see
        // coming — despawn, merge. Entities that were PICKED UP go to
        // `outPickups` instead and must not also be broadcast as removals; the
        // take packet is what retires them client-side, and it carries the
        // animation with it.
        void Tick(Game::World* world, PlayerSessionManager* sessions,
                  std::vector<int32_t>& outRemoved,
                  std::vector<ItemPickupEvent>& outPickups);

        // Drop every entity in a chunk that is being unloaded. Without this,
        // items in unloaded chunks keep ticking against a world that no longer
        // has the blocks under them and fall forever.
        void RemoveInChunk(Game::Math::ChunkPos chunk, std::vector<int32_t>& outRemoved);

        void Clear();

        // ── Access ─────────────────────────────────────────────────────────
        const std::unordered_map<int32_t, Game::ItemEntity>& All() const { return m_entities; }

        // Mutable lookup by id, for the command layer (/tp can move a dropped
        // item). Returns null when the id is unknown — an entity can despawn
        // between a selector resolving it and the caller acting on it, so the
        // pointer is deliberately not cached anywhere.
        Game::ItemEntity* Find(int32_t id) {
            auto it = m_entities.find(id);
            return it == m_entities.end() ? nullptr : &it->second;
        }

        size_t Count() const { return m_entities.size(); }

        // Split this tick's entities into the two kinds of update.
        //
        // `outFullRefresh` gets a whole spawn packet (stack included):
        // everything spawned since the last call, plus everything due on the
        // periodic cadence. The periodic half is what introduces existing items
        // to a player who has just walked into their chunk — there is no
        // per-client tracked-entity set here (MC's ServerEntity), so a cheap
        // full re-send on a rotation is what takes its place. A client that
        // already knows the entity treats the packet as an in-place update.
        //
        // `outMoveOnly` gets a compact position update: entities whose motion
        // changed enough to need an out-of-band resend but which the client
        // already knows about.
        //
        // Clears the per-entity needsSync flags and the spawn list as it goes.
        void CollectSyncSets(int64_t serverTick,
                             std::vector<int32_t>& outFullRefresh,
                             std::vector<int32_t>& outMoveOnly);

    private:
        // MC ServerEntity.updateInterval for EntityType.ITEM — refresh every
        // entity at least this often even when nothing changed, so a client
        // that missed a packet (or only just arrived) self-heals within a
        // second.
        // A new entity goes out on the very NEXT tick via its pendingSpawn
        // flag, not on this rotation — waiting for its turn would put up to a
        // second between breaking a block and seeing the drop, which reads as
        // the feature being broken.
        static constexpr int kSyncIntervalTicks = 20;

        std::unordered_map<int32_t, Game::ItemEntity> m_entities;
        int32_t  m_nextId = kItemEntityIdBase;
        Game::JavaRandom m_random{0};

        // Try to merge `entity` into any eligible neighbour. Returns the id of
        // whichever entity was consumed (0 if none merged).
        int32_t TryMergeWithNeighbours(Game::ItemEntity& entity);
    };

} // namespace Server

// File: src/client/entity/ItemEntityManager.hpp
//
// Client-side view of the dropped items the server owns.
//
// The client SIMULATES item physics locally, exactly as MC does — in
// ItemEntity.tick only merging, despawn and pickup sit behind
// `!level().isClientSide()`; `applyGravity` and `move` run on both sides. The
// server's snapshots are corrections layered on top, not the only source of
// motion.
//
// This matters more than it looks. A falling item's velocity changes by only
// 0.04 per tick (|Δv|² = 0.0016), far below any sane "motion changed, resend
// it" threshold, so a purely interpolating client receives almost nothing while
// an item is in the air and shows it teleporting between the sparse periodic
// snapshots. Simulating locally is what makes a dropped block fall smoothly.
//
// Corrections follow MC's InterpolationHandler: a snapshot sets a target and a
// step count, and on each tick the target is first advanced by the delta the
// LOCAL simulation just produced, then the entity eases 1/steps of the way
// toward it. Because the target tracks the simulation, a correct prediction
// converges to a no-op instead of fighting the local physics.
#pragma once

#include "common/entity/Item.hpp"
#include "common/entity/ItemEntity.hpp"
#include "common/physics/Physics.hpp"
#include <glm/glm.hpp>
#include <unordered_map>
#include <vector>
#include <memory>
#include <cstdint>

namespace Client {

    struct ClientItemEntity {
        // The locally simulated entity. Carries position, velocity, onGround,
        // the stack, and the bob phase.
        Game::ItemEntity sim;

        // ── Server correction (MC InterpolationHandler) ────────────────────
        int        interpSteps = 0;
        glm::dvec3 interpTarget{0.0};
        glm::dvec3 prevTickPos{0.0};

        // Previous-tick position for sub-tick render blending. Mirrors MC
        // Entity.xo/yo/zo, snapshotted at the top of the tick.
        glm::dvec3 renderPrevPosition{0.0};

        // Age in client ticks, driving the render bob and spin. Deliberately
        // not synced: the phase is already randomised per entity by bobOffs, so
        // a few ticks of disagreement is invisible, and syncing it would cost a
        // field on every move packet.
        float ageTicks = 0.0f;

        bool initialized = false;
    };

    // An item flying into the player who just collected it — MC's
    // ItemPickupParticle. Purely visual and purely client-side; the server has
    // already moved the items into the inventory by the time this exists.
    struct ItemPickupAnim {
        // What flew, and the frozen render state it flew with. MC snapshots the
        // entity's render state at pickup, so the item keeps the orientation and
        // bob phase it had at that instant rather than continuing to animate.
        Game::ItemStack stack{};
        float      bobOffs  = 0.0f;
        float      ageTicks = 0.0f;

        glm::dvec3 startPos{0.0};      // where the item was when collected

        uint32_t   targetPlayerId = 0;
        // Target tracked across two ticks so the render can interpolate it —
        // the collector keeps moving during the animation.
        //
        // Both slots are seeded from the COLLECTOR on the first tick, not from
        // where the item lay. MC does this in the particle's constructor
        // (updatePosition then saveOldPosition); seeding `old` with the item's
        // own position instead would make the first tick interpolate the target
        // from the item to the player and the flight would visibly stall for
        // its first frames.
        bool       targetSeeded = false;
        glm::dvec3 targetPos{0.0};
        glm::dvec3 targetPosOld{0.0};

        int life = 0;                  // ticks elapsed; dies at kPickupLifeTicks
    };

    class ItemEntityManager {
    public:
        // Spawn or full-refresh. Re-sending a known id updates in place; the
        // server uses that as its periodic "resync this entity" path, which is
        // also how an entity is introduced to a player who just walked up.
        void Spawn(int32_t id, const glm::dvec3& pos, const glm::vec3& vel,
                   float bobOffs, const Game::ItemStack& stack);

        // Periodic position/velocity refresh. `count` keeps the rendered stack
        // size in step with merges and partial pickups, which change it
        // mid-life.
        void Move(int32_t id, const glm::dvec3& pos, const glm::vec3& vel, int32_t count);

        void Remove(int32_t id) { m_entities.erase(id); }
        void Clear() { m_entities.clear(); m_pickups.clear(); }

        // A player collected `amount` items from this entity (MC
        // handleTakeItemEntity). Starts the fly-to-player animation and shrinks
        // the local copy, retiring the entity when it empties.
        //
        // This — not RemoveEntitiesS2CPacket — is what removes a fully
        // collected item on the client, so the animation always has an entity
        // to capture its starting state from.
        void TakeItem(int32_t itemId, uint32_t playerId, int32_t amount);

        // 20 Hz. Runs local physics, applies pending corrections, and advances
        // pickup animations. Takes the local player's feet position because a
        // pickup animation has to fly toward whoever collected it, and the
        // local player is not in any entity map to look up.
        void Tick(const glm::dvec3& localPlayerPos);

        const std::unordered_map<int32_t, ClientItemEntity>& GetEntities() const {
            return m_entities;
        }
        const std::vector<ItemPickupAnim>& GetPickups() const { return m_pickups; }
        size_t Count() const { return m_entities.size(); }

        // MC ItemPickupParticle.LIFE_TIME — the whole flight lasts 3 ticks
        // (150 ms). It is meant to read as a snap, not a glide.
        static constexpr int kPickupLifeTicks = 3;

    private:
        // MC InterpolationHandler.DEFAULT_INTERPOLATION_STEPS.
        static constexpr int kInterpSteps = 3;

        // Beyond this the correction is applied as a hard snap rather than
        // eased in. Easing a large error looks like the item gliding through
        // the world; that only happens when the client's simulation has
        // genuinely diverged (a missed packet, a chunk that wasn't loaded when
        // the item fell through it), and a snap is the honest fix.
        static constexpr double kSnapDistanceSq = 4.0 * 4.0;

        // Set from `interpolateTo`.
        void InterpolateTo(ClientItemEntity& e, const glm::dvec3& target);

        // Where a pickup animation should fly to. MC aims at the midpoint
        // between the collector's feet and eyes — chest height — not at their
        // feet, which is why items visibly arc up into the body.
        // Only REMOTE players live in g_remotePlayerManager, so an id that
        // misses there is the local player — which is also MC's own fallback
        // (`if (to == null) to = minecraft.player`). That means no local player
        // id is needed anywhere.
        glm::dvec3 ResolveTarget(uint32_t playerId,
                                 const glm::dvec3& localPlayerPos) const;

        std::unordered_map<int32_t, ClientItemEntity> m_entities;
        std::vector<ItemPickupAnim> m_pickups;
    };

    extern std::unique_ptr<ItemEntityManager> g_itemEntityManager;

} // namespace Client

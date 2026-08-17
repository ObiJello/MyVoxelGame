// File: src/common/entity/ItemEntity.hpp
//
// A dropped item lying in the world — MC's net.minecraft.world.entity.item.ItemEntity.
//
// Everything here runs in MC's PER-TICK units, not per-second. That is a
// deliberate departure from PlayerPhysics, whose constants are per-second
// because player physics is integrated once per FRAME. Item entities tick at a
// fixed 20 Hz on the server, so MC's numbers (gravity 0.04, drag 0.98, …) are
// used verbatim and no dt appears anywhere in Tick(). Mixing the two conventions
// is the easiest way to get items that fall at wildly wrong speeds — if you find
// yourself reaching for PlayerPhysics::GRAVITY here, you want kGravity instead.
//
// The entity is SERVER-AUTHORITATIVE. The client keeps its own copies purely for
// rendering and never runs this physics (see Client::ItemEntityManager, which
// interpolates between server snapshots exactly the way remote players do).
#pragma once

#include "common/entity/Item.hpp"
#include "common/physics/Physics.hpp"
#include <glm/glm.hpp>
#include <cstdint>

namespace Game {

    // ── Entity id space ────────────────────────────────────────────────────
    //
    // Item-entity ids live at or above this value; player entity ids (which are
    // connection ids) live below it.
    //
    // This split is load-bearing, not cosmetic. Despawn reuses
    // RemoveEntitiesS2CPacket for both kinds, so the client's handler has to
    // decide which of its two entity maps an incoming id belongs to. Sharing
    // one id space with no way to tell them apart would let a despawning item
    // silently evict a player with the same numeric id.
    //
    // It lives in common rather than on the server manager because BOTH sides
    // need it: the server allocates from it, the client dispatches on it.
    constexpr int32_t kItemEntityIdBase = 0x0100'0000;

    inline bool IsItemEntityId(int32_t id) { return id >= kItemEntityIdBase; }

    struct ItemEntity {
        // ── MC constants (ItemEntity.java / EntityType.ITEM) ───────────────
        // Width and height are both 0.25 (EntityType.ITEM .sized(0.25F, 0.25F)).
        static constexpr float  kWidth      = 0.25f;
        static constexpr float  kHeight     = 0.25f;

        // ItemEntity.getDefaultGravity() = 0.04, applied as vel.y -= gravity.
        static constexpr double kGravity    = 0.04;

        // Air drag, applied to ALL THREE axes each tick (the y multiplier is
        // 0.98 regardless of whether the entity is grounded).
        static constexpr double kAirDrag    = 0.98;

        // Horizontal drag while grounded is blockFriction * 0.98. This engine
        // has no per-block friction property anywhere (players don't model ice
        // either), so we use MC's DEFAULT block friction of 0.6 for every
        // block: 0.6 * 0.98 = 0.588. Items therefore do not slide on ice —
        // consistent with how the player behaves here, and the one number to
        // change if per-block friction ever lands.
        static constexpr double kBlockFriction  = 0.6;
        static constexpr double kGroundDrag     = kBlockFriction * kAirDrag;

        // On landing, MC damps the remaining downward velocity and flips it,
        // producing a small settle rather than a dead stop.
        static constexpr double kBounceDamping  = -0.5;

        // ItemEntity.LIFETIME — 6000 ticks = 5 minutes at 20 TPS.
        static constexpr int kLifetimeTicks         = 6000;
        // setDefaultPickUpDelay() — what a block drop gets.
        static constexpr int kDefaultPickupDelay    = 10;
        // LivingEntity.createItemStackToDrop — what a player throw gets, so you
        // can't instantly re-collect something you meant to discard.
        static constexpr int kThrowPickupDelay      = 40;

        // Merge scan cadence: every 2 ticks if the entity changed block this
        // tick, else every 40. Scanning every tick is pure waste for a pile of
        // items that has already settled.
        static constexpr int kMergeIntervalMoving = 2;
        static constexpr int kMergeIntervalIdle   = 40;

        // Merge search box: own AABB inflated by 0.5 horizontally and 0.0
        // vertically, so items merge across a small gap on the ground but never
        // with something floating above them.
        static constexpr float kMergeInflateXZ = 0.5f;

        // Below this squared horizontal speed a grounded item is considered
        // settled and its movement step is skipped on 3 ticks out of 4.
        static constexpr double kSleepSpeedSqEpsilon = 1.0e-5;

        // ── State ──────────────────────────────────────────────────────────
        int32_t    id    = 0;
        ItemStack  stack{};

        // `pos` is the entity's FEET, matching MC and MoveAABB's convention.
        // Doubles because items drift for minutes and float drift is visible.
        glm::dvec3 pos{0.0};
        glm::dvec3 vel{0.0};      // blocks per TICK

        int   age         = 0;
        int   pickupDelay = 0;
        bool  onGround    = false;

        // Random phase for the render bob/spin, so a pile of items doesn't
        // pulse in unison. Rolled once at spawn and sent on the wire — MC
        // re-rolls it per client, but syncing costs 4 bytes and keeps host and
        // joiner showing the same thing.
        float bobOffs = 0.0f;

        // Tick counter used for the merge cadence and the sleep skip. Distinct
        // from `age`, which can be negative for MC's extended-lifetime items.
        int   tickCount = 0;

        // ── Sync bookkeeping (server-side only) ────────────────────────────
        // Set when this entity's motion changed enough that clients need a
        // fresh snapshot rather than waiting for the periodic resend.
        bool  needsSync = false;
        // Set at spawn, cleared once the full spawn packet has gone out. These
        // cannot be sent as a compact move update — the client has never seen
        // the stack.
        bool  pendingSpawn = true;
        // Emptied by a player collecting it, rather than by despawning or being
        // merged away. Such an entity is deliberately left OUT of the removal
        // broadcast: the take packet removes it on the client, and a removal
        // racing ahead would delete the entity before its pickup animation
        // could fly it to the player.
        bool  pickedUp = false;

        bool IsEmpty() const { return stack.IsEmpty(); }

        // Half-extents for MoveAABB / AABB construction.
        static glm::vec3 HalfExtents() {
            return glm::vec3(kWidth * 0.5f, kHeight * 0.5f, kWidth * 0.5f);
        }

        AABB GetAABB() const {
            return AABB(glm::vec3(pos.x, pos.y + kHeight * 0.5f, pos.z),
                        glm::vec3(kWidth, kHeight, kWidth));
        }

        // Gravity, movement, drag and bounce for one tick.
        //
        // Runs on BOTH sides. In MC, ItemEntity.tick guards only merging,
        // despawn and pickup behind `!level().isClientSide()` — `applyGravity`
        // and `move` are unconditional, so the client simulates the same
        // trajectory and the server's periodic snapshots merely correct it.
        // A client that only interpolated between those snapshots would show
        // items teleporting, because free fall changes velocity too little per
        // tick to trip any reasonable resend threshold.
        //
        // Order matters and mirrors ItemEntity.tick exactly: the move must
        // happen BEFORE drag, because MoveAABB zeroes the velocity of any axis
        // that hit something and applying drag first would scale a value that
        // is about to be discarded.
        void TickMovement(const PhysicsContext& context);

        // Full server-side tick: TickMovement plus the pieces MC runs only on
        // the server — ageing, despawn, and resend bookkeeping.
        //
        // Returns false when the entity should be removed (it despawned or its
        // stack emptied); the manager is responsible for actually erasing it.
        bool Tick(const PhysicsContext& context);

        // True when this entity is eligible to merge with a neighbour: alive,
        // not already at a full stack, and not past its lifetime.
        bool IsMergable() const;
    };

    // Can these two stacks combine into one entity? Mirrors
    // ItemEntity.areMergable: ALL-OR-NOTHING — MC refuses outright when the
    // totals would overflow a stack rather than topping one up and leaving a
    // remainder, so two 40s of a 64-stack item stay two entities.
    bool CanMergeItemEntities(const ItemStack& a, const ItemStack& b);

} // namespace Game

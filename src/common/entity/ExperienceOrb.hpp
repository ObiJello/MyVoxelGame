// File: src/common/entity/ExperienceOrb.hpp
//
// An experience orb — MC's net.minecraft.world.entity.ExperienceOrb.
//
// Like ItemEntity, everything runs in MC's PER-TICK units (gravity 0.03,
// friction 0.98) with no dt anywhere, and the entity is SERVER-AUTHORITATIVE:
// the server owns spawning, merging, pickup and despawn, while the client
// simulates the same physics locally (MC's ExperienceOrb.tick runs its
// movement and followNearbyPlayer on both sides) and treats server snapshots
// as corrections. That local simulation is what makes the "sucked toward the
// player" pull look continuous — the pull changes velocity every tick by far
// less than any resend threshold.
#pragma once

#include "common/core/Uuid.hpp"

#include "common/physics/Physics.hpp"
#include "common/core/JavaRandom.hpp"
#include <glm/glm.hpp>
#include <cstdint>

namespace Game {

    // Orb ids live at or above Game::kXpOrbEntityIdBase (Entity.hpp) — the
    // shared RemoveEntitiesS2CPacket is dispatched by id range.

    struct ExperienceOrb {
        // ── MC constants (ExperienceOrb.java / EntityType.EXPERIENCE_ORB) ──
        // EntityType.EXPERIENCE_ORB.sized(0.5F, 0.5F).
        static constexpr float  kWidth  = 0.5f;
        static constexpr float  kHeight = 0.5f;
        // Entity.getEyeHeight default: 85% of the height.
        static constexpr float  kEyeHeight = kHeight * 0.85f;

        // getDefaultGravity() — lighter than an item's 0.04, which is why orbs
        // drift down noticeably slower than drops.
        static constexpr double kGravity = 0.03;

        // Air/ground drag, applied to ALL THREE axes (ExperienceOrb.tick does
        // `scale(friction)`, unlike the item's per-axis multiply).
        static constexpr double kAirDrag       = 0.98;
        static constexpr double kBlockFriction = 0.6;   // engine default — see ItemEntity
        static constexpr double kGroundDrag    = kBlockFriction * kAirDrag;

        // Landing bounce: `if (verticalCollisionBelow && fallSpeed < -gravity)
        // vel.y = -fallSpeed * 0.4` — orbs bounce visibly where items settle.
        static constexpr double kBounceFactor = 0.4;

        // Water: x/z keep 0.99, y climbs by 5.0E-4 up to a 0.06 cap
        // (setUnderwaterMovement — note the cap is a clamp here, min(y+…, cap),
        // subtly different from the item's "add only while below").
        static constexpr double kWaterDrag     = 0.99;
        static constexpr double kWaterBuoyancy = 5.0e-4;
        static constexpr double kWaterMaxRise  = 0.06;

        // LIFETIME — 6000 ticks = 5 minutes.
        static constexpr int kLifetimeTicks = 6000;

        // followNearbyPlayer: any non-spectator, non-dead player within
        // MAX_FOLLOW_DIST (8) pulls the orb with
        //   vel += normalize(delta) * ((1 - dist/8)² * 0.1)
        // where delta aims at the player's mid-eye height.
        static constexpr double kMaxFollowDist = 8.0;
        static constexpr double kFollowAccel   = 0.1;

        // scanForMerges cadence (`tickCount % 20 == 1`) and search box
        // inflation (0.5 on every axis).
        static constexpr int   kMergeScanPeriod = 20;
        static constexpr float kMergeInflate    = 0.5f;

        // ORB_GROUPS_PER_AREA — merging keys on `(id - group) % 40 == 0` so a
        // farm's worth of orbs collapses into at most 40 entities per value.
        static constexpr int kOrbGroups = 40;

        // Player.takeXpDelay — a player absorbs at most one orb count every 2
        // ticks, which is the staggered "drip" of a big pile being collected.
        static constexpr int kTakeDelayTicks = 2;

        // ── State ──────────────────────────────────────────────────────────
        int32_t id    = 0;
        // Persistent identity, minted by the manager that assigns `id`.
        // `id` itself is a per-session handle and cannot name this across a save.
        Uuid uuid{};
        int     value = 0;     // XP points this orb pays per count
        int     count = 1;     // merged orbs stack a multiplier instead of value

        glm::dvec3 pos{0.0};   // FEET, like every entity here
        glm::dvec3 vel{0.0};   // blocks per TICK

        int  age       = 0;
        int  tickCount = 0;
        bool onGround  = false;

        // Which player currently pulls this orb (server: connection id,
        // client: RemotePlayerManager key or kLocalPlayerFollowId). MC keeps
        // following its current player until they die / leave / pass 8 blocks
        // — it does NOT retarget to whoever is nearest — so the id is state,
        // not a per-tick derivation. -1 = nobody.
        int64_t followingPlayerId = -1;

        // ── Sync bookkeeping (server-side only) ────────────────────────────
        bool needsSync    = false;
        bool pendingSpawn = true;
        // A player started draining this orb. The first take packet removes it
        // client-side (MC removes the orb on ClientboundTakeItemEntityPacket
        // even when count > 1 keeps paying out server-side), so it must never
        // re-enter the periodic full-refresh rotation or be broadcast as a
        // removal when it finally empties.
        bool pickedUp     = false;

        static glm::vec3 HalfExtents() {
            return glm::vec3(kWidth * 0.5f, kHeight * 0.5f, kWidth * 0.5f);
        }

        AABB GetAABB() const {
            return AABB(glm::vec3(pos.x, pos.y + kHeight * 0.5f, pos.z),
                        glm::vec3(kWidth, kHeight, kWidth));
        }

        // One tick of ExperienceOrb.tick's movement, shared by both sides:
        // buoyancy/gravity, the lava pop, the player pull, the (server-only)
        // stuck-in-block escape, the move, friction and the landing bounce.
        //
        // `followTarget`, when non-null, is the point the orb is pulled toward
        // — the following player's Y plus HALF their eye height
        // (followNearbyPlayer's `getY() + getEyeHeight()/2`). The manager
        // resolves the player id to a position because only it can see the
        // player lists.
        //
        // `rng` feeds the lava scatter on both sides and the escape roll;
        // `isServer` gates the stuck-escape exactly as MC's isClientSide
        // checks do.
        void TickMovement(const PhysicsContext& context,
                          const glm::dvec3* followTarget,
                          JavaRandom& rng, bool isServer);

        // ── MC's value tables ──────────────────────────────────────────────

        // ExperienceOrb.getExperienceValue — the denominations XP is split
        // into when awarded (2477, 1237, 617, … 7, 3, 1).
        static int GetExperienceValue(int maxValue);

        // ExperienceOrb.getIcon — which of the texture's 11 sprites this
        // value renders as.
        static int GetIcon(int value);
    };

} // namespace Game

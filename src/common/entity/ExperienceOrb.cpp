// File: src/common/entity/ExperienceOrb.cpp
#include "ExperienceOrb.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    namespace {
        // MC Entity.isEyeInFluid(FluidTags.WATER), reduced to source fluids:
        // the eye point is submerged when its cell holds water whose surface
        // (8/9 of the cell, or the full cell when water continues above)
        // stands above the eye.
        bool EyeInWater(const glm::dvec3& feetPos, const PhysicsContext& ctx) {
            const double eyeY = feetPos.y + ExperienceOrb::kEyeHeight;
            const int bx = static_cast<int>(std::floor(feetPos.x));
            const int by = static_cast<int>(std::floor(eyeY));
            const int bz = static_cast<int>(std::floor(feetPos.z));
            if (!ctx.ContainsWater(bx, by, bz)) return false;
            const double surface = ctx.ContainsWater(bx, by + 1, bz)
                ? static_cast<double>(by) + 1.0
                : static_cast<double>(by) + 8.0 / 9.0;
            return eyeY < surface;
        }
    } // namespace

    void ExperienceOrb::TickMovement(const PhysicsContext& context,
                                     const glm::dvec3* followTarget,
                                     JavaRandom& rng, bool isServer) {
        ++tickCount;

        const AABB box = GetAABB();
        // ExperienceOrb.tick: `boolean colliding = !noCollision(bbox)` — the
        // orb is overlapping a collision shape (spawned inside a block, or a
        // block was placed on it).
        const bool colliding = CollidesAt(box, context);

        // ── Buoyancy or gravity ────────────────────────────────────────────
        if (EyeInWater(pos, context)) {
            // setUnderwaterMovement — note the y term is a CLAMP
            // (min(y + 5.0E-4, 0.06)), so an orb dropped into water from a
            // height has its plunge cut to the cap immediately, unlike items.
            vel.x *= kWaterDrag;
            vel.y = std::min(vel.y + kWaterBuoyancy, kWaterMaxRise);
            vel.z *= kWaterDrag;
        } else if (!colliding) {
            vel.y -= kGravity;
        }

        // ── Lava pop ───────────────────────────────────────────────────────
        // `if (getFluidState(blockPosition()).is(LAVA))` — the orb skips
        // upward out of lava with a random horizontal scatter, on BOTH sides
        // (each side rolls its own random; the server's periodic sync heals
        // the divergence, exactly as in MC).
        {
            const int bx = static_cast<int>(std::floor(pos.x));
            const int by = static_cast<int>(std::floor(pos.y));
            const int bz = static_cast<int>(std::floor(pos.z));
            if (context.GetBlock(bx, by, bz) == BlockID::Lava) {
                vel = glm::dvec3(
                    static_cast<double>((rng.NextFloat() - rng.NextFloat()) * 0.2f),
                    0.2,
                    static_cast<double>((rng.NextFloat() - rng.NextFloat()) * 0.2f));
            }
        }

        // ── The pull (followNearbyPlayer) ──────────────────────────────────
        // vel += normalize(delta) * ((1 - dist/8)² * 0.1). Runs on both sides
        // — the client predicting the pull is what makes the suck smooth.
        if (followTarget) {
            const glm::dvec3 delta = *followTarget - pos;
            const double lenSq = glm::dot(delta, delta);
            if (lenSq > 1.0e-12) {
                const double power = 1.0 - std::sqrt(lenSq) / kMaxFollowDist;
                if (power > 0.0) {
                    vel += (delta / std::sqrt(lenSq)) * (power * power * kFollowAccel);
                }
            }
        }

        // ── Stuck-in-block escape (server only, and only while unclaimed) ──
        // `if (followingPlayer == null && !isClientSide && colliding)` — a
        // stuck orb that would STILL collide after this tick's motion gets its
        // velocity pointed at the nearest open face.
        bool escaping = false;
        if (isServer && !followTarget && colliding) {
            AABB moved = box;
            moved.min += glm::vec3(vel);
            moved.max += glm::vec3(vel);
            if (CollidesAt(moved, context)) {
                const glm::dvec3 center(pos.x, pos.y + kHeight * 0.5, pos.z);
                const float speed = rng.NextFloat() * 0.2f + 0.1f;
                EscapeTowardsClosestSpace(center, speed, vel, context);
                needsSync = true;
                escaping = true;
            }
        }

        // ── Move ───────────────────────────────────────────────────────────
        const double fallSpeed = vel.y;
        bool landed = false;
        if (colliding || escaping) {
            // Already overlapping a block. MC's per-axis clip ignores
            // colliders the box is inside of, so motion out of the overlap is
            // free; MoveAABB is all-or-nothing and would freeze the orb
            // instead. Integrate directly — the escape speed is well under a
            // block per tick, so this cannot tunnel anywhere MC couldn't.
            pos += vel;
            onGround = false;
        } else {
            const MoveResult move = MoveAABB(pos, vel, HalfExtents(), context);
            onGround = move.onGround;
            landed = move.collidedY && fallSpeed < 0.0;
        }

        // ── Friction — ALL axes, unlike the item entity ────────────────────
        const double friction = onGround ? kGroundDrag : kAirDrag;
        vel *= friction;

        // ── Landing bounce ─────────────────────────────────────────────────
        // `if (verticalCollisionBelow && fallSpeed < -gravity)` — orbs bounce
        // off the floor with 40% of their impact speed, which is the springy
        // scatter of orbs bursting out of a dead mob.
        if (landed && fallSpeed < -kGravity) {
            vel.y = -fallSpeed * kBounceFactor;
        }
    }

    int ExperienceOrb::GetExperienceValue(int maxValue) {
        if (maxValue >= 2477) return 2477;
        if (maxValue >= 1237) return 1237;
        if (maxValue >= 617)  return 617;
        if (maxValue >= 307)  return 307;
        if (maxValue >= 149)  return 149;
        if (maxValue >= 73)   return 73;
        if (maxValue >= 37)   return 37;
        if (maxValue >= 17)   return 17;
        if (maxValue >= 7)    return 7;
        return maxValue >= 3 ? 3 : 1;
    }

    int ExperienceOrb::GetIcon(int value) {
        if (value >= 2477) return 10;
        if (value >= 1237) return 9;
        if (value >= 617)  return 8;
        if (value >= 307)  return 7;
        if (value >= 149)  return 6;
        if (value >= 73)   return 5;
        if (value >= 37)   return 4;
        if (value >= 17)   return 3;
        if (value >= 7)    return 2;
        return value >= 3 ? 1 : 0;
    }

} // namespace Game

// File: src/common/entity/projectile/EyeOfEnder.cpp
//
// Line references are to minecraft_code/decompiled_net/minecraft/world/entity/
// projectile/EyeOfEnder.java.

#include "common/entity/projectile/EyeOfEnder.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/world/level/WorldDrops.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"

#include <cmath>

namespace Game {

    namespace {

        // MC EyeOfEnder.updateDeltaMovement (:123) — transcribed exactly.
        //
        // The shape is worth understanding before touching it: the horizontal
        // speed is lerped toward the REMAINING horizontal distance with an
        // alpha of 0.0025, so the eye accelerates as it starts and decelerates
        // as it closes; the vertical component eases toward ±1 at 0.015 per
        // tick, which is what makes it rise first and level off. Neither
        // number is tunable without changing how the eye reads.
        glm::dvec3 UpdateDeltaMovement(const glm::dvec3& oldMovement,
                                       const glm::dvec3& position,
                                       const glm::dvec3& target) {
            const glm::dvec3 horizontalDelta{ target.x - position.x, 0.0,
                                              target.z - position.z };
            const double horizontalLength = std::sqrt(
                horizontalDelta.x * horizontalDelta.x + horizontalDelta.z * horizontalDelta.z);

            const double oldHorizontal = std::sqrt(
                oldMovement.x * oldMovement.x + oldMovement.z * oldMovement.z);

            // MC Mth.lerp(delta, start, end) — the ALPHA comes first. Reading
            // it as lerp(start, end, alpha) gives a completely different
            // trajectory and is the easy mistake here.
            double wantedSpeed = oldHorizontal + (horizontalLength - oldHorizontal) * 0.0025;
            double movementY   = oldMovement.y;
            if (horizontalLength < 1.0) {
                wantedSpeed *= 0.8;
                movementY   *= 0.8;
            }

            const double wantedMovementY =
                (position.y - oldMovement.y < target.y) ? 1.0 : -1.0;

            // Guard MC does not need: its horizontalDelta is never zero
            // because signalTo always offsets the target horizontally. Ours
            // could be if a caller signalled straight up, and the NaN that
            // produced would propagate into the position and never recover.
            if (horizontalLength <= 1.0e-7) {
                return { 0.0, movementY + (wantedMovementY - movementY) * 0.015, 0.0 };
            }

            const double scale = wantedSpeed / horizontalLength;
            return { horizontalDelta.x * scale,
                     movementY + (wantedMovementY - movementY) * 0.015,
                     horizontalDelta.z * scale };
        }

    } // namespace

    // EyeOfEnder.java:40
    void EyeOfEnder::SetItem(const ItemStack& stack) {
        if (stack.IsEmpty()) {
            m_item = ItemStack(Items::EnderEye, 1);
        } else {
            m_item = stack;
            m_item.count = 1;   // MC copyWithCount(1)
        }
    }

    // EyeOfEnder.java:71
    void EyeOfEnder::SignalTo(const glm::dvec3& target) {
        const glm::dvec3 delta = target - position;
        const double horizontalDistance = std::sqrt(delta.x * delta.x + delta.z * delta.z);

        if (horizontalDistance > kTooFarDistance) {
            // The clamp. See the header — this is why the eye only ever flies
            // 12 blocks out and 8 up, whatever the stronghold's real distance.
            m_target = position + glm::dvec3(delta.x / horizontalDistance * kTooFarDistance,
                                             kTooFarSignalHeight,
                                             delta.z / horizontalDistance * kTooFarDistance);
        } else {
            m_target = target;
        }

        m_life = 0;
        // MC random.nextInt(5) > 0 — survives 4 times in 5.
        m_surviveAfterDeath = (Level() && Level()->Random().NextInt(5) > 0);
    }

    // EyeOfEnder.java:84
    void EyeOfEnder::Tick() {
        BaseTick();

        // ORDER MATTERS and is MC's: the new position is computed from the
        // CURRENT velocity, then the velocity is recomputed for next tick,
        // then the position is committed. Recomputing velocity first would
        // make the eye respond one tick early and visibly overshoot.
        const glm::dvec3 newPosition = position + velocity;

        if (m_target) {
            velocity = UpdateDeltaMovement(velocity, newPosition, *m_target);
        }
        position = newPosition;

        // MC guards the whole death block with `!level().isClientSide()`.
        // Without it the client's own copy would drop a second item entity
        // that the server never created.
        if (Level() && Level()->IsClientSide()) return;

        ++m_life;
        if (m_life > kLifetimeTicks) {
            // MC EyeOfEnder.java:99-107 — exactly ONE of the two outcomes, and
            // the item drop is what makes an eye reusable four times in five.
            m_pendingDeath = m_surviveAfterDeath ? Death::DropItem : Death::Shatter;
            if (m_pendingDeath == Death::DropItem) {
                // Reuses the same common→server bridge block loot goes
                // through, so the eye needs no hook of its own in the mob
                // removal path — where it would otherwise have to be noticed
                // between being discarded and being erased.
                DropItemStackNear(m_level ? m_level->Dimension()
                                          : DimensionId::Overworld,
                                  BlockPosition(), m_item);
            }
            // MC's shatter is levelEvent 2003 (a particle + sound burst).
            // There is no level-event packet, so a shattered eye simply
            // vanishes; the 4-in-5 drop is the half that matters for play.
            Discard();
        }
    }

    EyeOfEnder::Death EyeOfEnder::ConsumeDeath() {
        const Death d = m_pendingDeath;
        m_pendingDeath = Death::None;
        return d;
    }

} // namespace Game

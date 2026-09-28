// File: src/common/entity/ImpulseContext.hpp
//
// MC LivingEntity's "current impulse" fields — currentImpulseImpactPos and
// currentImpulseContextResetGraceTime — and the handful of methods that read
// and write them (setIgnoreFallDamageFromCurrentImpulse,
// applyPostImpulseGraceTime, isIgnoringFallDamageFromCurrentImpulse,
// tryResetCurrentImpulseContext, resetCurrentImpulseContext).
//
// This is how vanilla keeps a launch from killing you on the way down: a
// player blown up by their own wind charge, or bounced by a mace smash,
// records WHERE the impulse happened, and the landing's fall damage is then
// capped at the height fallen below that point
// (LivingEntity.causeFallDamage: min(fallDistance, impactPos.y - y)). Fall
// back to where you took off and the fall costs nothing; fall further and
// only the extra counts.
//
// The grace time is what stops the context being thrown away on the very
// tick of the launch (the player is still on the ground then). It counts
// down once a tick (LivingEntity.tick) and tryReset only clears the context
// once it has run out.
//
// Held in a small struct rather than as loose LivingEntity fields because a
// PLAYER's copy must outlive the per-level entity view the mob code sees
// (Server::PlayerEntityView is rebuilt on every dimension change); the view
// hands out the ServerPlayer's instance through
// LivingEntity::GetImpulseContext.
//
// MC also keeps ServerPlayer.spawnExtraParticlesOnFall — the mace's "throw
// up a cloud of the block you land on" flag — which lives on the same
// object here for the same reason (it must survive until the landing).
#pragma once

#include <glm/glm.hpp>

namespace Game {

    struct ImpulseContext {
        // MC currentImpulseContextResetGraceTime.
        int        graceTime = 0;
        // MC currentImpulseImpactPos (@Nullable Vec3).
        bool       hasImpactPos = false;
        glm::dvec3 impactPos{0.0};

        // MC ServerPlayer.spawnExtraParticlesOnFall (players only).
        bool       spawnExtraParticlesOnFall = false;

        // MC setIgnoreFallDamageFromCurrentImpulse.
        void SetIgnoreFallDamage(bool ignoreFallDamage, const glm::dvec3& newImpactPos) {
            if (ignoreFallDamage) {
                ApplyPostImpulseGraceTime(40);
                impactPos = newImpactPos;
                hasImpactPos = true;
            } else {
                graceTime = 0;
            }
        }

        // MC applyPostImpulseGraceTime.
        void ApplyPostImpulseGraceTime(int ticks) {
            graceTime = graceTime > ticks ? graceTime : ticks;
        }

        // MC isIgnoringFallDamageFromCurrentImpulse.
        bool IsIgnoringFallDamage() const { return hasImpactPos; }

        // MC isInPostImpulseGraceTime.
        bool IsInPostImpulseGraceTime() const { return graceTime > 0; }

        // MC tryResetCurrentImpulseContext.
        void TryReset() {
            if (graceTime == 0) Reset();
        }

        // MC resetCurrentImpulseContext.
        void Reset() {
            graceTime = 0;
            hasImpactPos = false;
            impactPos = glm::dvec3(0.0);
        }

        // MC LivingEntity.tick: `if (currentImpulseContextResetGraceTime > 0)
        // --currentImpulseContextResetGraceTime`.
        void Tick() {
            if (graceTime > 0) --graceTime;
        }

        // MC LivingEntity.causeFallDamage's opening: the fall distance the
        // landing actually pays for, resetting (or trying to reset) the
        // context on the way.
        double EffectiveFallDistance(double fallDistance, double landingY) {
            if (!hasImpactPos) return fallDistance;
            const double below = impactPos.y - landingY;
            const double effective = fallDistance < below ? fallDistance : below;
            if (effective <= 0.0) Reset();
            else                  TryReset();
            return effective;
        }
    };

} // namespace Game

// File: src/common/entity/projectile/ShulkerBullet.hpp
//
// MC net.minecraft.world.entity.projectile.ShulkerBullet — the homing bullet
// with the axis-flip steering: it always travels toward the target along ONE
// axis at a time, re-choosing an axis (never the one it just used) every
// 10-50 ticks or when it lines up with the target on the current axis. That
// stair-step flight is the whole character of the projectile and is ported
// exactly. Same Mob-pipeline deviation as Arrow.hpp documents.
//
// Deviations, each also marked at its site:
//   * MC uses loadedAndEntityCanStandOn for the "wall ahead" re-steer probe
//     and isEmptyBlock for the option filter — both approximated with the
//     collision table;
//   * no save/load (projectiles are not persisted in this engine).
#pragma once

#include "common/entity/projectile/Projectile.hpp"

namespace Game {

    class ShulkerBullet : public Projectile {
    public:
        explicit ShulkerBullet(EntityLevel* level)
            : Projectile(EntityTypeId::ShulkerBullet, level) {}

        // MC ShulkerBullet(level, owner, target, axis): spawn at the owner's
        // box centre, first steer excluding the shulker's attach axis
        // (0 = X, 1 = Y, 2 = Z, -1 = none).
        void InitShot(LivingEntity& owner, LivingEntity& target, int excludeAxis);

        static constexpr double kSpeed = 0.15;

        void Tick() override;

        // MC: the bullet CAN be shot down — any hit pops it.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

        // MC ShulkerBullet.checkDespawn: gone on peaceful.
        void CheckDespawn() override;

        bool IsOnFire() const override { return false; }

        void ClearReferenceTo(const Entity* entity) override {
            Projectile::ClearReferenceTo(entity);
            if (m_finalTarget == entity) m_finalTarget = nullptr;
        }

    protected:
        // MC canHitEntity: also never an entity with noPhysics — the only
        // such entities here are other projectiles.
        bool CanHitEntity(const Entity& entity) const override;

        void OnHitEntity(LivingEntity& target, const HitResult& hit) override;
        void OnHit(const HitResult& hit) override;

    public:
        // Save/load: MC's "Dir"/"Steps"/"TXD"/"TYD"/"TZD". "Target" is NOT
        // round-tripped — m_finalTarget is a raw pointer with no identity
        // behind it, and vanilla itself drops a target whose UUID no longer
        // resolves. A loaded bullet therefore flies its current leg out and
        // expires, which is what an unresolvable target does in vanilla too.
        int    GetMoveDirection() const { return m_currentMoveDirection; }
        void   SetMoveDirection(int dir) { m_currentMoveDirection = dir; }
        int    GetFlightSteps() const { return m_flightSteps; }
        void   SetFlightSteps(int steps) { m_flightSteps = steps; }
        glm::dvec3 GetTargetDelta() const {
            return glm::dvec3(m_targetDeltaX, m_targetDeltaY, m_targetDeltaZ);
        }
        void SetTargetDelta(const glm::dvec3& d) {
            m_targetDeltaX = d.x; m_targetDeltaY = d.y; m_targetDeltaZ = d.z;
        }

    private:
        void SelectNextMoveDirection(int avoidAxis);
        void Destroy();

        LivingEntity* m_finalTarget = nullptr;
        int    m_currentMoveDirection = -1;   // MC Direction ordinal, -1 = none
        int    m_flightSteps = 0;
        double m_targetDeltaX = 0.0;
        double m_targetDeltaY = 0.0;
        double m_targetDeltaZ = 0.0;
    };

} // namespace Game

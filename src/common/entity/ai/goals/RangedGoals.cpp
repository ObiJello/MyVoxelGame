// File: src/common/entity/ai/goals/RangedGoals.cpp
#include "common/entity/ai/goals/RangedGoals.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/RangedAttackMob.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/Sensing.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/entity/projectile/HurtingProjectile.hpp"
#include "common/entity/projectile/ShulkerBullet.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/block/BlockRegistry.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    // ── RangedAttackGoal ───────────────────────────────────────────────────

    RangedAttackGoal::RangedAttackGoal(Mob* mob, RangedAttackMob* shooter,
                                       double speedModifier, int attackIntervalMin,
                                       int attackIntervalMax, float attackRadius)
        : m_mob(mob), m_shooter(shooter), m_speedModifier(speedModifier),
          m_attackIntervalMin(attackIntervalMin),
          m_attackIntervalMax(attackIntervalMax),
          m_attackRadius(attackRadius),
          m_attackRadiusSqr(attackRadius * attackRadius) {
        SetFlags(GoalFlag::Move | GoalFlag::Look);
    }

    bool RangedAttackGoal::CanUse() {
        LivingEntity* target = m_mob->GetTarget();
        if (target && target->IsAlive()) {
            m_target = target;
            return true;
        }
        return false;
    }

    bool RangedAttackGoal::CanContinueToUse() {
        return CanUse() ||
               (m_target && m_target->IsAlive() &&
                !m_mob->GetNavigation().IsDone());
    }

    void RangedAttackGoal::Stop() {
        m_target = nullptr;
        m_seeTime = 0;
        m_attackTime = -1;
    }

    void RangedAttackGoal::ClearReferenceTo(const Entity* entity) {
        if (m_target == entity) m_target = nullptr;
    }

    void RangedAttackGoal::Tick() {
        if (!m_target) return;

        const double targetDistSqr = m_mob->DistanceToSqr(
            m_target->position.x, m_target->position.y, m_target->position.z);
        const bool hasLineOfSight = m_mob->GetSensing().HasLineOfSight(*m_target);
        if (hasLineOfSight) {
            ++m_seeTime;
        } else {
            m_seeTime = 0;
        }

        if (targetDistSqr <= static_cast<double>(m_attackRadiusSqr) &&
            m_seeTime >= 5) {
            m_mob->GetNavigation().Stop();
        } else {
            m_mob->GetNavigation().MoveTo(*m_target, m_speedModifier);
        }

        m_mob->GetLookControl().SetLookAt(m_target->position.x,
                                          m_target->GetEyeY(),
                                          m_target->position.z, 30.0f, 30.0f);

        if (--m_attackTime == 0) {
            if (!hasLineOfSight) return;

            const float dist =
                static_cast<float>(std::sqrt(targetDistSqr)) / m_attackRadius;
            const float power = std::clamp(dist, 0.1f, 1.0f);
            m_shooter->PerformRangedAttack(*m_target, power);
            m_attackTime = static_cast<int>(std::floor(
                dist * static_cast<float>(m_attackIntervalMax - m_attackIntervalMin) +
                static_cast<float>(m_attackIntervalMin)));
        } else if (m_attackTime < 0) {
            m_attackTime = static_cast<int>(std::floor(Mth::Lerp(
                std::sqrt(targetDistSqr) / static_cast<double>(m_attackRadius),
                static_cast<double>(m_attackIntervalMin),
                static_cast<double>(m_attackIntervalMax))));
        }
    }

    // ── DrownedTridentAttackGoal ───────────────────────────────────────────

    DrownedTridentAttackGoal::DrownedTridentAttackGoal(Drowned* drowned,
                                                       double speedModifier,
                                                       int attackInterval,
                                                       float attackRadius)
        : RangedAttackGoal(drowned, drowned, speedModifier, attackInterval,
                           attackRadius),
          m_drowned(drowned) {}

    bool DrownedTridentAttackGoal::CanUse() {
        // MC gates on getMainHandItem().is(TRIDENT) — the FinalizeSpawn roll
        // here (see Drowned).
        return RangedAttackGoal::CanUse() && m_drowned->HasTrident();
    }

    void DrownedTridentAttackGoal::Start() {
        RangedAttackGoal::Start();
        // MC also startUsingItem (the raised-trident pose rides item use);
        // the aggressive flag is the wire bit the renderer keys on here.
        m_drowned->SetAggressive(true);
    }

    void DrownedTridentAttackGoal::Stop() {
        RangedAttackGoal::Stop();
        m_drowned->SetAggressive(false);
    }

    // ── BlazeAttackGoal ────────────────────────────────────────────────────

    BlazeAttackGoal::BlazeAttackGoal(Blaze* blaze) : m_blaze(blaze) {
        SetFlags(GoalFlag::Move | GoalFlag::Look);
    }

    bool BlazeAttackGoal::CanUse() {
        LivingEntity* target = m_blaze->GetTarget();
        return target && target->IsAlive() && m_blaze->CanAttack(*target);
    }

    void BlazeAttackGoal::Stop() {
        m_blaze->SetCharged(false);
        m_lastSeen = 0;
    }

    double BlazeAttackGoal::GetFollowDistance() const {
        return m_blaze->GetAttributeValue(Attribute::FollowRange);
    }

    void BlazeAttackGoal::Tick() {
        --m_attackTime;
        LivingEntity* target = m_blaze->GetTarget();
        if (!target) return;
        EntityLevel* level = m_blaze->Level();
        if (!level) return;

        const bool hasLineOfSight = m_blaze->GetSensing().HasLineOfSight(*target);
        if (hasLineOfSight) {
            m_lastSeen = 0;
        } else {
            ++m_lastSeen;
        }

        const double distance = m_blaze->DistanceToSqr(*target);
        if (distance < 4.0) {
            if (!hasLineOfSight) return;

            if (m_attackTime <= 0) {
                m_attackTime = 20;
                m_blaze->DoHurtTarget(*target);
            }
            m_blaze->GetMoveControl().SetWantedPosition(
                target->position.x, target->position.y, target->position.z, 1.0);
        } else if (distance < GetFollowDistance() * GetFollowDistance() &&
                   hasLineOfSight) {
            const double xd = target->position.x - m_blaze->position.x;
            const double yd =
                (target->position.y + target->GetBbHeight() * 0.5) -
                (m_blaze->position.y + m_blaze->GetBbHeight() * 0.5);
            const double zd = target->position.z - m_blaze->position.z;

            if (m_attackTime <= 0) {
                ++m_attackStep;
                if (m_attackStep == 1) {
                    m_attackTime = 60;
                    m_blaze->SetCharged(true);
                } else if (m_attackStep <= 4) {
                    m_attackTime = 6;
                } else {
                    m_attackTime = 100;
                    m_attackStep = 0;
                    m_blaze->SetCharged(false);
                }

                if (m_attackStep > 1) {
                    // MC level event 1018 (blaze shoot) — no sound system.
                    const double sqd = std::sqrt(std::sqrt(distance)) * 0.5;
                    JavaRandom& rng = level->Random();
                    glm::dvec3 direction(rng.Triangle(xd, 2.297 * sqd), yd,
                                         rng.Triangle(zd, 2.297 * sqd));
                    const double len = glm::length(direction);
                    if (len > 1.0e-9) direction /= len;

                    auto fireball = std::make_unique<SmallFireball>(level);
                    fireball->SetOwnerAndDirection(*m_blaze, direction);
                    // MC: setPos(x, blaze.getY(0.5) + 0.5, z).
                    fireball->position.y = m_blaze->position.y +
                                           m_blaze->GetBbHeight() * 0.5 + 0.5;
                    level->AddFreshEntity(std::move(fireball));
                }
            }

            m_blaze->GetLookControl().SetLookAt(target->position.x,
                                                target->GetEyeY(),
                                                target->position.z,
                                                10.0f, 10.0f);
        } else if (m_lastSeen < 5) {
            m_blaze->GetMoveControl().SetWantedPosition(
                target->position.x, target->position.y, target->position.z, 1.0);
        }
    }

    // ── RandomFloatAroundGoal ──────────────────────────────────────────────

    RandomFloatAroundGoal::RandomFloatAroundGoal(Mob* ghast) : m_ghast(ghast) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Move));
    }

    bool RandomFloatAroundGoal::CanUse() {
        MoveControl& control = m_ghast->GetMoveControl();
        if (!control.HasWanted()) return true;

        const double xd = control.GetWantedX() - m_ghast->position.x;
        const double yd = control.GetWantedY() - m_ghast->position.y;
        const double zd = control.GetWantedZ() - m_ghast->position.z;
        const double dd = xd * xd + yd * yd + zd * zd;
        return dd < 1.0 || dd > 3600.0;
    }

    void RandomFloatAroundGoal::Start() {
        const glm::dvec3 result = GetSuitableFlyToPosition(*m_ghast);
        m_ghast->GetMoveControl().SetWantedPosition(result.x, result.y,
                                                    result.z, 1.0);
    }

    glm::dvec3 RandomFloatAroundGoal::GetSuitableFlyToPosition(Mob& mob) {
        EntityLevel* level = mob.Level();
        const glm::dvec3 center = mob.position;
        if (!level) return center;
        JavaRandom& rng = level->Random();

        // MC chooseRandomPosition. With no home restriction and
        // distanceToBlocks 0, the 64-attempt loop accepts its first pick —
        // exactly the plain ghast's path through it.
        glm::dvec3 result(
            center.x + static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 16.0f),
            center.y + static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 16.0f),
            center.z + static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 16.0f));

        // MC then clamps against the MOTION_BLOCKING heightmap: when the pick
        // sits above the terrain column, mirror it BELOW the ghast instead —
        // this is what keeps ghasts near the ground instead of climbing
        // forever. No heightmap exists here, so scan the column.
        if (const IBlockAccess* blocks = level->Blocks()) {
            const int bx = static_cast<int>(std::floor(result.x));
            const int bz = static_cast<int>(std::floor(result.z));
            int heightY = -64;
            for (int y = 319; y >= -64; --y) {
                if (BlockRegistry::HasCollision(blocks->GetBlock(bx, y, bz))) {
                    heightY = y + 1;
                    break;
                }
            }
            const int by = static_cast<int>(std::floor(result.y));
            if (heightY < by && heightY > -64) {
                result.y = mob.position.y - std::abs(mob.position.y - result.y);
            }
        }
        return result;
    }

    // ── GhastLookGoal ──────────────────────────────────────────────────────

    GhastLookGoal::GhastLookGoal(Mob* ghast) : m_ghast(ghast) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Look));
    }

    void GhastLookGoal::Tick() {
        Ghast::FaceMovementDirection(*m_ghast);
    }

    // ── GhastShootFireballGoal ─────────────────────────────────────────────

    GhastShootFireballGoal::GhastShootFireballGoal(Ghast* ghast)
        : m_ghast(ghast) {}

    bool GhastShootFireballGoal::CanUse() {
        return m_ghast->GetTarget() != nullptr;
    }

    void GhastShootFireballGoal::Stop() {
        m_ghast->SetCharging(false);
    }

    void GhastShootFireballGoal::Tick() {
        LivingEntity* target = m_ghast->GetTarget();
        if (!target) return;
        EntityLevel* level = m_ghast->Level();
        if (!level) return;

        if (target->DistanceToSqr(*m_ghast) < 4096.0 &&
            m_ghast->GetSensing().HasLineOfSight(*target)) {
            ++m_chargeTime;
            // MC level events 1015 (warble, tick 10) and 1016 (shoot) — no
            // sound system.

            if (m_chargeTime == 20) {
                const glm::vec3 view =
                    Mth::ViewVector(m_ghast->xRot, m_ghast->yRot);
                const double mouthX =
                    m_ghast->position.x + static_cast<double>(view.x) * 4.0;
                const double mouthY = m_ghast->position.y +
                                      m_ghast->GetBbHeight() * 0.5 + 0.5;
                const double mouthZ =
                    m_ghast->position.z + static_cast<double>(view.z) * 4.0;

                const double xdd = target->position.x - mouthX;
                const double ydd =
                    (target->position.y + target->GetBbHeight() * 0.5) - mouthY;
                const double zdd = target->position.z - mouthZ;

                auto fireball = std::make_unique<LargeFireball>(
                    level, m_ghast->GetExplosionPower());
                fireball->SetOwnerAndDirection(*m_ghast,
                                               glm::dvec3(xdd, ydd, zdd));
                fireball->position = glm::dvec3(mouthX, mouthY, mouthZ);
                level->AddFreshEntity(std::move(fireball));

                m_chargeTime = -40;
            }
        } else if (m_chargeTime > 0) {
            --m_chargeTime;
        }

        m_ghast->SetCharging(m_chargeTime > 10);
    }

    // ── ShulkerAttackGoal ──────────────────────────────────────────────────

    ShulkerAttackGoal::ShulkerAttackGoal(Shulker* shulker) : m_shulker(shulker) {
        SetFlags(GoalFlag::Move | GoalFlag::Look);
    }

    bool ShulkerAttackGoal::CanUse() {
        LivingEntity* target = m_shulker->GetTarget();
        if (!target || !target->IsAlive()) return false;
        return !m_shulker->Level() ||
               m_shulker->Level()->GetDifficulty() != Difficulty::Peaceful;
    }

    void ShulkerAttackGoal::Start() {
        m_attackTime = 20;
        m_shulker->SetRawPeekAmount(100);
    }

    void ShulkerAttackGoal::Stop() {
        m_shulker->SetRawPeekAmount(0);
    }

    void ShulkerAttackGoal::Tick() {
        EntityLevel* level = m_shulker->Level();
        if (!level || level->GetDifficulty() == Difficulty::Peaceful) return;

        --m_attackTime;
        LivingEntity* target = m_shulker->GetTarget();
        if (!target) return;

        m_shulker->GetLookControl().SetLookAt(target->position.x,
                                              target->GetEyeY(),
                                              target->position.z,
                                              180.0f, 180.0f);

        const double distance = m_shulker->DistanceToSqr(*target);
        if (distance < 400.0) {
            if (m_attackTime <= 0) {
                m_attackTime = 20 + level->Random().NextInt(10) * 20 / 2;

                auto bullet = std::make_unique<ShulkerBullet>(level);
                bullet->InitShot(*m_shulker, *target, m_shulker->GetAttachAxis());
                level->AddFreshEntity(std::move(bullet));
            }
        } else {
            m_shulker->SetTarget(nullptr);
        }
    }

    // ── ShulkerPeekGoal ────────────────────────────────────────────────────

    ShulkerPeekGoal::ShulkerPeekGoal(Shulker* shulker) : m_shulker(shulker) {}

    bool ShulkerPeekGoal::CanUse() {
        EntityLevel* level = m_shulker->Level();
        return m_shulker->GetTarget() == nullptr && level &&
               level->Random().NextInt(ReducedTickDelay(40)) == 0 &&
               m_shulker->CanStayAt(m_shulker->BlockPosition(),
                                    m_shulker->GetAttachFace());
    }

    bool ShulkerPeekGoal::CanContinueToUse() {
        return m_shulker->GetTarget() == nullptr && m_peekTime > 0;
    }

    void ShulkerPeekGoal::Start() {
        EntityLevel* level = m_shulker->Level();
        const int roll = level ? level->Random().NextInt(3) : 0;
        m_peekTime = AdjustedTickDelay(20 * (1 + roll));
        m_shulker->SetRawPeekAmount(30);
    }

    void ShulkerPeekGoal::Stop() {
        if (m_shulker->GetTarget() == nullptr) {
            m_shulker->SetRawPeekAmount(0);
        }
    }

} // namespace Game

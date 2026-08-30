// File: src/common/entity/ai/goals/EvokerGoals.cpp
#include "common/entity/ai/goals/EvokerGoals.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/entity/projectile/EvokerFangs.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

#include <cmath>
#include <vector>

namespace Game {

    // ── SpellcasterCastingSpellGoal ────────────────────────────────────────

    SpellcasterCastingSpellGoal::SpellcasterCastingSpellGoal(
            SpellcasterIllager* caster)
        : m_caster(caster) {
        SetFlags(GoalFlag::Move | GoalFlag::Look);
    }

    bool SpellcasterCastingSpellGoal::CanUse() {
        return m_caster->GetSpellCastingTime() > 0;
    }

    void SpellcasterCastingSpellGoal::Start() {
        m_caster->GetNavigation().Stop();
    }

    void SpellcasterCastingSpellGoal::Stop() {
        m_caster->SetIsCastingSpell(SpellcasterIllager::IllagerSpell::None);
    }

    void SpellcasterCastingSpellGoal::Tick() {
        if (m_caster->GetTarget() != nullptr) {
            LivingEntity* target = m_caster->GetTarget();
            m_caster->GetLookControl().SetLookAt(
                target->position.x, target->GetEyeY(), target->position.z,
                static_cast<float>(m_caster->GetMaxHeadYRot()),
                static_cast<float>(m_caster->GetMaxHeadXRot()));
        }
    }

    // ── SpellcasterUseSpellGoal ────────────────────────────────────────────

    bool SpellcasterUseSpellGoal::CanUse() {
        LivingEntity* target = m_caster->GetTarget();
        if (target == nullptr || !target->IsAlive()) return false;
        if (m_caster->IsCastingSpell()) return false;
        return m_caster->tickCount >= m_nextAttackTickCount;
    }

    bool SpellcasterUseSpellGoal::CanContinueToUse() {
        LivingEntity* target = m_caster->GetTarget();
        return target != nullptr && target->IsAlive() && m_attackWarmupDelay > 0;
    }

    void SpellcasterUseSpellGoal::Start() {
        m_attackWarmupDelay = AdjustedTickDelay(GetCastWarmupTime());
        m_caster->SetSpellCastingTime(GetCastingTime());
        m_nextAttackTickCount = m_caster->tickCount + GetCastingInterval();
        // MC plays the spell's prepare sound here — no sound system.
        m_caster->SetIsCastingSpell(GetSpell());
    }

    void SpellcasterUseSpellGoal::Tick() {
        --m_attackWarmupDelay;
        if (m_attackWarmupDelay == 0) {
            PerformSpellCasting();
            // MC plays getCastingSoundEvent here — no sound system.
        }
    }

    // ── EvokerCastingSpellGoal ─────────────────────────────────────────────

    EvokerCastingSpellGoal::EvokerCastingSpellGoal(Evoker* evoker)
        : SpellcasterCastingSpellGoal(evoker), m_evoker(evoker) {}

    void EvokerCastingSpellGoal::Tick() {
        if (m_evoker->GetTarget() != nullptr) {
            SpellcasterCastingSpellGoal::Tick();
        } else if (m_evoker->GetWololoTarget() != nullptr) {
            LivingEntity* sheep = m_evoker->GetWololoTarget();
            m_evoker->GetLookControl().SetLookAt(
                sheep->position.x, sheep->GetEyeY(), sheep->position.z,
                static_cast<float>(m_evoker->GetMaxHeadYRot()),
                static_cast<float>(m_evoker->GetMaxHeadXRot()));
        }
    }

    // ── EvokerAttackSpellGoal ──────────────────────────────────────────────

    void EvokerAttackSpellGoal::PerformSpellCasting() {
        LivingEntity* target = m_evoker->GetTarget();
        if (target == nullptr) return;

        const double minY = std::min(target->position.y, m_evoker->position.y);
        const double maxY =
            std::max(target->position.y, m_evoker->position.y) + 1.0;
        const float angleTowardsTarget = static_cast<float>(
            std::atan2(target->position.z - m_evoker->position.z,
                       target->position.x - m_evoker->position.x));

        if (m_evoker->DistanceToSqr(*target) < 9.0) {
            // MC: two close-range rings — 5 fangs at radius 1.5 (no delay),
            // 8 at radius 2.5 (delay 3), each offset 1.2566371 rad.
            for (int i = 0; i < 5; ++i) {
                const float angle =
                    angleTowardsTarget + static_cast<float>(i) * Mth::kPi * 0.4f;
                CreateSpellEntity(
                    m_evoker->position.x +
                        static_cast<double>(std::cos(angle)) * 1.5,
                    m_evoker->position.z +
                        static_cast<double>(std::sin(angle)) * 1.5,
                    minY, maxY, angle, 0);
            }
            for (int i = 0; i < 8; ++i) {
                const float angle = angleTowardsTarget +
                                    static_cast<float>(i) * Mth::kPi * 2.0f / 8.0f +
                                    1.2566371f;
                CreateSpellEntity(
                    m_evoker->position.x +
                        static_cast<double>(std::cos(angle)) * 2.5,
                    m_evoker->position.z +
                        static_cast<double>(std::sin(angle)) * 2.5,
                    minY, maxY, angle, 3);
            }
        } else {
            // MC: the 16-fang line, 1.25 blocks and 1 delay tick per step.
            for (int i = 0; i < 16; ++i) {
                const double reach = 1.25 * static_cast<double>(i + 1);
                CreateSpellEntity(
                    m_evoker->position.x +
                        static_cast<double>(std::cos(angleTowardsTarget)) * reach,
                    m_evoker->position.z +
                        static_cast<double>(std::sin(angleTowardsTarget)) * reach,
                    minY, maxY, angleTowardsTarget, i);
            }
        }
    }

    void EvokerAttackSpellGoal::CreateSpellEntity(double x, double z, double minY,
                                                  double maxY, float angle,
                                                  int delayTicks) {
        EntityLevel* level = m_evoker->Level();
        if (!level || !level->Blocks()) return;
        const IBlockAccess& blocks = *level->Blocks();

        // MC: walk down from maxY looking for a top face sturdy enough to
        // stand fangs on; a collision block below with a free (or partial)
        // cell above it. The collision-shape max-Y read is approximated to a
        // full block — near-universal here, where shaped blocks are rare.
        glm::ivec3 pos(static_cast<int>(std::floor(x)),
                       static_cast<int>(std::floor(maxY)),
                       static_cast<int>(std::floor(z)));
        bool success = false;
        double topOffset = 0.0;

        do {
            const glm::ivec3 below(pos.x, pos.y - 1, pos.z);
            if (BlockRegistry::HasCollision(
                    blocks.GetBlock(below.x, below.y, below.z))) {
                if (blocks.GetBlock(pos.x, pos.y, pos.z) != BlockID::Air &&
                    BlockRegistry::HasCollision(
                        blocks.GetBlock(pos.x, pos.y, pos.z))) {
                    topOffset = 1.0;
                }
                success = true;
                break;
            }
            --pos.y;
        } while (pos.y >= static_cast<int>(std::floor(minY)) - 1);

        if (success) {
            auto fangs = std::make_unique<EvokerFangs>(level);
            fangs->Init(x, static_cast<double>(pos.y) + topOffset, z, angle,
                        delayTicks, m_evoker);
            level->AddFreshEntity(std::move(fangs));
        }
    }

    // ── EvokerSummonSpellGoal ──────────────────────────────────────────────

    bool EvokerSummonSpellGoal::CanUse() {
        if (!SpellcasterUseSpellGoal::CanUse()) return false;
        EntityLevel* level = m_evoker->Level();
        if (!level) return false;

        // MC: count vexes within 16 blocks (line of sight ignored); summon
        // only while rand(8) + 1 exceeds the count.
        AABB box = m_evoker->GetAABB();
        box.min -= glm::vec3(16.0f);
        box.max += glm::vec3(16.0f);
        std::vector<Entity*> nearby;
        level->GetEntitiesInBox(box, m_evoker, nearby);
        int vexes = 0;
        for (const Entity* e : nearby) {
            if (e->GetType() == EntityTypeId::Vex && e->IsAlive()) ++vexes;
        }
        return level->Random().NextInt(8) + 1 > vexes;
    }

    void EvokerSummonSpellGoal::PerformSpellCasting() {
        EntityLevel* level = m_evoker->Level();
        if (!level) return;
        JavaRandom& rng = level->Random();

        for (int i = 0; i < 3; ++i) {
            const glm::ivec3 pos =
                m_evoker->BlockPosition() +
                glm::ivec3(-2 + rng.NextInt(5), 1, -2 + rng.NextInt(5));

            auto vex = std::make_unique<Vex>(level);
            vex->position = glm::dvec3(pos.x + 0.5, pos.y, pos.z + 0.5);
            vex->yRot = vex->yBodyRot = vex->yHeadRot = 0.0f;
            vex->FinalizeSpawn(SpawnReason::MobSummoned, nullptr);
            vex->SetVexOwner(m_evoker);
            vex->SetBoundOrigin(pos);
            // MC: 20 * (30 + rand(90)) ticks — 30 s to 2 min of life.
            vex->SetLimitedLife(20 * (30 + rng.NextInt(90)));
            // MC also copies the evoker's scoreboard team — no team system.
            level->AddFreshEntity(std::move(vex));
        }
    }

    // ── EvokerWololoSpellGoal ──────────────────────────────────────────────

    bool EvokerWololoSpellGoal::CanUse() {
        if (m_evoker->GetTarget() != nullptr) return false;
        if (m_evoker->IsCastingSpell()) return false;
        if (m_evoker->tickCount < m_nextAttackTickCount) return false;
        // MC gates on the mobGriefing game rule — no game rules; ON (the
        // vanilla default).
        EntityLevel* level = m_evoker->Level();
        if (!level) return false;

        // MC wololoTargeting: forNonCombat().range(16), selector = the
        // sheep's colour is BLUE (dye id 11); search box inflated (16,4,16).
        AABB box = m_evoker->GetAABB();
        box.min -= glm::vec3(16.0f, 4.0f, 16.0f);
        box.max += glm::vec3(16.0f, 4.0f, 16.0f);
        std::vector<Entity*> nearby;
        level->GetEntitiesInBox(box, m_evoker, nearby);
        std::vector<Sheep*> blueSheep;
        for (Entity* e : nearby) {
            if (e->GetType() != EntityTypeId::Sheep || !e->IsAlive()) continue;
            auto* sheep = dynamic_cast<Sheep*>(e);
            if (sheep && sheep->GetColor() == 11) {
                blueSheep.push_back(sheep);
            }
        }
        if (blueSheep.empty()) return false;

        m_evoker->SetWololoTarget(
            blueSheep[level->Random().NextInt(static_cast<int>(blueSheep.size()))]);
        return true;
    }

    bool EvokerWololoSpellGoal::CanContinueToUse() {
        return m_evoker->GetWololoTarget() != nullptr && m_attackWarmupDelay > 0;
    }

    void EvokerWololoSpellGoal::Stop() {
        SpellcasterUseSpellGoal::Stop();
        m_evoker->SetWololoTarget(nullptr);
    }

    void EvokerWololoSpellGoal::PerformSpellCasting() {
        Sheep* sheep = m_evoker->GetWololoTarget();
        if (sheep != nullptr && sheep->IsAlive()) {
            sheep->SetColor(14);   // MC DyeColor.RED
        }
    }

    // ── VexMoveControl ─────────────────────────────────────────────────────

    VexMoveControl::VexMoveControl(Vex* vex) : MoveControl(vex), m_vex(vex) {}

    void VexMoveControl::Tick() {
        if (m_operation != Operation::MoveTo) return;

        const glm::dvec3 delta(m_wantedX - m_vex->position.x,
                               m_wantedY - m_vex->position.y,
                               m_wantedZ - m_vex->position.z);
        const double deltaLength = glm::length(delta);

        // MC: getBoundingBox().getSize() — the box's average edge length.
        const AABB box = m_vex->GetAABB();
        const glm::vec3 size3 = box.max - box.min;
        const double boxSize =
            (static_cast<double>(size3.x) + size3.y + size3.z) / 3.0;

        if (deltaLength < boxSize) {
            m_operation = Operation::Wait;
            m_vex->velocity *= 0.5;
        } else {
            m_vex->velocity += delta * (m_speedModifier * 0.05 / deltaLength);
            if (m_vex->GetTarget() == nullptr) {
                m_vex->yRot = -static_cast<float>(
                                  std::atan2(m_vex->velocity.x,
                                             m_vex->velocity.z)) *
                              Mth::kRadToDeg;
                m_vex->yBodyRot = m_vex->yRot;
            } else {
                const double tx =
                    m_vex->GetTarget()->position.x - m_vex->position.x;
                const double tz =
                    m_vex->GetTarget()->position.z - m_vex->position.z;
                m_vex->yRot =
                    -static_cast<float>(std::atan2(tx, tz)) * Mth::kRadToDeg;
                m_vex->yBodyRot = m_vex->yRot;
            }
        }
        m_vex->needsSync = true;
    }

    // ── VexChargeAttackGoal ────────────────────────────────────────────────

    VexChargeAttackGoal::VexChargeAttackGoal(Vex* vex) : m_vex(vex) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Move));
    }

    bool VexChargeAttackGoal::CanUse() {
        LivingEntity* target = m_vex->GetTarget();
        if (target != nullptr && target->IsAlive() &&
            !m_vex->GetMoveControl().HasWanted() && m_vex->Level() &&
            m_vex->Level()->Random().NextInt(ReducedTickDelay(7)) == 0) {
            return m_vex->DistanceToSqr(*target) > 4.0;
        }
        return false;
    }

    bool VexChargeAttackGoal::CanContinueToUse() {
        return m_vex->GetMoveControl().HasWanted() && m_vex->IsCharging() &&
               m_vex->GetTarget() != nullptr && m_vex->GetTarget()->IsAlive();
    }

    void VexChargeAttackGoal::Start() {
        LivingEntity* target = m_vex->GetTarget();
        if (target != nullptr) {
            const glm::dvec3 eye = target->GetEyePosition();
            m_vex->GetMoveControl().SetWantedPosition(eye.x, eye.y, eye.z, 1.0);
        }
        m_vex->SetIsCharging(true);
        // MC plays VEX_CHARGE — no sound system.
    }

    void VexChargeAttackGoal::Stop() {
        m_vex->SetIsCharging(false);
    }

    void VexChargeAttackGoal::Tick() {
        LivingEntity* target = m_vex->GetTarget();
        if (target == nullptr) return;

        if (m_vex->GetAABB().Intersects(target->GetAABB())) {
            m_vex->DoHurtTarget(*target);
            m_vex->SetIsCharging(false);
        } else if (m_vex->DistanceToSqr(*target) < 9.0) {
            const glm::dvec3 eye = target->GetEyePosition();
            m_vex->GetMoveControl().SetWantedPosition(eye.x, eye.y, eye.z, 1.0);
        }
    }

    // ── VexRandomMoveGoal ──────────────────────────────────────────────────

    VexRandomMoveGoal::VexRandomMoveGoal(Vex* vex) : m_vex(vex) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Move));
    }

    bool VexRandomMoveGoal::CanUse() {
        return !m_vex->GetMoveControl().HasWanted() && m_vex->Level() &&
               m_vex->Level()->Random().NextInt(ReducedTickDelay(7)) == 0;
    }

    void VexRandomMoveGoal::Tick() {
        EntityLevel* level = m_vex->Level();
        if (!level || !level->Blocks()) return;
        JavaRandom& rng = level->Random();

        glm::ivec3 boundOrigin = m_vex->HasBoundOrigin()
                                     ? m_vex->GetBoundOrigin()
                                     : m_vex->BlockPosition();

        for (int attempts = 0; attempts < 3; ++attempts) {
            const glm::ivec3 testPos =
                boundOrigin + glm::ivec3(rng.NextInt(15) - 7, rng.NextInt(11) - 5,
                                         rng.NextInt(15) - 7);
            if (level->Blocks()->GetBlock(testPos.x, testPos.y, testPos.z) ==
                BlockID::Air) {
                m_vex->GetMoveControl().SetWantedPosition(
                    testPos.x + 0.5, testPos.y + 0.5, testPos.z + 0.5, 0.25);
                if (m_vex->GetTarget() == nullptr) {
                    m_vex->GetLookControl().SetLookAt(
                        testPos.x + 0.5, testPos.y + 0.5, testPos.z + 0.5,
                        180.0f, 20.0f);
                }
                break;
            }
        }
    }

    // ── VexCopyOwnerTargetGoal ─────────────────────────────────────────────

    VexCopyOwnerTargetGoal::VexCopyOwnerTargetGoal(Vex* vex)
        : TargetGoal(vex, /*mustSee=*/false), m_vex(vex) {
        // MC: forNonCombat().ignoreLineOfSight().ignoreInvisibilityTesting().
        m_copyOwnerTargeting = TargetingConditions::ForNonCombat();
    }

    bool VexCopyOwnerTargetGoal::CanUse() {
        Mob* owner = m_vex->GetVexOwner();
        return owner != nullptr && owner->GetTarget() != nullptr &&
               m_vex->CanAttack(*owner->GetTarget()) &&
               m_copyOwnerTargeting.Test(m_vex, *owner->GetTarget());
    }

    void VexCopyOwnerTargetGoal::Start() {
        Mob* owner = m_vex->GetVexOwner();
        LivingEntity* target =
            owner != nullptr ? owner->GetTarget() : nullptr;
        m_vex->SetTarget(target);
        m_targetMob = target;
        TargetGoal::Start();
    }

} // namespace Game

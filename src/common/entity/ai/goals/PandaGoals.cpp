// File: src/common/entity/ai/goals/PandaGoals.cpp
#include "common/entity/ai/goals/PandaGoals.hpp"

#include "common/entity/mobs/Animals.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/entity/ai/TargetingConditions.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

#include <cmath>

namespace Game {

    namespace {

        // ── MC Panda.PandaMoveControl ──────────────────────────────────────

        class PandaMoveControl : public MoveControl {
        public:
            explicit PandaMoveControl(Panda* panda)
                : MoveControl(panda), m_panda(panda) {}

            void Tick() override {
                // MC: a sitting/rolling/scared/on-back panda ignores steering.
                if (m_panda->CanPerformAction()) MoveControl::Tick();
            }

        private:
            Panda* m_panda;
        };

    } // namespace

    std::unique_ptr<MoveControl> MakePandaMoveControl(Panda* panda) {
        return std::make_unique<PandaMoveControl>(panda);
    }

    // ── PandaPanicGoal ─────────────────────────────────────────────────────

    PandaPanicGoal::PandaPanicGoal(Panda* panda, double speedModifier)
        : PanicGoal(panda, speedModifier), m_panda(panda) {}

    bool PandaPanicGoal::ShouldPanic() const {
        // MC passes DamageTypeTags.PANIC_ENVIRONMENTAL_CAUSES: only damage
        // with no attacking mob behind it sends a panda running (the polar
        // bear adult uses the same mapping).
        return m_panda->HasLastDamageSource()
            && m_panda->GetLastHurtByMob() == nullptr;
    }

    bool PandaPanicGoal::CanContinueToUse() {
        if (m_panda->IsSitting()) {
            m_panda->GetNavigation().Stop();
            return false;
        }
        return PanicGoal::CanContinueToUse();
    }

    // ── PandaBreedGoal ─────────────────────────────────────────────────────

    PandaBreedGoal::PandaBreedGoal(Panda* panda, double speedModifier)
        : BreedGoal(panda, speedModifier), m_panda(panda) {}

    bool PandaBreedGoal::CanUse() {
        if (!BreedGoal::CanUse() || m_panda->GetUnhappyCounter() != 0) {
            return false;
        }
        if (!CanFindBamboo()) {
            if (m_unhappyCooldown <= m_panda->tickCount) {
                m_panda->SetUnhappyCounter(32);
                m_unhappyCooldown = m_panda->tickCount + 600;
                if (m_panda->IsEffectiveAi()) {
                    // MC: glare at the nearest player through the look goal.
                    // The goal pointer is wired by Panda::RegisterGoals.
                    EntityLevel* level = m_panda->Level();
                    if (level && m_panda->lookAtPlayerGoal) {
                        LivingEntity* player = level->GetNearestPlayer(
                            m_panda->position.x, m_panda->GetEyeY(),
                            m_panda->position.z, 8.0);
                        m_panda->lookAtPlayerGoal->SetTarget(player);
                    }
                }
            }
            return false;
        }
        return true;
    }

    bool PandaBreedGoal::CanFindBamboo() const {
        // MC canFindBamboo — an expanding square scan, 8 blocks out and 3 up,
        // for any BAMBOO block. The exact spiral order does not matter for a
        // boolean answer; the volume does.
        EntityLevel* level = m_panda->Level();
        const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
        if (!blocks) return false;
        const glm::ivec3 origin = m_panda->BlockPosition();
        for (int yOff = 0; yOff < 3; ++yOff) {
            for (int x = -8; x <= 8; ++x) {
                for (int z = -8; z <= 8; ++z) {
                    if (blocks->GetBlock(origin.x + x, origin.y + yOff,
                                         origin.z + z) == BlockID::Bamboo) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // ── PandaAttackGoal ────────────────────────────────────────────────────

    PandaAttackGoal::PandaAttackGoal(Panda* panda, double speedModifier,
                                     bool trackTarget)
        : MeleeAttackGoal(panda, speedModifier, trackTarget), m_panda(panda) {}

    bool PandaAttackGoal::CanUse() {
        return m_panda->CanPerformAction() && MeleeAttackGoal::CanUse();
    }

    // ── PandaAvoidGoal ─────────────────────────────────────────────────────

    PandaAvoidGoal::PandaAvoidGoal(Panda* panda, float maxDistance,
                                   double walkSpeedModifier,
                                   double sprintSpeedModifier)
        : AvoidEntityGoal(panda, maxDistance, walkSpeedModifier,
                          sprintSpeedModifier),
          m_panda(panda) {}

    PandaAvoidGoal::PandaAvoidGoal(Panda* panda, const EntityTypeId* types,
                                   int typeCount, float maxDistance,
                                   double walkSpeedModifier,
                                   double sprintSpeedModifier)
        : AvoidEntityGoal(panda, types, typeCount, maxDistance,
                          walkSpeedModifier, sprintSpeedModifier),
          m_panda(panda) {}

    bool PandaAvoidGoal::CanUse() {
        return m_panda->IsWorried() && m_panda->CanPerformAction()
            && AvoidEntityGoal::CanUse();
    }

    // ── PandaLieOnBackGoal ─────────────────────────────────────────────────

    PandaLieOnBackGoal::PandaLieOnBackGoal(Panda* panda) : m_panda(panda) {}

    bool PandaLieOnBackGoal::CanUse() {
        return m_cooldown < m_panda->tickCount && m_panda->IsLazy()
            && m_panda->CanPerformAction()
            && m_panda->Level()->Random().NextInt(ReducedTickDelay(400)) == 1;
    }

    bool PandaLieOnBackGoal::CanContinueToUse() {
        if (m_panda->IsInWater()) return false;
        JavaRandom& random = m_panda->Level()->Random();
        if (!m_panda->IsLazy()
            && random.NextInt(ReducedTickDelay(600)) == 1) {
            return false;
        }
        return random.NextInt(ReducedTickDelay(2000)) != 1;
    }

    void PandaLieOnBackGoal::Start() {
        m_panda->SetOnBack(true);
        m_cooldown = 0;
    }

    void PandaLieOnBackGoal::Stop() {
        m_panda->SetOnBack(false);
        m_cooldown = m_panda->tickCount + 200;
    }

    // ── PandaSneezeGoal ────────────────────────────────────────────────────

    bool PandaSneezeGoal::CanUse() {
        if (!m_panda->IsBaby() || !m_panda->CanPerformAction()) return false;
        JavaRandom& random = m_panda->Level()->Random();
        if (m_panda->IsWeak()
            && random.NextInt(ReducedTickDelay(500)) == 1) {
            return true;
        }
        return random.NextInt(ReducedTickDelay(6000)) == 1;
    }

    void PandaSneezeGoal::Start() { m_panda->Sneeze(true); }

    // ── PandaRollGoal ──────────────────────────────────────────────────────

    PandaRollGoal::PandaRollGoal(Panda* panda) : m_panda(panda) {
        SetFlags(GoalFlag::Move | GoalFlag::Look | GoalFlag::Jump);
    }

    bool PandaRollGoal::CanUse() {
        if ((!m_panda->IsBaby() && !m_panda->IsPlayful()) || !m_panda->onGround) {
            return false;
        }
        if (!m_panda->CanPerformAction()) return false;

        // MC: roll for sure when facing a ledge (air one step ahead-below).
        const float angle = m_panda->yRot * Mth::kDegToRad;
        const float xDir = -std::sin(angle);
        const float zDir = std::cos(angle);
        const int xStep = std::abs(xDir) > 0.5f ? (xDir > 0.0f ? 1 : -1) : 0;
        const int zStep = std::abs(zDir) > 0.5f ? (zDir > 0.0f ? 1 : -1) : 0;

        EntityLevel* level = m_panda->Level();
        const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
        const glm::ivec3 pos = m_panda->BlockPosition();
        if (blocks && blocks->GetBlock(pos.x + xStep, pos.y - 1,
                                       pos.z + zStep) == BlockID::Air) {
            return true;
        }
        if (m_panda->IsPlayful()
            && level->Random().NextInt(ReducedTickDelay(60)) == 1) {
            return true;
        }
        return level->Random().NextInt(ReducedTickDelay(500)) == 1;
    }

    void PandaRollGoal::Start() { m_panda->Roll(true); }

    // ── PandaLookAtPlayerGoal ──────────────────────────────────────────────

    PandaLookAtPlayerGoal::PandaLookAtPlayerGoal(Panda* panda, float lookDistance)
        : m_panda(panda), m_lookDistance(lookDistance) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Look));
    }

    bool PandaLookAtPlayerGoal::CanUse() {
        // MC: probability roll first (the base goal's 0.02), then either the
        // injected stare target or the nearest player.
        EntityLevel* level = m_panda->Level();
        if (!level) return false;
        if (level->Random().NextFloat() >= 0.02f) return false;

        if (!m_lookAt) {
            TargetingConditions conditions =
                TargetingConditions::ForNonCombat().Range(m_lookDistance);
            LivingEntity* nearest = level->GetNearestPlayer(
                m_panda->position.x, m_panda->GetEyeY(), m_panda->position.z,
                m_lookDistance);
            if (nearest && conditions.Test(m_panda, *nearest)) {
                m_lookAt = nearest;
            }
        }
        return m_panda->CanPerformAction() && m_lookAt != nullptr;
    }

    bool PandaLookAtPlayerGoal::CanContinueToUse() {
        if (!m_lookAt || !m_lookAt->IsAlive()) return false;
        if (m_panda->DistanceToSqr(*m_lookAt)
            > static_cast<double>(m_lookDistance) * m_lookDistance) {
            return false;
        }
        return m_lookTime > 0;
    }

    void PandaLookAtPlayerGoal::Start() {
        m_lookTime =
            AdjustedTickDelay(40 + m_panda->Level()->Random().NextInt(40));
    }

    void PandaLookAtPlayerGoal::Stop() { m_lookAt = nullptr; }

    void PandaLookAtPlayerGoal::Tick() {
        if (!m_lookAt || !m_lookAt->IsAlive()) return;
        m_panda->GetLookControl().SetLookAt(
            m_lookAt->position.x, m_lookAt->GetEyeY(), m_lookAt->position.z);
        --m_lookTime;
    }

    void PandaLookAtPlayerGoal::ClearReferenceTo(const Entity* entity) {
        if (m_lookAt == entity) m_lookAt = nullptr;
    }

    // ── PandaHurtByTargetGoal ──────────────────────────────────────────────

    PandaHurtByTargetGoal::PandaHurtByTargetGoal(Panda* panda)
        : HurtByTargetGoal(panda), m_panda(panda) {
        SetAlertOthers();
    }

    bool PandaHurtByTargetGoal::CanContinueToUse() {
        // MC: gotBamboo || didBite drops the grudge — gotBamboo is the
        // feed-while-angry stand-down (Panda::MobInteract sets it).
        if (m_panda->GotBamboo() || m_panda->DidBite()) {
            m_panda->SetTarget(nullptr);
            return false;
        }
        return HurtByTargetGoal::CanContinueToUse();
    }

    void PandaHurtByTargetGoal::AlertOther(Mob& other, LivingEntity& attacker) {
        // MC: only AGGRESSIVE-gene pandas answer the call.
        if (auto* panda = dynamic_cast<Panda*>(&other)) {
            if (panda->IsAggressiveGene()) {
                panda->SetTarget(&attacker);
            }
        }
    }

} // namespace Game

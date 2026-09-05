// File: src/common/entity/mobs/Slime.cpp
#include "common/entity/mobs/Slime.hpp"
#include "common/entity/ai/goals/SlimeGoals.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"
#include "common/entity/ai/Sensing.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"

#include <cmath>

namespace Game {

    // ── SlimeMoveControl ───────────────────────────────────────────────────

    SlimeMoveControl::SlimeMoveControl(Slime* slime)
        : MoveControl(slime), m_slime(slime) {
        m_yRot = slime->yRot;
    }

    void SlimeMoveControl::Tick() {
        // The whole body turns; a slime has no independent head.
        m_mob->yRot = RotLerp(m_mob->yRot, m_yRot, 90.0f);
        m_mob->yHeadRot = m_mob->yRot;
        m_mob->yBodyRot = m_mob->yRot;

        if (m_operation != Operation::MoveTo) {
            m_mob->SetZza(0.0f);
            return;
        }
        m_operation = Operation::Wait;

        if (m_mob->onGround) {
            m_mob->SetSpeed(static_cast<float>(
                m_speedModifier * m_mob->GetAttributeValue(Attribute::MovementSpeed)));
            if (m_jumpDelay-- <= 0) {
                m_jumpDelay = m_slime->GetJumpDelay();
                // Chasing slimes hop three times as often — the entire
                // difference between an idle slime and an angry one.
                if (m_aggressive) m_jumpDelay /= 3;
                m_slime->GetJumpControl().Jump();
            } else {
                // Grounded between hops: a slime does not slide.
                m_slime->SetXxa(0.0f);
                m_slime->SetZza(0.0f);
                m_mob->SetSpeed(0.0f);
            }
        } else {
            m_mob->SetSpeed(static_cast<float>(
                m_speedModifier * m_mob->GetAttributeValue(Attribute::MovementSpeed)));
        }
    }

    // ── Slime ──────────────────────────────────────────────────────────────

    Slime::Slime(EntityTypeId type, EntityLevel* level) : Mob(type, level) {
        SetMoveControl(std::make_unique<SlimeMoveControl>(this));
        SetSize(1, true);
        RegisterGoals();
    }

    void Slime::RegisterGoals() {
        m_goalSelector.AddGoal(1, std::make_unique<SlimeFloatGoal>(this));
        m_goalSelector.AddGoal(2, std::make_unique<SlimeAttackGoal>(this));
        m_goalSelector.AddGoal(3, std::make_unique<SlimeRandomDirectionGoal>(this));
        m_goalSelector.AddGoal(5, std::make_unique<SlimeKeepOnJumpingGoal>(this));

        // MC: players within 10 (with the |dy| <= 4 selector — a slime does
        // not notice you four floors up; the selector needs per-target
        // predicates, so the height clause is approximated by the goal's own
        // vertical search box), iron golems at 3.
        m_targetSelector.AddGoal(1, std::make_unique<NearestAttackablePlayerGoal>(this, true));
        static constexpr EntityTypeId kGolemTargets[] = { EntityTypeId::IronGolem };
        m_targetSelector.AddGoal(3, std::make_unique<NearestAttackableTargetGoal>(
                                        this, kGolemTargets, 1, true));
    }

    void Slime::SetSize(int size, bool resetHealth) {
        m_size = std::clamp(size, 1, 127);
        m_attributes.SetBaseValue(Attribute::MaxHealth,
                                  static_cast<double>(m_size * m_size));
        m_attributes.SetBaseValue(Attribute::MovementSpeed, 0.2 + 0.1 * m_size);
        m_attributes.SetBaseValue(Attribute::AttackDamage, static_cast<double>(m_size));
        if (resetHealth) m_health = GetMaxHealth();
    }

    int Slime::GetJumpDelay() {
        return m_level->Random().NextInt(20) + 10;
    }

    int Slime::GetSplitCount() {
        return 2 + m_level->Random().NextInt(3);
    }

    void Slime::JumpFromGround() {
        velocity.y = GetJumpPower();
        needsSync = true;
    }

    std::shared_ptr<SpawnGroupData>
    Slime::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        if (m_level) {
            JavaRandom& rng = m_level->Random();
            int sizeScale = rng.NextInt(3);
            if (sizeScale < 2 &&
                rng.NextFloat() < 0.5f * GetSpecialMultiplier(m_level->GetDifficulty())) {
                ++sizeScale;
            }
            SetSize(1 << sizeScale, true);
        }
        return Mob::FinalizeSpawn(reason, std::move(groupData));
    }

    void Slime::DealContactDamage() {
        // MC deals contact damage from playerTouch/push. The engine has no
        // entity-touch callbacks, so the server applies the same rule per
        // tick: alive, within melee reach, line of sight — the target's own
        // 10-tick invulnerability window sets the damage cadence.
        if (!DealsDamage() || !m_level || m_level->IsClientSide()) return;

        LivingEntity* target = GetTarget();
        if (!target || !target->IsAlive()) {
            // Contact hurts even without aggro — a player walking into an
            // idle slime takes the hit. Check the nearest player too.
            target = m_level->GetNearestPlayer(position.x, position.y, position.z,
                                               GetBbWidth() + 1.0);
        }
        if (!target || !target->IsAlive()) return;
        if (target->IsCreative() || target->IsSpectator()) return;

        if (IsWithinMeleeAttackRange(*target) && GetSensing().HasLineOfSight(*target)) {
            target->Hurt(MobDamageSource::MobAttack, GetAttackDamageValue(), this);
        }
    }

    void Slime::Tick() {
        // MC ticks the squish spring BEFORE super.tick so the landing below
        // overwrites the fresh lerp, not last tick's.
        m_oSquish = m_squish;
        m_squish += (m_targetSquish - m_squish) * 0.5f;

        Mob::Tick();

        if (onGround && !m_wasOnGround) {
            // Landing: full squash. (MC also bursts slime particles here.)
            m_targetSquish = -0.5f;
        } else if (!onGround && m_wasOnGround) {
            m_targetSquish = 1.0f;
        }
        m_wasOnGround = onGround;
        m_targetSquish *= SquishDecay();   // decreaseSquish

        DealContactDamage();
    }

    void Slime::TickDeath() {
        Mob::TickDeath();

        // MC Slime.remove: a lethal removal of size > 1 splits into 2-4
        // halves flung around the corpse. Runs exactly once — the base just
        // flipped IsRemoved on the 20th death tick.
        if (IsRemoved() && m_size > 1 && m_level && !m_level->IsClientSide()) {
            JavaRandom& rng = m_level->Random();
            const float offset = GetBbWidth() / 4.0f;   // width/2 halved
            const int halfSize = m_size / 2;
            const int count = GetSplitCount();

            for (int i = 0; i < count; ++i) {
                // MC's grid: i%2 alternates x, i/2 (INTEGER division, on
                // purpose) steps z every second slime.
                const float xd = (static_cast<float>(i % 2) - 0.5f) * offset;
                const int   zRow = i / 2;
                const float zd = (static_cast<float>(zRow) - 0.5f) * offset;

                std::unique_ptr<Slime> split = MakeSplitChild();
                split->SetSize(halfSize, true);
                split->position = glm::dvec3(position.x + xd, position.y + 0.5,
                                             position.z + zd);
                split->yRot = rng.NextFloat() * 360.0f;
                split->yHeadRot = split->yBodyRot = split->yRot;
                m_level->AddFreshEntity(std::move(split));
            }
        }
    }

} // namespace Game

// File: src/common/entity/ai/brain/NautilusAi.cpp
#include "common/entity/ai/brain/NautilusAi.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/Sensing.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/entity/ai/brain/CommonBehaviors.hpp"
#include "common/entity/ai/brain/CoreBehaviors.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/entity/mobs/Fish.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    namespace {

        // MC's constants.
        constexpr int kTimeBetweenNonPlayerAttacksMin = 2400;
        constexpr int kTimeBetweenNonPlayerAttacksMax = 3600;
        constexpr int kAngerDuration    = 400;
        constexpr int kTimeBetweenAttacks = 80;
        constexpr double kMaxChargeDistance = 12.0;
        constexpr double kMaxTargetDetectionDistance = 11.0;
        constexpr float kAttackKnockbackForce = 2.0f;

        // MC ai/behavior/ChargeAttack — a straight-line ram: lock a velocity
        // vector at start, sail along it, and hit the first attackable living
        // entity the body touches for attack damage plus speed-scaled
        // knockback. The dash sound is skipped; the isTame early-out is moot
        // (no taming system).
        class ChargeAttack : public Behavior {
        public:
            explicit ChargeAttack(float speed)
                : Behavior({ MemoryCondition{ MemoryModule::ChargeCooldownTicks,
                                              MemoryStatus::ValueAbsent },
                             MemoryCondition{ MemoryModule::AttackTarget,
                                              MemoryStatus::ValuePresent } }),
                  m_speed(speed) {}
            const char* DebugString() const override { return "ChargeAttack"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                const Brain* brain = body.GetBrain();
                return brain && brain->HasMemoryValue(MemoryModule::AttackTarget);
            }

            bool CanStillUse(EntityLevel&, LivingEntity& body, int64_t) override {
                auto* mob = dynamic_cast<Mob*>(&body);
                Brain* brain = body.GetBrain();
                if (!mob || !brain) return false;
                auto* target = dynamic_cast<LivingEntity*>(
                    brain->GetEntity(MemoryModule::AttackTarget));
                if (!target) return false;
                const glm::dvec3 travelled = body.position - m_startPosition;
                if (glm::dot(travelled, travelled)
                    >= kMaxChargeDistance * kMaxChargeDistance) return false;
                if (body.DistanceToSqr(*target)
                    >= kMaxTargetDetectionDistance * kMaxTargetDetectionDistance) {
                    return false;
                }
                if (!mob->GetSensing().HasLineOfSight(*target)) return false;
                return !brain->HasMemoryValue(MemoryModule::ChargeCooldownTicks);
            }

            void Start(EntityLevel& level, LivingEntity& body, int64_t timestamp) override {
                Brain* brain = body.GetBrain();
                if (!brain) return;
                m_startPosition = body.position;
                if (Entity* target = brain->GetEntity(MemoryModule::AttackTarget)) {
                    const glm::dvec3 d = target->position - body.position;
                    const double len = std::sqrt(glm::dot(d, d));
                    m_chargeVelocity = len > 1.0e-7 ? d / len * static_cast<double>(m_speed)
                                                    : glm::dvec3(0.0);
                }
                (void)level; (void)timestamp;
            }

            void Tick(EntityLevel& level, LivingEntity& body, int64_t timestamp) override {
                auto* mob = dynamic_cast<Mob*>(&body);
                Brain* brain = body.GetBrain();
                if (!mob || !brain) return;
                auto* target = dynamic_cast<LivingEntity*>(
                    brain->GetEntity(MemoryModule::AttackTarget));
                if (!target) return;

                // MC body.lookAt(target, 360, 360) — an unclamped snap.
                mob->GetLookControl().SetLookAt(target->position);
                body.velocity = m_chargeVelocity;
                body.needsSync = true;

                // MC: hit the first attackable living entity intersecting the
                // body's box.
                std::vector<Entity*> hits;
                level.GetEntitiesInBox(body.GetAABB(), &body, hits);
                for (Entity* e : hits) {
                    auto* victim = dynamic_cast<LivingEntity*>(e);
                    if (!victim || !victim->IsAlive()) continue;
                    if (!mob->CanAttack(*victim)) continue;
                    if (body.HasPassenger(*victim)) return;

                    // dealDamageToTarget.
                    const float damage = static_cast<float>(
                        body.GetAttributeValue(Attribute::AttackDamage));
                    victim->Hurt(MobDamageSource::MobAttack, damage, &body);

                    // dealKnockBack — speed effects scale it exactly as MC.
                    const MobEffectInstance* sp = body.GetEffect(MobEffectId::Speed);
                    const MobEffectInstance* sl = body.GetEffect(MobEffectId::Slowness);
                    const float boost = 0.25f * static_cast<float>(
                        (sp ? sp->amplifier + 1 : 0) - (sl ? sl->amplifier + 1 : 0));
                    const float speedFactor = std::clamp(
                        m_speed * static_cast<float>(
                            body.GetAttributeValue(Attribute::MovementSpeed)),
                        0.2f, 2.0f) + boost;
                    // MC causeExtraKnockback pushes the victim ALONG the
                    // charge; LivingEntity::Knockback pushes away from the
                    // given direction, so the vector is negated.
                    victim->Knockback(speedFactor * kAttackKnockbackForce,
                                      -m_chargeVelocity.x, -m_chargeVelocity.z);

                    DoStop(level, body, timestamp);
                    return;
                }
            }

            void Stop(EntityLevel&, LivingEntity& body, int64_t) override {
                if (Brain* brain = body.GetBrain()) {
                    brain->SetMemory(MemoryModule::ChargeCooldownTicks,
                                     kTimeBetweenAttacks);
                    brain->EraseMemory(MemoryModule::AttackTarget);
                }
            }

        private:
            float m_speed;
            glm::dvec3 m_chargeVelocity{0.0};
            glm::dvec3 m_startPosition{0.0};
        };

    } // namespace

    BehaviorPtr NautilusAi::MakeChargeAttack(float speed) {
        return std::make_unique<ChargeAttack>(speed);
    }

    void NautilusAi::InitMemories(Mob& nautilus) {
        Brain* brain = nautilus.GetBrain();
        EntityLevel* level = nautilus.Level();
        if (brain && level) {
            brain->SetMemory(MemoryModule::AttackTargetCooldown,
                             level->Random().NextInt(kTimeBetweenNonPlayerAttacksMin,
                                                     kTimeBetweenNonPlayerAttacksMax));
        }
    }

    void NautilusAi::SetAngerTarget(Mob& nautilus, LivingEntity& target) {
        Brain* brain = nautilus.GetBrain();
        if (!brain || !nautilus.CanAttack(target)) return;
        brain->EraseMemory(MemoryModule::CantReachWalkTargetSince);
        brain->SetMemoryWithExpiry(MemoryModule::AngryAt,
                                   static_cast<Entity*>(&target), kAngerDuration);
    }

    LivingEntity* NautilusAi::FindNearestValidAttackTarget(Mob& mob) {
        Brain* brain = mob.GetBrain();
        EntityLevel* level = mob.Level();
        if (!brain || !level) return nullptr;
        // MC: never while breeding, beached, a baby, or tame (no taming
        // system — the last is always false).
        if (brain->HasMemoryValue(MemoryModule::BreedTarget)) return nullptr;
        if (!mob.IsInWater() || mob.IsBaby()) return nullptr;

        if (auto* angry =
                dynamic_cast<LivingEntity*>(brain->GetEntity(MemoryModule::AngryAt))) {
            if (angry->IsInWater() && mob.CanAttack(*angry)) return angry;
        }
        if (brain->HasMemoryValue(MemoryModule::AttackTargetCooldown)) return nullptr;

        // MC: re-arm the cooldown, then a coin flip before the unprovoked
        // pufferfish hunt.
        JavaRandom& rng = level->Random();
        brain->SetMemory(MemoryModule::AttackTargetCooldown,
                         rng.NextInt(kTimeBetweenNonPlayerAttacksMin,
                                     kTimeBetweenNonPlayerAttacksMax));
        if (rng.NextFloat() < 0.5f) return nullptr;

        const NearestVisibleLivingEntities* visible =
            brain->GetVisibleEntities(MemoryModule::NearestVisibleLivingEntities);
        if (!visible) return nullptr;
        // MC EntityTypeTags.NAUTILUS_HOSTILES — the pufferfish.
        return visible->FindClosest([](LivingEntity* e) {
            return e->IsInWater() && e->GetType() == EntityTypeId::Pufferfish;
        });
    }

    void NautilusAi::InitBrain(Nautilus& nautilus, Brain& brain) {
        // MC NautilusAi.MEMORY_TYPES, in declaration order.
        for (MemoryModule m : { MemoryModule::LookTarget,
                                MemoryModule::NearestVisibleLivingEntities,
                                MemoryModule::WalkTarget,
                                MemoryModule::CantReachWalkTargetSince,
                                MemoryModule::Path,
                                MemoryModule::NearestVisibleAdult,
                                MemoryModule::TemptationCooldownTicks,
                                MemoryModule::IsTempted,
                                MemoryModule::TemptingPlayer,
                                MemoryModule::BreedTarget,
                                MemoryModule::IsPanicking,
                                MemoryModule::AttackTarget,
                                MemoryModule::ChargeCooldownTicks,
                                MemoryModule::HurtBy,
                                MemoryModule::HurtByEntity,
                                MemoryModule::AngryAt,
                                MemoryModule::AttackTargetCooldown,
                                MemoryModule::NearestLivingEntities }) {
            brain.RegisterMemory(m);
        }

        // MC SENSOR_TYPES: NEAREST_LIVING_ENTITIES, NEAREST_ADULT,
        // NEAREST_PLAYERS, HURT_BY, NAUTILUS_TEMPTATIONS.
        brain.AddSensor(std::make_unique<NearestLivingEntitySensor>());
        brain.AddSensor(std::make_unique<AdultSensor>());
        brain.AddSensor(std::make_unique<PlayerSensor>());
        brain.AddSensor(std::make_unique<HurtBySensor>());
        brain.AddSensor(std::make_unique<TemptingSensor>([&nautilus](uint32_t item) {
            // ItemTags.NAUTILUS_FOOD — the def's flattened food list.
            return nautilus.IsFood(item);
        }));

        // ── CORE (MC initCoreActivity) ─────────────────────────────────────
        std::vector<BehaviorPtr> core;
        core.push_back(std::make_unique<AnimalPanic>(1.6f));
        core.push_back(std::make_unique<LookAtTargetSink>(45, 90));
        core.push_back(std::make_unique<MoveToTargetSink>());
        core.push_back(std::make_unique<CountDownCooldownTicks>(
            MemoryModule::TemptationCooldownTicks));
        core.push_back(std::make_unique<CountDownCooldownTicks>(
            MemoryModule::ChargeCooldownTicks));
        core.push_back(std::make_unique<CountDownCooldownTicks>(
            MemoryModule::AttackTargetCooldown));
        brain.AddActivity(Activity::Core, 0, std::move(core));

        // ── IDLE (MC initIdleActivity, explicit priority pairs) ────────────
        std::vector<BehaviorPtr> idle;
        idle.push_back(std::make_unique<AnimalMakeLove>(EntityTypeId::Nautilus, 0.4f, 2));
        // MC FollowTemptation(1.3, baby ? 2.5 : 3.5) — the port ctor takes a
        // fixed close-enough; the adult's 3.5 is the common case.
        idle.push_back(std::make_unique<FollowTemptation>(
            SpeedFn([](LivingEntity&) { return 1.3f; }), 3.5));
        idle.push_back(std::make_unique<StartAttacking>(
            [](Mob&) { return true; }, &FindNearestValidAttackTarget));
        std::vector<GateBehavior::Entry> gate;
        gate.push_back({ RandomStroll::Swim(1.0f), 2 });
        gate.push_back({ std::make_unique<SetWalkTargetFromLookTarget>(1.0f, 3), 3 });
        idle.push_back(std::make_unique<GateBehavior>(
            std::vector<MemoryCondition>{
                MemoryCondition{ MemoryModule::WalkTarget, MemoryStatus::ValueAbsent } },
            std::vector<MemoryModule>{},
            GateBehavior::OrderPolicy::Ordered, GateBehavior::RunningPolicy::TryAll,
            std::move(gate)));
        brain.AddActivity(Activity::Idle, 1, std::move(idle));

        // ── FIGHT (MC initFightActivity) ───────────────────────────────────
        // MC uses addActivityWithConditions — the activity validates only
        // with a target, no temptation, no breeding and no charge cooldown,
        // and (unlike the usual FIGHT) erases nothing on exit: ChargeAttack
        // itself clears ATTACK_TARGET when the ram ends.
        std::vector<BehaviorPtr> fight;
        fight.push_back(MakeChargeAttack(0.6f));
        brain.AddActivityWithConditions(
            Activity::Fight, 0, std::move(fight),
            { MemoryCondition{ MemoryModule::AttackTarget, MemoryStatus::ValuePresent },
              MemoryCondition{ MemoryModule::TemptingPlayer, MemoryStatus::ValueAbsent },
              MemoryCondition{ MemoryModule::BreedTarget, MemoryStatus::ValueAbsent },
              MemoryCondition{ MemoryModule::ChargeCooldownTicks,
                               MemoryStatus::ValueAbsent } });

        brain.SetCoreActivities({ Activity::Core });
        brain.SetDefaultActivity(Activity::Idle);
        brain.UseDefaultActivity();
    }

    void NautilusAi::UpdateActivity(Nautilus& nautilus) {
        if (Brain* brain = nautilus.GetBrain()) {
            brain->SetActiveActivityToFirstValid({ Activity::Fight, Activity::Idle });
        }
    }

} // namespace Game

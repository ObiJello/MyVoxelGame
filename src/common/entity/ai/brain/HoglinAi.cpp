// File: src/common/entity/ai/brain/HoglinAi.cpp
//
// See HoglinAi.hpp. References (minecraft_code_26.3-pre-2/decompiled_net/
// minecraft/): world/entity/monster/hoglin/HoglinAi.java, world/entity/ai/
// sensing/HoglinSpecificSensor.java, world/entity/ai/behavior/
// BecomePassiveIfMemoryPresent.java, BehaviorUtils.java.
#include "common/entity/ai/brain/HoglinAi.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/TargetingConditions.hpp"
#include "common/entity/ai/brain/CommonBehaviors.hpp"
#include "common/entity/ai/brain/CoreBehaviors.hpp"
#include "common/entity/ai/brain/ManhattanBlockSearch.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

#include <functional>

namespace Game {

    namespace {

        // MC's constants.
        constexpr int kRetreatMin    = 5 * 20;    // RETREAT_DURATION = rangeOfSeconds(5, 20)
        constexpr int kRetreatMax    = 20 * 20;
        constexpr int kAttackDuration = 200;      // ATTACK_DURATION
        constexpr int kRepellentPacifyTime = 200; // REPELLENT_PACIFY_TIME
        constexpr int kAdultFollowMin = 5;        // ADULT_FOLLOW_RANGE = UniformInt.of(5, 16)
        constexpr int kAdultFollowMax = 16;

        bool IsBreeding(const LivingEntity& body) {
            const Brain* brain = body.GetBrain();
            return brain && brain->HasMemoryValue(MemoryModule::BreedTarget);
        }

        bool IsPacifiedBody(const LivingEntity& body) {
            const Brain* brain = body.GetBrain();
            return brain && brain->HasMemoryValue(MemoryModule::Pacified);
        }

        // MC BehaviorUtils.getNearestTarget(body, current, candidate): the
        // candidate unless the current one is strictly nearer.
        LivingEntity* NearestTarget(const LivingEntity& body, LivingEntity* current, LivingEntity* candidate) {
            if (!current) return candidate;
            return body.DistanceToSqr(*current) < body.DistanceToSqr(*candidate) ? current : candidate;
        }

        // MC BehaviorUtils.isOtherTargetMuchFurtherAwayThanCurrentAttackTarget.
        bool IsOtherTargetMuchFurtherAway(const LivingEntity& body, const LivingEntity& other, double howMuch) {
            const Brain* brain = body.GetBrain();
            const Entity* current = brain ? brain->GetEntity(MemoryModule::AttackTarget) : nullptr;
            if (!current) return false;
            return body.DistanceToSqr(other) > body.DistanceToSqr(*current) + howMuch * howMuch;
        }

        // MC Sensor.isEntityAttackable: the combat test at the follow range,
        // line of sight waived for the current attack target.
        bool IsEntityAttackable(Mob& body, LivingEntity& target) {
            const Brain* brain = body.GetBrain();
            TargetingConditions conditions = TargetingConditions::ForCombat().Range(
                body.GetAttributeValue(Attribute::FollowRange));
            if (brain && brain->IsMemoryValue(MemoryModule::AttackTarget, &target)) {
                conditions.IgnoreLineOfSight().IgnoreInvisibility();
            }
            return conditions.Test(&body, target);
        }

        std::vector<Hoglin*> VisibleAdultHoglins(const Hoglin& body) {
            std::vector<Hoglin*> out;
            const Brain* brain = body.GetBrain();
            if (const std::vector<Entity*>* list = brain ? brain->GetEntityList(MemoryModule::NearestVisibleAdultHoglins)
                                                         : nullptr) {
                for (Entity* e : *list) {
                    if (auto* hoglin = dynamic_cast<Hoglin*>(e)) out.push_back(hoglin);
                }
            }
            return out;
        }

        // MC HoglinAi.piglinsOutnumberHoglins — never for a baby.
        bool PiglinsOutnumberHoglins(const Hoglin& body) {
            if (body.IsBaby()) return false;
            const Brain* brain = body.GetBrain();
            if (!brain) return false;
            const int piglins = brain->GetInt(MemoryModule::VisibleAdultPiglinCount).value_or(0);
            const int hoglins = brain->GetInt(MemoryModule::VisibleAdultHoglinCount).value_or(0) + 1;
            return piglins > hoglins;
        }

        bool WantsToStopFleeing(LivingEntity& body) {
            auto* hoglin = dynamic_cast<Hoglin*>(&body);
            return hoglin && !hoglin->IsBaby() && !PiglinsOutnumberHoglins(*hoglin);
        }

        void SetAvoidTarget(Hoglin& body, LivingEntity& avoidTarget) {
            Brain* brain = body.GetBrain();
            EntityLevel* level = body.Level();
            if (!brain || !level) return;
            brain->EraseMemory(MemoryModule::AttackTarget);
            brain->EraseMemory(MemoryModule::WalkTarget);
            brain->SetMemoryWithExpiry(MemoryModule::AvoidTarget, static_cast<Entity*>(&avoidTarget),
                                       level->Random().NextInt(kRetreatMin, kRetreatMax));
        }

        void RetreatFromNearestTarget(Hoglin& body, LivingEntity& newAvoidTarget) {
            Brain* brain = body.GetBrain();
            if (!brain) return;
            LivingEntity* nearest = &newAvoidTarget;
            nearest = NearestTarget(body, dynamic_cast<LivingEntity*>(brain->GetEntity(MemoryModule::AvoidTarget)), nearest);
            nearest = NearestTarget(body, dynamic_cast<LivingEntity*>(brain->GetEntity(MemoryModule::AttackTarget)), nearest);
            SetAvoidTarget(body, *nearest);
        }

        void BroadcastRetreat(Hoglin& body, LivingEntity& target) {
            for (Hoglin* hoglin : VisibleAdultHoglins(body)) RetreatFromNearestTarget(*hoglin, target);
        }

        void SetAttackTarget(Hoglin& body, LivingEntity& target) {
            Brain* brain = body.GetBrain();
            if (!brain) return;
            brain->EraseMemory(MemoryModule::CantReachWalkTargetSince);
            brain->EraseMemory(MemoryModule::BreedTarget);
            brain->SetMemoryWithExpiry(MemoryModule::AttackTarget, static_cast<Entity*>(&target), kAttackDuration);
        }

        void SetAttackTargetIfCloserThanCurrent(Hoglin& body, LivingEntity& newTarget) {
            if (IsPacifiedBody(body)) return;
            const Brain* brain = body.GetBrain();
            auto* current = brain ? dynamic_cast<LivingEntity*>(brain->GetEntity(MemoryModule::AttackTarget)) : nullptr;
            SetAttackTarget(body, *NearestTarget(body, current, &newTarget));
        }

        void BroadcastAttackTarget(Hoglin& body, LivingEntity& target) {
            for (Hoglin* hoglin : VisibleAdultHoglins(body)) SetAttackTargetIfCloserThanCurrent(*hoglin, target);
        }

        // MC HoglinAi.findNearestValidAttackTarget.
        LivingEntity* FindNearestValidAttackTarget(Mob& body) {
            if (IsPacifiedBody(body) || IsBreeding(body)) return nullptr;
            const Brain* brain = body.GetBrain();
            return brain ? dynamic_cast<LivingEntity*>(brain->GetEntity(MemoryModule::NearestVisibleAttackablePlayer))
                         : nullptr;
        }

        bool IsAdult(LivingEntity& e) { return !e.IsBaby(); }
        bool IsBabyBody(LivingEntity& e) { return e.IsBaby(); }

        // MC BehaviorBuilder.triggerIf(predicate, behaviour): the wrapped
        // behaviour may only START while the predicate holds.
        class TriggerIfBehavior : public BehaviorControl {
        public:
            using Pred = std::function<bool(LivingEntity&)>;
            TriggerIfBehavior(Pred pred, BehaviorPtr inner) : m_pred(std::move(pred)), m_inner(std::move(inner)) {}
            BehaviorStatus GetStatus() const override { return m_inner->GetStatus(); }
            bool TryStart(EntityLevel& level, LivingEntity& body, int64_t timestamp) override {
                return m_pred(body) && m_inner->TryStart(level, body, timestamp);
            }
            void TickOrStop(EntityLevel& level, LivingEntity& body, int64_t timestamp) override {
                m_inner->TickOrStop(level, body, timestamp);
            }
            void DoStop(EntityLevel& level, LivingEntity& body, int64_t timestamp) override {
                m_inner->DoStop(level, body, timestamp);
            }
            const char* DebugString() const override { return m_inner->DebugString(); }
            void ClearReferenceTo(const Entity* entity) override { m_inner->ClearReferenceTo(entity); }
        private:
            Pred m_pred;
            BehaviorPtr m_inner;
        };

        // MC BecomePassiveIfMemoryPresent.create(memory, duration): while the
        // pacifying memory is present (and PACIFIED absent), turn PACIFIED on
        // for `duration` ticks and drop the attack target.
        class BecomePassiveIfMemoryPresent : public Behavior {
        public:
            BecomePassiveIfMemoryPresent(MemoryModule pacifying, int duration)
                : Behavior({ MemoryCondition{ MemoryModule::AttackTarget, MemoryStatus::Registered },
                             MemoryCondition{ MemoryModule::Pacified, MemoryStatus::ValueAbsent },
                             MemoryCondition{ pacifying, MemoryStatus::ValuePresent } },
                           1),
                  m_duration(duration) {}
            const char* DebugString() const override { return "BecomePassiveIfMemoryPresent"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                Brain* brain = body.GetBrain();
                if (!brain) return false;
                brain->SetMemoryWithExpiry(MemoryModule::Pacified, true, m_duration);
                brain->EraseMemory(MemoryModule::AttackTarget);
                return true;
            }
        private:
            int m_duration;
        };

        // MC HoglinSpecificSensor (scan rate 20).
        class HoglinSpecificSensor : public Sensor {
        public:
            std::vector<MemoryModule> Requires() const override {
                return { MemoryModule::NearestVisibleLivingEntities, MemoryModule::NearestRepellent,
                         MemoryModule::NearestVisibleAdultPiglin, MemoryModule::NearestVisibleAdultHoglins,
                         MemoryModule::VisibleAdultPiglinCount, MemoryModule::VisibleAdultHoglinCount };
            }
        protected:
            void DoTick(EntityLevel& level, LivingEntity& body) override {
                Brain* brain = body.GetBrain();
                if (!brain) return;
                // findNearestRepellent: the first BlockTags.HOGLIN_REPELLENTS
                // cell of the 8/4/8 manhattan walk.
                if (const IBlockAccess* blocks = level.Blocks()) {
                    const auto repellent = FindFirstInBoxByManhattanDistance(
                        body.BlockPosition(), 8, 4, 8, [blocks](const glm::ivec3& p) {
                            const BlockID id = blocks->GetBlock(p.x, p.y, p.z);
                            return id == BlockID::WarpedFungus || id == BlockID::PottedWarpedFungus ||
                                   id == BlockID::NetherPortal || id == BlockID::RespawnAnchor;
                        });
                    if (repellent) brain->SetMemory(MemoryModule::NearestRepellent, *repellent);
                    else           brain->EraseMemory(MemoryModule::NearestRepellent);
                }

                Entity* adultPiglin = nullptr;
                int adultPiglinCount = 0;
                std::vector<Entity*> adultHoglins;
                if (const NearestVisibleLivingEntities* visible =
                        brain->GetVisibleEntities(MemoryModule::NearestVisibleLivingEntities)) {
                    for (LivingEntity* e : visible->entities) {
                        if (!visible->IsVisible(e) || e->IsBaby()) continue;
                        // `instanceof Piglin` — the brute is not one.
                        if (e->GetType() == EntityTypeId::Piglin) {
                            ++adultPiglinCount;
                            if (!adultPiglin) adultPiglin = e;
                        }
                        if (e->GetType() == EntityTypeId::Hoglin) adultHoglins.push_back(e);
                    }
                }
                if (adultPiglin) brain->SetMemory(MemoryModule::NearestVisibleAdultPiglin, adultPiglin);
                else             brain->EraseMemory(MemoryModule::NearestVisibleAdultPiglin);
                brain->SetMemory(MemoryModule::VisibleAdultPiglinCount, adultPiglinCount);
                brain->SetMemory(MemoryModule::VisibleAdultHoglinCount, static_cast<int>(adultHoglins.size()));
                brain->SetMemory(MemoryModule::NearestVisibleAdultHoglins, std::move(adultHoglins));
            }
        };

        // MC HoglinAi.createIdleMovementBehaviors.
        BehaviorPtr IdleMovement() {
            std::vector<GateBehavior::Entry> gate;
            gate.push_back({ RandomStroll::Stroll(0.4f), 2 });
            gate.push_back({ std::make_unique<SetWalkTargetFromLookTarget>(0.4f, 3), 2 });
            gate.push_back({ std::make_unique<DoNothing>(30, 60), 1 });
            return MakeRunOne(std::move(gate));
        }

    } // namespace

    bool HoglinAi::IsPacified(const Hoglin& hoglin) { return IsPacifiedBody(hoglin); }

    bool HoglinAi::IsPosNearNearestRepellent(const Hoglin& hoglin, const glm::ivec3& pos) {
        const Brain* brain = hoglin.GetBrain();
        const std::optional<glm::ivec3> repellent = brain ? brain->GetBlockPos(MemoryModule::NearestRepellent)
                                                          : std::nullopt;
        if (!repellent) return false;
        // BlockPos.closerThan(pos, 8): the cells' distance, strictly < 8.
        const glm::dvec3 d = glm::dvec3(*repellent - pos);
        return glm::dot(d, d) < 64.0;
    }

    void HoglinAi::InitBrain(Hoglin& hoglin, Brain& brain) {
        (void)hoglin;
        for (MemoryModule m : { MemoryModule::LookTarget,
                                MemoryModule::WalkTarget,
                                MemoryModule::CantReachWalkTargetSince,
                                MemoryModule::Path,
                                MemoryModule::AttackTarget,
                                MemoryModule::AttackCoolingDown,
                                MemoryModule::BreedTarget,
                                MemoryModule::NearestVisibleAdult,
                                MemoryModule::AvoidTarget,
                                MemoryModule::Pacified }) {
            brain.RegisterMemory(m);
        }

        // MC SENSOR_TYPES: NEAREST_LIVING_ENTITIES, NEAREST_PLAYERS,
        // NEAREST_ADULT, HOGLIN_SPECIFIC_SENSOR.
        brain.AddSensor(std::make_unique<NearestLivingEntitySensor>());
        brain.AddSensor(std::make_unique<PlayerSensor>());
        brain.AddSensor(std::make_unique<AdultSensor>());
        brain.AddSensor(std::make_unique<HoglinSpecificSensor>());

        // ── CORE ───────────────────────────────────────────────────────────
        std::vector<BehaviorPtr> core;
        core.push_back(std::make_unique<LookAtTargetSink>(45, 90));
        core.push_back(std::make_unique<MoveToTargetSink>());
        brain.AddActivity(Activity::Core, 0, std::move(core));

        // ── IDLE (priority 10) ─────────────────────────────────────────────
        std::vector<BehaviorPtr> idle;
        idle.push_back(std::make_unique<BecomePassiveIfMemoryPresent>(MemoryModule::NearestRepellent,
                                                                      kRepellentPacifyTime));
        idle.push_back(std::make_unique<AnimalMakeLove>(EntityTypeId::Hoglin, 0.6f, 2));
        idle.push_back(SetWalkTargetAwayFrom::Pos(MemoryModule::NearestRepellent, 1.0f, 8, true));
        idle.push_back(std::make_unique<StartAttacking>([](Mob&) { return true; }, &FindNearestValidAttackTarget));
        idle.push_back(std::make_unique<TriggerIfBehavior>(
            &IsAdult, std::make_unique<SetWalkTargetAwayFrom>(MemoryModule::NearestVisibleAdultPiglin, 0.4f, 8, false)));
        idle.push_back(std::make_unique<SetEntityLookTargetSometimes>(8.0f, 30, 60));
        idle.push_back(std::make_unique<BabyFollowAdult>(kAdultFollowMin, kAdultFollowMax, 0.6f));
        idle.push_back(IdleMovement());
        brain.AddActivity(Activity::Idle, 10, std::move(idle));

        // ── FIGHT (priority 10, erases ATTACK_TARGET when it stops) ────────
        std::vector<BehaviorPtr> fight;
        fight.push_back(std::make_unique<BecomePassiveIfMemoryPresent>(MemoryModule::NearestRepellent,
                                                                       kRepellentPacifyTime));
        fight.push_back(std::make_unique<AnimalMakeLove>(EntityTypeId::Hoglin, 0.6f, 2));
        fight.push_back(std::make_unique<SetWalkTargetFromAttackTarget>(1.0f));
        fight.push_back(std::make_unique<TriggerIfBehavior>(&IsAdult, std::make_unique<MeleeAttack>(40)));
        fight.push_back(std::make_unique<TriggerIfBehavior>(&IsBabyBody, std::make_unique<MeleeAttack>(15)));
        fight.push_back(std::make_unique<StopAttackingIfTargetInvalid>());
        fight.push_back(std::make_unique<EraseMemoryIf>(
            [](LivingEntity& body) { return IsBreeding(body); }, MemoryModule::AttackTarget));
        brain.AddActivityAndRemoveMemoryWhenStopped(Activity::Fight, 10, std::move(fight),
                                                    MemoryModule::AttackTarget);

        // ── AVOID (priority 10, erases AVOID_TARGET when it stops) ─────────
        std::vector<BehaviorPtr> avoid;
        avoid.push_back(std::make_unique<SetWalkTargetAwayFrom>(MemoryModule::AvoidTarget, 1.3f, 15, false));
        avoid.push_back(IdleMovement());
        avoid.push_back(std::make_unique<SetEntityLookTargetSometimes>(8.0f, 30, 60));
        avoid.push_back(std::make_unique<EraseMemoryIf>(&WantsToStopFleeing, MemoryModule::AvoidTarget));
        brain.AddActivityAndRemoveMemoryWhenStopped(Activity::Avoid, 10, std::move(avoid),
                                                    MemoryModule::AvoidTarget);

        brain.SetCoreActivities({ Activity::Core });
        brain.SetDefaultActivity(Activity::Idle);
        brain.UseDefaultActivity();
    }

    void HoglinAi::UpdateActivity(Hoglin& hoglin) {
        Brain* brain = hoglin.GetBrain();
        if (!brain) return;
        const std::optional<Activity> oldActivity = brain->GetActiveNonCoreActivity();
        brain->SetActiveActivityToFirstValid({ Activity::Fight, Activity::Avoid, Activity::Idle });
        if (brain->GetActiveNonCoreActivity() != oldActivity) {
            hoglin.MakeSound(SoundForCurrentActivity(hoglin));
        }
        hoglin.SetAggressive(brain->HasMemoryValue(MemoryModule::AttackTarget));
    }

    const char* HoglinAi::SoundForCurrentActivity(const Hoglin& hoglin) {
        const Brain* brain = hoglin.GetBrain();
        const std::optional<Activity> activity = brain ? brain->GetActiveNonCoreActivity() : std::nullopt;
        if (!activity) return "";
        if (*activity == Activity::Avoid || hoglin.IsConverting()) return SoundEvents::HOGLIN_RETREAT;
        if (*activity == Activity::Fight) return SoundEvents::HOGLIN_ANGRY;
        return brain->HasMemoryValue(MemoryModule::NearestRepellent) ? SoundEvents::HOGLIN_RETREAT
                                                                     : SoundEvents::HOGLIN_AMBIENT;
    }

    void HoglinAi::OnHitTarget(Hoglin& attacker, LivingEntity& target) {
        if (attacker.IsBaby()) return;
        if (target.GetType() == EntityTypeId::Piglin && PiglinsOutnumberHoglins(attacker)) {
            SetAvoidTarget(attacker, target);
            BroadcastRetreat(attacker, target);
        } else {
            BroadcastAttackTarget(attacker, target);
        }
    }

    void HoglinAi::WasHurtBy(EntityLevel& level, Hoglin& hoglin, LivingEntity& attacker) {
        (void)level;
        Brain* brain = hoglin.GetBrain();
        if (!brain) return;
        brain->EraseMemory(MemoryModule::Pacified);
        brain->EraseMemory(MemoryModule::BreedTarget);
        if (hoglin.IsBaby()) {
            RetreatFromNearestTarget(hoglin, attacker);
            return;
        }
        // maybeRetaliate: not a piglin while retreating, never a hoglin, not
        // a target much further off than the current one, and attackable.
        if (brain->IsActive(Activity::Avoid) && attacker.GetType() == EntityTypeId::Piglin) return;
        if (attacker.GetType() == EntityTypeId::Hoglin) return;
        if (IsOtherTargetMuchFurtherAway(hoglin, attacker, 4.0)) return;
        if (!IsEntityAttackable(hoglin, attacker)) return;
        SetAttackTarget(hoglin, attacker);
        BroadcastAttackTarget(hoglin, attacker);
    }

} // namespace Game

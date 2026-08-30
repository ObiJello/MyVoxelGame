// File: src/common/entity/ai/brain/ZoglinAi.cpp
#include "common/entity/ai/brain/ZoglinAi.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/brain/CommonBehaviors.hpp"
#include "common/entity/ai/brain/CoreBehaviors.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"

namespace Game {

    namespace {

        // MC Zoglin.findNearestValidAttackTarget + isTargetable — the nearest
        // visible living entity that is not a zoglin, not a creeper, and
        // attackable (Sensor.isEntityAttackable's reachable half is
        // Mob::CanAttack). The check runs INSIDE the closest-first scan, as
        // in MC, so an untargetable nearest entity does not mask the next.
        LivingEntity* FindTarget(Mob& mob) {
            const Brain* brain = mob.GetBrain();
            if (!brain) return nullptr;
            const NearestVisibleLivingEntities* visible =
                brain->GetVisibleEntities(MemoryModule::NearestVisibleLivingEntities);
            if (!visible) return nullptr;
            return visible->FindClosest([&mob](LivingEntity* e) {
                return e->GetType() != EntityTypeId::Zoglin
                    && e->GetType() != EntityTypeId::Creeper
                    && mob.CanAttack(*e);
            });
        }

        // MC wraps two MeleeAttack.create calls in BehaviorBuilder.triggerIf
        // (isAdult -> 40, isBaby -> 15). The port's MeleeAttack owns its
        // cooldown, so the same split is one behaviour that reads the age at
        // swing time — the entry conditions and body are MeleeAttack's own.
        class ZoglinMeleeAttack : public Behavior {
        public:
            ZoglinMeleeAttack()
                : Behavior({ MemoryCondition{ MemoryModule::LookTarget,
                                              MemoryStatus::Registered },
                             MemoryCondition{ MemoryModule::AttackTarget,
                                              MemoryStatus::ValuePresent },
                             MemoryCondition{ MemoryModule::AttackCoolingDown,
                                              MemoryStatus::ValueAbsent },
                             MemoryCondition{ MemoryModule::NearestVisibleLivingEntities,
                                              MemoryStatus::ValuePresent } },
                           1) {}
            const char* DebugString() const override { return "ZoglinMeleeAttack"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                auto* mob = dynamic_cast<Mob*>(&body);
                Brain* brain = body.GetBrain();
                if (!mob || !brain) return false;

                auto* target = dynamic_cast<LivingEntity*>(
                    brain->GetEntity(MemoryModule::AttackTarget));
                if (!target) return false;
                if (!mob->IsWithinMeleeAttackRange(*target)) return false;

                const NearestVisibleLivingEntities* visible =
                    brain->GetVisibleEntities(MemoryModule::NearestVisibleLivingEntities);
                // MC contains applies the query-time visibility predicate too.
                if (!visible || !visible->Contains(target) ||
                    !visible->IsVisible(target)) {
                    return false;
                }

                brain->SetMemory(MemoryModule::LookTarget,
                                 PositionTracker::OfEntity(target, true));
                mob->Swing();
                mob->DoHurtTarget(*target);
                // MC ATTACK_INTERVAL 40 / BABY_ATTACK_INTERVAL 15.
                brain->SetMemoryWithExpiry(MemoryModule::AttackCoolingDown, true,
                                           body.IsBaby() ? 15 : 40);
                return true;
            }
        };

    } // namespace

    void ZoglinAi::InitBrain(Zoglin& zoglin, Brain& brain) {
        (void)zoglin;

        // MC Zoglin.MEMORY_TYPES, in declaration order.
        for (MemoryModule m : { MemoryModule::NearestLivingEntities,
                                MemoryModule::NearestVisibleLivingEntities,
                                MemoryModule::NearestVisiblePlayer,
                                MemoryModule::NearestVisibleAttackablePlayer,
                                MemoryModule::LookTarget,
                                MemoryModule::WalkTarget,
                                MemoryModule::CantReachWalkTargetSince,
                                MemoryModule::Path,
                                MemoryModule::AttackTarget,
                                MemoryModule::AttackCoolingDown }) {
            brain.RegisterMemory(m);
        }

        // MC SENSOR_TYPES: NEAREST_LIVING_ENTITIES, NEAREST_PLAYERS.
        brain.AddSensor(std::make_unique<NearestLivingEntitySensor>());
        brain.AddSensor(std::make_unique<PlayerSensor>());

        // ── CORE (MC initCoreActivity) ─────────────────────────────────────
        std::vector<BehaviorPtr> core;
        core.push_back(std::make_unique<LookAtTargetSink>(45, 90));
        core.push_back(std::make_unique<MoveToTargetSink>());
        brain.AddActivity(Activity::Core, 0, std::move(core));

        // ── IDLE (MC initIdleActivity, priority 10) ────────────────────────
        std::vector<BehaviorPtr> idle;
        idle.push_back(std::make_unique<StartAttacking>(
            [](Mob&) { return true; }, &FindTarget));
        idle.push_back(std::make_unique<SetEntityLookTargetSometimes>(8.0f, 30, 60));
        std::vector<GateBehavior::Entry> gate;
        gate.push_back({ RandomStroll::Stroll(0.4f), 2 });
        gate.push_back({ std::make_unique<SetWalkTargetFromLookTarget>(0.4f, 3), 2 });
        gate.push_back({ std::make_unique<DoNothing>(30, 60), 1 });
        idle.push_back(MakeRunOne(std::move(gate)));
        brain.AddActivity(Activity::Idle, 10, std::move(idle));

        // ── FIGHT (MC initFightActivity, priority 10) ──────────────────────
        std::vector<BehaviorPtr> fight;
        fight.push_back(std::make_unique<SetWalkTargetFromAttackTarget>(1.0f));
        fight.push_back(std::make_unique<ZoglinMeleeAttack>());
        fight.push_back(std::make_unique<StopAttackingIfTargetInvalid>());
        brain.AddActivityAndRemoveMemoryWhenStopped(Activity::Fight, 10, std::move(fight),
                                                    MemoryModule::AttackTarget);

        brain.SetCoreActivities({ Activity::Core });
        brain.SetDefaultActivity(Activity::Idle);
        brain.UseDefaultActivity();
    }

    void ZoglinAi::UpdateActivity(Zoglin& zoglin) {
        Brain* brain = zoglin.GetBrain();
        if (!brain) return;
        // MC Zoglin.updateActivity — the FIGHT-entry angry sound waits on the
        // sound system; the aggressive flag is what reaches the wire.
        brain->SetActiveActivityToFirstValid({ Activity::Fight, Activity::Idle });
        zoglin.SetAggressive(brain->HasMemoryValue(MemoryModule::AttackTarget));
    }

    void ZoglinAi::SetAttackTarget(Zoglin& zoglin, LivingEntity& target) {
        if (Brain* brain = zoglin.GetBrain()) {
            brain->EraseMemory(MemoryModule::CantReachWalkTargetSince);
            brain->SetMemoryWithExpiry(MemoryModule::AttackTarget,
                                       static_cast<Entity*>(&target), 200);
        }
    }

} // namespace Game

// File: src/common/entity/ai/brain/ZombieNautilusAi.cpp
#include "common/entity/ai/brain/ZombieNautilusAi.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/brain/CommonBehaviors.hpp"
#include "common/entity/ai/brain/CoreBehaviors.hpp"
#include "common/entity/ai/brain/NautilusAi.hpp"
#include "common/entity/mobs/Fish.hpp"

namespace Game {

    void ZombieNautilusAi::InitBrain(ZombieNautilus& nautilus, Brain& brain) {
        // MC ZombieNautilusAi.MEMORY_TYPES — the nautilus set (BREED_TARGET
        // included, though nothing writes it: the zombie variant has no
        // AnimalMakeLove).
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

        // MC SENSOR_TYPES — identical to the nautilus.
        brain.AddSensor(std::make_unique<NearestLivingEntitySensor>());
        brain.AddSensor(std::make_unique<AdultSensor>());
        brain.AddSensor(std::make_unique<PlayerSensor>());
        brain.AddSensor(std::make_unique<HurtBySensor>());
        brain.AddSensor(std::make_unique<TemptingSensor>([&nautilus](uint32_t item) {
            return nautilus.IsFood(item);
        }));

        // ── CORE (MC initCoreActivity — no AnimalPanic here) ───────────────
        std::vector<BehaviorPtr> core;
        core.push_back(std::make_unique<LookAtTargetSink>(45, 90));
        core.push_back(std::make_unique<MoveToTargetSink>());
        core.push_back(std::make_unique<CountDownCooldownTicks>(
            MemoryModule::TemptationCooldownTicks));
        core.push_back(std::make_unique<CountDownCooldownTicks>(
            MemoryModule::ChargeCooldownTicks));
        core.push_back(std::make_unique<CountDownCooldownTicks>(
            MemoryModule::AttackTargetCooldown));
        brain.AddActivity(Activity::Core, 0, std::move(core));

        // ── IDLE (MC initIdleActivity — no AnimalMakeLove) ─────────────────
        std::vector<BehaviorPtr> idle;
        idle.push_back(std::make_unique<FollowTemptation>(
            SpeedFn([](LivingEntity&) { return 0.9f; }), 3.5));
        idle.push_back(std::make_unique<StartAttacking>(
            [](Mob&) { return true; },
            &NautilusAi::FindNearestValidAttackTarget));
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

        // ── FIGHT (MC initFightActivity — 0.5 charge speed) ────────────────
        std::vector<BehaviorPtr> fight;
        fight.push_back(NautilusAi::MakeChargeAttack(0.5f));
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

    void ZombieNautilusAi::UpdateActivity(ZombieNautilus& nautilus) {
        if (Brain* brain = nautilus.GetBrain()) {
            brain->SetActiveActivityToFirstValid({ Activity::Fight, Activity::Idle });
        }
    }

} // namespace Game

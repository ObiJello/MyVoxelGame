// File: src/common/entity/ai/brain/AllayAi.cpp
#include "common/entity/ai/brain/AllayAi.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/brain/CommonBehaviors.hpp"
#include "common/entity/ai/brain/CoreBehaviors.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"

namespace Game {

    void AllayAi::InitBrain(Allay& allay, Brain& brain) {
        (void)allay;

        // MC Allay.MEMORY_TYPES, minus the item-courier set
        // (NEAREST_VISIBLE_WANTED_ITEM, LIKED_PLAYER,
        // LIKED_NOTEBLOCK_POSITION, LIKED_NOTEBLOCK_COOLDOWN_TICKS,
        // ITEM_PICKUP_COOLDOWN_TICKS) — items and noteblock game events.
        for (MemoryModule m : { MemoryModule::Path,
                                MemoryModule::LookTarget,
                                MemoryModule::NearestLivingEntities,
                                MemoryModule::NearestVisibleLivingEntities,
                                MemoryModule::WalkTarget,
                                MemoryModule::CantReachWalkTargetSince,
                                MemoryModule::HurtBy,
                                MemoryModule::HurtByEntity,
                                MemoryModule::NearestPlayers,
                                MemoryModule::NearestVisiblePlayer,
                                MemoryModule::NearestVisibleAttackablePlayer,
                                MemoryModule::NearestVisibleAttackablePlayers,
                                MemoryModule::IsPanicking }) {
            brain.RegisterMemory(m);
        }

        // MC SENSOR_TYPES: NEAREST_LIVING_ENTITIES, NEAREST_PLAYERS, HURT_BY
        // (NEAREST_ITEMS skipped — items).
        brain.AddSensor(std::make_unique<NearestLivingEntitySensor>());
        brain.AddSensor(std::make_unique<PlayerSensor>());
        brain.AddSensor(std::make_unique<HurtBySensor>());

        // ── CORE (MC initCoreActivity) ─────────────────────────────────────
        // The two CountDownCooldownTicks (liked noteblock, item pickup) are
        // skipped with their memories.
        std::vector<BehaviorPtr> core;
        core.push_back(std::make_unique<Swim>(0.8f));
        core.push_back(std::make_unique<AnimalPanic>(2.5f));
        core.push_back(std::make_unique<LookAtTargetSink>(45, 90));
        core.push_back(std::make_unique<MoveToTargetSink>());
        brain.AddActivity(Activity::Core, 0, std::move(core));

        // ── IDLE (MC initIdleActivity, explicit priority pairs) ────────────
        // Slots 0-2 (GoToWantedItem, GoAndGiveItemsToTarget,
        // StayCloseToTarget over the deposit position) are the item-courier
        // loop, skipped whole; the glances and the flying wander keep MC's
        // priorities 3 and 4.
        std::vector<BehaviorPtr> idle;
        idle.push_back(std::make_unique<SetEntityLookTargetSometimes>(6.0f, 30, 60));
        std::vector<GateBehavior::Entry> gate;
        gate.push_back({ RandomStroll::Fly(1.0f), 2 });
        gate.push_back({ std::make_unique<SetWalkTargetFromLookTarget>(1.0f, 3), 2 });
        gate.push_back({ std::make_unique<DoNothing>(30, 60), 1 });
        idle.push_back(MakeRunOne(std::move(gate)));
        brain.AddActivity(Activity::Idle, 3, std::move(idle));

        brain.SetCoreActivities({ Activity::Core });
        brain.SetDefaultActivity(Activity::Idle);
        brain.UseDefaultActivity();
    }

    void AllayAi::UpdateActivity(Allay& allay) {
        if (Brain* brain = allay.GetBrain()) {
            // MC AllayAi.updateActivity — IDLE is the only non-core activity.
            brain->SetActiveActivityToFirstValid({ Activity::Idle });
        }
    }

} // namespace Game

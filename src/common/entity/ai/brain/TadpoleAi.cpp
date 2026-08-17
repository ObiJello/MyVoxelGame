// File: src/common/entity/ai/brain/TadpoleAi.cpp
#include "common/entity/ai/brain/TadpoleAi.hpp"

#include "common/entity/ai/brain/CommonBehaviors.hpp"
#include "common/entity/ai/brain/CoreBehaviors.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/world/crafting/RecipeManager.hpp"

namespace Game {

    namespace {
        bool IsInWaterPred(LivingEntity& e) { return e.IsInWater(); }
    }

    void TadpoleAi::InitBrain(Tadpole& tadpole, Brain& brain) {
        (void)tadpole;

        for (MemoryModule m : { MemoryModule::LookTarget,
                                MemoryModule::WalkTarget,
                                MemoryModule::CantReachWalkTargetSince,
                                MemoryModule::Path,
                                MemoryModule::TemptingPlayer,
                                MemoryModule::TemptationCooldownTicks,
                                MemoryModule::IsTempted,
                                MemoryModule::HurtBy,
                                MemoryModule::HurtByEntity,
                                MemoryModule::IsPanicking }) {
            brain.RegisterMemory(m);
        }

        brain.AddSensor(std::make_unique<NearestLivingEntitySensor>());
        brain.AddSensor(std::make_unique<HurtBySensor>());
        brain.AddSensor(std::make_unique<TemptingSensor>([](uint32_t item) {
            static const ItemID slimeBall = RecipeManager::ItemFromSlug("slime_ball");
            return slimeBall != Items::Air && item == static_cast<uint32_t>(slimeBall);
        }));

        std::vector<BehaviorPtr> core;
        core.push_back(std::make_unique<AnimalPanic>(2.0f));
        core.push_back(std::make_unique<LookAtTargetSink>(45, 90));
        core.push_back(std::make_unique<MoveToTargetSink>());
        core.push_back(std::make_unique<CountDownCooldownTicks>(
            MemoryModule::TemptationCooldownTicks));
        brain.AddActivity(Activity::Core, 0, std::move(core));

        // MC's gate here is ORDERED / TRY_ALL, not a RunOne: everything that can
        // start does, which is what keeps a tadpole drifting rather than
        // choosing one thing and stopping.
        std::vector<GateBehavior::Entry> gate;
        gate.push_back({ RandomStroll::Swim(0.5f), 2 });
        gate.push_back({ std::make_unique<SetWalkTargetFromLookTarget>(0.5f, 3), 3 });
        gate.push_back({ std::make_unique<TriggerIf>(&IsInWaterPred), 5 });

        std::vector<BehaviorPtr> idle;
        idle.push_back(std::make_unique<SetEntityLookTargetSometimes>(6.0f, 30, 60));
        idle.push_back(std::make_unique<FollowTemptation>(1.25f));
        idle.push_back(std::make_unique<GateBehavior>(
            std::vector<MemoryCondition>{
                MemoryCondition{ MemoryModule::WalkTarget, MemoryStatus::ValueAbsent } },
            std::vector<MemoryModule>{},
            GateBehavior::OrderPolicy::Ordered, GateBehavior::RunningPolicy::TryAll,
            std::move(gate)));
        brain.AddActivity(Activity::Idle, 0, std::move(idle));

        brain.SetCoreActivities({ Activity::Core });
        brain.SetDefaultActivity(Activity::Idle);
        brain.UseDefaultActivity();
    }

    void TadpoleAi::UpdateActivity(Tadpole& tadpole) {
        if (Brain* brain = tadpole.GetBrain()) {
            brain->SetActiveActivityToFirstValid({ Activity::Idle });
        }
    }

} // namespace Game

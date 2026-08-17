// File: src/common/entity/ai/brain/HoglinAi.cpp
#include "common/entity/ai/brain/HoglinAi.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/brain/CommonBehaviors.hpp"
#include "common/entity/ai/brain/CoreBehaviors.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"

namespace Game {

    namespace {

        // MC's ADULT_FOLLOW_RANGE = UniformInt.of(5, 16).
        constexpr int kAdultFollowMin = 5;
        constexpr int kAdultFollowMax = 16;

        // MC HoglinAi.findNearestValidAttackTarget — the nearest visible
        // attackable PLAYER, and only when not pacified and not breeding.
        LivingEntity* FindTarget(Mob& mob) {
            const Brain* brain = mob.GetBrain();
            if (!brain) return nullptr;
            // MC's "pacified" is standing near warped fungus (NEAREST_REPELLENT);
            // that POI scan does not exist here, so the breeding half is the
            // whole of the reachable condition.
            if (brain->HasMemoryValue(MemoryModule::BreedTarget)) return nullptr;

            const NearestVisibleLivingEntities* visible =
                brain->GetVisibleEntities(MemoryModule::NearestVisibleLivingEntities);
            if (!visible) return nullptr;
            return visible->FindClosest([](LivingEntity* e) { return e->IsPlayer(); });
        }

        // MC HoglinAi.createIdleMovementBehaviors.
        BehaviorPtr IdleMovement() {
            std::vector<GateBehavior::Entry> gate;
            gate.push_back({ RandomStroll::Stroll(0.4f), 2 });
            gate.push_back({ std::make_unique<SetWalkTargetFromLookTarget>(0.4f, 3), 2 });
            gate.push_back({ std::make_unique<DoNothing>(30, 60), 1 });
            return MakeRunOne(std::move(gate));
        }

        bool IsBreeding(LivingEntity& body) {
            const Brain* brain = body.GetBrain();
            return brain && brain->HasMemoryValue(MemoryModule::BreedTarget);
        }

    } // namespace

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
                                MemoryModule::HurtBy,
                                MemoryModule::HurtByEntity,
                                MemoryModule::IsPanicking }) {
            brain.RegisterMemory(m);
        }

        brain.AddSensor(std::make_unique<NearestLivingEntitySensor>());
        brain.AddSensor(std::make_unique<HurtBySensor>());
        brain.AddSensor(std::make_unique<AdultSensor>());

        // ── CORE ───────────────────────────────────────────────────────────
        std::vector<BehaviorPtr> core;
        core.push_back(std::make_unique<LookAtTargetSink>(45, 90));
        core.push_back(std::make_unique<MoveToTargetSink>());
        brain.AddActivity(Activity::Core, 0, std::move(core));

        // ── IDLE ───────────────────────────────────────────────────────────
        //
        // MC starts this activity at priority 10, not 0. The number is not
        // decorative: priorities are global across activities, and starting
        // IDLE above FIGHT's range is what lets a fighting hoglin still run its
        // core movement without the idle set competing.
        std::vector<BehaviorPtr> idle;
        idle.push_back(std::make_unique<AnimalMakeLove>(EntityTypeId::Hoglin, 0.6f, 2));
        idle.push_back(std::make_unique<StartAttacking>(
            [](Mob&) { return true; }, &FindTarget));
        idle.push_back(std::make_unique<SetEntityLookTargetSometimes>(8.0f, 30, 60));
        idle.push_back(std::make_unique<BabyFollowAdult>(
            kAdultFollowMin, kAdultFollowMax, 0.6f));
        idle.push_back(IdleMovement());
        brain.AddActivity(Activity::Idle, 10, std::move(idle));

        // ── FIGHT ──────────────────────────────────────────────────────────
        //
        // Erases ATTACK_TARGET when it stops, so a hoglin that loses its target
        // drops straight back to IDLE instead of standing in a fight activity
        // with nothing to fight.
        std::vector<BehaviorPtr> fight;
        fight.push_back(std::make_unique<AnimalMakeLove>(EntityTypeId::Hoglin, 0.6f, 2));
        fight.push_back(std::make_unique<SetWalkTargetFromAttackTarget>(1.0f));
        // MC gives adults a 40-tick swing and babies 15. Babies are rarer than
        // the branch is cheap, so both are registered and the age decides.
        fight.push_back(std::make_unique<MeleeAttack>(40));
        fight.push_back(std::make_unique<StopAttackingIfTargetInvalid>());
        fight.push_back(std::make_unique<EraseMemoryIf>(&IsBreeding,
                                                        MemoryModule::AttackTarget));
        brain.AddActivityAndRemoveMemoryWhenStopped(Activity::Fight, 10, std::move(fight),
                                                    MemoryModule::AttackTarget);

        brain.SetCoreActivities({ Activity::Core });
        brain.SetDefaultActivity(Activity::Idle);
        brain.UseDefaultActivity();
    }

    void HoglinAi::UpdateActivity(Hoglin& hoglin) {
        if (Brain* brain = hoglin.GetBrain()) {
            brain->SetActiveActivityToFirstValid(
                { Activity::Fight, Activity::Idle });
        }
    }

} // namespace Game

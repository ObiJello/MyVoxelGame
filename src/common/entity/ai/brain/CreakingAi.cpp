// File: src/common/entity/ai/brain/CreakingAi.cpp
#include "common/entity/ai/brain/CreakingAi.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/brain/CommonBehaviors.hpp"
#include "common/entity/ai/brain/CoreBehaviors.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"

#include <algorithm>

namespace Game {

    namespace {

        // MC CreakingAi's anonymous Swim subclass — a frozen creaking does not
        // even bob to the surface.
        class CreakingSwim : public Swim {
        public:
            explicit CreakingSwim(float chance) : Swim(chance) {}
            const char* DebugString() const override { return "CreakingSwim"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                auto* creaking = dynamic_cast<Creaking*>(&body);
                return creaking && creaking->CanMove()
                    && Swim::CheckExtraStartConditions(level, body);
            }
        };

        // MC MeleeAttack.create(Creaking::canMove, 40) — the swing itself is
        // gated on the freeze, so staring one down mid-lunge stops the arm.
        class CreakingMeleeAttack : public MeleeAttack {
        public:
            explicit CreakingMeleeAttack(int cooldown) : MeleeAttack(cooldown) {}
            const char* DebugString() const override { return "CreakingMeleeAttack"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                auto* creaking = dynamic_cast<Creaking*>(&body);
                return creaking && creaking->CanMove()
                    && MeleeAttack::CheckExtraStartConditions(level, body);
            }
        };

    } // namespace

    void CreakingAi::InitBrain(Creaking& creaking, Brain& brain) {
        (void)creaking;

        // MC CreakingAi.MEMORY_TYPES.
        for (MemoryModule m : { MemoryModule::NearestLivingEntities,
                                MemoryModule::NearestVisibleLivingEntities,
                                MemoryModule::NearestVisiblePlayer,
                                MemoryModule::NearestVisibleAttackablePlayer,
                                MemoryModule::NearestVisibleAttackablePlayers,
                                MemoryModule::LookTarget,
                                MemoryModule::WalkTarget,
                                MemoryModule::CantReachWalkTargetSince,
                                MemoryModule::Path,
                                MemoryModule::AttackTarget,
                                MemoryModule::AttackCoolingDown }) {
            brain.RegisterMemory(m);
        }

        brain.AddSensor(std::make_unique<NearestLivingEntitySensor>());
        brain.AddSensor(std::make_unique<PlayerSensor>());

        // ── CORE ───────────────────────────────────────────────────────────
        std::vector<BehaviorPtr> core;
        core.push_back(std::make_unique<CreakingSwim>(0.8f));
        core.push_back(std::make_unique<LookAtTargetSink>(45, 90));
        core.push_back(std::make_unique<MoveToTargetSink>());
        brain.AddActivity(Activity::Core, 0, std::move(core));

        // ── IDLE ───────────────────────────────────────────────────────────
        std::vector<BehaviorPtr> idle;
        idle.push_back(std::make_unique<StartAttacking>(
            [](Mob& mob) {
                auto* c = dynamic_cast<Creaking*>(&mob);
                return c && c->IsActive();
            },
            [](Mob& mob) -> LivingEntity* {
                Brain* b = mob.GetBrain();
                return b ? dynamic_cast<LivingEntity*>(b->GetEntity(
                               MemoryModule::NearestVisibleAttackablePlayer))
                         : nullptr;
            }));
        idle.push_back(std::make_unique<SetEntityLookTargetSometimes>(8.0f, 30, 60));
        std::vector<GateBehavior::Entry> idleGate;
        idleGate.push_back({ RandomStroll::Stroll(0.3f), 2 });
        idleGate.push_back({ std::make_unique<SetWalkTargetFromLookTarget>(0.3f, 3), 2 });
        idleGate.push_back({ std::make_unique<DoNothing>(30, 60), 1 });
        idle.push_back(MakeRunOne(std::move(idleGate)));
        brain.AddActivity(Activity::Idle, 10, std::move(idle));

        // ── FIGHT ──────────────────────────────────────────────────────────
        std::vector<BehaviorPtr> fight;
        fight.push_back(std::make_unique<SetWalkTargetFromAttackTarget>(1.0f));
        fight.push_back(std::make_unique<CreakingMeleeAttack>(40));
        // MC StopAttackingIfTargetInvalid.create(!isAttackTargetStillReachable):
        // the target stays valid only while it is still one of
        // NEAREST_VISIBLE_ATTACKABLE_PLAYERS.
        fight.push_back(std::make_unique<StopAttackingIfTargetInvalid>(
            [](EntityLevel&, Mob& body, LivingEntity& target) {
                const Brain* b = body.GetBrain();
                const std::vector<Entity*>* players =
                    b ? b->GetEntityList(MemoryModule::NearestVisibleAttackablePlayers) : nullptr;
                if (!players || !target.IsPlayer()) return true;
                return std::find(players->begin(), players->end(),
                                 static_cast<Entity*>(&target)) == players->end();
            }));
        brain.AddActivityWithConditions(
            Activity::Fight, 10, std::move(fight),
            { MemoryCondition{ MemoryModule::AttackTarget, MemoryStatus::ValuePresent } });

        brain.SetCoreActivities({ Activity::Core });
        brain.SetDefaultActivity(Activity::Idle);
        brain.UseDefaultActivity();
    }

    void CreakingAi::UpdateActivity(Creaking& creaking) {
        Brain* brain = creaking.GetBrain();
        if (!brain) return;
        // MC: a frozen creaking drops to the default (IDLE) activity — the
        // fight resumes the moment everyone looks away.
        if (!creaking.CanMove()) {
            brain->UseDefaultActivity();
        } else {
            brain->SetActiveActivityToFirstValid({ Activity::Fight, Activity::Idle });
        }
    }

} // namespace Game

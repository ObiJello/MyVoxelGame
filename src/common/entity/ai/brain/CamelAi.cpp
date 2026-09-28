// File: src/common/entity/ai/brain/CamelAi.cpp
#include "common/entity/ai/brain/CamelAi.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/brain/CommonBehaviors.hpp"
#include "common/entity/ai/brain/CoreBehaviors.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"

namespace Game {

    namespace {

        // MC CamelAi.CamelPanic — AnimalPanic that stands the camel up first,
        // because a sitting camel cannot flee.
        class CamelPanic : public AnimalPanic {
        public:
            explicit CamelPanic(float speed) : AnimalPanic(speed) {}
            const char* DebugString() const override { return "CamelPanic"; }
        protected:
            // MC CamelPanic.checkExtraStartConditions: not while a mob rides
            // it (the camel husk under its husk).
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                const auto* camel = dynamic_cast<const Camel*>(&body);
                return AnimalPanic::CheckExtraStartConditions(level, body) && !(camel && camel->IsMobControlled());
            }
            void Start(EntityLevel& level, LivingEntity& body, int64_t t) override {
                if (auto* camel = dynamic_cast<Camel*>(&body)) camel->StandUpInstantly();
                AnimalPanic::Start(level, body, t);
            }
        };

        // MC CamelAi.RandomSitting — the behaviour that makes camels sit down
        // on their own. It TOGGLES: a standing camel that has held its pose for
        // 20 seconds lies down, and a sitting one gets back up.
        class RandomSitting : public Behavior {
        public:
            explicit RandomSitting(int minimalPoseTimeSec)
                : Behavior({}), m_minimalPoseTicks(minimalPoseTimeSec * 20) {}
            const char* DebugString() const override { return "RandomSitting"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                auto* camel = dynamic_cast<Camel*>(&body);
                if (!camel) return false;
                // MC: never in water, on a lead, mid-air or under a steering
                // rider, and only with room for the other pose.
                return !camel->IsInWater()
                    && camel->GetPoseTime() >= m_minimalPoseTicks
                    && !camel->IsLeashed()
                    && camel->onGround
                    && !camel->HasControllingPassenger()
                    && camel->CanCamelChangePose();
            }

            void Start(EntityLevel&, LivingEntity& body, int64_t) override {
                auto* camel = dynamic_cast<Camel*>(&body);
                if (!camel) return;
                if (camel->IsCamelSitting()) camel->StandUp();
                else if (!camel->IsCamelPanicking()) camel->SitDown();
            }

        private:
            int m_minimalPoseTicks;
        };

        // MC BehaviorBuilder.triggerIf(Predicate.not(Camel::refuseToMove), inner)
        // — a wrapper that only lets its child run while the camel is willing to
        // move. Without it a sitting camel would keep setting walk targets and
        // the MoveToTargetSink would fight the sitting pose every tick.
        class IfWillingToMove : public Behavior {
        public:
            explicit IfWillingToMove(BehaviorPtr inner)
                : Behavior({}, 1), m_inner(std::move(inner)) {}
            const char* DebugString() const override { return "IfWillingToMove"; }
            void ClearReferenceTo(const Entity* e) override { m_inner->ClearReferenceTo(e); }

        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                auto* camel = dynamic_cast<Camel*>(&body);
                if (!camel || camel->RefuseToMove()) return false;
                return m_inner->TryStart(level, body, 0);
            }

        private:
            BehaviorPtr m_inner;
        };

    } // namespace

    void CamelAi::InitBrain(Camel& camel, Brain& brain) {

        // MC Camel.MEMORY_TYPES.
        for (MemoryModule m : { MemoryModule::IsPanicking,
                                MemoryModule::HurtBy,
                                MemoryModule::HurtByEntity,
                                MemoryModule::WalkTarget,
                                MemoryModule::LookTarget,
                                MemoryModule::CantReachWalkTargetSince,
                                MemoryModule::Path,
                                MemoryModule::TemptingPlayer,
                                MemoryModule::TemptationCooldownTicks,
                                MemoryModule::GazeCooldownTicks,
                                MemoryModule::IsTempted,
                                MemoryModule::BreedTarget,
                                MemoryModule::NearestVisibleAdult }) {
            brain.RegisterMemory(m);
        }

        brain.AddSensor(std::make_unique<NearestLivingEntitySensor>());
        brain.AddSensor(std::make_unique<HurtBySensor>());
        // MC SENSOR_TYPES: NEAREST_LIVING_ENTITIES, HURT_BY,
        // FOOD_TEMPTATIONS (Camel.isFood — #camel_food cactus, the husk's
        // #camel_husk_food rabbit foot), NEAREST_ADULT.
        brain.AddSensor(std::make_unique<TemptingSensor>([&camel](uint32_t item) {
            return camel.IsFood(item);
        }));
        brain.AddSensor(std::make_unique<AdultSensor>());

        // ── CORE ───────────────────────────────────────────────────────────
        std::vector<BehaviorPtr> core;
        core.push_back(std::make_unique<Swim>(0.8f));
        core.push_back(std::make_unique<CamelPanic>(4.0f));
        core.push_back(std::make_unique<LookAtTargetSink>(45, 90));
        core.push_back(std::make_unique<MoveToTargetSink>());
        core.push_back(std::make_unique<CountDownCooldownTicks>(
            MemoryModule::TemptationCooldownTicks));
        core.push_back(std::make_unique<CountDownCooldownTicks>(
            MemoryModule::GazeCooldownTicks));
        brain.AddActivity(Activity::Core, 0, std::move(core));

        // ── IDLE ───────────────────────────────────────────────────────────
        //
        // MC's priority-2 gate: FollowTemptation(2.5, baby ? 2.5 : 3.5) or,
        // for a calf willing to move, BabyFollowAdult(5..16, 2.5). (The
        // behaviour's close-enough distance is fixed at construction, where
        // every camel is still an adult: the adult's 3.5.)
        std::vector<GateBehavior::Entry> temptGate;
        temptGate.push_back({ std::make_unique<FollowTemptation>(2.5f, 3.5), 1 });
        temptGate.push_back({ std::make_unique<IfWillingToMove>(
                                  std::make_unique<BabyFollowAdult>(5, 16, 2.5f)), 1 });

        // MC's priority-4 gate: stroll, look-walk, sit, or stand there.
        std::vector<GateBehavior::Entry> idleGate;
        idleGate.push_back({ std::make_unique<IfWillingToMove>(
                                 RandomStroll::Stroll(2.0f)), 1 });
        idleGate.push_back({ std::make_unique<IfWillingToMove>(
                                 std::make_unique<SetWalkTargetFromLookTarget>(2.0f, 3)), 1 });
        idleGate.push_back({ std::make_unique<RandomSitting>(20), 1 });
        idleGate.push_back({ std::make_unique<DoNothing>(30, 60), 1 });

        std::vector<BehaviorPtr> idle;
        idle.push_back(std::make_unique<SetEntityLookTargetSometimes>(6.0f, 30, 60));
        idle.push_back(std::make_unique<AnimalMakeLove>(EntityTypeId::Camel));
        idle.push_back(MakeRunOne(std::move(temptGate)));
        idle.push_back(std::make_unique<RandomLookAround>(150, 250, 30.0f, 0.0f, 0.0f));
        idle.push_back(MakeRunOne(
            { MemoryCondition{ MemoryModule::WalkTarget, MemoryStatus::ValueAbsent } },
            std::move(idleGate)));
        brain.AddActivity(Activity::Idle, 0, std::move(idle));

        brain.SetCoreActivities({ Activity::Core });
        brain.SetDefaultActivity(Activity::Idle);
        brain.UseDefaultActivity();
    }

    void CamelAi::UpdateActivity(Camel& camel) {
        if (Brain* brain = camel.GetBrain()) {
            brain->SetActiveActivityToFirstValid({ Activity::Idle });
        }
    }

} // namespace Game

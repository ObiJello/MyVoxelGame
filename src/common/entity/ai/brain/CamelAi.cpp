// File: src/common/entity/ai/brain/CamelAi.cpp
#include "common/entity/ai/brain/CamelAi.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/brain/CommonBehaviors.hpp"
#include "common/entity/ai/brain/CoreBehaviors.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/world/crafting/RecipeManager.hpp"

namespace Game {

    namespace {

        // MC CamelAi.CamelPanic — AnimalPanic that stands the camel up first,
        // because a sitting camel cannot flee.
        class CamelPanic : public AnimalPanic {
        public:
            explicit CamelPanic(float speed) : AnimalPanic(speed) {}
            const char* DebugString() const override { return "CamelPanic"; }
        protected:
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
                // MC also excludes leashed and ridden camels; neither exists.
                return !camel->IsInWater()
                    && camel->GetPoseTime() >= m_minimalPoseTicks
                    && camel->onGround;
            }

            void Start(EntityLevel&, LivingEntity& body, int64_t) override {
                auto* camel = dynamic_cast<Camel*>(&body);
                if (!camel) return;
                if (camel->IsCamelSitting()) camel->StandUp();
                else if (!camel->IsPanicking()) camel->SitDown();
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
        (void)camel;

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
                                MemoryModule::BreedTarget }) {
            brain.RegisterMemory(m);
        }

        brain.AddSensor(std::make_unique<NearestLivingEntitySensor>());
        brain.AddSensor(std::make_unique<HurtBySensor>());
        brain.AddSensor(std::make_unique<TemptingSensor>([](uint32_t item) {
            // MC ItemTags.CAMEL_FOOD — cactus.
            static const ItemID cactus = RecipeManager::ItemFromSlug("cactus");
            return cactus != Items::Air && item == static_cast<uint32_t>(cactus);
        }));

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
        // MC's priority-2 gate also carries BabyFollowAdult, which needs the
        // NEAREST_VISIBLE_ADULT sensor this port has not got. Its WEIGHT is left
        // out with it rather than silently redistributed onto the temptation.
        std::vector<GateBehavior::Entry> temptGate;
        temptGate.push_back({ std::make_unique<FollowTemptation>(2.5f, 3.5), 1 });

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

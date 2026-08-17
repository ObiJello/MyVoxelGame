// File: src/common/entity/ai/brain/GoatAi.cpp
#include "common/entity/ai/brain/GoatAi.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/brain/CommonBehaviors.hpp"
#include "common/entity/ai/brain/CoreBehaviors.hpp"
#include "common/entity/ai/goals/LongJumpGoal.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/world/crafting/RecipeManager.hpp"

namespace Game {

    namespace {

        constexpr int kAdultFollowMin = 5;
        constexpr int kAdultFollowMax = 16;

        // MC LongJumpToRandomPos with the goat's numbers — the plain variant,
        // not the frog's preferred-block one. Drives the same LongJumpGoal
        // ballistics, so the arcs come out of identical code.
        class GoatLongJump : public Behavior {
        public:
            GoatLongJump()
                : Behavior({ MemoryCondition{ MemoryModule::LookTarget,
                                              MemoryStatus::Registered },
                             MemoryCondition{ MemoryModule::LongJumpCooldownTicks,
                                              MemoryStatus::ValueAbsent },
                             MemoryCondition{ MemoryModule::LongJumpMidJump,
                                              MemoryStatus::ValueAbsent } },
                           200) {}

            const char* DebugString() const override { return "GoatLongJump"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                EnsureGoal(body);
                return m_goal && m_goal->CanUse();
            }
            bool CanStillUse(EntityLevel&, LivingEntity&, int64_t) override {
                return m_goal && m_goal->CanContinueToUse();
            }
            void Start(EntityLevel&, LivingEntity&, int64_t) override {
                if (m_goal) m_goal->Start();
            }
            void Tick(EntityLevel&, LivingEntity& body, int64_t) override {
                if (!m_goal) return;
                m_goal->Tick();
                if (!body.onGround) {
                    if (Brain* brain = body.GetBrain()) {
                        brain->SetMemory(MemoryModule::LongJumpMidJump, true);
                    }
                }
            }
            void Stop(EntityLevel&, LivingEntity&, int64_t) override {
                if (m_goal) m_goal->Stop();
            }

        private:
            void EnsureGoal(LivingEntity& body) {
                if (m_goal) return;
                auto* mob = dynamic_cast<Mob*>(&body);
                if (!mob) return;
                LongJumpGoal::Config cfg;
                cfg.cooldownMin = GoatAi::kTimeBetweenLongJumpsMin;
                cfg.cooldownMax = GoatAi::kTimeBetweenLongJumpsMax;
                cfg.maxHeight = 5;                 // MC MAX_LONG_JUMP_HEIGHT
                cfg.maxWidth = 5;                  // MC MAX_LONG_JUMP_WIDTH
                cfg.maxJumpVelocityMultiplier = 3.5714288f;
                // The brain owns the cooldown through LONG_JUMP_COOLDOWN_TICKS.
                cfg.ownsCooldown = false;
                m_goal = std::make_unique<LongJumpGoal>(mob, cfg);
            }

            std::unique_ptr<LongJumpGoal> m_goal;
        };

    } // namespace

    void GoatAi::InitMemories(Goat& goat) {
        EntityLevel* level = goat.Level();
        if (Brain* brain = goat.GetBrain()) {
            brain->SetMemory(MemoryModule::LongJumpCooldownTicks,
                             level ? level->Random().NextInt(kTimeBetweenLongJumpsMin,
                                                             kTimeBetweenLongJumpsMax)
                                   : kTimeBetweenLongJumpsMin);
        }
    }

    void GoatAi::InitBrain(Goat& goat, Brain& brain) {
        (void)goat;

        for (MemoryModule m : { MemoryModule::LookTarget,
                                MemoryModule::WalkTarget,
                                MemoryModule::CantReachWalkTargetSince,
                                MemoryModule::Path,
                                MemoryModule::BreedTarget,
                                MemoryModule::NearestVisibleAdult,
                                MemoryModule::LongJumpCooldownTicks,
                                MemoryModule::LongJumpMidJump,
                                MemoryModule::RamCooldownTicks,
                                MemoryModule::RamTarget,
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
        brain.AddSensor(std::make_unique<AdultSensor>());
        brain.AddSensor(std::make_unique<TemptingSensor>([](uint32_t item) {
            // MC ItemTags.GOAT_FOOD — wheat.
            static const ItemID wheat = RecipeManager::ItemFromSlug("wheat");
            return wheat != Items::Air && item == static_cast<uint32_t>(wheat);
        }));

        // ── CORE ───────────────────────────────────────────────────────────
        std::vector<BehaviorPtr> core;
        core.push_back(std::make_unique<Swim>(0.8f));
        core.push_back(std::make_unique<AnimalPanic>(2.0f));
        core.push_back(std::make_unique<LookAtTargetSink>(45, 90));
        core.push_back(std::make_unique<MoveToTargetSink>());
        core.push_back(std::make_unique<CountDownCooldownTicks>(
            MemoryModule::TemptationCooldownTicks));
        core.push_back(std::make_unique<CountDownCooldownTicks>(
            MemoryModule::LongJumpCooldownTicks));
        core.push_back(std::make_unique<CountDownCooldownTicks>(
            MemoryModule::RamCooldownTicks));
        brain.AddActivity(Activity::Core, 0, std::move(core));

        // ── IDLE ───────────────────────────────────────────────────────────
        std::vector<GateBehavior::Entry> gate;
        gate.push_back({ RandomStroll::Stroll(1.0f), 2 });
        gate.push_back({ std::make_unique<SetWalkTargetFromLookTarget>(1.0f, 3), 2 });
        gate.push_back({ std::make_unique<DoNothing>(30, 60), 1 });

        std::vector<BehaviorPtr> idle;
        idle.push_back(std::make_unique<SetEntityLookTargetSometimes>(6.0f, 30, 60));
        idle.push_back(std::make_unique<AnimalMakeLove>(EntityTypeId::Goat));
        idle.push_back(std::make_unique<FollowTemptation>(1.25f));
        idle.push_back(std::make_unique<BabyFollowAdult>(
            kAdultFollowMin, kAdultFollowMax, 1.25f));
        idle.push_back(MakeRunOne(std::move(gate)));
        brain.AddActivityWithConditions(
            Activity::Idle, 0, std::move(idle),
            { MemoryCondition{ MemoryModule::RamTarget, MemoryStatus::ValueAbsent },
              MemoryCondition{ MemoryModule::LongJumpMidJump, MemoryStatus::ValueAbsent } });

        // ── LONG_JUMP ──────────────────────────────────────────────────────
        std::vector<BehaviorPtr> jump;
        jump.push_back(std::make_unique<LongJumpMidJump>(kTimeBetweenLongJumpsMin,
                                                         kTimeBetweenLongJumpsMax));
        jump.push_back(std::make_unique<GoatLongJump>());
        brain.AddActivityWithConditions(
            Activity::LongJump, 0, std::move(jump),
            { MemoryCondition{ MemoryModule::TemptingPlayer, MemoryStatus::ValueAbsent },
              MemoryCondition{ MemoryModule::BreedTarget, MemoryStatus::ValueAbsent },
              MemoryCondition{ MemoryModule::WalkTarget, MemoryStatus::ValueAbsent },
              MemoryCondition{ MemoryModule::LongJumpCooldownTicks,
                               MemoryStatus::ValueAbsent } });

        // MC's RAM activity is deliberately NOT here: RamTarget and
        // PrepareRamNearestTarget are two bespoke behaviours plus a knockback
        // and horn-break path, and a half-built ram would be a goat that charges
        // and does nothing. RAM_TARGET stays registered so IDLE's condition on
        // it is meaningful, and the activity is simply never valid.

        brain.SetCoreActivities({ Activity::Core });
        brain.SetDefaultActivity(Activity::Idle);
        brain.UseDefaultActivity();
    }

    void GoatAi::UpdateActivity(Goat& goat) {
        if (Brain* brain = goat.GetBrain()) {
            // MC's list includes RAM first; naming an activity that was never
            // added is harmless — its requirements can never be met.
            brain->SetActiveActivityToFirstValid(
                { Activity::Ram, Activity::LongJump, Activity::Idle });
        }
    }

} // namespace Game

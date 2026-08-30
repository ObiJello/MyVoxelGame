// File: src/common/entity/ai/brain/ArmadilloAi.cpp
#include "common/entity/ai/brain/ArmadilloAi.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/brain/CommonBehaviors.hpp"
#include "common/entity/ai/brain/CoreBehaviors.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"

namespace Game {

    namespace {

        // MC ArmadilloAi.ArmadilloPanic — AnimalPanic that rolls out first
        // (panic and the ball are mutually exclusive; PANIC's activity
        // condition requires IS_PANICKING absent for the same reason).
        class ArmadilloPanic : public AnimalPanic {
        public:
            explicit ArmadilloPanic(float speedMultiplier)
                : AnimalPanic(speedMultiplier) {}
            const char* DebugString() const override { return "ArmadilloPanic"; }
        protected:
            void Start(EntityLevel& level, LivingEntity& body, int64_t timestamp) override {
                if (auto* armadillo = dynamic_cast<Armadillo*>(&body)) {
                    armadillo->RollOut();
                }
                AnimalPanic::Start(level, body, timestamp);
            }
        };

        // MC's anonymous MoveToTargetSink override in initCoreActivity — a
        // scared armadillo does not walk.
        class ArmadilloMoveToTargetSink : public MoveToTargetSink {
        public:
            const char* DebugString() const override { return "ArmadilloMoveToTargetSink"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                auto* armadillo = dynamic_cast<Armadillo*>(&body);
                if (armadillo && armadillo->IsScared()) return false;
                return MoveToTargetSink::CheckExtraStartConditions(level, body);
            }
        };

        // MC ArmadilloAi.ARMADILLO_ROLLING_OUT — a core one-shot: with the
        // danger memory gone, a scared armadillo unrolls.
        class ArmadilloRollingOut : public Behavior {
        public:
            ArmadilloRollingOut()
                : Behavior({ MemoryCondition{ MemoryModule::DangerDetectedRecently,
                                              MemoryStatus::ValueAbsent } },
                           1) {}
            const char* DebugString() const override { return "ArmadilloRollingOut"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                auto* armadillo = dynamic_cast<Armadillo*>(&body);
                if (!armadillo || !armadillo->IsScared()) return false;
                armadillo->RollOut();
                return true;
            }
        };

        // MC ArmadilloAi.ArmadilloBallUp — the whole rolled-up life: the
        // Rolling→Scared transition, the peek cycle (entity event 64 on a
        // 100-400 tick jitter over the scared clip length, re-rolled whenever
        // the danger reading flips), and the Scared↔Unrolling flicker as the
        // 80-tick danger memory drains and refills.
        class ArmadilloBallUp : public Behavior {
        public:
            // MC BALL_UP_STAY_IN_STATE = 5 minutes.
            ArmadilloBallUp() : Behavior({}, 5 * 60 * 20) {}
            const char* DebugString() const override { return "ArmadilloBallUp"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                return body.onGround;
            }
            bool CanStillUse(EntityLevel&, LivingEntity& body, int64_t) override {
                auto* armadillo = dynamic_cast<Armadillo*>(&body);
                return armadillo && Armadillo::IsThreatened(armadillo->GetState());
            }
            void Start(EntityLevel&, LivingEntity& body, int64_t) override {
                if (auto* armadillo = dynamic_cast<Armadillo*>(&body)) {
                    armadillo->RollUp();
                }
            }
            void Stop(EntityLevel&, LivingEntity& body, int64_t) override {
                auto* armadillo = dynamic_cast<Armadillo*>(&body);
                if (armadillo && !armadillo->CanStayRolledUp()) {
                    armadillo->RollOut();
                }
            }
            void Tick(EntityLevel& level, LivingEntity& body, int64_t) override {
                auto* armadillo = dynamic_cast<Armadillo*>(&body);
                if (!armadillo) return;
                if (m_nextPeekTimer > 0) --m_nextPeekTimer;

                if (armadillo->ShouldSwitchToScaredState()) {
                    armadillo->SwitchToState(Armadillo::State::Scared);
                    // The ARMADILLO_LAND sound waits on the sound system.
                    return;
                }

                const Armadillo::State state = armadillo->GetState();
                const int64_t dangerTicks = armadillo->DangerTicksRemaining();
                const bool dangerIsAround = dangerTicks > kDangerThreshold;
                if (dangerIsAround != m_dangerWasAround) {
                    m_nextPeekTimer = PickNextPeekTimer(level);
                }
                m_dangerWasAround = dangerIsAround;

                if (state == Armadillo::State::Scared) {
                    if (m_nextPeekTimer == 0 && armadillo->onGround && dangerIsAround) {
                        // MC broadcasts entity event 64; the client turns it
                        // into one peek, which is why the armadillo pokes its
                        // head out at intervals instead of continuously.
                        level.BroadcastEntityEvent(*armadillo, 64);
                        m_nextPeekTimer = PickNextPeekTimer(level);
                    }
                    if (dangerTicks
                        < Armadillo::AnimationDuration(Armadillo::State::Unrolling)) {
                        // The ARMADILLO_UNROLL_START sound waits on the sound
                        // system.
                        armadillo->SwitchToState(Armadillo::State::Unrolling);
                    }
                } else if (state == Armadillo::State::Unrolling
                           && dangerTicks
                              > Armadillo::AnimationDuration(Armadillo::State::Unrolling)) {
                    armadillo->SwitchToState(Armadillo::State::Scared);
                }
            }

        private:
            // MC ArmadilloBallUp.DANGER_DETECTED_RECENTLY_DANGER_THRESHOLD.
            static constexpr int kDangerThreshold = 75;

            static int PickNextPeekTimer(EntityLevel& level) {
                return Armadillo::AnimationDuration(Armadillo::State::Scared)
                     + level.Random().NextInt(100, 400);
            }

            int  m_nextPeekTimer = 0;
            bool m_dangerWasAround = false;
        };

        // MC SensorType.ARMADILLO_SCARE_DETECTED — a MobSensor(scanRate 5):
        // when canStayRolledUp fails the danger memory is ERASED outright (an
        // armadillo that falls into water unrolls immediately); otherwise any
        // entity in the raw NEAREST_LIVING_ENTITIES list passing isScaredBy
        // re-arms DANGER_DETECTED_RECENTLY for 80 ticks.
        class ArmadilloScareDetectedSensor : public Sensor {
        public:
            ArmadilloScareDetectedSensor() : Sensor(5) {}
            std::vector<MemoryModule> Requires() const override {
                return { MemoryModule::DangerDetectedRecently };
            }
        protected:
            void DoTick(EntityLevel&, LivingEntity& body) override {
                auto* armadillo = dynamic_cast<Armadillo*>(&body);
                Brain* brain = body.GetBrain();
                if (!armadillo || !brain) return;
                if (!armadillo->CanStayRolledUp()) {
                    brain->EraseMemory(MemoryModule::DangerDetectedRecently);
                    return;
                }
                const std::vector<Entity*>* nearby =
                    brain->GetEntityList(MemoryModule::NearestLivingEntities);
                if (!nearby) return;
                for (Entity* e : *nearby) {
                    auto* living = dynamic_cast<LivingEntity*>(e);
                    if (living && armadillo->IsScaredBy(*living)) {
                        brain->SetMemoryWithExpiry(
                            MemoryModule::DangerDetectedRecently, true, 80);
                        return;
                    }
                }
            }
        };

    } // namespace

    void ArmadilloAi::InitBrain(Armadillo& armadillo, Brain& brain) {
        // MC ArmadilloAi.MEMORY_TYPES, in declaration order.
        for (MemoryModule m : { MemoryModule::IsPanicking,
                                MemoryModule::HurtBy,
                                MemoryModule::HurtByEntity,
                                MemoryModule::WalkTarget,
                                MemoryModule::LookTarget,
                                MemoryModule::CantReachWalkTargetSince,
                                MemoryModule::Path,
                                MemoryModule::NearestLivingEntities,
                                MemoryModule::NearestVisibleLivingEntities,
                                MemoryModule::TemptingPlayer,
                                MemoryModule::TemptationCooldownTicks,
                                MemoryModule::GazeCooldownTicks,
                                MemoryModule::IsTempted,
                                MemoryModule::BreedTarget,
                                MemoryModule::NearestVisibleAdult,
                                MemoryModule::DangerDetectedRecently }) {
            brain.RegisterMemory(m);
        }

        // MC SENSOR_TYPES: NEAREST_LIVING_ENTITIES, HURT_BY,
        // FOOD_TEMPTATIONS, NEAREST_ADULT, ARMADILLO_SCARE_DETECTED.
        brain.AddSensor(std::make_unique<NearestLivingEntitySensor>());
        brain.AddSensor(std::make_unique<HurtBySensor>());
        brain.AddSensor(std::make_unique<TemptingSensor>([&armadillo](uint32_t item) {
            // ItemTags.ARMADILLO_FOOD (spider eye) — the def's food list.
            return armadillo.IsFood(item);
        }));
        brain.AddSensor(std::make_unique<AdultSensor>());
        brain.AddSensor(std::make_unique<ArmadilloScareDetectedSensor>());

        // ── CORE (MC initCoreActivity) ─────────────────────────────────────
        std::vector<BehaviorPtr> core;
        core.push_back(std::make_unique<Swim>(0.8f));
        core.push_back(std::make_unique<ArmadilloPanic>(2.0f));
        core.push_back(std::make_unique<LookAtTargetSink>(45, 90));
        core.push_back(std::make_unique<ArmadilloMoveToTargetSink>());
        core.push_back(std::make_unique<CountDownCooldownTicks>(
            MemoryModule::TemptationCooldownTicks));
        core.push_back(std::make_unique<CountDownCooldownTicks>(
            MemoryModule::GazeCooldownTicks));
        core.push_back(std::make_unique<ArmadilloRollingOut>());
        brain.AddActivity(Activity::Core, 0, std::move(core));

        // ── IDLE (MC initIdleActivity, explicit priority pairs) ────────────
        std::vector<BehaviorPtr> idle;
        idle.push_back(std::make_unique<SetEntityLookTargetSometimes>(6.0f, 30, 60));
        idle.push_back(std::make_unique<AnimalMakeLove>(EntityTypeId::Armadillo,
                                                        1.0f, 1));
        std::vector<GateBehavior::Entry> follow;
        // MC FollowTemptation(1.25, baby ? 1.0 : 2.0) — the adult's 2.0 is
        // the fixed close-enough here.
        follow.push_back({ std::make_unique<FollowTemptation>(1.25f, 2.0), 1 });
        follow.push_back({ std::make_unique<BabyFollowAdult>(5, 16, 1.25f), 1 });
        idle.push_back(MakeRunOne(std::move(follow)));
        idle.push_back(std::make_unique<RandomLookAround>(150, 250, 30.0f, 0.0f, 0.0f));
        std::vector<GateBehavior::Entry> move;
        move.push_back({ RandomStroll::Stroll(1.0f), 1 });
        move.push_back({ std::make_unique<SetWalkTargetFromLookTarget>(1.0f, 3), 1 });
        move.push_back({ std::make_unique<DoNothing>(30, 60), 1 });
        idle.push_back(MakeRunOne(
            { MemoryCondition{ MemoryModule::WalkTarget, MemoryStatus::ValueAbsent } },
            std::move(move)));
        brain.AddActivity(Activity::Idle, 0, std::move(idle));

        // ── PANIC / scared (MC initScaredActivity) ─────────────────────────
        // The ball-up owns the activity; real panic (IS_PANICKING) blocks it,
        // because a burning armadillo must run, not hide.
        std::vector<BehaviorPtr> scared;
        scared.push_back(std::make_unique<ArmadilloBallUp>());
        brain.AddActivityWithConditions(
            Activity::Panic, 0, std::move(scared),
            { MemoryCondition{ MemoryModule::DangerDetectedRecently,
                               MemoryStatus::ValuePresent },
              MemoryCondition{ MemoryModule::IsPanicking,
                               MemoryStatus::ValueAbsent } });

        brain.SetCoreActivities({ Activity::Core });
        brain.SetDefaultActivity(Activity::Idle);
        brain.UseDefaultActivity();
    }

    void ArmadilloAi::UpdateActivity(Armadillo& armadillo) {
        if (Brain* brain = armadillo.GetBrain()) {
            brain->SetActiveActivityToFirstValid({ Activity::Panic, Activity::Idle });
        }
    }

} // namespace Game

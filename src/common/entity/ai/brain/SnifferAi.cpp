// File: src/common/entity/ai/brain/SnifferAi.cpp
#include "common/entity/ai/brain/SnifferAi.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/brain/CommonBehaviors.hpp"
#include "common/entity/ai/brain/CoreBehaviors.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"

namespace Game {

    namespace {

        // MC SnifferAi.resetSniffing — every interruption (panic, breeding,
        // temptation, a dig cut short) funnels through this.
        Sniffer& ResetSniffing(Sniffer& sniffer) {
            if (Brain* brain = sniffer.GetBrain()) {
                brain->EraseMemory(MemoryModule::SnifferDigging);
                brain->EraseMemory(MemoryModule::SnifferSniffingTarget);
            }
            return sniffer.TransitionTo(Sniffer::State::Idling);
        }

        // MC's three anonymous "reset first" subclasses.

        class SnifferPanic : public AnimalPanic {
        public:
            explicit SnifferPanic(float speed) : AnimalPanic(speed) {}
            const char* DebugString() const override { return "SnifferPanic"; }
        protected:
            void Start(EntityLevel& level, LivingEntity& body, int64_t t) override {
                if (auto* sniffer = dynamic_cast<Sniffer*>(&body)) ResetSniffing(*sniffer);
                AnimalPanic::Start(level, body, t);
            }
        };

        class SnifferMakeLove : public AnimalMakeLove {
        public:
            SnifferMakeLove() : AnimalMakeLove(EntityTypeId::Sniffer) {}
            const char* DebugString() const override { return "SnifferMakeLove"; }
        protected:
            void Start(EntityLevel& level, LivingEntity& body, int64_t t) override {
                if (auto* sniffer = dynamic_cast<Sniffer*>(&body)) ResetSniffing(*sniffer);
                AnimalMakeLove::Start(level, body, t);
            }
        };

        class SnifferFollowTemptation : public FollowTemptation {
        public:
            // MC: speed 1.25, closeEnough 3.5 (2.5 for a baby — the adult
            // value stands in; the half-block only shows while hand-feeding a
            // baby).
            SnifferFollowTemptation() : FollowTemptation(1.25f, 3.5) {}
            const char* DebugString() const override { return "SnifferFollowTemptation"; }
        protected:
            void Start(EntityLevel& level, LivingEntity& body, int64_t t) override {
                if (auto* sniffer = dynamic_cast<Sniffer*>(&body)) ResetSniffing(*sniffer);
                FollowTemptation::Start(level, body, t);
            }
        };

        // ── Scenting (MC SnifferAi.Scenting) ───────────────────────────────
        class Scenting : public Behavior {
        public:
            Scenting(int min, int max)
                : Behavior({ { MemoryModule::IsPanicking, MemoryStatus::ValueAbsent },
                             { MemoryModule::SnifferDigging, MemoryStatus::ValueAbsent },
                             { MemoryModule::SnifferSniffingTarget, MemoryStatus::ValueAbsent },
                             { MemoryModule::SnifferHappy, MemoryStatus::ValueAbsent },
                             { MemoryModule::BreedTarget, MemoryStatus::ValueAbsent } },
                           min, max) {}
            const char* DebugString() const override { return "SnifferScenting"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                auto* sniffer = dynamic_cast<Sniffer*>(&body);
                return sniffer && !sniffer->IsTempted();
            }
            bool CanStillUse(EntityLevel&, LivingEntity&, int64_t) override { return true; }
            void Start(EntityLevel&, LivingEntity& body, int64_t) override {
                if (auto* sniffer = dynamic_cast<Sniffer*>(&body)) {
                    sniffer->TransitionTo(Sniffer::State::Scenting);
                }
            }
            void Stop(EntityLevel&, LivingEntity& body, int64_t) override {
                if (auto* sniffer = dynamic_cast<Sniffer*>(&body)) {
                    sniffer->TransitionTo(Sniffer::State::Idling);
                }
            }
        };

        // ── Sniffing (MC SnifferAi.Sniffing) ───────────────────────────────
        // The nose-down search. Only a sniff that runs its FULL 40–80 ticks
        // picks a dig target — an interrupted one finds nothing.
        class Sniffing : public Behavior {
        public:
            Sniffing(int min, int max)
                : Behavior({ { MemoryModule::WalkTarget, MemoryStatus::ValueAbsent },
                             { MemoryModule::SnifferSniffingTarget, MemoryStatus::ValueAbsent },
                             { MemoryModule::SniffCooldown, MemoryStatus::ValueAbsent } },
                           min, max) {}
            const char* DebugString() const override { return "SnifferSniffing"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                auto* sniffer = dynamic_cast<Sniffer*>(&body);
                return sniffer && !sniffer->IsBaby() && sniffer->CanSniff();
            }
            bool CanStillUse(EntityLevel&, LivingEntity& body, int64_t) override {
                auto* sniffer = dynamic_cast<Sniffer*>(&body);
                return sniffer && sniffer->CanSniff();
            }
            void Start(EntityLevel&, LivingEntity& body, int64_t) override {
                if (auto* sniffer = dynamic_cast<Sniffer*>(&body)) {
                    sniffer->TransitionTo(Sniffer::State::Sniffing);
                }
            }
            void Stop(EntityLevel&, LivingEntity& body, int64_t timestamp) override {
                auto* sniffer = dynamic_cast<Sniffer*>(&body);
                if (!sniffer) return;
                const bool finished = TimedOut(timestamp);
                sniffer->TransitionTo(Sniffer::State::Idling);
                if (!finished) return;
                if (const std::optional<glm::ivec3> position =
                        sniffer->CalculateDigPosition()) {
                    if (Brain* brain = sniffer->GetBrain()) {
                        brain->SetMemory(MemoryModule::SnifferSniffingTarget, *position);
                        brain->SetMemory(MemoryModule::WalkTarget,
                                         WalkTarget(*position, 1.25f, 0));
                    }
                }
            }
        };

        // ── Searching (MC SnifferAi.Searching) ─────────────────────────────
        // The walk to the dig spot, nose still down. Lives in its own SNIFF
        // activity because it must survive the WALK_TARGET that IDLE's gate
        // behaviours would trample.
        class Searching : public Behavior {
        public:
            Searching()
                : Behavior({ { MemoryModule::WalkTarget, MemoryStatus::ValuePresent },
                             { MemoryModule::IsPanicking, MemoryStatus::ValueAbsent },
                             { MemoryModule::SnifferSniffingTarget, MemoryStatus::ValuePresent } },
                           600) {}
            const char* DebugString() const override { return "SnifferSearching"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                auto* sniffer = dynamic_cast<Sniffer*>(&body);
                return sniffer && sniffer->CanSniff();
            }

            bool CanStillUse(EntityLevel&, LivingEntity& body, int64_t) override {
                auto* sniffer = dynamic_cast<Sniffer*>(&body);
                if (!sniffer) return false;
                if (!sniffer->CanSniff()) {
                    sniffer->TransitionTo(Sniffer::State::Idling);
                    return false;
                }
                // Still valid only while the walk target IS the sniffing
                // target — anything that redirected the walk ends the search.
                const Brain* brain = sniffer->GetBrain();
                if (!brain) return false;
                const WalkTarget* walk = brain->GetWalkTarget(MemoryModule::WalkTarget);
                const std::optional<glm::ivec3> target =
                    brain->GetBlockPos(MemoryModule::SnifferSniffingTarget);
                return walk && target
                    && walk->target.CurrentBlockPosition() == *target;
            }

            void Start(EntityLevel&, LivingEntity& body, int64_t) override {
                if (auto* sniffer = dynamic_cast<Sniffer*>(&body)) {
                    sniffer->TransitionTo(Sniffer::State::Searching);
                }
            }

            void Stop(EntityLevel&, LivingEntity& body, int64_t) override {
                auto* sniffer = dynamic_cast<Sniffer*>(&body);
                Brain* brain = body.GetBrain();
                if (!sniffer || !brain) return;
                if (sniffer->CanDig() && sniffer->CanSniff()) {
                    brain->SetMemory(MemoryModule::SnifferDigging, true);
                }
                brain->EraseMemory(MemoryModule::WalkTarget);
                brain->EraseMemory(MemoryModule::SnifferSniffingTarget);
            }
        };

        // ── Digging (MC SnifferAi.Digging) ─────────────────────────────────
        class Digging : public Behavior {
        public:
            Digging(int min, int max)
                : Behavior({ { MemoryModule::IsPanicking, MemoryStatus::ValueAbsent },
                             { MemoryModule::WalkTarget, MemoryStatus::ValueAbsent },
                             { MemoryModule::SnifferDigging, MemoryStatus::ValuePresent },
                             { MemoryModule::SniffCooldown, MemoryStatus::ValueAbsent } },
                           min, max) {}
            const char* DebugString() const override { return "SnifferDigging"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                auto* sniffer = dynamic_cast<Sniffer*>(&body);
                return sniffer && sniffer->CanSniff();
            }
            bool CanStillUse(EntityLevel&, LivingEntity& body, int64_t) override {
                auto* sniffer = dynamic_cast<Sniffer*>(&body);
                const Brain* brain = body.GetBrain();
                return sniffer && brain
                    && brain->HasMemoryValue(MemoryModule::SnifferDigging)
                    && sniffer->CanDig() && !sniffer->IsInLove();
            }
            void Start(EntityLevel&, LivingEntity& body, int64_t) override {
                if (auto* sniffer = dynamic_cast<Sniffer*>(&body)) {
                    sniffer->TransitionTo(Sniffer::State::Digging);
                }
            }
            void Stop(EntityLevel&, LivingEntity& body, int64_t timestamp) override {
                auto* sniffer = dynamic_cast<Sniffer*>(&body);
                Brain* brain = body.GetBrain();
                if (!sniffer || !brain) return;
                if (TimedOut(timestamp)) {
                    // A finished dig pays the 8-minute sniff cooldown; the
                    // FinishedDigging behaviour takes over from here.
                    brain->SetMemoryWithExpiry(MemoryModule::SniffCooldown,
                                               std::monostate{}, 9600);
                } else {
                    ResetSniffing(*sniffer);
                }
            }
        };

        // ── FinishedDigging (MC SnifferAi.FinishedDigging) ─────────────────
        class FinishedDigging : public Behavior {
        public:
            explicit FinishedDigging(int duration)
                : Behavior({ { MemoryModule::IsPanicking, MemoryStatus::ValueAbsent },
                             { MemoryModule::WalkTarget, MemoryStatus::ValueAbsent },
                             { MemoryModule::SnifferDigging, MemoryStatus::ValuePresent },
                             { MemoryModule::SniffCooldown, MemoryStatus::ValuePresent } },
                           duration, duration) {}
            const char* DebugString() const override { return "SnifferFinishedDigging"; }

        protected:
            bool CanStillUse(EntityLevel&, LivingEntity& body, int64_t) override {
                const Brain* brain = body.GetBrain();
                return brain && brain->HasMemoryValue(MemoryModule::SnifferDigging);
            }
            void Start(EntityLevel&, LivingEntity& body, int64_t) override {
                if (auto* sniffer = dynamic_cast<Sniffer*>(&body)) {
                    sniffer->TransitionTo(Sniffer::State::Rising);
                }
            }
            void Stop(EntityLevel&, LivingEntity& body, int64_t timestamp) override {
                auto* sniffer = dynamic_cast<Sniffer*>(&body);
                Brain* brain = body.GetBrain();
                if (!sniffer || !brain) return;
                const bool finished = TimedOut(timestamp);
                sniffer->TransitionTo(Sniffer::State::Idling);
                sniffer->OnDiggingComplete(finished);
                brain->EraseMemory(MemoryModule::SnifferDigging);
                brain->SetMemory(MemoryModule::SnifferHappy, true);
            }
        };

        // ── FeelingHappy (MC SnifferAi.FeelingHappy) ───────────────────────
        class FeelingHappy : public Behavior {
        public:
            FeelingHappy(int min, int max)
                : Behavior({ { MemoryModule::SnifferHappy, MemoryStatus::ValuePresent } },
                           min, max) {}
            const char* DebugString() const override { return "SnifferFeelingHappy"; }

        protected:
            bool CanStillUse(EntityLevel&, LivingEntity&, int64_t) override { return true; }
            void Start(EntityLevel&, LivingEntity& body, int64_t) override {
                if (auto* sniffer = dynamic_cast<Sniffer*>(&body)) {
                    sniffer->TransitionTo(Sniffer::State::FeelingHappy);
                }
            }
            void Stop(EntityLevel&, LivingEntity& body, int64_t) override {
                auto* sniffer = dynamic_cast<Sniffer*>(&body);
                if (!sniffer) return;
                sniffer->TransitionTo(Sniffer::State::Idling);
                if (Brain* brain = sniffer->GetBrain()) {
                    brain->EraseMemory(MemoryModule::SnifferHappy);
                }
            }
        };

    } // namespace

    void SnifferAi::InitBrain(Sniffer& sniffer, Brain& brain) {
        (void)sniffer;

        // MC SnifferAi.MEMORY_TYPES. SNIFFER_EXPLORED_POSITIONS lives on the
        // Sniffer class — see the note there.
        for (MemoryModule m : { MemoryModule::LookTarget,
                                MemoryModule::WalkTarget,
                                MemoryModule::CantReachWalkTargetSince,
                                MemoryModule::Path,
                                MemoryModule::IsPanicking,
                                MemoryModule::SnifferSniffingTarget,
                                MemoryModule::SnifferDigging,
                                MemoryModule::SnifferHappy,
                                MemoryModule::SniffCooldown,
                                MemoryModule::NearestVisibleLivingEntities,
                                MemoryModule::BreedTarget,
                                MemoryModule::TemptingPlayer,
                                MemoryModule::TemptationCooldownTicks,
                                MemoryModule::IsTempted }) {
            brain.RegisterMemory(m);
        }

        brain.AddSensor(std::make_unique<NearestLivingEntitySensor>());
        brain.AddSensor(std::make_unique<HurtBySensor>());
        brain.AddSensor(std::make_unique<TemptingSensor>(&Sniffer::IsSnifferFood));

        // ── CORE ───────────────────────────────────────────────────────────
        std::vector<BehaviorPtr> core;
        core.push_back(std::make_unique<Swim>(0.8f));
        core.push_back(std::make_unique<SnifferPanic>(2.0f));
        core.push_back(std::make_unique<MoveToTargetSink>(500, 700));
        core.push_back(std::make_unique<CountDownCooldownTicks>(
            MemoryModule::TemptationCooldownTicks));
        brain.AddActivity(Activity::Core, 0, std::move(core));

        // ── SNIFF (the walk to the dig spot) ───────────────────────────────
        std::vector<BehaviorPtr> sniff;
        sniff.push_back(std::make_unique<Searching>());
        brain.AddActivityWithConditions(
            Activity::Sniff, 0, std::move(sniff),
            { MemoryCondition{ MemoryModule::IsPanicking, MemoryStatus::ValueAbsent },
              MemoryCondition{ MemoryModule::SnifferSniffingTarget,
                               MemoryStatus::ValuePresent },
              MemoryCondition{ MemoryModule::WalkTarget, MemoryStatus::ValuePresent } });

        // ── DIG ────────────────────────────────────────────────────────────
        // Digging then FinishedDigging — MC gives both priority 0; their
        // SNIFF_COOLDOWN conditions (absent vs present) are what sequence
        // them, since a finished dig sets the cooldown as it stops.
        std::vector<BehaviorPtr> dig;
        dig.push_back(std::make_unique<Digging>(160, 180));
        dig.push_back(std::make_unique<FinishedDigging>(40));
        brain.AddActivityWithConditions(
            Activity::Dig, 0, std::move(dig),
            { MemoryCondition{ MemoryModule::IsPanicking, MemoryStatus::ValueAbsent },
              MemoryCondition{ MemoryModule::WalkTarget, MemoryStatus::ValueAbsent },
              MemoryCondition{ MemoryModule::SnifferDigging,
                               MemoryStatus::ValuePresent } });

        // ── IDLE ───────────────────────────────────────────────────────────
        // Note LookAtTargetSink lives HERE, not in core — a digging sniffer
        // must not track passers-by with its buried head.
        std::vector<BehaviorPtr> idle;
        idle.push_back(std::make_unique<SnifferMakeLove>());
        idle.push_back(std::make_unique<SnifferFollowTemptation>());
        idle.push_back(std::make_unique<LookAtTargetSink>(45, 90));
        idle.push_back(std::make_unique<FeelingHappy>(40, 100));
        std::vector<GateBehavior::Entry> idleGate;
        idleGate.push_back({ std::make_unique<SetWalkTargetFromLookTarget>(1.0f, 3), 2 });
        idleGate.push_back({ std::make_unique<Scenting>(40, 80), 1 });
        idleGate.push_back({ std::make_unique<Sniffing>(40, 80), 1 });
        idleGate.push_back({ std::make_unique<SetEntityLookTarget>(
                                 [](LivingEntity& e) { return e.IsPlayer(); }, 6.0f), 1 });
        idleGate.push_back({ RandomStroll::Stroll(1.0f), 1 });
        idleGate.push_back({ std::make_unique<DoNothing>(5, 20), 2 });
        idle.push_back(MakeRunOne(std::move(idleGate)));
        brain.AddActivityWithConditions(
            Activity::Idle, 0, std::move(idle),
            { MemoryCondition{ MemoryModule::SnifferDigging, MemoryStatus::ValueAbsent } });

        brain.SetCoreActivities({ Activity::Core });
        brain.SetDefaultActivity(Activity::Idle);
        brain.UseDefaultActivity();
    }

    void SnifferAi::UpdateActivity(Sniffer& sniffer) {
        if (Brain* brain = sniffer.GetBrain()) {
            brain->SetActiveActivityToFirstValid(
                { Activity::Dig, Activity::Sniff, Activity::Idle });
        }
    }

} // namespace Game

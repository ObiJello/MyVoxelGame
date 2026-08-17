// File: src/common/entity/ai/brain/FrogAi.cpp
#include "common/entity/ai/brain/FrogAi.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/brain/CommonBehaviors.hpp"
#include "common/entity/ai/brain/CoreBehaviors.hpp"
#include "common/entity/ai/goals/LongJumpGoal.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/world/crafting/RecipeManager.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

#include <cmath>

namespace Game {

    namespace {

        // MC BlockTags.FROG_PREFER_JUMP_TO.
        constexpr BlockID kFrogPreferJumpTo[] = { BlockID::LilyPad, BlockID::BigDripleaf };

        // MC EntityTypeTags.FROG_FOOD —
        // data/minecraft/tags/entity_type/frog_food.json.
        bool FrogCanEat(LivingEntity& e) {
            // MC additionally requires a slime to be size 1; slime sizes are not
            // modelled, so every slime qualifies. Documented rather than silent.
            return e.GetType() == EntityTypeId::Slime
                || e.GetType() == EntityTypeId::MagmaCube;
        }

        // ── Croak ──────────────────────────────────────────────────────────

        class Croak : public Behavior {
        public:
            static constexpr int kCroakTicks = 60;        // MC CROAK_TICKS
            static constexpr int kTimeOutDuration = 100;  // MC TIME_OUT_DURATION

            Croak()
                : Behavior({ MemoryCondition{ MemoryModule::WalkTarget,
                                              MemoryStatus::ValueAbsent } },
                           kTimeOutDuration) {}

            const char* DebugString() const override { return "Croak"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                return body.GetPose() == Pose::Standing;
            }
            bool CanStillUse(EntityLevel&, LivingEntity&, int64_t) override {
                return m_croakCounter < kCroakTicks;
            }
            void Start(EntityLevel&, LivingEntity& body, int64_t) override {
                if (!body.IsInLiquid()) {
                    body.SetPose(Pose::Croaking);
                    m_croakCounter = 0;
                }
            }
            void Tick(EntityLevel&, LivingEntity&, int64_t) override { ++m_croakCounter; }
            void Stop(EntityLevel&, LivingEntity& body, int64_t) override {
                body.SetPose(Pose::Standing);
            }

        private:
            int m_croakCounter = 0;
        };

        // TriggerIf now lives in CommonBehaviors.

        // ── ShootTongue ────────────────────────────────────────────────────

        // MC net.minecraft.world.entity.animal.frog.ShootTongue. A four-state
        // machine, and one of the two behaviours in MC that legitimately drives
        // the mob directly rather than through a memory.
        class ShootTongue : public Behavior {
        public:
            static constexpr int kTimeOutDuration = 100;
            static constexpr int kCatchAnimationDuration = 6;
            static constexpr int kTongueAnimationDuration = 10;
            static constexpr float kEatingDistance = 1.75f;
            static constexpr float kEatingMovementFactor = 0.75f;

            ShootTongue()
                : Behavior({ MemoryCondition{ MemoryModule::WalkTarget,
                                              MemoryStatus::ValueAbsent },
                             MemoryCondition{ MemoryModule::LookTarget,
                                              MemoryStatus::Registered },
                             MemoryCondition{ MemoryModule::AttackTarget,
                                              MemoryStatus::ValuePresent },
                             MemoryCondition{ MemoryModule::IsPanicking,
                                              MemoryStatus::ValueAbsent } },
                           kTimeOutDuration) {}

            const char* DebugString() const override { return "ShootTongue"; }

        protected:
            enum class State : uint8_t { MoveToTarget, CatchAnimation, EatAnimation, Done };

            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                Brain* brain = body.GetBrain();
                auto* mob = dynamic_cast<Mob*>(&body);
                if (!brain || !mob) return false;
                auto* target = dynamic_cast<LivingEntity*>(
                    brain->GetEntity(MemoryModule::AttackTarget));
                if (!target) return false;

                // MC drops a target it cannot path within tongue reach of, and
                // remembers it so it does not immediately re-acquire it.
                const bool reachable = CanPathfindToTarget(*mob, *target);
                if (!reachable) {
                    brain->EraseMemory(MemoryModule::AttackTarget);
                    return false;
                }
                return body.GetPose() != Pose::Croaking && FrogCanEat(*target);
            }

            bool CanStillUse(EntityLevel&, LivingEntity& body, int64_t) override {
                const Brain* brain = body.GetBrain();
                if (!brain) return false;
                return brain->HasMemoryValue(MemoryModule::AttackTarget)
                    && m_state != State::Done
                    && !brain->HasMemoryValue(MemoryModule::IsPanicking);
            }

            void Start(EntityLevel&, LivingEntity& body, int64_t) override {
                Brain* brain = body.GetBrain();
                if (!brain) return;
                Entity* target = brain->GetEntity(MemoryModule::AttackTarget);
                if (!target) return;
                brain->SetMemory(MemoryModule::LookTarget,
                                 PositionTracker::OfEntity(target, true));
                brain->SetMemory(MemoryModule::WalkTarget,
                                 WalkTarget(PositionTracker::OfEntity(target, false),
                                            2.0f, 0));
                m_calculatePathCounter = 10;
                m_state = State::MoveToTarget;
            }

            void Tick(EntityLevel&, LivingEntity& body, int64_t) override {
                Brain* brain = body.GetBrain();
                auto* mob = dynamic_cast<Mob*>(&body);
                if (!brain || !mob) return;
                auto* target = dynamic_cast<LivingEntity*>(
                    brain->GetEntity(MemoryModule::AttackTarget));
                if (!target) { m_state = State::Done; return; }

                switch (m_state) {
                    case State::MoveToTarget: {
                        const double d = std::sqrt(body.DistanceToSqr(*target));
                        if (d < kEatingDistance) {
                            body.SetPose(Pose::UsingTongue);
                            // MC yanks the prey toward the frog along the tongue.
                            const glm::dvec3 toFrog = body.position - target->position;
                            const double len = glm::length(toFrog);
                            if (len > 1.0e-6) {
                                target->velocity = toFrog / len
                                                 * static_cast<double>(kEatingMovementFactor);
                            }
                            m_eatAnimationTimer = 0;
                            m_state = State::CatchAnimation;
                        } else if (m_calculatePathCounter <= 0) {
                            brain->SetMemory(
                                MemoryModule::WalkTarget,
                                WalkTarget(PositionTracker::OfEntity(target, false),
                                           2.0f, 0));
                            m_calculatePathCounter = 10;
                        } else {
                            --m_calculatePathCounter;
                        }
                        break;
                    }
                    case State::CatchAnimation:
                        if (m_eatAnimationTimer++ >= kCatchAnimationDuration) {
                            m_state = State::EatAnimation;
                            if (target->IsAlive()) {
                                mob->DoHurtTarget(*target);
                                if (!target->IsAlive()) target->Remove(RemovalReason::Killed);
                            }
                        }
                        break;
                    case State::EatAnimation:
                        if (m_eatAnimationTimer >= kTongueAnimationDuration) m_state = State::Done;
                        else ++m_eatAnimationTimer;
                        break;
                    case State::Done:
                        break;
                }
            }

            void Stop(EntityLevel&, LivingEntity& body, int64_t) override {
                if (Brain* brain = body.GetBrain()) {
                    brain->EraseMemory(MemoryModule::AttackTarget);
                }
                body.SetPose(Pose::Standing);
                m_state = State::Done;
            }

        private:
            static bool CanPathfindToTarget(Mob& body, LivingEntity& target) {
                auto path = body.GetNavigation().CreatePath(target.BlockPosition(), 0);
                return path.has_value() && path->CanReach();
            }

            int   m_eatAnimationTimer = 0;
            int   m_calculatePathCounter = 0;
            State m_state = State::Done;
        };

        // ── Long jump ──────────────────────────────────────────────────────

        // LongJumpMidJump now lives in CommonBehaviors — the goat uses the
        // same one, with its own cooldown range.

        // MC LongJumpToPreferredBlock. The search, the ballistics and the
        // preferred-block bias already exist as LongJumpGoal — this is the same
        // machine driven from the brain instead of the goal selector, so the
        // arcs are literally the same code.
        class LongJumpToPreferredBlock : public Behavior {
        public:
            LongJumpToPreferredBlock()
                : Behavior({ MemoryCondition{ MemoryModule::LookTarget,
                                              MemoryStatus::Registered },
                             MemoryCondition{ MemoryModule::LongJumpCooldownTicks,
                                              MemoryStatus::ValueAbsent },
                             MemoryCondition{ MemoryModule::LongJumpMidJump,
                                              MemoryStatus::ValueAbsent } },
                           200) {}

            const char* DebugString() const override { return "LongJumpToPreferredBlock"; }

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
                // The moment the goal has launched, hand off to LongJumpMidJump
                // exactly as MC does through the LONG_JUMP_MID_JUMP memory.
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
            void EnsureGoal(LivingEntity& body);
            std::unique_ptr<LongJumpGoal> m_goal;
        };

        bool FrogAcceptableLandingSpot(Mob& mob, const glm::ivec3& target);

        void LongJumpToPreferredBlock::EnsureGoal(LivingEntity& body) {
            if (m_goal) return;
            auto* mob = dynamic_cast<Mob*>(&body);
            if (!mob) return;
            LongJumpGoal::Config cfg;
            cfg.cooldownMin = FrogAi::kTimeBetweenLongJumpsMin;
            cfg.cooldownMax = FrogAi::kTimeBetweenLongJumpsMax;
            cfg.maxHeight = 2;                        // MAX_LONG_JUMP_HEIGHT
            cfg.maxWidth = 4;                         // MAX_LONG_JUMP_WIDTH
            cfg.maxJumpVelocityMultiplier = 3.5714288f;
            cfg.preferredBlocks = kFrogPreferJumpTo;
            cfg.preferredBlockCount =
                static_cast<int>(sizeof(kFrogPreferJumpTo) / sizeof(kFrogPreferJumpTo[0]));
            cfg.preferredBlocksChance = 0.5f;
            cfg.acceptableLandingSpot = &FrogAcceptableLandingSpot;
            // The brain owns the cooldown: LONG_JUMP is gated on
            // LONG_JUMP_COOLDOWN_TICKS being absent, and LongJumpMidJump
            // re-arms it on landing. That IS MC's arrangement.
            cfg.ownsCooldown = false;
            m_goal = std::make_unique<LongJumpGoal>(mob, cfg);
        }

        // MC FrogAi.isAcceptableLandingSpot.
        bool FrogAcceptableLandingSpot(Mob& mob, const glm::ivec3& target) {
            EntityLevel* level = mob.Level();
            const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
            if (!blocks) return false;

            if (blocks->IsBlockFluid(target.x, target.y, target.z)) return false;
            if (blocks->IsBlockFluid(target.x, target.y - 1, target.z)) return false;
            if (blocks->IsBlockFluid(target.x, target.y + 1, target.z)) return false;

            const BlockID at    = blocks->GetBlock(target.x, target.y, target.z);
            const BlockID below = blocks->GetBlock(target.x, target.y - 1, target.z);
            for (const BlockID preferred : kFrogPreferJumpTo) {
                if (at == preferred || below == preferred) return true;
            }
            return LongJumpGoal::DefaultAcceptableLandingSpot(mob, target);
        }

        bool IsOnGround(LivingEntity& e) { return e.onGround; }
        bool IsInWaterPred(LivingEntity& e) { return e.IsInWater(); }

    } // namespace

    // ── FrogAi ─────────────────────────────────────────────────────────────

    void FrogAi::InitMemories(Frog& frog) {
        EntityLevel* level = frog.Level();
        if (Brain* brain = frog.GetBrain()) {
            brain->SetMemory(MemoryModule::LongJumpCooldownTicks,
                             level ? level->Random().NextInt(kTimeBetweenLongJumpsMin,
                                                             kTimeBetweenLongJumpsMax)
                                   : kTimeBetweenLongJumpsMin);
        }
    }

    void FrogAi::InitBrain(Frog& frog, Brain& brain) {
        (void)frog;

        // MC Frog.MEMORY_TYPES. A memory that is not registered fails every
        // CheckMemory INCLUDING VALUE_ABSENT, so an unregistered one silently
        // disables every behaviour that waits on it being empty.
        for (MemoryModule m : { MemoryModule::LookTarget,
                                MemoryModule::WalkTarget,
                                MemoryModule::CantReachWalkTargetSince,
                                MemoryModule::Path,
                                MemoryModule::BreedTarget,
                                MemoryModule::LongJumpCooldownTicks,
                                MemoryModule::LongJumpMidJump,
                                MemoryModule::AttackTarget,
                                MemoryModule::TemptingPlayer,
                                MemoryModule::TemptationCooldownTicks,
                                MemoryModule::IsTempted,
                                MemoryModule::HurtBy,
                                MemoryModule::HurtByEntity,
                                MemoryModule::NearestAttackable,
                                MemoryModule::IsInWater,
                                MemoryModule::IsPregnant,
                                MemoryModule::IsPanicking }) {
            brain.RegisterMemory(m);
        }

        // MC Frog.SENSOR_TYPES.
        brain.AddSensor(std::make_unique<NearestLivingEntitySensor>());
        brain.AddSensor(std::make_unique<HurtBySensor>());
        brain.AddSensor(std::make_unique<IsInWaterSensor>());
        brain.AddSensor(std::make_unique<NearestAttackableSensor>(&FrogCanEat));
        brain.AddSensor(std::make_unique<TemptingSensor>([](uint32_t item) {
            // MC ItemTags.FROG_FOOD — a single item, resolved by slug because a
            // slime ball's id is not a compile-time constant here.
            static const ItemID slimeBall = RecipeManager::ItemFromSlug("slime_ball");
            return slimeBall != Items::Air && item == static_cast<uint32_t>(slimeBall);
        }));

        // ── CORE ───────────────────────────────────────────────────────────
        std::vector<BehaviorPtr> core;
        core.push_back(std::make_unique<AnimalPanic>(2.0f));
        core.push_back(std::make_unique<LookAtTargetSink>(45, 90));
        core.push_back(std::make_unique<MoveToTargetSink>());
        core.push_back(std::make_unique<CountDownCooldownTicks>(
            MemoryModule::TemptationCooldownTicks));
        core.push_back(std::make_unique<CountDownCooldownTicks>(
            MemoryModule::LongJumpCooldownTicks));
        brain.AddActivity(Activity::Core, 0, std::move(core));

        // ── IDLE ───────────────────────────────────────────────────────────
        std::vector<GateBehavior::Entry> idleGate;
        idleGate.push_back({ RandomStroll::Stroll(1.0f), 1 });
        idleGate.push_back({ std::make_unique<SetWalkTargetFromLookTarget>(1.0f, 3), 1 });
        idleGate.push_back({ std::make_unique<Croak>(), 3 });
        idleGate.push_back({ std::make_unique<TriggerIf>(&IsOnGround), 2 });

        std::vector<BehaviorPtr> idle;
        idle.push_back(std::make_unique<SetEntityLookTargetSometimes>(6.0f, 30, 60));
        idle.push_back(std::make_unique<AnimalMakeLove>(EntityTypeId::Frog));
        idle.push_back(std::make_unique<FollowTemptation>(1.25f));
        idle.push_back(std::make_unique<StartAttacking>(
            [](Mob& m) {
                // MC canAttack: not while breeding.
                const Brain* b = m.GetBrain();
                return b && !b->HasMemoryValue(MemoryModule::BreedTarget);
            },
            [](Mob& m) -> LivingEntity* {
                const Brain* b = m.GetBrain();
                return b ? dynamic_cast<LivingEntity*>(
                               b->GetEntity(MemoryModule::NearestAttackable))
                         : nullptr;
            }));
        idle.push_back(std::make_unique<TryFindLand>(6, 1.0f));
        idle.push_back(MakeRunOne(
            { MemoryCondition{ MemoryModule::WalkTarget, MemoryStatus::ValueAbsent } },
            std::move(idleGate)));
        brain.AddActivityWithConditions(
            Activity::Idle, 0, std::move(idle),
            { MemoryCondition{ MemoryModule::LongJumpMidJump, MemoryStatus::ValueAbsent },
              MemoryCondition{ MemoryModule::IsInWater, MemoryStatus::ValueAbsent } });

        // ── SWIM ───────────────────────────────────────────────────────────
        std::vector<GateBehavior::Entry> swimGate;
        swimGate.push_back({ RandomStroll::Swim(0.75f), 1 });
        swimGate.push_back({ RandomStroll::Stroll(1.0f, true), 1 });
        swimGate.push_back({ std::make_unique<SetWalkTargetFromLookTarget>(1.0f, 3), 1 });
        swimGate.push_back({ std::make_unique<TriggerIf>(&IsInWaterPred), 5 });

        std::vector<BehaviorPtr> swim;
        swim.push_back(std::make_unique<SetEntityLookTargetSometimes>(6.0f, 30, 60));
        swim.push_back(std::make_unique<FollowTemptation>(1.25f));
        swim.push_back(std::make_unique<StartAttacking>(
            [](Mob& m) {
                const Brain* b = m.GetBrain();
                return b && !b->HasMemoryValue(MemoryModule::BreedTarget);
            },
            [](Mob& m) -> LivingEntity* {
                const Brain* b = m.GetBrain();
                return b ? dynamic_cast<LivingEntity*>(
                               b->GetEntity(MemoryModule::NearestAttackable))
                         : nullptr;
            }));
        swim.push_back(std::make_unique<TryFindLand>(8, 1.5f));
        // MC's SWIM gate is ORDERED / TRY_ALL, not a RunOne — every option that
        // can start does, which is what keeps a swimming frog moving.
        swim.push_back(std::make_unique<GateBehavior>(
            std::vector<MemoryCondition>{
                MemoryCondition{ MemoryModule::WalkTarget, MemoryStatus::ValueAbsent } },
            std::vector<MemoryModule>{},
            GateBehavior::OrderPolicy::Ordered, GateBehavior::RunningPolicy::TryAll,
            std::move(swimGate)));
        brain.AddActivityWithConditions(
            Activity::Swim, 0, std::move(swim),
            { MemoryCondition{ MemoryModule::LongJumpMidJump, MemoryStatus::ValueAbsent },
              MemoryCondition{ MemoryModule::IsInWater, MemoryStatus::ValuePresent } });

        // ── LONG_JUMP ──────────────────────────────────────────────────────
        std::vector<BehaviorPtr> jump;
        jump.push_back(std::make_unique<LongJumpMidJump>(
            kTimeBetweenLongJumpsMin, kTimeBetweenLongJumpsMax));
        jump.push_back(std::make_unique<LongJumpToPreferredBlock>());
        brain.AddActivityWithConditions(
            Activity::LongJump, 0, std::move(jump),
            { MemoryCondition{ MemoryModule::TemptingPlayer, MemoryStatus::ValueAbsent },
              MemoryCondition{ MemoryModule::BreedTarget, MemoryStatus::ValueAbsent },
              MemoryCondition{ MemoryModule::LongJumpCooldownTicks,
                               MemoryStatus::ValueAbsent },
              MemoryCondition{ MemoryModule::IsInWater, MemoryStatus::ValueAbsent } });

        // ── TONGUE ─────────────────────────────────────────────────────────
        std::vector<BehaviorPtr> tongue;
        tongue.push_back(std::make_unique<StopAttackingIfTargetInvalid>());
        tongue.push_back(std::make_unique<ShootTongue>());
        brain.AddActivityAndRemoveMemoryWhenStopped(Activity::Tongue, 0, std::move(tongue),
                                                    MemoryModule::AttackTarget);

        brain.SetCoreActivities({ Activity::Core });
        brain.SetDefaultActivity(Activity::Idle);
        brain.UseDefaultActivity();
    }

    void FrogAi::UpdateActivity(Frog& frog) {
        Brain* brain = frog.GetBrain();
        if (!brain) return;
        // MC's order. LAY_SPAWN is omitted: it needs the frogspawn block and
        // TryLaySpawnOnWaterNearLand, neither of which exists — naming it here
        // would be a lie, since its requirements could never be met anyway.
        brain->SetActiveActivityToFirstValid(
            { Activity::Tongue, Activity::LongJump, Activity::Swim, Activity::Idle });
    }

} // namespace Game

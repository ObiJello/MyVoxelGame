// File: src/common/entity/ai/brain/AxolotlAi.cpp
#include "common/entity/ai/brain/AxolotlAi.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/brain/CommonBehaviors.hpp"
#include "common/entity/ai/brain/CoreBehaviors.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/world/crafting/RecipeManager.hpp"

namespace Game {

    namespace {

        // MC AxolotlAi's speed table: 0.15 crawling on land, 0.5 idling in
        // water, 0.6 chasing or following an adult in water.
        float SpeedIdle(LivingEntity& e)  { return e.IsInWater() ? 0.5f : 0.15f; }
        float SpeedChase(LivingEntity& e) { return e.IsInWater() ? 0.6f : 0.15f; }

        bool IsBreeding(LivingEntity& body) {
            const Brain* brain = body.GetBrain();
            return brain && brain->HasMemoryValue(MemoryModule::BreedTarget);
        }

        // MC AxolotlAi.findNearestValidAttackTarget — NEAREST_ATTACKABLE, but
        // never while breeding.
        LivingEntity* FindTarget(Mob& mob) {
            Brain* brain = mob.GetBrain();
            if (!brain) return nullptr;
            if (brain->HasMemoryValue(MemoryModule::BreedTarget)) return nullptr;
            return dynamic_cast<LivingEntity*>(
                brain->GetEntity(MemoryModule::NearestAttackable));
        }

        // MC AxolotlAi.canSetWalkTargetFromLookTarget — only chase a look
        // target on the axolotl's own side of the water line.
        bool CanSetWalkTargetFromLookTarget(LivingEntity& body) {
            EntityLevel* level = body.Level();
            const Brain* brain = body.GetBrain();
            if (!level || !brain || !level->Blocks()) return false;
            const PositionTracker* look = brain->GetPositionTracker(MemoryModule::LookTarget);
            if (!look) return false;
            const glm::ivec3 pos = look->CurrentBlockPosition();
            return level->Blocks()->ContainsWater(pos.x, pos.y, pos.z) == body.IsInWater();
        }

        // MC animal/axolotl/PlayDead — go limp in the water for as long as
        // PLAY_DEAD_TICKS holds, with a self-regen on entry. The 200-tick
        // duration is the behaviour's own cap; ValidatePlayDead (core) is
        // what actually counts the memory down.
        class PlayDead : public Behavior {
        public:
            PlayDead()
                : Behavior({ MemoryCondition{ MemoryModule::PlayDeadTicks,
                                              MemoryStatus::ValuePresent },
                             MemoryCondition{ MemoryModule::HurtByEntity,
                                              MemoryStatus::ValuePresent } },
                           200) {}
            const char* DebugString() const override { return "PlayDead"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                return body.IsInWater();
            }
            bool CanStillUse(EntityLevel&, LivingEntity& body, int64_t) override {
                const Brain* brain = body.GetBrain();
                return body.IsInWater() && brain
                    && brain->HasMemoryValue(MemoryModule::PlayDeadTicks);
            }
            void Start(EntityLevel&, LivingEntity& body, int64_t) override {
                if (Brain* brain = body.GetBrain()) {
                    brain->EraseMemory(MemoryModule::WalkTarget);
                    brain->EraseMemory(MemoryModule::LookTarget);
                }
                // MC: REGENERATION 200, amplifier 0, on the axolotl itself.
                body.AddEffect(MobEffectInstance(MobEffectId::Regeneration, 200, 0));
            }
        };

        // MC animal/axolotl/ValidatePlayDead — a CORE behaviour that counts
        // PLAY_DEAD_TICKS down every tick and, at zero, erases it (plus
        // HURT_BY_ENTITY) and drops the brain back to the default activity.
        // Like the port's CountDownCooldownTicks it re-arms across the
        // behaviour-duration boundary rather than overriding timedOut.
        class ValidatePlayDead : public Behavior {
        public:
            ValidatePlayDead()
                : Behavior({ MemoryCondition{ MemoryModule::PlayDeadTicks,
                                              MemoryStatus::ValuePresent },
                             MemoryCondition{ MemoryModule::HurtByEntity,
                                              MemoryStatus::Registered } }) {}
            const char* DebugString() const override { return "ValidatePlayDead"; }

        protected:
            bool CanStillUse(EntityLevel&, LivingEntity& body, int64_t) override {
                const Brain* brain = body.GetBrain();
                return brain && brain->HasMemoryValue(MemoryModule::PlayDeadTicks);
            }
            void Tick(EntityLevel&, LivingEntity& body, int64_t) override {
                Brain* brain = body.GetBrain();
                if (!brain) return;
                const std::optional<int> ticks = brain->GetInt(MemoryModule::PlayDeadTicks);
                if (!ticks) return;
                if (*ticks <= 0) {
                    brain->EraseMemory(MemoryModule::PlayDeadTicks);
                    brain->EraseMemory(MemoryModule::HurtByEntity);
                    brain->UseDefaultActivity();
                } else {
                    brain->SetMemory(MemoryModule::PlayDeadTicks, *ticks - 1);
                }
            }
        };

        // MC's StopAttackingIfTargetInvalid.create(Axolotl::onStopAttacking) —
        // the port's shared class has no stop callback, so this is its body
        // plus the axolotl's kill-reward hook.
        class AxolotlStopAttacking : public Behavior {
        public:
            AxolotlStopAttacking()
                : Behavior({ MemoryCondition{ MemoryModule::AttackTarget,
                                              MemoryStatus::ValuePresent },
                             MemoryCondition{ MemoryModule::CantReachWalkTargetSince,
                                              MemoryStatus::Registered } },
                           1) {}
            const char* DebugString() const override { return "AxolotlStopAttacking"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                auto* axolotl = dynamic_cast<Axolotl*>(&body);
                Brain* brain = body.GetBrain();
                if (!axolotl || !brain) return false;

                Entity* target = brain->GetEntity(MemoryModule::AttackTarget);
                auto* living = dynamic_cast<LivingEntity*>(target);

                bool tired = false;
                if (const std::optional<int64_t> since =
                        brain->GetLong(MemoryModule::CantReachWalkTargetSince)) {
                    tired = (level.GetGameTime() - *since) > 200;
                }

                if (!living || !living->IsAlive()
                    || !axolotl->CanAttack(*living) || tired) {
                    if (living) Axolotl::OnStopAttacking(level, *axolotl, *living);
                    brain->EraseMemory(MemoryModule::AttackTarget);
                }
                return true;
            }
        };

    } // namespace

    void AxolotlAi::InitBrain(Axolotl& axolotl, Brain& brain) {
        (void)axolotl;

        // MC Axolotl.MEMORY_TYPES, in declaration order.
        for (MemoryModule m : { MemoryModule::BreedTarget,
                                MemoryModule::NearestLivingEntities,
                                MemoryModule::NearestVisibleLivingEntities,
                                MemoryModule::NearestVisiblePlayer,
                                MemoryModule::NearestVisibleAttackablePlayer,
                                MemoryModule::LookTarget,
                                MemoryModule::WalkTarget,
                                MemoryModule::CantReachWalkTargetSince,
                                MemoryModule::Path,
                                MemoryModule::AttackTarget,
                                MemoryModule::AttackCoolingDown,
                                MemoryModule::NearestVisibleAdult,
                                MemoryModule::HurtByEntity,
                                MemoryModule::PlayDeadTicks,
                                MemoryModule::NearestAttackable,
                                MemoryModule::TemptingPlayer,
                                MemoryModule::TemptationCooldownTicks,
                                MemoryModule::IsTempted,
                                MemoryModule::HasHuntingCooldown,
                                MemoryModule::IsPanicking }) {
            brain.RegisterMemory(m);
        }

        // MC SENSOR_TYPES: NEAREST_LIVING_ENTITIES, NEAREST_ADULT, HURT_BY,
        // AXOLOTL_ATTACKABLES, FOOD_TEMPTATIONS.
        brain.AddSensor(std::make_unique<NearestLivingEntitySensor>());
        brain.AddSensor(std::make_unique<AdultSensor>());
        brain.AddSensor(std::make_unique<HurtBySensor>());
        // MC AxolotlAttackablesSensor: within 8 blocks, IN WATER, and either
        // an always-hostile (drowned, guardian, elder guardian — the
        // axolotl_always_hostiles tag) or, while not on hunting cooldown, a
        // hunt target (the axolotl_hunt_targets tag).
        brain.AddSensor(std::make_unique<NearestAttackableSensor>(
            [&axolotl](LivingEntity& e) {
                if (axolotl.DistanceToSqr(e) > 64.0) return false;
                if (!e.IsInWater()) return false;
                const EntityTypeId t = e.GetType();
                const bool hostile = t == EntityTypeId::Drowned
                                  || t == EntityTypeId::Guardian
                                  || t == EntityTypeId::ElderGuardian;
                const bool hunt = t == EntityTypeId::TropicalFish
                               || t == EntityTypeId::Pufferfish
                               || t == EntityTypeId::Salmon
                               || t == EntityTypeId::Cod
                               || t == EntityTypeId::Squid
                               || t == EntityTypeId::GlowSquid
                               || t == EntityTypeId::Tadpole;
                const Brain* b = axolotl.GetBrain();
                const bool onCooldown =
                    b && b->HasMemoryValue(MemoryModule::HasHuntingCooldown);
                // Sensor.isEntityAttackable's reachable half.
                if (!axolotl.CanAttack(e)) return false;
                return hostile || (hunt && !onCooldown);
            }));
        // MC FOOD_TEMPTATIONS — ItemTags.AXOLOTL_FOOD is the tropical fish
        // bucket.
        brain.AddSensor(std::make_unique<TemptingSensor>([](uint32_t item) {
            static const ItemID bucket =
                RecipeManager::ItemFromSlug("tropical_fish_bucket");
            return bucket != Items::Air && item == static_cast<uint32_t>(bucket);
        }));

        // ── CORE ───────────────────────────────────────────────────────────
        std::vector<BehaviorPtr> core;
        core.push_back(std::make_unique<LookAtTargetSink>(45, 90));
        core.push_back(std::make_unique<MoveToTargetSink>());
        core.push_back(std::make_unique<ValidatePlayDead>());
        core.push_back(std::make_unique<CountDownCooldownTicks>(
            MemoryModule::TemptationCooldownTicks));
        brain.AddActivity(Activity::Core, 0, std::move(core));

        // ── IDLE (MC initIdleActivity, explicit priority pairs) ────────────
        std::vector<BehaviorPtr> idle;
        // 0: glance at players.
        idle.push_back(std::make_unique<SetEntityLookTargetSometimes>(6.0f, 30, 60));
        // 1: breed.
        idle.push_back(std::make_unique<AnimalMakeLove>(EntityTypeId::Axolotl, 0.2f, 2));
        // 2: RunOne { FollowTemptation, BabyFollowAdult }.
        std::vector<GateBehavior::Entry> follow;
        follow.push_back({ std::make_unique<FollowTemptation>(SpeedFn(&SpeedIdle)), 1 });
        follow.push_back({ std::make_unique<BabyFollowAdult>(5, 16, SpeedFn(&SpeedChase)), 1 });
        idle.push_back(MakeRunOne(std::move(follow)));
        // 3: pick a fight; 4 (MC gives TryFindWater the same 3): find water.
        idle.push_back(std::make_unique<StartAttacking>(
            [](Mob&) { return true; }, &FindTarget));
        idle.push_back(std::make_unique<TryFindWater>(6, 0.15f));
        // 5: the ORDERED / TRY_ALL movement gate.
        std::vector<GateBehavior::Entry> gate;
        gate.push_back({ RandomStroll::Swim(0.5f), 2 });
        gate.push_back({ RandomStroll::Stroll(0.15f, false), 2 });
        gate.push_back({ std::make_unique<SetWalkTargetFromLookTarget>(
                             &CanSetWalkTargetFromLookTarget, SpeedFn(&SpeedIdle), 3), 3 });
        gate.push_back({ std::make_unique<TriggerIf>(
                             [](LivingEntity& e) { return e.IsInWater(); }), 5 });
        gate.push_back({ std::make_unique<TriggerIf>(
                             [](LivingEntity& e) { return e.onGround; }), 5 });
        idle.push_back(std::make_unique<GateBehavior>(
            std::vector<MemoryCondition>{
                MemoryCondition{ MemoryModule::WalkTarget, MemoryStatus::ValueAbsent } },
            std::vector<MemoryModule>{},
            GateBehavior::OrderPolicy::Ordered, GateBehavior::RunningPolicy::TryAll,
            std::move(gate)));
        brain.AddActivity(Activity::Idle, 0, std::move(idle));

        // ── FIGHT (MC initFightActivity) ───────────────────────────────────
        std::vector<BehaviorPtr> fight;
        fight.push_back(std::make_unique<AxolotlStopAttacking>());
        fight.push_back(std::make_unique<SetWalkTargetFromAttackTarget>(
            SpeedFn(&SpeedChase)));
        fight.push_back(std::make_unique<MeleeAttack>(20));
        fight.push_back(std::make_unique<EraseMemoryIf>(&IsBreeding,
                                                        MemoryModule::AttackTarget));
        brain.AddActivityAndRemoveMemoryWhenStopped(Activity::Fight, 0, std::move(fight),
                                                    MemoryModule::AttackTarget);

        // ── PLAY_DEAD (MC initPlayDeadActivity) ────────────────────────────
        std::vector<BehaviorPtr> playDead;
        playDead.push_back(std::make_unique<PlayDead>());
        playDead.push_back(std::make_unique<EraseMemoryIf>(&IsBreeding,
                                                           MemoryModule::PlayDeadTicks));
        brain.AddActivityAndRemoveMemoryWhenStopped(Activity::PlayDead, 0,
                                                    std::move(playDead),
                                                    MemoryModule::PlayDeadTicks);

        brain.SetCoreActivities({ Activity::Core });
        brain.SetDefaultActivity(Activity::Idle);
        brain.UseDefaultActivity();
    }

    void AxolotlAi::UpdateActivity(Axolotl& axolotl) {
        Brain* brain = axolotl.GetBrain();
        if (!brain) return;
        // MC: while PLAY_DEAD is active the switch is frozen — only
        // ValidatePlayDead's useDefaultActivity leaves it.
        const std::optional<Activity> old = brain->GetActiveNonCoreActivity();
        if (old == Activity::PlayDead) return;

        brain->SetActiveActivityToFirstValid(
            { Activity::PlayDead, Activity::Fight, Activity::Idle });
        if (old == Activity::Fight
            && brain->GetActiveNonCoreActivity() != Activity::Fight) {
            // MC: leaving FIGHT arms the 2400-tick hunting cooldown.
            brain->SetMemoryWithExpiry(MemoryModule::HasHuntingCooldown, true, 2400);
        }
    }

} // namespace Game

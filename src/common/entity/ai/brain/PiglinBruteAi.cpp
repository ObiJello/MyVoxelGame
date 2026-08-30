// File: src/common/entity/ai/brain/PiglinBruteAi.cpp
#include "common/entity/ai/brain/PiglinBruteAi.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/RandomPos.hpp"
#include "common/entity/ai/brain/CommonBehaviors.hpp"
#include "common/entity/ai/brain/CoreBehaviors.hpp"
#include "common/entity/ai/brain/PiglinAi.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"

#include <cmath>

namespace Game {

    namespace {

        // MC PiglinBruteAi.findNearestValidAttackTarget — ANGRY_AT, else the
        // nearest visible attackable player, else the wither-class nemesis.
        LivingEntity* FindTarget(Mob& mob) {
            Brain* brain = mob.GetBrain();
            if (!brain) return nullptr;
            if (auto* angry =
                    dynamic_cast<LivingEntity*>(brain->GetEntity(MemoryModule::AngryAt))) {
                if (mob.CanAttack(*angry)) return angry;
            }
            if (auto* player = dynamic_cast<LivingEntity*>(
                    brain->GetEntity(MemoryModule::NearestVisibleAttackablePlayer))) {
                return player;
            }
            return dynamic_cast<LivingEntity*>(
                brain->GetEntity(MemoryModule::NearestVisibleNemesis));
        }

        // MC StopAttackingIfTargetInvalid.create(target != nearest valid),
        // with the brute's own validity rule — same shape as the piglin's.
        class BruteStopAttacking : public Behavior {
        public:
            BruteStopAttacking()
                : Behavior({ MemoryCondition{ MemoryModule::AttackTarget,
                                              MemoryStatus::ValuePresent },
                             MemoryCondition{ MemoryModule::CantReachWalkTargetSince,
                                              MemoryStatus::Registered } },
                           1) {}
            const char* DebugString() const override { return "BruteStopAttacking"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                auto* mob = dynamic_cast<Mob*>(&body);
                Brain* brain = body.GetBrain();
                if (!mob || !brain) return false;

                auto* target = dynamic_cast<LivingEntity*>(
                    brain->GetEntity(MemoryModule::AttackTarget));

                bool tired = false;
                if (const std::optional<int64_t> since =
                        brain->GetLong(MemoryModule::CantReachWalkTargetSince)) {
                    tired = (level.GetGameTime() - *since) > 200;
                }

                if (!target || !target->IsAlive() || !mob->CanAttack(*target) || tired
                    || FindTarget(*mob) != target) {
                    brain->EraseMemory(MemoryModule::AttackTarget);
                }
                return true;
            }
        };

        // MC StrollToPoi(HOME, 0.6, 2, 100) — drift back toward the post.
        class StrollToPoi : public Behavior {
        public:
            StrollToPoi(MemoryModule memory, float speedModifier, int closeEnoughDist,
                        int maxDistanceFromPoi)
                : Behavior({ MemoryCondition{ MemoryModule::WalkTarget,
                                              MemoryStatus::ValueAbsent },
                             MemoryCondition{ memory, MemoryStatus::ValuePresent } },
                           1),
                  m_memory(memory), m_speedModifier(speedModifier),
                  m_closeEnough(closeEnoughDist), m_maxDistance(maxDistanceFromPoi) {}
            const char* DebugString() const override { return "StrollToPoi"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                Brain* brain = body.GetBrain();
                if (!brain) return false;
                const std::optional<glm::ivec3> pos = brain->GetBlockPos(m_memory);
                if (!pos) return false;
                const glm::dvec3 centre(pos->x + 0.5, pos->y + 0.5, pos->z + 0.5);
                const glm::dvec3 d = centre - body.position;
                if (glm::dot(d, d) > static_cast<double>(m_maxDistance) * m_maxDistance) {
                    return false;
                }
                const int64_t now = level.GetGameTime();
                if (now <= m_nextOkStartTime) return true;
                brain->SetMemory(MemoryModule::WalkTarget,
                                 WalkTarget(*pos, m_speedModifier, m_closeEnough));
                m_nextOkStartTime = now + 80;
                return true;
            }

        private:
            MemoryModule m_memory;
            float m_speedModifier;
            int   m_closeEnough;
            int   m_maxDistance;
            int64_t m_nextOkStartTime = 0;
        };

        // MC StrollAroundPoi(HOME, 0.6, 5) — potter about near the post.
        class StrollAroundPoi : public Behavior {
        public:
            StrollAroundPoi(MemoryModule memory, float speedModifier,
                            int maxDistanceFromPoi)
                : Behavior({ MemoryCondition{ MemoryModule::WalkTarget,
                                              MemoryStatus::ValueAbsent },
                             MemoryCondition{ memory, MemoryStatus::ValuePresent } },
                           1),
                  m_memory(memory), m_speedModifier(speedModifier),
                  m_maxDistance(maxDistanceFromPoi) {}
            const char* DebugString() const override { return "StrollAroundPoi"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                auto* mob = dynamic_cast<PathfinderMob*>(&body);
                Brain* brain = body.GetBrain();
                if (!mob || !brain) return false;
                const std::optional<glm::ivec3> pos = brain->GetBlockPos(m_memory);
                if (!pos) return false;
                const glm::dvec3 centre(pos->x + 0.5, pos->y + 0.5, pos->z + 0.5);
                const glm::dvec3 d = centre - body.position;
                if (glm::dot(d, d) > static_cast<double>(m_maxDistance) * m_maxDistance) {
                    return false;
                }
                const int64_t now = level.GetGameTime();
                if (now <= m_nextOkStartTime) return true;
                if (const auto stroll = RandomPos::GetLandPos(*mob, 8, 6)) {
                    brain->SetMemory(MemoryModule::WalkTarget,
                                     WalkTarget(glm::ivec3(
                                                    static_cast<int>(std::floor(stroll->x)),
                                                    static_cast<int>(std::floor(stroll->y)),
                                                    static_cast<int>(std::floor(stroll->z))),
                                                m_speedModifier, 1));
                } else {
                    brain->EraseMemory(MemoryModule::WalkTarget);
                }
                m_nextOkStartTime = now + 180;
                return true;
            }

        private:
            MemoryModule m_memory;
            float m_speedModifier;
            int   m_maxDistance;
            int64_t m_nextOkStartTime = 0;
        };

    } // namespace

    void PiglinBruteAi::InitMemories(PiglinBrute& brute) {
        // MC: HOME = the GlobalPos it spawned at (single dimension here).
        if (Brain* brain = brute.GetBrain()) {
            brain->SetMemory(MemoryModule::Home, brute.BlockPosition());
        }
    }

    void PiglinBruteAi::InitBrain(PiglinBrute& brute, Brain& brain) {
        (void)brute;

        // MC PiglinBrute.MEMORY_TYPES (DOORS_TO_CLOSE skipped — no door
        // interaction on the brain path).
        for (MemoryModule m : { MemoryModule::LookTarget,
                                MemoryModule::NearestLivingEntities,
                                MemoryModule::NearestVisibleLivingEntities,
                                MemoryModule::NearestVisiblePlayer,
                                MemoryModule::NearestVisibleAttackablePlayer,
                                MemoryModule::NearestVisibleAdultPiglins,
                                MemoryModule::NearbyAdultPiglins,
                                MemoryModule::HurtBy,
                                MemoryModule::HurtByEntity,
                                MemoryModule::WalkTarget,
                                MemoryModule::CantReachWalkTargetSince,
                                MemoryModule::AttackTarget,
                                MemoryModule::AttackCoolingDown,
                                MemoryModule::InteractionTarget,
                                MemoryModule::Path,
                                MemoryModule::AngryAt,
                                MemoryModule::NearestVisibleNemesis,
                                MemoryModule::Home }) {
            brain.RegisterMemory(m);
        }

        // MC SENSOR_TYPES: NEAREST_LIVING_ENTITIES, NEAREST_PLAYERS, HURT_BY,
        // PIGLIN_BRUTE_SPECIFIC_SENSOR (NEAREST_ITEMS skipped — items). The
        // brute-specific sensor is small enough to inline: the wither-class
        // nemesis plus the nearby adult piglin list.
        brain.AddSensor(std::make_unique<NearestLivingEntitySensor>());
        brain.AddSensor(std::make_unique<PlayerSensor>());
        brain.AddSensor(std::make_unique<HurtBySensor>());
        class BruteSpecificSensor : public Sensor {
        public:
            std::vector<MemoryModule> Requires() const override {
                return { MemoryModule::NearestVisibleNemesis,
                         MemoryModule::NearbyAdultPiglins };
            }
        protected:
            void DoTick(EntityLevel&, LivingEntity& body) override {
                Brain* b = body.GetBrain();
                if (!b) return;
                LivingEntity* nemesis = nullptr;
                if (const NearestVisibleLivingEntities* visible = b->GetVisibleEntities(
                        MemoryModule::NearestVisibleLivingEntities)) {
                    nemesis = visible->FindClosest([](LivingEntity* e) {
                        return e->GetType() == EntityTypeId::WitherSkeleton
                            || e->GetType() == EntityTypeId::Wither;
                    });
                }
                if (nemesis) {
                    b->SetMemory(MemoryModule::NearestVisibleNemesis,
                                 static_cast<Entity*>(nemesis));
                } else {
                    b->EraseMemory(MemoryModule::NearestVisibleNemesis);
                }
                std::vector<Entity*> adults;
                if (const std::vector<Entity*>* raw =
                        b->GetEntityList(MemoryModule::NearestLivingEntities)) {
                    for (Entity* e : *raw) {
                        const EntityTypeId t = e->GetType();
                        if ((t == EntityTypeId::Piglin
                             || t == EntityTypeId::PiglinBrute)
                            && !e->IsBaby()) {
                            adults.push_back(e);
                        }
                    }
                }
                b->SetMemory(MemoryModule::NearbyAdultPiglins, std::move(adults));
            }
        };
        brain.AddSensor(std::make_unique<BruteSpecificSensor>());

        // ── CORE (MC initCoreActivity; InteractWithDoor skipped) ───────────
        std::vector<BehaviorPtr> core;
        core.push_back(std::make_unique<LookAtTargetSink>(45, 90));
        core.push_back(std::make_unique<MoveToTargetSink>());
        core.push_back(std::make_unique<StopBeingAngryIfTargetDead>());
        brain.AddActivity(Activity::Core, 0, std::move(core));

        // ── IDLE (MC initIdleActivity, priority 10; SetLookAndInteract
        // skipped — its consumer is the bartering interaction) ──────────────
        std::vector<BehaviorPtr> idle;
        idle.push_back(std::make_unique<StartAttacking>(
            [](Mob&) { return true; }, &FindTarget));
        // createIdleLookBehaviors.
        std::vector<GateBehavior::Entry> looks;
        looks.push_back({ std::make_unique<SetEntityLookTarget>(
                              [](LivingEntity& e) { return e.IsPlayer(); }, 8.0f), 1 });
        looks.push_back({ SetEntityLookTarget::OfType(EntityTypeId::Piglin, 8.0f), 1 });
        looks.push_back({ SetEntityLookTarget::OfType(EntityTypeId::PiglinBrute, 8.0f), 1 });
        looks.push_back({ SetEntityLookTarget::Any(8.0f), 1 });
        looks.push_back({ std::make_unique<DoNothing>(30, 60), 1 });
        idle.push_back(MakeRunOne(std::move(looks)));
        // createIdleMovementBehaviors.
        std::vector<GateBehavior::Entry> moves;
        moves.push_back({ RandomStroll::Stroll(0.6f), 2 });
        moves.push_back({ std::make_unique<InteractWith>(
                              EntityTypeId::Piglin, 8, 0.6f, 2), 2 });
        moves.push_back({ std::make_unique<InteractWith>(
                              EntityTypeId::PiglinBrute, 8, 0.6f, 2), 2 });
        moves.push_back({ std::make_unique<StrollToPoi>(MemoryModule::Home, 0.6f, 2, 100), 2 });
        moves.push_back({ std::make_unique<StrollAroundPoi>(MemoryModule::Home, 0.6f, 5), 2 });
        moves.push_back({ std::make_unique<DoNothing>(30, 60), 1 });
        idle.push_back(MakeRunOne(std::move(moves)));
        brain.AddActivity(Activity::Idle, 10, std::move(idle));

        // ── FIGHT (MC initFightActivity, priority 10) ──────────────────────
        std::vector<BehaviorPtr> fight;
        fight.push_back(std::make_unique<BruteStopAttacking>());
        fight.push_back(std::make_unique<SetWalkTargetFromAttackTarget>(1.0f));
        fight.push_back(std::make_unique<MeleeAttack>(20));
        brain.AddActivityAndRemoveMemoryWhenStopped(Activity::Fight, 10, std::move(fight),
                                                    MemoryModule::AttackTarget);

        brain.SetCoreActivities({ Activity::Core });
        brain.SetDefaultActivity(Activity::Idle);
        brain.UseDefaultActivity();
    }

    void PiglinBruteAi::UpdateActivity(PiglinBrute& brute) {
        Brain* brain = brute.GetBrain();
        if (!brain) return;
        // MC's FIGHT/IDLE switch; the activity-change and ambient angry
        // sounds are skipped.
        brain->SetActiveActivityToFirstValid({ Activity::Fight, Activity::Idle });
        brute.SetAggressive(brain->HasMemoryValue(MemoryModule::AttackTarget));
    }

    void PiglinBruteAi::WasHurtBy(EntityLevel& level, PiglinBrute& brute,
                                  LivingEntity& attacker) {
        // MC PiglinBruteAi.wasHurtBy — never against a fellow piglin.
        if (attacker.GetType() == EntityTypeId::Piglin
            || attacker.GetType() == EntityTypeId::PiglinBrute) {
            return;
        }
        PiglinAi::MaybeRetaliate(level, brute, attacker);
    }

} // namespace Game

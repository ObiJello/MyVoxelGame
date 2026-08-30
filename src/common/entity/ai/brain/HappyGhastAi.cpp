// File: src/common/entity/ai/brain/HappyGhastAi.cpp
#include "common/entity/ai/brain/HappyGhastAi.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/brain/CommonBehaviors.hpp"
#include "common/entity/ai/brain/CoreBehaviors.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"

#include <algorithm>

namespace Game {

    namespace {

        // MC AdultSensorAnyType — the nearest visible non-baby member of
        // EntityTypeTags.FOLLOWABLE_FRIENDLY_MOBS, flattened here.
        class AdultSensorAnyType : public Sensor {
        public:
            std::vector<MemoryModule> Requires() const override {
                return { MemoryModule::NearestVisibleAdult };
            }
        protected:
            void DoTick(EntityLevel&, LivingEntity& body) override {
                Brain* brain = body.GetBrain();
                if (!brain) return;
                const NearestVisibleLivingEntities* visible =
                    brain->GetVisibleEntities(MemoryModule::NearestVisibleLivingEntities);
                LivingEntity* adult = nullptr;
                if (visible) {
                    adult = visible->FindClosest([](LivingEntity* e) {
                        return !e->IsBaby() && IsFollowable(e->GetType());
                    });
                }
                if (adult) brain->SetMemory(MemoryModule::NearestVisibleAdult,
                                            static_cast<Entity*>(adult));
                else       brain->EraseMemory(MemoryModule::NearestVisibleAdult);
            }
        private:
            static bool IsFollowable(EntityTypeId t) {
                switch (t) {
                    case EntityTypeId::Armadillo: case EntityTypeId::Bee:
                    case EntityTypeId::Camel:     case EntityTypeId::Cat:
                    case EntityTypeId::Chicken:   case EntityTypeId::Cow:
                    case EntityTypeId::Donkey:    case EntityTypeId::Fox:
                    case EntityTypeId::Goat:      case EntityTypeId::HappyGhast:
                    case EntityTypeId::Horse:     case EntityTypeId::SkeletonHorse:
                    case EntityTypeId::Llama:     case EntityTypeId::Mule:
                    case EntityTypeId::Ocelot:    case EntityTypeId::Panda:
                    case EntityTypeId::Parrot:    case EntityTypeId::Pig:
                    case EntityTypeId::PolarBear: case EntityTypeId::Rabbit:
                    case EntityTypeId::Sheep:     case EntityTypeId::Sniffer:
                    case EntityTypeId::Strider:   case EntityTypeId::Villager:
                    case EntityTypeId::Wolf:
                        return true;
                    default:
                        return false;
                }
            }
        };

    } // namespace

    void HappyGhastAi::InitBrain(HappyGhast& ghast, Brain& brain) {
        // MC HappyGhastAi.MEMORY_TYPES, in declaration order.
        for (MemoryModule m : { MemoryModule::WalkTarget,
                                MemoryModule::LookTarget,
                                MemoryModule::CantReachWalkTargetSince,
                                MemoryModule::Path,
                                MemoryModule::NearestLivingEntities,
                                MemoryModule::NearestVisibleLivingEntities,
                                MemoryModule::TemptingPlayer,
                                MemoryModule::TemptationCooldownTicks,
                                MemoryModule::IsTempted,
                                MemoryModule::BreedTarget,
                                MemoryModule::IsPanicking,
                                MemoryModule::HurtBy,
                                MemoryModule::HurtByEntity,
                                MemoryModule::NearestVisibleAdult,
                                MemoryModule::NearestPlayers,
                                MemoryModule::NearestVisiblePlayer,
                                MemoryModule::NearestVisibleAttackablePlayer,
                                MemoryModule::NearestVisibleAttackablePlayers }) {
            brain.RegisterMemory(m);
        }

        // MC SENSOR_TYPES: NEAREST_LIVING_ENTITIES, HURT_BY,
        // FOOD_TEMPTATIONS, NEAREST_ADULT_ANY_TYPE, NEAREST_PLAYERS.
        brain.AddSensor(std::make_unique<NearestLivingEntitySensor>());
        brain.AddSensor(std::make_unique<HurtBySensor>());
        brain.AddSensor(std::make_unique<TemptingSensor>([&ghast](uint32_t item) {
            // ItemTags.HAPPY_GHAST_FOOD — the def's flattened food list
            // (snowballs).
            return ghast.IsFood(item);
        }));
        brain.AddSensor(std::make_unique<AdultSensorAnyType>());
        brain.AddSensor(std::make_unique<PlayerSensor>());

        // ── CORE (MC initCoreActivity) ─────────────────────────────────────
        std::vector<BehaviorPtr> core;
        core.push_back(std::make_unique<Swim>(0.8f));
        // MC AnimalPanic(2.0F, 0) — the 0 is the panic-distance override; the
        // port's AnimalPanic keeps its default flee search.
        core.push_back(std::make_unique<AnimalPanic>(2.0f));
        core.push_back(std::make_unique<LookAtTargetSink>(45, 90));
        core.push_back(std::make_unique<MoveToTargetSink>());
        core.push_back(std::make_unique<CountDownCooldownTicks>(
            MemoryModule::TemptationCooldownTicks));
        brain.AddActivity(Activity::Core, 0, std::move(core));

        // ── IDLE (MC initIdleActivity, explicit priority pairs) ────────────
        std::vector<BehaviorPtr> idle;
        // 1: FollowTemptation(1.25, 3.0, true).
        idle.push_back(std::make_unique<FollowTemptation>(
            SpeedFn([](LivingEntity&) { return 1.25f; }), 3.0));
        // 2: trail the nearest visible PLAYER; 3: trail any followable adult.
        idle.push_back(std::make_unique<BabyFollowAdult>(
            3, 16, SpeedFn([](LivingEntity&) { return 1.1f; }),
            MemoryModule::NearestVisiblePlayer, true));
        idle.push_back(std::make_unique<BabyFollowAdult>(
            3, 16, SpeedFn([](LivingEntity&) { return 1.1f; }),
            MemoryModule::NearestVisibleAdult, true));
        // 4: the flying wander.
        std::vector<GateBehavior::Entry> gate;
        gate.push_back({ RandomStroll::Fly(1.0f), 1 });
        gate.push_back({ std::make_unique<SetWalkTargetFromLookTarget>(1.0f, 3), 1 });
        idle.push_back(MakeRunOne(std::move(gate)));
        brain.AddActivity(Activity::Idle, 1, std::move(idle));

        // ── PANIC (MC initPanicActivity) ───────────────────────────────────
        // An EMPTY activity gated on IS_PANICKING: entering it shuts the
        // idle set down while the core's AnimalPanic does the running.
        brain.AddActivityWithConditions(
            Activity::Panic, 0, {},
            { MemoryCondition{ MemoryModule::IsPanicking,
                               MemoryStatus::ValuePresent } });

        brain.SetCoreActivities({ Activity::Core });
        brain.SetDefaultActivity(Activity::Idle);
        brain.UseDefaultActivity();
    }

    void HappyGhastAi::UpdateActivity(HappyGhast& ghast) {
        if (Brain* brain = ghast.GetBrain()) {
            brain->SetActiveActivityToFirstValid({ Activity::Panic, Activity::Idle });
        }
    }

} // namespace Game

// File: src/common/entity/ai/brain/PiglinAi.cpp
#include "common/entity/ai/brain/PiglinAi.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/brain/CommonBehaviors.hpp"
#include "common/entity/ai/brain/CoreBehaviors.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/crafting/RecipeManager.hpp"

#include <algorithm>

namespace Game {

    namespace {

        // MC's UniformInt constants, in ticks.
        constexpr int kTimeBetweenHuntsMin = 30 * 20,  kTimeBetweenHuntsMax = 120 * 20;
        constexpr int kRetreatMin          = 5 * 20,   kRetreatMax          = 20 * 20;
        constexpr int kAvoidZombifiedMin   = 5 * 20,   kAvoidZombifiedMax   = 7 * 20;
        constexpr int kBabyAvoidNemesisMin = 5 * 20,   kBabyAvoidNemesisMax = 7 * 20;
        constexpr int kRideStartMin        = 10 * 20,  kRideStartMax        = 40 * 20;
        constexpr int kRideDurationMin     = 10 * 20,  kRideDurationMax     = 30 * 20;
        constexpr int kAngerDuration       = 600;      // MC ANGER_DURATION
        constexpr int kCelebrationTime     = 300;      // MC CELEBRATION_TIME

        bool IsPiglinLike(const Entity& e) {
            return e.GetType() == EntityTypeId::Piglin
                || e.GetType() == EntityTypeId::PiglinBrute;
        }

        // MC Piglin.canHunt / PiglinBrute.canHunt, through the Mob*s the
        // NEARBY_ADULT_PIGLINS list carries.
        bool PiglinCanHunt(Mob& mob) {
            if (auto* piglin = dynamic_cast<Piglin*>(&mob)) return piglin->CanHunt();
            return false;   // the brute never hunts
        }

        bool IsAdultPiglin(const LivingEntity& e) {
            return IsPiglinLike(e) && !e.IsBaby();
        }

        // MC ItemTags.PIGLIN_LOVED, flattened to the items this game's
        // registry actually has (unknown slugs resolve to Air and drop out).
        const std::vector<ItemID>& LovedItems() {
            static const std::vector<ItemID> items = [] {
                std::vector<ItemID> out;
                for (const char* slug :
                     { "gold_ore", "deepslate_gold_ore", "nether_gold_ore",
                       "gold_block", "gilded_blackstone",
                       "light_weighted_pressure_plate", "gold_ingot", "bell",
                       "clock", "golden_carrot", "glistering_melon_slice",
                       "golden_apple", "enchanted_golden_apple",
                       "golden_helmet", "golden_chestplate", "golden_leggings",
                       "golden_boots", "golden_horse_armor", "golden_sword",
                       "golden_pickaxe", "golden_shovel", "golden_axe",
                       "golden_hoe", "raw_gold", "raw_gold_block" }) {
                    const ItemID id = RecipeManager::ItemFromSlug(slug);
                    if (id != Items::Air) out.push_back(id);
                }
                return out;
            }();
            return items;
        }

        void SampleAndSetHuntedRecently(Mob& piglin, JavaRandom& rng) {
            // MC PiglinAi.dontKillAnyMoreHoglinsForAWhile.
            if (Brain* brain = piglin.GetBrain()) {
                brain->SetMemoryWithExpiry(
                    MemoryModule::HuntedRecently, true,
                    rng.NextInt(kTimeBetweenHuntsMin, kTimeBetweenHuntsMax));
            }
        }

        std::vector<Entity*> AdultPiglinList(const Brain& brain, MemoryModule memory) {
            const std::vector<Entity*>* list = brain.GetEntityList(memory);
            return list ? *list : std::vector<Entity*>{};
        }

        // MC PiglinAi.isNearZombified.
        bool IsNearZombified(LivingEntity& body) {
            const Brain* brain = body.GetBrain();
            if (!brain) return false;
            Entity* z = brain->GetEntity(MemoryModule::NearestVisibleZombified);
            return z && body.DistanceToSqr(*z) <= 6.0 * 6.0;
        }

        // MC PiglinAi.hoglinsOutnumberPiglins.
        bool HoglinsOutnumberPiglins(LivingEntity& body) {
            const Brain* brain = body.GetBrain();
            if (!brain) return false;
            const int piglins =
                brain->GetInt(MemoryModule::VisibleAdultPiglinCount).value_or(0) + 1;
            const int hoglins =
                brain->GetInt(MemoryModule::VisibleAdultHoglinCount).value_or(0);
            return hoglins > piglins;
        }

        // MC PiglinAi.findNearestValidAttackTarget. UNIVERSAL_ANGER is gated
        // on a game rule that defaults OFF, so that branch is dead in vanilla
        // defaults and skipped here.
        LivingEntity* FindTarget(Mob& mob) {
            Brain* brain = mob.GetBrain();
            if (!brain) return nullptr;
            if (IsNearZombified(mob)) return nullptr;

            if (auto* angry =
                    dynamic_cast<LivingEntity*>(brain->GetEntity(MemoryModule::AngryAt))) {
                if (mob.CanAttack(*angry)) return angry;
            }
            if (auto* nemesis = dynamic_cast<LivingEntity*>(
                    brain->GetEntity(MemoryModule::NearestVisibleNemesis))) {
                return nemesis;
            }
            // MC NEAREST_TARGETABLE_PLAYER_NOT_WEARING_GOLD — with no
            // equipment system no player can wear gold, so every targetable
            // player qualifies (the sensor writes it on that basis).
            return dynamic_cast<LivingEntity*>(
                brain->GetEntity(MemoryModule::NearestTargetablePlayerNotWearingGold));
        }

        // MC PiglinAi.wantsToDance — 10% per kill, rolled from a RandomSource
        // seeded with the game time so every piglin at the party agrees.
        bool WantsToDance(EntityLevel& level, LivingEntity& target) {
            if (target.GetType() != EntityTypeId::Hoglin) return false;
            JavaRandom rng(level.GetGameTime());
            return rng.NextFloat() < 0.1f;
        }

        // MC PiglinAi.wantsToStopFleeing.
        bool WantsToStopFleeing(LivingEntity& body) {
            Brain* brain = body.GetBrain();
            if (!brain) return true;
            auto* avoided =
                dynamic_cast<LivingEntity*>(brain->GetEntity(MemoryModule::AvoidTarget));
            if (!avoided) return true;
            if (avoided->GetType() == EntityTypeId::Hoglin) {
                return !HoglinsOutnumberPiglins(body);
            }
            if (PiglinAi::IsZombified(*avoided)) {
                return !brain->IsMemoryValue(MemoryModule::NearestVisibleZombified,
                                             avoided);
            }
            return false;
        }

        // ── Local behaviours ───────────────────────────────────────────────

        // MC CopyMemoryWithExpiry — copy one memory into another with a rolled
        // TTL when the predicate holds (the baby's nemesis-flight and the
        // avoid-zombified reflex).
        class CopyMemoryWithExpiry : public Behavior {
        public:
            using Pred = std::function<bool(LivingEntity&)>;
            CopyMemoryWithExpiry(Pred pred, MemoryModule source, MemoryModule target,
                                 int durationMin, int durationMax)
                : Behavior({ MemoryCondition{ source, MemoryStatus::ValuePresent },
                             MemoryCondition{ target, MemoryStatus::ValueAbsent } },
                           1),
                  m_pred(std::move(pred)), m_source(source), m_target(target),
                  m_durationMin(durationMin), m_durationMax(durationMax) {}
            const char* DebugString() const override { return "CopyMemoryWithExpiry"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                Brain* brain = body.GetBrain();
                if (!brain || !m_pred(body)) return false;
                const MemoryValue* value = brain->GetMemory(m_source);
                if (!value) return false;
                brain->SetMemoryWithExpiry(
                    m_target, *value,
                    level.Random().NextInt(m_durationMin, m_durationMax));
                return true;
            }

        private:
            Pred m_pred;
            MemoryModule m_source, m_target;
            int m_durationMin, m_durationMax;
        };

        // MC piglin/StartHuntingHoglin.
        class StartHuntingHoglin : public Behavior {
        public:
            StartHuntingHoglin()
                : Behavior({ MemoryCondition{ MemoryModule::NearestVisibleHuntableHoglin,
                                              MemoryStatus::ValuePresent },
                             MemoryCondition{ MemoryModule::AngryAt,
                                              MemoryStatus::ValueAbsent },
                             MemoryCondition{ MemoryModule::HuntedRecently,
                                              MemoryStatus::ValueAbsent },
                             MemoryCondition{ MemoryModule::NearestVisibleAdultPiglins,
                                              MemoryStatus::Registered } },
                           1) {}
            const char* DebugString() const override { return "StartHuntingHoglin"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                auto* piglin = dynamic_cast<Piglin*>(&body);
                Brain* brain = body.GetBrain();
                if (!piglin || !brain) return false;
                // MC gates the whole behaviour on Piglin::canHunt via
                // BehaviorBuilder.triggerIf.
                if (!piglin->CanHunt()) return false;
                if (piglin->IsBaby()) return false;
                // MC: no hunt while any visible adult packmate has hunted
                // recently.
                for (Entity* e : AdultPiglinList(*brain,
                                                 MemoryModule::NearestVisibleAdultPiglins)) {
                    auto* mate = dynamic_cast<Mob*>(e);
                    const Brain* theirs = mate ? mate->GetBrain() : nullptr;
                    if (theirs && theirs->HasMemoryValue(MemoryModule::HuntedRecently)) {
                        return false;
                    }
                }
                auto* hoglin = dynamic_cast<LivingEntity*>(
                    brain->GetEntity(MemoryModule::NearestVisibleHuntableHoglin));
                if (!hoglin) return false;

                PiglinAi::SetAngerTarget(*piglin, *hoglin);
                SampleAndSetHuntedRecently(*piglin, level.Random());
                // MC broadcastAngerTarget + dontKillAnyMoreHoglinsForAWhile on
                // every packmate.
                for (Entity* e : AdultPiglinList(*brain, MemoryModule::NearbyAdultPiglins)) {
                    if (auto* mate = dynamic_cast<Mob*>(e)) {
                        if (PiglinCanHunt(*mate)) {
                            PiglinAi::SetAngerTarget(*mate, *hoglin);
                        }
                        SampleAndSetHuntedRecently(*mate, level.Random());
                    }
                }
                return true;
            }
        };

        // MC piglin/RememberIfHoglinWasKilled — arm the hunt cooldown the
        // moment the hoglin under attack dies.
        class RememberIfHoglinWasKilled : public Behavior {
        public:
            RememberIfHoglinWasKilled()
                : Behavior({ MemoryCondition{ MemoryModule::AttackTarget,
                                              MemoryStatus::ValuePresent },
                             MemoryCondition{ MemoryModule::HuntedRecently,
                                              MemoryStatus::Registered } },
                           1) {}
            const char* DebugString() const override { return "RememberIfHoglinWasKilled"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                auto* mob = dynamic_cast<Mob*>(&body);
                Brain* brain = body.GetBrain();
                if (!mob || !brain) return false;
                auto* target = dynamic_cast<LivingEntity*>(
                    brain->GetEntity(MemoryModule::AttackTarget));
                if (target && target->GetType() == EntityTypeId::Hoglin
                    && target->IsDeadOrDying()) {
                    SampleAndSetHuntedRecently(*mob, level.Random());
                }
                return true;
            }
        };

        // MC StartCelebratingIfTargetDead — party over the corpse.
        // FORGIVE_DEAD_PLAYERS defaults true, so the dead-player anger wipe is
        // unconditional here.
        class StartCelebratingIfTargetDead : public Behavior {
        public:
            explicit StartCelebratingIfTargetDead(int celebrateDuration)
                : Behavior({ MemoryCondition{ MemoryModule::AttackTarget,
                                              MemoryStatus::ValuePresent },
                             MemoryCondition{ MemoryModule::AngryAt,
                                              MemoryStatus::Registered },
                             MemoryCondition{ MemoryModule::CelebrateLocation,
                                              MemoryStatus::ValueAbsent },
                             MemoryCondition{ MemoryModule::Dancing,
                                              MemoryStatus::Registered } },
                           1),
                  m_duration(celebrateDuration) {}
            const char* DebugString() const override { return "StartCelebratingIfTargetDead"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                Brain* brain = body.GetBrain();
                if (!brain) return false;
                auto* target = dynamic_cast<LivingEntity*>(
                    brain->GetEntity(MemoryModule::AttackTarget));
                if (!target || !target->IsDeadOrDying()) return false;

                if (WantsToDance(level, *target)) {
                    brain->SetMemoryWithExpiry(MemoryModule::Dancing, true, m_duration);
                }
                brain->SetMemoryWithExpiry(MemoryModule::CelebrateLocation,
                                           target->BlockPosition(), m_duration);
                brain->EraseMemory(MemoryModule::AttackTarget);
                brain->EraseMemory(MemoryModule::AngryAt);
                return true;
            }

        private:
            int m_duration;
        };

        // MC GoToTargetLocation — shuffle toward a remembered block position
        // (the celebration site), jittered by one block each re-pick. The
        // optional predicate carries MC's triggerIf(isDancing/!isDancing)
        // wrappers.
        class GoToTargetLocation : public Behavior {
        public:
            using Pred = std::function<bool(LivingEntity&)>;
            GoToTargetLocation(MemoryModule location, int closeEnoughDist,
                               float speedModifier, Pred pred = {})
                : Behavior({ MemoryCondition{ location, MemoryStatus::ValuePresent },
                             MemoryCondition{ MemoryModule::AttackTarget,
                                              MemoryStatus::ValueAbsent },
                             MemoryCondition{ MemoryModule::WalkTarget,
                                              MemoryStatus::ValueAbsent },
                             MemoryCondition{ MemoryModule::LookTarget,
                                              MemoryStatus::Registered } },
                           1),
                  m_location(location), m_closeEnough(closeEnoughDist),
                  m_speedModifier(speedModifier), m_pred(std::move(pred)) {}
            const char* DebugString() const override { return "GoToTargetLocation"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                Brain* brain = body.GetBrain();
                if (!brain) return false;
                if (m_pred && !m_pred(body)) return false;
                const std::optional<glm::ivec3> pos = brain->GetBlockPos(m_location);
                if (!pos) return false;

                const glm::ivec3 mine = body.BlockPosition();
                const glm::ivec3 d = *pos - mine;
                if (static_cast<double>(d.x) * d.x + static_cast<double>(d.y) * d.y
                        + static_cast<double>(d.z) * d.z
                    >= static_cast<double>(m_closeEnough) * m_closeEnough) {
                    JavaRandom& rng = level.Random();
                    const glm::ivec3 jittered = *pos + glm::ivec3(rng.NextInt(3) - 1, 0,
                                                                  rng.NextInt(3) - 1);
                    brain->SetMemory(MemoryModule::LookTarget,
                                     PositionTracker::OfBlock(jittered));
                    brain->SetMemory(MemoryModule::WalkTarget,
                                     WalkTarget(PositionTracker::OfBlock(jittered),
                                                m_speedModifier, m_closeEnough));
                }
                return true;
            }

        private:
            MemoryModule m_location;
            int   m_closeEnough;
            float m_speedModifier;
            Pred  m_pred;
        };

        // MC's StopAttackingIfTargetInvalid.create(target != nearest valid) —
        // the shared body plus the piglin's tighter validity rule.
        class PiglinStopAttacking : public Behavior {
        public:
            PiglinStopAttacking()
                : Behavior({ MemoryCondition{ MemoryModule::AttackTarget,
                                              MemoryStatus::ValuePresent },
                             MemoryCondition{ MemoryModule::CantReachWalkTargetSince,
                                              MemoryStatus::Registered } },
                           1) {}
            const char* DebugString() const override { return "PiglinStopAttacking"; }

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

        // MC piglin babySometimesRideBabyHoglin — the ticker-gated copy of
        // NEAREST_VISIBLE_BABY_HOGLIN into RIDE_TARGET.
        class BabySometimesRideBabyHoglin : public Behavior {
        public:
            BabySometimesRideBabyHoglin()
                : Behavior({ MemoryCondition{ MemoryModule::NearestVisibleBabyHoglin,
                                              MemoryStatus::ValuePresent },
                             MemoryCondition{ MemoryModule::RideTarget,
                                              MemoryStatus::ValueAbsent } },
                           1) {}
            const char* DebugString() const override { return "BabySometimesRideBabyHoglin"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                if (!body.IsBaby()) return false;
                // MC SetEntityLookTargetSometimes.Ticker over RIDE_START_INTERVAL.
                JavaRandom& rng = level.Random();
                if (m_ticksUntilRide == 0) {
                    m_ticksUntilRide = rng.NextInt(kRideStartMin, kRideStartMax) - 1;
                    return false;
                }
                if (--m_ticksUntilRide != 0) return false;

                Brain* brain = body.GetBrain();
                if (!brain) return false;
                const MemoryValue* hoglin =
                    brain->GetMemory(MemoryModule::NearestVisibleBabyHoglin);
                if (!hoglin) return false;
                brain->SetMemoryWithExpiry(
                    MemoryModule::RideTarget, *hoglin,
                    rng.NextInt(kRideDurationMin, kRideDurationMax));
                return true;
            }

        private:
            int m_ticksUntilRide = 0;
        };

        // MC Mount — walk to the RIDE_TARGET and climb on. The baby-on-hoglin
        // pile-up rule (ride the TOP passenger of a stack, three high) is
        // MC Piglin.startRiding's, applied here where the ride begins.
        class PiglinMount : public Behavior {
        public:
            explicit PiglinMount(float speedModifier)
                : Behavior({ MemoryCondition{ MemoryModule::LookTarget,
                                              MemoryStatus::Registered },
                             MemoryCondition{ MemoryModule::WalkTarget,
                                              MemoryStatus::ValueAbsent },
                             MemoryCondition{ MemoryModule::RideTarget,
                                              MemoryStatus::ValuePresent } },
                           1),
                  m_speedModifier(speedModifier) {}
            const char* DebugString() const override { return "PiglinMount"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                if (body.IsPassenger()) return false;
                Brain* brain = body.GetBrain();
                if (!brain) return false;
                Entity* target = brain->GetEntity(MemoryModule::RideTarget);
                if (!target) return false;

                if (body.DistanceToSqr(*target) <= 1.0) {
                    // MC Piglin.startRiding: a baby joining a hoglin ride
                    // stacks on the top passenger, at most three high.
                    Entity* seat = target;
                    if (body.IsBaby() && target->GetType() == EntityTypeId::Hoglin) {
                        int depth = 3;
                        while (depth > 1 && !seat->GetPassengers().empty()) {
                            seat = seat->GetPassengers().front();
                            --depth;
                        }
                    }
                    body.StartRiding(*seat, /*force=*/true);
                } else {
                    brain->SetMemory(MemoryModule::LookTarget,
                                     PositionTracker::OfEntity(target, true));
                    brain->SetMemory(MemoryModule::WalkTarget,
                                     WalkTarget(PositionTracker::OfEntity(target, false),
                                                m_speedModifier, 1));
                }
                return true;
            }

        private:
            float m_speedModifier;
        };

        // MC DismountOrSkipMounting(8, wantsToStopRiding).
        class PiglinDismount : public Behavior {
        public:
            PiglinDismount()
                : Behavior({ MemoryCondition{ MemoryModule::RideTarget,
                                              MemoryStatus::Registered } },
                           1) {}
            const char* DebugString() const override { return "PiglinDismount"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                Brain* brain = body.GetBrain();
                if (!brain) return false;
                Entity* current = body.GetVehicle();
                Entity* target = brain->GetEntity(MemoryModule::RideTarget);
                Entity* vehicle = current ? current : target;
                if (!vehicle) return false;

                if (IsVehicleValid(body, *vehicle) && !WantsToStopRiding(body, *vehicle)) {
                    return false;
                }
                body.StopRiding();
                brain->EraseMemory(MemoryModule::RideTarget);
                return true;
            }

        private:
            static bool IsVehicleValid(LivingEntity& body, Entity& vehicle) {
                return vehicle.IsAlive() && body.DistanceToSqr(vehicle) <= 8.0 * 8.0;
            }
            // MC PiglinAi.wantsToStopRiding.
            static bool WantsToStopRiding(LivingEntity& body, Entity& vehicle) {
                auto* mob = dynamic_cast<Mob*>(&vehicle);
                if (!mob) return false;
                const Brain* mine = body.GetBrain();
                const Brain* theirs = mob->GetBrain();
                const bool hurtRecently =
                    (mine && mine->HasMemoryValue(MemoryModule::HurtBy))
                    || (theirs && theirs->HasMemoryValue(MemoryModule::HurtBy));
                return !mob->IsBaby() || !mob->IsAlive() || hurtRecently
                    || (IsPiglinLike(*mob) && mob->GetVehicle() == nullptr);
            }
        };

        // ── PiglinSpecificSensor ───────────────────────────────────────────
        //
        // MC ai/sensing/PiglinSpecificSensor: one pass over the visible set,
        // classifying everything the piglin brain reads, plus the soul-block
        // repellent scan.
        class PiglinSpecificSensor : public Sensor {
        public:
            std::vector<MemoryModule> Requires() const override {
                return { MemoryModule::NearestVisibleNemesis,
                         MemoryModule::NearestVisibleHuntableHoglin,
                         MemoryModule::NearestVisibleBabyHoglin,
                         MemoryModule::NearestVisibleZombified,
                         MemoryModule::NearestTargetablePlayerNotWearingGold,
                         MemoryModule::NearestPlayerHoldingWantedItem,
                         MemoryModule::NearbyAdultPiglins,
                         MemoryModule::NearestVisibleAdultPiglins,
                         MemoryModule::VisibleAdultPiglinCount,
                         MemoryModule::VisibleAdultHoglinCount,
                         MemoryModule::NearestRepellent };
            }

        protected:
            void DoTick(EntityLevel& level, LivingEntity& body) override {
                Brain* brain = body.GetBrain();
                if (!brain) return;
                auto* mob = dynamic_cast<Mob*>(&body);

                // MC findNearestRepellent — BlockTags.PIGLIN_REPELLENTS within
                // 8 horizontally / 4 vertically. The tag here is the soul
                // blocks the registry has: soul torch (+wall), soul lantern,
                // soul campfire and soul fire itself. The campfire lit check
                // is skipped (no lit blockstate reader on this path).
                if (const std::optional<glm::ivec3> repellent =
                        FindNearestRepellent(level, body)) {
                    brain->SetMemory(MemoryModule::NearestRepellent, *repellent);
                } else {
                    brain->EraseMemory(MemoryModule::NearestRepellent);
                }

                LivingEntity* nemesis = nullptr;
                LivingEntity* huntableHoglin = nullptr;
                LivingEntity* babyHoglin = nullptr;
                LivingEntity* zombified = nullptr;
                LivingEntity* playerNotWearingGold = nullptr;
                LivingEntity* playerHoldingWantedItem = nullptr;
                int adultHoglinCount = 0;
                std::vector<Entity*> visibleAdultPiglins;

                const NearestVisibleLivingEntities* visible =
                    brain->GetVisibleEntities(MemoryModule::NearestVisibleLivingEntities);
                if (visible) {
                    for (LivingEntity* e : visible->entities) {
                        // MC iterates through findAll(...), which applies the
                        // snapshot's query-time visibility predicate — the raw
                        // list now holds unseen entities too.
                        if (!visible->IsVisible(e)) continue;
                        const EntityTypeId t = e->GetType();
                        if (t == EntityTypeId::Hoglin) {
                            if (e->IsBaby()) {
                                if (!babyHoglin) babyHoglin = e;
                            } else {
                                ++adultHoglinCount;
                                // MC also asks Hoglin.canBeHunted (false for a
                                // zombification-immune hoglin); the port's
                                // hoglin has no immunity flag, so every adult
                                // qualifies.
                                if (!huntableHoglin) huntableHoglin = e;
                            }
                        } else if (t == EntityTypeId::PiglinBrute) {
                            visibleAdultPiglins.push_back(e);
                        } else if (t == EntityTypeId::Piglin) {
                            if (!e->IsBaby()) visibleAdultPiglins.push_back(e);
                        } else if (e->IsPlayer()) {
                            // MC: "not wearing gold" — no equipment system, so
                            // no player ever wears the safe armor.
                            if (!playerNotWearingGold && mob && mob->CanAttack(*e)) {
                                playerNotWearingGold = e;
                            }
                            if (!playerHoldingWantedItem
                                && PiglinAi::IsPlayerHoldingLovedItem(level, *e)) {
                                playerHoldingWantedItem = e;
                            }
                        } else if (!nemesis && (t == EntityTypeId::WitherSkeleton
                                                || t == EntityTypeId::Wither)) {
                            nemesis = e;
                        } else if (!zombified && PiglinAi::IsZombified(*e)) {
                            zombified = e;
                        }
                    }
                }

                // MC findNearbyAdultPiglins — from the RAW list, not just the
                // visible one.
                std::vector<Entity*> nearbyAdultPiglins;
                if (const std::vector<Entity*>* raw =
                        brain->GetEntityList(MemoryModule::NearestLivingEntities)) {
                    for (Entity* e : *raw) {
                        auto* living = dynamic_cast<LivingEntity*>(e);
                        if (living && IsAdultPiglin(*living)) {
                            nearbyAdultPiglins.push_back(e);
                        }
                    }
                }

                SetOrErase(*brain, MemoryModule::NearestVisibleNemesis, nemesis);
                SetOrErase(*brain, MemoryModule::NearestVisibleHuntableHoglin,
                           huntableHoglin);
                SetOrErase(*brain, MemoryModule::NearestVisibleBabyHoglin, babyHoglin);
                SetOrErase(*brain, MemoryModule::NearestVisibleZombified, zombified);
                SetOrErase(*brain, MemoryModule::NearestTargetablePlayerNotWearingGold,
                           playerNotWearingGold);
                SetOrErase(*brain, MemoryModule::NearestPlayerHoldingWantedItem,
                           playerHoldingWantedItem);
                brain->SetMemory(MemoryModule::NearbyAdultPiglins,
                                 std::move(nearbyAdultPiglins));
                brain->SetMemory(MemoryModule::VisibleAdultPiglinCount,
                                 static_cast<int>(visibleAdultPiglins.size()));
                brain->SetMemory(MemoryModule::VisibleAdultHoglinCount, adultHoglinCount);
                brain->SetMemory(MemoryModule::NearestVisibleAdultPiglins,
                                 std::move(visibleAdultPiglins));
            }

        private:
            static void SetOrErase(Brain& brain, MemoryModule m, LivingEntity* e) {
                if (e) brain.SetMemory(m, static_cast<Entity*>(e));
                else   brain.EraseMemory(m);
            }

            static bool IsRepellentBlock(BlockID id) {
                return id == BlockID::SoulTorch || id == BlockID::SoulWallTorch
                    || id == BlockID::SoulLantern || id == BlockID::SoulCampfire
                    || id == BlockID::SoulFire;
            }

            static std::optional<glm::ivec3> FindNearestRepellent(EntityLevel& level,
                                                                  LivingEntity& body) {
                const IBlockAccess* blocks = level.Blocks();
                if (!blocks) return std::nullopt;
                const glm::ivec3 origin = body.BlockPosition();
                std::optional<glm::ivec3> best;
                double bestDistSq = 0.0;
                for (int dx = -8; dx <= 8; ++dx) {
                    for (int dy = -4; dy <= 4; ++dy) {
                        for (int dz = -8; dz <= 8; ++dz) {
                            const glm::ivec3 p = origin + glm::ivec3(dx, dy, dz);
                            if (!IsRepellentBlock(blocks->GetBlock(p.x, p.y, p.z))) {
                                continue;
                            }
                            const double d = static_cast<double>(dx) * dx
                                           + static_cast<double>(dy) * dy
                                           + static_cast<double>(dz) * dz;
                            if (!best || d < bestDistSq) { best = p; bestDistSq = d; }
                        }
                    }
                }
                return best;
            }
        };

        // MC createLookBehaviors + createIdleLookBehaviors.
        BehaviorPtr IdleLookBehaviors() {
            std::vector<GateBehavior::Entry> gate;
            gate.push_back({ std::make_unique<SetEntityLookTarget>(
                                 [](LivingEntity& e) { return e.IsPlayer(); }, 8.0f), 1 });
            gate.push_back({ SetEntityLookTarget::OfType(EntityTypeId::Piglin, 8.0f), 1 });
            gate.push_back({ SetEntityLookTarget::Any(8.0f), 1 });
            gate.push_back({ std::make_unique<DoNothing>(30, 60), 1 });
            return MakeRunOne(std::move(gate));
        }

        // MC createIdleMovementBehaviors.
        BehaviorPtr IdleMovementBehaviors(EntityLevel* level) {
            (void)level;
            std::vector<GateBehavior::Entry> gate;
            gate.push_back({ RandomStroll::Stroll(0.6f), 2 });
            gate.push_back({ std::make_unique<InteractWith>(
                                 EntityTypeId::Piglin, 8, 0.6f, 2), 2 });
            // MC wraps this in triggerIf(doesntSeeAnyPlayerHoldingLovedItem).
            gate.push_back({ std::make_unique<SetWalkTargetFromLookTarget>(
                                 [](LivingEntity& body) {
                                     const Brain* brain = body.GetBrain();
                                     return brain && !brain->HasMemoryValue(
                                         MemoryModule::NearestPlayerHoldingWantedItem);
                                 },
                                 SpeedFn([](LivingEntity&) { return 0.6f; }), 3), 2 });
            gate.push_back({ std::make_unique<DoNothing>(30, 60), 1 });
            return MakeRunOne(std::move(gate));
        }

        // MC avoidRepellent.
        BehaviorPtr AvoidRepellent() {
            return SetWalkTargetAwayFrom::Pos(MemoryModule::NearestRepellent, 1.0f, 8,
                                              false);
        }

    } // namespace

    // ── Public helpers ─────────────────────────────────────────────────────

    bool PiglinAi::IsZombified(const Entity& entity) {
        return entity.GetType() == EntityTypeId::ZombifiedPiglin
            || entity.GetType() == EntityTypeId::Zoglin;
    }

    bool PiglinAi::IsPlayerHoldingLovedItem(EntityLevel& level, LivingEntity& entity) {
        if (!entity.IsPlayer()) return false;
        const uint32_t held = level.GetHeldItemId(entity);
        if (held == 0) return false;
        const std::vector<ItemID>& loved = LovedItems();
        return std::find(loved.begin(), loved.end(), static_cast<ItemID>(held))
               != loved.end();
    }

    void PiglinAi::SetAngerTarget(Mob& piglin, LivingEntity& target) {
        // MC PiglinAi.setAngerTarget. The attackable pre-check runs in the
        // callers that need it (StartAttacking re-checks either way); the
        // UNIVERSAL_ANGER game rule defaults off, so that branch is skipped.
        Brain* brain = piglin.GetBrain();
        if (!brain || !piglin.CanAttack(target)) return;
        brain->EraseMemory(MemoryModule::CantReachWalkTargetSince);
        brain->SetMemoryWithExpiry(MemoryModule::AngryAt,
                                   static_cast<Entity*>(&target), kAngerDuration);
        if (target.GetType() == EntityTypeId::Hoglin && PiglinCanHunt(piglin)) {
            if (EntityLevel* level = piglin.Level()) {
                SampleAndSetHuntedRecently(piglin, level->Random());
            }
        }
    }

    void PiglinAi::MaybeRetaliate(EntityLevel& level, Mob& piglin,
                                  LivingEntity& attacker) {
        (void)level;
        Brain* brain = piglin.GetBrain();
        if (!brain) return;
        // MC: never retaliate out of an active retreat.
        if (brain->IsActive(Activity::Avoid)) return;
        if (!piglin.CanAttack(attacker)) return;

        // MC isOtherTargetMuchFurtherAwayThanCurrentAttackTarget(4.0).
        if (auto* current = dynamic_cast<LivingEntity*>(
                brain->GetEntity(MemoryModule::AttackTarget))) {
            if (piglin.DistanceToSqr(attacker)
                > piglin.DistanceToSqr(*current) + 4.0 * 4.0) {
                return;
            }
        }

        SetAngerTarget(piglin, attacker);
        // MC broadcastAngerTarget across the nearby adult pack.
        for (Entity* e : AdultPiglinList(*brain, MemoryModule::NearbyAdultPiglins)) {
            auto* mate = dynamic_cast<Mob*>(e);
            if (!mate) continue;
            if (attacker.GetType() == EntityTypeId::Hoglin && !PiglinCanHunt(*mate)) {
                continue;
            }
            // MC setAngerTargetIfCloserThanCurrent.
            Brain* theirs = mate->GetBrain();
            auto* theirTarget = theirs ? dynamic_cast<LivingEntity*>(
                theirs->GetEntity(MemoryModule::AngryAt)) : nullptr;
            if (!theirTarget
                || mate->DistanceToSqr(attacker) < mate->DistanceToSqr(*theirTarget)) {
                SetAngerTarget(*mate, attacker);
            }
        }
    }

    void PiglinAi::WasHurtBy(EntityLevel& level, Piglin& piglin,
                             LivingEntity& attacker) {
        // MC PiglinAi.wasHurtBy. The offhand-item drop and the ADMIRING
        // memory wipes are items-system work and skipped; the celebration
        // stop, baby flight, outnumbered retreat and retaliation are ported.
        if (IsPiglinLike(attacker)) return;
        Brain* brain = piglin.GetBrain();
        if (!brain) return;

        brain->EraseMemory(MemoryModule::CelebrateLocation);
        brain->EraseMemory(MemoryModule::Dancing);

        // MC: an avoid target of a DIFFERENT type than the attacker is
        // dropped, so the new threat can take the slot.
        if (auto* avoiding = dynamic_cast<LivingEntity*>(
                brain->GetEntity(MemoryModule::AvoidTarget))) {
            if (avoiding->GetType() != attacker.GetType()) {
                brain->EraseMemory(MemoryModule::AvoidTarget);
            }
        }

        JavaRandom& rng = level.Random();
        if (piglin.IsBaby()) {
            // MC BABY_FLEE_DURATION_AFTER_GETTING_HIT = 100.
            brain->SetMemoryWithExpiry(MemoryModule::AvoidTarget,
                                       static_cast<Entity*>(&attacker), 100);
            if (piglin.CanAttack(attacker)) {
                // broadcastAngerTarget without retaliating itself.
                for (Entity* e : AdultPiglinList(*brain,
                                                 MemoryModule::NearbyAdultPiglins)) {
                    auto* mate = dynamic_cast<Mob*>(e);
                    if (!mate) continue;
                    if (attacker.GetType() == EntityTypeId::Hoglin
                        && !PiglinCanHunt(*mate)) {
                        continue;
                    }
                    SetAngerTarget(*mate, attacker);
                }
            }
        } else if (attacker.GetType() != EntityTypeId::Hoglin
                   && HoglinsOutnumberPiglins(piglin)) {
            // MC setAvoidTargetAndDontHuntForAWhile + broadcastRetreat.
            auto retreat = [&rng](Piglin& p, LivingEntity& threat) {
                Brain* b = p.GetBrain();
                if (!b) return;
                b->EraseMemory(MemoryModule::AngryAt);
                b->EraseMemory(MemoryModule::AttackTarget);
                b->EraseMemory(MemoryModule::WalkTarget);
                b->SetMemoryWithExpiry(MemoryModule::AvoidTarget,
                                       static_cast<Entity*>(&threat),
                                       rng.NextInt(kRetreatMin, kRetreatMax));
                SampleAndSetHuntedRecently(p, rng);
            };
            retreat(piglin, attacker);
            for (Entity* e : AdultPiglinList(*brain,
                                             MemoryModule::NearestVisibleAdultPiglins)) {
                if (auto* mate = dynamic_cast<Piglin*>(e)) {
                    // MC retreatFromNearestTarget picks the nearest of the
                    // mate's current threats and the new one; the new threat
                    // stands in — the mate's own sensors resettle it.
                    retreat(*mate, attacker);
                }
            }
        } else {
            MaybeRetaliate(level, piglin, attacker);
        }
    }

    void PiglinAi::InitMemories(Piglin& piglin) {
        if (EntityLevel* level = piglin.Level()) {
            SampleAndSetHuntedRecently(piglin, level->Random());
        }
    }

    void PiglinAi::InitBrain(Piglin& piglin, Brain& brain) {
        // MC Piglin.MEMORY_TYPES, minus the item/door machinery this port
        // does not have (DOORS_TO_CLOSE, NEAREST_VISIBLE_WANTED_ITEM,
        // ITEM_PICKUP_COOLDOWN_TICKS, ADMIRING_*, TIME_TRYING_TO_REACH_
        // ADMIRE_ITEM, DISABLE_WALK_TO_ADMIRE_ITEM, UNIVERSAL_ANGER, and the
        // five SPEAR_* memories).
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
                                MemoryModule::AvoidTarget,
                                MemoryModule::CelebrateLocation,
                                MemoryModule::Dancing,
                                MemoryModule::HuntedRecently,
                                MemoryModule::NearestVisibleBabyHoglin,
                                MemoryModule::NearestVisibleNemesis,
                                MemoryModule::NearestVisibleZombified,
                                MemoryModule::RideTarget,
                                MemoryModule::VisibleAdultPiglinCount,
                                MemoryModule::VisibleAdultHoglinCount,
                                MemoryModule::NearestVisibleHuntableHoglin,
                                MemoryModule::NearestTargetablePlayerNotWearingGold,
                                MemoryModule::NearestPlayerHoldingWantedItem,
                                MemoryModule::AteRecently,
                                MemoryModule::NearestRepellent }) {
            brain.RegisterMemory(m);
        }

        // MC SENSOR_TYPES: NEAREST_LIVING_ENTITIES, NEAREST_PLAYERS, HURT_BY,
        // PIGLIN_SPECIFIC_SENSOR (NEAREST_ITEMS skipped — no item entities on
        // this path).
        brain.AddSensor(std::make_unique<NearestLivingEntitySensor>());
        brain.AddSensor(std::make_unique<PlayerSensor>());
        brain.AddSensor(std::make_unique<HurtBySensor>());
        brain.AddSensor(std::make_unique<PiglinSpecificSensor>());

        EntityLevel* level = piglin.Level();

        // ── CORE (MC initCoreActivity) ─────────────────────────────────────
        // InteractWithDoor, StopHoldingItemIfNoLongerAdmiring and
        // StartAdmiringItemIfSeen are skipped — doors and items.
        std::vector<BehaviorPtr> core;
        core.push_back(std::make_unique<LookAtTargetSink>(45, 90));
        core.push_back(std::make_unique<MoveToTargetSink>());
        // babyAvoidNemesis.
        core.push_back(std::make_unique<CopyMemoryWithExpiry>(
            [](LivingEntity& e) { return e.IsBaby(); },
            MemoryModule::NearestVisibleNemesis, MemoryModule::AvoidTarget,
            kBabyAvoidNemesisMin, kBabyAvoidNemesisMax));
        // avoidZombified.
        core.push_back(std::make_unique<CopyMemoryWithExpiry>(
            &IsNearZombified,
            MemoryModule::NearestVisibleZombified, MemoryModule::AvoidTarget,
            kAvoidZombifiedMin, kAvoidZombifiedMax));
        core.push_back(std::make_unique<StartCelebratingIfTargetDead>(kCelebrationTime));
        core.push_back(std::make_unique<StopBeingAngryIfTargetDead>());
        brain.AddActivity(Activity::Core, 0, std::move(core));

        // ── IDLE (MC initIdleActivity, priority 10) ────────────────────────
        // SetLookAndInteract is skipped — its downstream consumer is the
        // bartering interaction.
        std::vector<BehaviorPtr> idle;
        idle.push_back(std::make_unique<SetEntityLookTarget>(
            [level](LivingEntity& e) {
                return level && PiglinAi::IsPlayerHoldingLovedItem(*level, e);
            }, 14.0f));
        idle.push_back(std::make_unique<StartAttacking>(
            [](Mob& m) { return !m.IsBaby(); }, &FindTarget));
        idle.push_back(std::make_unique<StartHuntingHoglin>());
        idle.push_back(AvoidRepellent());
        idle.push_back(std::make_unique<BabySometimesRideBabyHoglin>());
        idle.push_back(IdleLookBehaviors());
        idle.push_back(IdleMovementBehaviors(level));
        brain.AddActivity(Activity::Idle, 10, std::move(idle));

        // ── FIGHT (MC initFightActivity, priority 10) ──────────────────────
        // BackUpIfTooClose + CrossbowAttack (crossbow) and the three Spear
        // behaviours are skipped — no weapon items exist to hold.
        std::vector<BehaviorPtr> fight;
        fight.push_back(std::make_unique<PiglinStopAttacking>());
        fight.push_back(std::make_unique<SetWalkTargetFromAttackTarget>(1.0f));
        fight.push_back(std::make_unique<MeleeAttack>(20));
        fight.push_back(std::make_unique<RememberIfHoglinWasKilled>());
        fight.push_back(std::make_unique<EraseMemoryIf>(&IsNearZombified,
                                                        MemoryModule::AttackTarget));
        brain.AddActivityAndRemoveMemoryWhenStopped(Activity::Fight, 10, std::move(fight),
                                                    MemoryModule::AttackTarget);

        // ── CELEBRATE (MC initCelebrateActivity, priority 10) ──────────────
        std::vector<BehaviorPtr> celebrate;
        celebrate.push_back(AvoidRepellent());
        celebrate.push_back(std::make_unique<SetEntityLookTarget>(
            [level](LivingEntity& e) {
                return level && PiglinAi::IsPlayerHoldingLovedItem(*level, e);
            }, 14.0f));
        celebrate.push_back(std::make_unique<StartAttacking>(
            [](Mob& m) { return !m.IsBaby(); }, &FindTarget));
        auto isDancing = [](LivingEntity& e) {
            const Brain* b = e.GetBrain();
            return b && b->HasMemoryValue(MemoryModule::Dancing);
        };
        celebrate.push_back(std::make_unique<GoToTargetLocation>(
            MemoryModule::CelebrateLocation, 2, 1.0f,
            [isDancing](LivingEntity& e) { return !isDancing(e); }));
        celebrate.push_back(std::make_unique<GoToTargetLocation>(
            MemoryModule::CelebrateLocation, 4, 0.6f, isDancing));
        std::vector<GateBehavior::Entry> party;
        party.push_back({ SetEntityLookTarget::OfType(EntityTypeId::Piglin, 8.0f), 1 });
        party.push_back({ RandomStroll::Stroll(0.6f, 2, 1), 1 });
        party.push_back({ std::make_unique<DoNothing>(10, 20), 1 });
        celebrate.push_back(MakeRunOne(std::move(party)));
        brain.AddActivityAndRemoveMemoryWhenStopped(Activity::Celebrate, 10,
                                                    std::move(celebrate),
                                                    MemoryModule::CelebrateLocation);

        // ── ADMIRE_ITEM is not registered — the whole activity is the item
        // system (GoToWantedItem, StopAdmiring*). UpdateActivity still asks
        // for it first, exactly as MC orders the list; an unregistered
        // activity simply never validates.

        // ── AVOID (MC initRetreatActivity, priority 10) ────────────────────
        std::vector<BehaviorPtr> avoid;
        avoid.push_back(std::make_unique<SetWalkTargetAwayFrom>(
            MemoryModule::AvoidTarget, 1.0f, 12, true));
        avoid.push_back(IdleLookBehaviors());
        avoid.push_back(IdleMovementBehaviors(level));
        avoid.push_back(std::make_unique<EraseMemoryIf>(&WantsToStopFleeing,
                                                        MemoryModule::AvoidTarget));
        brain.AddActivityAndRemoveMemoryWhenStopped(Activity::Avoid, 10, std::move(avoid),
                                                    MemoryModule::AvoidTarget);

        // ── RIDE (MC initRideHoglinActivity, priority 10) ──────────────────
        std::vector<BehaviorPtr> ride;
        ride.push_back(std::make_unique<PiglinMount>(0.8f));
        ride.push_back(std::make_unique<SetEntityLookTarget>(
            [level](LivingEntity& e) {
                return level && PiglinAi::IsPlayerHoldingLovedItem(*level, e);
            }, 8.0f));
        // MC's passenger-gated shuffled look gate, flattened: the look
        // behaviours themselves carry the "while riding" gate.
        ride.push_back(std::make_unique<PiglinDismount>());
        brain.AddActivityAndRemoveMemoryWhenStopped(Activity::Ride, 10, std::move(ride),
                                                    MemoryModule::RideTarget);

        brain.SetCoreActivities({ Activity::Core });
        brain.SetDefaultActivity(Activity::Idle);
        brain.UseDefaultActivity();
    }

    void PiglinAi::UpdateActivity(Piglin& piglin) {
        Brain* brain = piglin.GetBrain();
        if (!brain) return;
        // MC's order, ADMIRE_ITEM included (it never validates here — the
        // activity is not registered without the item system). The activity-
        // change sounds are skipped.
        brain->SetActiveActivityToFirstValid(
            { Activity::AdmireItem, Activity::Fight, Activity::Avoid,
              Activity::Celebrate, Activity::Ride, Activity::Idle });

        piglin.SetAggressive(brain->HasMemoryValue(MemoryModule::AttackTarget));

        // MC: a baby stacked on another baby dismounts when the ride memory
        // lapses.
        if (!brain->HasMemoryValue(MemoryModule::RideTarget) && piglin.IsBaby()) {
            auto* vehicle = dynamic_cast<Mob*>(piglin.GetVehicle());
            if (vehicle && vehicle->IsBaby()
                && (IsPiglinLike(*vehicle)
                    || vehicle->GetType() == EntityTypeId::Hoglin)) {
                piglin.StopRiding();
            }
        }

        if (!brain->HasMemoryValue(MemoryModule::CelebrateLocation)) {
            brain->EraseMemory(MemoryModule::Dancing);
        }
        piglin.SetDancing(brain->HasMemoryValue(MemoryModule::Dancing));
    }

} // namespace Game

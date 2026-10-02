// File: src/common/entity/ai/brain/PiglinAi.cpp
#include "common/entity/ai/brain/PiglinAi.hpp"
#include "common/entity/ai/goals/SpearGoals.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/brain/CommonBehaviors.hpp"
#include "common/entity/ai/brain/CoreBehaviors.hpp"
#include "common/entity/ai/brain/ManhattanBlockSearch.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/MobCrossbow.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/entity/ai/RandomPos.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/core/Mth.hpp"
#include "common/world/level/WorldDrops.hpp"
#include "common/world/loot/ChestLootTables.hpp"
#include "common/world/tags/DataTags.hpp"

#include <algorithm>
#include <cmath>

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

        // MC's constants for the item half.
        constexpr int kAdmireDuration          = 119;   // MC ADMIRE_DURATION
        constexpr int kMaxDistanceToWalkToItem = 9;     // MC MAX_DISTANCE_TO_WALK_TO_ITEM
        constexpr int kMaxTimeToWalkToItem     = 200;   // MC MAX_TIME_TO_WALK_TO_ITEM
        constexpr int kDisableAdmireWalking    = 200;   // HOW_LONG_TIME_TO_DISABLE_ADMIRE_WALKING_IF_CANT_REACH_ITEM
        constexpr int kEatCooldown             = 200;   // MC EAT_COOLDOWN
        constexpr int kHitByPlayerMemoryTimeout = 400;  // MC HIT_BY_PLAYER_MEMORY_TIMEOUT
        constexpr int kMinDistFromTargetWithCrossbow = 5;
        constexpr float kSpeedWhenStrafingBack = 0.75f;

        // MC ItemStack.is(TagKey<Item>) over the data pack's item tags.
        bool HasItemTag(const ItemStack& stack, const char* tag) {
            return !stack.IsEmpty() &&
                   DataTags::HasTag(DataTags::Registry::Item, ItemRegistry::Slug(stack.itemId), tag);
        }

        bool IsAdmiringItem(const Brain& brain) { return brain.HasMemoryValue(MemoryModule::AdmiringItem); }
        bool IsAdmiringDisabled(const Brain& brain) { return brain.HasMemoryValue(MemoryModule::AdmiringDisabled); }
        bool HasEatenRecently(const Brain& brain) { return brain.HasMemoryValue(MemoryModule::AteRecently); }

        // MC PiglinAi.isNotHoldingLovedItemInOffHand.
        bool IsNotHoldingLovedItemInOffHand(const Piglin& piglin) {
            const ItemStack& off = piglin.GetOffhandEquipment();
            return off.IsEmpty() || !PiglinAi::IsLovedItem(off);
        }

        // MC PiglinAi.hasCrossbow — isHolding(CROSSBOW).
        bool HasCrossbow(const LivingEntity& body) {
            const auto* mob = dynamic_cast<const Mob*>(&body);
            return mob && mob->IsHoldingItem(Items::Crossbow);
        }

        // MC PiglinAi.stopWalking.
        void StopWalking(Piglin& piglin) {
            if (Brain* brain = piglin.GetBrain()) brain->EraseMemory(MemoryModule::WalkTarget);
            piglin.GetNavigation().Stop();
        }

        // MC PiglinAi.admireGoldItem.
        void AdmireGoldItem(Piglin& piglin) {
            if (Brain* brain = piglin.GetBrain()) {
                brain->SetMemoryWithExpiry(MemoryModule::AdmiringItem, true, kAdmireDuration);
            }
        }

        // MC Entity.spawnAtLocation(level, stack).
        void SpawnAtLocation(EntityLevel& level, const Mob& mob, const ItemStack& stack) {
            if (stack.IsEmpty()) return;
            DropItemStackAt(level.Dimension(), mob.position, stack);
        }

        // MC BehaviorUtils.throwItem(thrower, item, targetPos): from 0.3
        // below the eye, 0.3 blocks/tick toward the target, the default
        // pickup delay.
        void ThrowItem(EntityLevel& level, LivingEntity& thrower, const ItemStack& item,
                       const glm::dvec3& targetPos) {
            if (item.IsEmpty()) return;
            const glm::dvec3 from(thrower.position.x, thrower.GetEyeY() - 0.30000001192092896,
                                  thrower.position.z);
            glm::dvec3 dir = targetPos - thrower.position;
            const double len = glm::length(dir);
            dir = len < 1.0e-5 ? glm::dvec3(0.0) : dir / len;
            level.SpawnThrownItem(from, dir * 0.30000001192092896, item, 10);
        }

        // MC PiglinAi.throwItemsTowardPos: a swing of the off hand, then each
        // stack at the target raised a block.
        void ThrowItemsTowardPos(EntityLevel& level, Piglin& piglin, const std::vector<ItemStack>& stacks,
                                 const glm::dvec3& targetPos) {
            if (stacks.empty()) return;
            piglin.Swing();   // swing(OFF_HAND)
            for (const ItemStack& stack : stacks) {
                ThrowItem(level, piglin, stack, targetPos + glm::dvec3(0.0, 1.0, 0.0));
            }
        }

        // MC PiglinAi.getRandomNearbyPos: LandRandomPos.getPos(body, 4, 2),
        // else where it stands.
        void ThrowItemsTowardRandomPos(EntityLevel& level, Piglin& piglin, const std::vector<ItemStack>& stacks) {
            const std::optional<glm::dvec3> pos = RandomPos::GetLandPos(piglin, 4, 2);
            ThrowItemsTowardPos(level, piglin, stacks, pos ? *pos : piglin.position);
        }

        // MC PiglinAi.throwItems: toward NEAREST_VISIBLE_PLAYER, else a
        // random nearby spot.
        void ThrowItems(EntityLevel& level, Piglin& piglin, const std::vector<ItemStack>& stacks) {
            const Brain* brain = piglin.GetBrain();
            Entity* player = brain ? brain->GetEntity(MemoryModule::NearestVisiblePlayer) : nullptr;
            if (player) {
                ThrowItemsTowardPos(level, piglin, stacks, player->position);
            } else {
                ThrowItemsTowardRandomPos(level, piglin, stacks);
            }
        }

        // MC PiglinAi.putInInventory: what does not fit is thrown aside (the
        // list always holds the remainder, empty or not — the swing plays).
        void PutInInventory(EntityLevel& level, Piglin& piglin, const ItemStack& stack) {
            const ItemStack couldNotFit = piglin.AddToInventory(stack);
            ThrowItemsTowardRandomPos(level, piglin, { couldNotFit });
        }

        // MC PiglinAi.holdInOffhand: whatever the off hand held drops first.
        void HoldInOffhand(EntityLevel& level, Piglin& piglin, const ItemStack& stack) {
            if (!piglin.GetOffhandEquipment().IsEmpty()) {
                SpawnAtLocation(level, piglin, piglin.GetOffhandEquipment());
            }
            piglin.HoldInOffHand(stack);
        }

        // MC PiglinAi.getBarterResponseItems: the gameplay/piglin_bartering
        // table (PIGLIN_BARTER params, the level's random).
        std::vector<ItemStack> BarterResponseItems(EntityLevel& level, Piglin& piglin) {
            std::vector<ItemStack> items;
            ChestLoot::LootLevelContext context;
            context.dimensionId = DimensionToRaw(level.Dimension());
            context.origin = piglin.position;
            ChestLoot::GetRandomItems("minecraft:gameplay/piglin_bartering", level.Random(), 0.0f, items, &context);
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

        // MC PiglinAi.findNearestValidAttackTarget.
        LivingEntity* FindTarget(Mob& mob) {
            Brain* brain = mob.GetBrain();
            if (!brain) return nullptr;
            if (IsNearZombified(mob)) return nullptr;

            if (auto* angry =
                    dynamic_cast<LivingEntity*>(brain->GetEntity(MemoryModule::AngryAt))) {
                // MC Sensor.isEntityAttackableIgnoringLineOfSight — the
                // grudge holds only within follow range; past it the piglin
                // falls back to the other candidates (or none).
                if (SensorTargeting::IsEntityAttackableIgnoringLineOfSight(mob, *angry)) return angry;
            }
            // UNIVERSAL_ANGER (the universal_anger game rule): any visible
            // attackable player will do.
            if (brain->HasMemoryValue(MemoryModule::UniversalAnger)) {
                if (auto* player = dynamic_cast<LivingEntity*>(
                        brain->GetEntity(MemoryModule::NearestVisibleAttackablePlayer))) {
                    return player;
                }
            }
            if (auto* nemesis = dynamic_cast<LivingEntity*>(
                    brain->GetEntity(MemoryModule::NearestVisibleNemesis))) {
                return nemesis;
            }
            // MC NEAREST_TARGETABLE_PLAYER_NOT_WEARING_GOLD — the sensor
            // leaves out a player in the gold set — and only while
            // Sensor.isEntityAttackable holds (sight and follow range).
            auto* player = dynamic_cast<LivingEntity*>(
                brain->GetEntity(MemoryModule::NearestTargetablePlayerNotWearingGold));
            return player && SensorTargeting::IsEntityAttackable(mob, *player) ? player : nullptr;
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

                // MC: setAngerTarget, dontKillAnyMoreHoglinsForAWhile,
                // broadcastAngerTarget (the nearby adult pack), then the
                // hunt cooldown on every VISIBLE adult packmate.
                PiglinAi::SetAngerTarget(*piglin, *hoglin);
                SampleAndSetHuntedRecently(*piglin, level.Random());
                PiglinAi::BroadcastAngerTarget(level, *piglin, *hoglin);
                for (Entity* e : AdultPiglinList(*brain, MemoryModule::NearestVisibleAdultPiglins)) {
                    if (auto* mate = dynamic_cast<Mob*>(e)) SampleAndSetHuntedRecently(*mate, level.Random());
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

        // MC StartCelebratingIfTargetDead — party over the corpse; the
        // target and the grudge are dropped unless it was a player and
        // forgive_dead_players is off.
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
                if (!target->IsPlayer() || Rules::GetBool(Rules::Id::ForgiveDeadPlayers)) {
                    brain->EraseMemory(MemoryModule::AttackTarget);
                    brain->EraseMemory(MemoryModule::AngryAt);
                }
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

        // ── The item half (MC piglin/*Admir*, GoToWantedItem, NearestItemSensor,
        //    BackUpIfTooClose, CrossbowAttack, SetLookAndInteract) ────────────

        // BehaviorUtils.canSee: the target is in the visible set.
        bool CanSee(const Brain& brain, LivingEntity* target) {
            const NearestVisibleLivingEntities* visible =
                brain.GetVisibleEntities(MemoryModule::NearestVisibleLivingEntities);
            return target && visible && visible->Contains(target) && visible->IsVisible(target);
        }

        // MC LivingEntity.hasLineOfSight(item entity): eye to the item's eye
        // (0.2125 above it), within 128, no collider in between (the
        // quarter-block walk the allay's and villager's item sensors take).
        bool ClearSightToItem(EntityLevel& level, const glm::dvec3& from, const glm::dvec3& to) {
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks) return false;
            const glm::dvec3 delta = to - from;
            const double distance = glm::length(delta);
            if (distance > 128.0) return false;
            if (distance < 1.0e-4) return true;
            const int steps = static_cast<int>(std::ceil(distance * 4.0));
            const glm::dvec3 step = delta / static_cast<double>(steps);
            glm::dvec3 p = from;
            for (int i = 1; i < steps; ++i) {
                p += step;
                const glm::ivec3 b(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)),
                                   static_cast<int>(std::floor(p.z)));
                if (BlockRegistry::HasCollision(blocks->GetBlock(b.x, b.y, b.z))) return false;
            }
            return true;
        }

        // The wanted item entity, if it still exists: its position.
        std::optional<glm::dvec3> WantedItemPos(EntityLevel& level, const Piglin& piglin, double searchRadius) {
            const std::optional<int32_t> id = piglin.GetWantedItemId();
            if (!id) return std::nullopt;
            AABBd box = piglin.GetAABBd();
            box.min -= glm::dvec3(searchRadius + 1.0);
            box.max += glm::dvec3(searchRadius + 1.0);
            std::vector<EntityLevel::NearbyItemEntity> items;
            level.GetItemEntitiesInBox(box, items);
            for (const auto& item : items) {
                if (item.id == *id) return item.pos;
            }
            return std::nullopt;
        }

        // MC NearestItemSensor (scan rate 20): the nearest item entity within
        // (32, 16, 32) the piglin wants, closer than 32 and in sight. The
        // answer lives on the piglin (item entities are not Entities).
        class PiglinNearestItemSensor : public Sensor {
        public:
            std::vector<MemoryModule> Requires() const override {
                return { MemoryModule::NearestVisibleWantedItem };
            }
        protected:
            void DoTick(EntityLevel& level, LivingEntity& body) override {
                auto* piglin = dynamic_cast<Piglin*>(&body);
                if (!piglin) return;
                AABBd box = body.GetAABBd();
                box.min -= glm::dvec3(32.0, 16.0, 32.0);
                box.max += glm::dvec3(32.0, 16.0, 32.0);
                std::vector<EntityLevel::NearbyItemEntity> items;
                level.GetItemEntitiesInBox(box, items);
                std::sort(items.begin(), items.end(), [&](const auto& a, const auto& b) {
                    return glm::dot(a.pos - body.position, a.pos - body.position) <
                           glm::dot(b.pos - body.position, b.pos - body.position);
                });
                std::optional<int32_t> found;
                for (const auto& item : items) {
                    const ItemStack* stack = level.GetItemEntityStack(item.id);
                    if (!stack || !piglin->WantsToPickUp(*stack)) continue;
                    const glm::dvec3 d = item.pos - body.position;
                    if (glm::dot(d, d) >= 32.0 * 32.0) continue;
                    if (!ClearSightToItem(level, body.GetEyePosition(), item.pos + glm::dvec3(0.0, 0.2125, 0.0))) {
                        continue;
                    }
                    found = item.id;
                    break;
                }
                piglin->SetWantedItemId(found);
            }
        };

        // MC piglin/StopHoldingItemIfNoLongerAdmiring (CORE): the admire
        // over, whatever is in the off hand (no shield) is bartered or kept.
        class StopHoldingItemIfNoLongerAdmiring : public Behavior {
        public:
            StopHoldingItemIfNoLongerAdmiring()
                : Behavior({ MemoryCondition{ MemoryModule::AdmiringItem, MemoryStatus::ValueAbsent } }, 1) {}
            const char* DebugString() const override { return "StopHoldingItemIfNoLongerAdmiring"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                auto* piglin = dynamic_cast<Piglin*>(&body);
                if (!piglin) return false;
                const ItemStack& off = piglin->GetOffhandEquipment();
                if (off.IsEmpty() || off.get(DataComponents::BLOCKS_ATTACKS)) return false;
                PiglinAi::StopHoldingOffHandItem(level, *piglin, /*barteringEnabled=*/true);
                return true;
            }
        };

        // MC piglin/StartAdmiringItemIfSeen(119) (CORE): a loved item in
        // sight starts the ADMIRE_ITEM activity.
        class StartAdmiringItemIfSeen : public Behavior {
        public:
            explicit StartAdmiringItemIfSeen(int admireDuration)
                : Behavior({ MemoryCondition{ MemoryModule::AdmiringItem, MemoryStatus::ValueAbsent },
                             MemoryCondition{ MemoryModule::AdmiringDisabled, MemoryStatus::ValueAbsent },
                             MemoryCondition{ MemoryModule::DisableWalkToAdmireItem, MemoryStatus::ValueAbsent } },
                           1),
                  m_admireDuration(admireDuration) {}
            const char* DebugString() const override { return "StartAdmiringItemIfSeen"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                auto* piglin = dynamic_cast<Piglin*>(&body);
                if (!piglin || !piglin->GetWantedItemId()) return false;   // i.present(WANTED_ITEM)
                const ItemStack* stack = level.GetItemEntityStack(*piglin->GetWantedItemId());
                if (!stack || !PiglinAi::IsLovedItem(*stack)) return false;
                body.GetBrain()->SetMemoryWithExpiry(MemoryModule::AdmiringItem, true, m_admireDuration);
                return true;
            }
        private:
            int m_admireDuration;
        };

        // MC GoToWantedItem.create(isNotHoldingLovedItemInOffHand, 1.0, true,
        // 9) — the ADMIRE_ITEM walk (interrupting any other walk).
        class PiglinGoToWantedItem : public Behavior {
        public:
            PiglinGoToWantedItem(float speed, int maxDistToWalk)
                : Behavior({ MemoryCondition{ MemoryModule::LookTarget, MemoryStatus::Registered },
                             MemoryCondition{ MemoryModule::WalkTarget, MemoryStatus::Registered },
                             MemoryCondition{ MemoryModule::ItemPickupCooldownTicks, MemoryStatus::Registered } },
                           1),
                  m_speed(speed), m_maxDist(maxDistToWalk) {}
            const char* DebugString() const override { return "GoToWantedItem"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                auto* piglin = dynamic_cast<Piglin*>(&body);
                Brain* brain = body.GetBrain();
                if (!piglin || !brain || brain->HasMemoryValue(MemoryModule::ItemPickupCooldownTicks)) return false;
                if (!IsNotHoldingLovedItemInOffHand(*piglin)) return false;
                const std::optional<glm::dvec3> pos = WantedItemPos(level, *piglin, m_maxDist);
                if (!pos) return false;
                const glm::dvec3 d = *pos - body.position;
                if (glm::dot(d, d) >= static_cast<double>(m_maxDist) * m_maxDist) return false;   // closerThan
                const glm::ivec3 cell(static_cast<int>(std::floor(pos->x)), static_cast<int>(std::floor(pos->y)),
                                      static_cast<int>(std::floor(pos->z)));
                brain->SetMemory(MemoryModule::LookTarget, PositionTracker::OfBlock(cell));
                brain->SetMemory(MemoryModule::WalkTarget, WalkTarget(cell, m_speed, 0));
                return true;
            }
        private:
            float m_speed;
            int   m_maxDist;
        };

        // MC piglin/StopAdmiringIfItemTooFarAway(9).
        class StopAdmiringIfItemTooFarAway : public Behavior {
        public:
            explicit StopAdmiringIfItemTooFarAway(int maxDistanceToItem)
                : Behavior({ MemoryCondition{ MemoryModule::AdmiringItem, MemoryStatus::ValuePresent } }, 1),
                  m_maxDist(maxDistanceToItem) {}
            const char* DebugString() const override { return "StopAdmiringIfItemTooFarAway"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                auto* piglin = dynamic_cast<Piglin*>(&body);
                if (!piglin || !piglin->GetOffhandEquipment().IsEmpty()) return false;
                const std::optional<glm::dvec3> pos = WantedItemPos(level, *piglin, m_maxDist);
                if (pos) {
                    const glm::dvec3 d = *pos - body.position;
                    if (glm::dot(d, d) < static_cast<double>(m_maxDist) * m_maxDist) return false;
                }
                body.GetBrain()->EraseMemory(MemoryModule::AdmiringItem);
                return true;
            }
        private:
            int m_maxDist;
        };

        // MC piglin/StopAdmiringIfTiredOfTryingToReachItem(200, 200): the
        // walk counts up; past the limit the admire ends and walking to
        // admire is disabled for a while.
        class StopAdmiringIfTiredOfTryingToReachItem : public Behavior {
        public:
            StopAdmiringIfTiredOfTryingToReachItem(int maxTimeToReachItem, int disableTime)
                : Behavior({ MemoryCondition{ MemoryModule::AdmiringItem, MemoryStatus::ValuePresent },
                             MemoryCondition{ MemoryModule::TimeTryingToReachAdmireItem, MemoryStatus::Registered },
                             MemoryCondition{ MemoryModule::DisableWalkToAdmireItem, MemoryStatus::Registered } },
                           1),
                  m_maxTime(maxTimeToReachItem), m_disableTime(disableTime) {}
            const char* DebugString() const override { return "StopAdmiringIfTiredOfTryingToReachItem"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                auto* piglin = dynamic_cast<Piglin*>(&body);
                // i.present(NEAREST_VISIBLE_WANTED_ITEM).
                if (!piglin || !piglin->GetWantedItemId() || !piglin->GetOffhandEquipment().IsEmpty()) return false;
                Brain* brain = body.GetBrain();
                const std::optional<int> time = brain->GetInt(MemoryModule::TimeTryingToReachAdmireItem);
                if (!time) {
                    brain->SetMemory(MemoryModule::TimeTryingToReachAdmireItem, 0);
                } else if (*time > m_maxTime) {
                    brain->EraseMemory(MemoryModule::AdmiringItem);
                    brain->EraseMemory(MemoryModule::TimeTryingToReachAdmireItem);
                    brain->SetMemoryWithExpiry(MemoryModule::DisableWalkToAdmireItem, true, m_disableTime);
                } else {
                    brain->SetMemory(MemoryModule::TimeTryingToReachAdmireItem, *time + 1);
                }
                return true;
            }
        private:
            int m_maxTime;
            int m_disableTime;
        };

        // MC BackUpIfTooClose.create(5, 0.75) under triggerIf(hasCrossbow):
        // strafe backwards from a target inside 5 blocks.
        class BackUpIfTooClose : public Behavior {
        public:
            BackUpIfTooClose(int tooCloseDistance, float strafeSpeed)
                : Behavior({ MemoryCondition{ MemoryModule::WalkTarget, MemoryStatus::ValueAbsent },
                             MemoryCondition{ MemoryModule::LookTarget, MemoryStatus::Registered },
                             MemoryCondition{ MemoryModule::AttackTarget, MemoryStatus::ValuePresent },
                             MemoryCondition{ MemoryModule::NearestVisibleLivingEntities,
                                              MemoryStatus::ValuePresent } },
                           1),
                  m_tooClose(tooCloseDistance), m_strafeSpeed(strafeSpeed) {}
            const char* DebugString() const override { return "BackUpIfTooClose"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                auto* mob = dynamic_cast<Mob*>(&body);
                Brain* brain = body.GetBrain();
                if (!mob || !brain || !HasCrossbow(body)) return false;
                auto* target = dynamic_cast<LivingEntity*>(brain->GetEntity(MemoryModule::AttackTarget));
                if (!target) return false;
                const double d2 = target->DistanceToSqr(body);
                if (!(d2 < static_cast<double>(m_tooClose) * m_tooClose) || !CanSee(*brain, target)) return false;
                brain->SetMemory(MemoryModule::LookTarget, PositionTracker::OfEntity(target, true));
                mob->GetMoveControl().Strafe(-m_strafeSpeed, 0.0f);
                mob->yRot = Mth::RotateIfNecessary(mob->yRot, mob->yHeadRot, 0.0f);
                return true;
            }
        private:
            int   m_tooClose;
            float m_strafeSpeed;
        };

        // ── The spear fight (MC SpearApproach / SpearAttack / SpearRetreat) ─
        //
        // SPEAR_STATUS walks APPROACH → CHARGING → RETREAT: close to the
        // approach distance, lower the spear and charge (the use's
        // KineticWeapon.damageEntities does the hitting), veer off past the
        // target, and when the charge window is spent back off 9-11 blocks.
        // A mounted piglin runs at its mount's chargeSpeedModifier.
        enum SpearStatus : int { kSpearApproach = 0, kSpearCharging = 1, kSpearRetreat = 2 };
        constexpr int kSpearNeverTimesOut = 1 << 29;   // timedOut() == false

        LivingEntity* SpearTarget(LivingEntity& body) {
            Brain* brain = body.GetBrain();
            return brain ? dynamic_cast<LivingEntity*>(brain->GetEntity(MemoryModule::AttackTarget)) : nullptr;
        }
        bool SpearAbleToAttack(LivingEntity& body) {
            auto* mob = dynamic_cast<Mob*>(&body);
            return mob && SpearTarget(body) && SpearAi::HoldsKineticWeapon(*mob);
        }

        class SpearApproach : public Behavior {
        public:
            SpearApproach(double speedModifierWhenRepositioning, float approachDistance)
                : Behavior({ MemoryCondition{ MemoryModule::SpearStatus, MemoryStatus::ValueAbsent } },
                           kSpearNeverTimesOut),
                  m_speed(speedModifierWhenRepositioning),
                  m_approachDistanceSq(approachDistance * approachDistance) {}
            const char* DebugString() const override { return "SpearApproach"; }
            void ClearReferenceTo(const Entity*) override {}
        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                return SpearAbleToAttack(body) && !body.IsUsingItem();
            }
            void Start(EntityLevel&, LivingEntity& body, int64_t) override {
                if (auto* mob = dynamic_cast<Mob*>(&body)) mob->SetAggressive(true);
                body.GetBrain()->SetMemory(MemoryModule::SpearStatus, static_cast<int>(kSpearApproach));
            }
            bool CanStillUse(EntityLevel&, LivingEntity& body, int64_t) override {
                LivingEntity* target = SpearTarget(body);
                return SpearAbleToAttack(body) && target &&
                       body.DistanceToSqr(target->position.x, target->position.y, target->position.z) >
                           static_cast<double>(m_approachDistanceSq);
            }
            void Tick(EntityLevel&, LivingEntity& body, int64_t) override {
                auto* mob = dynamic_cast<Mob*>(&body);
                LivingEntity* target = SpearTarget(body);
                if (!mob || !target) return;
                const float speedModifier = SpearAi::ChargeSpeedModifier(*mob);
                body.GetBrain()->SetMemory(MemoryModule::LookTarget, PositionTracker::OfEntity(target, true));
                mob->GetNavigation().MoveTo(*target, speedModifier * m_speed);
            }
            void Stop(EntityLevel&, LivingEntity& body, int64_t) override {
                if (auto* mob = dynamic_cast<Mob*>(&body)) mob->GetNavigation().Stop();
                body.GetBrain()->SetMemory(MemoryModule::SpearStatus, static_cast<int>(kSpearCharging));
            }
        private:
            double m_speed;
            float  m_approachDistanceSq;
        };

        class SpearAttack : public Behavior {
        public:
            SpearAttack(double speedModifierWhenCharging, double speedModifierWhenRepositioning,
                        float targetInRangeRadius)
                : Behavior({ MemoryCondition{ MemoryModule::SpearStatus, MemoryStatus::ValuePresent } },
                           kSpearNeverTimesOut),
                  m_chargeSpeed(speedModifierWhenCharging), m_repositionSpeed(speedModifierWhenRepositioning),
                  m_targetInRangeRadiusSq(targetInRangeRadius * targetInRangeRadius) {}
            const char* DebugString() const override { return "SpearAttack"; }
            void ClearReferenceTo(const Entity*) override {}
        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                const int status = body.GetBrain()->GetInt(MemoryModule::SpearStatus).value_or(kSpearApproach);
                return status == kSpearCharging && SpearAbleToAttack(body) && !body.IsUsingItem();
            }
            void Start(EntityLevel&, LivingEntity& body, int64_t) override {
                auto* mob = dynamic_cast<Mob*>(&body);
                if (!mob) return;
                mob->SetAggressive(true);
                Brain* brain = body.GetBrain();
                brain->SetMemory(MemoryModule::SpearEngageTime, SpearAi::KineticUseDuration(*mob));
                brain->EraseMemory(MemoryModule::SpearChargePosition);
                body.StartUsingItem(EquipmentSlot::MAINHAND);
            }
            bool CanStillUse(EntityLevel&, LivingEntity& body, int64_t) override {
                return body.GetBrain()->GetInt(MemoryModule::SpearEngageTime).value_or(0) > 0 &&
                       SpearAbleToAttack(body);
            }
            void Tick(EntityLevel&, LivingEntity& body, int64_t) override {
                auto* mob = dynamic_cast<PathfinderMob*>(&body);
                LivingEntity* target = SpearTarget(body);
                if (!mob || !target) return;
                Brain* brain = body.GetBrain();
                const double targetDistSqr =
                    mob->DistanceToSqr(target->position.x, target->position.y, target->position.z);
                const float speedModifier = SpearAi::ChargeSpeedModifier(*mob);
                const int mountDistance = mob->IsPassenger() ? 2 : 0;
                brain->SetMemory(MemoryModule::LookTarget, PositionTracker::OfEntity(target, true));
                brain->SetMemory(MemoryModule::SpearEngageTime,
                                 brain->GetInt(MemoryModule::SpearEngageTime).value_or(0) - 1);
                if (const std::optional<glm::dvec3> away = brain->GetVec3(MemoryModule::SpearChargePosition)) {
                    mob->GetNavigation().MoveTo(away->x, away->y, away->z, speedModifier * m_repositionSpeed);
                    if (mob->GetNavigation().IsDone()) brain->EraseMemory(MemoryModule::SpearChargePosition);
                } else {
                    mob->GetNavigation().MoveTo(*target, speedModifier * m_chargeSpeed);
                    if (targetDistSqr < static_cast<double>(m_targetInRangeRadiusSq) ||
                        mob->GetNavigation().IsDone()) {
                        const double distance = std::sqrt(targetDistSqr);
                        const std::optional<glm::dvec3> newAway = RandomPos::GetLandPosAway(
                            *mob, static_cast<double>(6 + mountDistance) - distance,
                            static_cast<double>(7 + mountDistance) - distance, 7, target->position);
                        if (newAway) brain->SetMemory(MemoryModule::SpearChargePosition, *newAway);
                        else         brain->EraseMemory(MemoryModule::SpearChargePosition);
                    }
                }
            }
            void Stop(EntityLevel&, LivingEntity& body, int64_t) override {
                if (auto* mob = dynamic_cast<Mob*>(&body)) mob->GetNavigation().Stop();
                body.StopUsingItem();
                Brain* brain = body.GetBrain();
                brain->EraseMemory(MemoryModule::SpearChargePosition);
                brain->EraseMemory(MemoryModule::SpearEngageTime);
                brain->SetMemory(MemoryModule::SpearStatus, static_cast<int>(kSpearRetreat));
            }
        private:
            double m_chargeSpeed;
            double m_repositionSpeed;
            float  m_targetInRangeRadiusSq;
        };

        class SpearRetreat : public Behavior {
        public:
            explicit SpearRetreat(double speedModifierWhenRepositioning)
                : Behavior({ MemoryCondition{ MemoryModule::SpearStatus, MemoryStatus::ValuePresent } }, 100),
                  m_speed(speedModifierWhenRepositioning) {}
            const char* DebugString() const override { return "SpearRetreat"; }
            void ClearReferenceTo(const Entity*) override {}
        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                if (!SpearAbleToAttack(body) || body.IsUsingItem()) return false;
                Brain* brain = body.GetBrain();
                if (brain->GetInt(MemoryModule::SpearStatus).value_or(kSpearApproach) != kSpearRetreat) return false;
                auto* mob = dynamic_cast<PathfinderMob*>(&body);
                LivingEntity* target = SpearTarget(body);
                if (!mob || !target) return false;
                const double distance = std::sqrt(
                    mob->DistanceToSqr(target->position.x, target->position.y, target->position.z));
                const int mountDistance = mob->IsPassenger() ? 2 : 0;
                const std::optional<glm::dvec3> away = RandomPos::GetLandPosAway(
                    *mob, std::max(0.0, static_cast<double>(9 + mountDistance) - distance),
                    std::max(1.0, static_cast<double>(11 + mountDistance) - distance), 7, target->position);
                if (!away) return false;
                brain->SetMemory(MemoryModule::SpearFleeingPosition, *away);
                return true;
            }
            void Start(EntityLevel&, LivingEntity& body, int64_t) override {
                if (auto* mob = dynamic_cast<Mob*>(&body)) mob->SetAggressive(true);
                body.GetBrain()->SetMemory(MemoryModule::SpearFleeingTime, 0);
            }
            bool CanStillUse(EntityLevel&, LivingEntity& body, int64_t) override {
                auto* mob = dynamic_cast<Mob*>(&body);
                Brain* brain = body.GetBrain();
                return mob && brain->GetInt(MemoryModule::SpearFleeingTime).value_or(100) < 100 &&
                       brain->HasMemoryValue(MemoryModule::SpearFleeingPosition) &&
                       !mob->GetNavigation().IsDone() && SpearAbleToAttack(body);
            }
            void Tick(EntityLevel&, LivingEntity& body, int64_t) override {
                auto* mob = dynamic_cast<Mob*>(&body);
                LivingEntity* target = SpearTarget(body);
                if (!mob || !target) return;
                Brain* brain = body.GetBrain();
                const float speedModifier = SpearAi::ChargeSpeedModifier(*mob);
                brain->SetMemory(MemoryModule::LookTarget, PositionTracker::OfEntity(target, true));
                brain->SetMemory(MemoryModule::SpearFleeingTime,
                                 brain->GetInt(MemoryModule::SpearFleeingTime).value_or(0) + 1);
                if (const std::optional<glm::dvec3> flee = brain->GetVec3(MemoryModule::SpearFleeingPosition)) {
                    mob->GetNavigation().MoveTo(flee->x, flee->y, flee->z, speedModifier * m_speed);
                }
            }
            void Stop(EntityLevel&, LivingEntity& body, int64_t) override {
                if (auto* mob = dynamic_cast<Mob*>(&body)) {
                    mob->GetNavigation().Stop();
                    mob->SetAggressive(false);
                }
                body.StopUsingItem();
                Brain* brain = body.GetBrain();
                brain->EraseMemory(MemoryModule::SpearFleeingTime);
                brain->EraseMemory(MemoryModule::SpearFleeingPosition);
                brain->EraseMemory(MemoryModule::SpearStatus);
            }
        private:
            double m_speed;
        };

        // MC CrossbowAttack: draw, hold 20..39 ticks, fire — the brain twin
        // of RangedCrossbowAttackGoal (no approach of its own; the FIGHT
        // activity's walk behaviour keeps the range).
        class PiglinCrossbowAttack : public Behavior {
        public:
            PiglinCrossbowAttack()
                : Behavior({ MemoryCondition{ MemoryModule::LookTarget, MemoryStatus::Registered },
                             MemoryCondition{ MemoryModule::AttackTarget, MemoryStatus::ValuePresent } },
                           1200) {}
            const char* DebugString() const override { return "CrossbowAttack"; }
            void ClearReferenceTo(const Entity*) override {}
        protected:
            static LivingEntity* Target(LivingEntity& body) {
                Brain* brain = body.GetBrain();
                return brain ? dynamic_cast<LivingEntity*>(brain->GetEntity(MemoryModule::AttackTarget)) : nullptr;
            }
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                auto* mob = dynamic_cast<Mob*>(&body);
                LivingEntity* target = Target(body);
                return mob && target && mob->IsHoldingItem(Items::Crossbow) && CanSee(*body.GetBrain(), target) &&
                       MobCrossbow::IsWithinAttackRange(*mob, *target, 0);
            }
            bool CanStillUse(EntityLevel& level, LivingEntity& body, int64_t) override {
                return body.GetBrain()->HasMemoryValue(MemoryModule::AttackTarget) &&
                       CheckExtraStartConditions(level, body);
            }
            void Tick(EntityLevel& level, LivingEntity& body, int64_t) override {
                auto* piglin = dynamic_cast<Piglin*>(&body);
                LivingEntity* target = Target(body);
                if (!piglin || !target) return;
                // LivingEntity.tick's use clock, ahead of the brain.
                MobCrossbow::TickUsingItem(*piglin, m_use);
                body.GetBrain()->SetMemory(MemoryModule::LookTarget, PositionTracker::OfEntity(target, true));
                switch (m_state) {
                    case State::Uncharged:
                        MobCrossbow::StartUsingItem(*piglin, m_use,
                                                    MobCrossbow::WeaponHoldingHand(*piglin, Items::Crossbow));
                        m_state = State::Charging;
                        piglin->SetChargingCrossbow(true);
                        break;
                    case State::Charging: {
                        if (!m_use.usingItem) m_state = State::Uncharged;
                        const int pullTime = m_use.TicksUsingItem();
                        if (pullTime >= MobCrossbow::ChargeDuration(piglin->GetEquipment(m_use.hand))) {
                            MobCrossbow::ReleaseUsingItem(*piglin, m_use);
                            m_state = State::Charged;
                            m_attackDelay = 20 + level.Random().NextInt(20);
                            piglin->SetChargingCrossbow(false);
                        }
                        break;
                    }
                    case State::Charged:
                        --m_attackDelay;
                        if (m_attackDelay == 0) m_state = State::ReadyToAttack;
                        break;
                    case State::ReadyToAttack:
                        piglin->PerformRangedAttack(*target, 1.0f);
                        m_state = State::Uncharged;
                        break;
                }
            }
            void Stop(EntityLevel&, LivingEntity& body, int64_t) override {
                if (m_use.usingItem) MobCrossbow::StopUsingItem(m_use);
                auto* piglin = dynamic_cast<Piglin*>(&body);
                if (piglin && piglin->IsHoldingItem(Items::Crossbow)) {
                    piglin->SetChargingCrossbow(false);
                    // MC then empties getUseItem()'s CHARGED_PROJECTILES —
                    // useItem is already EMPTY after stopUsingItem, so a
                    // loaded crossbow stays loaded.
                }
            }
        private:
            enum class State : uint8_t { Uncharged, Charging, Charged, ReadyToAttack };
            State m_state = State::Uncharged;
            int   m_attackDelay = 0;
            MobCrossbow::UseState m_use;
        };

        // MC SetLookAndInteract.create(PLAYER, 4): the nearest visible player
        // within reach becomes the INTERACTION_TARGET and the look target.
        class SetLookAndInteractPlayer : public Behavior {
        public:
            explicit SetLookAndInteractPlayer(int interactionRange)
                : Behavior({ MemoryCondition{ MemoryModule::LookTarget, MemoryStatus::Registered },
                             MemoryCondition{ MemoryModule::InteractionTarget, MemoryStatus::ValueAbsent },
                             MemoryCondition{ MemoryModule::NearestVisibleLivingEntities,
                                              MemoryStatus::ValuePresent } },
                           1),
                  m_rangeSqr(interactionRange * interactionRange) {}
            const char* DebugString() const override { return "SetLookAndInteract"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                Brain* brain = body.GetBrain();
                const NearestVisibleLivingEntities* visible =
                    brain->GetVisibleEntities(MemoryModule::NearestVisibleLivingEntities);
                if (!visible) return false;
                LivingEntity* found = visible->FindClosest([&](LivingEntity* e) {
                    return e->IsPlayer() && e->DistanceToSqr(body) <= static_cast<double>(m_rangeSqr);
                });
                if (!found) return false;
                brain->SetMemory(MemoryModule::InteractionTarget, static_cast<Entity*>(found));
                brain->SetMemory(MemoryModule::LookTarget, PositionTracker::OfEntity(found, true));
                return true;
            }
        private:
            int m_rangeSqr;
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

                // MC findNearestRepellent — the first BlockTags.PIGLIN_REPELLENTS
                // cell (soul fire, soul torch and wall torch, soul lantern, a
                // LIT soul campfire) of the 8/4/8 manhattan-ordered walk.
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
                                // Hoglin.canBeHunted: an adult without
                                // CannotBeHunted.
                                const auto* hoglin = dynamic_cast<const Hoglin*>(e);
                                if (!huntableHoglin && hoglin && hoglin->CanBeHunted()) huntableHoglin = e;
                            }
                        } else if (t == EntityTypeId::PiglinBrute) {
                            visibleAdultPiglins.push_back(e);
                        } else if (t == EntityTypeId::Piglin) {
                            if (!e->IsBaby()) visibleAdultPiglins.push_back(e);
                        } else if (e->IsPlayer()) {
                            // MC: a player in any piece of piglin_safe_armor
                            // (the gold set) is left alone.
                            if (!playerNotWearingGold && !PiglinAi::IsWearingSafeArmor(*e)
                                && mob && mob->CanAttack(*e)) {
                                playerNotWearingGold = e;
                            }
                            if (!playerHoldingWantedItem && !e->IsSpectator()
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

            // MC PiglinSpecificSensor.isValidRepellent: the tag, and a soul
            // campfire only while lit.
            static bool IsValidRepellent(const BlockState& state) {
                switch (state.Block()) {
                    case BlockID::SoulTorch:
                    case BlockID::SoulWallTorch:
                    case BlockID::SoulLantern:
                    case BlockID::SoulFire:
                        return true;
                    case BlockID::SoulCampfire:
                        return state.HasProperty(PropertyId::LIT) && state.GetName(PropertyId::LIT) == "true";
                    default:
                        return false;
                }
            }

            static std::optional<glm::ivec3> FindNearestRepellent(EntityLevel& level,
                                                                  LivingEntity& body) {
                const IBlockAccess* blocks = level.Blocks();
                if (!blocks) return std::nullopt;
                return FindFirstInBoxByManhattanDistance(
                    body.BlockPosition(), 8, 4, 8, [blocks](const glm::ivec3& p) {
                        return IsValidRepellent(blocks->GetBlockState(p.x, p.y, p.z));
                    });
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
        // MC: entity.is(PLAYER) && entity.isHolding(PiglinAi::isLovedItem) —
        // either hand.
        (void)level;
        if (!entity.IsPlayer()) return false;
        for (const EquipmentSlot hand : { EquipmentSlot::MAINHAND, EquipmentSlot::OFFHAND }) {
            if (const ItemStack* held = entity.EquipmentInSlot(hand); held && IsLovedItem(*held)) return true;
        }
        return false;
    }

    bool PiglinAi::IsLovedItem(const ItemStack& stack) {
        return HasItemTag(stack, "minecraft:piglin_loved");
    }

    bool PiglinAi::IsBarterCurrency(const ItemStack& stack) {
        return !stack.IsEmpty() && stack.itemId == Items::GoldIngot;   // MC BARTERING_ITEM
    }

    bool PiglinAi::IsFood(const ItemStack& stack) {
        return HasItemTag(stack, "minecraft:piglin_food");
    }

    bool PiglinAi::IsWearingSafeArmor(LivingEntity& entity) {
        // EquipmentSlotGroup.ARMOR: head, chest, legs, feet (and body).
        for (const EquipmentSlot slot : { EquipmentSlot::FEET, EquipmentSlot::LEGS, EquipmentSlot::CHEST,
                                          EquipmentSlot::HEAD }) {
            if (const ItemStack* worn = entity.EquipmentInSlot(slot);
                worn && HasItemTag(*worn, "minecraft:piglin_safe_armor")) {
                return true;
            }
        }
        return false;
    }

    bool PiglinAi::WantsToPickup(const Piglin& piglin, const ItemStack& stack) {
        const Brain* brain = piglin.GetBrain();
        if (!brain) return false;
        if (piglin.IsBaby() && HasItemTag(stack, "minecraft:ignored_by_piglin_babies")) return false;
        if (HasItemTag(stack, "minecraft:piglin_repellents")) return false;
        if (IsAdmiringDisabled(*brain) && brain->HasMemoryValue(MemoryModule::AttackTarget)) return false;
        if (IsBarterCurrency(stack)) return IsNotHoldingLovedItemInOffHand(piglin);
        const bool hasSpace = piglin.CanAddToInventory(stack);
        if (stack.itemId == Items::GoldNugget) return hasSpace;
        if (IsFood(stack)) return !HasEatenRecently(*brain) && hasSpace;
        if (!IsLovedItem(stack)) return piglin.CanReplaceCurrentItem(stack);
        return IsNotHoldingLovedItemInOffHand(piglin) && hasSpace;
    }

    void PiglinAi::PickUpItem(EntityLevel& level, Piglin& piglin, int32_t itemEntityId, const ItemStack& stack) {
        StopWalking(piglin);
        ItemStack taken;
        if (stack.itemId == Items::GoldNugget) {
            // body.take(entity, count) + the whole stack; the entity goes.
            piglin.TakeItemEntity(itemEntityId, stack.count);
            taken = stack;
        } else {
            // body.take(entity, 1) + removeOneItemFromItemEntity.
            piglin.TakeItemEntity(itemEntityId, 1);
            taken = stack;
            taken.count = 1;
        }

        Brain* brain = piglin.GetBrain();
        if (IsLovedItem(taken)) {
            if (brain) brain->EraseMemory(MemoryModule::TimeTryingToReachAdmireItem);
            HoldInOffhand(level, piglin, taken);
            AdmireGoldItem(piglin);
        } else if (IsFood(taken) && brain && !HasEatenRecently(*brain)) {
            // PiglinAi.eat: the food is gone; ATE_RECENTLY for 200 ticks.
            brain->SetMemoryWithExpiry(MemoryModule::AteRecently, true, kEatCooldown);
        } else {
            const bool itemEquipped = !piglin.EquipItemIfPossible(taken).IsEmpty();
            if (!itemEquipped) PutInInventory(level, piglin, taken);
        }
    }

    void PiglinAi::StopHoldingOffHandItem(EntityLevel& level, Piglin& piglin, bool barteringEnabled) {
        const ItemStack itemStack = piglin.GetOffhandEquipment();
        piglin.SetEquipment(EquipmentSlot::OFFHAND, ItemStack{});
        if (piglin.IsAdult()) {
            const bool barterCurrency = IsBarterCurrency(itemStack);
            if (barteringEnabled && barterCurrency) {
                ThrowItems(level, piglin, BarterResponseItems(level, piglin));
            } else if (!barterCurrency) {
                const bool equipped = !piglin.EquipItemIfPossible(itemStack).IsEmpty();
                if (!equipped) PutInInventory(level, piglin, itemStack);
            }
            // A gold ingot with bartering disabled (the admire cut short by a
            // hit) is simply gone, as in MC.
        } else {
            const bool equipped = !piglin.EquipItemIfPossible(itemStack).IsEmpty();
            if (!equipped) {
                const ItemStack mainHandItem = piglin.GetMainHandEquipment();
                if (IsLovedItem(mainHandItem)) {
                    PutInInventory(level, piglin, mainHandItem);
                } else {
                    ThrowItems(level, piglin, { mainHandItem });
                }
                piglin.HoldInMainHand(itemStack);
            }
        }
    }

    void PiglinAi::CancelAdmiring(EntityLevel& level, Piglin& piglin) {
        const Brain* brain = piglin.GetBrain();
        if (brain && IsAdmiringItem(*brain) && !piglin.GetOffhandEquipment().IsEmpty()) {
            SpawnAtLocation(level, piglin, piglin.GetOffhandEquipment());
            piglin.SetEquipment(EquipmentSlot::OFFHAND, ItemStack{});
        }
    }

    bool PiglinAi::CanAdmire(const Piglin& piglin, const ItemStack& held) {
        const Brain* brain = piglin.GetBrain();
        if (!brain) return false;
        return !IsAdmiringDisabled(*brain) && !IsAdmiringItem(*brain) && piglin.IsAdult() &&
               IsBarterCurrency(held);
    }

    bool PiglinAi::MobInteract(EntityLevel& level, Piglin& piglin, ItemStack& held) {
        if (!CanAdmire(piglin, held)) return false;
        // playerHeldItemStack.consumeAndReturn(1, player) — the caller hands
        // a creative player's stack back.
        ItemStack taken = held;
        taken.count = 1;
        held.count -= 1;
        if (held.count <= 0) held = ItemStack{};
        HoldInOffhand(level, piglin, taken);
        AdmireGoldItem(piglin);
        StopWalking(piglin);
        return true;
    }

    namespace {

        bool UniversalAnger() { return Rules::GetBool(Rules::Id::UniversalAnger); }

        // MC BehaviorUtils.getNearestTarget(body, current, candidate): the
        // candidate unless the current one is strictly nearer.
        LivingEntity* NearestTarget(const LivingEntity& body, LivingEntity* current, LivingEntity& candidate) {
            if (!current) return &candidate;
            return body.DistanceToSqr(*current) < body.DistanceToSqr(candidate) ? current : &candidate;
        }

        LivingEntity* AngerTargetOf(const Mob& piglin) {
            const Brain* brain = piglin.GetBrain();
            return brain ? dynamic_cast<LivingEntity*>(brain->GetEntity(MemoryModule::AngryAt)) : nullptr;
        }

        // MC PiglinAi.setAngerTargetIfCloserThanCurrent.
        void SetAngerTargetIfCloserThanCurrent(Mob& piglin, LivingEntity& newTarget) {
            LivingEntity* current = AngerTargetOf(piglin);
            LivingEntity* nearest = NearestTarget(piglin, current, newTarget);
            if (!current || current != nearest) PiglinAi::SetAngerTarget(piglin, *nearest);
        }

        // MC PiglinAi.getNearestVisibleTargetablePlayer.
        LivingEntity* NearestVisibleTargetablePlayer(const Mob& piglin) {
            const Brain* brain = piglin.GetBrain();
            return brain ? dynamic_cast<LivingEntity*>(brain->GetEntity(MemoryModule::NearestVisibleAttackablePlayer))
                         : nullptr;
        }

        // MC PiglinAi.setAngerTargetToNearestTargetablePlayerIfFound.
        void SetAngerTargetToNearestTargetablePlayerIfFound(Mob& piglin, LivingEntity& targetIfNoPlayerFound) {
            if (LivingEntity* player = NearestVisibleTargetablePlayer(piglin)) {
                PiglinAi::SetAngerTarget(piglin, *player);
            } else {
                PiglinAi::SetAngerTarget(piglin, targetIfNoPlayerFound);
            }
        }

        // MC PiglinAi.broadcastUniversalAnger: every nearby adult piglin
        // turns on the player it can see and attack.
        void BroadcastUniversalAnger(Mob& piglin) {
            const Brain* brain = piglin.GetBrain();
            if (!brain) return;
            for (Entity* e : AdultPiglinList(*brain, MemoryModule::NearbyAdultPiglins)) {
                auto* mate = dynamic_cast<Mob*>(e);
                if (!mate) continue;
                if (LivingEntity* player = NearestVisibleTargetablePlayer(*mate)) {
                    PiglinAi::SetAngerTarget(*mate, *player);
                }
            }
        }

        // MC PiglinAi.setAvoidTargetAndDontHuntForAWhile.
        void SetAvoidTargetAndDontHuntForAWhile(Piglin& piglin, LivingEntity& target) {
            Brain* brain = piglin.GetBrain();
            EntityLevel* level = piglin.Level();
            if (!brain || !level) return;
            brain->EraseMemory(MemoryModule::AngryAt);
            brain->EraseMemory(MemoryModule::AttackTarget);
            brain->EraseMemory(MemoryModule::WalkTarget);
            brain->SetMemoryWithExpiry(MemoryModule::AvoidTarget, static_cast<Entity*>(&target),
                                       level->Random().NextInt(kRetreatMin, kRetreatMax));
            SampleAndSetHuntedRecently(piglin, level->Random());
        }

        // MC PiglinAi.retreatFromNearestTarget: flee the nearest of the new
        // threat, the current avoid target and the current attack target.
        void RetreatFromNearestTarget(Piglin& piglin, LivingEntity& newAvoidTarget) {
            const Brain* brain = piglin.GetBrain();
            if (!brain) return;
            LivingEntity* nearest = &newAvoidTarget;
            nearest = NearestTarget(piglin, dynamic_cast<LivingEntity*>(brain->GetEntity(MemoryModule::AvoidTarget)),
                                    *nearest);
            nearest = NearestTarget(piglin, dynamic_cast<LivingEntity*>(brain->GetEntity(MemoryModule::AttackTarget)),
                                    *nearest);
            SetAvoidTargetAndDontHuntForAWhile(piglin, *nearest);
        }

    } // namespace

    BehaviorPtr PiglinAi::MakeSetLookAndInteractPlayer(int interactionRange) {
        return std::make_unique<SetLookAndInteractPlayer>(interactionRange);
    }

    void PiglinAi::BroadcastAngerTarget(EntityLevel& level, Mob& piglin, LivingEntity& target) {
        (void)level;
        const Brain* brain = piglin.GetBrain();
        if (!brain) return;
        const auto* hoglin = target.GetType() == EntityTypeId::Hoglin ? dynamic_cast<const Hoglin*>(&target)
                                                                      : nullptr;
        for (Entity* e : AdultPiglinList(*brain, MemoryModule::NearbyAdultPiglins)) {
            auto* mate = dynamic_cast<Mob*>(e);
            if (!mate) continue;
            if (hoglin && (!PiglinCanHunt(*mate) || !hoglin->CanBeHunted())) continue;
            SetAngerTargetIfCloserThanCurrent(*mate, target);
        }
    }

    void PiglinAi::AngerNearbyPiglins(EntityLevel& level, LivingEntity& player, bool onlyIfTheySeeThePlayer) {
        // MC: the piglins in the player's box grown by 16 that are IDLE (and
        // see the player, when asked) take the player — or, under
        // UNIVERSAL_ANGER, the nearest player each can target.
        AABB box = player.GetAABB();
        box.min -= glm::vec3(16.0f);
        box.max += glm::vec3(16.0f);
        std::vector<Entity*> nearby;
        level.GetEntitiesInBox(box, &player, nearby);
        const bool universal = UniversalAnger();
        for (Entity* e : nearby) {
            auto* piglin = dynamic_cast<Piglin*>(e);
            if (!piglin || !piglin->IsAlive()) continue;
            Brain* brain = piglin->GetBrain();
            if (!brain || !brain->IsActive(Activity::Idle)) continue;   // PiglinAi.isIdle
            if (onlyIfTheySeeThePlayer && !CanSee(*brain, &player)) continue;   // BehaviorUtils.canSee
            if (universal) SetAngerTargetToNearestTargetablePlayerIfFound(*piglin, player);
            else           SetAngerTarget(*piglin, player);
        }
    }

    void PiglinAi::SetAngerTarget(Mob& piglin, LivingEntity& target) {
        // MC PiglinAi.setAngerTarget, gated on
        // Sensor.isEntityAttackableIgnoringLineOfSight.
        Brain* brain = piglin.GetBrain();
        if (!brain || !SensorTargeting::IsEntityAttackableIgnoringLineOfSight(piglin, target)) return;
        brain->EraseMemory(MemoryModule::CantReachWalkTargetSince);
        brain->SetMemoryWithExpiry(MemoryModule::AngryAt,
                                   static_cast<Entity*>(&target), kAngerDuration);
        if (target.GetType() == EntityTypeId::Hoglin && PiglinCanHunt(piglin)) {
            if (EntityLevel* level = piglin.Level()) {
                SampleAndSetHuntedRecently(piglin, level->Random());
            }
        }
        if (target.IsPlayer() && UniversalAnger()) {
            brain->SetMemoryWithExpiry(MemoryModule::UniversalAnger, true, kAngerDuration);
        }
    }

    void PiglinAi::MaybeRetaliate(EntityLevel& level, Mob& piglin,
                                  LivingEntity& attacker) {
        Brain* brain = piglin.GetBrain();
        if (!brain) return;
        // MC: never retaliate out of an active retreat.
        if (brain->IsActive(Activity::Avoid)) return;
        if (!SensorTargeting::IsEntityAttackableIgnoringLineOfSight(piglin, attacker)) return;

        // MC isOtherTargetMuchFurtherAwayThanCurrentAttackTarget(4.0).
        if (auto* current = dynamic_cast<LivingEntity*>(
                brain->GetEntity(MemoryModule::AttackTarget))) {
            if (piglin.DistanceToSqr(attacker)
                > piglin.DistanceToSqr(*current) + 4.0 * 4.0) {
                return;
            }
        }

        if (attacker.IsPlayer() && UniversalAnger()) {
            SetAngerTargetToNearestTargetablePlayerIfFound(piglin, attacker);
            BroadcastUniversalAnger(piglin);
        } else {
            SetAngerTarget(piglin, attacker);
            BroadcastAngerTarget(level, piglin, attacker);
        }
    }

    void PiglinAi::WasHurtBy(EntityLevel& level, Piglin& piglin,
                             LivingEntity& attacker) {
        // MC PiglinAi.wasHurtBy (`attacker instanceof Piglin` — the brute is
        // an AbstractPiglin, not a Piglin, so a brute's hit counts).
        if (attacker.GetType() == EntityTypeId::Piglin) return;
        Brain* brain = piglin.GetBrain();
        if (!brain) return;

        // A hit ends an admire: the off-hand item is kept or dropped, never
        // bartered (a gold ingot is lost).
        if (!piglin.GetOffhandEquipment().IsEmpty()) StopHoldingOffHandItem(level, piglin, false);

        brain->EraseMemory(MemoryModule::CelebrateLocation);
        brain->EraseMemory(MemoryModule::Dancing);
        brain->EraseMemory(MemoryModule::AdmiringItem);
        if (attacker.IsPlayer()) {
            brain->SetMemoryWithExpiry(MemoryModule::AdmiringDisabled, true, kHitByPlayerMemoryTimeout);
        }

        // MC: an avoid target of a DIFFERENT type than the attacker is
        // dropped, so the new threat can take the slot.
        if (auto* avoiding = dynamic_cast<LivingEntity*>(
                brain->GetEntity(MemoryModule::AvoidTarget))) {
            if (avoiding->GetType() != attacker.GetType()) {
                brain->EraseMemory(MemoryModule::AvoidTarget);
            }
        }

        if (piglin.IsBaby()) {
            // MC BABY_FLEE_DURATION_AFTER_GETTING_HIT = 100; the pack is
            // told, the baby itself runs.
            brain->SetMemoryWithExpiry(MemoryModule::AvoidTarget,
                                       static_cast<Entity*>(&attacker), 100);
            if (SensorTargeting::IsEntityAttackableIgnoringLineOfSight(piglin, attacker)) {
                BroadcastAngerTarget(level, piglin, attacker);
            }
        } else if (attacker.GetType() == EntityTypeId::Hoglin
                   && HoglinsOutnumberPiglins(piglin)) {
            // MC setAvoidTargetAndDontHuntForAWhile + broadcastRetreat (the
            // VISIBLE adult piglins — brutes never retreat).
            SetAvoidTargetAndDontHuntForAWhile(piglin, attacker);
            for (Entity* e : AdultPiglinList(*brain, MemoryModule::NearestVisibleAdultPiglins)) {
                if (auto* mate = dynamic_cast<Piglin*>(e)) RetreatFromNearestTarget(*mate, attacker);
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
        // MC Piglin's memories, minus the door machinery this port does not
        // run. NEAREST_VISIBLE_WANTED_ITEM is
        // registered for the sensor; its value (an item entity) lives on the
        // piglin (Piglin::GetWantedItemId).
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
                                MemoryModule::NearestRepellent,
                                MemoryModule::NearestVisibleWantedItem,
                                MemoryModule::ItemPickupCooldownTicks,
                                MemoryModule::AdmiringItem,
                                MemoryModule::TimeTryingToReachAdmireItem,
                                MemoryModule::DisableWalkToAdmireItem,
                                MemoryModule::AdmiringDisabled,
                                MemoryModule::UniversalAnger,
                                // The spear fight's (Piglin.BRAIN_PROVIDER).
                                MemoryModule::SpearFleeingTime,
                                MemoryModule::SpearFleeingPosition,
                                MemoryModule::SpearChargePosition,
                                MemoryModule::SpearEngageTime,
                                MemoryModule::SpearStatus }) {
            brain.RegisterMemory(m);
        }

        // MC SENSOR_TYPES: NEAREST_LIVING_ENTITIES, NEAREST_PLAYERS,
        // NEAREST_ITEMS, HURT_BY, PIGLIN_SPECIFIC_SENSOR.
        brain.AddSensor(std::make_unique<NearestLivingEntitySensor>());
        brain.AddSensor(std::make_unique<PlayerSensor>());
        brain.AddSensor(std::make_unique<PiglinNearestItemSensor>());
        brain.AddSensor(std::make_unique<HurtBySensor>());
        brain.AddSensor(std::make_unique<PiglinSpecificSensor>());

        EntityLevel* level = piglin.Level();

        // ── CORE (MC initCoreActivity) ─────────────────────────────────────
        // InteractWithDoor is skipped — door interaction.
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
        core.push_back(std::make_unique<StopHoldingItemIfNoLongerAdmiring>());
        core.push_back(std::make_unique<StartAdmiringItemIfSeen>(kAdmireDuration));
        core.push_back(std::make_unique<StartCelebratingIfTargetDead>(kCelebrationTime));
        core.push_back(std::make_unique<StopBeingAngryIfTargetDead>());
        brain.AddActivity(Activity::Core, 0, std::move(core));

        // ── IDLE (MC initIdleActivity, priority 10) ────────────────────────
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
        idle.push_back(std::make_unique<SetLookAndInteractPlayer>(4));
        brain.AddActivity(Activity::Idle, 10, std::move(idle));

        // ── FIGHT (MC initFightActivity, priority 10) ──────────────────────
        // The walk keeps a crossbow's range (isWithinAttackRange, margin 1),
        // the spear fights by its three behaviours, the melee stands down
        // while either is held (canUseNonMeleeWeapon).
        std::vector<BehaviorPtr> fight;
        // MC StopAttackingIfTargetInvalid.create(!isNearestValidAttackTarget).
        fight.push_back(std::make_unique<StopAttackingIfTargetInvalid>(
            [](EntityLevel&, Mob& body, LivingEntity& target) {
                return FindTarget(body) != &target;
            }));
        fight.push_back(std::make_unique<BackUpIfTooClose>(kMinDistFromTargetWithCrossbow,
                                                           kSpeedWhenStrafingBack));
        fight.push_back(std::make_unique<SetWalkTargetFromAttackTarget>(1.0f));
        fight.push_back(std::make_unique<SpearApproach>(1.0, 10.0f));
        fight.push_back(std::make_unique<SpearAttack>(1.0, 1.0, 2.0f));
        fight.push_back(std::make_unique<SpearRetreat>(1.0));
        fight.push_back(std::make_unique<MeleeAttack>(20));
        fight.push_back(std::make_unique<PiglinCrossbowAttack>());
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

        // ── ADMIRE_ITEM (MC initAdmireItemActivity, priority 10) ───────────
        std::vector<BehaviorPtr> admire;
        admire.push_back(std::make_unique<PiglinGoToWantedItem>(1.0f, kMaxDistanceToWalkToItem));
        admire.push_back(std::make_unique<StopAdmiringIfItemTooFarAway>(kMaxDistanceToWalkToItem));
        admire.push_back(std::make_unique<StopAdmiringIfTiredOfTryingToReachItem>(kMaxTimeToWalkToItem,
                                                                                   kDisableAdmireWalking));
        brain.AddActivityAndRemoveMemoryWhenStopped(Activity::AdmireItem, 10, std::move(admire),
                                                    MemoryModule::AdmiringItem);

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

    const char* PiglinAi::SoundForCurrentActivity(const Piglin& piglin) {
        const Brain* brain = piglin.GetBrain();
        const std::optional<Activity> activity = brain ? brain->GetActiveNonCoreActivity() : std::nullopt;
        if (!activity) return "";
        if (*activity == Activity::Fight) return SoundEvents::PIGLIN_ANGRY;
        if (piglin.IsConverting()) return SoundEvents::PIGLIN_RETREAT;
        if (*activity == Activity::Avoid) {
            const auto* avoid = dynamic_cast<const LivingEntity*>(brain->GetEntity(MemoryModule::AvoidTarget));
            if (avoid) {
                const glm::dvec3 d = avoid->position - piglin.position;
                if (d.x * d.x + d.y * d.y + d.z * d.z < 12.0 * 12.0) return SoundEvents::PIGLIN_RETREAT;
            }
        }
        if (*activity == Activity::AdmireItem) return SoundEvents::PIGLIN_ADMIRING_ITEM;
        if (*activity == Activity::Celebrate) return SoundEvents::PIGLIN_CELEBRATE;
        // seesPlayerHoldingLovedItem → JEALOUS; isNearRepellent → RETREAT.
        if (brain->HasMemoryValue(MemoryModule::NearestPlayerHoldingWantedItem)) return SoundEvents::PIGLIN_JEALOUS;
        if (brain->HasMemoryValue(MemoryModule::NearestRepellent)) return SoundEvents::PIGLIN_RETREAT;
        return SoundEvents::PIGLIN_AMBIENT;
    }

    void PiglinAi::UpdateActivity(Piglin& piglin) {
        Brain* brain = piglin.GetBrain();
        if (!brain) return;
        // MC's order, ADMIRE_ITEM included (it never validates here — the
        // activity is not registered without the item system). An activity
        // change voices the new activity (MC makeSound).
        const std::optional<Activity> oldActivity = brain->GetActiveNonCoreActivity();
        brain->SetActiveActivityToFirstValid(
            { Activity::AdmireItem, Activity::Fight, Activity::Avoid,
              Activity::Celebrate, Activity::Ride, Activity::Idle });
        if (brain->GetActiveNonCoreActivity() != oldActivity) {
            piglin.MakeSound(SoundForCurrentActivity(piglin));
        }

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

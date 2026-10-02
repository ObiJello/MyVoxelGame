// File: src/common/entity/ai/brain/AllayAi.cpp
#include "common/entity/ai/brain/AllayAi.hpp"
#include "server/advancements/CriteriaTriggers.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/ai/brain/CommonBehaviors.hpp"
#include "common/entity/ai/brain/CoreBehaviors.hpp"
#include "common/entity/ai/brain/Sensor.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace Game {

    namespace {

        // MC AllayAi constants.
        constexpr float kSpeedWhenIdling = 1.0f;
        constexpr float kSpeedWhenFollowingDepositTarget = 2.25f;
        constexpr float kSpeedWhenRetrievingItem = 1.75f;
        constexpr float kSpeedWhenPanicking = 2.5f;
        constexpr int   kCloseEnoughToTarget = 4;
        constexpr int   kTooFarFromTarget = 16;
        constexpr int   kTimeToForgetNoteblock = 600;
        constexpr int   kDistanceToWantedItem = 32;
        constexpr int   kGiveItemTimeoutDuration = 20;
        constexpr int   kItemPickupCooldownDuration = 60;
        const glm::dvec3 kThrowVelocity(0.20000000298023224, 0.30000001192092896, 0.20000000298023224);

        // MC Allay.THROW_SOUND_PITCHES.
        constexpr float kThrowSoundPitches[] = {
            0.5625f, 0.625f, 0.75f, 0.9375f, 1.0f, 1.0f, 1.125f, 1.25f,
            1.5f, 1.875f, 2.0f, 2.25f, 2.5f, 3.0f, 3.75f, 4.0f };

        Allay& A(LivingEntity& body) { return static_cast<Allay&>(body); }

        glm::ivec3 Containing(const glm::dvec3& p) {
            return glm::ivec3(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)),
                              static_cast<int>(std::floor(p.z)));
        }

        // MC PositionTracker.currentPosition: an EntityTracker's entity (eye
        // or feet), a BlockPosTracker's Vec3.atCenterOf(pos).
        glm::dvec3 TrackerPosition(const PositionTracker& t) {
            if (t.entity) return t.CurrentPosition();
            return glm::dvec3(t.blockPos) + glm::dvec3(0.5);
        }

        // MC AllayAi.shouldDepositItemsAtLikedNoteblock.
        bool ShouldDepositItemsAtLikedNoteblock(const Allay& allay, const Brain& brain,
                                                const glm::ivec3& noteblock) {
            const EntityLevel* level = allay.Level();
            const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
            if (!blocks) return false;
            // GlobalPos.isCloseEnough(dimension, blockPosition, 1024): the
            // memory is per level here, so the dimension always matches.
            const glm::dvec3 d = glm::dvec3(noteblock) - glm::dvec3(allay.BlockPosition());
            const double limit = static_cast<double>(Allay::kMaxNoteblockDistance);
            if (!(glm::dot(d, d) < limit * limit)) return false;
            if (blocks->GetBlock(noteblock.x, noteblock.y, noteblock.z) != BlockID::NoteBlock) return false;
            return brain.HasMemoryValue(MemoryModule::LikedNoteblockCooldownTicks);
        }

        // MC AllayAi.getItemDepositPosition — which may ERASE a liked
        // noteblock that no longer qualifies, exactly as MC's getter does.
        std::optional<PositionTracker> GetItemDepositPosition(Allay& allay) {
            Brain* brain = allay.GetBrain();
            if (!brain) return std::nullopt;
            if (const auto liked = brain->GetBlockPos(MemoryModule::LikedNoteblockPosition)) {
                if (ShouldDepositItemsAtLikedNoteblock(allay, *brain, *liked)) {
                    return PositionTracker::OfBlock(*liked + glm::ivec3(0, 1, 0));
                }
                brain->EraseMemory(MemoryModule::LikedNoteblockPosition);
            }
            // getLikedPlayerPositionTracker: EntityTracker(player, true).
            if (LivingEntity* player = allay.GetLikedPlayer()) {
                return PositionTracker::OfEntity(player, true);
            }
            return std::nullopt;
        }

        bool HasWantedItem(const Allay& allay) { return allay.GetWantedItemId().has_value(); }

        // MC AllayAi.throwItem: one item out of the inventory, thrown with
        // BehaviorUtils.throwItem(thrower, item, target + (0, 1, 0),
        // THROW_VELOCITY, 0.2), and — on a game time ≡ 0 (mod 7), 90 % of
        // the time — ALLAY_THROW at one of the sixteen pitches.
        void ThrowItem(EntityLevel& level, Allay& thrower, const glm::dvec3& targetPos) {
            SimpleContainer& inv = thrower.GetInventory();
            ItemStack& slot = inv.GetItem(0);
            if (slot.IsEmpty()) return;
            ItemStack item = slot;
            item.count = 1;
            slot.count -= 1;
            if (slot.count <= 0) inv.SetItem(0, ItemStack{});

            const glm::dvec3 target = targetPos + glm::dvec3(0.0, 1.0, 0.0);
            const glm::dvec3 from(thrower.position.x, thrower.GetEyeY() - 0.2, thrower.position.z);
            glm::dvec3 dir = target - thrower.position;
            const double len = glm::length(dir);
            dir = len < 1.0e-5 ? glm::dvec3(0.0) : dir / len;   // Vec3.normalize
            // setDefaultPickUpDelay; BehaviorUtils.throwItem makes the allay
            // the item's owner (THROWN_ITEM_PICKED_UP_BY_PLAYER reads it).
            level.SpawnThrownItem(from, dir * kThrowVelocity, item, 10, thrower.GetId());
            // getLikedPlayer(thrower).ifPresent(player →
            // ALLAY_DROP_ITEM_ON_BLOCK.trigger(player, containing(target).below(), item)).
            if (!level.IsClientSide()) {
                if (Server::ServerPlayer* liked = Server::CriteriaTriggers::PlayerOf(thrower.GetLikedPlayer())) {
                    const glm::ivec3 below(static_cast<int>(std::floor(targetPos.x)),
                                           static_cast<int>(std::floor(targetPos.y)) - 1,
                                           static_cast<int>(std::floor(targetPos.z)));
                    Server::CriteriaTriggers::AllayDropItemOnBlock(*liked, below, item);
                }
            }

            JavaRandom& rng = level.Random();
            if (level.GetGameTime() % 7 == 0 && rng.NextDouble() < 0.9) {
                const int n = static_cast<int>(sizeof(kThrowSoundPitches) / sizeof(kThrowSoundPitches[0]));
                const float pitch = kThrowSoundPitches[rng.NextInt(n)];
                level.PlaySoundFromEntity(nullptr, thrower, SoundEvents::ALLAY_THROW,
                                          SoundSource::Neutral, 1.0f, pitch);
            }
        }

        // ── Sensor ───────────────────────────────────────────────────────

        // MC NearestItemSensor (Sensor<Mob>, scan rate 20): the nearest item
        // entity within (32, 16, 32) that the allay wants, closer than 32
        // and in sight. Item entities are not Entities here, so the answer
        // goes on the allay (SetWantedItemId).
        class AllayNearestItemSensor : public Sensor {
        public:
            std::vector<MemoryModule> Requires() const override {
                return { MemoryModule::NearestVisibleWantedItem };
            }
        protected:
            // MC LivingEntity.hasLineOfSight(item): eye to the item's eye
            // (0.2125 above it), within 128, no collider in between — the
            // same quarter-block walk the villager's item sensor takes.
            static bool ClearSight(EntityLevel& level, const glm::dvec3& from, const glm::dvec3& to) {
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
                    const glm::ivec3 b = Containing(p);
                    if (BlockRegistry::HasCollision(blocks->GetBlock(b.x, b.y, b.z))) return false;
                }
                return true;
            }
            void DoTick(EntityLevel& level, LivingEntity& body) override {
                Allay& allay = A(body);
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
                    if (!stack || !allay.WantsToPickUp(*stack)) continue;
                    const glm::dvec3 d = item.pos - body.position;
                    if (glm::dot(d, d) >= 32.0 * 32.0) continue;
                    if (!ClearSight(level, body.GetEyePosition(), item.pos + glm::dvec3(0.0, 0.2125, 0.0))) continue;
                    found = item.id;
                    break;
                }
                allay.SetWantedItemId(found);
            }
        };

        // ── IDLE behaviours ──────────────────────────────────────────────

        // MC GoToWantedItem.create(mob -> true, 1.75, true, 32): interrupting
        // any walk (WALK_TARGET only REGISTERED), a one-shot that aims the
        // look and the walk at the wanted item.
        class GoToWantedItem : public Behavior {
        public:
            GoToWantedItem(float speed, int maxDist)
                : Behavior({ { MemoryModule::LookTarget, MemoryStatus::Registered },
                             { MemoryModule::WalkTarget, MemoryStatus::Registered },
                             { MemoryModule::ItemPickupCooldownTicks, MemoryStatus::Registered } }, 1),
                  m_speed(speed), m_maxDist(maxDist) {}
            const char* DebugString() const override { return "GoToWantedItem"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                Allay& allay = A(body);
                const auto id = allay.GetWantedItemId();
                if (!id) return false;
                Brain* brain = body.GetBrain();
                if (brain->HasMemoryValue(MemoryModule::ItemPickupCooldownTicks)) return false;
                if (!allay.CanPickUpLootNow()) return false;
                AABBd box = body.GetAABBd();
                box.min -= glm::dvec3(m_maxDist + 1.0);
                box.max += glm::dvec3(m_maxDist + 1.0);
                std::vector<EntityLevel::NearbyItemEntity> items;
                level.GetItemEntitiesInBox(box, items);
                for (const auto& item : items) {
                    if (item.id != *id) continue;
                    // item.closerThan(body, maxDist).
                    const glm::dvec3 d = item.pos - body.position;
                    if (glm::dot(d, d) >= static_cast<double>(m_maxDist) * m_maxDist) return false;
                    const glm::ivec3 cell = Containing(item.pos);
                    brain->SetMemory(MemoryModule::LookTarget, PositionTracker::OfBlock(cell));
                    brain->SetMemory(MemoryModule::WalkTarget, WalkTarget(cell, m_speed, 0));
                    return true;
                }
                return false;
            }
        private:
            float m_speed;
            int   m_maxDist;
        };

        // MC GoAndGiveItemsToTarget(getItemDepositPosition, 2.25, 20,
        // throwItem, ITEM_PICKUP_COOLDOWN_TICKS, 60, hasItemToThrow).
        class GoAndGiveItemsToTarget : public Behavior {
        public:
            GoAndGiveItemsToTarget(float speed, int timeout, int cooldown)
                : Behavior({ { MemoryModule::LookTarget, MemoryStatus::Registered },
                             { MemoryModule::WalkTarget, MemoryStatus::Registered },
                             { MemoryModule::ItemPickupCooldownTicks, MemoryStatus::Registered } }, timeout),
                  m_speed(speed), m_cooldown(cooldown) {}
            const char* DebugString() const override { return "GoAndGiveItemsToTarget"; }
        protected:
            static bool CanThrowItemToTarget(Allay& allay) {
                // hasItemToThrow && the deposit position is present.
                bool any = false;
                const SimpleContainer& inv = allay.GetInventory();
                for (int i = 0; i < inv.GetContainerSize(); ++i) any |= !inv.GetItem(i).IsEmpty();
                return any && GetItemDepositPosition(allay).has_value();
            }
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                return CanThrowItemToTarget(A(body));
            }
            bool CanStillUse(EntityLevel&, LivingEntity& body, int64_t) override {
                return CanThrowItemToTarget(A(body));
            }
            void Start(EntityLevel&, LivingEntity& body, int64_t) override {
                // BehaviorUtils.setWalkAndLookTargetMemories(body, tracker, speed, 3).
                if (const auto tracker = GetItemDepositPosition(A(body))) {
                    Brain* brain = body.GetBrain();
                    brain->SetMemory(MemoryModule::LookTarget, *tracker);
                    brain->SetMemory(MemoryModule::WalkTarget, WalkTarget(*tracker, m_speed, 3));
                }
            }
            void Tick(EntityLevel& level, LivingEntity& body, int64_t) override {
                Allay& allay = A(body);
                const auto tracker = GetItemDepositPosition(allay);
                if (!tracker) return;
                const glm::dvec3 depositPosition = TrackerPosition(*tracker);
                if (glm::length(depositPosition - body.GetEyePosition()) < 3.0) {
                    ThrowItem(level, allay, depositPosition);
                    body.GetBrain()->SetMemory(MemoryModule::ItemPickupCooldownTicks, m_cooldown);
                }
            }
        private:
            float m_speed;
            int   m_cooldown;
        };

        // MC StayCloseToTarget.create(getItemDepositPosition,
        // not(hasWantedItem), 4, 16, 2.25): a one-shot that walks back to the
        // deposit target once it is 16 or more away.
        class StayCloseToTarget : public Behavior {
        public:
            StayCloseToTarget(int closeEnough, int tooFar, float speed)
                : Behavior({ { MemoryModule::LookTarget, MemoryStatus::Registered },
                             { MemoryModule::WalkTarget, MemoryStatus::Registered } }, 1),
                  m_closeEnough(closeEnough), m_tooFar(tooFar), m_speed(speed) {}
            const char* DebugString() const override { return "StayCloseToTarget"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                Allay& allay = A(body);
                const auto tracker = GetItemDepositPosition(allay);
                if (!tracker || HasWantedItem(allay)) return false;
                const glm::dvec3 d = body.position - TrackerPosition(*tracker);
                if (glm::dot(d, d) < static_cast<double>(m_tooFar) * m_tooFar) return false;
                Brain* brain = body.GetBrain();
                brain->SetMemory(MemoryModule::LookTarget, *tracker);
                brain->SetMemory(MemoryModule::WalkTarget, WalkTarget(*tracker, m_speed, m_closeEnough));
                return true;
            }
        private:
            int   m_closeEnough;
            int   m_tooFar;
            float m_speed;
        };

    } // namespace

    void AllayAi::InitBrain(Allay& allay, Brain& brain) {
        (void)allay;

        // MC Allay.BRAIN_PROVIDER: LIKED_PLAYER, LIKED_NOTEBLOCK_POSITION,
        // LIKED_NOTEBLOCK_COOLDOWN_TICKS, plus every memory its sensors and
        // behaviours name.
        for (MemoryModule m : { MemoryModule::Path,
                                MemoryModule::LookTarget,
                                MemoryModule::NearestLivingEntities,
                                MemoryModule::NearestVisibleLivingEntities,
                                MemoryModule::WalkTarget,
                                MemoryModule::CantReachWalkTargetSince,
                                MemoryModule::HurtBy,
                                MemoryModule::HurtByEntity,
                                MemoryModule::NearestPlayers,
                                MemoryModule::NearestVisiblePlayer,
                                MemoryModule::NearestVisibleAttackablePlayer,
                                MemoryModule::NearestVisibleAttackablePlayers,
                                MemoryModule::IsPanicking,
                                MemoryModule::NearestVisibleWantedItem,
                                MemoryModule::LikedPlayer,
                                MemoryModule::LikedNoteblockPosition,
                                MemoryModule::LikedNoteblockCooldownTicks,
                                MemoryModule::ItemPickupCooldownTicks }) {
            brain.RegisterMemory(m);
        }

        // MC SENSOR_TYPES: NEAREST_LIVING_ENTITIES, NEAREST_PLAYERS, HURT_BY,
        // NEAREST_ITEMS.
        brain.AddSensor(std::make_unique<NearestLivingEntitySensor>());
        brain.AddSensor(std::make_unique<PlayerSensor>());
        brain.AddSensor(std::make_unique<HurtBySensor>());
        brain.AddSensor(std::make_unique<AllayNearestItemSensor>());

        // ── CORE (MC initCoreActivity) ─────────────────────────────────────
        std::vector<BehaviorPtr> core;
        core.push_back(std::make_unique<Swim>(0.8f));
        core.push_back(std::make_unique<AnimalPanic>(kSpeedWhenPanicking));
        core.push_back(std::make_unique<LookAtTargetSink>(45, 90));
        core.push_back(std::make_unique<MoveToTargetSink>());
        core.push_back(std::make_unique<CountDownCooldownTicks>(MemoryModule::LikedNoteblockCooldownTicks));
        core.push_back(std::make_unique<CountDownCooldownTicks>(MemoryModule::ItemPickupCooldownTicks));
        brain.AddActivity(Activity::Core, 0, std::move(core));

        // ── IDLE (MC initIdleActivity, priorities 0..4) ────────────────────
        std::vector<BehaviorPtr> idle;
        idle.push_back(std::make_unique<GoToWantedItem>(kSpeedWhenRetrievingItem, kDistanceToWantedItem));
        idle.push_back(std::make_unique<GoAndGiveItemsToTarget>(kSpeedWhenFollowingDepositTarget,
                                                                kGiveItemTimeoutDuration,
                                                                kItemPickupCooldownDuration));
        idle.push_back(std::make_unique<StayCloseToTarget>(kCloseEnoughToTarget, kTooFarFromTarget,
                                                           kSpeedWhenFollowingDepositTarget));
        idle.push_back(std::make_unique<SetEntityLookTargetSometimes>(6.0f, 30, 60));
        std::vector<GateBehavior::Entry> gate;
        gate.push_back({ RandomStroll::Fly(kSpeedWhenIdling), 2 });
        gate.push_back({ std::make_unique<SetWalkTargetFromLookTarget>(kSpeedWhenIdling, 3), 2 });
        gate.push_back({ std::make_unique<DoNothing>(30, 60), 1 });
        idle.push_back(MakeRunOne(std::move(gate)));
        brain.AddActivity(Activity::Idle, 0, std::move(idle));

        brain.SetCoreActivities({ Activity::Core });
        brain.SetDefaultActivity(Activity::Idle);
        brain.UseDefaultActivity();
    }

    void AllayAi::UpdateActivity(Allay& allay) {
        if (Brain* brain = allay.GetBrain()) {
            // MC AllayAi.updateActivity — IDLE is the only non-core activity.
            brain->SetActiveActivityToFirstValid({ Activity::Idle });
        }
    }

    void AllayAi::HearNoteblock(Allay& allay, const glm::ivec3& pos) {
        Brain* brain = allay.GetBrain();
        if (!brain) return;
        const auto liked = brain->GetBlockPos(MemoryModule::LikedNoteblockPosition);
        if (!liked) {
            brain->SetMemory(MemoryModule::LikedNoteblockPosition, pos);
            brain->SetMemory(MemoryModule::LikedNoteblockCooldownTicks, kTimeToForgetNoteblock);
        } else if (*liked == pos) {
            brain->SetMemory(MemoryModule::LikedNoteblockCooldownTicks, kTimeToForgetNoteblock);
        }
    }


} // namespace Game

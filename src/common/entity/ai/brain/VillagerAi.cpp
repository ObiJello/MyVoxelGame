// File: src/common/entity/ai/brain/VillagerAi.cpp
//
// See VillagerAi.hpp. Every class below names the MC class it ports; the
// entry conditions (the MemoryCondition lists) are MC's BehaviorBuilder
// groups in order, and a MC OneShot is a Behavior whose trigger body runs in
// CheckExtraStartConditions (it starts, is ticked once, and stops — the
// engine's default CanStillUse is false, which is exactly OneShot's life).
#include "common/entity/ai/brain/VillagerAi.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/ai/RandomPos.hpp"
#include "common/entity/ai/brain/Brain.hpp"
#include "common/entity/ai/brain/CommonBehaviors.hpp"
#include "common/entity/ai/brain/CoreBehaviors.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/entity/ai/village/PoiManager.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/entity/npc/Villager.hpp"
#include "common/sound/LevelEventSounds.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/sound/SoundType.hpp"
#include "common/world/block/BedBlock.hpp"
#include "common/world/block/BlockPlacement.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/loot/ChestLootTables.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <vector>

namespace Game {

    namespace {

        // ═════════════════════════════════════════════════════════════════
        // Helpers
        // ═════════════════════════════════════════════════════════════════

        // A MC Behavior whose timedOut() returns false: an effectively
        // infinite duration (max()-1 because TryStart adds 1).
        constexpr int kNoTimeout = std::numeric_limits<int>::max() - 1;
        constexpr double kHalfPi = 1.5707963705062866;   // (double)(float)(PI/2)

        Villager& V(LivingEntity& body) { return static_cast<Villager&>(body); }

        bool IsVillager(const Entity* e) { return e && e->GetType() == EntityTypeId::Villager; }

        // MC Vec3i.closerToCenterThan(Position, dist): the block's CENTRE
        // within `dist` (strictly) of the point.
        bool CloserToCenterThan(const glm::ivec3& b, const glm::dvec3& p, double dist) {
            const double dx = b.x + 0.5 - p.x, dy = b.y + 0.5 - p.y, dz = b.z + 0.5 - p.z;
            return dx * dx + dy * dy + dz * dz < dist * dist;
        }
        // MC Vec3i.closerThan(Vec3i, dist): block-to-block.
        bool BlockCloserThan(const glm::ivec3& a, const glm::ivec3& b, double dist) {
            const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return dx * dx + dy * dy + dz * dz < dist * dist;
        }
        int DistManhattan(const glm::ivec3& a, const glm::ivec3& b) {
            return std::abs(a.x - b.x) + std::abs(a.y - b.y) + std::abs(a.z - b.z);
        }
        glm::ivec3 Containing(const glm::dvec3& p) {
            return glm::ivec3(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)),
                              static_cast<int>(std::floor(p.z)));
        }
        glm::dvec3 BottomCenter(const glm::ivec3& b) { return glm::dvec3(b.x + 0.5, b.y, b.z + 0.5); }

        PoiManager* Poi(EntityLevel& level) { return level.GetPoiManager(); }

        BlockState StateAt(EntityLevel& level, const glm::ivec3& p) {
            const IBlockAccess* blocks = level.Blocks();
            return blocks ? blocks->GetBlockState(p.x, p.y, p.z) : BlockState{};
        }

        bool IsDyedBed(BlockID id) { return IsBedBlock(id) && id != BlockID::StrawBed; }

        // MC BehaviorUtils.lookAtEntity / setWalkAndLookTargetMemories.
        void LookAtEntity(LivingEntity& looker, LivingEntity& target) {
            if (Brain* b = looker.GetBrain()) b->SetMemory(MemoryModule::LookTarget, PositionTracker::OfEntity(&target, true));
        }
        void SetWalkAndLookTargetMemories(LivingEntity& walker, const PositionTracker& target,
                                          float speed, int closeEnough) {
            Brain* b = walker.GetBrain();
            if (!b) return;
            b->SetMemory(MemoryModule::LookTarget, target);
            b->SetMemory(MemoryModule::WalkTarget, WalkTarget(target, speed, closeEnough));
        }
        void LockGazeAndWalkToEachOther(LivingEntity& a, LivingEntity& b, float speed, int closeEnough) {
            LookAtEntity(a, b);
            LookAtEntity(b, a);
            SetWalkAndLookTargetMemories(a, PositionTracker::OfEntity(&b, true), speed, closeEnough);
            SetWalkAndLookTargetMemories(b, PositionTracker::OfEntity(&a, true), speed, closeEnough);
        }

        // MC NearestVisibleLivingEntities.contains(entity) — in the list AND
        // passing the lazy line-of-sight test.
        bool EntityIsVisible(const Brain& brain, LivingEntity* target) {
            const NearestVisibleLivingEntities* visible =
                brain.GetVisibleEntities(MemoryModule::NearestVisibleLivingEntities);
            return visible && target && visible->Contains(target) && visible->IsVisible(target);
        }
        // MC BehaviorUtils.targetIsValid(brain, memory, VILLAGER).
        Villager* ValidVillagerTarget(const Brain& brain, MemoryModule memory) {
            Entity* e = brain.GetEntity(memory);
            if (!IsVillager(e)) return nullptr;
            auto* v = static_cast<Villager*>(e);
            if (!v->IsAlive() || !EntityIsVisible(brain, v)) return nullptr;
            return v;
        }

        // MC BehaviorUtils.throwItem(thrower, item, targetPos): from 0.3
        // below the eye, 0.3 blocks/tick toward the target on each axis.
        void ThrowItem(LivingEntity& thrower, const ItemStack& item, const glm::dvec3& targetPos) {
            EntityLevel* level = thrower.Level();
            if (!level || item.IsEmpty()) return;
            const glm::dvec3 from(thrower.position.x, thrower.GetEyeY() - 0.3, thrower.position.z);
            glm::dvec3 dir = targetPos - thrower.position;
            const double len = glm::length(dir);
            if (len > 1.0e-7) dir /= len;
            level->SpawnThrownItem(from, dir * 0.30000001192092896, item, 10);   // setDefaultPickUpDelay
        }

        // MC SectionPos.of(BlockPos).
        glm::ivec3 SectionOf(const glm::ivec3& b) { return glm::ivec3(b.x >> 4, b.y >> 4, b.z >> 4); }

        // ── Crops (MC CropBlock family) ────────────────────────────────────
        int CropMaxAge(BlockID id) {
            switch (id) {
                case BlockID::Wheat: case BlockID::Carrots: case BlockID::Potatoes: return 7;
                case BlockID::Beetroots:       return 3;   // BeetrootBlock
                case BlockID::TorchflowerCrop: return 2;   // TorchflowerCropBlock (never reached)
                default:                       return -1;
            }
        }
        bool IsCropBlock(BlockID id) { return CropMaxAge(id) >= 0; }
        int ReadIntProperty(BlockState s, std::string_view name) {
            const std::string_view v = s.GetValueByName(name);
            int out = 0;
            std::from_chars(v.data(), v.data() + v.size(), out);
            return out;
        }
        bool IsMaxAgeCrop(BlockState s) {
            const int max = CropMaxAge(s.Block());
            return max >= 0 && ReadIntProperty(s, "age") >= max;
        }

        // ── Doors (MC DoorBlock.setOpen / isOpen, #mob_interactable_doors) ─
        bool IsMobInteractableDoor(BlockState s) { return IsWoodenDoorBlock(s.Block()); }
        bool IsDoorOpen(BlockState s) { return s.GetValueByName("open") == "true"; }

        void SetDoorOpen(EntityLevel& level, const glm::ivec3& pos, bool open) {
            // MC DoorBlock.setOpen(entity, level, state, pos, open): flags 10
            // (UPDATE_CLIENTS | UPDATE_IMMEDIATE), the other half following
            // (MC through updateShape; explicit here — the engine has no
            // double-block linkage), and the door's open/close sound.
            ILevelWrite* world = level.MutableBlocks();
            if (!world) return;
            const BlockState state = world->GetBlockState(pos.x, pos.y, pos.z);
            if (!IsMobInteractableDoor(state) || IsDoorOpen(state) == open) return;
            const std::string_view to = open ? "true" : "false";
            constexpr uint32_t kFlags = World::UpdateFlags::UpdateClients | World::UpdateFlags::Immediate;
            world->SetBlock(pos.x, pos.y, pos.z, state.SetName(PropertyId::OPEN, to), kFlags);
            const bool lower = state.GetName(PropertyId::DOUBLE_BLOCK_HALF) == "lower";
            const glm::ivec3 other = pos + glm::ivec3(0, lower ? 1 : -1, 0);
            const BlockState otherState = world->GetBlockState(other.x, other.y, other.z);
            if (otherState.Block() == state.Block() &&
                otherState.GetName(PropertyId::DOUBLE_BLOCK_HALF) == (lower ? "upper" : "lower")) {
                world->SetBlock(other.x, other.y, other.z, otherState.SetName(PropertyId::OPEN, to), kFlags);
            }
            if (const BlockSetType* set = BlockSetTypeOf(state.Block())) {
                // DoorBlock.playSound: pitch nextFloat() * 0.1 + 0.9.
                const float pitch = level.Random().NextFloat() * 0.1f + 0.9f;
                level.PlaySound(nullptr, pos, open ? set->doorOpen : set->doorClose,
                                SoundSource::Blocks, 1.0f, pitch);
            }
        }

        // MC InteractWithDoor.isMobComingThroughDoor — the other mob's live
        // path (MC reads its PATH memory; the navigation holds the same path).
        bool IsMobComingThroughDoor(LivingEntity& other, const glm::ivec3& doorPos) {
            auto* mob = dynamic_cast<Mob*>(&other);
            if (!mob || !mob->HasAiControls()) return false;
            const Path* path = mob->GetNavigation().GetPath();
            if (!path || path->IsDone() || path->GetNextNodeIndex() <= 0) return false;
            const Node& from = path->GetNode(path->GetNextNodeIndex() - 1);
            const Node& to = path->GetNextNode();
            return doorPos == glm::ivec3(from.x, from.y, from.z) || doorPos == glm::ivec3(to.x, to.y, to.z);
        }

        // MC InteractWithDoor.closeDoorsThatIHaveOpenedOrPassedThrough.
        void CloseDoorsThatIHaveOpenedOrPassedThrough(EntityLevel& level, Villager& body,
                                                      const glm::ivec3* movingFrom,
                                                      const glm::ivec3* movingTo) {
            std::vector<glm::ivec3>& doors = body.DoorsToClose();
            const Brain* brain = body.GetBrain();
            const std::vector<Entity*>* nearest =
                brain ? brain->GetEntityList(MemoryModule::NearestLivingEntities) : nullptr;
            for (auto it = doors.begin(); it != doors.end();) {
                const glm::ivec3 doorPos = *it;
                if ((movingFrom && *movingFrom == doorPos) || (movingTo && *movingTo == doorPos)) {
                    ++it;
                    continue;
                }
                // isDoorTooFarAway: another dimension, or the door's centre 3+
                // blocks from the body.
                if (!CloserToCenterThan(doorPos, body.position, 3.0)) { it = doors.erase(it); continue; }
                const BlockState state = StateAt(level, doorPos);
                if (!IsMobInteractableDoor(state) || !IsDoorOpen(state)) { it = doors.erase(it); continue; }
                bool othersComing = false;
                if (nearest) {
                    for (Entity* e : *nearest) {
                        auto* other = e ? e->AsLiving() : nullptr;
                        if (!other || other->GetType() != body.GetType()) continue;
                        if (!CloserToCenterThan(doorPos, other->position, 2.0)) continue;
                        if (IsMobComingThroughDoor(*other, doorPos)) { othersComing = true; break; }
                    }
                }
                if (!othersComing) SetDoorOpen(level, doorPos, false);
                it = doors.erase(it);
            }
        }

        void RememberDoorToClose(Villager& body, const glm::ivec3& pos) {
            auto& doors = body.DoorsToClose();
            if (std::find(doors.begin(), doors.end(), pos) == doors.end()) doors.push_back(pos);
        }

        // MC AcquirePoi.findPathToPois.
        std::optional<Path> FindPathToPois(Mob& body, const std::vector<std::pair<PoiType, glm::ivec3>>& pois) {
            if (pois.empty()) return std::nullopt;
            std::vector<glm::ivec3> targets;
            int maxRange = 1;
            for (const auto& [type, pos] : pois) {
                maxRange = std::max(maxRange, PoiValidRange(type));
                targets.push_back(pos);
            }
            return body.GetNavigation().CreatePath(targets, maxRange);
        }

        // MC BlockPos.asLong.
        int64_t PosKey(const glm::ivec3& p) {
            return ((static_cast<int64_t>(p.x) & 0x3FFFFFF) << 38) |
                   ((static_cast<int64_t>(p.z) & 0x3FFFFFF) << 12) |
                   (static_cast<int64_t>(p.y) & 0xFFF);
        }

        // ═════════════════════════════════════════════════════════════════
        // Sensors
        // ═════════════════════════════════════════════════════════════════

        // MC VillagerHostilesSensor — the nearest visible hostile within its
        // own flee distance.
        class VillagerHostilesSensor : public Sensor {
        public:
            std::vector<MemoryModule> Requires() const override { return { MemoryModule::NearestHostile }; }
        protected:
            static float AcceptableDistance(const LivingEntity& e) {
                // Player views wear a placeholder type: never a hostile.
                if (e.IsPlayer()) return -1.0f;
                switch (e.GetType()) {
                    case EntityTypeId::Drowned:        return 8.0f;
                    case EntityTypeId::Evoker:         return 12.0f;
                    case EntityTypeId::Husk:           return 8.0f;
                    case EntityTypeId::Illusioner:     return 12.0f;
                    case EntityTypeId::Pillager:       return 15.0f;
                    case EntityTypeId::Ravager:        return 12.0f;
                    case EntityTypeId::Vex:            return 8.0f;
                    case EntityTypeId::Vindicator:     return 10.0f;
                    case EntityTypeId::Zoglin:         return 10.0f;
                    case EntityTypeId::Zombie:         return 8.0f;
                    case EntityTypeId::ZombieVillager: return 8.0f;
                    default:                           return -1.0f;
                }
            }
            void DoTick(EntityLevel&, LivingEntity& body) override {
                Brain* brain = body.GetBrain();
                if (!brain) return;
                const NearestVisibleLivingEntities* visible =
                    brain->GetVisibleEntities(MemoryModule::NearestVisibleLivingEntities);
                LivingEntity* hostile = visible ? visible->FindClosest([&](LivingEntity* e) {
                    const float d = AcceptableDistance(*e);
                    return d > 0.0f && e->DistanceToSqr(body) <= static_cast<double>(d * d);
                }) : nullptr;
                if (hostile) brain->SetMemory(MemoryModule::NearestHostile, static_cast<Entity*>(hostile));
                else brain->EraseMemory(MemoryModule::NearestHostile);
            }
        };

        // MC VillagerBabiesSensor.
        class VillagerBabiesSensor : public Sensor {
        public:
            std::vector<MemoryModule> Requires() const override { return { MemoryModule::VisibleVillagerBabies }; }
        protected:
            void DoTick(EntityLevel&, LivingEntity& body) override {
                Brain* brain = body.GetBrain();
                if (!brain) return;
                std::vector<Entity*> babies;
                if (const NearestVisibleLivingEntities* visible =
                        brain->GetVisibleEntities(MemoryModule::NearestVisibleLivingEntities)) {
                    for (LivingEntity* e : visible->entities) {
                        if (IsVillager(e) && e->IsBaby() && visible->IsVisible(e)) babies.push_back(e);
                    }
                }
                brain->SetMemory(MemoryModule::VisibleVillagerBabies, std::move(babies));
            }
        };

        // MC SecondaryPoiSensor (scan rate 40): the profession's secondary
        // POI blocks (the farmer's farmland) in a 9×5×9 box.
        class SecondaryPoiSensor : public Sensor {
        public:
            SecondaryPoiSensor() : Sensor(40) {}
            std::vector<MemoryModule> Requires() const override { return { MemoryModule::SecondaryJobSite }; }
        protected:
            void DoTick(EntityLevel& level, LivingEntity& body) override {
                Villager& v = V(body);
                Brain* brain = body.GetBrain();
                const IBlockAccess* blocks = level.Blocks();
                if (!brain || !blocks) return;
                const glm::ivec3 center = body.BlockPosition();
                std::vector<glm::ivec3> sites;
                for (int x = -4; x <= 4; ++x) {
                    for (int y = -2; y <= 2; ++y) {
                        for (int z = -4; z <= 4; ++z) {
                            const glm::ivec3 p = center + glm::ivec3(x, y, z);
                            if (ProfessionHasSecondaryPoi(v.GetVillagerData().profession,
                                                          blocks->GetBlock(p.x, p.y, p.z))) {
                                sites.push_back(p);
                            }
                        }
                    }
                }
                if (!sites.empty()) {
                    v.SetSecondaryJobSites(std::move(sites));
                    // The positions live on the villager (no position-list
                    // memory kind); the memory marks their presence.
                    brain->SetMemory(MemoryModule::SecondaryJobSite, std::vector<Entity*>{});
                } else {
                    v.SetSecondaryJobSites({});
                    brain->EraseMemory(MemoryModule::SecondaryJobSite);
                }
            }
        };

        // MC GolemSensor (scan rate 200).
        class GolemSensor : public Sensor {
        public:
            GolemSensor() : Sensor(200) {}
            std::vector<MemoryModule> Requires() const override {
                return { MemoryModule::NearestLivingEntities, MemoryModule::GolemDetectedRecently };
            }
        protected:
            void DoTick(EntityLevel&, LivingEntity& body) override {
                Brain* brain = body.GetBrain();
                if (!brain) return;
                const std::vector<Entity*>* living = brain->GetEntityList(MemoryModule::NearestLivingEntities);
                if (!living) return;
                for (Entity* e : *living) {
                    if (e && e->GetType() == EntityTypeId::IronGolem && !e->IsPlayer()) {
                        brain->SetMemoryWithExpiry(MemoryModule::GolemDetectedRecently, true, 599);
                        return;
                    }
                }
            }
        };

        // MC NearestBedSensor (scan rate 20): a baby's reachable bed.
        class NearestBedSensor : public Sensor {
        public:
            NearestBedSensor() : Sensor(20) {}
            std::vector<MemoryModule> Requires() const override { return { MemoryModule::NearestBed }; }
        protected:
            void DoTick(EntityLevel& level, LivingEntity& body) override {
                if (!body.IsBaby()) return;
                PoiManager* poi = Poi(level);
                auto* mob = dynamic_cast<Mob*>(&body);
                if (!poi || !mob) return;
                m_triedCount = 0;
                m_lastUpdate = level.GetGameTime() + level.Random().NextInt(20);
                const auto cacheTest = [this](const glm::ivec3& pos) {
                    const int64_t key = PosKey(pos);
                    if (m_batchCache.count(key)) return false;
                    if (++m_triedCount >= 5) return false;
                    m_batchCache[key] = m_lastUpdate + 40;
                    return true;
                };
                const auto pois = poi->FindAllWithType([](PoiType t) { return t == PoiType::Home; },
                                                       cacheTest, body.BlockPosition(), 48,
                                                       PoiManager::Occupancy::Any);
                const std::optional<Path> path = FindPathToPois(*mob, pois);
                if (path && path->CanReach()) {
                    const glm::ivec3 target = path->GetTarget();
                    if (poi->GetType(target)) {
                        if (Brain* b = body.GetBrain()) b->SetMemory(MemoryModule::NearestBed, target);
                    }
                } else if (m_triedCount < 5) {
                    for (auto it = m_batchCache.begin(); it != m_batchCache.end();) {
                        if (it->second < m_lastUpdate) it = m_batchCache.erase(it);
                        else ++it;
                    }
                }
            }
        private:
            std::unordered_map<int64_t, int64_t> m_batchCache;
            int     m_triedCount = 0;
            int64_t m_lastUpdate = 0;
        };

        // MC NearestItemSensor: the nearest dropped item the villager wants,
        // within 32 blocks and in sight. Item entities are not Entities here,
        // so the answer goes on the villager (SetWantedItemId).
        class NearestItemSensor : public Sensor {
        public:
            std::vector<MemoryModule> Requires() const override { return { MemoryModule::NearestVisibleWantedItem }; }
        protected:
            static bool ClearSight(EntityLevel& level, const glm::dvec3& from, const glm::dvec3& to) {
                // The same quarter-block DDA as Sensing::ComputeLineOfSight.
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
                Villager& v = V(body);
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
                    if (!stack || !v.WantsToPickUp(*stack)) continue;
                    const glm::dvec3 d = item.pos - body.position;
                    if (glm::dot(d, d) >= 32.0 * 32.0) continue;
                    // ItemEntity eye height: 0.25 * 0.85.
                    if (!ClearSight(level, body.GetEyePosition(), item.pos + glm::dvec3(0.0, 0.2125, 0.0))) continue;
                    found = item.id;
                    break;
                }
                v.SetWantedItemId(found);
            }
        };

        // ═════════════════════════════════════════════════════════════════
        // CORE
        // ═════════════════════════════════════════════════════════════════

        // MC InteractWithDoor.
        class InteractWithDoor : public Behavior {
        public:
            InteractWithDoor()
                : Behavior({ { MemoryModule::DoorsToClose, MemoryStatus::Registered },
                             { MemoryModule::NearestLivingEntities, MemoryStatus::Registered } }, 1) {}
            const char* DebugString() const override { return "InteractWithDoor"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                Villager& v = V(body);
                // i.present(PATH): the navigation's live path.
                const Path* path = v.GetNavigation().GetPath();
                if (!path || path->GetNextNodeIndex() <= 0 || path->IsDone()) return false;
                const Node& nextNode = path->GetNextNode();
                const glm::ivec3 toPos(nextNode.x, nextNode.y, nextNode.z);
                if (m_lastCheckedNode && *m_lastCheckedNode == toPos) {
                    m_remainingCooldown = 20;
                } else if (--m_remainingCooldown > 0) {
                    return false;
                }
                m_lastCheckedNode = toPos;
                const Node& fromNode = path->GetNode(path->GetNextNodeIndex() - 1);
                const glm::ivec3 fromPos(fromNode.x, fromNode.y, fromNode.z);

                const BlockState fromState = StateAt(level, fromPos);
                if (IsMobInteractableDoor(fromState)) {
                    if (!IsDoorOpen(fromState)) SetDoorOpen(level, fromPos, true);
                    RememberDoorToClose(v, fromPos);
                }
                const BlockState toState = StateAt(level, toPos);
                if (IsMobInteractableDoor(toState) && !IsDoorOpen(toState)) {
                    SetDoorOpen(level, toPos, true);
                    RememberDoorToClose(v, toPos);
                }
                CloseDoorsThatIHaveOpenedOrPassedThrough(level, v, &fromPos, &toPos);
                return true;
            }
        private:
            std::optional<glm::ivec3> m_lastCheckedNode;
            int m_remainingCooldown = 0;
        };

        // MC VillagerPanicTrigger.
        class VillagerPanicTrigger : public Behavior {
        public:
            VillagerPanicTrigger() : Behavior({}) {}
            const char* DebugString() const override { return "VillagerPanicTrigger"; }
        protected:
            static bool IsHurt(LivingEntity& b) { return b.GetBrain()->HasMemoryValue(MemoryModule::HurtBy); }
            static bool HasHostile(LivingEntity& b) { return b.GetBrain()->HasMemoryValue(MemoryModule::NearestHostile); }
            bool CanStillUse(EntityLevel&, LivingEntity& body, int64_t) override {
                return IsHurt(body) || HasHostile(body);
            }
            void Start(EntityLevel&, LivingEntity& body, int64_t) override {
                if (!IsHurt(body) && !HasHostile(body)) return;
                Brain* brain = body.GetBrain();
                if (!brain->IsActive(Activity::Panic)) {
                    // (MC also erases PATH — the navigation's path; dropping
                    // the walk target stops MoveToTargetSink the same tick.)
                    brain->EraseMemory(MemoryModule::WalkTarget);
                    brain->EraseMemory(MemoryModule::LookTarget);
                    brain->EraseMemory(MemoryModule::BreedTarget);
                    brain->EraseMemory(MemoryModule::InteractionTarget);
                }
                brain->SetActiveActivityIfPossible(Activity::Panic);
            }
            void Tick(EntityLevel&, LivingEntity& body, int64_t timestamp) override {
                if (timestamp % 100 == 0) V(body).SpawnGolemIfNeeded(timestamp, 3);
            }
        };

        // MC WakeUp.
        class WakeUp : public Behavior {
        public:
            WakeUp() : Behavior({}, 1) {}
            const char* DebugString() const override { return "WakeUp"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                Villager& v = V(body);
                if (!body.GetBrain()->IsActive(Activity::Rest) && v.IsSleeping()) {
                    v.StopSleeping();
                    return true;
                }
                return false;
            }
        };

        // MC ReactToBell — no raid can be in progress, so HIDE.
        class ReactToBell : public Behavior {
        public:
            ReactToBell() : Behavior({ { MemoryModule::HeardBellTime, MemoryStatus::ValuePresent } }, 1) {}
            const char* DebugString() const override { return "ReactToBell"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                body.GetBrain()->SetActiveActivityIfPossible(Activity::Hide);
                return true;
            }
        };

        // MC ValidateNearbyPoi.
        class ValidateNearbyPoi : public Behavior {
        public:
            ValidateNearbyPoi(std::function<bool(PoiType)> pred, MemoryModule memory)
                : Behavior({ { memory, MemoryStatus::ValuePresent } }, 1),
                  m_pred(std::move(pred)), m_memory(memory) {}
            const char* DebugString() const override { return "ValidateNearbyPoi"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                Brain* brain = body.GetBrain();
                const auto pos = brain->GetBlockPos(m_memory);
                if (!pos || !CloserToCenterThan(*pos, body.position, 16.0)) return false;
                PoiManager* poi = Poi(level);
                if (!poi) return false;
                // MC's getOrLoad reads the POI file of an unloaded section;
                // here a section is known once its chunk has been scanned, and
                // until then the claim is not judged.
                if (!poi->IsChunkScanned(pos->x >> 4, pos->z >> 4)) return false;
                if (poi->Exists(*pos, m_pred)) {
                    // bedIsOccupied: someone else lies in the claimed bed.
                    const BlockState bed = StateAt(level, *pos);
                    if (IsDyedBed(bed.Block()) && IsBedOccupied(bed) && !V(body).IsSleeping()) {
                        brain->EraseMemory(m_memory);
                        if (!BedIsOccupiedByVillager(level, *pos)) poi->Release(*pos, body.GetUuid());
                    }
                } else {
                    brain->EraseMemory(m_memory);
                }
                return true;
            }
        private:
            static bool BedIsOccupiedByVillager(EntityLevel& level, const glm::ivec3& pos) {
                AABB box(glm::vec3(pos.x + 0.5f, pos.y + 0.5f, pos.z + 0.5f), glm::vec3(1.0f));
                std::vector<Entity*> found;
                level.GetEntitiesInBox(box, nullptr, found);
                for (Entity* e : found) {
                    if (IsVillager(e) && static_cast<Villager*>(e)->IsSleeping()) return true;
                }
                return false;
            }
            std::function<bool(PoiType)> m_pred;
            MemoryModule m_memory;
        };

        // MC PoiCompetitorScan.
        class PoiCompetitorScan : public Behavior {
        public:
            PoiCompetitorScan()
                : Behavior({ { MemoryModule::JobSite, MemoryStatus::ValuePresent },
                             { MemoryModule::NearestLivingEntities, MemoryStatus::ValuePresent } }, 1) {}
            const char* DebugString() const override { return "PoiCompetitorScan"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                Brain* brain = body.GetBrain();
                const auto pos = brain->GetBlockPos(MemoryModule::JobSite);
                PoiManager* poi = Poi(level);
                if (!pos || !poi) return true;
                const auto type = poi->GetType(*pos);
                if (!type) return true;
                const std::vector<Entity*>* nearest = brain->GetEntityList(MemoryModule::NearestLivingEntities);
                if (!nearest) return true;
                Villager* winner = &V(body);
                for (Entity* e : *nearest) {
                    if (!IsVillager(e) || e == &body) continue;
                    auto* other = static_cast<Villager*>(e);
                    if (!other->IsAlive()) continue;
                    // competesForSameJobsite.
                    const Brain* ob = other->GetBrain();
                    const auto otherSite = ob ? ob->GetBlockPos(MemoryModule::JobSite) : std::nullopt;
                    if (!otherSite || *otherSite != *pos ||
                        !ProfessionHoldsJobSite(other->GetVillagerData().profession, *type)) {
                        continue;
                    }
                    // selectWinner: the higher merchant XP keeps the site.
                    Villager* w;
                    Villager* loser;
                    if (winner->GetVillagerXp() > other->GetVillagerXp()) { w = winner; loser = other; }
                    else { w = other; loser = winner; }
                    if (Brain* lb = loser->GetBrain()) lb->EraseMemory(MemoryModule::JobSite);
                    winner = w;
                }
                return true;
            }
        };

        // MC LookAndFollowTradingPlayerSink.
        class LookAndFollowTradingPlayerSink : public Behavior {
        public:
            explicit LookAndFollowTradingPlayerSink(float speed)
                : Behavior({ { MemoryModule::WalkTarget, MemoryStatus::Registered },
                             { MemoryModule::LookTarget, MemoryStatus::Registered } }, kNoTimeout),
                  m_speed(speed) {}
            const char* DebugString() const override { return "LookAndFollowTradingPlayerSink"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                Villager& v = V(body);
                LivingEntity* player = v.GetTradingPlayer();
                return v.IsAlive() && player && !v.IsInWater() && v.hurtTime <= 0 &&
                       v.DistanceToSqr(*player) <= 16.0;
            }
            bool CanStillUse(EntityLevel& level, LivingEntity& body, int64_t) override {
                return CheckExtraStartConditions(level, body);
            }
            void Start(EntityLevel&, LivingEntity& body, int64_t) override { Follow(body); }
            void Tick(EntityLevel&, LivingEntity& body, int64_t) override { Follow(body); }
            void Stop(EntityLevel&, LivingEntity& body, int64_t) override {
                body.GetBrain()->EraseMemory(MemoryModule::WalkTarget);
                body.GetBrain()->EraseMemory(MemoryModule::LookTarget);
            }
        private:
            void Follow(LivingEntity& body) {
                LivingEntity* player = V(body).GetTradingPlayer();
                if (!player) return;
                Brain* brain = body.GetBrain();
                brain->SetMemory(MemoryModule::WalkTarget,
                                 WalkTarget(PositionTracker::OfEntity(player, false), m_speed, 2));
                brain->SetMemory(MemoryModule::LookTarget, PositionTracker::OfEntity(player, true));
            }
            float m_speed;
        };

        // MC GoToWantedItem.create(speed, false, 4) over the villager-held
        // wanted item (see NearestItemSensor).
        class GoToWantedItem : public Behavior {
        public:
            GoToWantedItem(float speed, int maxDist)
                : Behavior({ { MemoryModule::LookTarget, MemoryStatus::Registered },
                             { MemoryModule::WalkTarget, MemoryStatus::ValueAbsent } }, 1),
                  m_speed(speed), m_maxDist(maxDist) {}
            const char* DebugString() const override { return "GoToWantedItem"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                Villager& v = V(body);
                const auto id = v.GetWantedItemId();
                if (!id || !v.CanPickUpLoot()) return false;
                if (body.GetBrain()->HasMemoryValue(MemoryModule::ItemPickupCooldownTicks)) return false;
                // Where the item is now.
                AABBd box = body.GetAABBd();
                box.min -= glm::dvec3(m_maxDist + 1.0);
                box.max += glm::dvec3(m_maxDist + 1.0);
                std::vector<EntityLevel::NearbyItemEntity> items;
                level.GetItemEntitiesInBox(box, items);
                for (const auto& item : items) {
                    if (item.id != *id) continue;
                    const glm::dvec3 d = item.pos - body.position;
                    if (glm::dot(d, d) >= static_cast<double>(m_maxDist) * m_maxDist) return false;
                    const glm::ivec3 cell = Containing(item.pos);
                    Brain* brain = body.GetBrain();
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

        // MC AcquirePoi's JitteredLinearRetry.
        struct JitteredLinearRetry {
            int64_t previousAttemptTimestamp = 0;
            int64_t nextScheduledAttemptTimestamp = 0;
            int     currentDelay = 0;
            JitteredLinearRetry(JavaRandom& r, int64_t first) { MarkAttempt(r, first); }
            void MarkAttempt(JavaRandom& r, int64_t ts) {
                previousAttemptTimestamp = ts;
                const int suggested = currentDelay + r.NextInt(40) + 40;
                currentDelay = std::min(suggested, 400);
                nextScheduledAttemptTimestamp = ts + currentDelay;
            }
            bool IsStillValid(int64_t ts) const { return ts - previousAttemptTimestamp < 400; }
            bool ShouldRetry(int64_t ts) const { return ts >= nextScheduledAttemptTimestamp; }
        };

        // MC AcquirePoi.
        class AcquirePoi : public Behavior {
        public:
            using ValidPoi = std::function<bool(EntityLevel&, const glm::ivec3&)>;
            AcquirePoi(std::function<bool(PoiType)> pred, MemoryModule toValidate, MemoryModule toAcquire,
                       bool onlyIfAdult, std::optional<uint8_t> event, ValidPoi validPoi)
                : Behavior(Conditions(toValidate, toAcquire), 1),
                  m_pred(std::move(pred)), m_toAcquire(toAcquire), m_onlyIfAdult(onlyIfAdult),
                  m_event(event), m_validPoi(std::move(validPoi)) {}
            const char* DebugString() const override { return "AcquirePoi"; }
        protected:
            static std::vector<MemoryCondition> Conditions(MemoryModule validate, MemoryModule acquire) {
                std::vector<MemoryCondition> c{ { acquire, MemoryStatus::ValueAbsent } };
                if (validate != acquire) c.push_back({ validate, MemoryStatus::ValueAbsent });
                return c;
            }
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                if (m_onlyIfAdult && body.IsBaby()) return false;
                JavaRandom& random = level.Random();
                const int64_t timestamp = level.GetGameTime();
                if (m_nextScheduledStart == 0) {
                    m_nextScheduledStart = level.GetGameTime() + random.NextInt(20);
                    return false;
                }
                if (level.GetGameTime() < m_nextScheduledStart) return false;
                m_nextScheduledStart = timestamp + 20 + random.NextInt(20);
                PoiManager* poi = Poi(level);
                auto* mob = dynamic_cast<Mob*>(&body);
                if (!poi || !mob) return true;

                for (auto it = m_batchCache.begin(); it != m_batchCache.end();) {
                    if (!it->second.IsStillValid(timestamp)) it = m_batchCache.erase(it);
                    else ++it;
                }
                const auto cacheTest = [&](const glm::ivec3& pos) {
                    auto it = m_batchCache.find(PosKey(pos));
                    if (it == m_batchCache.end()) return true;
                    if (!it->second.ShouldRetry(timestamp)) return false;
                    it->second.MarkAttempt(random, timestamp);
                    return true;
                };
                auto candidates = poi->FindAllClosestFirstWithType(m_pred, cacheTest, body.BlockPosition(), 48,
                                                                   PoiManager::Occupancy::HasSpace);
                if (candidates.size() > 5) candidates.resize(5);   // .limit(5)
                std::vector<std::pair<PoiType, glm::ivec3>> pois;
                for (const auto& c : candidates) {
                    if (!m_validPoi || m_validPoi(level, c.second)) pois.push_back(c);
                }
                const std::optional<Path> path = FindPathToPois(*mob, pois);
                if (path && path->CanReach()) {
                    const glm::ivec3 target = path->GetTarget();
                    if (poi->GetType(target)) {
                        poi->Take(m_pred, [target](PoiType, const glm::ivec3& p) { return p == target; },
                                  target, 1, body.GetUuid());
                        body.GetBrain()->SetMemory(m_toAcquire, target);
                        if (m_event) level.BroadcastEntityEvent(body, *m_event);
                        m_batchCache.clear();
                    }
                } else {
                    for (const auto& p : pois) {
                        m_batchCache.try_emplace(PosKey(p.second), random, timestamp);
                    }
                }
                return true;
            }
        private:
            std::function<bool(PoiType)> m_pred;
            MemoryModule m_toAcquire;
            bool m_onlyIfAdult;
            std::optional<uint8_t> m_event;
            ValidPoi m_validPoi;
            int64_t m_nextScheduledStart = 0;
            std::unordered_map<int64_t, JitteredLinearRetry> m_batchCache;
        };

        // MC GoToPotentialJobSite.
        class GoToPotentialJobSite : public Behavior {
        public:
            explicit GoToPotentialJobSite(float speed)
                : Behavior({ { MemoryModule::PotentialJobSite, MemoryStatus::ValuePresent } }, 1200),
                  m_speed(speed) {}
            const char* DebugString() const override { return "GoToPotentialJobSite"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                const auto a = body.GetBrain()->GetActiveNonCoreActivity();
                return !a || *a == Activity::Idle || *a == Activity::Work || *a == Activity::Play;
            }
            bool CanStillUse(EntityLevel&, LivingEntity& body, int64_t) override {
                return body.GetBrain()->HasMemoryValue(MemoryModule::PotentialJobSite);
            }
            void Tick(EntityLevel&, LivingEntity& body, int64_t) override {
                if (const auto pos = body.GetBrain()->GetBlockPos(MemoryModule::PotentialJobSite)) {
                    SetWalkAndLookTargetMemories(body, PositionTracker::OfBlock(*pos), m_speed, 1);
                }
            }
            void Stop(EntityLevel& level, LivingEntity& body, int64_t) override {
                Brain* brain = body.GetBrain();
                if (const auto pos = brain->GetBlockPos(MemoryModule::PotentialJobSite)) {
                    PoiManager* poi = Poi(level);
                    if (poi && poi->Exists(*pos, {})) poi->Release(*pos, body.GetUuid());
                }
                brain->EraseMemory(MemoryModule::PotentialJobSite);
            }
        private:
            float m_speed;
        };

        // MC YieldJobSite.
        class YieldJobSite : public Behavior {
        public:
            explicit YieldJobSite(float speed)
                : Behavior({ { MemoryModule::PotentialJobSite, MemoryStatus::ValuePresent },
                             { MemoryModule::JobSite, MemoryStatus::ValueAbsent },
                             { MemoryModule::NearestLivingEntities, MemoryStatus::ValuePresent },
                             { MemoryModule::WalkTarget, MemoryStatus::Registered },
                             { MemoryModule::LookTarget, MemoryStatus::Registered } }, 1),
                  m_speed(speed) {}
            const char* DebugString() const override { return "YieldJobSite"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                Villager& v = V(body);
                if (v.IsBaby()) return false;
                if (v.GetVillagerData().profession != VillagerProfession::None) return false;
                Brain* brain = body.GetBrain();
                const glm::ivec3 poiPos = *brain->GetBlockPos(MemoryModule::PotentialJobSite);
                PoiManager* poi = Poi(level);
                const auto type = poi ? poi->GetType(poiPos) : std::nullopt;
                if (!type) return true;
                const std::vector<Entity*>* nearest = brain->GetEntityList(MemoryModule::NearestLivingEntities);
                if (!nearest) return true;
                for (Entity* e : *nearest) {
                    if (!IsVillager(e) || e == &body) continue;
                    auto* other = static_cast<Villager*>(e);
                    if (!other->IsAlive() || !NearbyWantsJobsite(*type, *other, poiPos)) continue;
                    brain->EraseMemory(MemoryModule::WalkTarget);
                    brain->EraseMemory(MemoryModule::LookTarget);
                    brain->EraseMemory(MemoryModule::PotentialJobSite);
                    Brain* ob = other->GetBrain();
                    if (ob && !ob->HasMemoryValue(MemoryModule::JobSite)) {
                        SetWalkAndLookTargetMemories(*other, PositionTracker::OfBlock(poiPos), m_speed, 1);
                        ob->SetMemory(MemoryModule::PotentialJobSite, poiPos);
                        poi->TransferHolder(poiPos, body.GetUuid(), other->GetUuid());
                    }
                    break;   // findFirst
                }
                return true;
            }
        private:
            static bool NearbyWantsJobsite(PoiType type, Villager& nearby, const glm::ivec3& poiPos) {
                Brain* ob = nearby.GetBrain();
                if (!ob || ob->HasMemoryValue(MemoryModule::PotentialJobSite)) return false;
                if (!ProfessionHoldsJobSite(nearby.GetVillagerData().profession, type)) return false;
                const auto jobSite = ob->GetBlockPos(MemoryModule::JobSite);
                if (!jobSite) {
                    // canReachPos.
                    const std::optional<Path> path = nearby.GetNavigation().CreatePath(poiPos, PoiValidRange(type));
                    return path && path->CanReach();
                }
                return *jobSite == poiPos;
            }
            float m_speed;
        };

        // MC AssignProfessionFromJobSite.
        class AssignProfessionFromJobSite : public Behavior {
        public:
            AssignProfessionFromJobSite()
                : Behavior({ { MemoryModule::PotentialJobSite, MemoryStatus::ValuePresent },
                             { MemoryModule::JobSite, MemoryStatus::Registered } }, 1) {}
            const char* DebugString() const override { return "AssignProfessionFromJobSite"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                Villager& v = V(body);
                Brain* brain = body.GetBrain();
                const glm::ivec3 pos = *brain->GetBlockPos(MemoryModule::PotentialJobSite);
                if (!CloserToCenterThan(pos, body.position, 2.0)) return false;
                brain->EraseMemory(MemoryModule::PotentialJobSite);
                brain->SetMemory(MemoryModule::JobSite, pos);
                level.BroadcastEntityEvent(body, 14);
                if (v.GetVillagerData().profession != VillagerProfession::None) return true;
                PoiManager* poi = Poi(level);
                const auto type = poi ? poi->GetType(pos) : std::nullopt;
                if (type) {
                    if (const auto profession = ProfessionForJobSite(*type)) {
                        v.SetVillagerData(v.GetVillagerData().WithProfession(*profession));
                        v.RequestBrainRefresh();
                    }
                }
                return true;
            }
        };

        // MC ResetProfession.
        class ResetProfession : public Behavior {
        public:
            ResetProfession() : Behavior({ { MemoryModule::JobSite, MemoryStatus::ValueAbsent } }, 1) {}
            const char* DebugString() const override { return "ResetProfession"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                Villager& v = V(body);
                const VillagerData& d = v.GetVillagerData();
                const bool canBeFired = d.profession != VillagerProfession::None &&
                                        d.profession != VillagerProfession::Nitwit;
                if (canBeFired && v.GetVillagerXp() == 0 && d.level <= 1) {
                    v.SetVillagerData(d.WithProfession(VillagerProfession::None));
                    v.RequestBrainRefresh();
                    return true;
                }
                return false;
            }
        };

        // ═════════════════════════════════════════════════════════════════
        // WORK
        // ═════════════════════════════════════════════════════════════════

        // MC WorkAtPoi / WorkAtComposter.
        class WorkAtPoi : public Behavior {
        public:
            explicit WorkAtPoi(bool composter)
                : Behavior({ { MemoryModule::JobSite, MemoryStatus::ValuePresent },
                             { MemoryModule::LookTarget, MemoryStatus::Registered } }),
                  m_composter(composter) {}
            const char* DebugString() const override { return m_composter ? "WorkAtComposter" : "WorkAtPoi"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                if (level.GetGameTime() - m_lastCheck < 300) return false;
                if (level.Random().NextInt(2) != 0) return false;
                m_lastCheck = level.GetGameTime();
                const auto target = body.GetBrain()->GetBlockPos(MemoryModule::JobSite);
                return target && CloserToCenterThan(*target, body.position, 1.73);
            }
            void Start(EntityLevel& level, LivingEntity& body, int64_t timestamp) override {
                Villager& v = V(body);
                Brain* brain = body.GetBrain();
                brain->SetMemory(MemoryModule::LastWorkedAtPoi, timestamp);
                if (const auto site = brain->GetBlockPos(MemoryModule::JobSite)) {
                    brain->SetMemory(MemoryModule::LookTarget, PositionTracker::OfBlock(*site));
                }
                v.PlayWorkSound();
                if (m_composter) UseComposter(level, v);
                if (v.ShouldRestock()) v.Restock();
            }
            bool CanStillUse(EntityLevel&, LivingEntity& body, int64_t) override {
                const auto site = body.GetBrain()->GetBlockPos(MemoryModule::JobSite);
                return site && CloserToCenterThan(*site, body.position, 1.73);
            }
        private:
            // MC WorkAtComposter.useWorkstation.
            static void UseComposter(EntityLevel& level, Villager& v) {
                const auto site = v.GetBrain()->GetBlockPos(MemoryModule::JobSite);
                if (!site) return;
                const BlockState state = StateAt(level, *site);
                if (state.Block() != BlockID::Composter) return;
                MakeBread(level, v);
                CompostItems(level, v, *site, state);
            }
            static void MakeBread(EntityLevel& level, Villager& v) {
                if (v.CountInventoryItem(Items::Bread) > 36) return;
                const int wheat = v.CountInventoryItem(Items::Wheat);
                const int breadToMake = std::min(3, wheat / 3);
                if (breadToMake == 0) return;
                v.RemoveInventoryItemType(Items::Wheat, breadToMake * 3);
                const ItemStack cantCarry = v.AddToInventory(ItemStack(Items::Bread, breadToMake));
                if (!cantCarry.IsEmpty()) {
                    level.SpawnItemStackDrop(v.position + glm::dvec3(0.0, 0.5, 0.0), cantCarry);
                }
            }
            static int ComposterLevel(BlockState s) { return ReadIntProperty(s, "level"); }
            static void CompostItems(EntityLevel& level, Villager& v, const glm::ivec3& pos, BlockState state) {
                ILevelWrite* world = level.MutableBlocks();
                if (!world) return;
                if (ComposterLevel(state) == 8) {
                    // ComposterBlock.extractProduce: bone meal pops out above.
                    JavaRandom& r = level.Random();
                    const double ox = (r.NextFloat() * 0.7f) - 0.35f;   // offsetRandomXZ(0.7)
                    const double oz = (r.NextFloat() * 0.7f) - 0.35f;
                    level.SpawnThrownItem(glm::dvec3(pos.x + 0.5 + ox, pos.y + 1.01, pos.z + 0.5 + oz),
                                          glm::dvec3(0.0), ItemStack(Items::BoneMeal, 1), 10);
                    state = state.SetName(PropertyId::LEVEL_COMPOSTER, "0");
                    world->SetBlock(pos.x, pos.y, pos.z, state, World::UpdateFlags::All);
                    level.PlaySound(nullptr, pos, SoundEvents::COMPOSTER_EMPTY, SoundSource::Blocks, 1.0f, 1.0f);
                }
                // MC: up to 20 seeds, keeping 10 of each kind back.
                static const ItemID kCompostable[2] = { Items::WheatSeeds, Items::BeetrootSeeds };
                int totalItemsToUse = 20;
                int seen[2] = { 0, 0 };
                SimpleContainer& inventory = v.GetInventory();
                const BlockState before = state;
                BlockState temp = state;
                bool filled = false;
                for (int i = inventory.GetContainerSize() - 1; i >= 0 && totalItemsToUse > 0 && !filled; --i) {
                    ItemStack& stack = inventory.GetItem(i);
                    int idx = -1;
                    for (int k = 0; k < 2; ++k) if (stack.itemId == kCompostable[k]) idx = k;
                    if (idx < 0 || stack.IsEmpty()) continue;
                    const int stackSize = stack.count;
                    seen[idx] += stackSize;
                    const int itemsToUse = std::min(std::min(seen[idx] - 10, totalItemsToUse), stackSize);
                    if (itemsToUse <= 0) continue;
                    totalItemsToUse -= itemsToUse;
                    for (int j = 0; j < itemsToUse; ++j) {
                        // ComposterBlock.insertItem: below 7, a layer at the
                        // compostable's chance (the seeds' COMPOSTABLE_LOW,
                        // 0.3), and the item is spent either way.
                        const int fill = ComposterLevel(temp);
                        if (fill < 7) {
                            if (level.Random().NextFloat() < 0.3f) {
                                temp = temp.SetName(PropertyId::LEVEL_COMPOSTER, std::to_string(fill + 1));
                                world->SetBlock(pos.x, pos.y, pos.z, temp, World::UpdateFlags::All);
                            }
                            stack.count -= 1;
                            if (stack.count <= 0) stack.Clear();
                        }
                        if (ComposterLevel(temp) == 7) { filled = true; break; }
                    }
                }
                // Level event 1500 (ComposterBlock.handleFill): the fill sound.
                level.PlaySound(nullptr, pos,
                                temp != before ? SoundEvents::COMPOSTER_FILL_SUCCESS : SoundEvents::COMPOSTER_FILL,
                                SoundSource::Blocks, 1.0f, 1.0f);
            }
            bool    m_composter;
            int64_t m_lastCheck = 0;
        };

        // MC StrollAroundPoi.
        class StrollAroundPoi : public Behavior {
        public:
            StrollAroundPoi(MemoryModule memory, float speed, int maxDistanceFromPoi)
                : Behavior({ { MemoryModule::WalkTarget, MemoryStatus::Registered },
                             { memory, MemoryStatus::ValuePresent } }, 1),
                  m_memory(memory), m_speed(speed), m_maxDist(maxDistanceFromPoi) {}
            const char* DebugString() const override { return "StrollAroundPoi"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                Brain* brain = body.GetBrain();
                const auto pos = brain->GetBlockPos(m_memory);
                if (!pos || !CloserToCenterThan(*pos, body.position, m_maxDist)) return false;
                const int64_t timestamp = level.GetGameTime();
                if (timestamp <= m_nextOkStartTime) return true;
                const auto land = RandomPos::GetLandPos(V(body), 8, 6);
                if (land) brain->SetMemory(MemoryModule::WalkTarget, WalkTarget(Containing(*land), m_speed, 1));
                else brain->EraseMemory(MemoryModule::WalkTarget);
                m_nextOkStartTime = timestamp + 180;
                return true;
            }
        private:
            MemoryModule m_memory;
            float   m_speed;
            int     m_maxDist;
            int64_t m_nextOkStartTime = 0;
        };

        // MC StrollToPoi.
        class StrollToPoi : public Behavior {
        public:
            StrollToPoi(MemoryModule memory, float speed, int closeEnough, int maxDistanceFromPoi)
                : Behavior({ { MemoryModule::WalkTarget, MemoryStatus::Registered },
                             { memory, MemoryStatus::ValuePresent } }, 1),
                  m_memory(memory), m_speed(speed), m_close(closeEnough), m_maxDist(maxDistanceFromPoi) {}
            const char* DebugString() const override { return "StrollToPoi"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                Brain* brain = body.GetBrain();
                const auto pos = brain->GetBlockPos(m_memory);
                if (!pos || !CloserToCenterThan(*pos, body.position, m_maxDist)) return false;
                const int64_t timestamp = level.GetGameTime();
                if (timestamp <= m_nextOkStartTime) return true;
                brain->SetMemory(MemoryModule::WalkTarget, WalkTarget(*pos, m_speed, m_close));
                m_nextOkStartTime = timestamp + 80;
                return true;
            }
        private:
            MemoryModule m_memory;
            float   m_speed;
            int     m_close, m_maxDist;
            int64_t m_nextOkStartTime = 0;
        };

        // MC StrollToPoiList(SECONDARY_JOB_SITE, …, JOB_SITE).
        class StrollToPoiList : public Behavior {
        public:
            StrollToPoiList(float speed, int closeEnough, int maxDistanceFromPoi)
                : Behavior({ { MemoryModule::WalkTarget, MemoryStatus::Registered },
                             { MemoryModule::SecondaryJobSite, MemoryStatus::ValuePresent },
                             { MemoryModule::JobSite, MemoryStatus::ValuePresent } }, 1),
                  m_speed(speed), m_close(closeEnough), m_maxDist(maxDistanceFromPoi) {}
            const char* DebugString() const override { return "StrollToPoiList"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                Villager& v = V(body);
                const std::vector<glm::ivec3>& strollTo = v.GetSecondaryJobSites();
                if (strollTo.empty()) return false;
                const glm::ivec3 target = strollTo[static_cast<size_t>(
                    level.Random().NextInt(static_cast<int>(strollTo.size())))];
                const auto stayCloseTo = body.GetBrain()->GetBlockPos(MemoryModule::JobSite);
                if (!stayCloseTo || !CloserToCenterThan(*stayCloseTo, body.position, m_maxDist)) return false;
                const int64_t timestamp = level.GetGameTime();
                if (timestamp > m_nextOkStartTime) {
                    body.GetBrain()->SetMemory(MemoryModule::WalkTarget, WalkTarget(target, m_speed, m_close));
                    m_nextOkStartTime = timestamp + 100;
                }
                return true;
            }
        private:
            float   m_speed;
            int     m_close, m_maxDist;
            int64_t m_nextOkStartTime = 0;
        };

        // MC HarvestFarmland.
        class HarvestFarmland : public Behavior {
        public:
            HarvestFarmland()
                : Behavior({ { MemoryModule::LookTarget, MemoryStatus::ValueAbsent },
                             { MemoryModule::WalkTarget, MemoryStatus::ValueAbsent },
                             { MemoryModule::SecondaryJobSite, MemoryStatus::ValuePresent } }) {}
            const char* DebugString() const override { return "HarvestFarmland"; }
        protected:
            static bool ValidPos(EntityLevel& level, const glm::ivec3& pos) {
                const BlockState state = StateAt(level, pos);
                if (IsCropBlock(state.Block()) && IsMaxAgeCrop(state)) return true;
                const BlockID below = StateAt(level, pos - glm::ivec3(0, 1, 0)).Block();
                return state.Block() == BlockID::Air && below == BlockID::Farmland;
            }
            std::optional<glm::ivec3> GetValidFarmland(EntityLevel& level) {
                if (m_valid.empty()) return std::nullopt;
                return m_valid[static_cast<size_t>(level.Random().NextInt(static_cast<int>(m_valid.size())))];
            }
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                if (!level.MobGriefing()) return false;
                if (V(body).GetVillagerData().profession != VillagerProfession::Farmer) return false;
                m_valid.clear();
                for (int x = -1; x <= 1; ++x) {
                    for (int y = -1; y <= 1; ++y) {
                        for (int z = -1; z <= 1; ++z) {
                            const glm::ivec3 p = Containing(body.position + glm::dvec3(x, y, z));
                            if (ValidPos(level, p)) m_valid.push_back(p);
                        }
                    }
                }
                m_target = GetValidFarmland(level);
                return m_target.has_value();
            }
            void Start(EntityLevel&, LivingEntity& body, int64_t timestamp) override {
                if (timestamp > m_nextOkStartTime && m_target) {
                    Brain* brain = body.GetBrain();
                    brain->SetMemory(MemoryModule::LookTarget, PositionTracker::OfBlock(*m_target));
                    brain->SetMemory(MemoryModule::WalkTarget, WalkTarget(*m_target, 0.5f, 1));
                }
            }
            void Stop(EntityLevel&, LivingEntity& body, int64_t timestamp) override {
                body.GetBrain()->EraseMemory(MemoryModule::LookTarget);
                body.GetBrain()->EraseMemory(MemoryModule::WalkTarget);
                m_timeWorkedSoFar = 0;
                m_nextOkStartTime = timestamp + 40;
            }
            bool CanStillUse(EntityLevel&, LivingEntity&, int64_t) override { return m_timeWorkedSoFar < 200; }
            void Tick(EntityLevel& level, LivingEntity& body, int64_t timestamp) override {
                if (m_target && !CloserToCenterThan(*m_target, body.position, 1.0)) return;
                if (m_target && timestamp > m_nextOkStartTime) {
                    Villager& v = V(body);
                    const glm::ivec3 pos = *m_target;
                    const BlockState state = StateAt(level, pos);
                    const BlockID block = state.Block();
                    const BlockID below = StateAt(level, pos - glm::ivec3(0, 1, 0)).Block();
                    if (IsCropBlock(block) && IsMaxAgeCrop(state)) level.DestroyBlock(pos, true);
                    if (block == BlockID::Air && below == BlockID::Farmland && v.HasFarmSeeds()) {
                        SimpleContainer& inv = v.GetInventory();
                        for (int i = 0; i < inv.GetContainerSize(); ++i) {
                            ItemStack& stack = inv.GetItem(i);
                            if (stack.IsEmpty() || !IsVillagerPlantableSeed(stack.itemId)) continue;
                            const BlockID place = stack.AsBlockID();
                            if (place == BlockID::Air) continue;
                            level.SetBlockState(pos, BlockStates::Default(place));
                            level.PlaySound(nullptr, pos, SoundEvents::CROP_PLANTED, SoundSource::Blocks, 1.0f, 1.0f);
                            stack.count -= 1;
                            if (stack.count <= 0) stack.Clear();
                            break;
                        }
                    }
                    if (IsCropBlock(block) && !IsMaxAgeCrop(state)) {
                        m_valid.erase(std::remove(m_valid.begin(), m_valid.end(), pos), m_valid.end());
                        m_target = GetValidFarmland(level);
                        if (m_target) {
                            m_nextOkStartTime = timestamp + 20;
                            Brain* brain = body.GetBrain();
                            brain->SetMemory(MemoryModule::WalkTarget, WalkTarget(*m_target, 0.5f, 1));
                            brain->SetMemory(MemoryModule::LookTarget, PositionTracker::OfBlock(*m_target));
                        }
                    }
                }
                ++m_timeWorkedSoFar;
            }
        private:
            std::optional<glm::ivec3> m_target;
            int64_t m_nextOkStartTime = 0;
            int     m_timeWorkedSoFar = 0;
            std::vector<glm::ivec3> m_valid;
        };

        // MC UseBonemeal.
        class UseBonemeal : public Behavior {
        public:
            UseBonemeal()
                : Behavior({ { MemoryModule::LookTarget, MemoryStatus::ValueAbsent },
                             { MemoryModule::WalkTarget, MemoryStatus::ValueAbsent } }) {}
            const char* DebugString() const override { return "UseBonemeal"; }
        protected:
            static bool ValidPos(EntityLevel& level, const glm::ivec3& p) {
                const BlockState s = StateAt(level, p);
                return IsCropBlock(s.Block()) && !IsMaxAgeCrop(s);
            }
            std::optional<glm::ivec3> PickNextTarget(EntityLevel& level, LivingEntity& body) {
                std::optional<glm::ivec3> result;
                int count = 0;
                const glm::ivec3 base = body.BlockPosition();
                for (int x = -1; x <= 1; ++x) {
                    for (int y = -1; y <= 1; ++y) {
                        for (int z = -1; z <= 1; ++z) {
                            const glm::ivec3 p = base + glm::ivec3(x, y, z);
                            if (!ValidPos(level, p)) continue;
                            ++count;
                            if (level.Random().NextInt(count) == 0) result = p;
                        }
                    }
                }
                return result;
            }
            void SetCurrentCropAsTarget(LivingEntity& body) {
                if (!m_cropPos) return;
                Brain* brain = body.GetBrain();
                brain->SetMemory(MemoryModule::LookTarget, PositionTracker::OfBlock(*m_cropPos));
                brain->SetMemory(MemoryModule::WalkTarget, WalkTarget(*m_cropPos, 0.5f, 1));
            }
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                if (body.tickCount % 10 != 0 ||
                    (m_lastBonemealingSession != 0 && m_lastBonemealingSession + 160 > body.tickCount)) {
                    return false;
                }
                if (V(body).CountInventoryItem(Items::BoneMeal) <= 0) return false;
                m_cropPos = PickNextTarget(level, body);
                return m_cropPos.has_value();
            }
            bool CanStillUse(EntityLevel&, LivingEntity&, int64_t) override {
                return m_timeWorkedSoFar < 80 && m_cropPos.has_value();
            }
            void Start(EntityLevel&, LivingEntity& body, int64_t timestamp) override {
                SetCurrentCropAsTarget(body);
                // (MC shows the bone meal in the main hand — no mob hand
                // items are drawn in this engine.)
                m_nextWorkCycleTime = timestamp;
                m_timeWorkedSoFar = 0;
            }
            void Stop(EntityLevel&, LivingEntity& body, int64_t) override {
                m_lastBonemealingSession = body.tickCount;
            }
            void Tick(EntityLevel& level, LivingEntity& body, int64_t timestamp) override {
                if (!m_cropPos) return;
                const glm::ivec3 target = *m_cropPos;
                if (timestamp < m_nextWorkCycleTime || !CloserToCenterThan(target, body.position, 1.0)) return;
                SimpleContainer& inv = V(body).GetInventory();
                ItemStack* bonemeal = nullptr;
                for (int i = 0; i < inv.GetContainerSize(); ++i) {
                    if (inv.GetItem(i).itemId == Items::BoneMeal && !inv.GetItem(i).IsEmpty()) {
                        bonemeal = &inv.GetItem(i);
                        break;
                    }
                }
                if (bonemeal && GrowCrop(level, *bonemeal, target)) {
                    PlayLevelEventSound(level, nullptr, LevelEvent::PARTICLES_AND_SOUND_PLANT_GROWTH,
                                        target, 15, &level.Random());
                    m_cropPos = PickNextTarget(level, body);
                    SetCurrentCropAsTarget(body);
                    m_nextWorkCycleTime = timestamp + 40;
                }
                ++m_timeWorkedSoFar;
            }
        private:
            // MC BoneMealItem.growCrop.
            static bool GrowCrop(EntityLevel& level, ItemStack& stack, const glm::ivec3& pos) {
                ILevelWrite* world = level.MutableBlocks();
                if (!world) return false;
                const BlockState state = world->GetBlockState(pos.x, pos.y, pos.z);
                const Block& def = BlockRegistry::Get(state.Block());
                if (!def.performBonemeal || !def.isValidBonemealTarget) return false;
                if (!def.isValidBonemealTarget(*world, pos, state)) return false;
                def.performBonemeal(*world, pos, state, level.Random());
                stack.count -= 1;
                if (stack.count <= 0) stack.Clear();
                return true;
            }
            std::optional<glm::ivec3> m_cropPos;
            int64_t m_nextWorkCycleTime = 0;
            int64_t m_lastBonemealingSession = 0;
            int     m_timeWorkedSoFar = 0;
        };

        // MC ShowTradesToPlayer (400..1600).
        class ShowTradesToPlayer : public Behavior {
        public:
            ShowTradesToPlayer(int minDuration, int maxDuration)
                : Behavior({ { MemoryModule::InteractionTarget, MemoryStatus::ValuePresent } },
                           minDuration, maxDuration) {}
            const char* DebugString() const override { return "ShowTradesToPlayer"; }
        protected:
            static LivingEntity* Target(LivingEntity& body) {
                Entity* e = body.GetBrain()->GetEntity(MemoryModule::InteractionTarget);
                return e ? e->AsLiving() : nullptr;
            }
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                LivingEntity* target = Target(body);
                return target && target->IsPlayer() && body.IsAlive() && target->IsAlive() &&
                       !body.IsBaby() && body.DistanceToSqr(*target) <= 17.0;
            }
            bool CanStillUse(EntityLevel& level, LivingEntity& body, int64_t) override {
                return CheckExtraStartConditions(level, body) && m_lookTime > 0 &&
                       body.GetBrain()->HasMemoryValue(MemoryModule::InteractionTarget);
            }
            void Start(EntityLevel&, LivingEntity& body, int64_t) override {
                LookAt(body);
                m_cycleCounter = 0;
                m_displayIndex = 0;
                m_lookTime = 40;
            }
            void Tick(EntityLevel& level, LivingEntity& body, int64_t) override {
                LivingEntity* target = LookAt(body);
                if (target) FindItemsToDisplay(level, *target, V(body));
                if (!m_displayItems.empty()) {
                    DisplayCyclingItems();
                } else {
                    m_lookTime = std::min(m_lookTime, 40);
                }
                --m_lookTime;
            }
            void Stop(EntityLevel&, LivingEntity& body, int64_t) override {
                body.GetBrain()->EraseMemory(MemoryModule::InteractionTarget);
                m_playerItem.reset();
                m_displayItems.clear();
            }
        private:
            LivingEntity* LookAt(LivingEntity& body) {
                LivingEntity* target = Target(body);
                if (target) body.GetBrain()->SetMemory(MemoryModule::LookTarget, PositionTracker::OfEntity(target, true));
                return target;
            }
            void FindItemsToDisplay(EntityLevel& level, LivingEntity& player, Villager& v) {
                const ItemID current = static_cast<ItemID>(level.GetHeldItemId(player));
                bool changed = false;
                if (!m_playerItem || *m_playerItem != current) {
                    m_playerItem = current;
                    changed = true;
                    m_displayItems.clear();
                }
                if (changed && current != Items::Air) {
                    for (const MerchantOffer& offer : v.GetOffers()) {
                        if (offer.IsOutOfStock()) continue;
                        if (offer.GetCostA().itemId == current || offer.GetCostB().itemId == current) {
                            m_displayItems.push_back(offer.Assemble());
                        }
                    }
                    if (!m_displayItems.empty()) m_lookTime = 900;
                }
            }
            void DisplayCyclingItems() {
                // The cycling is kept (the timing drives lookTime); the item
                // itself would sit in the villager's hand — MC's crossed-arms
                // item layer, which this engine does not draw yet.
                if (m_displayItems.size() >= 2 && ++m_cycleCounter >= 40) {
                    ++m_displayIndex;
                    m_cycleCounter = 0;
                    if (m_displayIndex > static_cast<int>(m_displayItems.size()) - 1) m_displayIndex = 0;
                }
            }
            std::optional<ItemID>  m_playerItem;
            std::vector<ItemStack> m_displayItems;
            int m_cycleCounter = 0, m_displayIndex = 0, m_lookTime = 0;
        };

        // MC SetLookAndInteract(PLAYER, range).
        class SetLookAndInteract : public Behavior {
        public:
            explicit SetLookAndInteract(int range)
                : Behavior({ { MemoryModule::LookTarget, MemoryStatus::Registered },
                             { MemoryModule::InteractionTarget, MemoryStatus::ValueAbsent },
                             { MemoryModule::NearestVisibleLivingEntities, MemoryStatus::ValuePresent } }, 1),
                  m_rangeSqr(range * range) {}
            const char* DebugString() const override { return "SetLookAndInteract"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                Brain* brain = body.GetBrain();
                const NearestVisibleLivingEntities* visible =
                    brain->GetVisibleEntities(MemoryModule::NearestVisibleLivingEntities);
                LivingEntity* closest = visible ? visible->FindClosest([&](LivingEntity* e) {
                    return e->DistanceToSqr(body) <= static_cast<double>(m_rangeSqr) && e->IsPlayer();
                }) : nullptr;
                if (!closest) return false;
                brain->SetMemory(MemoryModule::InteractionTarget, static_cast<Entity*>(closest));
                brain->SetMemory(MemoryModule::LookTarget, PositionTracker::OfEntity(closest, true));
                return true;
            }
        private:
            int m_rangeSqr;
        };

        // MC SetWalkTargetFromBlockMemory.
        class SetWalkTargetFromBlockMemory : public Behavior {
        public:
            SetWalkTargetFromBlockMemory(MemoryModule memory, float speed, int closeEnough, int tooFar,
                                         int tooLongUnreachable)
                : Behavior({ { MemoryModule::CantReachWalkTargetSince, MemoryStatus::Registered },
                             { MemoryModule::WalkTarget, MemoryStatus::ValueAbsent },
                             { memory, MemoryStatus::ValuePresent } }, 1),
                  m_memory(memory), m_speed(speed), m_close(closeEnough), m_tooFar(tooFar),
                  m_tooLong(tooLongUnreachable) {}
            const char* DebugString() const override { return "SetWalkTargetFromBlockMemory"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                Villager& v = V(body);
                Brain* brain = body.GetBrain();
                const glm::ivec3 target = *brain->GetBlockPos(m_memory);
                const auto cantReachSince = brain->GetLong(MemoryModule::CantReachWalkTargetSince);
                const int64_t timestamp = level.GetGameTime();
                if (!cantReachSince || level.GetGameTime() - *cantReachSince <= m_tooLong) {
                    const glm::ivec3 bodyPos = body.BlockPosition();
                    if (DistManhattan(target, bodyPos) > m_tooFar) {
                        std::optional<glm::dvec3> towards;
                        int tries = 0;
                        while (!towards || DistManhattan(Containing(*towards), bodyPos) > m_tooFar) {
                            towards = RandomPos::GetPosTowards(v, 15, 7, BottomCenter(target), kHalfPi);
                            ++tries;
                            if (tries == 1000) {
                                v.ReleasePoi(m_memory);
                                brain->EraseMemory(m_memory);
                                brain->SetMemory(MemoryModule::CantReachWalkTargetSince, timestamp);
                                return true;
                            }
                        }
                        brain->SetMemory(MemoryModule::WalkTarget, WalkTarget(Containing(*towards), m_speed, m_close));
                    } else if (DistManhattan(target, bodyPos) > m_close) {
                        brain->SetMemory(MemoryModule::WalkTarget, WalkTarget(target, m_speed, m_close));
                    }
                } else {
                    v.ReleasePoi(m_memory);
                    brain->EraseMemory(m_memory);
                    brain->SetMemory(MemoryModule::CantReachWalkTargetSince, timestamp);
                }
                return true;
            }
        private:
            MemoryModule m_memory;
            float m_speed;
            int   m_close, m_tooFar, m_tooLong;
        };

        // MC GiveGiftToHero(100).
        class GiveGiftToHero : public Behavior {
        public:
            explicit GiveGiftToHero(int timeout)
                : Behavior({ { MemoryModule::WalkTarget, MemoryStatus::Registered },
                             { MemoryModule::LookTarget, MemoryStatus::Registered },
                             { MemoryModule::InteractionTarget, MemoryStatus::Registered },
                             { MemoryModule::NearestVisiblePlayer, MemoryStatus::ValuePresent } }, timeout) {}
            const char* DebugString() const override { return "GiveGiftToHero"; }
        protected:
            static LivingEntity* Hero(LivingEntity& body) {
                Entity* e = body.GetBrain()->GetEntity(MemoryModule::NearestVisiblePlayer);
                LivingEntity* p = e ? e->AsLiving() : nullptr;
                return p && p->HasEffect(MobEffectId::HeroOfTheVillage) ? p : nullptr;
            }
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                if (!Hero(body)) return false;
                if (m_timeUntilNextGift > 0) { --m_timeUntilNextGift; return false; }
                return true;
            }
            void Start(EntityLevel&, LivingEntity& body, int64_t timestamp) override {
                m_given = false;
                m_timeSinceStart = timestamp;
                LivingEntity* hero = Hero(body);
                if (!hero) return;
                body.GetBrain()->SetMemory(MemoryModule::InteractionTarget, static_cast<Entity*>(hero));
                LookAtEntity(body, *hero);
            }
            bool CanStillUse(EntityLevel&, LivingEntity& body, int64_t) override {
                return Hero(body) != nullptr && !m_given;
            }
            void Tick(EntityLevel& level, LivingEntity& body, int64_t timestamp) override {
                LivingEntity* hero = Hero(body);
                if (!hero) return;
                LookAtEntity(body, *hero);
                if (BlockCloserThan(body.BlockPosition(), hero->BlockPosition(), 5.0)) {
                    if (timestamp - m_timeSinceStart > 20) {
                        ThrowGift(level, V(body), *hero);
                        m_given = true;
                    }
                } else {
                    SetWalkAndLookTargetMemories(body, PositionTracker::OfEntity(hero, true), 0.5f, 5);
                }
            }
            void Stop(EntityLevel& level, LivingEntity& body, int64_t) override {
                m_timeUntilNextGift = 600 + level.Random().NextInt(6001);
                Brain* brain = body.GetBrain();
                brain->EraseMemory(MemoryModule::InteractionTarget);
                brain->EraseMemory(MemoryModule::WalkTarget);
                brain->EraseMemory(MemoryModule::LookTarget);
            }
        private:
            static void ThrowGift(EntityLevel& level, Villager& v, LivingEntity& target) {
                // MC getLootTableToThrow: the baby's, the profession's, else
                // the unemployed one — gameplay/hero_of_the_village/*.
                std::string table;
                const VillagerProfession p = v.GetVillagerData().profession;
                if (v.IsBaby()) table = "minecraft:gameplay/hero_of_the_village/baby_gift";
                else if (p == VillagerProfession::None || p == VillagerProfession::Nitwit)
                    table = "minecraft:gameplay/hero_of_the_village/unemployed_gift";
                else table = "minecraft:gameplay/hero_of_the_village/" +
                             std::string(VillagerProfessionId(p)) + "_gift";
                std::vector<ItemStack> items;
                if (!ChestLoot::GetRandomItems(table, level.Random(), 0.0f, items)) return;
                for (const ItemStack& item : items) ThrowItem(v, item, target.position);
            }
            int     m_timeUntilNextGift = 600;
            bool    m_given = false;
            int64_t m_timeSinceStart = 0;
        };

        // MC UpdateActivityFromSchedule.
        class UpdateActivityFromSchedule : public Behavior {
        public:
            UpdateActivityFromSchedule() : Behavior({}, 1) {}
            const char* DebugString() const override { return "UpdateActivityFromSchedule"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                V(body).UpdateActivityFromSchedule();
                return true;
            }
        };

        // ═════════════════════════════════════════════════════════════════
        // PLAY / IDLE
        // ═════════════════════════════════════════════════════════════════

        // MC InteractWith.of(type, range, selfFilter, targetFilter, memory,
        // speed, stopDistance) — the general form (the engine's shared
        // InteractWith is the INTERACTION_TARGET-only shape).
        class VillagerInteractWith : public Behavior {
        public:
            using Filter = std::function<bool(LivingEntity&)>;
            VillagerInteractWith(EntityTypeId type, int range, Filter selfFilter, Filter targetFilter,
                                 MemoryModule memory, float speed, int stopDistance)
                : Behavior({ { memory, MemoryStatus::Registered },
                             { MemoryModule::LookTarget, MemoryStatus::Registered },
                             { MemoryModule::WalkTarget, MemoryStatus::ValueAbsent },
                             { MemoryModule::NearestVisibleLivingEntities, MemoryStatus::ValuePresent } }, 1),
                  m_type(type), m_rangeSqr(range * range), m_self(std::move(selfFilter)),
                  m_target(std::move(targetFilter)), m_memory(memory), m_speed(speed), m_stop(stopDistance) {}
            const char* DebugString() const override { return "InteractWith"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                if (m_self && !m_self(body)) return false;
                Brain* brain = body.GetBrain();
                const NearestVisibleLivingEntities* visible =
                    brain->GetVisibleEntities(MemoryModule::NearestVisibleLivingEntities);
                LivingEntity* t = visible ? visible->FindClosest([&](LivingEntity* e) {
                    return e->DistanceToSqr(body) <= static_cast<double>(m_rangeSqr) && !e->IsPlayer() &&
                           e->GetType() == m_type && (!m_target || m_target(*e));
                }) : nullptr;
                if (!t) return false;
                brain->SetMemory(m_memory, static_cast<Entity*>(t));
                brain->SetMemory(MemoryModule::LookTarget, PositionTracker::OfEntity(t, true));
                brain->SetMemory(MemoryModule::WalkTarget,
                                 WalkTarget(PositionTracker::OfEntity(t, false), m_speed, m_stop));
                return true;
            }
        private:
            EntityTypeId m_type;
            int          m_rangeSqr;
            Filter       m_self, m_target;
            MemoryModule m_memory;
            float        m_speed;
            int          m_stop;
        };

        // MC VillageBoundRandomStroll.
        class VillageBoundRandomStroll : public Behavior {
        public:
            VillageBoundRandomStroll(float speed, int maxXz = 10, int maxY = 7)
                : Behavior({ { MemoryModule::WalkTarget, MemoryStatus::ValueAbsent } }, 1),
                  m_speed(speed), m_maxXz(maxXz), m_maxY(maxY) {}
            const char* DebugString() const override { return "VillageBoundRandomStroll"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                Villager& v = V(body);
                const glm::ivec3 bodyPos = body.BlockPosition();
                PoiManager* poi = Poi(level);
                std::optional<glm::dvec3> landPos;
                if (!poi || poi->IsVillage(bodyPos)) {
                    landPos = RandomPos::GetLandPos(v, m_maxXz, m_maxY);
                } else {
                    const glm::ivec3 section = SectionOf(bodyPos);
                    const glm::ivec3 optimal = FindSectionClosestToVillage(*poi, section, 2);
                    if (optimal != section) {
                        const glm::ivec3 center(optimal.x * 16 + 8, optimal.y * 16 + 8, optimal.z * 16 + 8);
                        landPos = RandomPos::GetPosTowards(v, m_maxXz, m_maxY, BottomCenter(center), kHalfPi);
                    } else {
                        landPos = RandomPos::GetLandPos(v, m_maxXz, m_maxY);
                    }
                }
                Brain* brain = body.GetBrain();
                if (landPos) brain->SetMemory(MemoryModule::WalkTarget, WalkTarget(Containing(*landPos), m_speed, 0));
                else brain->EraseMemory(MemoryModule::WalkTarget);
                return true;
            }
        private:
            // MC BehaviorUtils.findSectionClosestToVillage.
            static glm::ivec3 FindSectionClosestToVillage(const PoiManager& poi, const glm::ivec3& center, int radius) {
                const int distToVillage = poi.SectionsToVillage(center.x, center.y, center.z);
                glm::ivec3 best = center;
                int bestDist = std::numeric_limits<int>::max();
                bool found = false;
                for (int dz = -radius; dz <= radius; ++dz) {
                    for (int dy = -radius; dy <= radius; ++dy) {
                        for (int dx = -radius; dx <= radius; ++dx) {
                            const glm::ivec3 s = center + glm::ivec3(dx, dy, dz);
                            const int d = poi.SectionsToVillage(s.x, s.y, s.z);
                            if (d >= distToVillage) continue;
                            if (!found || d < bestDist) { best = s; bestDist = d; found = true; }
                        }
                    }
                }
                return best;
            }
            float m_speed;
            int   m_maxXz, m_maxY;
        };

        // MC PlayTagWithOtherKids.
        class PlayTagWithOtherKids : public Behavior {
        public:
            PlayTagWithOtherKids()
                : Behavior({ { MemoryModule::VisibleVillagerBabies, MemoryStatus::ValuePresent },
                             { MemoryModule::WalkTarget, MemoryStatus::ValueAbsent },
                             { MemoryModule::LookTarget, MemoryStatus::Registered },
                             { MemoryModule::InteractionTarget, MemoryStatus::Registered } }, 1) {}
            const char* DebugString() const override { return "PlayTagWithOtherKids"; }
        protected:
            static Entity* ChasingWho(Entity* friendEntity) {
                auto* l = friendEntity ? friendEntity->AsLiving() : nullptr;
                const Brain* b = l ? l->GetBrain() : nullptr;
                return b ? b->GetEntity(MemoryModule::InteractionTarget) : nullptr;
            }
            static void ChaseKid(LivingEntity& me, LivingEntity& kid) {
                Brain* brain = me.GetBrain();
                brain->SetMemory(MemoryModule::InteractionTarget, static_cast<Entity*>(&kid));
                brain->SetMemory(MemoryModule::LookTarget, PositionTracker::OfEntity(&kid, true));
                brain->SetMemory(MemoryModule::WalkTarget, WalkTarget(PositionTracker::OfEntity(&kid, false), 0.6f, 1));
            }
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& me) override {
                if (level.Random().NextInt(10) != 0) return false;
                const std::vector<Entity*>* friends = me.GetBrain()->GetEntityList(MemoryModule::VisibleVillagerBabies);
                if (!friends) return false;
                bool someoneChasingMe = false;
                for (Entity* f : *friends) if (ChasingWho(f) == &me) { someoneChasingMe = true; break; }
                if (!someoneChasingMe) {
                    // findSomeoneBeingChased: the least-chased kid with 1..5
                    // chasers (ties: first seen).
                    std::vector<std::pair<Entity*, int>> chased;
                    for (Entity* f : *friends) {
                        Entity* t = ChasingWho(f);
                        if (!t) continue;
                        auto it = std::find_if(chased.begin(), chased.end(), [t](const auto& p) { return p.first == t; });
                        if (it == chased.end()) chased.emplace_back(t, 1);
                        else ++it->second;
                    }
                    std::stable_sort(chased.begin(), chased.end(),
                                     [](const auto& a, const auto& b) { return a.second < b.second; });
                    for (const auto& [kid, count] : chased) {
                        if (count > 0 && count <= 5 && kid->AsLiving()) {
                            ChaseKid(me, *kid->AsLiving());
                            return true;
                        }
                    }
                    for (Entity* f : *friends) {
                        if (f && f->AsLiving()) { ChaseKid(me, *f->AsLiving()); break; }
                    }
                    return true;
                }
                // Run away — somewhere inside the village.
                PoiManager* poi = Poi(level);
                for (int j = 0; j < 10; ++j) {
                    const auto pos = RandomPos::GetLandPos(V(me), 20, 8);
                    if (pos && (!poi || poi->IsVillage(Containing(*pos)))) {
                        me.GetBrain()->SetMemory(MemoryModule::WalkTarget, WalkTarget(Containing(*pos), 0.6f, 0));
                        break;
                    }
                }
                return true;
            }
        };

        // MC JumpOnBed.
        class JumpOnBed : public Behavior {
        public:
            explicit JumpOnBed(float speed)
                : Behavior({ { MemoryModule::NearestBed, MemoryStatus::ValuePresent },
                             { MemoryModule::WalkTarget, MemoryStatus::ValueAbsent } }, kNoTimeout),
                  m_speed(speed) {}
            const char* DebugString() const override { return "JumpOnBed"; }
        protected:
            static bool IsJumpable(EntityLevel& level, const glm::ivec3& p) {
                return IsDyedBed(StateAt(level, p).Block());   // #villager_babies_can_jump_on_bed
            }
            bool OnOrOverBed(EntityLevel& level, LivingEntity& body) const {
                const glm::ivec3 p = body.BlockPosition();
                return IsJumpable(level, p) || IsJumpable(level, p - glm::ivec3(0, 1, 0));
            }
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                return body.IsBaby() &&
                       (OnOrOverBed(level, body) || body.GetBrain()->HasMemoryValue(MemoryModule::NearestBed));
            }
            void Start(EntityLevel& level, LivingEntity& body, int64_t) override {
                if (const auto bed = body.GetBrain()->GetBlockPos(MemoryModule::NearestBed)) {
                    m_targetBed = *bed;
                    m_remainingTimeToReachBed = 100;
                    m_remainingJumps = 3 + level.Random().NextInt(4);
                    m_remainingCooldownUntilNextJump = 0;
                    body.GetBrain()->SetMemory(MemoryModule::WalkTarget, WalkTarget(*bed, m_speed, 0));
                }
            }
            void Stop(EntityLevel&, LivingEntity&, int64_t) override {
                m_targetBed.reset();
                m_remainingTimeToReachBed = 0;
                m_remainingJumps = 0;
                m_remainingCooldownUntilNextJump = 0;
            }
            bool CanStillUse(EntityLevel& level, LivingEntity& body, int64_t) override {
                if (!body.IsBaby() || !m_targetBed || !IsJumpable(level, *m_targetBed)) return false;
                const bool over = OnOrOverBed(level, body);
                const bool tiredOfWalking = !over && m_remainingTimeToReachBed <= 0;
                const bool tiredOfJumping = over && m_remainingJumps <= 0;
                return !tiredOfWalking && !tiredOfJumping;
            }
            void Tick(EntityLevel& level, LivingEntity& body, int64_t) override {
                if (!OnOrOverBed(level, body)) {
                    --m_remainingTimeToReachBed;
                } else if (m_remainingCooldownUntilNextJump > 0) {
                    --m_remainingCooldownUntilNextJump;
                } else if (IsJumpable(level, body.BlockPosition())) {
                    V(body).GetJumpControl().Jump();
                    --m_remainingJumps;
                    m_remainingCooldownUntilNextJump = 5;
                }
            }
        private:
            float m_speed;
            std::optional<glm::ivec3> m_targetBed;
            int m_remainingTimeToReachBed = 0, m_remainingJumps = 0, m_remainingCooldownUntilNextJump = 0;
        };

        // ═════════════════════════════════════════════════════════════════
        // REST
        // ═════════════════════════════════════════════════════════════════

        // MC SleepInBed.
        class SleepInBed : public Behavior {
        public:
            SleepInBed()
                : Behavior({ { MemoryModule::Home, MemoryStatus::ValuePresent },
                             { MemoryModule::LastWoken, MemoryStatus::Registered },
                             { MemoryModule::LastSlept, MemoryStatus::Registered },
                             { MemoryModule::WalkTarget, MemoryStatus::Registered },
                             { MemoryModule::CantReachWalkTargetSince, MemoryStatus::Registered } }, kNoTimeout) {}
            const char* DebugString() const override { return "SleepInBed"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                if (body.IsPassenger()) return false;
                Brain* brain = body.GetBrain();
                const glm::ivec3 target = *brain->GetBlockPos(MemoryModule::Home);
                if (const auto woken = brain->GetLong(MemoryModule::LastWoken)) {
                    const int64_t since = level.GetGameTime() - *woken;
                    if (since > 0 && since < 100) return false;
                }
                const BlockState state = StateAt(level, target);
                return CloserToCenterThan(target, body.position, 2.0) && IsDyedBed(state.Block()) &&
                       !IsBedOccupied(state);
            }
            bool CanStillUse(EntityLevel&, LivingEntity& body, int64_t) override {
                Brain* brain = body.GetBrain();
                const auto bed = brain->GetBlockPos(MemoryModule::Home);
                if (!bed) return false;
                return brain->IsActive(Activity::Rest) && body.position.y > bed->y + 0.4 &&
                       CloserToCenterThan(*bed, body.position, 1.14);
            }
            void Start(EntityLevel& level, LivingEntity& body, int64_t timestamp) override {
                if (timestamp <= m_nextOkStartTime) return;
                Villager& v = V(body);
                Brain* brain = body.GetBrain();
                if (!v.DoorsToClose().empty()) {
                    CloseDoorsThatIHaveOpenedOrPassedThrough(level, v, nullptr, nullptr);
                }
                const glm::ivec3 home = *brain->GetBlockPos(MemoryModule::Home);
                if (v.StartSleeping(home)) brain->SetMemory(MemoryModule::LastSlept, timestamp);
                brain->EraseMemory(MemoryModule::WalkTarget);
                brain->EraseMemory(MemoryModule::CantReachWalkTargetSince);
            }
            void Stop(EntityLevel&, LivingEntity& body, int64_t timestamp) override {
                Villager& v = V(body);
                if (v.IsSleeping()) {
                    v.StopSleeping();
                    m_nextOkStartTime = timestamp + 40;
                }
            }
        private:
            int64_t m_nextOkStartTime = 0;
        };

        // MC SetClosestHomeAsWalkTarget.
        class SetClosestHomeAsWalkTarget : public Behavior {
        public:
            explicit SetClosestHomeAsWalkTarget(float speed)
                : Behavior({ { MemoryModule::WalkTarget, MemoryStatus::ValueAbsent },
                             { MemoryModule::Home, MemoryStatus::ValueAbsent } }, 1),
                  m_speed(speed) {}
            const char* DebugString() const override { return "SetClosestHomeAsWalkTarget"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                if (level.GetGameTime() - m_lastUpdate < 20) return false;
                PoiManager* poi = Poi(level);
                if (!poi) return false;
                const auto isHome = [](PoiType t) { return t == PoiType::Home; };
                const glm::ivec3 bodyPos = body.BlockPosition();
                const auto closest = poi->FindClosest(isHome, bodyPos, 48, PoiManager::Occupancy::Any);
                if (!closest) return false;
                const glm::ivec3 d = *closest - bodyPos;
                if (d.x * d.x + d.y * d.y + d.z * d.z <= 4) return false;
                int triedCount = 0;
                m_lastUpdate = level.GetGameTime() + level.Random().NextInt(20);
                const auto cacheTest = [&](const glm::ivec3& pos) {
                    const int64_t key = PosKey(pos);
                    if (m_batchCache.count(key)) return false;
                    if (++triedCount >= 5) return false;
                    m_batchCache[key] = m_lastUpdate + 40;
                    return true;
                };
                const auto pois = poi->FindAllWithType(isHome, cacheTest, bodyPos, 48, PoiManager::Occupancy::Any);
                const std::optional<Path> path = FindPathToPois(V(body), pois);
                if (path && path->CanReach()) {
                    const glm::ivec3 target = path->GetTarget();
                    if (poi->GetType(target)) {
                        body.GetBrain()->SetMemory(MemoryModule::WalkTarget, WalkTarget(target, m_speed, 1));
                    }
                } else if (triedCount < 5) {
                    for (auto it = m_batchCache.begin(); it != m_batchCache.end();) {
                        if (it->second < m_lastUpdate) it = m_batchCache.erase(it);
                        else ++it;
                    }
                }
                return true;
            }
        private:
            float m_speed;
            std::unordered_map<int64_t, int64_t> m_batchCache;
            int64_t m_lastUpdate = 0;
        };

        // MC InsideBrownianWalk.
        class InsideBrownianWalk : public Behavior {
        public:
            explicit InsideBrownianWalk(float speed)
                : Behavior({ { MemoryModule::WalkTarget, MemoryStatus::ValueAbsent } }, 1), m_speed(speed) {}
            const char* DebugString() const override { return "InsideBrownianWalk"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                const glm::ivec3 bodyPos = body.BlockPosition();
                if (level.CanSeeSky(bodyPos.x, bodyPos.y, bodyPos.z)) return false;
                std::vector<glm::ivec3> poses;
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) poses.push_back(bodyPos + glm::ivec3(dx, dy, dz));
                // Collections.shuffle.
                JavaRandom& r = level.Random();
                for (int i = static_cast<int>(poses.size()) - 1; i > 0; --i) {
                    std::swap(poses[static_cast<size_t>(i)], poses[static_cast<size_t>(r.NextInt(i + 1))]);
                }
                for (const glm::ivec3& p : poses) {
                    if (level.CanSeeSky(p.x, p.y, p.z)) continue;
                    // loadedAndEntityCanStandOn: a full top face.
                    if (!BlockRegistry::IsOcclusionFullCube(StateAt(level, p))) continue;
                    body.GetBrain()->SetMemory(MemoryModule::WalkTarget, WalkTarget(p, m_speed, 0));
                    break;
                }
                return true;
            }
        private:
            float m_speed;
        };

        // MC GoToClosestVillage.
        class GoToClosestVillage : public Behavior {
        public:
            GoToClosestVillage(float speed, int closeEnough)
                : Behavior({ { MemoryModule::WalkTarget, MemoryStatus::ValueAbsent } }, 1),
                  m_speed(speed), m_close(closeEnough) {}
            const char* DebugString() const override { return "GoToClosestVillage"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                PoiManager* poi = Poi(level);
                if (!poi || poi->IsVillage(body.BlockPosition())) return false;
                const int sectionsToVillage = poi->SectionsToVillage(body.BlockPosition());
                std::optional<glm::dvec3> targetPos;
                for (int j = 0; j < 5; ++j) {
                    const auto landPos = RandomPos::GetLandPos(V(body), 15, 7, [poi](const glm::ivec3& p) {
                        return -static_cast<double>(poi->SectionsToVillage(p));
                    });
                    if (!landPos) continue;
                    const int landSections = poi->SectionsToVillage(Containing(*landPos));
                    if (landSections < sectionsToVillage) { targetPos = landPos; break; }
                    if (landSections == sectionsToVillage) targetPos = landPos;
                }
                if (targetPos) {
                    body.GetBrain()->SetMemory(MemoryModule::WalkTarget, WalkTarget(Containing(*targetPos), m_speed, m_close));
                }
                return true;
            }
        private:
            float m_speed;
            int   m_close;
        };

        // ═════════════════════════════════════════════════════════════════
        // MEET
        // ═════════════════════════════════════════════════════════════════

        // MC SocializeAtBell.
        class SocializeAtBell : public Behavior {
        public:
            SocializeAtBell()
                : Behavior({ { MemoryModule::WalkTarget, MemoryStatus::Registered },
                             { MemoryModule::LookTarget, MemoryStatus::Registered },
                             { MemoryModule::MeetingPoint, MemoryStatus::ValuePresent },
                             { MemoryModule::NearestVisibleLivingEntities, MemoryStatus::ValuePresent },
                             { MemoryModule::InteractionTarget, MemoryStatus::ValueAbsent } }, 1) {}
            const char* DebugString() const override { return "SocializeAtBell"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                Brain* brain = body.GetBrain();
                const glm::ivec3 meeting = *brain->GetBlockPos(MemoryModule::MeetingPoint);
                const NearestVisibleLivingEntities* visible =
                    brain->GetVisibleEntities(MemoryModule::NearestVisibleLivingEntities);
                if (level.Random().NextInt(100) != 0 || !visible ||
                    !CloserToCenterThan(meeting, body.position, 4.0)) {
                    return false;
                }
                bool anyVillager = false;
                for (LivingEntity* e : visible->entities) {
                    if (IsVillager(e) && visible->IsVisible(e)) { anyVillager = true; break; }
                }
                if (!anyVillager) return false;
                if (LivingEntity* mob = visible->FindClosest([&](LivingEntity* e) {
                        return IsVillager(e) && e->DistanceToSqr(body) <= 32.0;
                    })) {
                    brain->SetMemory(MemoryModule::InteractionTarget, static_cast<Entity*>(mob));
                    brain->SetMemory(MemoryModule::LookTarget, PositionTracker::OfEntity(mob, true));
                    brain->SetMemory(MemoryModule::WalkTarget, WalkTarget(PositionTracker::OfEntity(mob, false), 0.3f, 1));
                }
                return true;
            }
        };

        // MC TradeWithVillager.
        class TradeWithVillager : public Behavior {
        public:
            TradeWithVillager()
                : Behavior({ { MemoryModule::InteractionTarget, MemoryStatus::ValuePresent },
                             { MemoryModule::NearestVisibleLivingEntities, MemoryStatus::ValuePresent } }) {}
            const char* DebugString() const override { return "TradeWithVillager"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                return ValidVillagerTarget(*body.GetBrain(), MemoryModule::InteractionTarget) != nullptr;
            }
            bool CanStillUse(EntityLevel& level, LivingEntity& body, int64_t) override {
                return CheckExtraStartConditions(level, body);
            }
            void Start(EntityLevel&, LivingEntity& body, int64_t) override {
                Villager* target = ValidVillagerTarget(*body.GetBrain(), MemoryModule::InteractionTarget);
                if (!target) return;
                LockGazeAndWalkToEachOther(body, *target, 0.5f, 2);
                // figureOutWhatIAmWillingToTrade: the target's requested
                // items that this villager does not request itself.
                m_trades.clear();
                const VillagerProfession mine = V(body).GetVillagerData().profession;
                const VillagerProfession theirs = target->GetVillagerData().profession;
                for (ItemID item : { Items::Wheat, Items::WheatSeeds, Items::BeetrootSeeds, Items::BoneMeal }) {
                    if (ProfessionRequestsItem(theirs, item) && !ProfessionRequestsItem(mine, item)) {
                        m_trades.push_back(item);
                    }
                }
            }
            void Tick(EntityLevel& level, LivingEntity& body, int64_t timestamp) override {
                Villager* target = ValidVillagerTarget(*body.GetBrain(), MemoryModule::InteractionTarget);
                if (!target || body.DistanceToSqr(*target) > 5.0) return;
                Villager& v = V(body);
                LockGazeAndWalkToEachOther(body, *target, 0.5f, 2);
                v.Gossip(*target, timestamp);
                const bool isFarmer = v.GetVillagerData().profession == VillagerProfession::Farmer;
                if (v.HasExcessFood() && (isFarmer || target->WantsMoreFood())) {
                    ThrowHalfStack(v, [](const ItemStack& s) { return VillagerFoodNutrition(s.itemId) > 0; }, *target);
                }
                if (isFarmer && v.CountInventoryItem(Items::Wheat) > ItemRegistry::Get(Items::Wheat).maxStackSize / 2) {
                    ThrowHalfStack(v, [](const ItemStack& s) { return s.itemId == Items::Wheat; }, *target);
                }
                if (!m_trades.empty()) {
                    bool hasAny = false;
                    for (ItemID item : m_trades) if (v.CountInventoryItem(item) > 0) { hasAny = true; break; }
                    if (hasAny) {
                        ThrowHalfStack(v, [this](const ItemStack& s) {
                            return std::find(m_trades.begin(), m_trades.end(), s.itemId) != m_trades.end();
                        }, *target);
                    }
                }
                (void)level;
            }
            void Stop(EntityLevel&, LivingEntity& body, int64_t) override {
                body.GetBrain()->EraseMemory(MemoryModule::InteractionTarget);
            }
        private:
            template <typename Pred>
            static void ThrowHalfStack(Villager& v, Pred pred, LivingEntity& target) {
                SimpleContainer& inv = v.GetInventory();
                for (int i = 0; i < inv.GetContainerSize(); ++i) {
                    ItemStack& s = inv.GetItem(i);
                    if (s.IsEmpty() || !pred(s)) continue;
                    const int maxStack = ItemRegistry::Get(s.itemId).maxStackSize;
                    int count = 0;
                    if (s.count > maxStack / 2) count = s.count / 2;
                    else if (s.count > 24) count = s.count - 24;
                    else continue;
                    ItemStack toThrow = s;
                    toThrow.count = count;
                    s.count -= count;
                    if (s.count <= 0) s.Clear();
                    ThrowItem(v, toThrow, target.position);
                    return;
                }
            }
            std::vector<ItemID> m_trades;
        };

        // MC VillagerMakeLove.
        class VillagerMakeLove : public Behavior {
        public:
            VillagerMakeLove()
                : Behavior({ { MemoryModule::BreedTarget, MemoryStatus::ValuePresent },
                             { MemoryModule::NearestVisibleLivingEntities, MemoryStatus::ValuePresent } }, 350, 350) {}
            const char* DebugString() const override { return "VillagerMakeLove"; }
        protected:
            static Villager* Partner(LivingEntity& body) {
                Entity* e = body.GetBrain()->GetEntity(MemoryModule::BreedTarget);
                return IsVillager(e) ? static_cast<Villager*>(e) : nullptr;
            }
            static bool IsBreedingPossible(LivingEntity& body) {
                Villager* partner = Partner(body);
                if (!partner) return false;
                return ValidVillagerTarget(*body.GetBrain(), MemoryModule::BreedTarget) != nullptr &&
                       V(body).CanBreed() && partner->CanBreed();
            }
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override { return IsBreedingPossible(body); }
            bool CanStillUse(EntityLevel&, LivingEntity& body, int64_t timestamp) override {
                return timestamp <= m_birthTimestamp && IsBreedingPossible(body);
            }
            void Start(EntityLevel& level, LivingEntity& body, int64_t timestamp) override {
                Villager* partner = Partner(body);
                if (!partner) return;
                LockGazeAndWalkToEachOther(body, *partner, 0.5f, 2);
                level.BroadcastEntityEvent(*partner, 18);
                level.BroadcastEntityEvent(body, 18);
                const int duration = 275 + level.Random().NextInt(50);
                m_birthTimestamp = timestamp + duration;
            }
            void Tick(EntityLevel& level, LivingEntity& body, int64_t timestamp) override {
                Villager* target = Partner(body);
                if (!target || body.DistanceToSqr(*target) > 5.0) return;
                Villager& v = V(body);
                LockGazeAndWalkToEachOther(body, *target, 0.5f, 2);
                if (timestamp >= m_birthTimestamp) {
                    v.EatAndDigestFood();
                    target->EatAndDigestFood();
                    TryToGiveBirth(level, v, *target);
                } else if (level.Random().NextInt(35) == 0) {
                    level.BroadcastEntityEvent(*target, 12);
                    level.BroadcastEntityEvent(body, 12);
                }
            }
            void Stop(EntityLevel&, LivingEntity& body, int64_t) override {
                body.GetBrain()->EraseMemory(MemoryModule::BreedTarget);
            }
        private:
            static void TryToGiveBirth(EntityLevel& level, Villager& body, Villager& target) {
                PoiManager* poi = Poi(level);
                std::optional<glm::ivec3> bed;
                if (poi) {
                    // takeVacantBed: the first free, reachable bed within 48.
                    bed = poi->Take([](PoiType t) { return t == PoiType::Home; },
                                    [&body](PoiType type, const glm::ivec3& pos) {
                                        const std::optional<Path> path =
                                            body.GetNavigation().CreatePath(pos, PoiValidRange(type));
                                        return path && path->CanReach();
                                    },
                                    body.BlockPosition(), 48, Uuid{});
                }
                if (!bed) {
                    level.BroadcastEntityEvent(target, 13);
                    level.BroadcastEntityEvent(body, 13);
                    return;
                }
                std::unique_ptr<Villager> child = body.MakeBreedOffspring(target);
                if (!child) {
                    poi->Release(*bed, Uuid{});
                    return;
                }
                body.SetAge(6000);
                target.SetAge(6000);
                child->SetAge(AgeableMob::kBabyStartAge);
                child->position = body.position;
                child->oldPosition = body.position;
                child->yRot = 0.0f;
                child->xRot = 0.0f;
                child->MintUuidIfUnset();
                // giveBedToChild: the claim becomes the child's HOME.
                if (Brain* cb = child->GetBrain()) cb->SetMemory(MemoryModule::Home, *bed);
                poi->TransferHolder(*bed, Uuid{}, child->GetUuid());
                // (MC broadcasts the child's hearts (event 12) as it is added;
                // the engine assigns the child's id when the level drains its
                // spawns, after this.)
                level.AddFreshEntity(std::move(child));
            }
            int64_t m_birthTimestamp = 0;
        };

        // ═════════════════════════════════════════════════════════════════
        // PANIC / HIDE
        // ═════════════════════════════════════════════════════════════════

        // MC VillagerCalmDown.
        class VillagerCalmDown : public Behavior {
        public:
            VillagerCalmDown()
                : Behavior({ { MemoryModule::HurtBy, MemoryStatus::Registered },
                             { MemoryModule::HurtByEntity, MemoryStatus::Registered },
                             { MemoryModule::NearestHostile, MemoryStatus::Registered } }, 1) {}
            const char* DebugString() const override { return "VillagerCalmDown"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                Brain* brain = body.GetBrain();
                Entity* hurtByEntity = brain->GetEntity(MemoryModule::HurtByEntity);
                const bool feelScared = brain->HasMemoryValue(MemoryModule::HurtBy) ||
                                        brain->HasMemoryValue(MemoryModule::NearestHostile) ||
                                        (hurtByEntity && hurtByEntity->DistanceToSqr(body) <= 36.0);
                if (!feelScared) {
                    brain->EraseMemory(MemoryModule::HurtBy);
                    brain->EraseMemory(MemoryModule::HurtByEntity);
                    V(body).UpdateActivityFromSchedule();
                }
                return true;
            }
        };

        // MC SetHiddenState(seconds, closeEnough).
        class SetHiddenState : public Behavior {
        public:
            SetHiddenState(int seconds, int closeEnough)
                : Behavior({ { MemoryModule::HidingPlace, MemoryStatus::ValuePresent },
                             { MemoryModule::HeardBellTime, MemoryStatus::ValuePresent } }, 1),
                  m_stayHiddenTicks(seconds * 20), m_close(closeEnough) {}
            const char* DebugString() const override { return "SetHiddenState"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                Brain* brain = body.GetBrain();
                const int64_t triggered = *brain->GetLong(MemoryModule::HeardBellTime);
                const int64_t timestamp = level.GetGameTime();
                const bool timedOutTryingToHide = triggered + 300 <= timestamp;
                if (m_ticksHidden <= m_stayHiddenTicks && !timedOutTryingToHide) {
                    const glm::ivec3 hidePos = *brain->GetBlockPos(MemoryModule::HidingPlace);
                    if (BlockCloserThan(hidePos, body.BlockPosition(), m_close)) ++m_ticksHidden;
                    return true;
                }
                brain->EraseMemory(MemoryModule::HeardBellTime);
                brain->EraseMemory(MemoryModule::HidingPlace);
                V(body).UpdateActivityFromSchedule();
                m_ticksHidden = 0;
                return true;
            }
        private:
            int m_stayHiddenTicks, m_close;
            int m_ticksHidden = 0;
        };

        // MC LocateHidingPlace(radius, speed, closeEnough).
        class LocateHidingPlace : public Behavior {
        public:
            LocateHidingPlace(int radius, float speed, int closeEnough)
                : Behavior({ { MemoryModule::WalkTarget, MemoryStatus::ValueAbsent },
                             { MemoryModule::Home, MemoryStatus::Registered },
                             { MemoryModule::HidingPlace, MemoryStatus::Registered },
                             { MemoryModule::LookTarget, MemoryStatus::Registered },
                             { MemoryModule::BreedTarget, MemoryStatus::Registered },
                             { MemoryModule::InteractionTarget, MemoryStatus::Registered } }, 1),
                  m_radius(radius), m_speed(speed), m_close(closeEnough) {}
            const char* DebugString() const override { return "LocateHidingPlace"; }
        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                PoiManager* poi = Poi(level);
                Brain* brain = body.GetBrain();
                const auto isHome = [](PoiType t) { return t == PoiType::Home; };
                std::optional<glm::ivec3> pos;
                if (poi) {
                    // find(home, any, bodyPos, close+1, ANY).filter(closerToCenter(close))
                    const auto near = poi->FindAllWithType(isHome, {}, body.BlockPosition(), m_close + 1,
                                                           PoiManager::Occupancy::Any);
                    if (!near.empty() && CloserToCenterThan(near.front().second, body.position, m_close)) {
                        pos = near.front().second;
                    }
                    if (!pos) {
                        // getRandom(home, radius, ANY): a shuffled walk.
                        auto all = poi->FindAllWithType(isHome, {}, body.BlockPosition(), m_radius,
                                                        PoiManager::Occupancy::Any);
                        JavaRandom& r = level.Random();
                        for (int i = static_cast<int>(all.size()) - 1; i > 0; --i) {
                            std::swap(all[static_cast<size_t>(i)], all[static_cast<size_t>(r.NextInt(i + 1))]);
                        }
                        if (!all.empty()) pos = all.front().second;
                    }
                }
                if (!pos) pos = brain->GetBlockPos(MemoryModule::Home);
                if (pos) {
                    brain->EraseMemory(MemoryModule::LookTarget);
                    brain->EraseMemory(MemoryModule::BreedTarget);
                    brain->EraseMemory(MemoryModule::InteractionTarget);
                    brain->SetMemory(MemoryModule::HidingPlace, *pos);
                    if (!CloserToCenterThan(*pos, body.position, m_close)) {
                        brain->SetMemory(MemoryModule::WalkTarget, WalkTarget(*pos, m_speed, m_close));
                    }
                }
                return true;
            }
        private:
            int   m_radius;
            float m_speed;
            int   m_close;
        };

        // ═════════════════════════════════════════════════════════════════
        // Look packages
        // ═════════════════════════════════════════════════════════════════

        BehaviorPtr LookAtPlayer(float dist) {
            return std::make_unique<SetEntityLookTarget>([](LivingEntity& e) { return e.IsPlayer(); }, dist);
        }
        BehaviorPtr LookAtType(EntityTypeId type, float dist) {
            return std::make_unique<SetEntityLookTarget>(
                [type](LivingEntity& e) { return !e.IsPlayer() && e.GetType() == type; }, dist);
        }
        BehaviorPtr LookAtCategory(MobCategory category, float dist) {
            return std::make_unique<SetEntityLookTarget>(
                [category](LivingEntity& e) { return !e.IsPlayer() && e.TypeInfo().category == category; }, dist);
        }

        // VillagerGoalPackages.getMinimalLookBehavior.
        Brain::PrioritizedBehavior MinimalLook() {
            std::vector<GateBehavior::Entry> e;
            e.push_back({ LookAtType(EntityTypeId::Villager, 8.0f), 2 });
            e.push_back({ LookAtPlayer(8.0f), 2 });
            e.push_back({ std::make_unique<DoNothing>(30, 60), 8 });
            return { 5, MakeRunOne(std::move(e)) };
        }

        // VillagerGoalPackages.getFullLookBehavior.
        Brain::PrioritizedBehavior FullLook() {
            std::vector<GateBehavior::Entry> e;
            e.push_back({ LookAtType(EntityTypeId::Cat, 8.0f), 8 });
            e.push_back({ LookAtType(EntityTypeId::Villager, 8.0f), 2 });
            e.push_back({ LookAtPlayer(8.0f), 2 });
            e.push_back({ LookAtCategory(MobCategory::Creature, 8.0f), 1 });
            e.push_back({ LookAtCategory(MobCategory::WaterCreature, 8.0f), 1 });
            e.push_back({ LookAtCategory(MobCategory::Axolotls, 8.0f), 1 });
            e.push_back({ LookAtCategory(MobCategory::UndergroundWaterCreature, 8.0f), 1 });
            e.push_back({ LookAtCategory(MobCategory::WaterAmbient, 8.0f), 1 });
            e.push_back({ LookAtCategory(MobCategory::Monster, 8.0f), 1 });
            e.push_back({ std::make_unique<DoNothing>(30, 60), 2 });
            return { 5, MakeRunOne(std::move(e)) };
        }

        template <typename T, typename... Args>
        Brain::PrioritizedBehavior P(int priority, Args&&... args) {
            return { priority, std::make_unique<T>(std::forward<Args>(args)...) };
        }

        BehaviorPtr TradeGate() {
            std::vector<GateBehavior::Entry> e;
            e.push_back({ std::make_unique<TradeWithVillager>(), 1 });
            return std::make_unique<GateBehavior>(std::vector<MemoryCondition>{},
                                                  std::vector<MemoryModule>{ MemoryModule::InteractionTarget },
                                                  GateBehavior::OrderPolicy::Ordered,
                                                  GateBehavior::RunningPolicy::RunOne, std::move(e));
        }

        // ═════════════════════════════════════════════════════════════════
        // Packages
        // ═════════════════════════════════════════════════════════════════

        std::vector<Brain::PrioritizedBehavior> CorePackage(VillagerProfession profession, float speed) {
            std::vector<Brain::PrioritizedBehavior> b;
            const auto held = [profession](PoiType t) { return ProfessionHoldsJobSite(profession, t); };
            const auto acquirable = [profession](PoiType t) { return ProfessionCanAcquireJobSite(profession, t); };
            b.push_back(P<Swim>(0, 0.8f));
            b.push_back(P<InteractWithDoor>(0));
            b.push_back(P<LookAtTargetSink>(0, 45, 90));
            b.push_back(P<VillagerPanicTrigger>(0));
            b.push_back(P<WakeUp>(0));
            b.push_back(P<ReactToBell>(0));
            // (SetRaidStatus — raids do not exist.)
            b.push_back(P<ValidateNearbyPoi>(0, held, MemoryModule::JobSite));
            b.push_back(P<ValidateNearbyPoi>(0, acquirable, MemoryModule::PotentialJobSite));
            b.push_back(P<MoveToTargetSink>(1));
            b.push_back(P<PoiCompetitorScan>(2));
            b.push_back(P<LookAndFollowTradingPlayerSink>(3, speed));
            b.push_back(P<GoToWantedItem>(5, speed, 4));
            b.push_back(P<AcquirePoi>(6, acquirable, MemoryModule::JobSite, MemoryModule::PotentialJobSite,
                                      true, std::nullopt, AcquirePoi::ValidPoi{}));
            b.push_back(P<GoToPotentialJobSite>(7, speed));
            b.push_back(P<YieldJobSite>(8, speed));
            b.push_back(P<AcquirePoi>(10, [](PoiType t) { return t == PoiType::Home; },
                                      MemoryModule::Home, MemoryModule::Home, false, uint8_t{14},
                                      // validateBedPoi: a bed villagers sleep in, not occupied.
                                      [](EntityLevel& level, const glm::ivec3& p) {
                                          const BlockState s = StateAt(level, p);
                                          return IsDyedBed(s.Block()) && !IsBedOccupied(s);
                                      }));
            b.push_back(P<AcquirePoi>(10, [](PoiType t) { return t == PoiType::Meeting; },
                                      MemoryModule::MeetingPoint, MemoryModule::MeetingPoint, true,
                                      uint8_t{14}, AcquirePoi::ValidPoi{}));
            b.push_back(P<AssignProfessionFromJobSite>(10));
            b.push_back(P<ResetProfession>(10));
            return b;
        }

        std::vector<Brain::PrioritizedBehavior> WorkPackage(VillagerProfession profession, float speed) {
            const bool farmer = profession == VillagerProfession::Farmer;
            std::vector<Brain::PrioritizedBehavior> b;
            b.push_back(MinimalLook());
            std::vector<GateBehavior::Entry> work;
            work.push_back({ std::make_unique<WorkAtPoi>(farmer), 7 });
            work.push_back({ std::make_unique<StrollAroundPoi>(MemoryModule::JobSite, 0.4f, 4), 2 });
            work.push_back({ std::make_unique<StrollToPoi>(MemoryModule::JobSite, 0.4f, 1, 10), 5 });
            work.push_back({ std::make_unique<StrollToPoiList>(speed, 1, 6), 5 });
            work.push_back({ std::make_unique<HarvestFarmland>(), farmer ? 2 : 5 });
            work.push_back({ std::make_unique<UseBonemeal>(), farmer ? 4 : 7 });
            b.push_back({ 5, MakeRunOne(std::move(work)) });
            b.push_back(P<ShowTradesToPlayer>(10, 400, 1600));
            b.push_back(P<SetLookAndInteract>(10, 4));
            b.push_back(P<SetWalkTargetFromBlockMemory>(2, MemoryModule::JobSite, speed, 9, 100, 1200));
            b.push_back(P<GiveGiftToHero>(3, 100));
            b.push_back(P<UpdateActivityFromSchedule>(99));
            return b;
        }

        std::vector<Brain::PrioritizedBehavior> PlayPackage(float speed) {
            std::vector<Brain::PrioritizedBehavior> b;
            b.push_back(P<MoveToTargetSink>(0, 80, 120));
            b.push_back(FullLook());
            b.push_back(P<PlayTagWithOtherKids>(5));
            std::vector<GateBehavior::Entry> e;
            e.push_back({ std::make_unique<VillagerInteractWith>(EntityTypeId::Villager, 8, nullptr, nullptr,
                                                                 MemoryModule::InteractionTarget, speed, 2), 2 });
            e.push_back({ std::make_unique<VillagerInteractWith>(EntityTypeId::Cat, 8, nullptr, nullptr,
                                                                 MemoryModule::InteractionTarget, speed, 2), 1 });
            e.push_back({ std::make_unique<VillageBoundRandomStroll>(speed), 1 });
            e.push_back({ std::make_unique<SetWalkTargetFromLookTarget>(speed, 2), 1 });
            e.push_back({ std::make_unique<JumpOnBed>(speed), 2 });
            e.push_back({ std::make_unique<DoNothing>(20, 40), 2 });
            b.push_back({ 5, MakeRunOne({ { MemoryModule::VisibleVillagerBabies, MemoryStatus::ValueAbsent } },
                                        std::move(e)) });
            b.push_back(P<UpdateActivityFromSchedule>(99));
            return b;
        }

        std::vector<Brain::PrioritizedBehavior> RestPackage(float speed) {
            std::vector<Brain::PrioritizedBehavior> b;
            b.push_back(P<SetWalkTargetFromBlockMemory>(2, MemoryModule::Home, speed, 1, 150, 1200));
            b.push_back(P<ValidateNearbyPoi>(3, [](PoiType t) { return t == PoiType::Home; }, MemoryModule::Home));
            b.push_back(P<SleepInBed>(3));
            std::vector<GateBehavior::Entry> e;
            e.push_back({ std::make_unique<SetClosestHomeAsWalkTarget>(speed), 1 });
            e.push_back({ std::make_unique<InsideBrownianWalk>(speed), 4 });
            e.push_back({ std::make_unique<GoToClosestVillage>(speed, 4), 2 });
            e.push_back({ std::make_unique<DoNothing>(20, 40), 2 });
            b.push_back({ 5, MakeRunOne({ { MemoryModule::Home, MemoryStatus::ValueAbsent } }, std::move(e)) });
            b.push_back(MinimalLook());
            b.push_back(P<UpdateActivityFromSchedule>(99));
            return b;
        }

        std::vector<Brain::PrioritizedBehavior> MeetPackage(float speed) {
            std::vector<Brain::PrioritizedBehavior> b;
            std::vector<GateBehavior::Entry> trigger;
            trigger.push_back({ std::make_unique<StrollAroundPoi>(MemoryModule::MeetingPoint, 0.4f, 40), 2 });
            trigger.push_back({ std::make_unique<SocializeAtBell>(), 2 });
            b.push_back({ 2, MakeRunOne(std::move(trigger)) });   // TriggerGate.triggerOneShuffled
            b.push_back(P<ShowTradesToPlayer>(10, 400, 1600));
            b.push_back(P<SetLookAndInteract>(10, 4));
            b.push_back(P<SetWalkTargetFromBlockMemory>(2, MemoryModule::MeetingPoint, speed, 6, 100, 200));
            b.push_back(P<GiveGiftToHero>(3, 100));
            b.push_back(P<ValidateNearbyPoi>(3, [](PoiType t) { return t == PoiType::Meeting; },
                                             MemoryModule::MeetingPoint));
            b.push_back({ 3, TradeGate() });
            b.push_back(FullLook());
            b.push_back(P<UpdateActivityFromSchedule>(99));
            return b;
        }

        std::vector<Brain::PrioritizedBehavior> IdlePackage(float speed) {
            std::vector<Brain::PrioritizedBehavior> b;
            const auto canBreed = [](LivingEntity& e) {
                return IsVillager(&e) && static_cast<Villager&>(e).CanBreed();
            };
            std::vector<GateBehavior::Entry> e;
            e.push_back({ std::make_unique<VillagerInteractWith>(EntityTypeId::Villager, 8, nullptr, nullptr,
                                                                 MemoryModule::InteractionTarget, speed, 2), 2 });
            e.push_back({ std::make_unique<VillagerInteractWith>(EntityTypeId::Villager, 8, canBreed, canBreed,
                                                                 MemoryModule::BreedTarget, speed, 2), 1 });
            e.push_back({ std::make_unique<VillagerInteractWith>(EntityTypeId::Cat, 8, nullptr, nullptr,
                                                                 MemoryModule::InteractionTarget, speed, 2), 1 });
            e.push_back({ std::make_unique<VillageBoundRandomStroll>(speed), 1 });
            e.push_back({ std::make_unique<SetWalkTargetFromLookTarget>(speed, 2), 1 });
            e.push_back({ std::make_unique<JumpOnBed>(speed), 1 });
            e.push_back({ std::make_unique<DoNothing>(30, 60), 1 });
            b.push_back({ 2, MakeRunOne(std::move(e)) });
            b.push_back(P<GiveGiftToHero>(3, 100));
            b.push_back(P<SetLookAndInteract>(3, 4));
            b.push_back(P<ShowTradesToPlayer>(3, 400, 1600));
            b.push_back({ 3, TradeGate() });
            {
                std::vector<GateBehavior::Entry> love;
                love.push_back({ std::make_unique<VillagerMakeLove>(), 1 });
                b.push_back({ 3, std::make_unique<GateBehavior>(
                                     std::vector<MemoryCondition>{},
                                     std::vector<MemoryModule>{ MemoryModule::BreedTarget },
                                     GateBehavior::OrderPolicy::Ordered, GateBehavior::RunningPolicy::RunOne,
                                     std::move(love)) });
            }
            b.push_back(FullLook());
            b.push_back(P<UpdateActivityFromSchedule>(99));
            return b;
        }

        std::vector<Brain::PrioritizedBehavior> PanicPackage(float speed) {
            const float runawaySpeed = speed * 1.5f;
            std::vector<Brain::PrioritizedBehavior> b;
            b.push_back(P<VillagerCalmDown>(0));
            b.push_back(P<SetWalkTargetAwayFrom>(1, MemoryModule::NearestHostile, runawaySpeed, 6, false));
            b.push_back(P<SetWalkTargetAwayFrom>(1, MemoryModule::HurtByEntity, runawaySpeed, 6, false));
            b.push_back(P<VillageBoundRandomStroll>(3, runawaySpeed, 2, 2));
            b.push_back(MinimalLook());
            return b;
        }

        // PRE_RAID / RAID: entered only by SetRaidStatus with a raid in
        // progress. Their ResetRaidStatus (priority 99) returns the brain to
        // the schedule once the raid is gone — which, with no raids, is at
        // once; the activities are registered so the brain's activity table
        // matches MC's.
        std::vector<Brain::PrioritizedBehavior> RaidStubPackage() {
            std::vector<Brain::PrioritizedBehavior> b;
            b.push_back(MinimalLook());
            b.push_back(P<UpdateActivityFromSchedule>(99));
            return b;
        }

        std::vector<Brain::PrioritizedBehavior> HidePackage(float speed) {
            std::vector<Brain::PrioritizedBehavior> b;
            b.push_back(P<SetHiddenState>(0, 15, 3));
            b.push_back(P<LocateHidingPlace>(1, 32, speed * 1.25f, 2));
            b.push_back(MinimalLook());
            return b;
        }

    } // namespace

    // ═════════════════════════════════════════════════════════════════════
    // Public
    // ═════════════════════════════════════════════════════════════════════

    namespace VillagerAi {

        Activity ScheduledActivity(bool baby, int64_t dayTime) {
            // data/minecraft/timeline/villager_schedule.json, period 24000.
            // A step track: the value is the last keyframe at or before the
            // time, wrapping to the day's last keyframe before the first.
            struct Key { int ticks; Activity activity; };
            static constexpr Key kAdult[] = {
                { 10, Activity::Idle }, { 2000, Activity::Work }, { 9000, Activity::Meet },
                { 11000, Activity::Idle }, { 12000, Activity::Rest },
            };
            static constexpr Key kBaby[] = {
                { 10, Activity::Idle }, { 3000, Activity::Play }, { 6000, Activity::Idle },
                { 10000, Activity::Play }, { 12000, Activity::Rest },
            };
            const Key* keys = baby ? kBaby : kAdult;
            constexpr int kCount = 5;
            int64_t t = dayTime % 24000;
            if (t < 0) t += 24000;
            Activity result = keys[kCount - 1].activity;
            for (int i = 0; i < kCount; ++i) {
                if (t >= keys[i].ticks) result = keys[i].activity;
            }
            return result;
        }

        void InitBrain(Villager& villager, Brain& brain) {
            const float speed = Villager::kSpeedModifier;
            const VillagerProfession profession = villager.GetVillagerData().profession;

            // The memories behaviours read without a sensor writing them.
            for (MemoryModule m : {
                     MemoryModule::Home, MemoryModule::JobSite, MemoryModule::PotentialJobSite,
                     MemoryModule::MeetingPoint, MemoryModule::WalkTarget, MemoryModule::LookTarget,
                     MemoryModule::InteractionTarget, MemoryModule::BreedTarget, MemoryModule::Path,
                     MemoryModule::DoorsToClose, MemoryModule::HidingPlace, MemoryModule::HeardBellTime,
                     MemoryModule::CantReachWalkTargetSince, MemoryModule::LastSlept, MemoryModule::LastWoken,
                     MemoryModule::LastWorkedAtPoi, MemoryModule::ItemPickupCooldownTicks,
                     MemoryModule::DangerDetectedRecently, MemoryModule::NearestVisibleWantedItem }) {
                brain.RegisterMemory(m);
            }

            // Villager.BRAIN_PROVIDER's sensors, in MC's order.
            brain.AddSensor(std::make_unique<NearestLivingEntitySensor>());
            brain.AddSensor(std::make_unique<PlayerSensor>());
            brain.AddSensor(std::make_unique<NearestItemSensor>());
            brain.AddSensor(std::make_unique<NearestBedSensor>());
            brain.AddSensor(std::make_unique<HurtBySensor>());
            brain.AddSensor(std::make_unique<VillagerHostilesSensor>());
            brain.AddSensor(std::make_unique<VillagerBabiesSensor>());
            brain.AddSensor(std::make_unique<SecondaryPoiSensor>());
            brain.AddSensor(std::make_unique<GolemSensor>());

            brain.SetCoreActivities({ Activity::Core });
            brain.SetDefaultActivity(Activity::Idle);

            if (villager.IsBaby()) {
                brain.AddActivityWithPriorities(Activity::Play, PlayPackage(speed));
            } else {
                brain.AddActivityWithPriorities(Activity::Work, WorkPackage(profession, speed),
                                                { { MemoryModule::JobSite, MemoryStatus::ValuePresent } });
            }
            brain.AddActivityWithPriorities(Activity::Core, CorePackage(profession, speed));
            brain.AddActivityWithPriorities(Activity::Meet, MeetPackage(speed),
                                            { { MemoryModule::MeetingPoint, MemoryStatus::ValuePresent } });
            brain.AddActivityWithPriorities(Activity::Rest, RestPackage(speed));
            brain.AddActivityWithPriorities(Activity::Idle, IdlePackage(speed));
            brain.AddActivityWithPriorities(Activity::Panic, PanicPackage(speed));
            brain.AddActivityWithPriorities(Activity::PreRaid, RaidStubPackage());
            brain.AddActivityWithPriorities(Activity::Raid, RaidStubPackage());
            brain.AddActivityWithPriorities(Activity::Hide, HidePackage(speed));
            brain.UseDefaultActivity();
        }

    } // namespace VillagerAi

} // namespace Game

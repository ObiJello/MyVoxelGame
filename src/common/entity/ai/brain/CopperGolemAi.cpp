// File: src/common/entity/ai/brain/CopperGolemAi.cpp
#include "common/entity/ai/brain/CopperGolemAi.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/brain/CommonBehaviors.hpp"
#include "common/entity/ai/brain/CoreBehaviors.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/inventory/CompoundContainer.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/entity/ChestBlockEntity.hpp"
#include "common/world/block/entity/DoubleChest.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <vector>

namespace Game {

    namespace {

        // MC CopperGolemAi's constants.
        constexpr float kSpeedMultiplierWhenPanicking = 1.5f;
        constexpr float kSpeedMultiplierWhenIdling    = 1.0f;
        constexpr int   kTransportItemHorizontalSearchRadius = 32;
        constexpr int   kTransportItemVerticalSearchRadius   = 8;
        constexpr int   kTickToStartOnReachedInteraction = 1;

        // MC TRANSPORT_ITEM_SOURCE_BLOCK = BlockTags.COPPER_CHESTS.
        // DEVIATION: copper chests exist here only as decorative blocks — no
        // container block entity is registered for them (BlockEntityTypes.cpp
        // maps the chest BE to Chest/TrappedChest/EnderChest only), so a
        // copper chest can never hold an item and a COPPER_CHESTS source
        // predicate would never match a transport target. Until copper chests
        // become real containers, regular chests serve as BOTH source and
        // destination (the parity doc's determination): hand-empty still
        // decides the direction, and the visited-positions bookkeeping keeps
        // the trips moving between different chests.
        bool IsTransportSourceBlock(BlockID block) {
            return block == BlockID::Chest || block == BlockID::TrappedChest;
        }

        // MC TRANSPORT_ITEM_DESTINATION_BLOCK — Blocks.CHEST or TRAPPED_CHEST.
        bool IsTransportDestinationBlock(BlockID block) {
            return block == BlockID::Chest || block == BlockID::TrappedChest;
        }

        // MC TransportItemsBetweenContainers.TransportItemTarget (pos,
        // container, blockEntity, state). The container half is resolved on
        // demand rather than cached — a double chest's partner BE lives in a
        // chunk that can unload independently, and MC's own targetHasNotChanged
        // only revalidates the primary — and the BlockID stands in for the
        // BlockState the predicates test.
        struct TransportItemTarget {
            glm::ivec3        pos{0};
            ChestBlockEntity* blockEntity = nullptr;
            BlockID           block = BlockID::Air;
        };

        TransportItemTarget MakeTarget(ChestBlockEntity& blockEntity) {
            // MC TransportItemTarget.tryCreatePossibleTarget(blockEntity,
            // level). Its null branch — getBlockEntityContainer failing —
            // cannot happen here: a ChestBlockEntity IS its container.
            return { blockEntity.GetWorldPos(), &blockEntity,
                     blockEntity.GetBlockId() };
        }

        // MC ChestBlock.getContainer — the chest's own 27 slots, or the
        // CompoundContainer view when it is half of a double chest
        // (DoubleChest.hpp decides which half is MC's FIRST).
        struct TargetContainerView {
            std::optional<CompoundContainer> compound;
            IContainer* container = nullptr;
        };

        TargetContainerView GetTargetContainer(EntityLevel& level,
                                               const TransportItemTarget& target) {
            TargetContainerView view;
            view.container = target.blockEntity;
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks) return view;
            if (auto pairing = FindChestPartner(*blocks, target.pos)) {
                auto* partner = dynamic_cast<ChestBlockEntity*>(
                    level.GetContainerBlockEntity(pairing->partnerPos));
                if (partner) {
                    IContainer* self = target.blockEntity;
                    view.compound.emplace(pairing->selfIsFirst ? self : partner,
                                          pairing->selfIsFirst ? partner : self);
                    view.container = &*view.compound;
                }
            }
            return view;
        }

        // MC getConnectedTargets — the chest plus its double-chest partner.
        std::vector<TransportItemTarget>
        GetConnectedTargets(EntityLevel& level, const TransportItemTarget& target) {
            std::vector<TransportItemTarget> out{ target };
            if (const IBlockAccess* blocks = level.Blocks()) {
                if (auto pairing = FindChestPartner(*blocks, target.pos)) {
                    if (auto* partner = dynamic_cast<ChestBlockEntity*>(
                            level.GetContainerBlockEntity(pairing->partnerPos))) {
                        out.push_back(MakeTarget(*partner));
                    }
                }
            }
            return out;
        }

        // MC Container.isEmpty over the IContainer surface.
        bool ContainerIsEmpty(const IContainer& container) {
            for (int i = 0; i < container.GetContainerSize(); ++i) {
                if (!container.GetItem(i).IsEmpty()) return false;
            }
            return true;
        }

        double DistToCenterSqr(const glm::ivec3& pos, const glm::dvec3& p) {
            const double dx = pos.x + 0.5 - p.x;
            const double dy = pos.y + 0.5 - p.y;
            const double dz = pos.z + 0.5 - p.z;
            return dx * dx + dy * dy + dz * dz;
        }

        // The COLLIDER clip (same quarter-block DDA as Sensing), returning the
        // first colliding block rather than a boolean — MC's canSeeAnyTargetSide
        // needs the hit BLOCK to be the chest itself.
        std::optional<glm::ivec3> FirstCollidingBlock(const IBlockAccess& blocks,
                                                      const glm::dvec3& from,
                                                      const glm::dvec3& to) {
            const glm::dvec3 delta = to - from;
            const double dist = std::sqrt(delta.x * delta.x + delta.y * delta.y
                                          + delta.z * delta.z);
            if (dist < 1.0e-4) return std::nullopt;
            const int steps = static_cast<int>(std::ceil(dist * 4.0));
            const glm::dvec3 step = delta / static_cast<double>(steps);
            glm::dvec3 p = from;
            for (int i = 1; i <= steps; ++i) {
                p += step;
                const glm::ivec3 bp(static_cast<int>(std::floor(p.x)),
                                    static_cast<int>(std::floor(p.y)),
                                    static_cast<int>(std::floor(p.z)));
                if (BlockRegistry::HasCollision(blocks.GetBlock(bp.x, bp.y, bp.z))) {
                    return bp;
                }
            }
            return std::nullopt;
        }

        // ── TransportItemsBetweenContainers ────────────────────────────────
        //
        // MC world/entity/ai/behavior/TransportItemsBetweenContainers, whole.
        //
        // The GlobalPos sets MC keeps in the VISITED_BLOCK_POSITIONS /
        // UNREACHABLE_TRANSPORT_BLOCK_POSITIONS memories live on the behaviour
        // instead, with MC's whole-set 6000-tick expiry re-armed on every
        // write: the memory variant has no position-set alternative (the
        // sniffer's explored-positions list took the same route). Both
        // memories stay REGISTERED so the entry conditions read as MC's.
        class TransportItemsBetweenContainers : public Behavior {
        public:
            // MC's constants, name for name.
            static constexpr int    kTargetInteractionTime = 60;
            static constexpr int    kVisitedPositionsMemoryTime = 6000;
            static constexpr int    kTransportedItemMaxStackSize = 16;
            static constexpr int    kMaxVisitedPositions = 10;
            static constexpr int    kMaxUnreachablePositions = 50;
            static constexpr int    kPassengerMobTargetSearchDistance = 1;
            static constexpr int    kIdleCooldown = 140;
            static constexpr double kCloseEnoughToStartQueuingDistance = 3.0;
            static constexpr double kCloseEnoughToStartInteractingWithTargetDistance = 0.5;
            static constexpr double kCloseEnoughToStartInteractingWithTargetPathEndDistance = 1.0;
            static constexpr double kCloseEnoughToContinueInteractingWithTarget = 2.0;

            enum class TransportItemState : uint8_t { Travelling, Queuing, Interacting };
            enum class ContainerInteractionState : uint8_t {
                PickupItem, PickupNoItem, PlaceItem, PlaceNoItem,
            };

            using BlockPredicate = bool (*)(BlockID);
            using OnTargetReachedInteraction =
                std::function<void(PathfinderMob&, const TransportItemTarget&, int)>;
            using ContainerConsumer = std::function<void(PathfinderMob&, IContainer&)>;

            TransportItemsBetweenContainers(
                float speedModifier, BlockPredicate sourceBlockType,
                BlockPredicate destinationBlockType, int horizontalSearchDistance,
                int verticalSearchDistance,
                std::map<ContainerInteractionState, OnTargetReachedInteraction>
                    onTargetInteractionActions,
                std::function<void(PathfinderMob&)> onStartTravelling,
                std::function<bool(const TransportItemTarget&)> shouldQueueForTarget)
                : Behavior({ { MemoryModule::VisitedBlockPositions,
                               MemoryStatus::Registered },
                             { MemoryModule::UnreachableTransportBlockPositions,
                               MemoryStatus::Registered },
                             { MemoryModule::TransportItemsCooldownTicks,
                               MemoryStatus::ValueAbsent },
                             { MemoryModule::IsPanicking, MemoryStatus::ValueAbsent } },
                           // MC overrides timedOut() to false; the port's
                           // TimedOut is non-virtual, so an effectively
                           // infinite duration is the same statement. (max()-1
                           // because TryStart computes maxDuration + 1.)
                           std::numeric_limits<int>::max() - 1),
                  m_speedModifier(speedModifier),
                  m_horizontalSearchDistance(horizontalSearchDistance),
                  m_verticalSearchDistance(verticalSearchDistance),
                  m_sourceBlockType(sourceBlockType),
                  m_destinationBlockType(destinationBlockType),
                  m_shouldQueueForTarget(std::move(shouldQueueForTarget)),
                  m_onStartTravelling(std::move(onStartTravelling)),
                  m_onTargetInteractionActions(std::move(onTargetInteractionActions)) {}

            const char* DebugString() const override {
                return "TransportItemsBetweenContainers";
            }

        protected:
            void Start(EntityLevel&, LivingEntity&, int64_t) override {
                // MC flips GroundPathNavigation.setCanPathToTargetsBelowSurface
                // (true) here — the pathfinder has no below-surface toggle, so
                // a chest sunk into the floor may be unreachable and takes the
                // unreachable-cooldown path instead.
            }

            bool CheckExtraStartConditions(EntityLevel&, LivingEntity&) override {
                // MC: !body.isLeashed() — no leash system.
                return true;
            }

            bool CanStillUse(EntityLevel&, LivingEntity& body, int64_t) override {
                // MC: the cooldown memory empty, !isPanicking(), !isLeashed()
                // (no leash system).
                const Brain* brain = body.GetBrain();
                return brain
                    && !brain->HasMemoryValue(MemoryModule::TransportItemsCooldownTicks)
                    && !brain->HasMemoryValue(MemoryModule::IsPanicking);
            }

            void Tick(EntityLevel& level, LivingEntity& body, int64_t timestamp) override {
                auto* mob = dynamic_cast<PathfinderMob*>(&body);
                if (!mob) return;
                const bool updatedInvalidTarget = UpdateInvalidTarget(level, *mob);
                if (!m_target) {
                    Stop(level, body, timestamp);
                } else if (!updatedInvalidTarget) {
                    if (m_state == TransportItemState::Queuing) {
                        OnQueuingForTarget(*m_target, level, *mob);
                    }
                    if (m_state == TransportItemState::Travelling) {
                        OnTravelToTarget(*m_target, level, *mob);
                    }
                    if (m_state == TransportItemState::Interacting) {
                        OnReachedTarget(*m_target, level, *mob);
                    }
                }
            }

            void Stop(EntityLevel&, LivingEntity& body, int64_t) override {
                if (auto* mob = dynamic_cast<PathfinderMob*>(&body)) {
                    OnStartTravelling(*mob);
                }
                // MC resets setCanPathToTargetsBelowSurface(false) — skipped
                // with Start's half.
            }

        private:
            bool UpdateInvalidTarget(EntityLevel& level, PathfinderMob& body) {
                if (!HasValidTarget(level, body)) {
                    StopTargetingCurrentTarget(body);
                    std::optional<TransportItemTarget> targetBlockPosition =
                        GetTransportTarget(level, body);
                    if (targetBlockPosition) {
                        m_target = targetBlockPosition;
                        OnStartTravelling(body);
                        SetVisitedBlockPos(body, level, m_target->pos);
                        return true;
                    }
                    EnterCooldownAfterNoMatchingTargetFound(body);
                    return true;
                }
                return false;
            }

            void OnQueuingForTarget(const TransportItemTarget& target,
                                    EntityLevel& level, PathfinderMob& body) {
                if (!IsAnotherMobInteractingWithTarget(target, level)) {
                    ResumeTravelling(body);
                }
            }

            void OnTravelToTarget(const TransportItemTarget& target,
                                  EntityLevel& level, PathfinderMob& body) {
                if (IsWithinTargetDistance(kCloseEnoughToStartQueuingDistance, target,
                                           level, body, GetCenterPos(body))
                    && IsAnotherMobInteractingWithTarget(target, level)) {
                    StartQueuing(body);
                } else if (IsWithinTargetDistance(GetInteractionRange(body), target,
                                                  level, body, GetCenterPos(body))) {
                    StartOnReachedTargetInteraction(target, level, body);
                } else {
                    WalkTowardsTarget(body);
                }
            }

            glm::dvec3 GetCenterPos(const PathfinderMob& body) const {
                return SetMiddleYPosition(body, body.position);
            }

            void OnReachedTarget(const TransportItemTarget& target,
                                 EntityLevel& level, PathfinderMob& body) {
                if (!IsWithinTargetDistance(kCloseEnoughToContinueInteractingWithTarget,
                                            target, level, body, GetCenterPos(body))) {
                    OnStartTravelling(body);
                } else {
                    ++m_ticksSinceReachingTarget;
                    OnTargetInteraction(target, body);
                    if (m_ticksSinceReachingTarget >= kTargetInteractionTime) {
                        TargetContainerView view = GetTargetContainer(level, target);
                        DoReachedTargetInteraction(
                            body, *view.container,
                            [this](PathfinderMob& mob, IContainer& container) {
                                PickUpItems(mob, container);
                            },
                            [this](PathfinderMob& mob, IContainer&) {
                                StopTargetingCurrentTarget(mob);
                            },
                            [this](PathfinderMob& mob, IContainer& container) {
                                PutDownItem(mob, container);
                            },
                            [this](PathfinderMob& mob, IContainer&) {
                                StopTargetingCurrentTarget(mob);
                            });
                        OnStartTravelling(body);
                    }
                }
            }

            void StartQueuing(PathfinderMob& body) {
                StopInPlace(body);
                SetTransportingState(TransportItemState::Queuing);
            }

            void ResumeTravelling(PathfinderMob& body) {
                SetTransportingState(TransportItemState::Travelling);
                WalkTowardsTarget(body);
            }

            void WalkTowardsTarget(PathfinderMob& body) {
                if (m_target) {
                    // MC BehaviorUtils.setWalkAndLookTargetMemories(body,
                    // target.pos, speedModifier, 0).
                    if (Brain* brain = body.GetBrain()) {
                        brain->SetMemory(MemoryModule::LookTarget,
                                         PositionTracker::OfBlock(m_target->pos));
                        brain->SetMemory(MemoryModule::WalkTarget,
                                         WalkTarget(m_target->pos, m_speedModifier, 0));
                    }
                }
            }

            void StartOnReachedTargetInteraction(const TransportItemTarget& target,
                                                 EntityLevel& level,
                                                 PathfinderMob& body) {
                TargetContainerView view = GetTargetContainer(level, target);
                DoReachedTargetInteraction(
                    body, *view.container,
                    OnReachedInteraction(ContainerInteractionState::PickupItem),
                    OnReachedInteraction(ContainerInteractionState::PickupNoItem),
                    OnReachedInteraction(ContainerInteractionState::PlaceItem),
                    OnReachedInteraction(ContainerInteractionState::PlaceNoItem));
                SetTransportingState(TransportItemState::Interacting);
            }

            void OnStartTravelling(PathfinderMob& body) {
                m_onStartTravelling(body);
                SetTransportingState(TransportItemState::Travelling);
                m_interactionState.reset();
                m_ticksSinceReachingTarget = 0;
            }

            ContainerConsumer OnReachedInteraction(ContainerInteractionState state) {
                return [this, state](PathfinderMob&, IContainer&) {
                    SetInteractionState(state);
                };
            }

            void SetTransportingState(TransportItemState state) { m_state = state; }
            void SetInteractionState(ContainerInteractionState state) {
                m_interactionState = state;
            }

            void OnTargetInteraction(const TransportItemTarget& target,
                                     PathfinderMob& body) {
                if (Brain* brain = body.GetBrain()) {
                    brain->SetMemory(MemoryModule::LookTarget,
                                     PositionTracker::OfBlock(target.pos));
                }
                StopInPlace(body);
                if (m_interactionState) {
                    auto it = m_onTargetInteractionActions.find(*m_interactionState);
                    if (it != m_onTargetInteractionActions.end()) {
                        it->second(body, target, m_ticksSinceReachingTarget);
                    }
                }
            }

            void DoReachedTargetInteraction(PathfinderMob& body, IContainer& container,
                                            const ContainerConsumer& onPickupSuccess,
                                            const ContainerConsumer& onPickupFailure,
                                            const ContainerConsumer& onPlaceSuccess,
                                            const ContainerConsumer& onPlaceFailure) {
                if (IsPickingUpItems(body)) {
                    if (MatchesGettingItemsRequirement(container)) {
                        onPickupSuccess(body, container);
                    } else {
                        onPickupFailure(body, container);
                    }
                } else if (MatchesLeavingItemsRequirement(body, container)) {
                    onPlaceSuccess(body, container);
                } else {
                    onPlaceFailure(body, container);
                }
            }

            std::optional<TransportItemTarget>
            GetTransportTarget(EntityLevel& level, PathfinderMob& body) {
                ExpirePositionSets(level);
                const glm::ivec3 center = body.BlockPosition();
                // MC ChunkPos.rangeClosed(chunk(body), floorDiv(dist, 16) + 1)
                // over each loaded chunk's block entities, through the seam.
                std::vector<BaseContainerBlockEntity*> candidates;
                level.GetContainerBlockEntities(
                    center, GetHorizontalSearchDistance(body) / 16 + 1, candidates);

                std::optional<TransportItemTarget> target;
                double closestDistance = static_cast<double>(
                    std::numeric_limits<float>::max());   // MC (double)Float.MAX_VALUE
                for (BaseContainerBlockEntity* potentialTarget : candidates) {
                    auto* chestBlockEntity = dynamic_cast<ChestBlockEntity*>(potentialTarget);
                    if (!chestBlockEntity) continue;   // MC: instanceof ChestBlockEntity
                    const double distance =
                        DistToCenterSqr(chestBlockEntity->GetWorldPos(), body.position);
                    if (distance < closestDistance) {
                        std::optional<TransportItemTarget> targetValidToPick =
                            IsTargetValidToPick(body, level, *chestBlockEntity, center);
                        if (targetValidToPick) {
                            target = targetValidToPick;
                            closestDistance = distance;
                        }
                    }
                }
                return target;
            }

            std::optional<TransportItemTarget>
            IsTargetValidToPick(PathfinderMob& body, EntityLevel& level,
                                ChestBlockEntity& blockEntity, const glm::ivec3& center) {
                // MC's targetBlockSearchArea: AABB(mob.blockPosition())
                // .inflate(h, v, h) — as an integer containment test.
                const glm::ivec3 pos = blockEntity.GetWorldPos();
                const int h = GetHorizontalSearchDistance(body);
                const int v = GetVerticalSearchDistance(body);
                const bool isWithinSearchArea =
                    pos.x >= center.x - h && pos.x <= center.x + h &&
                    pos.y >= center.y - v && pos.y <= center.y + v &&
                    pos.z >= center.z - h && pos.z <= center.z + h;
                if (!isWithinSearchArea) return std::nullopt;

                TransportItemTarget transportItemTarget = MakeTarget(blockEntity);
                const bool isValidTarget =
                    IsWantedBlock(body, transportItemTarget.block)
                    && !IsPositionAlreadyVisited(level, transportItemTarget)
                    && !IsContainerLocked(transportItemTarget);
                if (!isValidTarget) return std::nullopt;
                return transportItemTarget;
            }

            static bool IsContainerLocked(const TransportItemTarget&) {
                // MC BaseContainerBlockEntity.isLocked — no container lock
                // component exists here, so nothing is ever locked.
                return false;
            }

            bool HasValidTarget(EntityLevel& level, PathfinderMob& body) {
                const bool targetIsOfValidType =
                    m_target && IsWantedBlock(body, m_target->block)
                    && TargetHasNotChanged(level, *m_target);
                if (targetIsOfValidType && !IsTargetBlocked(level, *m_target)) {
                    if (m_state != TransportItemState::Travelling) return true;
                    if (HasValidTravellingPath(level, *m_target, body)) return true;
                    MarkVisitedBlockPosAsUnreachable(body, level, m_target->pos);
                }
                return false;
            }

            bool HasValidTravellingPath(EntityLevel& level,
                                        const TransportItemTarget& target,
                                        PathfinderMob& body) {
                const Path* path = body.GetNavigation().GetPath();
                std::optional<Path> created;
                if (!path) {
                    created = body.GetNavigation().CreatePath(target.pos, 0);
                    if (created) path = &*created;
                }
                const glm::dvec3 posFromWhichToReachTarget =
                    GetPositionToReachTargetFrom(path, body);
                const bool canReachTarget =
                    IsWithinTargetDistance(GetInteractionRange(body), target, level,
                                           body, posFromWhichToReachTarget);
                const bool hasNotYetCreatedPathToTarget = !path && !canReachTarget;
                return hasNotYetCreatedPathToTarget
                    || (canReachTarget
                        && CanSeeAnyTargetSide(target, level,
                                               posFromWhichToReachTarget));
            }

            glm::dvec3 GetPositionToReachTargetFrom(const Path* path,
                                                    const PathfinderMob& body) const {
                const Node* endNode = path ? path->GetEndNode() : nullptr;
                const glm::dvec3 bottomCenter =
                    !endNode ? body.position
                             : glm::dvec3(endNode->x + 0.5, endNode->y, endNode->z + 0.5);
                return SetMiddleYPosition(body, bottomCenter);
            }

            glm::dvec3 SetMiddleYPosition(const PathfinderMob& body,
                                          const glm::dvec3& pos) const {
                return pos + glm::dvec3(0.0, body.GetBbHeight() / 2.0, 0.0);
            }

            static bool IsTargetBlocked(EntityLevel& level,
                                        const TransportItemTarget& target) {
                // MC ChestBlock.isChestBlockedAt: a solid block above
                // (isRedstoneConductor — IsBlockSolid stands in) or a cat
                // sitting on the lid.
                const glm::ivec3 above = target.pos + glm::ivec3(0, 1, 0);
                const IBlockAccess* blocks = level.Blocks();
                if (blocks && blocks->IsBlockSolid(above.x, above.y, above.z)) {
                    return true;
                }
                std::vector<Entity*> entities;
                level.GetEntitiesInBox(
                    AABB(glm::vec3(above.x + 0.5f, above.y + 0.5f, above.z + 0.5f),
                         glm::vec3(1.0f, 1.0f, 1.0f)),
                    nullptr, entities);
                for (Entity* e : entities) {
                    auto* cat = dynamic_cast<Cat*>(e);
                    if (cat && cat->IsInSittingPose()) return true;
                }
                return false;
            }

            static bool TargetHasNotChanged(EntityLevel& level,
                                            const TransportItemTarget& target) {
                return level.GetContainerBlockEntity(target.pos) == target.blockEntity;
            }

            int GetHorizontalSearchDistance(const PathfinderMob& body) const {
                return body.IsPassenger() ? kPassengerMobTargetSearchDistance
                                          : m_horizontalSearchDistance;
            }

            int GetVerticalSearchDistance(const PathfinderMob& body) const {
                return body.IsPassenger() ? kPassengerMobTargetSearchDistance
                                          : m_verticalSearchDistance;
            }

            bool IsPositionAlreadyVisited(EntityLevel& level,
                                          const TransportItemTarget& target) {
                for (const TransportItemTarget& connected :
                     GetConnectedTargets(level, target)) {
                    if (Contains(m_visitedPositions, connected.pos)
                        || Contains(m_unreachablePositions, connected.pos)) {
                        return true;
                    }
                }
                return false;
            }

            static bool HasFinishedPath(const PathfinderMob& body) {
                const Path* path = body.GetNavigation().GetPath();
                return path != nullptr && path->IsDone();
            }

            static bool Contains(const std::vector<glm::ivec3>& set,
                                 const glm::ivec3& pos) {
                return std::find(set.begin(), set.end(), pos) != set.end();
            }

            // The whole-set 6000-tick expiry MC gets from setMemoryWithExpiry:
            // a set whose deadline passed is forgotten wholesale.
            void ExpirePositionSets(EntityLevel& level) {
                const int64_t now = level.GetGameTime();
                if (now >= m_visitedExpiry) m_visitedPositions.clear();
                if (now >= m_unreachableExpiry) m_unreachablePositions.clear();
            }

            void SetVisitedBlockPos(PathfinderMob& body, EntityLevel& level,
                                    const glm::ivec3& target) {
                ExpirePositionSets(level);
                if (!Contains(m_visitedPositions, target)) {
                    m_visitedPositions.push_back(target);
                }
                if (static_cast<int>(m_visitedPositions.size()) > kMaxVisitedPositions) {
                    EnterCooldownAfterNoMatchingTargetFound(body);
                } else {
                    m_visitedExpiry = level.GetGameTime() + kVisitedPositionsMemoryTime;
                }
            }

            void MarkVisitedBlockPosAsUnreachable(PathfinderMob& body,
                                                  EntityLevel& level,
                                                  const glm::ivec3& target) {
                ExpirePositionSets(level);
                m_visitedPositions.erase(std::remove(m_visitedPositions.begin(),
                                                     m_visitedPositions.end(), target),
                                         m_visitedPositions.end());
                if (!Contains(m_unreachablePositions, target)) {
                    m_unreachablePositions.push_back(target);
                }
                if (static_cast<int>(m_unreachablePositions.size())
                    > kMaxUnreachablePositions) {
                    EnterCooldownAfterNoMatchingTargetFound(body);
                } else {
                    m_visitedExpiry = level.GetGameTime() + kVisitedPositionsMemoryTime;
                    m_unreachableExpiry = level.GetGameTime() + kVisitedPositionsMemoryTime;
                }
            }

            bool IsWantedBlock(PathfinderMob& mob, BlockID block) const {
                return IsPickingUpItems(mob) ? m_sourceBlockType(block)
                                             : m_destinationBlockType(block);
            }

            static double GetInteractionRange(const PathfinderMob& body) {
                return HasFinishedPath(body)
                    ? kCloseEnoughToStartInteractingWithTargetPathEndDistance
                    : kCloseEnoughToStartInteractingWithTargetDistance;
            }

            bool IsWithinTargetDistance(double distance,
                                        const TransportItemTarget& target,
                                        EntityLevel& level, const PathfinderMob& body,
                                        const glm::dvec3& fromPos) const {
                // MC: target.state.getCollisionShape(level, pos).bounds()
                // .inflate(distance, 0.5, distance).move(pos).intersects(
                // AABB.ofSize(fromPos, mob dimensions)) — strict on all axes.
                BlockRegistry::BlockShape shape;
                if (const IBlockAccess* blocks = level.Blocks()) {
                    shape = BlockRegistry::GetBlockShapeAt(
                        *blocks, target.pos,
                        blocks->GetBlockState(target.pos.x, target.pos.y, target.pos.z));
                }
                const double minX = target.pos.x + shape.min.x - distance;
                const double minY = target.pos.y + shape.min.y - 0.5;
                const double minZ = target.pos.z + shape.min.z - distance;
                const double maxX = target.pos.x + shape.max.x + distance;
                const double maxY = target.pos.y + shape.max.y + 0.5;
                const double maxZ = target.pos.z + shape.max.z + distance;
                const double w = body.GetBbWidth();
                const double h = body.GetBbHeight();
                return minX < fromPos.x + w * 0.5 && maxX > fromPos.x - w * 0.5
                    && minY < fromPos.y + h * 0.5 && maxY > fromPos.y - h * 0.5
                    && minZ < fromPos.z + w * 0.5 && maxZ > fromPos.z - w * 0.5;
            }

            static bool CanSeeAnyTargetSide(const TransportItemTarget& target,
                                            EntityLevel& level,
                                            const glm::dvec3& eyePosition) {
                const IBlockAccess* blocks = level.Blocks();
                if (!blocks) return false;
                const glm::dvec3 center(target.pos.x + 0.5, target.pos.y + 0.5,
                                        target.pos.z + 0.5);
                static const glm::dvec3 kDirections[6] = {
                    { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 },
                    { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
                };
                for (const glm::dvec3& direction : kDirections) {
                    // MC clips to the exact face centre (0.5 along the axis);
                    // the epsilon pulls the endpoint just inside the chest's
                    // own cell so the voxel walk can report the chest as the
                    // hit block, the way MC's shape clip does from a face hit.
                    const glm::dvec3 hitTarget = center + (0.5 - 1.0e-3) * direction;
                    const std::optional<glm::ivec3> hit =
                        FirstCollidingBlock(*blocks, eyePosition, hitTarget);
                    if (hit && *hit == target.pos) return true;
                }
                return false;
            }

            bool IsAnotherMobInteractingWithTarget(const TransportItemTarget& target,
                                                   EntityLevel& level) const {
                for (const TransportItemTarget& connected :
                     GetConnectedTargets(level, target)) {
                    if (m_shouldQueueForTarget(connected)) return true;
                }
                return false;
            }

            static bool IsPickingUpItems(const PathfinderMob& body) {
                // MC body.getMainHandItem().isEmpty() — the hand stack lives
                // on CopperGolem (no equipment system).
                auto* golem = dynamic_cast<const CopperGolem*>(&body);
                return golem && golem->GetMainHandItem().IsEmpty();
            }

            static bool MatchesGettingItemsRequirement(const IContainer& container) {
                return !ContainerIsEmpty(container);
            }

            static bool MatchesLeavingItemsRequirement(const PathfinderMob& body,
                                                       const IContainer& container) {
                return ContainerIsEmpty(container)
                    || HasItemMatchingHandItem(body, container);
            }

            static bool HasItemMatchingHandItem(const PathfinderMob& body,
                                                const IContainer& container) {
                auto* golem = dynamic_cast<const CopperGolem*>(&body);
                if (!golem) return false;
                const ItemStack& mainHandItem = golem->GetMainHandItem();
                for (int i = 0; i < container.GetContainerSize(); ++i) {
                    // MC ItemStack.isSameItem — the item alone, no components.
                    if (container.GetItem(i).itemId == mainHandItem.itemId) return true;
                }
                return false;
            }

            void PickUpItems(PathfinderMob& body, IContainer& container) {
                auto* golem = dynamic_cast<CopperGolem*>(&body);
                if (!golem) return;
                golem->SetItemInHand(PickupItemFromContainer(container));
                // MC setGuaranteedDrop(MAINHAND) — CopperGolem::Die drops the
                // hand stack.
                container.SetChanged();
                ClearMemoriesAfterMatchingTargetFound(body);
            }

            void PutDownItem(PathfinderMob& body, IContainer& container) {
                auto* golem = dynamic_cast<CopperGolem*>(&body);
                if (!golem) return;
                const ItemStack itemsLeftAfterVisitingChest =
                    AddItemsToContainer(*golem, container);
                container.SetChanged();
                golem->SetItemInHand(itemsLeftAfterVisitingChest);
                if (itemsLeftAfterVisitingChest.IsEmpty()) {
                    ClearMemoriesAfterMatchingTargetFound(body);
                } else {
                    StopTargetingCurrentTarget(body);
                }
            }

            static ItemStack PickupItemFromContainer(IContainer& container) {
                for (int slot = 0; slot < container.GetContainerSize(); ++slot) {
                    const ItemStack& itemStack = container.GetItem(slot);
                    if (itemStack.IsEmpty()) continue;
                    // MC container.removeItem(slot, min(count, 16)).
                    const int itemCount =
                        std::min(itemStack.count, kTransportedItemMaxStackSize);
                    ItemStack removed = itemStack;
                    removed.count = itemCount;
                    ItemStack rest = itemStack;
                    rest.count -= itemCount;
                    container.SetItem(slot, rest.count > 0 ? rest : ItemStack{});
                    return removed;
                }
                return ItemStack{};
            }

            static ItemStack AddItemsToContainer(const CopperGolem& golem,
                                                 IContainer& container) {
                ItemStack itemStack = golem.GetMainHandItem();
                for (int slot = 0; slot < container.GetContainerSize(); ++slot) {
                    ItemStack containerItemStack = container.GetItem(slot);
                    if (containerItemStack.IsEmpty()) {
                        container.SetItem(slot, itemStack);
                        return ItemStack{};
                    }
                    // MC containerItemStack.getMaxStackSize() — the item's own
                    // ceiling (the per-stack MAX_STACK_SIZE component override
                    // is not consulted, matching the menus' merge paths).
                    const int maxStackSize =
                        ItemRegistry::Get(containerItemStack.itemId).maxStackSize;
                    if (IsSameItemSameComponents(containerItemStack, itemStack)
                        && containerItemStack.count < maxStackSize) {
                        const int countThatCanBeAdded =
                            maxStackSize - containerItemStack.count;
                        const int countToAdd =
                            std::min(countThatCanBeAdded, itemStack.count);
                        containerItemStack.count += countToAdd;
                        // MC subtracts countThatCanBeAdded, not countToAdd —
                        // transcribed as-is: the overshoot drives the count
                        // negative and isEmpty treats that as an empty hand,
                        // exactly as MC's does.
                        itemStack.count -= countThatCanBeAdded;
                        container.SetItem(slot, containerItemStack);
                        if (itemStack.IsEmpty()) return ItemStack{};
                    }
                }
                return itemStack;
            }

            void StopTargetingCurrentTarget(PathfinderMob& body) {
                m_ticksSinceReachingTarget = 0;
                m_target.reset();
                body.GetNavigation().Stop();
                if (Brain* brain = body.GetBrain()) {
                    brain->EraseMemory(MemoryModule::WalkTarget);
                }
            }

            void ClearMemoriesAfterMatchingTargetFound(PathfinderMob& body) {
                StopTargetingCurrentTarget(body);
                // MC erases VISITED_BLOCK_POSITIONS and
                // UNREACHABLE_TRANSPORT_BLOCK_POSITIONS.
                m_visitedPositions.clear();
                m_unreachablePositions.clear();
            }

            void EnterCooldownAfterNoMatchingTargetFound(PathfinderMob& body) {
                StopTargetingCurrentTarget(body);
                if (Brain* brain = body.GetBrain()) {
                    brain->SetMemory(MemoryModule::TransportItemsCooldownTicks,
                                     kIdleCooldown);
                }
                m_visitedPositions.clear();
                m_unreachablePositions.clear();
            }

            void StopInPlace(PathfinderMob& mob) {
                mob.GetNavigation().Stop();
                mob.xxa = 0.0f;
                mob.yya = 0.0f;
                mob.SetSpeed(0.0f);
                mob.velocity = glm::dvec3(0.0, mob.velocity.y, 0.0);
            }

            float m_speedModifier;
            int   m_horizontalSearchDistance;
            int   m_verticalSearchDistance;
            BlockPredicate m_sourceBlockType;
            BlockPredicate m_destinationBlockType;
            std::function<bool(const TransportItemTarget&)> m_shouldQueueForTarget;
            std::function<void(PathfinderMob&)> m_onStartTravelling;
            std::map<ContainerInteractionState, OnTargetReachedInteraction>
                m_onTargetInteractionActions;

            std::optional<TransportItemTarget> m_target;
            TransportItemState m_state = TransportItemState::Travelling;
            std::optional<ContainerInteractionState> m_interactionState;
            int m_ticksSinceReachingTarget = 0;

            // The two position-set memories (see the class comment).
            std::vector<glm::ivec3> m_visitedPositions;
            std::vector<glm::ivec3> m_unreachablePositions;
            int64_t m_visitedExpiry = 0;
            int64_t m_unreachableExpiry = 0;
        };

        // ── CopperGolemAi's wiring (MC's static half) ──────────────────────

        // MC CopperGolemAi.onReachedTargetInteraction(state, sound) — each
        // state's sound (COPPER_GOLEM_ITEM_GET / NO_GET / DROP / NO_DROP, at
        // TICK_TO_PLAY_ON_REACHED_SOUND = 9) waits on a sound system.
        TransportItemsBetweenContainers::OnTargetReachedInteraction
        OnReachedTargetInteraction(CopperGolem::State state) {
            return [state](PathfinderMob& body, const TransportItemTarget& target,
                           int ticksSinceReachingTarget) {
                auto* copperGolem = dynamic_cast<CopperGolem*>(&body);
                if (!copperGolem) return;
                if (ticksSinceReachingTarget == kTickToStartOnReachedInteraction) {
                    // MC container.startOpen(copperGolem) — the chest lid
                    // counter (ContainerOpenersCounter) does not exist yet;
                    // ChestRenderer draws the lid closed, so there is nothing
                    // to animate open.
                    copperGolem->SetOpenedChestPos(target.pos);
                    copperGolem->SetState(state);
                }
                if (ticksSinceReachingTarget
                    == TransportItemsBetweenContainers::kTargetInteractionTime) {
                    // MC container.stopOpen(copperGolem) — the same missing
                    // lid counter.
                    copperGolem->ClearOpenedChestPos();
                }
            };
        }

        std::map<TransportItemsBetweenContainers::ContainerInteractionState,
                 TransportItemsBetweenContainers::OnTargetReachedInteraction>
        GetTargetReachedInteractions() {
            using CIS = TransportItemsBetweenContainers::ContainerInteractionState;
            std::map<CIS, TransportItemsBetweenContainers::OnTargetReachedInteraction> map;
            map[CIS::PickupItem]   = OnReachedTargetInteraction(CopperGolem::State::GettingItem);
            map[CIS::PickupNoItem] = OnReachedTargetInteraction(CopperGolem::State::GettingNoItem);
            map[CIS::PlaceItem]    = OnReachedTargetInteraction(CopperGolem::State::DroppingItem);
            map[CIS::PlaceNoItem]  = OnReachedTargetInteraction(CopperGolem::State::DroppingNoItem);
            return map;
        }

        // MC CopperGolemAi.onTravelling.
        std::function<void(PathfinderMob&)> OnTravelling() {
            return [](PathfinderMob& body) {
                if (auto* copperGolem = dynamic_cast<CopperGolem*>(&body)) {
                    copperGolem->ClearOpenedChestPos();
                    copperGolem->SetState(CopperGolem::State::Idle);
                }
            };
        }

        // MC CopperGolemAi.shouldQueueForTarget.
        std::function<bool(const TransportItemTarget&)> ShouldQueueForTarget() {
            return [](const TransportItemTarget&) {
                // MC: !chestBlockEntity.getEntitiesWithContainerOpen()
                // .isEmpty() — the ContainerOpenersCounter does not exist, so
                // no golem ever queues; the QUEUING machinery above stays MC's
                // for when it does.
                return false;
            };
        }

    } // namespace

    void CopperGolemAi::InitBrain(CopperGolem& golem, Brain& brain) {
        (void)golem;
        // MC CopperGolemAi.MEMORY_TYPES, in declaration order. DOORS_TO_CLOSE
        // is omitted with InteractWithDoor below.
        for (MemoryModule m : { MemoryModule::IsPanicking,
                                MemoryModule::HurtBy,
                                MemoryModule::HurtByEntity,
                                MemoryModule::NearestLivingEntities,
                                MemoryModule::NearestVisibleLivingEntities,
                                MemoryModule::WalkTarget,
                                MemoryModule::LookTarget,
                                MemoryModule::CantReachWalkTargetSince,
                                MemoryModule::Path,
                                MemoryModule::GazeCooldownTicks,
                                MemoryModule::TransportItemsCooldownTicks,
                                MemoryModule::VisitedBlockPositions,
                                MemoryModule::UnreachableTransportBlockPositions }) {
            brain.RegisterMemory(m);
        }

        // MC SENSOR_TYPES: NEAREST_LIVING_ENTITIES, HURT_BY.
        brain.AddSensor(std::make_unique<NearestLivingEntitySensor>());
        brain.AddSensor(std::make_unique<HurtBySensor>());

        // ── CORE (MC initCoreActivity) ─────────────────────────────────────
        // InteractWithDoor.create() is skipped — no brain door behaviour (the
        // goal system's DoorGoals are the door port).
        std::vector<BehaviorPtr> core;
        core.push_back(std::make_unique<AnimalPanic>(kSpeedMultiplierWhenPanicking));
        core.push_back(std::make_unique<LookAtTargetSink>(45, 90));
        core.push_back(std::make_unique<MoveToTargetSink>());
        core.push_back(std::make_unique<CountDownCooldownTicks>(
            MemoryModule::GazeCooldownTicks));
        core.push_back(std::make_unique<CountDownCooldownTicks>(
            MemoryModule::TransportItemsCooldownTicks));
        brain.AddActivity(Activity::Core, 0, std::move(core));

        // ── IDLE (MC initIdleActivity, explicit priority pairs) ────────────
        std::vector<BehaviorPtr> idle;
        idle.push_back(std::make_unique<TransportItemsBetweenContainers>(
            kSpeedMultiplierWhenIdling, &IsTransportSourceBlock,
            &IsTransportDestinationBlock, kTransportItemHorizontalSearchRadius,
            kTransportItemVerticalSearchRadius, GetTargetReachedInteractions(),
            OnTravelling(), ShouldQueueForTarget()));
        // MC SetEntityLookTargetSometimes.create(PLAYER, 6.0F, 40..80).
        idle.push_back(std::make_unique<SetEntityLookTargetSometimes>(6.0f, 40, 80));
        std::vector<GateBehavior::Entry> move;
        move.push_back({ RandomStroll::Stroll(1.0f, 2, 2), 1 });
        move.push_back({ std::make_unique<DoNothing>(30, 60), 1 });
        idle.push_back(MakeRunOne(
            { MemoryCondition{ MemoryModule::WalkTarget, MemoryStatus::ValueAbsent },
              MemoryCondition{ MemoryModule::TransportItemsCooldownTicks,
                               MemoryStatus::ValuePresent } },
            std::move(move)));
        brain.AddActivity(Activity::Idle, 0, std::move(idle));

        brain.SetCoreActivities({ Activity::Core });
        brain.SetDefaultActivity(Activity::Idle);
        brain.UseDefaultActivity();
    }

    void CopperGolemAi::UpdateActivity(CopperGolem& golem) {
        // MC CopperGolemAi.updateActivity.
        if (Brain* brain = golem.GetBrain()) {
            brain->SetActiveActivityToFirstValid({ Activity::Idle });
        }
    }

} // namespace Game

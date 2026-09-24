// File: src/common/world/block/entity/PistonMovingBlockEntity.cpp
#include "common/world/block/entity/PistonMovingBlockEntity.hpp"

#include "common/entity/Entity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/nbt/NbtWrite.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"
#include "common/world/block/piston/PistonBaseBlock.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace Game {

    namespace {

        BlockState PistonHeadState(BlockState movedState, bool isShort) {
            const bool sticky = movedState.Is(BlockID::StickyPiston);
            BlockState s = BlockStates::Default(BlockID::PistonHead);
            s = s.SetIndex(PropertyId::PISTON_TYPE, sticky ? 1 : 0);
            s = WithFacing(s, FacingOf(movedState));
            return WithBool(s, PropertyId::SHORT, isShort);
        }

        // MC PistonMath.getMovementArea.
        AABBd GetMovementArea(const AABBd& aabb, Direction direction, double amount) {
            const int step = (direction == Direction::Up || direction == Direction::South ||
                              direction == Direction::East) ? 1 : -1;
            const double delta = amount * step;
            const double lo = std::min(delta, 0.0);
            const double hi = std::max(delta, 0.0);
            switch (direction) {
                case Direction::West:  return AABBd::FromMinMax({aabb.min.x + lo, aabb.min.y, aabb.min.z}, {aabb.min.x + hi, aabb.max.y, aabb.max.z});
                case Direction::East:  return AABBd::FromMinMax({aabb.max.x + lo, aabb.min.y, aabb.min.z}, {aabb.max.x + hi, aabb.max.y, aabb.max.z});
                case Direction::Down:  return AABBd::FromMinMax({aabb.min.x, aabb.min.y + lo, aabb.min.z}, {aabb.max.x, aabb.min.y + hi, aabb.max.z});
                case Direction::North: return AABBd::FromMinMax({aabb.min.x, aabb.min.y, aabb.min.z + lo}, {aabb.max.x, aabb.max.y, aabb.min.z + hi});
                case Direction::South: return AABBd::FromMinMax({aabb.min.x, aabb.min.y, aabb.max.z + lo}, {aabb.max.x, aabb.max.y, aabb.max.z + hi});
                case Direction::Up:
                default:               return AABBd::FromMinMax({aabb.min.x, aabb.max.y + lo, aabb.min.z}, {aabb.max.x, aabb.max.y + hi, aabb.max.z});
            }
        }

        AABBd MinMax(const AABBd& a, const AABBd& b) {
            return AABBd::FromMinMax(glm::min(a.min, b.min), glm::max(a.max, b.max));
        }

        AABBd Shifted(const AABBd& a, const glm::dvec3& d) {
            return AABBd::FromMinMax(a.min + d, a.max + d);
        }

        // MC PistonMovingBlockEntity.getMovement.
        double GetMovement(const AABBd& aabbToBeOutsideOf, Direction movement, const AABBd& aabb) {
            switch (movement) {
                case Direction::East:  return aabbToBeOutsideOf.max.x - aabb.min.x;
                case Direction::West:  return aabb.max.x - aabbToBeOutsideOf.min.x;
                case Direction::Down:  return aabb.max.y - aabbToBeOutsideOf.min.y;
                case Direction::South: return aabbToBeOutsideOf.max.z - aabb.min.z;
                case Direction::North: return aabb.max.z - aabbToBeOutsideOf.min.z;
                case Direction::Up:
                default:               return aabbToBeOutsideOf.max.y - aabb.min.y;
            }
        }

        // MC moveEntityByPiston: entity.move(MoverType.PISTON, delta) — a
        // direct move for every entity, players included. The server player
        // is moved here as ServerPlayer is in vanilla; the client's own
        // piston tick moves its LocalPlayer through the level hooks below,
        // and the two agree because both run the same tick.
        void MoveEntityByPiston(Entity& entity, double delta, Direction movement) {
            const glm::dvec3 d(delta * StepX(movement), delta * StepY(movement), delta * StepZ(movement));
            entity.Move(d);
        }

        std::vector<AABBd> ShapeBoxes(BlockState state) {
            std::vector<AABBd> out;
            if (state.Block() == BlockID::Air || !BlockRegistry::HasCollision(state.Block())) return out;
            for (const auto& b : BlockRegistry::GetBlockCollisionShapeSet(state)) {
                out.push_back(AABBd::FromMinMax(glm::dvec3(b.min), glm::dvec3(b.max)));
            }
            return out;
        }

        void CollectEntities(ILevelWrite& level, const AABBd& box, std::vector<Entity*>& out) {
            EntityLevel* entities = level.Entities();
            if (!entities) return;
            entities->GetEntitiesInBox(AABB::FromMinMax(glm::vec3(box.min), glm::vec3(box.max)), nullptr, out);
        }

    } // namespace

    float PistonMovingBlockEntity::GetProgress(float a) const {
        if (m_landed) return 1.0f;
        if (a > 1.0f) a = 1.0f;
        return m_progressO + (m_progress - m_progressO) * a;
    }

    BlockState PistonMovingBlockEntity::GetCollisionRelatedBlockState() const {
        const bool isBase = m_movedState.Is(BlockID::Piston) || m_movedState.Is(BlockID::StickyPiston);
        if (!m_extending && m_isSourcePiston && isBase) {
            return PistonHeadState(m_movedState, m_progress > 0.25f);
        }
        return m_movedState;
    }

    // MC moveByPositionAndProgress.
    static AABBd MoveByPositionAndProgress(const glm::ivec3& pos, const AABBd& aabb,
                                           float progress, bool extending, Direction direction) {
        const double currentPosition = extending ? progress - 1.0f : 1.0f - progress;
        return Shifted(aabb, glm::dvec3(pos) + currentPosition * glm::dvec3(StepX(direction), StepY(direction), StepZ(direction)));
    }

    void PistonMovingBlockEntity::MoveCollidedEntities(ILevelWrite& level, const glm::ivec3& pos,
                                                       float newProgress, PistonMovingBlockEntity& self) {
        const Direction movement = self.GetMovementDirection();
        const double deltaProgress = static_cast<double>(newProgress - self.m_progress);
        const std::vector<AABBd> shapeAabbs = ShapeBoxes(self.GetCollisionRelatedBlockState());
        if (shapeAabbs.empty()) return;

        AABBd bounds = shapeAabbs[0];
        for (const AABBd& b : shapeAabbs) bounds = MinMax(bounds, b);
        const AABBd aabb = MoveByPositionAndProgress(pos, bounds, self.m_progress, self.m_extending, self.m_direction);

        std::vector<Entity*> entities;
        const AABBd searchBox = MinMax(GetMovementArea(aabb, movement, deltaProgress), aabb);
        CollectEntities(level, searchBox, entities);

        // The client's LocalPlayer: not a Game::Entity, reached through the
        // level's hooks and pushed by exactly the same arithmetic.
        {
            glm::dvec3 pmin, pmax;
            if (level.GetLocalPlayerBox(pmin, pmax)) {
                AABBd entityAabb = AABBd::FromMinMax(pmin, pmax);
                if (searchBox.Intersects(entityAabb)) {
                    double delta = 0.0;
                    for (const AABBd& shapeAabb : shapeAabbs) {
                        const AABBd movingAABB = GetMovementArea(
                            MoveByPositionAndProgress(pos, shapeAabb, self.m_progress, self.m_extending, self.m_direction),
                            movement, deltaProgress);
                        if (movingAABB.Intersects(entityAabb)) {
                            delta = std::max(delta, GetMovement(movingAABB, movement, entityAabb));
                            if (delta >= deltaProgress) break;
                        }
                    }
                    if (delta > 0.0) {
                        delta = std::min(delta, deltaProgress) + kPushOffset;
                        level.MoveLocalPlayerByPiston(glm::dvec3(delta * StepX(movement), delta * StepY(movement), delta * StepZ(movement)));
                        if (!self.m_extending && self.m_isSourcePiston && level.GetLocalPlayerBox(pmin, pmax)) {
                            entityAabb = AABBd::FromMinMax(pmin, pmax);
                            const AABBd box = AABBd::FromMinMax(glm::dvec3(pos), glm::dvec3(pos) + glm::dvec3(1.0));
                            if (entityAabb.Intersects(box)) {
                                const Direction opposite = Opposite(movement);
                                double d = GetMovement(box, opposite, entityAabb) + kPushOffset;
                                const AABBd inter = AABBd::FromMinMax(glm::max(entityAabb.min, box.min), glm::min(entityAabb.max, box.max));
                                const double deltaIntersected = GetMovement(box, opposite, inter) + kPushOffset;
                                if (std::abs(d - deltaIntersected) < kPushOffset) {
                                    d = std::min(d, deltaProgress) + kPushOffset;
                                    level.MoveLocalPlayerByPiston(glm::dvec3(d * StepX(opposite), d * StepY(opposite), d * StepZ(opposite)));
                                }
                            }
                        }
                    }
                }
            }
        }
        if (entities.empty()) return;

        const bool causeBounce = self.m_movedState.Is(BlockID::SlimeBlock);
        for (Entity* entity : entities) {
            if (!entity || entity->IsRemoved() || entity->IsSpectator()) continue;
            if (causeBounce) {
                glm::dvec3 v = entity->velocity;
                switch (AxisOf(movement)) {
                    case Axis::X: v.x = StepX(movement); break;
                    case Axis::Y: v.y = StepY(movement); break;
                    case Axis::Z: v.z = StepZ(movement); break;
                }
                entity->velocity = v;
            }
            double delta = 0.0;
            for (const AABBd& shapeAabb : shapeAabbs) {
                const AABBd movingAABB = GetMovementArea(
                    MoveByPositionAndProgress(pos, shapeAabb, self.m_progress, self.m_extending, self.m_direction),
                    movement, deltaProgress);
                const AABBd entityAabb = entity->GetAABBd();
                if (movingAABB.Intersects(entityAabb)) {
                    delta = std::max(delta, GetMovement(movingAABB, movement, entityAabb));
                    if (delta >= deltaProgress) break;
                }
            }
            if (delta <= 0.0) continue;
            delta = std::min(delta, deltaProgress) + kPushOffset;
            MoveEntityByPiston(*entity, delta, movement);
            if (!self.m_extending && self.m_isSourcePiston) {
                // fixEntityWithinPistonBase
                const AABBd entityAabb = entity->GetAABBd();
                const AABBd box = AABBd::FromMinMax(glm::dvec3(pos), glm::dvec3(pos) + glm::dvec3(1.0));
                if (entityAabb.Intersects(box)) {
                    const Direction opposite = Opposite(movement);
                    double d = GetMovement(box, opposite, entityAabb) + kPushOffset;
                    const AABBd inter = AABBd::FromMinMax(glm::max(entityAabb.min, box.min), glm::min(entityAabb.max, box.max));
                    const double deltaIntersected = GetMovement(box, opposite, inter) + kPushOffset;
                    if (std::abs(d - deltaIntersected) < kPushOffset) {
                        d = std::min(d, deltaProgress) + kPushOffset;
                        MoveEntityByPiston(*entity, d, opposite);
                    }
                }
            }
        }
    }

    void PistonMovingBlockEntity::MoveStuckEntities(ILevelWrite& level, const glm::ivec3& pos,
                                                    float newProgress, PistonMovingBlockEntity& self) {
        if (!self.m_movedState.Is(BlockID::HoneyBlock)) return;   // isStickyForEntities
        const Direction movement = self.GetMovementDirection();
        if (!IsHorizontal(movement)) return;
        double stickyTop = 1.0;
        for (const AABBd& b : ShapeBoxes(self.m_movedState)) stickyTop = std::max(stickyTop, b.max.y);
        const AABBd aabb = MoveByPositionAndProgress(
            pos, AABBd::FromMinMax({0.0, stickyTop, 0.0}, {1.0, 1.5000010000000001, 1.0}),
            self.m_progress, self.m_extending, self.m_direction);
        const double deltaProgress = static_cast<double>(newProgress - self.m_progress);
        std::vector<Entity*> entities;
        CollectEntities(level, aabb, entities);
        for (Entity* entity : entities) {
            if (!entity || entity->IsRemoved() || entity->IsSpectator() || !entity->onGround) continue;
            const glm::dvec3 p = entity->position;
            const bool within = p.x >= aabb.min.x && p.x <= aabb.max.x && p.z >= aabb.min.z && p.z <= aabb.max.z;
            if (!within) continue;
            MoveEntityByPiston(*entity, deltaProgress, movement);
        }
    }

    void PistonMovingBlockEntity::FinalTick(ILevelWrite& level) {
        if (m_progressO < 1.0f || level.IsClientSide()) {
            m_progress  = 1.0f;
            m_progressO = m_progress;
            const glm::ivec3 pos = GetWorldPos();
            const bool       isSourcePiston = m_isSourcePiston;
            const BlockState movedState     = m_movedState;
            level.RemoveBlockEntity(pos);   // `this` is dead after this line
            if (level.GetBlockState(pos.x, pos.y, pos.z).Is(BlockID::MovingPiston)) {
                BlockState newState;
                if (isSourcePiston) newState = BlockState{};
                else                newState = UpdateFromNeighbourShapes(level, movedState, pos);
                level.SetBlock(pos.x, pos.y, pos.z, newState, World::UpdateFlags::All);
                level.NeighborChanged(pos, newState.Block());
            }
        }
    }

    void PistonMovingBlockEntity::PreRemoveSideEffects(ILevelWrite& level, const glm::ivec3&, BlockState) {
        FinalTick(level);
    }

    void PistonMovingBlockEntity::Tick(World* world, float /*deltaTime*/) {
        if (world) Tick(*world);
    }

    // MC PistonMovingBlockEntity.tick, on either side. The client keeps a
    // finished cell for five more ticks (deathTicks) so the server's final
    // block write normally lands first; if it does not, the client writes
    // the carried block itself, as vanilla's does.
    void PistonMovingBlockEntity::Tick(ILevelWrite& level) {
        if (m_landed) return;
        const glm::ivec3 pos = GetWorldPos();
        m_lastTicked = level.GameTime();
        m_progressO  = m_progress;
        if (m_progressO >= 1.0f) {
            if (level.IsClientSide() && m_deathTicks < 5) {
                ++m_deathTicks;
                return;
            }
            const BlockState movedState = m_movedState;
            if (level.IsClientSide()) {
                // The client's write over the moving cell lands this entity
                // (ClientChunkManager::SetBlockLocal) instead of removing it,
                // so it is still drawn while the mesh catches up. A cell that
                // is no longer ours just lets go.
                if (!level.GetBlockState(pos.x, pos.y, pos.z).Is(BlockID::MovingPiston)) {
                    level.RemoveBlockEntity(pos);
                    return;
                }
            } else {
                level.RemoveBlockEntity(pos);   // `this` is dead after this line
            }
            if (level.GetBlockState(pos.x, pos.y, pos.z).Is(BlockID::MovingPiston)) {
                BlockState newState = UpdateFromNeighbourShapes(level, movedState, pos);
                if (newState.Block() == BlockID::Air) {
                    // setBlock(pos, movedState, 340) then updateOrDestroy → destroy with drops.
                    level.SetBlock(pos.x, pos.y, pos.z, movedState, 256 | 64 | 16 | 4);
                    level.DestroyBlock(pos, true);
                } else {
                    if (newState.HasProperty(PropertyId::WATERLOGGED) && BoolOf(newState, PropertyId::WATERLOGGED)) {
                        newState = WithBool(newState, PropertyId::WATERLOGGED, false);
                    }
                    level.SetBlock(pos.x, pos.y, pos.z, newState, 64 | 2 | 1);
                    level.NeighborChanged(pos, newState.Block());
                }
            }
            return;
        }
        const float newProgress = m_progress + 0.5f;
        MoveCollidedEntities(level, pos, newProgress, *this);
        MoveStuckEntities(level, pos, newProgress, *this);
        m_progress = std::min(newProgress, 1.0f);
        MarkDirty();
    }

    void PistonMovingBlockEntity::Save(Network::PacketBuffer& out) const {
        out.WriteVarInt(m_movedState.RawId());
        out.WriteByte(static_cast<uint8_t>(m_direction));
        out.WriteByte(m_extending ? 1 : 0);
        out.WriteByte(m_isSourcePiston ? 1 : 0);
        out.WriteFloat(m_progress);
        out.WriteFloat(m_progressO);
    }

    void PistonMovingBlockEntity::Load(Network::PacketReader& in) {
        if (!in.HasMore()) return;
        m_movedState     = BlockState::FromRawId(in.ReadVarInt());
        m_direction      = static_cast<Direction>(in.ReadByte() % 6);
        m_extending      = in.ReadByte() != 0;
        m_isSourcePiston = in.ReadByte() != 0;
        m_progress       = in.ReadFloat();
        m_progressO      = in.ReadFloat();
    }

    void PistonMovingBlockEntity::WriteNbt(Nbt::Writer& w) const {
        // MC: blockState (BlockState.CODEC), facing (LEGACY_ID_CODEC =
        // the 3D data value), progress, extending, source.
        const BlockID id = m_movedState.Block();
        w.BeginCompound("blockState");
        w.String("Name", "minecraft:" + BlockRegistry::Get(id).registrySlug);
        const uint16_t propCount = BlockStates::PropertyCount(id);
        if (propCount > 0) {
            w.BeginCompound("Properties");
            for (uint16_t slot = 0; slot < propCount; ++slot) {
                const PropertyId prop = BlockStates::PropertyAt(id, slot);
                w.String(BlockStates::PropertyName(prop), m_movedState.GetName(prop));
            }
            w.EndCompound();
        }
        w.EndCompound();
        w.Int("facing", static_cast<int32_t>(m_direction));
        w.Float("progress", m_progressO);
        w.Bool("extending", m_extending);
        w.Bool("source", m_isSourcePiston);
    }

} // namespace Game

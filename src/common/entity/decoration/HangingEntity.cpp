// File: src/common/entity/decoration/HangingEntity.cpp
#include "common/entity/decoration/HangingEntity.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/block/LegacySolid.hpp"
#include "common/world/block/RedstoneFamilies.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

#include <cmath>
#include <vector>

namespace Game {

    HangingEntity::HangingEntity(EntityTypeId type, EntityLevel* level)
        : BlockAttachedEntity(type, level) {}

    // ── Geometry ───────────────────────────────────────────────────────────

    void HangingEntity::RecalculateBoundingBox() {
        const AABBd box = CalculateBoundingBox(m_pos, m_direction);
        position = (box.min + box.max) * 0.5;
        oldPosition = position;
        SetFixedBoundingBox(box);
    }

    void HangingEntity::SetDirection(Direction direction) {
        if (!IsHorizontal(direction)) return;   // MC Validate.isTrue(horizontal)
        m_direction = direction;
        xRot = xRotO = 0.0f;
        yRot = yRotO = ToYRot(direction);
        yBodyRot = yBodyRotO = yRot;
        yHeadRot = yHeadRotO = yRot;
        RecalculateBoundingBox();
    }

    void HangingEntity::SetHangingPos(const glm::ivec3& pos) {
        m_pos = pos;
        RecalculateBoundingBox();
    }

    void HangingEntity::SyncFromNetwork() {
        // The facing: MC's setDirection writes xRot = -90 × step for a
        // ceiling-facing (up) frame and +90 for a floor-facing (down) one,
        // yRot for a wall.
        if (xRot <= -45.0f)     m_direction = Direction::Up;
        else if (xRot >= 45.0f) m_direction = Direction::Down;
        else                    m_direction = FromYRot(yRot);
        // The cell: the centre the server sent, minus the box's offset from
        // its cell's centre (the same offset for every cell).
        const AABBd probe = CalculateBoundingBox(glm::ivec3(0), m_direction);
        const glm::dvec3 offset = (probe.min + probe.max) * 0.5 - glm::dvec3(0.5);
        const glm::dvec3 cellCenter = position - offset;
        m_pos = glm::ivec3(static_cast<int>(std::floor(cellCenter.x)),
                           static_cast<int>(std::floor(cellCenter.y)),
                           static_cast<int>(std::floor(cellCenter.z)));
        RecalculateBoundingBox();
    }

    // ── Survival ───────────────────────────────────────────────────────────

    bool HangingEntity::HasLevelCollision(const AABBd& box) const {
        // The box lies flush on the wall's face, so the test box is pulled
        // in by a hair — touching is not overlapping. Kept in double: a
        // float box far from the origin cannot hold a 1e-5 inset.
        if (!m_level || !m_level->Blocks()) return true;
        constexpr double kEps = 1.0e-5;
        const AABBd pop = AABBd::FromMinMax(box.min + glm::dvec3(kEps), box.max - glm::dvec3(kEps));
        PhysicsContext phys;
        phys.blockAccess = m_level->Blocks();
        return CollidesAt(pop, phys);
    }

    bool HangingEntity::CanCoexist(bool allowIntersectingSameType) const {
        if (!m_level) return false;
        const AABBd pop = PopBox();
        std::vector<Entity*> others;
        // The level buckets entities by the chunk of their POSITION, and a
        // canvas up to 16 wide reaches 8 blocks past its centre into the
        // next chunk — so the query is widened by that much (MC's section
        // walk pads its box the same way) and the real test is the
        // Intersects(pop) below.
        constexpr float kReach = 8.0f;
        AABB query;
        query.min = glm::vec3(pop.min) - glm::vec3(kReach, 0.0f, kReach);
        query.max = glm::vec3(pop.max) + glm::vec3(kReach, 0.0f, kReach);
        m_level->GetEntitiesInBox(query, this, others);
        for (const Entity* other : others) {
            const auto* hanging = dynamic_cast<const HangingEntity*>(other);
            if (!hanging || hanging == this || hanging->IsRemoved()) continue;
            const bool intersectsSameType = !allowIntersectingSameType && hanging->GetType() == GetType();
            const bool isSameDirection = hanging->GetDirection() == m_direction;
            if ((intersectsSameType || isSameDirection) && hanging->GetAABBd().Intersects(pop)) return false;
        }
        return true;
    }

    bool HangingEntity::Survives() const {
        return SurvivesWith(/*requireFullSupport=*/true);
    }

    bool HangingEntity::StillHangs() const {
        return SurvivesWith(/*requireFullSupport=*/true);
    }

    bool HangingEntity::SurvivesWith(bool requireFullSupport) const {
        if (!m_level || !m_level->Blocks()) return false;
        if (HasLevelCollision(PopBox())) return false;

        // calculateSupportBox: the box pushed half a block into the wall and
        // deflated by 1e-7; every cell it touches must be isSolid() or a diode
        // (MC's allMatched) — or, with `requireFullSupport` off, just one of
        // them (anyMatched).
        const IBlockAccess& blocks = *m_level->Blocks();
        const AABBd box = GetAABBd();
        const glm::dvec3 shift = -Step(m_direction) * 0.5;
        const glm::dvec3 lo = box.min + shift + glm::dvec3(1.0e-7);
        const glm::dvec3 hi = box.max + shift - glm::dvec3(1.0e-7);
        bool anySupport = false;
        for (int x = static_cast<int>(std::floor(lo.x)); x <= static_cast<int>(std::floor(hi.x)); ++x) {
            for (int y = static_cast<int>(std::floor(lo.y)); y <= static_cast<int>(std::floor(hi.y)); ++y) {
                for (int z = static_cast<int>(std::floor(lo.z)); z <= static_cast<int>(std::floor(hi.z)); ++z) {
                    const BlockState state = blocks.GetBlockState(x, y, z);
                    const bool supporting = IsLegacySolid(state) || IsDiodeBlock(state.Block());
                    if (supporting) anySupport = true;
                    else if (requireFullSupport) return false;
                }
            }
        }
        if (!anySupport) return false;
        return CanCoexist(false);
    }

    // ── Breaking ───────────────────────────────────────────────────────────
    // (Tick, Hurt, SkipAttackInteraction and Kill are BlockAttachedEntity's.)

    void HangingEntity::SpawnAtLocation(const ItemStack& stack, float yOffs) {
        if (!m_level || stack.IsEmpty()) return;
        const glm::dvec3 at(position.x + StepX(m_direction) * 0.15, position.y + yOffs,
                            position.z + StepZ(m_direction) * 0.15);
        m_level->SpawnItemStackDrop(at, stack);
    }

} // namespace Game

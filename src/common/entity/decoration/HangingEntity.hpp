// File: src/common/entity/decoration/HangingEntity.hpp
//
// MC net.minecraft.world.entity.decoration.HangingEntity — what a painting
// and an item frame share on top of BlockAttachedEntity (the cell, the
// 100-tick "does my wall still hold me" check, breaking): the way they face
// and a box derived from the cell and the facing.
//
// Same architecture note as ArmorStand / EndCrystal: MC's hanging entities
// are plain Entities; here they ride the Mob pipeline (Mob's NoAiTag) because
// the tracker, the wire, the NBT and the client store are Mob-shaped. Tick
// replaces Mob::Tick wholesale — a hanging entity never moves.
//
// GEOMETRY
//   `m_pos` is the cell the entity hangs IN (the one in front of the wall),
//   `m_direction` the way it faces (away from the wall). The subclass turns
//   the two into a box (CalculateBoundingBox); `position` is that box's
//   centre and the box itself is the entity's fixed bounding box
//   (Entity::SetFixedBoundingBox).
//
// WIRE
//   The facing rides the rotation MC's setDirection stamps — yRot = 2D value
//   × 90 for a wall, xRot = ∓90 for a floor / ceiling (item frames) — and
//   the client rebuilds facing, cell and box from the position and rotation
//   it is sent (SyncFromNetwork), so nothing else has to travel.
#pragma once

#include "common/entity/decoration/BlockAttachedEntity.hpp"
#include "common/entity/Item.hpp"
#include "common/world/block/Direction.hpp"

#include <glm/glm.hpp>

namespace Game {

    class HangingEntity : public BlockAttachedEntity {
    public:
        // MC HangingEntity.OFFSET: the canvas's offset from its cell's centre
        // toward the wall. (The survival re-check interval is
        // BlockAttachedEntity::kCheckInterval.)
        static constexpr double kShiftToBlockWall = 0.46875;

        // ── State ──────────────────────────────────────────────────────────
        const glm::ivec3& HangingPos() const { return m_pos; }
        Direction GetDirection() const { return m_direction; }

        // MC HangingEntity.setDirection: facing, rotation, box. A painting
        // takes walls only; an item frame overrides it for floors and
        // ceilings.
        virtual void SetDirection(Direction direction);
        // MC BlockAttachedEntity.setPos — the cell, then the box from it.
        void SetHangingPos(const glm::ivec3& pos);

        // MC HangingEntity.survives: nothing solid in the pop box, every cell
        // of the wall behind solid (isSolid, or a repeater / comparator), and
        // canCoexist(false).
        bool Survives() const override;
        // The 100-tick re-check of a hanging entity already in place (MC
        // runs survives() here too). A painting overrides it — see Painting.
        virtual bool StillHangs() const;

        // (DropItem — MC dropItem(level, causedBy) — is BlockAttachedEntity's.)
        virtual void PlayPlacementSound() = 0;

    protected:
        // The 100-tick re-check runs StillHangs (a painting's relaxed rule).
        bool SurvivesCheck() const override { return StillHangs(); }

        HangingEntity(EntityTypeId type, EntityLevel* level);

        // MC calculateBoundingBox(pos, direction).
        virtual AABBd CalculateBoundingBox(const glm::ivec3& pos, Direction direction) const = 0;
        // MC getPopBox — the box tested against blocks in Survives.
        virtual AABBd PopBox() const { return GetAABBd(); }
        // MC HangingEntity.recalculateBoundingBox: the box, and `position` at
        // its centre.
        void RecalculateBoundingBox();
        // MC canCoexist(allowIntersectingSameType): no other hanging entity
        // in the pop box facing the same way — nor, unless allowed, any of
        // this one's own type.
        bool CanCoexist(bool allowIntersectingSameType) const;
        // HangingEntity.survives with the support rule chosen: every cell
        // behind solid (MC), or at least one.
        bool SurvivesWith(bool requireFullSupport) const;
        // MC hasLevelCollision(popBox).
        bool HasLevelCollision(const AABBd& box) const;
        // MC HangingEntity.spawnAtLocation: nudged 0.15 off the wall.
        void SpawnAtLocation(const ItemStack& stack, float yOffs = 0.0f);

        // Client: the facing from the rotation the server stamped, the cell
        // from the centre it sent (CalculateBoundingBox's offset from the
        // cell's centre, undone), then the box. Called once the subclass's
        // own synced state (a painting's variant) is in.
        void SyncFromNetwork();

        static glm::dvec3 Step(Direction d) {
            return glm::dvec3(StepX(d), StepY(d), StepZ(d));
        }

        Direction  m_direction = Direction::South;   // MC DEFAULT_DIRECTION
    };

} // namespace Game

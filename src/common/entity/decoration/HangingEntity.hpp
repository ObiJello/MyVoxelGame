// File: src/common/entity/decoration/HangingEntity.hpp
//
// MC net.minecraft.world.entity.decoration.HangingEntity, with its base
// BlockAttachedEntity folded in — what a painting and an item frame share:
// the cell they hang in, the way they face, a box derived from the two, the
// 100-tick "does my wall still hold me" check, and breaking.
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

#include "common/entity/Mob.hpp"
#include "common/entity/Item.hpp"
#include "common/world/block/Direction.hpp"

#include <glm/glm.hpp>

namespace Game {

    class HangingEntity : public Mob {
    public:
        // MC HangingEntity / BlockAttachedEntity constants: the canvas's
        // offset from its cell's centre toward the wall, and the survival
        // re-check interval.
        static constexpr double kShiftToBlockWall = 0.46875;
        static constexpr int    kCheckInterval = 100;

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
        virtual bool Survives() const;

        // MC dropItem(level, causedBy): the break sound and the drops.
        virtual void DropItem(Entity* causedBy) = 0;
        virtual void PlayPlacementSound() = 0;

        // ── Entity hooks ───────────────────────────────────────────────────
        void Tick() override;
        // MC BlockAttachedEntity.hurtServer: past the guards, any hit takes
        // the entity down with its drops.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;
        // MC BlockAttachedEntity.skipAttackInteraction: a player's punch is
        // hurtOrSimulate(playerAttack, 0), and the attack goes no further.
        bool SkipAttackInteraction(Entity& source) override;
        // MC push / move: anything that would move it breaks it instead; an
        // explosion's push arrives after its damage already has.
        void Knockback(double, double, double) override {}
        bool IsPickable() const override { return true; }
        bool IsPushable() const override { return false; }
        bool IsAffectedByPotions() const override { return false; }
        bool IsAttackable() const override { return true; }
        // MC: blocksBuilding stays false for a hanging entity.
        bool BlocksBuilding() const override { return false; }
        void CheckDespawn() override {}
        bool RemoveWhenFarAway(double) const override { return false; }

    protected:
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
        // MC hasLevelCollision(popBox).
        bool HasLevelCollision(const AABBd& box) const;
        // MC HangingEntity.spawnAtLocation: nudged 0.15 off the wall.
        void SpawnAtLocation(const ItemStack& stack, float yOffs = 0.0f);
        // MC kill(level, attributedTo): removed as KILLED.
        void Kill();

        // Client: the facing from the rotation the server stamped, the cell
        // from the centre it sent (CalculateBoundingBox's offset from the
        // cell's centre, undone), then the box. Called once the subclass's
        // own synced state (a painting's variant) is in.
        void SyncFromNetwork();

        static glm::dvec3 Step(Direction d) {
            return glm::dvec3(StepX(d), StepY(d), StepZ(d));
        }

        glm::ivec3 m_pos{0};
        Direction  m_direction = Direction::South;   // MC DEFAULT_DIRECTION
        int        m_ticksSinceLastCheck = 0;
    };

} // namespace Game

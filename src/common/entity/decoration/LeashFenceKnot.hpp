// File: src/common/entity/decoration/LeashFenceKnot.hpp
//
// MC net.minecraft.world.entity.decoration.LeashFenceKnotEntity — the knot a
// lead makes round a fence post.
//
// A BlockAttachedEntity, as in MC: the fence cell it hangs on, the 100-tick
// "is the fence still there" check (BlockAttachedEntity::Tick), and breaking
// on any hit — the punch drops nothing itself; the leads tied to it drop from
// their mobs the next tick, when they see their holder gone.
//
// GEOMETRY
//   MC's recalculateBoundingBox puts the knot's position (its feet) at the
//   fence cell's (x + 0.5, y + 0.375, z + 0.5) and gives it the type's
//   0.375 x 0.5 box from there — which is exactly the engine's ordinary
//   width/height box around `position`, so `position` is MC's and no fixed
//   box is needed.
//
// NEVER SAVED (EntityType .noSave()): a leashed mob remembers the fence
// ("leash": [x, y, z]) and recreates the knot when it loads
// (Leash::GetOrCreateKnot, MC restoreLeashFromSave).
//
// WIRE
//   Position and a zero variant byte. The client rebuilds the fence cell
//   from the position when the byte is applied (the knot's feet sit 0.375
//   into the cell, so the cell is the position floored).
//
// LOOKUP
//   MC finds a fence's knot with getEntitiesOfClass over the level, which
//   sees an entity the moment addFreshEntity returns. This engine's
//   AddFreshEntity queues until the manager's next absorb, so a second mob
//   restored onto the same fence in the same tick would not see the first's
//   new knot and would make another. Every knot therefore registers itself
//   (constructor to destructor) and Find() answers from that registry — the
//   answer MC's query gives, queued or not.
#pragma once

#include "common/entity/decoration/BlockAttachedEntity.hpp"

#include <memory>

namespace Game {

    class LeashFenceKnot final : public BlockAttachedEntity {
    public:
        // MC LeashFenceKnotEntity.OFFSET_Y.
        static constexpr double kOffsetY = 0.375;

        explicit LeashFenceKnot(EntityLevel* level);
        ~LeashFenceKnot() override;

        // MC `new LeashFenceKnotEntity(level, pos)`: on the fence at `pos`,
        // not yet added to the level.
        static std::unique_ptr<LeashFenceKnot> Create(EntityLevel* level, const glm::ivec3& pos);

        // The live, not-removed knot on the fence at `pos` in `level`, if any
        // (MC LeashFenceKnotEntity.getKnot). See LOOKUP above.
        static LeashFenceKnot* Find(const EntityLevel* level, const glm::ivec3& pos);

        // MC BlockAttachedEntity.setPos + recalculateBoundingBox.
        void SetKnotPos(const glm::ivec3& pos);

        // MC LeashFenceKnotEntity.getRopeHoldPosition: position + 0.2 up. A
        // knot never moves, so the partial tick changes nothing.
        glm::dvec3 GetRopeHoldPosition() const { return position + glm::dvec3(0.0, 0.2, 0.0); }

        // MC survives(): the cell still holds a #fences block.
        bool Survives() const override;

        // MC dropItem: no item — a knot is only the leads tied to it, which
        // drop from their mobs. Just the untie sound.
        void DropItem(Entity* causedBy) override;
        // MC playPlacementSound: LEAD_TIED.
        void PlayPlacementSound();

        // MC notifyLeasheeRemoved: the last lead off, the knot goes.
        void NotifyLeasheeRemoved(Mob& leashee) override;

        // EntityType.noSave().
        bool CanSerialize() const override { return false; }

        // Entity.getSoundSource — NEUTRAL (the knot's type has no row of its
        // own in the generated sound table).
        SoundSource GetSoundSource() const override { return SoundSource::Neutral; }

        // The wire (see WIRE above).
        uint8_t GetVariantByte() const override { return 0; }
        void    SetVariantByte(uint8_t) override;
    };

} // namespace Game

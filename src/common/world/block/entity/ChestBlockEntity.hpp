// File: src/common/world/block/entity/ChestBlockEntity.hpp
//
// Per-cell chest state. Stores the cardinal `facing` direction set at
// placement time so the renderer can rotate the chest model to face the
// player who placed it. Mirrors MC's `ChestBlock.FACING` blockstate
// property; in MC the BE reads it back from the blockstate. We store it
// directly on the BE since our blocks are flat.
//
// One class backs Chest, TrappedChest, AND EnderChest — they share their
// renderer in MC (BlockEntityRenderers.java registers ChestRenderer::new for
// all three). The variant is encoded in m_blockId (inherited from
// BlockEntity).
//
// Holds the 27-slot inventory (MC ChestBlockEntity.items). Save/Load of the
// contents comes from BaseContainerBlockEntity.
//
// Future state to add as the corresponding stages ship:
//   - openersCounter          (Stage 4, lid animation)
//   - chestLidController      (Stage 4)
#pragma once

#include "BaseContainerBlockEntity.hpp"
#include "common/network/PacketRegistry.hpp"

namespace Game {

    // Cardinal direction enum. Matches MC's Direction ordering for
    // horizontal directions; FacingToYRot returns the Y-rotation needed
    // to align the model's +Z (lock side) with this direction.
    enum class HorizontalDirection : uint8_t {
        North = 0,   // -Z
        South = 1,   // +Z
        West  = 2,   // -X
        East  = 3,   // +X
    };

    class ChestBlockEntity : public BaseContainerBlockEntity {
    public:
        // MC ChestBlockEntity is a 27-slot container (a double chest is two of
        // them joined by a CompoundContainer, not one 54-slot BE).
        static constexpr int SLOT_COUNT = 27;

        ChestBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BaseContainerBlockEntity(type, worldPos, blockId, SLOT_COUNT) {}

        // Auto-close lid arrives with Stage 4; nothing to tick yet.
        bool NeedsTicking() const override { return false; }

        // Facing deliberately does NOT live here. In vanilla, orientation is a
        // blockstate property and a chest's block entity carries none — the
        // whole field set is items + openersCounter + chestLidController
        // (ChestBlockEntity.java:33-35), and ChestRenderer reads the angle off
        // the block (ChestRenderer.java:67). We match that: the facing is on the
        // block's state, written at placement by the shared placement table and
        // replicated with the block itself, so it needs no separate BE sync and
        // survives a world reload with the chunk rather than alongside it.
        //
        // Save/Load are BaseContainerBlockEntity's — they write the 27 stacks.
        // The old placeholder byte is gone; overriding them here again would
        // silently drop every chest's contents on save.
    };

    // Convert a HorizontalDirection to the Y-rotation in radians needed
    // to point the chest's lock (+Z face in the model) along that
    // direction.
    //
    // glm's RH rotation around +Y produces the matrix
    //     [ cosθ  0  sinθ ]
    //     [  0    1   0   ]
    //     [-sinθ  0  cosθ ]
    // Applied to (0,0,1) (the +Z lock direction) it gives (sinθ, 0, cosθ):
    //   θ = 0     → +Z = south
    //   θ = π/2   → +X = east
    //   θ = π     → -Z = north
    //   θ = -π/2  → -X = west
    // Earlier this table had East/West swapped — symptom was "two opposite
    // ways face me, two face away" because π and 0 are symmetric under the
    // sign flip but ±π/2 are not.
    inline float ChestFacingToYRot(HorizontalDirection d) {
        switch (d) {
            case HorizontalDirection::South: return  0.0f;
            case HorizontalDirection::East:  return  1.5707963f;
            case HorizontalDirection::North: return  3.1415927f;
            case HorizontalDirection::West:  return -1.5707963f;
        }
        return 0.0f;
    }

} // namespace Game

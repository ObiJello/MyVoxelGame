// File: src/common/world/block/entity/SkyFacesBlockEntity.hpp
//
// Engine block entity (`obeycraft:sky_faces`): which faces of a door open
// onto the sky the way a sky block's do (Render::SkyBlockRenderer). Holding
// a sky block with the sneak and sprint keys both down and using it on a
// door's face toggles that face — on both halves of the door
// (PlayerSession::HandleUseItemOn, UseItemOnC2SPacket::skyFaceGesture).
//
// ── The door frame ──────────────────────────────────────────────────────
// The mask is kept in the DOOR's frame, not the world's, so a sky face stays
// on the same side of the slab as the door swings: bit d (MC Direction
// order) is face d of the door as it stands closed facing east — the plate
// on the west edge of its cell (models/block/door_bottom_left.json, element
// x 0..3), west its outer face, east its inner one, north/south its edges.
// DoorQuarterTurns gives the clockwise (seen from above) quarter turns that
// carry that frame to the world for any door state: the closed variant's
// own y rotation (blockstates/*_door.json), plus a quarter turn
// counter-clockwise (hinge left) or clockwise (hinge right) for an open
// door, the way the slab swings about its hinge. (MC's *_open models are the
// closed element under a different y rotation with mirrored UVs — their
// model-local faces are not the physical ones, so the frame cannot be read
// off the model.) Down and up do not turn; the lower half has no up face and
// the upper half no down face, so one mask serves both halves.
//
// ── Lazy ────────────────────────────────────────────────────────────────
// Like the shared crafting table's, the entity is LAZY
// (BlockEntityTypes::LazyForBlock): no door gets one when placed or
// generated, and nothing that asks HasBlockEntity — the mesher, pistons,
// placement — sees a door differently. The session creates it on the first
// sky face and removes it when the last one is turned back. World::SetBlock
// drops it with the door, and keeps it while the cell stays a door (a copper
// door weathering or being waxed). A world without sky faces stays vanilla
// on disk.
#pragma once

#include "BlockEntity.hpp"
#include "../Direction.hpp"

#include <cstdint>

namespace Game {

    namespace SkyFaces {

        // Whether a block's faces can be opened onto the sky: every door.
        bool AppliesTo(BlockID id);

        // Clockwise quarter turns (0..3, seen from above) carrying the door
        // frame to the world for this door state. See the header note.
        int DoorQuarterTurns(BlockState state);

        // A face of the door in `state`: world → door frame, and back.
        Direction ToDoorFrame(BlockState state, Direction worldFace);
        Direction ToWorld(BlockState state, Direction frameFace);

        constexpr uint8_t Bit(Direction d) { return static_cast<uint8_t>(1u << static_cast<int>(d)); }
        constexpr uint8_t kAllFaces = 0x3F;

    } // namespace SkyFaces

    class SkyFacesBlockEntity : public BlockEntity {
    public:
        SkyFacesBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BlockEntity(type, worldPos, blockId) {}

        // Door-frame face bits (SkyFaces::Bit).
        uint8_t Mask() const { return m_mask; }
        void SetMask(uint8_t mask) { m_mask = static_cast<uint8_t>(mask & SkyFaces::kAllFaces); }

        // Binary form (the wire): the mask byte. Disk is BlockEntityNbt's — a
        // `SkyFaces` byte tag.
        void Save(Network::PacketBuffer& out) const override;
        void Load(Network::PacketReader& in) override;

    private:
        uint8_t m_mask = 0;
    };

} // namespace Game

// File: src/common/network/packets/game/PlayerActionC2SPacket.hpp
//
// Client → server: discrete player actions (release a held-use item, drop the
// held stack, swap main/off hand, block-dig stages).
//
// Mirrors MC ServerboundPlayerActionPacket. Action ordinals MUST match
// ServerboundPlayerActionPacket.java:66-74 (writeEnum encodes the ordinal).
//
// Wire order per :35-40:
//   writeEnum(action)        → VarInt
//   writeBlockPos(pos)       → MC packs the BlockPos into one long; we write
//                              3×int32 per house convention (same documented
//                              deviation as UseItemOnC2SPacket's hit fields)
//   writeByte(direction)     → face 3D data value (0=down..5=east); 0 for
//                              non-block actions (MC sends Direction.DOWN)
//   writeVarInt(sequence)    → shared interaction sequence (0 for actions MC
//                              doesn't sequence, e.g. RELEASE_USE_ITEM sends
//                              through with the dig-ack path only for the
//                              DESTROY_BLOCK stages)
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    // Ordinals mirror ServerboundPlayerActionPacket.Action (:66-74).
    // PERFORM_RESPAWN is our extension — MC ships it as ServerboundClientCommand
    // Action.PERFORM_RESPAWN; we append it here instead of a new packet.
    enum class PlayerAction : uint8_t {
        START_DESTROY_BLOCK    = 0,
        ABORT_DESTROY_BLOCK    = 1,
        STOP_DESTROY_BLOCK     = 2,
        DROP_ALL_ITEMS         = 3,
        DROP_ITEM              = 4,
        RELEASE_USE_ITEM       = 5,
        SWAP_ITEM_WITH_OFFHAND = 6,
        STAB                   = 7,
        PERFORM_RESPAWN        = 8,
    };

    struct PlayerActionC2SPacket {
        PlayerAction action    = PlayerAction::RELEASE_USE_ITEM;
        int32_t      blockX    = 0;  // BlockPos.ZERO for non-block actions (:24-26)
        int32_t      blockY    = 0;
        int32_t      blockZ    = 0;
        uint8_t      direction = 0;  // Direction.DOWN default
        uint32_t     sequence  = 0;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const PlayerActionC2SPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteVarInt(static_cast<uint32_t>(packet.action)); // :36
            buffer.WriteInt(static_cast<uint32_t>(packet.blockX));    // :37 (3×int32, see header)
            buffer.WriteInt(static_cast<uint32_t>(packet.blockY));
            buffer.WriteInt(static_cast<uint32_t>(packet.blockZ));
            buffer.WriteByte(packet.direction);                       // :38
            buffer.WriteVarInt(packet.sequence);                      // :39
            return buffer.GetData();
        }

        inline PlayerActionC2SPacket DeserializePlayerActionC2S(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            PlayerActionC2SPacket packet;
            packet.action    = static_cast<PlayerAction>(reader.ReadVarInt());
            packet.blockX    = static_cast<int32_t>(reader.ReadInt());
            packet.blockY    = static_cast<int32_t>(reader.ReadInt());
            packet.blockZ    = static_cast<int32_t>(reader.ReadInt());
            packet.direction = reader.ReadByte();
            packet.sequence  = reader.ReadVarInt();
            return packet;
        }

    } // namespace Serialization

} // namespace Network

// File: src/common/network/packets/game/ShoulderParrotsS2CPacket.hpp
//
// Server → every client: what sits on a player's shoulders.
//
// MC keeps this as two synched entity-data slots on Player,
// DATA_SHOULDER_PARROT_LEFT / _RIGHT (OptionalInt: the Parrot.Variant id of
// a parrot on that shoulder, empty for none) — the server writes them
// whenever ServerPlayer.setShoulderEntityLeft/Right changes the saved tag,
// and every tracking client (the player's own included) draws them through
// ParrotOnShoulderLayer. Players here are not tracked entities with a
// metadata stream, so the pair rides this packet: broadcast on every
// change, and sent for every player to a joining client.
//
// Wire: VarInt playerId, byte left, byte right — each the variant id
// (0..4) or 0xFF for an empty shoulder.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct ShoulderParrotsS2CPacket {
        static constexpr int8_t kNone = -1;

        uint32_t playerId = 0;
        int8_t   left  = kNone;   // Parrot.Variant id, or kNone
        int8_t   right = kNone;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const ShoulderParrotsS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteVarInt(packet.playerId);
            buffer.WriteByte(static_cast<uint8_t>(packet.left));
            buffer.WriteByte(static_cast<uint8_t>(packet.right));
            return buffer.GetData();
        }

        inline ShoulderParrotsS2CPacket DeserializeShoulderParrotsS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            ShoulderParrotsS2CPacket packet;
            packet.playerId = reader.ReadVarInt();
            packet.left  = static_cast<int8_t>(reader.ReadByte());
            packet.right = static_cast<int8_t>(reader.ReadByte());
            return packet;
        }

    } // namespace Serialization

} // namespace Network

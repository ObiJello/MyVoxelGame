// File: src/common/network/packets/game/SetHealthS2CPacket.hpp
//
// Server → client: authoritative health / hunger / saturation triple. Sent on
// change (dirty-checked in PlayerSession::Tick) and on the first PLAYING tick.
//
// Mirrors MC ClientboundSetHealthPacket field-for-field, in MC's write order:
//   writeFloat(health), writeVarInt(food), writeFloat(saturation)
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct SetHealthS2CPacket {
        float    health     = 20.0f;
        uint32_t food       = 20;
        float    saturation = 5.0f;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const SetHealthS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteFloat(packet.health);
            buffer.WriteVarInt(packet.food);
            buffer.WriteFloat(packet.saturation);
            return buffer.GetData();
        }

        inline SetHealthS2CPacket DeserializeSetHealthS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            SetHealthS2CPacket packet;
            packet.health     = reader.ReadFloat();
            packet.food       = reader.ReadVarInt();
            packet.saturation = reader.ReadFloat();
            return packet;
        }

    } // namespace Serialization

} // namespace Network

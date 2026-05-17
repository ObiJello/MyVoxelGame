// File: src/common/network/packets/game/ClientboundBlockUpdateS2CPacket.hpp
//
// Mirrors MC ClientboundBlockUpdatePacket — modern naming convention,
// integer block ID for forward-compat with the block-state-id rework.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct ClientboundBlockUpdateS2CPacket {
        int32_t x, y, z;     // Block position
        int32_t blockId;     // Block ID as int (not BlockID enum for flexibility)

        ClientboundBlockUpdateS2CPacket() = default;
        ClientboundBlockUpdateS2CPacket(int32_t x, int32_t y, int32_t z, int32_t blockId)
            : x(x), y(y), z(z), blockId(blockId) {}
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const ClientboundBlockUpdateS2CPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteInt(packet.x);
            buffer.WriteInt(packet.y);
            buffer.WriteInt(packet.z);
            buffer.WriteInt(packet.blockId);
            return buffer.GetData();
        }

        inline ClientboundBlockUpdateS2CPacket DeserializeClientboundBlockUpdate(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            ClientboundBlockUpdateS2CPacket packet;
            packet.x = reader.ReadInt();
            packet.y = reader.ReadInt();
            packet.z = reader.ReadInt();
            packet.blockId = reader.ReadInt();
            return packet;
        }

    } // namespace Serialization

} // namespace Network

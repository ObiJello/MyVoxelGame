// File: src/common/network/packets/game/PlayerUpdateS2CPacket.hpp
//
// Position broadcast for OTHER players (multiplayer). Sent by server when a
// remote player moves; the local client uses it to interpolate the remote
// player's position+yaw+pitch and animate accordingly.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace Network {

    struct PlayerUpdateS2CPacket {
        uint32_t  playerId;
        glm::vec3 position;
        glm::vec2 rotation;     // yaw, pitch (head look direction)
        bool      isCrouching = false;
        uint32_t  sequenceNumber;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const PlayerUpdateS2CPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteVarInt(packet.playerId);
            buffer.WriteFloat(packet.position.x);
            buffer.WriteFloat(packet.position.y);
            buffer.WriteFloat(packet.position.z);
            buffer.WriteFloat(packet.rotation.x);
            buffer.WriteFloat(packet.rotation.y);
            buffer.WriteByte(packet.isCrouching ? 1 : 0);
            buffer.WriteVarInt(packet.sequenceNumber);
            return buffer.GetData();
        }

        inline PlayerUpdateS2CPacket DeserializePlayerUpdateS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            PlayerUpdateS2CPacket packet;
            packet.playerId = reader.ReadVarInt();
            packet.position.x = reader.ReadFloat();
            packet.position.y = reader.ReadFloat();
            packet.position.z = reader.ReadFloat();
            packet.rotation.x = reader.ReadFloat();
            packet.rotation.y = reader.ReadFloat();
            packet.isCrouching = reader.ReadByte() != 0;
            packet.sequenceNumber = reader.ReadVarInt();
            return packet;
        }

    } // namespace Serialization

} // namespace Network

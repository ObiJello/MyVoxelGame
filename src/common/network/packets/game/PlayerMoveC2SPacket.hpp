// File: src/common/network/packets/game/PlayerMoveC2SPacket.hpp
//
// Local player position update broadcast to the server. Mirrors MC's
// ServerboundMovePlayerPacket family (Pos, Rot, PosRot, StatusOnly) collapsed
// into one full-fields packet — server uses the diff between this and the
// last known to figure out if it's a position-only or rotation-only change.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <glm/glm.hpp>
#include <chrono>
#include <cstdint>
#include <vector>

namespace Network {

    struct PlayerMoveC2SPacket {
        glm::vec3 position;
        glm::vec2 rotation;          // yaw, pitch
        bool      onGround    = false;
        bool      isCrouching = false;
        uint32_t  sequenceNumber = 0;
        std::chrono::steady_clock::time_point timestamp;

        PlayerMoveC2SPacket() = default;
        PlayerMoveC2SPacket(const glm::vec3& pos, const glm::vec2& rot)
            : position(pos), rotation(rot), timestamp(std::chrono::steady_clock::now()) {}
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const PlayerMoveC2SPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteDouble(packet.position.x);
            buffer.WriteDouble(packet.position.y);
            buffer.WriteDouble(packet.position.z);
            buffer.WriteFloat(packet.rotation.x); // yaw
            buffer.WriteFloat(packet.rotation.y); // pitch
            uint8_t flags = 0;
            if (packet.onGround)    flags |= 0x01;
            if (packet.isCrouching) flags |= 0x02;
            buffer.WriteByte(flags);
            buffer.WriteVarInt(packet.sequenceNumber);
            return buffer.GetData();
        }

        inline PlayerMoveC2SPacket DeserializePlayerMoveC2S(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            PlayerMoveC2SPacket packet;
            packet.position.x = reader.ReadDouble();
            packet.position.y = reader.ReadDouble();
            packet.position.z = reader.ReadDouble();
            packet.rotation.x = reader.ReadFloat();
            packet.rotation.y = reader.ReadFloat();
            uint8_t flags = reader.ReadByte();
            packet.onGround    = (flags & 0x01) != 0;
            packet.isCrouching = (flags & 0x02) != 0;
            packet.sequenceNumber = reader.ReadVarInt();
            packet.timestamp = std::chrono::steady_clock::now();
            return packet;
        }

    } // namespace Serialization

} // namespace Network

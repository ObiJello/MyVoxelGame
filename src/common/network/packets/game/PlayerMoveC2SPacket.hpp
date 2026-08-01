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
        bool      isSprinting = false;  // For sprint exhaustion (server-side FoodData)
        bool      jumpedThisTick = false; // Jump impulse since last move send (jump exhaustion)
        // Largest fall-landing distance since the last send (0 = no landing).
        // Client-tracked: its physics knows exact ground contact, while the
        // server's 20 Hz snapshots miss bunny-hop landings entirely.
        float     fallDistance = 0.0f;
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
            if (packet.onGround)       flags |= 0x01;
            if (packet.isCrouching)    flags |= 0x02;
            if (packet.isSprinting)    flags |= 0x04;
            if (packet.jumpedThisTick) flags |= 0x08;
            buffer.WriteByte(flags);
            buffer.WriteFloat(packet.fallDistance);
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
            packet.onGround       = (flags & 0x01) != 0;
            packet.isCrouching    = (flags & 0x02) != 0;
            packet.isSprinting    = (flags & 0x04) != 0;
            packet.jumpedThisTick = (flags & 0x08) != 0;
            packet.fallDistance   = reader.ReadFloat();
            packet.sequenceNumber = reader.ReadVarInt();
            packet.timestamp = std::chrono::steady_clock::now();
            return packet;
        }

    } // namespace Serialization

} // namespace Network

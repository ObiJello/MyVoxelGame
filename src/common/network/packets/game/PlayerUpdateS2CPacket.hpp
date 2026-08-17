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
        // MC LivingEntity.hurtTime, counting down from 10. Rides the position
        // broadcast rather than an entity event because that broadcast already
        // runs every tick for every player — a separate event would be a second
        // packet carrying one byte.
        uint8_t   hurtTime = 0;
        // MC LivingEntity.deathTime, 0..20 — drives the corpse's topple
        // (LivingEntityRenderer.setupRotations). Rides here for the same reason
        // hurtTime does: it is per-tick state every watcher needs and the
        // position broadcast is already going out.
        uint8_t   deathTime = 0;
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
            buffer.WriteByte(packet.hurtTime);
            buffer.WriteByte(packet.deathTime);
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
            packet.hurtTime = reader.ReadByte();
            packet.deathTime = reader.ReadByte();
            packet.sequenceNumber = reader.ReadVarInt();
            return packet;
        }

    } // namespace Serialization

} // namespace Network

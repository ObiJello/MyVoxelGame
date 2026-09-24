// File: src/common/network/packets/game/PlayerSleepS2CPacket.hpp
//
// Server → every client: a player lay down in a bed, or got up.
//
// MC carries this in two pieces — LivingEntity's SLEEPING_POS synched entity
// data (the bed position, whose presence IS `isSleeping()`) and
// ClientboundAnimatePacket action 0 (WAKE_UP). Players here are not tracked
// entities with a metadata stream, so both pieces ride one packet: `sleeping`
// with the bed's HEAD cell, or `!sleeping` for the wake-up.
//
// Wire: VarInt playerId, byte sleeping, int32 x, int32 y, int32 z (the
// position is written even for a wake-up, as zeros — fixed layout).
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace Network {

    struct PlayerSleepS2CPacket {
        uint32_t   playerId = 0;
        bool       sleeping = false;
        glm::ivec3 bedPos{0};
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const PlayerSleepS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteVarInt(packet.playerId);
            buffer.WriteByte(packet.sleeping ? 1 : 0);
            buffer.WriteInt(static_cast<uint32_t>(packet.bedPos.x));
            buffer.WriteInt(static_cast<uint32_t>(packet.bedPos.y));
            buffer.WriteInt(static_cast<uint32_t>(packet.bedPos.z));
            return buffer.GetData();
        }

        inline PlayerSleepS2CPacket DeserializePlayerSleepS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            PlayerSleepS2CPacket packet;
            packet.playerId = reader.ReadVarInt();
            packet.sleeping = reader.ReadByte() != 0;
            packet.bedPos.x = static_cast<int32_t>(reader.ReadInt());
            packet.bedPos.y = static_cast<int32_t>(reader.ReadInt());
            packet.bedPos.z = static_cast<int32_t>(reader.ReadInt());
            return packet;
        }

    } // namespace Serialization

} // namespace Network

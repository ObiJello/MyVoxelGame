// File: src/common/network/packets/game/ClientboundPlayerPositionPacket.hpp
//
// Mirrors MC ClientboundPlayerPositionPacket: an authoritative position snap
// from server to a single player. Carries position + delta movement +
// yaw/pitch + a Relative-flag bitmask. The client must echo `id` back via
// ServerboundAcceptTeleportationPacket so the server can ignore stale
// pre-teleport client position updates that were in flight.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "../common/PacketCommon.hpp"  // Relative::* constants
#include <cstdint>
#include <vector>

namespace Network {

    struct ClientboundPlayerPositionPacket {
        int32_t id = 0;
        // PositionMoveRotation: position + deltaMovement + yRot + xRot
        double  x = 0.0,  y = 0.0,  z = 0.0;
        double  dx = 0.0, dy = 0.0, dz = 0.0;
        float   yRot = 0.0f;
        float   xRot = 0.0f;
        int32_t relatives = 0; // bitmask of Relative::* — 0 means fully absolute
    };

    namespace Serialization {

        // Wire format mirrors MC's ClientboundPlayerPositionPacket. We use a
        // fixed-size int32 for `id` instead of MC's VarInt for simplicity.
        inline std::vector<uint8_t> Serialize(const ClientboundPlayerPositionPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteInt(packet.id);
            buffer.WriteDouble(packet.x);
            buffer.WriteDouble(packet.y);
            buffer.WriteDouble(packet.z);
            buffer.WriteDouble(packet.dx);
            buffer.WriteDouble(packet.dy);
            buffer.WriteDouble(packet.dz);
            buffer.WriteFloat(packet.yRot);
            buffer.WriteFloat(packet.xRot);
            buffer.WriteInt(packet.relatives);
            return buffer.GetData();
        }

        inline ClientboundPlayerPositionPacket DeserializeClientboundPlayerPosition(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            ClientboundPlayerPositionPacket packet;
            packet.id = static_cast<int32_t>(reader.ReadInt());
            packet.x  = reader.ReadDouble();
            packet.y  = reader.ReadDouble();
            packet.z  = reader.ReadDouble();
            packet.dx = reader.ReadDouble();
            packet.dy = reader.ReadDouble();
            packet.dz = reader.ReadDouble();
            packet.yRot = reader.ReadFloat();
            packet.xRot = reader.ReadFloat();
            packet.relatives = static_cast<int32_t>(reader.ReadInt());
            return packet;
        }

    } // namespace Serialization

} // namespace Network

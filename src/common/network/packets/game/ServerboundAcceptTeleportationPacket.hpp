// File: src/common/network/packets/game/ServerboundAcceptTeleportationPacket.hpp
//
// Mirrors MC ServerboundAcceptTeleportationPacket — the client echoes back
// the teleport id from a ClientboundPlayerPositionPacket so the server can
// fence off stale pre-teleport movement updates.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct ServerboundAcceptTeleportationPacket {
        int32_t id = 0;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const ServerboundAcceptTeleportationPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteInt(packet.id);
            return buffer.GetData();
        }

        inline ServerboundAcceptTeleportationPacket DeserializeServerboundAcceptTeleportation(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            ServerboundAcceptTeleportationPacket packet;
            packet.id = static_cast<int32_t>(reader.ReadInt());
            return packet;
        }

    } // namespace Serialization

} // namespace Network

// File: src/common/network/packets/game/SetHeldSlotS2CPacket.hpp
//
// Server → client: the selected hotbar slot changed on the server's side
// (pick block moved it). Mirrors MC ClientboundSetHeldSlotPacket.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct SetHeldSlotS2CPacket {
        uint8_t slot = 0;   // 0..8
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const SetHeldSlotS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteByte(packet.slot);
            return buffer.GetData();
        }

        inline SetHeldSlotS2CPacket DeserializeSetHeldSlotS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            SetHeldSlotS2CPacket packet;
            packet.slot = reader.ReadByte();
            return packet;
        }

    } // namespace Serialization

} // namespace Network

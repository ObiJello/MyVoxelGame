// File: src/common/network/packets/game/HotbarSyncS2CPacket.hpp
//
// Server → client: full hotbar contents on join. Single-slot updates after
// that flow through InventorySetSlotS2CPacket.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace Network {

    struct HotbarSyncS2CPacket {
        std::array<uint16_t, 9> slots;
        HotbarSyncS2CPacket() { slots.fill(0); }
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const HotbarSyncS2CPacket& packet) {
            PacketBuffer buffer;
            for (int i = 0; i < 9; i++) buffer.WriteShort(packet.slots[i]);
            return buffer.GetData();
        }

        inline HotbarSyncS2CPacket DeserializeHotbarSyncS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            HotbarSyncS2CPacket packet;
            for (int i = 0; i < 9; i++) {
                packet.slots[i] = static_cast<uint16_t>(reader.ReadShort());
            }
            return packet;
        }

    } // namespace Serialization

} // namespace Network

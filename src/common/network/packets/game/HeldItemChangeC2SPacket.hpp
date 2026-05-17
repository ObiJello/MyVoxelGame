// File: src/common/network/packets/game/HeldItemChangeC2SPacket.hpp
//
// Mirrors MC ServerboundSetCarriedItemPacket — sent when the player presses
// 1-9 / scrolls hotbar to change the selected slot.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct HeldItemChangeC2SPacket {
        int16_t  slot    = 0;
        uint16_t blockId = 0;  // Block type currently in selected slot (server sync hint)

        HeldItemChangeC2SPacket() = default;
        HeldItemChangeC2SPacket(int16_t s, uint16_t block = 0) : slot(s), blockId(block) {}
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const HeldItemChangeC2SPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteShort(packet.slot);
            buffer.WriteShort(static_cast<int16_t>(packet.blockId));
            return buffer.GetData();
        }

        inline HeldItemChangeC2SPacket DeserializeHeldItemChangeC2S(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            HeldItemChangeC2SPacket packet;
            packet.slot = reader.ReadShort();
            // Older clients may have only sent the slot — keep blockId optional.
            if (reader.Remaining() >= 2) {
                packet.blockId = static_cast<uint16_t>(reader.ReadShort());
            }
            return packet;
        }

    } // namespace Serialization

} // namespace Network

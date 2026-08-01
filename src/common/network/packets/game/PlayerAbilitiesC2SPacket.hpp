// File: src/common/network/packets/game/PlayerAbilitiesC2SPacket.hpp
//
// Client → server: fly-state change. Mirrors MC's
// ServerboundPlayerAbilitiesPacket, which only carries the FLYING bit —
// everything else in Abilities is server-authoritative. Sent when the
// client toggles creative flight (double-tap space) or auto-cancels it on
// landing. The server accepts the bit only when the player mayFly;
// otherwise it replies with a corrective PlayerAbilitiesS2C.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct PlayerAbilitiesC2SPacket {
        static constexpr uint8_t FLAG_FLYING = 0x02; // same bit as the S2C packet

        uint8_t flags = 0;

        bool flying() const { return (flags & FLAG_FLYING) != 0; }
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const PlayerAbilitiesC2SPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteByte(packet.flags);
            return buffer.GetData();
        }

        inline PlayerAbilitiesC2SPacket DeserializePlayerAbilitiesC2S(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            PlayerAbilitiesC2SPacket packet;
            packet.flags = reader.ReadByte();
            return packet;
        }

    } // namespace Serialization

} // namespace Network

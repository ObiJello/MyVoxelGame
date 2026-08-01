// File: src/common/network/packets/game/PlayerAbilitiesS2CPacket.hpp
//
// Server → client: authoritative player abilities + game mode. Sent on join
// and whenever the mode/abilities change (/gamemode, fly toggle rejection).
//
// Mirrors MC ClientboundPlayerAbilitiesPacket (flags byte + fly/walk speeds)
// with one extra trailing byte for the game mode — MC ships that separately
// via ClientboundGameEventPacket CHANGE_GAME_MODE, but the client needs both
// together for prediction (HUD hiding, flight, creative no-consume), so we
// fold them into one packet.
//
// Flag bits match MC's Abilities.Packed exactly:
//   0x01 invulnerable, 0x02 flying, 0x04 mayFly, 0x08 instabuild
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct PlayerAbilitiesS2CPacket {
        static constexpr uint8_t FLAG_INVULNERABLE = 0x01;
        static constexpr uint8_t FLAG_FLYING       = 0x02;
        static constexpr uint8_t FLAG_MAY_FLY      = 0x04;
        static constexpr uint8_t FLAG_INSTABUILD   = 0x08;

        uint8_t flags        = 0;
        float   flyingSpeed  = 0.05f;   // MC Abilities.flyingSpeed default
        float   walkingSpeed = 0.1f;    // MC Abilities.walkingSpeed default
        uint8_t gameMode     = 0;       // Server::GameMode raw value (0 = survival)

        bool invulnerable() const { return (flags & FLAG_INVULNERABLE) != 0; }
        bool flying()       const { return (flags & FLAG_FLYING) != 0; }
        bool mayFly()       const { return (flags & FLAG_MAY_FLY) != 0; }
        bool instabuild()   const { return (flags & FLAG_INSTABUILD) != 0; }
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const PlayerAbilitiesS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteByte(packet.flags);
            buffer.WriteFloat(packet.flyingSpeed);
            buffer.WriteFloat(packet.walkingSpeed);
            buffer.WriteByte(packet.gameMode);
            return buffer.GetData();
        }

        inline PlayerAbilitiesS2CPacket DeserializePlayerAbilitiesS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            PlayerAbilitiesS2CPacket packet;
            packet.flags        = reader.ReadByte();
            packet.flyingSpeed  = reader.ReadFloat();
            packet.walkingSpeed = reader.ReadFloat();
            packet.gameMode     = reader.ReadByte();
            return packet;
        }

    } // namespace Serialization

} // namespace Network

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
        // Non-vanilla, see the C2S packet: the saved debug-noclip state coming
        // back on join. A vanilla client would ignore the bit.
        static constexpr uint8_t FLAG_NOCLIP       = 0x10;

        uint8_t flags        = 0;
        float   flyingSpeed  = 0.05f;   // MC Abilities.flyingSpeed default
        float   walkingSpeed = 0.1f;    // MC Abilities.walkingSpeed default
        uint8_t gameMode     = 0;       // Server::GameMode raw value (0 = survival)
        // The player's size (scaled immersive portals). Trailing, optional.
        float   scale        = 1.0f;
        // /morph: what this player has become (Game::Morph code: kind + id,
        // 0xFFFFFFFF = none) and, for a mob, the speed its move control
        // walks it at — the client takes the body's size and eye height and
        // walks at that speed. Trailing, optional.
        uint32_t morph       = 0xFFFFFFFFu;
        float    morphSpeed  = 0.0f;
        // MC ClientboundLoginPacket / CommonPlayerSpawnInfo.previousGameType
        // (-1 = none): the mode Debug Modifier+N goes back to from spectator. Rides here
        // because the gameMode byte above already stands in for MC's
        // CHANGE_GAME_MODE game event. Trailing, optional.
        int8_t   previousGameMode = -1;
        // /morph: the morph's look beyond the code (Game::Morph
        // DefaultVariantOf). Trailing, optional.
        int32_t  morphVariant = 0;
        bool     hasMorphVariant = false;   // read side: the field was on the wire

        bool invulnerable() const { return (flags & FLAG_INVULNERABLE) != 0; }
        bool flying()       const { return (flags & FLAG_FLYING) != 0; }
        bool mayFly()       const { return (flags & FLAG_MAY_FLY) != 0; }
        bool instabuild()   const { return (flags & FLAG_INSTABUILD) != 0; }
        bool noclip()       const { return (flags & FLAG_NOCLIP) != 0; }
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const PlayerAbilitiesS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteByte(packet.flags);
            buffer.WriteFloat(packet.flyingSpeed);
            buffer.WriteFloat(packet.walkingSpeed);
            buffer.WriteByte(packet.gameMode);
            buffer.WriteFloat(packet.scale);
            buffer.WriteVarInt(packet.morph);
            buffer.WriteFloat(packet.morphSpeed);
            buffer.WriteByte(static_cast<uint8_t>(packet.previousGameMode));
            buffer.WriteVarInt(static_cast<uint32_t>(packet.morphVariant));
            return buffer.GetData();
        }

        inline PlayerAbilitiesS2CPacket DeserializePlayerAbilitiesS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            PlayerAbilitiesS2CPacket packet;
            packet.flags        = reader.ReadByte();
            packet.flyingSpeed  = reader.ReadFloat();
            packet.walkingSpeed = reader.ReadFloat();
            packet.gameMode     = reader.ReadByte();
            packet.scale        = reader.HasMore() ? reader.ReadFloat() : 1.0f;
            packet.morph        = reader.HasMore() ? reader.ReadVarInt() : 0xFFFFFFFFu;
            packet.morphSpeed   = reader.HasMore() ? reader.ReadFloat() : 0.0f;
            packet.previousGameMode = reader.HasMore() ? static_cast<int8_t>(reader.ReadByte()) : int8_t(-1);
            packet.hasMorphVariant = reader.HasMore();
            packet.morphVariant = packet.hasMorphVariant
                ? static_cast<int32_t>(reader.ReadVarInt()) : 0;
            return packet;
        }

    } // namespace Serialization

} // namespace Network

// File: src/common/network/packets/game/SetHealthS2CPacket.hpp
//
// Server → client: authoritative health / hunger / saturation triple. Sent on
// change (dirty-checked in PlayerSession::Tick) and on the first PLAYING tick.
//
// Mirrors MC ClientboundSetHealthPacket field-for-field, in MC's write order:
//   writeFloat(health), writeVarInt(food), writeFloat(saturation)
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct SetHealthS2CPacket {
        float    health     = 20.0f;
        uint32_t food       = 20;
        float    saturation = 5.0f;
        // APPENDED FIELDS — absence decodes as 0. What MC's HUD reads off
        // the player entity itself (entity data, effects, the level):
        // the absorption hearts, and the flags that pick the heart and
        // food sprites (Hud.HeartType.forPlayer, the HUNGER food row, the
        // REGENERATION heart bob, hardcore's sheet).
        float    absorption = 0.0f;
        static constexpr uint8_t kFlagPoison       = 0x01;
        static constexpr uint8_t kFlagWither       = 0x02;
        static constexpr uint8_t kFlagHunger       = 0x04;
        static constexpr uint8_t kFlagRegeneration = 0x08;
        static constexpr uint8_t kFlagHardcore     = 0x10;
        static constexpr uint8_t kFlagFrozen       = 0x20;
        uint8_t  hudFlags   = 0;
        // MC DATA_AIR_SUPPLY_ID: the air bar. Negative while drowning.
        int32_t  airSupply  = 300;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const SetHealthS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteFloat(packet.health);
            buffer.WriteVarInt(packet.food);
            buffer.WriteFloat(packet.saturation);
            buffer.WriteFloat(packet.absorption);
            buffer.WriteByte(packet.hudFlags);
            buffer.WriteInt(static_cast<uint32_t>(packet.airSupply));
            return buffer.GetData();
        }

        inline SetHealthS2CPacket DeserializeSetHealthS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            SetHealthS2CPacket packet;
            packet.health     = reader.ReadFloat();
            packet.food       = reader.ReadVarInt();
            packet.saturation = reader.ReadFloat();
            if (reader.Remaining() >= 5) {
                packet.absorption = reader.ReadFloat();
                packet.hudFlags   = reader.ReadByte();
            }
            if (reader.Remaining() >= 4) packet.airSupply = static_cast<int32_t>(reader.ReadInt());
            return packet;
        }

    } // namespace Serialization

} // namespace Network

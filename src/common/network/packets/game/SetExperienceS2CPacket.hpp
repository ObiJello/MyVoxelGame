// File: src/common/network/packets/game/SetExperienceS2CPacket.hpp
//
// Server → client: the authoritative XP triple. Mirrors MC
// ClientboundSetExperiencePacket field-for-field, in MC's write order:
//   writeFloat(experienceProgress), writeVarInt(experienceLevel),
//   writeVarInt(totalExperience)
//
// Sent on change (dirty-checked in PlayerSession::Tick, exactly like
// SetHealthS2C) and on the first PLAYING tick.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct SetExperienceS2CPacket {
        float    progress = 0.0f;   // 0..1 bar fill
        uint32_t level    = 0;
        uint32_t total    = 0;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const SetExperienceS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteFloat(packet.progress);
            buffer.WriteVarInt(packet.level);
            buffer.WriteVarInt(packet.total);
            return buffer.GetData();
        }

        inline SetExperienceS2CPacket
        DeserializeSetExperienceS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            SetExperienceS2CPacket packet;
            packet.progress = reader.ReadFloat();
            packet.level    = reader.ReadVarInt();
            packet.total    = reader.ReadVarInt();
            return packet;
        }

    } // namespace Serialization

} // namespace Network

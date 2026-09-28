// File: src/common/network/packets/game/CooldownS2CPacket.hpp
//
// MC ClientboundCooldownPacket(cooldownGroup, duration) — sent only to the
// player whose ItemCooldowns changed (ServerItemCooldowns.onCooldownStarted
// with the duration, onCooldownEnded with 0). The client mirrors it into its
// own table (ClientPacketListener.handleItemCooldown: duration 0 removes the
// group, anything else starts it), which draws the hotbar sweep.
//
// Wire: String cooldownGroup ("minecraft:wind_charge"), VarInt duration.
#pragma once

#include "common/network/PacketRegistry.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace Network {

    struct CooldownS2CPacket {
        std::string cooldownGroup;
        int32_t     duration = 0;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const CooldownS2CPacket& p) {
            PacketBuffer b;
            b.WriteString(p.cooldownGroup);
            b.WriteVarInt(static_cast<uint32_t>(p.duration));
            return b.GetData();
        }

        inline CooldownS2CPacket DeserializeCooldownS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            CooldownS2CPacket p;
            p.cooldownGroup = r.ReadString();
            p.duration      = static_cast<int32_t>(r.ReadVarInt());
            return p;
        }

    } // namespace Serialization

} // namespace Network

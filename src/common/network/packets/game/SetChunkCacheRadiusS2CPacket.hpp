// File: src/common/network/packets/game/SetChunkCacheRadiusS2CPacket.hpp
//
// Mirrors MC ClientboundSetChunkCacheRadiusPacket — server tells client
// the effective view distance after clamping/policy.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct SetChunkCacheRadiusS2CPacket {
        int32_t viewDistance = 8;

        SetChunkCacheRadiusS2CPacket() = default;
        explicit SetChunkCacheRadiusS2CPacket(int32_t dist) : viewDistance(dist) {}
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const SetChunkCacheRadiusS2CPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteVarInt(packet.viewDistance);
            return buffer.GetData();
        }

        inline SetChunkCacheRadiusS2CPacket DeserializeSetChunkCacheRadiusS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            SetChunkCacheRadiusS2CPacket packet;
            packet.viewDistance = reader.ReadVarInt();
            return packet;
        }

    } // namespace Serialization

} // namespace Network

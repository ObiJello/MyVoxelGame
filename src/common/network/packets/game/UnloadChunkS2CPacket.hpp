// File: src/common/network/packets/game/UnloadChunkS2CPacket.hpp
//
// Mirrors MC ClientboundForgetLevelChunkPacket. Tells the client to drop a
// chunk it no longer needs (out of view distance).
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>
#include <chrono>

namespace Network {

    struct UnloadChunkS2CPacket {
        int32_t chunkX;
        int32_t chunkZ;
        std::chrono::steady_clock::time_point timestamp;

        UnloadChunkS2CPacket() = default;
        UnloadChunkS2CPacket(int32_t x, int32_t z)
            : chunkX(x), chunkZ(z), timestamp(std::chrono::steady_clock::now()) {}
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const UnloadChunkS2CPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteInt(packet.chunkX);
            buffer.WriteInt(packet.chunkZ);
            return buffer.GetData();
        }

        inline UnloadChunkS2CPacket DeserializeUnloadChunkS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            UnloadChunkS2CPacket packet;
            packet.chunkX = reader.ReadInt();
            packet.chunkZ = reader.ReadInt();
            packet.timestamp = std::chrono::steady_clock::now();
            return packet;
        }

    } // namespace Serialization

} // namespace Network

// File: src/common/network/packets/game/ChunkBatchFinishedS2CPacket.hpp
//
// Mirrors MC ClientboundChunkBatchFinishedPacket — sent by the server at the
// end of a chunk batch with the count, so the client can compute the achieved
// rate and reply with ChunkBatchAckC2SPacket.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct ChunkBatchFinishedS2CPacket {
        int32_t batchSize = 0;

        ChunkBatchFinishedS2CPacket() = default;
        explicit ChunkBatchFinishedS2CPacket(int32_t size) : batchSize(size) {}
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const ChunkBatchFinishedS2CPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteInt(packet.batchSize);
            return buffer.GetData();
        }

        inline ChunkBatchFinishedS2CPacket DeserializeChunkBatchFinishedS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            ChunkBatchFinishedS2CPacket packet;
            packet.batchSize = reader.ReadInt();
            return packet;
        }

    } // namespace Serialization

} // namespace Network

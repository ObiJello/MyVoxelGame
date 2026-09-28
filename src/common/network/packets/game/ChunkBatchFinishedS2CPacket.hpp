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
        // Trailing (not in MC's packet): how long the server took to hand the
        // batch to its connection, start packet to finish packet. The client
        // subtracts it from the batch's arrival span to see what the LINK
        // added (ClientPacketHandler's batch-rate estimator). 0 = not sent.
        uint32_t serverSendMicros = 0;

        ChunkBatchFinishedS2CPacket() = default;
        ChunkBatchFinishedS2CPacket(int32_t size, uint32_t sendMicros)
            : batchSize(size), serverSendMicros(sendMicros) {}
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const ChunkBatchFinishedS2CPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteInt(packet.batchSize);
            buffer.WriteInt(packet.serverSendMicros);
            return buffer.GetData();
        }

        inline ChunkBatchFinishedS2CPacket DeserializeChunkBatchFinishedS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            ChunkBatchFinishedS2CPacket packet;
            packet.batchSize = reader.ReadInt();
            if (reader.Remaining() >= 4) packet.serverSendMicros = reader.ReadInt();
            return packet;
        }

    } // namespace Serialization

} // namespace Network

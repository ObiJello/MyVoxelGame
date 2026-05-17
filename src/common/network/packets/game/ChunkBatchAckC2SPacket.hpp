// File: src/common/network/packets/game/ChunkBatchAckC2SPacket.hpp
//
// Mirrors MC ServerboundChunkBatchReceivedPacket — client acknowledges a
// batch with the chunks-per-tick rate it can keep up with. Server uses
// this to throttle subsequent batches.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <vector>

namespace Network {

    struct ChunkBatchAckC2SPacket {
        float desiredChunksPerTick = 9.0f;

        ChunkBatchAckC2SPacket() = default;
        explicit ChunkBatchAckC2SPacket(float rate) : desiredChunksPerTick(rate) {}
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const ChunkBatchAckC2SPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteFloat(packet.desiredChunksPerTick);
            return buffer.GetData();
        }

        inline ChunkBatchAckC2SPacket DeserializeChunkBatchAckC2S(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            ChunkBatchAckC2SPacket packet;
            packet.desiredChunksPerTick = reader.ReadFloat();
            return packet;
        }

    } // namespace Serialization

} // namespace Network

// File: src/common/network/packets/game/ChunkBatchStartS2CPacket.hpp
//
// Mirrors MC ClientboundChunkBatchStartPacket — empty marker that signals
// the start of a batch of chunks. Used by the chunk batch flow control
// (server measures inter-batch latency, client replies with ChunkBatchAck).
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <vector>

namespace Network {

    struct ChunkBatchStartS2CPacket {};

    namespace Serialization {

        // Empty marker packet — no payload.
        inline std::vector<uint8_t> Serialize(const ChunkBatchStartS2CPacket&) {
            return {};
        }

    } // namespace Serialization

} // namespace Network

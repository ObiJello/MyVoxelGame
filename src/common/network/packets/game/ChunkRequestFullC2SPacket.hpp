#pragma once
#include "common/network/PacketRegistry.hpp"
#include <vector>
#include <cstdint>

namespace Network {
    // Client answer to ChunkUnchangedS2C when it no longer holds the chunk
    // (retention cache evicted it): send the full ChunkDataS2C after all.
    // Carries the dimension too: a client holds a level per dimension, so
    // the chunk it evicted is identified by (dimension, x, z).
    struct ChunkRequestFullC2SPacket {
        int32_t chunkX = 0;
        int32_t chunkZ = 0;
        int8_t  dimensionId = 0;
    };
    namespace Serialization {
        inline std::vector<uint8_t> Serialize(const ChunkRequestFullC2SPacket& p) {
            Network::PacketBuffer b; b.WriteInt(p.chunkX); b.WriteInt(p.chunkZ);
            b.WriteByte(static_cast<uint8_t>(p.dimensionId));
            return b.GetData();
        }
        inline ChunkRequestFullC2SPacket DeserializeChunkRequestFullC2S(const std::vector<uint8_t>& data) {
            Network::PacketReader r(data); ChunkRequestFullC2SPacket p;
            p.chunkX = r.ReadInt(); p.chunkZ = r.ReadInt();
            p.dimensionId = static_cast<int8_t>(r.ReadByte());
            return p;
        }
    }
}

#pragma once
#include "common/network/PacketRegistry.hpp"
#include <vector>
#include <cstdint>

namespace Network {
    // "The chunk you already hold for (x,z) with this stamp is still current":
    // sent instead of ChunkDataS2C when the server's Chunk::modStamp matches
    // the stamp it last sent this client. The client revives its retained
    // copy, or answers ChunkRequestFullC2S if it has evicted it.
    struct ChunkUnchangedS2CPacket {
        int32_t chunkX = 0;
        int32_t chunkZ = 0;
        uint64_t modStamp = 0;
    };
    namespace Serialization {
        inline std::vector<uint8_t> Serialize(const ChunkUnchangedS2CPacket& p) {
            Network::PacketBuffer b; b.WriteInt(p.chunkX); b.WriteInt(p.chunkZ); b.WriteLong(p.modStamp); return b.GetData();
        }
        inline ChunkUnchangedS2CPacket DeserializeChunkUnchangedS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader r(data); ChunkUnchangedS2CPacket p;
            p.chunkX = r.ReadInt(); p.chunkZ = r.ReadInt(); p.modStamp = r.ReadLong(); return p;
        }
    }
}

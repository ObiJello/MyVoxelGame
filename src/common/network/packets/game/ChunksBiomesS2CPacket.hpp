#pragma once
// ChunksBiomesS2C (0x64) — MC ClientboundChunksBiomesPacket.
//
// The whole biome column of each listed chunk, sent to a player after a
// runtime biome edit (/fillbiome → ChunkMap.resendBiomesForChunks). Chunk
// delivery never uses it: ChunkDataS2C carries each section's biomes itself.
// One packet per player, holding every edited chunk that player watches.
//
//   VarInt count, then per chunk:
//     Int chunkX, Int chunkZ,
//     VarInt byteLength, then SECTIONS_PER_CHUNK biome containers in section
//     order — MC ChunkBiomeData.extractChunkData, each container exactly as
//     ChunkDataS2C writes it (WriteContainer).
//
// The byte length is MC's `byteArray` framing: a chunk whose containers fail
// to decode is skipped without losing the rest of the list.
//
// Decoded on the I/O thread into ContainerData; the main thread builds the
// paletted containers and swaps them in.
#include "common/network/PacketRegistry.hpp"
#include "ChunkDataS2CPacket.hpp"   // ContainerData, WriteContainer / ReadContainer

#include <array>
#include <cstdint>
#include <exception>
#include <vector>

namespace Network {

    struct ChunksBiomesS2CPacket {
        struct ChunkBiomeData {
            int32_t chunkX = 0;
            int32_t chunkZ = 0;
            std::array<ChunkDataS2CPacket::ContainerData, Game::Math::SECTIONS_PER_CHUNK> sections;
        };
        std::vector<ChunkBiomeData> chunks;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const ChunksBiomesS2CPacket& packet) {
            Network::PacketBuffer b(16 + packet.chunks.size() * 256);
            b.WriteVarInt(static_cast<uint32_t>(packet.chunks.size()));
            for (const auto& chunk : packet.chunks) {
                b.WriteInt(static_cast<uint32_t>(chunk.chunkX));
                b.WriteInt(static_cast<uint32_t>(chunk.chunkZ));
                Network::PacketBuffer column(256);
                for (const auto& container : chunk.sections) WriteContainer(column, container);
                b.WriteVarInt(static_cast<uint32_t>(column.GetData().size()));
                b.WriteBytes(column.GetData());
            }
            return b.GetData();
        }

        inline ChunksBiomesS2CPacket DeserializeChunksBiomesS2C(const std::vector<uint8_t>& data) {
            // MC ChunkBiomeData's byteArray(2097152) cap.
            constexpr uint32_t kMaxColumnBytes = 2097152;
            Network::PacketReader r(data);
            ChunksBiomesS2CPacket p;
            try {
                const uint32_t count = r.ReadVarInt();
                // Every entry is at least 9 bytes, so a count beyond that is a
                // lie and must not reach reserve().
                if (count > r.Remaining() / 9) return p;
                p.chunks.reserve(count);
                for (uint32_t i = 0; i < count; ++i) {
                    ChunksBiomesS2CPacket::ChunkBiomeData chunk;
                    chunk.chunkX = static_cast<int32_t>(r.ReadInt());
                    chunk.chunkZ = static_cast<int32_t>(r.ReadInt());
                    const uint32_t length = r.ReadVarInt();
                    if (length > kMaxColumnBytes || length > r.Remaining()) break;
                    const std::vector<uint8_t> column = r.ReadBytes(length);
                    Network::PacketReader cr(column);
                    bool ok = true;
                    for (auto& container : chunk.sections) {
                        if (!ReadContainer(cr, container, 64)) { ok = false; break; }
                    }
                    if (ok) p.chunks.push_back(std::move(chunk));
                }
            } catch (const std::exception&) {
                // Truncated: keep the chunks that decoded whole.
            }
            return p;
        }

    } // namespace Serialization

} // namespace Network

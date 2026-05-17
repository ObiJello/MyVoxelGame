// File: src/common/network/packets/game/ChunkDataS2CPacket.hpp
//
// Mirrors MC ClientboundLevelChunkWithLightPacket (MC protocol 0x20-ish).
// Sends an entire chunk's section data to the client. Each section is a
// palletted container: blockCount + bitsPerEntry + (palette + dataArray).
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>
#include <chrono>
#include <algorithm>

namespace Network {

    struct ChunkDataS2CPacket {
        // Chunk coordinates
        int32_t chunkX;
        int32_t chunkZ;

        // If true, send all sections plus biomes; otherwise only sections in bitmask
        bool groundUpContinuous = true;

        // Bitmask indicating which sections contain data (VarInt).
        // Bit 0 = section at minBuildHeight, bit 1 = next section up, etc.
        uint32_t primaryBitmask = 0;

        // Palletted container data for each section
        struct SectionData {
            uint16_t blockCount = 0;            // Number of non-air blocks
            uint8_t  bitsPerEntry = 0;          // Bits per block index (for palette mode)
            std::vector<uint32_t> palette;      // Block state IDs in palette (optional)
            std::vector<uint64_t> dataArray;    // Packed block indices/IDs

            bool IsEmpty() const { return blockCount == 0; }
        };

        // Section data for each section with bit set in primaryBitmask
        std::vector<SectionData> sections;

        // Optional biome data (256 bytes for ground-up continuous)
        std::vector<uint8_t> biomeData;

        // Timestamp for tracking
        std::chrono::steady_clock::time_point timestamp;

        ChunkDataS2CPacket() = default;
        ChunkDataS2CPacket(int32_t x, int32_t z)
            : chunkX(x), chunkZ(z), timestamp(std::chrono::steady_clock::now()) {}

        // Calculate total data size for the packet
        size_t CalculateDataSize() const {
            size_t size = 0;
            for (const auto& section : sections) {
                size += sizeof(section.blockCount) + sizeof(section.bitsPerEntry);
                size += section.palette.size() * sizeof(uint32_t);
                size += section.dataArray.size() * sizeof(uint64_t);
            }
            size += biomeData.size();
            return size;
        }
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const ChunkDataS2CPacket& packet) {
            Network::PacketBuffer buffer(4096); // Reserve more space for chunk data

            // Calculate total data size first
            size_t dataSize = 0;
            for (const auto& section : packet.sections) {
                dataSize += sizeof(uint16_t); // blockCount
                dataSize += sizeof(uint8_t);  // bitsPerEntry
                if (!section.palette.empty()) {
                    dataSize += VarInt::GetSize(static_cast<uint32_t>(section.palette.size()));
                    for (uint32_t id : section.palette) {
                        dataSize += VarInt::GetSize(id);
                    }
                }
                dataSize += VarInt::GetSize(static_cast<uint32_t>(section.dataArray.size()));
                dataSize += section.dataArray.size() * sizeof(uint64_t);
            }
            if (packet.groundUpContinuous) {
                dataSize += packet.biomeData.size();
            }

            // Write packet structure following Minecraft protocol.
            // Note: Packet ID and length prefix are handled by the network layer.
            buffer.WriteInt(packet.chunkX);
            buffer.WriteInt(packet.chunkZ);
            buffer.WriteByte(packet.groundUpContinuous ? 1 : 0);
            buffer.WriteVarInt(packet.primaryBitmask);
            buffer.WriteVarInt(static_cast<uint32_t>(dataSize));

            // Section data — written in ascending Y order
            for (const auto& section : packet.sections) {
                buffer.WriteShort(section.blockCount);
                buffer.WriteByte(section.bitsPerEntry);
                if (section.bitsPerEntry > 0 && section.bitsPerEntry <= 8) {
                    // Palette mode
                    buffer.WriteVarInt(static_cast<uint32_t>(section.palette.size()));
                    for (uint32_t blockStateId : section.palette) {
                        buffer.WriteVarInt(blockStateId);
                    }
                }
                buffer.WriteVarInt(static_cast<uint32_t>(section.dataArray.size()));
                for (uint64_t data : section.dataArray) {
                    buffer.WriteLong(data);
                }
            }

            // Biome data (if ground-up continuous)
            if (packet.groundUpContinuous && !packet.biomeData.empty()) {
                buffer.WriteBytes(packet.biomeData);
            }

            return buffer.GetData();
        }

        inline ChunkDataS2CPacket DeserializeChunkDataS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            ChunkDataS2CPacket packet;

            packet.chunkX = reader.ReadInt();
            packet.chunkZ = reader.ReadInt();
            packet.groundUpContinuous = reader.ReadByte() != 0;
            packet.primaryBitmask = reader.ReadVarInt();

            // Read data size (consumed but not used directly — sections decode by bitmask)
            (void)reader.ReadVarInt();

            // Count sections from bitmask
            uint32_t sectionCount = 0;
            for (int i = 0; i < 24; ++i) { // Max 24 sections for world height 384
                if (packet.primaryBitmask & (1 << i)) {
                    sectionCount++;
                }
            }

            packet.sections.reserve(sectionCount);
            for (uint32_t i = 0; i < sectionCount; ++i) {
                ChunkDataS2CPacket::SectionData section;
                section.blockCount = reader.ReadShort();
                section.bitsPerEntry = reader.ReadByte();
                if (section.bitsPerEntry > 0 && section.bitsPerEntry <= 8) {
                    uint32_t paletteSize = reader.ReadVarInt();
                    section.palette.reserve(paletteSize);
                    for (uint32_t j = 0; j < paletteSize; ++j) {
                        section.palette.push_back(reader.ReadVarInt());
                    }
                }
                uint32_t dataArraySize = reader.ReadVarInt();
                section.dataArray.reserve(dataArraySize);
                for (uint32_t j = 0; j < dataArraySize; ++j) {
                    section.dataArray.push_back(reader.ReadLong());
                }
                packet.sections.push_back(std::move(section));
            }

            // Read biome data if ground-up continuous
            if (packet.groundUpContinuous && reader.HasMore()) {
                size_t biomeSize = std::min(reader.Remaining(), size_t(256));
                packet.biomeData = reader.ReadBytes(biomeSize);
            }

            packet.timestamp = std::chrono::steady_clock::now();
            return packet;
        }

    } // namespace Serialization

} // namespace Network

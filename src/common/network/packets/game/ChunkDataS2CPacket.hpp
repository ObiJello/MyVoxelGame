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

        // One section on the wire, shaped exactly like MC
        // LevelChunkSection.write: a non-empty count, then the block-state
        // container, then the biome container.
        //
        //     buffer.writeShort(this.nonEmptyBlockCount);
        //     this.states.write(buffer);
        //     this.biomes.write(buffer);
        //
        // Each container is `bits`, then its palette (absent when the container
        // went global, where a value is its own index), then the packed words.
        // The words carry NO length prefix — MC writeFixedSizeLongArray —
        // because the reader derives the count from `bits` and the entry count.
        //
        // This replaced a fixed 16-bits-per-block encoding plus a separate
        // 4096-byte state plane and a flat 1536-entry biome array: ~8.2 KB per
        // section and 3 KB of biomes per chunk, versus a palette that is
        // typically 4 bits and a biome container that is usually one entry.
        struct ContainerData {
            uint8_t               bits = 0;   // 0 = single-value palette
            std::vector<uint32_t> palette;    // empty when global
            std::vector<uint64_t> words;      // empty when bits == 0
        };

        struct SectionData {
            uint16_t      blockCount = 0;   // non-air voxels (MC nonEmptyBlockCount)
            ContainerData states;
            ContainerData biomes;

            bool IsEmpty() const { return blockCount == 0; }
        };

        // Section data for each section with bit set in primaryBitmask
        std::vector<SectionData> sections;

        // Timestamp for tracking
        std::chrono::steady_clock::time_point timestamp;

        ChunkDataS2CPacket() = default;
        ChunkDataS2CPacket(int32_t x, int32_t z)
            : chunkX(x), chunkZ(z), timestamp(std::chrono::steady_clock::now()) {}

        // Calculate total data size for the packet
        size_t CalculateDataSize() const {
            auto containerSize = [](const ContainerData& c) {
                return sizeof(uint8_t)
                     + c.palette.size() * sizeof(uint32_t)
                     + c.words.size() * sizeof(uint64_t);
            };
            size_t size = 0;
            for (const auto& section : sections) {
                size += sizeof(section.blockCount);
                size += containerSize(section.states);
                size += containerSize(section.biomes);
            }
            return size;
        }
    };

    namespace Serialization {

        // MC PalettedContainer.write: bits, palette (unless global), raw words.
        inline void WriteContainer(Network::PacketBuffer& buffer,
                                   const ChunkDataS2CPacket::ContainerData& c) {
            buffer.WriteByte(c.bits);
            // A global container has no palette to send. The reader knows from
            // `bits` alone, exactly as MC's does.
            if (!c.palette.empty()) {
                buffer.WriteVarInt(static_cast<uint32_t>(c.palette.size()));
                for (uint32_t id : c.palette) buffer.WriteVarInt(id);
            } else if (c.bits > 0) {
                buffer.WriteVarInt(0);   // global: explicit empty palette
            }
            for (uint64_t w : c.words) buffer.WriteLong(w);
        }

        // `entryCount` is what makes the length prefix unnecessary: the word
        // count follows from it and `bits`.
        inline bool ReadContainer(Network::PacketReader& reader,
                                  ChunkDataS2CPacket::ContainerData& c,
                                  int entryCount) {
            if (reader.Remaining() < 1) return false;
            c.bits = reader.ReadByte();

            if (c.bits > 0) {
                const uint32_t paletteSize = reader.ReadVarInt();
                c.palette.reserve(paletteSize);
                for (uint32_t i = 0; i < paletteSize; ++i) c.palette.push_back(reader.ReadVarInt());
            } else {
                // Single value: one palette entry, no words.
                const uint32_t paletteSize = reader.ReadVarInt();
                if (paletteSize != 1) return false;
                c.palette.push_back(reader.ReadVarInt());
                return true;
            }

            const int perLong = 64 / c.bits;
            const size_t words = static_cast<size_t>((entryCount + perLong - 1) / perLong);
            if (reader.Remaining() < words * sizeof(uint64_t)) return false;
            c.words.reserve(words);
            for (size_t i = 0; i < words; ++i) c.words.push_back(reader.ReadLong());
            return true;
        }

        inline std::vector<uint8_t> Serialize(const ChunkDataS2CPacket& packet) {
            Network::PacketBuffer buffer(4096);

            buffer.WriteInt(packet.chunkX);
            buffer.WriteInt(packet.chunkZ);
            buffer.WriteByte(packet.groundUpContinuous ? 1 : 0);
            buffer.WriteVarInt(packet.primaryBitmask);

            // Section data — ascending Y, matching the bitmask order.
            for (const auto& section : packet.sections) {
                buffer.WriteShort(section.blockCount);
                WriteContainer(buffer, section.states);
                WriteContainer(buffer, section.biomes);
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

            uint32_t sectionCount = 0;
            for (int i = 0; i < 24; ++i) {
                if (packet.primaryBitmask & (1u << i)) sectionCount++;
            }

            packet.sections.reserve(sectionCount);
            for (uint32_t i = 0; i < sectionCount; ++i) {
                ChunkDataS2CPacket::SectionData section;
                if (reader.Remaining() < sizeof(uint16_t)) break;
                section.blockCount = reader.ReadShort();
                if (!ReadContainer(reader, section.states, 4096)) break;
                if (!ReadContainer(reader, section.biomes, 64))   break;
                packet.sections.push_back(std::move(section));
            }

            packet.timestamp = std::chrono::steady_clock::now();
            return packet;
        }

    } // namespace Serialization

} // namespace Network

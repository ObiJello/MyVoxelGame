// File: src/common/network/packets/game/ChunkDataS2CPacket.hpp
//
// Mirrors MC ClientboundLevelChunkWithLightPacket (MC protocol 0x20-ish).
// Sends an entire chunk's section data to the client. Each section is a
// palletted container: blockCount + bitsPerEntry + (palette + dataArray).
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/world/math/WorldMath.hpp"   // SECTIONS_PER_CHUNK
#include <cstdint>
#include <vector>
#include <chrono>
#include <algorithm>

namespace Network {

    struct ChunkDataS2CPacket {
        // Chunk coordinates
        int32_t chunkX;
        int32_t chunkZ;

        // If true this is a full chunk load (the client throws away whatever
        // it had); otherwise an update to a chunk it already holds.
        bool groundUpContinuous = true;

        // NO SECTION BITMASK. MC removed `primaryBitMask` when it moved to
        // 3D biomes: ClientboundLevelChunkPacketData.extractChunkData:81 is
        //
        //     for (LevelChunkSection section : chunk.getSections())
        //         section.write(buffer);
        //
        // — every section, unconditionally, positionally. `sections` below is
        // therefore exactly SECTIONS_PER_CHUNK entries, indexed by section Y.
        //
        // The bitmask was not free to keep. Omitting all-air sections omits
        // their BIOMES too, and sky biomes are not recoverable from anywhere
        // else — MC's ClientboundChunksBiomesPacket is only produced by
        // runtime biome edits (ChunkMap.resendBiomesForChunks:1236), never as
        // part of chunk delivery. That is why blocks placed high above terrain
        // used to tint with the fallback biome.

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

            // "Holds no blocks" — NOT "skip this section". The distinction
            // cost real time: the client used to gate its whole decode on
            // `!IsEmpty()`, so an all-air section's BIOMES were thrown away
            // along with its (empty) block data. Biomes must be adopted
            // regardless; only the mesh-scheduling side cares about this.
            bool HasNoBlocks() const { return blockCount == 0; }
        };

        // Exactly SECTIONS_PER_CHUNK entries, indexed by section Y. An
        // all-air section is present and cheap (see CalculateDataSize).
        std::vector<SectionData> sections;

        // Timestamp for tracking
        std::chrono::steady_clock::time_point timestamp;

        ChunkDataS2CPacket() = default;
        ChunkDataS2CPacket(int32_t x, int32_t z)
            : chunkX(x), chunkZ(z), timestamp(std::chrono::steady_clock::now()) {}

        // Serialized size, used to size the send buffer and by the session's
        // byte accounting. Mirrors Serialize() below exactly — MC does the
        // same thing in ClientboundLevelChunkPacketData.calculateChunkSize,
        // which sizes a fixed buffer from LevelChunkSection.getSerializedSize
        // and then asserts the writer filled it precisely.
        //
        // Palette ids are VarInts, so charging sizeof(uint32_t) for each (as
        // this used to) over-counts by up to 3 bytes an entry.
        static size_t VarIntSize(uint32_t v) {
            size_t n = 1;
            while (v >= 0x80) { v >>= 7; ++n; }
            return n;
        }

        size_t CalculateDataSize() const {
            auto containerSize = [](const ContainerData& c) {
                size_t n = sizeof(uint8_t);                     // bits
                if (!c.palette.empty()) {
                    n += VarIntSize(static_cast<uint32_t>(c.palette.size()));
                    for (uint32_t id : c.palette) n += VarIntSize(id);
                } else if (c.bits > 0) {
                    n += 1;                                     // explicit empty palette
                }
                return n + c.words.size() * sizeof(uint64_t);
            };
            // chunkX + chunkZ + groundUpContinuous
            size_t size = sizeof(int32_t) * 2 + sizeof(uint8_t);
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

            // Palette size is attacker-controlled and feeds reserve(), so it
            // is bounded before it is believed. A container can never hold
            // more distinct values than it has entries, nor more than its
            // index width addresses. PacketReader::ReadVarInt does no bounds
            // checking of its own, so Remaining() is checked ahead of each.
            if (c.bits > 0) {
                if (reader.Remaining() < 1) return false;
                const uint32_t paletteSize = reader.ReadVarInt();
                const uint64_t widthCap = (c.bits >= 32) ? 0xFFFFFFFFull
                                                         : (1ull << c.bits);
                if (paletteSize > widthCap ||
                    paletteSize > static_cast<uint32_t>(entryCount)) return false;
                if (reader.Remaining() < paletteSize) return false;   // >= 1 byte each
                c.palette.reserve(paletteSize);
                for (uint32_t i = 0; i < paletteSize; ++i) {
                    if (reader.Remaining() < 1) return false;
                    c.palette.push_back(reader.ReadVarInt());
                }
            } else {
                // Single value: one palette entry, no words.
                if (reader.Remaining() < 1) return false;
                const uint32_t paletteSize = reader.ReadVarInt();
                if (paletteSize != 1) return false;
                if (reader.Remaining() < 1) return false;
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
            Network::PacketBuffer buffer(packet.CalculateDataSize());

            buffer.WriteInt(packet.chunkX);
            buffer.WriteInt(packet.chunkZ);
            buffer.WriteByte(packet.groundUpContinuous ? 1 : 0);

            // Every section, ascending Y, positionally — MC
            // ClientboundLevelChunkPacketData.extractChunkData:81. No mask and
            // no per-section framing, so the reader's loop count is fixed and
            // section Y is the index.
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

            // Fixed count, from the shared constant rather than a literal —
            // the sender loops SECTIONS_PER_CHUNK, and with the mask gone the
            // two counts being equal is the only thing keeping the stream in
            // phase. A short read below leaves the tail sections default
            // constructed (all air), which the client treats as such.
            packet.sections.reserve(Game::Math::SECTIONS_PER_CHUNK);
            for (int i = 0; i < Game::Math::SECTIONS_PER_CHUNK; ++i) {
                ChunkDataS2CPacket::SectionData section;
                if (reader.Remaining() < sizeof(uint16_t)) break;
                section.blockCount = reader.ReadShort();
                if (!ReadContainer(reader, section.states, 4096)) break;
                if (!ReadContainer(reader, section.biomes, 64))   break;
                packet.sections.push_back(std::move(section));
            }

            // Hold the positional invariant even for a truncated stream: the
            // tail stays default constructed, which decodes as all air. The
            // client indexes `sections[sectionY]` directly, so a short vector
            // would otherwise be an out-of-bounds read rather than a visibly
            // empty chunk.
            packet.sections.resize(Game::Math::SECTIONS_PER_CHUNK);

            packet.timestamp = std::chrono::steady_clock::now();
            return packet;
        }

    } // namespace Serialization

} // namespace Network

// File: src/common/network/packets/game/ClientboundSectionBlocksUpdateS2CPacket.hpp
//
// Mirrors MC ClientboundSectionBlocksUpdatePacket (1.16.2+ format). Per-section
// packed-record layout, matching MC's own `(blockState << 12) | position` long:
//   each record = (blockId << 20) | (stateIndex << 12) | (localX << 8) | (localZ << 4) | localY
// All changes MUST be within the same chunk section (16x16x16).
//
// The record is a uint64_t, not a uint32_t, for the same reason MC uses a long:
// the block-state value does not fit alongside the position in 32 bits. The old
// layout gave the block id only 12 bits (4095 values) while BlockDefs.inc
// already defines over 1100 blocks and grows with every MC version — and that
// was before the state index needed a home. 64 bits leaves the id a full 16 and
// the state a full 8 with room to spare.
//
// The SENDER is IntegratedServer::SendSectionBlocksUpdateS2CPacket, which
// hand-rolls the byte layout. The decoder below mirrors it exactly:
//   chunkX  : 2 bytes, big-endian
//   chunkZ  : 2 bytes, big-endian
//   sectionY: 1 byte
//   count   : VarInt
//   records : `count` 64-bit VarInts
//
// This is what carries a change the player did NOT predict — the second half of
// a double chest being re-typed when its partner is placed, for one. Without a
// decoder the client logged "Unhandled packet ID: 0x22" and silently kept the
// stale state, so the paired chest went on rendering as a single.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/world/math/WorldMath.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct ClientboundSectionBlocksUpdateS2CPacket {
        Game::Math::ChunkPos chunkPos;
        int32_t              sectionY;  // Section index (0-23 for -64 to 319 world height)
        std::vector<uint64_t> packedRecords;

        ClientboundSectionBlocksUpdateS2CPacket() = default;
        ClientboundSectionBlocksUpdateS2CPacket(Game::Math::ChunkPos pos, int32_t section)
            : chunkPos(pos), sectionY(section) {}

        // Helper to add a block change. `stateIndex` is the block-state index
        // within blockId's own state list (MC BlockState.getId()); 0 = default.
        void AddChange(uint8_t localX, uint8_t localY, uint8_t localZ,
                       uint16_t blockId, uint8_t stateIndex = 0) {
            uint64_t packed = (static_cast<uint64_t>(blockId)    << 20) |
                              (static_cast<uint64_t>(stateIndex) << 12) |
                              (static_cast<uint64_t>(localX & 0xF) << 8) |
                              (static_cast<uint64_t>(localZ & 0xF) << 4) |
                              (static_cast<uint64_t>(localY & 0xF));
            packedRecords.push_back(packed);
        }

        // Helper to unpack a record.
        static void UnpackRecord(uint64_t packed, uint8_t& localX, uint8_t& localY,
                                 uint8_t& localZ, uint16_t& blockId, uint8_t& stateIndex) {
            localX     = (packed >> 8)  & 0xF;
            localZ     = (packed >> 4)  & 0xF;
            localY     =  packed        & 0xF;
            stateIndex = static_cast<uint8_t>((packed >> 12) & 0xFF);
            blockId    = static_cast<uint16_t>((packed >> 20) & 0xFFFF);
        }
    };

    namespace Serialization {

        inline ClientboundSectionBlocksUpdateS2CPacket
        DeserializeClientboundSectionBlocksUpdate(const std::vector<uint8_t>& data) {
            ClientboundSectionBlocksUpdateS2CPacket packet;
            size_t i = 0;
            auto byte = [&]() -> uint8_t { return i < data.size() ? data[i++] : 0; };

            // Read each byte into its own variable first: the two calls in
            // `(byte() << 8) | byte()` are UNSEQUENCED, so the compiler may
            // evaluate them in either order and swap the halves.
            const uint8_t cxHi = byte(), cxLo = byte();
            const uint8_t czHi = byte(), czLo = byte();
            const int16_t cx = static_cast<int16_t>((cxHi << 8) | cxLo);
            const int16_t cz = static_cast<int16_t>((czHi << 8) | czLo);
            packet.chunkPos  = Game::Math::ChunkPos{cx, cz};
            packet.sectionY  = static_cast<int8_t>(byte());

            auto readVarInt64 = [&]() -> uint64_t {
                uint64_t value = 0; int shift = 0;
                while (i < data.size()) {
                    const uint8_t b = data[i++];
                    value |= static_cast<uint64_t>(b & 0x7F) << shift;
                    if ((b & 0x80) == 0) break;
                    shift += 7;
                    if (shift >= 64) break;
                }
                return value;
            };

            const uint32_t count = static_cast<uint32_t>(readVarInt64());
            packet.packedRecords.reserve(count);
            for (uint32_t n = 0; n < count && i < data.size(); ++n) {
                packet.packedRecords.push_back(readVarInt64());
            }
            return packet;
        }

    } // namespace Serialization

} // namespace Network

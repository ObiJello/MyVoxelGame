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
// NOTE: this packet currently has no Serialize / Deserialize implementation
// (it's plumbed but no live caller emits it yet). Add when the multi-block
// change broadcast path migrates from MultiBlockChangeS2CPacket.
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

} // namespace Network

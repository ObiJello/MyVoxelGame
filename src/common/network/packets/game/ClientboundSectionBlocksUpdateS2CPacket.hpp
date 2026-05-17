// File: src/common/network/packets/game/ClientboundSectionBlocksUpdateS2CPacket.hpp
//
// Mirrors MC ClientboundSectionBlocksUpdatePacket (1.16.2+ format). Per-section
// packed-record layout:
//   each VarInt = (localX << 12) | (localZ << 8) | (localY << 4) | blockStateId
// All changes MUST be within the same chunk section (16x16x16).
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
        std::vector<uint32_t> packedRecords;

        ClientboundSectionBlocksUpdateS2CPacket() = default;
        ClientboundSectionBlocksUpdateS2CPacket(Game::Math::ChunkPos pos, int32_t section)
            : chunkPos(pos), sectionY(section) {}

        // Helper to add a block change.
        void AddChange(uint8_t localX, uint8_t localY, uint8_t localZ, uint16_t blockStateId) {
            uint32_t packed = (static_cast<uint32_t>(localX & 0xF) << 12) |
                              (static_cast<uint32_t>(localZ & 0xF) << 8) |
                              (static_cast<uint32_t>(localY & 0xF) << 4) |
                              (blockStateId & 0xFFF);  // up to 12 bits for block ID
            packedRecords.push_back(packed);
        }

        // Helper to unpack a record.
        static void UnpackRecord(uint32_t packed, uint8_t& localX, uint8_t& localY,
                                 uint8_t& localZ, uint16_t& blockStateId) {
            localX = (packed >> 12) & 0xF;
            localZ = (packed >> 8)  & 0xF;
            localY = (packed >> 4)  & 0xF;
            blockStateId = packed & 0xFFF;
        }
    };

} // namespace Network

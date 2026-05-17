// File: src/common/network/packets/game/MultiBlockChangeS2CPacket.hpp
//
// Older-style multi-block-change packet (one chunk's worth of changes in
// one packet). The MC equivalent uses the per-section packed-record layout
// (see ClientboundSectionBlocksUpdateS2CPacket) but this fallback path is
// kept for callers that haven't migrated.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/world/math/WorldMath.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct MultiBlockChangeS2CPacket {
        Game::Math::ChunkPos chunkPos;

        struct BlockChange {
            uint8_t       localX, localY, localZ; // 0-15 within chunk
            Game::BlockID blockId;
        };

        std::vector<BlockChange> changes;
        uint32_t timestamp;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const MultiBlockChangeS2CPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteInt(packet.chunkPos.x);
            buffer.WriteInt(packet.chunkPos.z);
            buffer.WriteVarInt(static_cast<uint32_t>(packet.changes.size()));
            for (const auto& change : packet.changes) {
                buffer.WriteByte(change.localX);
                buffer.WriteByte(change.localY);
                buffer.WriteByte(change.localZ);
                buffer.WriteShort(static_cast<uint16_t>(change.blockId));
            }
            buffer.WriteInt(packet.timestamp);
            return buffer.GetData();
        }

        inline MultiBlockChangeS2CPacket DeserializeMultiBlockChangeS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            MultiBlockChangeS2CPacket packet;
            packet.chunkPos.x = reader.ReadInt();
            packet.chunkPos.z = reader.ReadInt();
            uint32_t changeCount = reader.ReadVarInt();
            packet.changes.reserve(changeCount);
            for (uint32_t i = 0; i < changeCount; ++i) {
                MultiBlockChangeS2CPacket::BlockChange change;
                change.localX = reader.ReadByte();
                change.localY = reader.ReadByte();
                change.localZ = reader.ReadByte();
                change.blockId = static_cast<Game::BlockID>(reader.ReadShort());
                packet.changes.push_back(change);
            }
            packet.timestamp = reader.ReadInt();
            return packet;
        }

    } // namespace Serialization

} // namespace Network

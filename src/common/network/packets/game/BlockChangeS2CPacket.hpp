// File: src/common/network/packets/game/BlockChangeS2CPacket.hpp
//
// Pre-MC-style single block change broadcast. Older naming kept for the
// existing in-engine path (see ClientboundBlockUpdateS2CPacket below for
// the modern MC-named equivalent).
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/world/block/Blocks.hpp"
#include <cstdint>
#include <vector>
#include <chrono>

namespace Network {

    struct BlockChangeS2CPacket {
        int worldX, worldY, worldZ;
        Game::BlockID newBlockId;
        uint32_t timestamp;
        bool playSound       = true;
        bool updateNeighbors = true;

        BlockChangeS2CPacket() = default;
        BlockChangeS2CPacket(int x, int y, int z, Game::BlockID blockId)
            : worldX(x), worldY(y), worldZ(z), newBlockId(blockId)
            , timestamp(static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count())) {}
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const BlockChangeS2CPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteInt(packet.worldX);
            buffer.WriteInt(packet.worldY);
            buffer.WriteInt(packet.worldZ);
            buffer.WriteShort(static_cast<uint16_t>(packet.newBlockId));
            buffer.WriteByte((packet.playSound ? 0x01 : 0) | (packet.updateNeighbors ? 0x02 : 0));
            return buffer.GetData();
        }

        inline BlockChangeS2CPacket DeserializeBlockChangeS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            BlockChangeS2CPacket packet;
            packet.worldX = reader.ReadInt();
            packet.worldY = reader.ReadInt();
            packet.worldZ = reader.ReadInt();
            packet.newBlockId = static_cast<Game::BlockID>(reader.ReadShort());
            uint8_t flags = reader.ReadByte();
            packet.playSound = (flags & 0x01) != 0;
            packet.updateNeighbors = (flags & 0x02) != 0;
            packet.timestamp = static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
            return packet;
        }

    } // namespace Serialization

} // namespace Network

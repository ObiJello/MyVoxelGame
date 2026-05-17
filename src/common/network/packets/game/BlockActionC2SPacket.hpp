// File: src/common/network/packets/game/BlockActionC2SPacket.hpp
//
// Older-style multi-purpose block action (BREAK / PLACE / INTERACT) used by
// our pre-MC-protocol path. The MC equivalent splits these across
// ServerboundPlayerActionPacket (break stages) and ServerboundUseItemOnPacket
// (place/interact). Kept for callers that haven't migrated.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/world/block/Blocks.hpp"
#include "../common/PacketCommon.hpp"   // BlockActionType
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace Network {

    struct BlockActionC2SPacket {
        int worldX, worldY, worldZ;
        BlockActionType action;
        Game::BlockID   blockId = Game::BlockID::Air; // For PLACE action
        uint8_t         face    = 0;                  // 0..5
        glm::vec3       hitPosition;                  // exact hit point (for placement)
        uint32_t        sequenceNumber = 0;

        BlockActionC2SPacket() = default;
        BlockActionC2SPacket(int x, int y, int z, BlockActionType act)
            : worldX(x), worldY(y), worldZ(z), action(act) {}
        BlockActionC2SPacket(int x, int y, int z, BlockActionType act, Game::BlockID block)
            : worldX(x), worldY(y), worldZ(z), action(act), blockId(block) {}
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const BlockActionC2SPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteInt(packet.worldX);
            buffer.WriteInt(packet.worldY);
            buffer.WriteInt(packet.worldZ);
            buffer.WriteByte(static_cast<uint8_t>(packet.action));
            buffer.WriteShort(static_cast<uint16_t>(packet.blockId));
            buffer.WriteByte(packet.face);
            buffer.WriteFloat(packet.hitPosition.x);
            buffer.WriteFloat(packet.hitPosition.y);
            buffer.WriteFloat(packet.hitPosition.z);
            buffer.WriteVarInt(packet.sequenceNumber);
            return buffer.GetData();
        }

        inline BlockActionC2SPacket DeserializeBlockActionC2S(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            BlockActionC2SPacket packet;
            packet.worldX = reader.ReadInt();
            packet.worldY = reader.ReadInt();
            packet.worldZ = reader.ReadInt();
            packet.action = static_cast<BlockActionType>(reader.ReadByte());
            packet.blockId = static_cast<Game::BlockID>(reader.ReadShort());
            packet.face = reader.ReadByte();
            packet.hitPosition.x = reader.ReadFloat();
            packet.hitPosition.y = reader.ReadFloat();
            packet.hitPosition.z = reader.ReadFloat();
            packet.sequenceNumber = reader.ReadVarInt();
            return packet;
        }

    } // namespace Serialization

} // namespace Network

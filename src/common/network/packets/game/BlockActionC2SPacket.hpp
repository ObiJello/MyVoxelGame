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
        // Block-state index of the block being broken, captured client-side at
        // the START of the dig.
        //
        // The server cannot read this itself. In integrated-server mode client
        // and server share one World, so the client's local break prediction
        // has already cleared the cell by the time this packet is handled —
        // which is exactly why `blockId` above is carried in the packet rather
        // than read from the world. The state needs the same treatment, and
        // without it every loot condition on a state reads the DEFAULT: a fully
        // grown wheat evaluated as age=0 and dropped seeds instead of wheat.
        Game::BlockStateIndex         blockState = 0;

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
            buffer.WriteShort(packet.blockState);
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
            // Tail-appended, same pattern as BlockChangeS2CPacket's state byte:
            // absent from a pre-state sender, which means "default state" — the
            // correct reading for a block that carries no properties.
            if (reader.Remaining() >= 1) {
                packet.blockState = reader.ReadShort();
            }
            return packet;
        }

    } // namespace Serialization

} // namespace Network

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
        // The face the dig started on, RaycastHit::hitFace order (0=+X,
        // 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z), or kFaceUnknown. The vein mine's
        // tunnel shape runs into it.
        static constexpr uint8_t kFaceUnknown = 255;
        uint8_t         face    = kFaceUnknown;
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
        // Immersive portals: the dimension the targeted block is in, which
        // is not the player's own when they reach through a portal. Trailing
        // and optional on the wire; kDimensionUnknown = the player's own.
        static constexpr int8_t kDimensionUnknown = 127;
        int8_t          dimensionId = kDimensionUnknown;
        // Vein mine: on STOP_DESTROY / BREAK, the server also breaks every
        // touching block of the same kind (PlayerSession::VeinMineFrom).
        // Set when the player held the vein-mine key together with Sneak.
        // Trailing and optional on the wire; absent = false.
        bool            veinMine = false;

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
            buffer.WriteByte(static_cast<uint8_t>(packet.dimensionId));
            buffer.WriteByte(packet.veinMine ? 1 : 0);
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
            if (reader.Remaining() >= 2) {
                packet.blockState = reader.ReadShort();
            }
            if (reader.Remaining() >= 1) {
                packet.dimensionId = static_cast<int8_t>(reader.ReadByte());
            }
            if (reader.Remaining() >= 1) {
                packet.veinMine = reader.ReadByte() != 0;
            }
            return packet;
        }

    } // namespace Serialization

} // namespace Network

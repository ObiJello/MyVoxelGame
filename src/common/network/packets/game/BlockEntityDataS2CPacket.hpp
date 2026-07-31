// File: src/common/network/packets/game/BlockEntityDataS2CPacket.hpp
//
// Server → client BlockEntity create / state update. Mirrors MC
// `ClientboundBlockEntityDataPacket` but with our binary-tagged blob instead
// of NBT (Stage 1 ships without the NBT codec; later stages can swap the
// blob format without changing the packet wire layout).
//
// Carries:
//   - worldPos     : absolute block position the BE lives at
//   - typeId       : BlockEntityType wire id (BlockEntityTypeIds::*)
//   - dataBlob     : opaque bytes — produced by BlockEntity::Save and consumed
//                    by BlockEntity::Load via PacketBuffer / PacketReader.
//                    Empty for stateless / Stage-1 BEs.
//
// Companion packet `BlockEntityRemoveS2CPacket` (defined below in the same
// header for locality) tells the client to delete the BE at a given position —
// usually triggered when the underlying block is replaced with a non-BE block.
// MC implicitly handles this via the new block's blockstate; we send an
// explicit packet so client-side lifecycle is symmetric with the server.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct BlockEntityDataS2CPacket {
        int32_t  worldX = 0;
        int32_t  worldY = 0;
        int32_t  worldZ = 0;
        uint16_t typeId = 0;
        std::vector<uint8_t> dataBlob;

        BlockEntityDataS2CPacket() = default;
        BlockEntityDataS2CPacket(int x, int y, int z, uint16_t type)
            : worldX(x), worldY(y), worldZ(z), typeId(type) {}
    };

    struct BlockEntityRemoveS2CPacket {
        int32_t worldX = 0;
        int32_t worldY = 0;
        int32_t worldZ = 0;

        BlockEntityRemoveS2CPacket() = default;
        BlockEntityRemoveS2CPacket(int x, int y, int z)
            : worldX(x), worldY(y), worldZ(z) {}
    };

    // Stage 4: "block event" — MC's Level.blockEvent path. Triggers a
    // discrete client-side animation on the BE without sending its full
    // state (chest lid open count, bell ring, note-block sound, …).
    // `actionType` is BE-specific (e.g. 1 = openersCounter changed for chest);
    // `actionParam` carries the small data payload.
    struct BlockEntityActionS2CPacket {
        int32_t  worldX = 0;
        int32_t  worldY = 0;
        int32_t  worldZ = 0;
        uint8_t  actionType  = 0;
        uint8_t  actionParam = 0;

        BlockEntityActionS2CPacket() = default;
        BlockEntityActionS2CPacket(int x, int y, int z, uint8_t type, uint8_t param)
            : worldX(x), worldY(y), worldZ(z), actionType(type), actionParam(param) {}
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const BlockEntityDataS2CPacket& packet) {
            PacketBuffer buffer(32 + packet.dataBlob.size());
            buffer.WriteInt(packet.worldX);
            buffer.WriteInt(packet.worldY);
            buffer.WriteInt(packet.worldZ);
            buffer.WriteShort(packet.typeId);
            buffer.WriteVarInt(static_cast<uint32_t>(packet.dataBlob.size()));
            if (!packet.dataBlob.empty()) buffer.WriteBytes(packet.dataBlob);
            return buffer.GetData();
        }

        inline BlockEntityDataS2CPacket DeserializeBlockEntityDataS2C(
                const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            BlockEntityDataS2CPacket p;
            p.worldX = reader.ReadInt();
            p.worldY = reader.ReadInt();
            p.worldZ = reader.ReadInt();
            p.typeId = reader.ReadShort();
            uint32_t blobSize = reader.ReadVarInt();
            if (blobSize > 0) p.dataBlob = reader.ReadBytes(blobSize);
            return p;
        }

        inline std::vector<uint8_t> Serialize(const BlockEntityRemoveS2CPacket& packet) {
            PacketBuffer buffer(12);
            buffer.WriteInt(packet.worldX);
            buffer.WriteInt(packet.worldY);
            buffer.WriteInt(packet.worldZ);
            return buffer.GetData();
        }

        inline BlockEntityRemoveS2CPacket DeserializeBlockEntityRemoveS2C(
                const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            BlockEntityRemoveS2CPacket p;
            p.worldX = reader.ReadInt();
            p.worldY = reader.ReadInt();
            p.worldZ = reader.ReadInt();
            return p;
        }

        inline std::vector<uint8_t> Serialize(const BlockEntityActionS2CPacket& packet) {
            PacketBuffer buffer(14);
            buffer.WriteInt(packet.worldX);
            buffer.WriteInt(packet.worldY);
            buffer.WriteInt(packet.worldZ);
            buffer.WriteByte(packet.actionType);
            buffer.WriteByte(packet.actionParam);
            return buffer.GetData();
        }

        inline BlockEntityActionS2CPacket DeserializeBlockEntityActionS2C(
                const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            BlockEntityActionS2CPacket p;
            p.worldX = reader.ReadInt();
            p.worldY = reader.ReadInt();
            p.worldZ = reader.ReadInt();
            p.actionType  = reader.ReadByte();
            p.actionParam = reader.ReadByte();
            return p;
        }

    } // namespace Serialization

} // namespace Network

// File: src/common/network/packets/game/UseItemOnC2SPacket.hpp
//
// Mirrors MC ServerboundUseItemOnPacket (1.14+ style). Sent on right-click
// when the ray hits a block. Server's ServerPlayerGameMode.useItemOn flow
// runs `block.useItemOn` → `block.useWithoutItem` → `item.useOn` →
// BlockItem placement (see PlayerSession::HandleUseItemOn).
//
// Wire format note: MC packs pos+face+cursor+insideBlock into a single
// `BlockHitResult` blob (writeBlockHitResult). We write each piece
// separately, so this is logically identical but NOT byte-compatible with
// vanilla MC.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct UseItemOnC2SPacket {
        uint32_t hand = 0;            // VarInt: 0=main hand, 1=off hand
        int32_t  blockX = 0;
        int32_t  blockY = 0;
        int32_t  blockZ = 0;
        uint32_t direction = 0;       // VarInt: clicked face (0=bottom, 1=top, 2=N, 3=S, 4=W, 5=E)
        float    cursorX = 0.0f;      // [0,1) hit position in block-local coords
        float    cursorY = 0.0f;
        float    cursorZ = 0.0f;
        bool     insideBlock = false; // raycast started inside the block volume
        uint32_t sequence = 0;        // VarInt: monotonic interaction id for ack
        bool     altInteract = false; // false = right-click (default), true = left-click
                                      //   "use" semantics (only meaningful for items
                                      //   that overload left-click — currently just
                                      //   PortalGun: left=blue, right=orange).

        UseItemOnC2SPacket() = default;
        UseItemOnC2SPacket(uint32_t h, int32_t x, int32_t y, int32_t z, uint32_t dir,
                           float cx, float cy, float cz, bool inside, uint32_t seq,
                           bool alt = false)
            : hand(h), blockX(x), blockY(y), blockZ(z), direction(dir),
              cursorX(cx), cursorY(cy), cursorZ(cz), insideBlock(inside), sequence(seq),
              altInteract(alt) {}
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const UseItemOnC2SPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteVarInt(packet.hand);
            buffer.WriteInt(packet.blockX);
            buffer.WriteInt(packet.blockY);
            buffer.WriteInt(packet.blockZ);
            buffer.WriteVarInt(packet.direction);
            buffer.WriteFloat(packet.cursorX);
            buffer.WriteFloat(packet.cursorY);
            buffer.WriteFloat(packet.cursorZ);
            buffer.WriteByte(packet.insideBlock ? 0x01 : 0x00);
            buffer.WriteVarInt(packet.sequence);
            buffer.WriteByte(packet.altInteract ? 0x01 : 0x00);
            return buffer.GetData();
        }

        inline UseItemOnC2SPacket DeserializeUseItemOnC2S(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            UseItemOnC2SPacket packet;
            packet.hand = reader.ReadVarInt();
            packet.blockX = reader.ReadInt();
            packet.blockY = reader.ReadInt();
            packet.blockZ = reader.ReadInt();
            packet.direction = reader.ReadVarInt();
            packet.cursorX = reader.ReadFloat();
            packet.cursorY = reader.ReadFloat();
            packet.cursorZ = reader.ReadFloat();
            packet.insideBlock = reader.ReadByte() != 0;
            packet.sequence = reader.ReadVarInt();
            // altInteract — appended at end so older serialized packets
            // (without this byte) cleanly default to false.
            packet.altInteract = reader.HasMore() ? (reader.ReadByte() != 0) : false;
            return packet;
        }

    } // namespace Serialization

} // namespace Network

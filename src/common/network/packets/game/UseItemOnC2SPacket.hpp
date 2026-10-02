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
#include <cmath>
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
        // Immersive portals: the dimension the clicked block is in (see
        // BlockActionC2SPacket::dimensionId). Trailing, optional.
        static constexpr int8_t kDimensionUnknown = 127;
        int8_t   dimensionId = kDimensionUnknown;
                                      //   "use" semantics (only meaningful for items
                                      //   that overload left-click — currently just
                                      //   PortalGun: left=blue, right=orange).
        // The hit came from the held item's own `use` clip, not from the
        // crosshair: MC PlaceOnWaterBlockItem.use (lily pad, frogspawn) clips
        // with Fluid.SOURCE_ONLY and hands BlockItem.useOn the cell ABOVE what
        // it hit. The server runs that as a bare BlockItem placement — no
        // block use, no item useOn. Trailing, optional.
        bool     fromUse = false;
        // The look rotation the click was made with, in the CLICKED BLOCK'S
        // space (MC yaw/pitch degrees): the player's own rotation, or — when
        // the ray reached the block through a portal — the look vector mapped
        // through that portal. Block placement derives facing from it (MC
        // BlockPlaceContext.getHorizontalDirection / getNearestLookingDirection
        // / getRotation). Rotation is client-authoritative anyway (every move
        // packet carries it), so trusting it grants nothing new. Trailing,
        // optional: absent → the server uses the player's synced rotation.
        bool     hasLookRotation = false;
        float    lookYaw   = 0.0f;
        float    lookPitch = 0.0f;

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
            buffer.WriteByte(static_cast<uint8_t>(packet.dimensionId));
            buffer.WriteByte(packet.fromUse ? 0x01 : 0x00);
            buffer.WriteByte(packet.hasLookRotation ? 0x01 : 0x00);
            if (packet.hasLookRotation) {
                buffer.WriteFloat(packet.lookYaw);
                buffer.WriteFloat(packet.lookPitch);
            }
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
            packet.dimensionId = reader.HasMore() ? static_cast<int8_t>(reader.ReadByte()) : packet.kDimensionUnknown;
            packet.fromUse = reader.HasMore() ? (reader.ReadByte() != 0) : false;
            packet.hasLookRotation = reader.HasMore() ? (reader.ReadByte() != 0) : false;
            if (packet.hasLookRotation) {
                packet.lookYaw   = reader.ReadFloat();
                packet.lookPitch = reader.ReadFloat();
                // A malformed rotation is dropped, not trusted.
                if (!std::isfinite(packet.lookYaw) || !std::isfinite(packet.lookPitch)) {
                    packet.hasLookRotation = false;
                    packet.lookYaw = packet.lookPitch = 0.0f;
                }
            }
            return packet;
        }

    } // namespace Serialization

} // namespace Network

// File: src/common/network/packets/game/FillBlocksC2SPacket.hpp
//
// FillBlocksC2S — "place my held block in every open cell of this box".
// The fill tool: Alt + right-click marks one corner (client-side), a plain
// right-click elsewhere sends both corners here. The server places what
// fits (air and replaceable cells only, never through the player), takes
// the items in survival, and answers in chat.
//
//   VarInt hand
//   Int    x0 y0 z0, x1 y1 z1      the two corner cells, any order
//   Int    rawState                the block state the client showed in its
//                                  preview (checked against the held block)
//   Byte   dimensionId             the level the corners are in
#pragma once

#include "common/network/PacketRegistry.hpp"

#include <cstdint>
#include <vector>

namespace Network {

    struct FillBlocksC2SPacket {
        uint32_t hand = 0;
        int32_t  x0 = 0, y0 = 0, z0 = 0;
        int32_t  x1 = 0, y1 = 0, z1 = 0;
        uint32_t rawState = 0;
        int8_t   dimensionId = 0;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const FillBlocksC2SPacket& p) {
            PacketBuffer b;
            b.WriteVarInt(p.hand);
            b.WriteInt(static_cast<uint32_t>(p.x0)); b.WriteInt(static_cast<uint32_t>(p.y0)); b.WriteInt(static_cast<uint32_t>(p.z0));
            b.WriteInt(static_cast<uint32_t>(p.x1)); b.WriteInt(static_cast<uint32_t>(p.y1)); b.WriteInt(static_cast<uint32_t>(p.z1));
            b.WriteInt(p.rawState);
            b.WriteByte(static_cast<uint8_t>(p.dimensionId));
            return b.GetData();
        }

        inline FillBlocksC2SPacket DeserializeFillBlocksC2S(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            FillBlocksC2SPacket p;
            p.hand = r.ReadVarInt();
            p.x0 = static_cast<int32_t>(r.ReadInt()); p.y0 = static_cast<int32_t>(r.ReadInt()); p.z0 = static_cast<int32_t>(r.ReadInt());
            p.x1 = static_cast<int32_t>(r.ReadInt()); p.y1 = static_cast<int32_t>(r.ReadInt()); p.z1 = static_cast<int32_t>(r.ReadInt());
            p.rawState = r.ReadInt();
            p.dimensionId = static_cast<int8_t>(r.ReadByte());
            return p;
        }

    } // namespace Serialization

} // namespace Network

// File: src/common/network/packets/game/OpenSignEditorS2CPacket.hpp
//
// Server → client: open the sign editor for the sign at `pos`, editing
// `front` (MC ClientboundOpenSignEditorPacket(pos, SignTextSlot)). Sent
// right after a sign is placed and on an empty-hand click on one.
//
// MC sends a ClientboundBlockUpdatePacket first so the client is sure to
// have the sign; here the packet carries what the editor needs itself —
// the block (the wood picks the sprite and the kind) and the face's
// current lines and ink — so the editor never depends on the client's
// chunk copy being in step.
//
// Wire: int32 x, y, z; byte slot (0 = back, 1 = front — MC's SignTextSlot
// ids); uint16 blockId; 4 × string; byte dye colour; byte glowing.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Network {

    struct OpenSignEditorS2CPacket {
        glm::ivec3 pos{0};
        bool       front = true;
        uint16_t   blockId = 0;
        std::array<std::string, 4> lines{};
        uint8_t    color = 15;      // Game::DyeColor ordinal; 15 = black
        bool       glowing = false;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const OpenSignEditorS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteInt(static_cast<uint32_t>(packet.pos.x));
            buffer.WriteInt(static_cast<uint32_t>(packet.pos.y));
            buffer.WriteInt(static_cast<uint32_t>(packet.pos.z));
            buffer.WriteByte(packet.front ? 1 : 0);
            buffer.WriteShort(packet.blockId);
            for (const std::string& line : packet.lines) buffer.WriteString(line);
            buffer.WriteByte(packet.color);
            buffer.WriteByte(packet.glowing ? 1 : 0);
            return buffer.GetData();
        }

        inline OpenSignEditorS2CPacket DeserializeOpenSignEditorS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            OpenSignEditorS2CPacket packet;
            packet.pos.x = static_cast<int32_t>(reader.ReadInt());
            packet.pos.y = static_cast<int32_t>(reader.ReadInt());
            packet.pos.z = static_cast<int32_t>(reader.ReadInt());
            packet.front = reader.ReadByte() != 0;
            if (reader.HasMore()) packet.blockId = reader.ReadShort();
            for (std::string& line : packet.lines) {
                if (!reader.HasMore()) return packet;
                line = reader.ReadString();
            }
            if (reader.HasMore()) packet.color = reader.ReadByte();
            if (reader.HasMore()) packet.glowing = reader.ReadByte() != 0;
            return packet;
        }

    } // namespace Serialization

} // namespace Network

// File: src/common/network/packets/game/SignUpdateC2SPacket.hpp
//
// Client → server: the sign editor closed — here are the four lines (MC
// ServerboundSignUpdatePacket(pos, lines, slot)). MC caps each line at 384
// bytes of UTF-8 on the wire; the editor itself keeps them under the
// board's width.
//
// Wire: int32 x, y, z; byte slot (0 back, 1 front); 4 × string.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Network {

    struct SignUpdateC2SPacket {
        static constexpr size_t kMaxLineLength = 384;
        glm::ivec3 pos{0};
        bool       front = true;
        std::array<std::string, 4> lines{};
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const SignUpdateC2SPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteInt(static_cast<uint32_t>(packet.pos.x));
            buffer.WriteInt(static_cast<uint32_t>(packet.pos.y));
            buffer.WriteInt(static_cast<uint32_t>(packet.pos.z));
            buffer.WriteByte(packet.front ? 1 : 0);
            for (const std::string& line : packet.lines) {
                buffer.WriteString(line.size() > SignUpdateC2SPacket::kMaxLineLength
                                       ? line.substr(0, SignUpdateC2SPacket::kMaxLineLength) : line);
            }
            return buffer.GetData();
        }

        inline SignUpdateC2SPacket DeserializeSignUpdateC2S(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            SignUpdateC2SPacket packet;
            packet.pos.x = static_cast<int32_t>(reader.ReadInt());
            packet.pos.y = static_cast<int32_t>(reader.ReadInt());
            packet.pos.z = static_cast<int32_t>(reader.ReadInt());
            packet.front = reader.ReadByte() != 0;
            for (std::string& line : packet.lines) {
                if (!reader.HasMore()) break;
                line = reader.ReadString(SignUpdateC2SPacket::kMaxLineLength);
            }
            return packet;
        }

    } // namespace Serialization

} // namespace Network

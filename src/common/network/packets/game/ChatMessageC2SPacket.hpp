// File: src/common/network/packets/game/ChatMessageC2SPacket.hpp
//
// Player-typed chat message (or `/command`). Mirrors MC's
// ServerboundChatPacket / ServerboundChatCommandPacket pair (we use one
// packet with an `isCommand` flag rather than two). Server splits on the
// flag to either broadcast the message or dispatch to the command handler.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace Network {

    struct ChatMessageC2SPacket {
        std::string message;
        uint32_t    timestamp;
        bool        isCommand = false; // True if message starts with '/'
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const ChatMessageC2SPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteString(packet.message);
            buffer.WriteByte(packet.isCommand ? 0x01 : 0x00);
            buffer.WriteInt(packet.timestamp);
            return buffer.GetData();
        }

        inline ChatMessageC2SPacket DeserializeChatMessageC2S(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            ChatMessageC2SPacket packet;
            packet.message = reader.ReadString();
            packet.isCommand = reader.ReadByte() != 0;
            packet.timestamp = reader.ReadInt();
            return packet;
        }

    } // namespace Serialization

} // namespace Network

// File: src/common/network/packets/game/ChatMessageS2CPacket.hpp
//
// Server → client chat, carrying styled segments rather than a bare string.
// This is our (much smaller) stand-in for MC's Component tree: MC serialises a
// full nested Component with a Style holding colour, ClickEvent and HoverEvent;
// we send a flat list of runs, which covers everything the game actually emits
// and keeps the wire format trivial to read.
//
// Both ends of this packet used to be hand-serialised inline in
// ServerConnection.cpp / ClientConnection.cpp with no shared struct. Having the
// definition in one place is what stops the two drifting apart.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace Network {

    // Mirrors MC ClickEvent's action set, trimmed to what we emit.
    enum class ChatClickAction : uint8_t {
        None = 0,
        CopyToClipboard = 1,
    };

    struct ChatSegmentData {
        std::string text;
        uint32_t    color = 0xFFFFFFFF;   // ARGB
        ChatClickAction click = ChatClickAction::None;
        std::string clickValue;
        std::string hoverText;
    };

    struct ChatMessageS2CPacket {
        uint32_t senderId = 0;   // 0 = system message
        uint8_t  position = 0;   // 0 = chat, 1 = system, 2 = action bar
        std::vector<ChatSegmentData> segments;

        ChatMessageS2CPacket() = default;

        // Convenience for the overwhelmingly common plain-text case.
        ChatMessageS2CPacket(const std::string& text, uint8_t pos = 0, uint32_t sender = 0)
            : senderId(sender), position(pos) {
            segments.push_back(ChatSegmentData{text, 0xFFFFFFFF, ChatClickAction::None, "", ""});
        }

        // True when this is a single unstyled run — lets the sender fall back to
        // the legacy on-wire shape so older clients still read it.
        bool IsPlainText() const {
            return segments.size() == 1 &&
                   segments[0].click == ChatClickAction::None &&
                   segments[0].color == 0xFFFFFFFF;
        }
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const ChatMessageS2CPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteInt(static_cast<int32_t>(packet.senderId));

            // The legacy layout was: int senderId, string message, byte position.
            // Keep exactly that when nothing is styled, then append the segment
            // block. A reader that stops after `position` still gets the right
            // text, and a reader that knows about segments prefers them.
            std::string flat;
            for (const auto& s : packet.segments) flat += s.text;
            buffer.WriteString(flat);
            buffer.WriteByte(packet.position);

            buffer.WriteVarInt(static_cast<uint32_t>(packet.segments.size()));
            for (const auto& s : packet.segments) {
                buffer.WriteString(s.text);
                buffer.WriteInt(static_cast<int32_t>(s.color));
                buffer.WriteByte(static_cast<uint8_t>(s.click));
                buffer.WriteString(s.clickValue);
                buffer.WriteString(s.hoverText);
            }
            return buffer.GetData();
        }

        inline ChatMessageS2CPacket DeserializeChatMessageS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            ChatMessageS2CPacket packet;
            packet.senderId = static_cast<uint32_t>(reader.ReadInt());
            const std::string flat = reader.ReadString();
            packet.position = reader.ReadByte();

            if (reader.Remaining() >= 1) {
                const uint32_t count = reader.ReadVarInt();
                packet.segments.reserve(count);
                for (uint32_t i = 0; i < count; ++i) {
                    ChatSegmentData s;
                    s.text = reader.ReadString();
                    s.color = static_cast<uint32_t>(reader.ReadInt());
                    const uint8_t act = reader.ReadByte();
                    s.click = act == 1 ? ChatClickAction::CopyToClipboard : ChatClickAction::None;
                    s.clickValue = reader.ReadString();
                    s.hoverText = reader.ReadString();
                    packet.segments.push_back(std::move(s));
                }
            }

            // Pre-segment sender: reconstruct one plain run from the flat text.
            if (packet.segments.empty()) {
                packet.segments.push_back(
                    ChatSegmentData{flat, 0xFFFFFFFF, ChatClickAction::None, "", ""});
            }
            return packet;
        }

    } // namespace Serialization

} // namespace Network

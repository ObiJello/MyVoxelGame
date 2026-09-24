// File: src/common/network/packets/game/BookPackets.hpp
//
// The book and lectern packets:
//
//   OpenBookS2C            MC ClientboundOpenBookPacket(hand) — the server
//                          resolved the written book in `hand` and asks the
//                          client to show it (ServerPlayer.openItemGui).
//                          Wire: byte hand (0 main, 1 off).
//
//   EditBookC2S            MC ServerboundEditBookPacket(slot, pages, title) —
//                          the book-and-quill editor closed ("Done": no
//                          title) or signed ("Sign and Close": a title).
//                          Wire: VarInt slot; VarInt count (≤ 100) then that
//                          many strings (≤ 1024 characters each); byte
//                          hasTitle then the title (≤ 32 characters).
//
//   ContainerButtonClickC2S MC ServerboundContainerButtonClickPacket
//                          (containerId, buttonId) — a menu button that is
//                          not a slot: the lectern's page turns, page jumps
//                          and Take Book. Wire: VarInt containerId, VarInt
//                          buttonId.
#pragma once

#include "common/network/PacketRegistry.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace Network {

    struct OpenBookS2CPacket {
        uint8_t hand = 0;   // 0 main hand, 1 off hand
    };

    struct EditBookC2SPacket {
        static constexpr size_t kMaxPages      = 100;    // WritableBookContent.MAX_PAGES
        static constexpr size_t kMaxPageChars  = 1024;   // WritableBookContent.PAGE_EDIT_LENGTH
        static constexpr size_t kMaxTitleChars = 32;     // WrittenBookContent.TITLE_MAX_LENGTH
        // Inventory index MC-style: 0..8 hotbar, 40 the offhand.
        int32_t                    slot = 0;
        std::vector<std::string>   pages;
        std::optional<std::string> title;
    };

    struct ContainerButtonClickC2SPacket {
        uint32_t containerId = 0;
        uint32_t buttonId    = 0;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const OpenBookS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteByte(packet.hand);
            return buffer.GetData();
        }

        inline OpenBookS2CPacket DeserializeOpenBookS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            OpenBookS2CPacket packet;
            packet.hand = reader.ReadByte() != 0 ? 1 : 0;
            return packet;
        }

        inline std::vector<uint8_t> Serialize(const EditBookC2SPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteVarInt(static_cast<uint32_t>(packet.slot));
            const size_t count = packet.pages.size() < EditBookC2SPacket::kMaxPages
                                     ? packet.pages.size() : EditBookC2SPacket::kMaxPages;
            buffer.WriteVarInt(static_cast<uint32_t>(count));
            for (size_t i = 0; i < count; ++i) buffer.WriteString(packet.pages[i]);
            buffer.WriteByte(packet.title ? 1 : 0);
            if (packet.title) buffer.WriteString(*packet.title);
            return buffer.GetData();
        }

        inline EditBookC2SPacket DeserializeEditBookC2S(const std::vector<uint8_t>& data) {
            // ByteBufCodecs.stringUtf8(n) bounds characters; four bytes is
            // the widest a UTF-8 character can be. The server re-checks the
            // character counts when it applies the edit.
            PacketReader reader(data);
            EditBookC2SPacket packet;
            packet.slot = static_cast<int32_t>(reader.ReadVarInt());
            const uint32_t count = reader.ReadVarInt();
            if (count > EditBookC2SPacket::kMaxPages) {
                throw std::runtime_error("EditBookC2S: " + std::to_string(count) + " pages exceeds 100");
            }
            packet.pages.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                packet.pages.push_back(reader.ReadString(EditBookC2SPacket::kMaxPageChars * 4));
            }
            if (reader.ReadByte() != 0) {
                packet.title = reader.ReadString(EditBookC2SPacket::kMaxTitleChars * 4);
            }
            return packet;
        }

        inline std::vector<uint8_t> Serialize(const ContainerButtonClickC2SPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteVarInt(packet.containerId);
            buffer.WriteVarInt(packet.buttonId);
            return buffer.GetData();
        }

        inline ContainerButtonClickC2SPacket DeserializeContainerButtonClickC2S(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            ContainerButtonClickC2SPacket packet;
            packet.containerId = reader.ReadVarInt();
            packet.buttonId    = reader.ReadVarInt();
            return packet;
        }

    } // namespace Serialization

} // namespace Network

// File: src/common/network/packets/game/OpenScreenS2CPacket.hpp
//
// Server → client: "I have opened menu <containerId> of type <menuType> for
// you; put the matching screen up." Mirrors MC ClientboundOpenScreenPacket.
//
// The client answers by BUILDING the same menu class over its own inventory,
// so both sides then agree on the slot list that every subsequent click,
// snapshot and delta is indexed by. A full container snapshot follows
// immediately behind this packet and fills it in.
//
// MC's title is a Component; ours is a plain string the screen draws verbatim.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/inventory/MenuType.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace Network {

    struct OpenScreenS2CPacket {
        uint32_t       containerId = 0;
        Game::MenuType menuType    = Game::MenuType::Crafting;
        std::string    title;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const OpenScreenS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteVarInt(packet.containerId);
            buffer.WriteByte(static_cast<uint8_t>(packet.menuType));
            buffer.WriteString(packet.title);
            return buffer.GetData();
        }

        inline OpenScreenS2CPacket DeserializeOpenScreenS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            OpenScreenS2CPacket packet;
            packet.containerId = reader.ReadVarInt();
            packet.menuType    = static_cast<Game::MenuType>(reader.ReadByte());
            packet.title       = reader.ReadString();
            return packet;
        }

    } // namespace Serialization

} // namespace Network

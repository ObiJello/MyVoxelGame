#pragma once
// RenameItemC2S (0xBD) — MC ServerboundRenameItemPacket.
//
// The anvil screen's name box, sent each time the typed name changes (the
// client's AnvilMenu.setItemName accepted it). The server runs the same
// setItemName on its copy of the menu, which recomputes the result and the
// level cost; the result slot and cost data slot then sync back as usual.
//
//   String name (MC writeUtf, max 32767) — validated again server side
//   (filtered text, at most AnvilMenu::MAX_NAME_LENGTH characters).
#include "common/network/PacketRegistry.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace Network {

    struct RenameItemC2SPacket {
        std::string name;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const RenameItemC2SPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteString(packet.name);
            return buffer.GetData();
        }

        inline RenameItemC2SPacket DeserializeRenameItemC2S(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            RenameItemC2SPacket packet;
            packet.name = reader.ReadString(32767);
            return packet;
        }

    } // namespace Serialization

} // namespace Network

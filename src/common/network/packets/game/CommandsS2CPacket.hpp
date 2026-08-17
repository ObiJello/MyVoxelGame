// File: src/common/network/packets/game/CommandsS2CPacket.hpp
//
// Server → client: the list of commands this server accepts. Sent once per
// player on join, and it is what drives chat's tab-completion.
//
// MC's equivalent is ClientboundCommandsPacket, which ships the whole Brigadier
// tree — every node, its argument type, and its suggestion provider — so the
// client can complete arguments as well as names. This carries only the
// top-level NAMES, because that is all this engine's dispatcher has: it is a
// flat name -> handler map with no argument nodes to describe (see
// CommandDispatcher.hpp). Per-argument suggestions stay client-side.
//
// The point is that the client stops guessing. The command list used to be
// hardcoded in ChatScreen.cpp and drifted from the server's registrations every
// time a command was added — /tick was registered and simply never appeared in
// the popup. Now adding a command to IntegratedServer is the only step.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace Network {

    struct CommandsS2CPacket {
        std::vector<std::string> commandNames;   // bare names, no leading '/'

        CommandsS2CPacket() = default;
        explicit CommandsS2CPacket(std::vector<std::string> names)
            : commandNames(std::move(names)) {}
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const CommandsS2CPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteVarInt(static_cast<uint32_t>(packet.commandNames.size()));
            for (const auto& name : packet.commandNames) {
                buffer.WriteString(name);
            }
            return buffer.GetData();
        }

        inline CommandsS2CPacket DeserializeCommandsS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            CommandsS2CPacket packet;
            const uint32_t count = reader.ReadVarInt();
            // Bound the count against what the payload could possibly hold —
            // a corrupt or hostile length must not make us reserve gigabytes.
            // Every name costs at least one byte on the wire.
            const uint32_t sane = std::min<uint32_t>(count,
                                                     static_cast<uint32_t>(data.size()));
            packet.commandNames.reserve(sane);
            for (uint32_t i = 0; i < sane; ++i) {
                packet.commandNames.push_back(reader.ReadString());
            }
            return packet;
        }

    } // namespace Serialization

} // namespace Network

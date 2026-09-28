// File: src/common/network/packets/game/CommandsS2CPacket.hpp
//
// Server → client: the list of commands this server accepts. Sent once per
// player on join, and it is what drives chat's tab-completion.
//
// MC's equivalent is ClientboundCommandsPacket, which ships the whole Brigadier
// tree so the client can complete arguments as well as names. So does this:
// after the names comes a trailing syntax block — for each name, in order, a
// byte (1 = has a tree) and the tree (Game::Cmd::WriteNode,
// common/command/CommandSyntax.hpp). The client's usage hint and argument
// completion come from those trees alone (client/renderer/gui/
// CommandSuggestions), so a command's grammar is declared once, where the
// command is registered.
//
// The point is that the client stops guessing. The command list used to be
// hardcoded in ChatScreen.cpp and drifted from the server's registrations every
// time a command was added — /tick was registered and simply never appeared in
// the popup. Now registering a command (with its tree) is the only step.
#pragma once

#include "common/command/CommandSyntax.hpp"
#include "common/network/PacketRegistry.hpp"
#include <algorithm>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

namespace Network {

    struct CommandsS2CPacket {
        std::vector<std::string> commandNames;   // bare names, no leading '/'
        // Trailing: one entry per name (same order) when present; an entry
        // with hasSyntax false is a command registered without a tree.
        struct Syntax {
            bool            hasSyntax = false;
            Game::Cmd::Node root;
        };
        std::vector<Syntax> syntax;
        // Trailing, after the syntax block: every custom entity name the
        // server knows (loaded or in its named-entity index) — what a
        // selector's `name=` completes.
        std::vector<std::string> entityNames;

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
            // Trailing syntax block (see the header note): one flag per name,
            // always present so the names block after it can be found.
            const bool haveSyntax = packet.syntax.size() == packet.commandNames.size();
            for (size_t i = 0; i < packet.commandNames.size(); ++i) {
                const bool has = haveSyntax && packet.syntax[i].hasSyntax;
                buffer.WriteByte(has ? 1 : 0);
                if (has) Game::Cmd::WriteNode(buffer, packet.syntax[i].root);
            }
            // Trailing custom entity names.
            buffer.WriteVarInt(static_cast<uint32_t>(packet.entityNames.size()));
            for (const auto& name : packet.entityNames) buffer.WriteString(name);
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
            // The syntax block is optional; a malformed one drops the whole
            // block (names still complete), never the packet.
            if (reader.HasMore()) {
                std::vector<CommandsS2CPacket::Syntax> syntax(packet.commandNames.size());
                bool ok = true;
                try {
                    for (auto& entry : syntax) {
                        entry.hasSyntax = reader.ReadByte() != 0;
                        if (entry.hasSyntax && !Game::Cmd::ReadNode(reader, entry.root)) { ok = false; break; }
                    }
                } catch (const std::exception&) {
                    ok = false;
                }
                if (ok) packet.syntax = std::move(syntax);
                // The names block (only readable after an intact syntax block).
                if (ok && reader.HasMore()) {
                    try {
                        const uint32_t names = reader.ReadVarInt();
                        const uint32_t saneNames = std::min<uint32_t>(names, static_cast<uint32_t>(data.size()));
                        packet.entityNames.reserve(saneNames);
                        for (uint32_t i = 0; i < saneNames; ++i) packet.entityNames.push_back(reader.ReadString(256));
                    } catch (const std::exception&) {
                        packet.entityNames.clear();
                    }
                }
            }
            return packet;
        }

    } // namespace Serialization

} // namespace Network

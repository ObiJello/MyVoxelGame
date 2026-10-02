// File: src/server/commands/FillCommand.hpp
//
// MC FillCommand:
//
//   /fill <from> <to> <block> [destroy|hollow|keep|outline|replace [<filter>]|strict]
//   /fill <from> <to> <block> replace <filter> [destroy|hollow|outline|strict]
//
//   replace   every block (the default); with a filter, only matching blocks
//   keep      only air
//   outline   the shell only; the inside is left alone
//   hollow    the shell, and the inside emptied to air
//   destroy   every block broken first (drops, as a player would)
//   strict    placed without shape updates, drops or onPlace (MC 816), and
//             no neighbour updates afterwards
//
// The volume is capped by the max_block_modifications game rule. The block
// takes MC's whole BlockStateArgument: `chest[facing=east]{Lock:...}`.
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class FillCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server

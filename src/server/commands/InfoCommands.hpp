// File: src/server/commands/InfoCommands.hpp
//
// Vanilla's informational commands:
//
//   /list [uuids]                      MC ListPlayersCommand
//   /help [<command>]                  MC HelpCommand — the smart usage of every
//                                      command, or of what follows a typed prefix
//   /version                           MC VersionCommand — this build's numbers
//   /random (value|roll) <range> [<sequence>]
//   /random reset (*|<sequence>) [<seed> [<includeWorldSeed> [<includeSequenceId>]]]
//                                      MC RandomCommand over RandomSequences
//                                      (CommandSavedData.hpp)
//
// /list, /help, /version (integrated server) and /random value|roll without a
// sequence are permission level 0 in MC; ServerConnection's command gate lets
// every player run them. A sequence and `reset` need the command permission
// (CommandText::HasGamemasterPermission), as MC's LEVEL_GAMEMASTERS does.
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class InfoCommands {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void ExecuteList(const CommandSourceStack& source, const std::vector<std::string>& args,
                                ServerConnection& connection, PlayerSessionManager& sessionManager);
        static void ExecuteHelp(const CommandSourceStack& source, const std::vector<std::string>& args,
                                ServerConnection& connection, PlayerSessionManager& sessionManager);
        static void ExecuteVersion(const CommandSourceStack& source, const std::vector<std::string>& args,
                                   ServerConnection& connection, PlayerSessionManager& sessionManager);
        static void ExecuteRandom(const CommandSourceStack& source, const std::vector<std::string>& args,
                                  ServerConnection& connection, PlayerSessionManager& sessionManager);

        // /random's tree with the live sequence ids as the <sequence>
        // suggestions (MC suggestRandomSequence asks the server).
        static Game::Cmd::Node RandomSyntax();
    };

} // namespace Server

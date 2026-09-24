// File: src/server/commands/SetBlockCommand.hpp
//
// MC SetBlockCommand: `/setblock <x> <y> <z> <block>[state] [destroy|keep|replace]`.
//
//   destroy  break what is there first (drops, as a player would), then place
//   keep     place only into air
//   replace  place regardless (the default)
//
// The block argument is MC's BlockStateParser form: `minecraft:oak_stairs
// [facing=east,half=top]` (namespace optional). What F3+I copies to the
// clipboard is exactly this command.
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class SetBlockCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server

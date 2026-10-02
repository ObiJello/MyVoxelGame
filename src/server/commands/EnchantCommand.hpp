// File: src/server/commands/EnchantCommand.hpp
//
// MC EnchantCommand: `/enchant <targets> <enchantment> [<level>]` — the
// enchantment onto each living target's main-hand item when the item supports
// it and nothing on it is exclusive with it, up to the enchantment's max level.
#pragma once
#include "CommandDispatcher.hpp"

namespace Server {

    class EnchantCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);
        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server

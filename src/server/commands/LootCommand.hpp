// File: src/server/commands/LootCommand.hpp
//
// `/loot give <targets> loot <loot_table>` — MC LootCommand, the `give`
// target with the `loot` source: the table is rolled (LootTable.getRandomItems)
// and every resulting stack goes into each target's inventory, the rest
// dropped at their feet. The other targets (insert, spawn, replace) and
// sources (fish, kill, mine) are not modelled.
//
// It is how a book-carrying table can be read without finding its chest:
// `/loot give @s loot minecraft:chests/sunken_library`.
#pragma once
#include "CommandDispatcher.hpp"

namespace Server {

    class LootCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);
        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server

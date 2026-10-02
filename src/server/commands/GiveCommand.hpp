// File: src/server/commands/GiveCommand.hpp
//
// MC net.minecraft.server.commands.GiveCommand:
//
//   /give <targets> <item> [<count>]
//
// <item> is MC's ItemArgument (`diamond_sword[enchantments={sharpness:5}]`,
// ItemArgument.hpp); <count> at least 1 and at most 100 of the item's stacks
// (MAX_ALLOWED_ITEMSTACKS). Each stack goes into the inventory (Inventory.add);
// what does not fit drops at the player's feet with no pickup delay and the
// player as its only collector; when a stack fits entirely, the pickup pop
// plays (and MC's never-collectable "fake" item flashes where it came from).
#pragma once
#include "CommandDispatcher.hpp"

namespace Server {

    class GiveCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);
        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);

        // MC GiveCommand.MAX_ALLOWED_ITEMSTACKS.
        static constexpr int kMaxAllowedItemStacks = 100;
    };

} // namespace Server

// File: src/server/commands/WardenSpawnTrackerCommand.cpp
#include "WardenSpawnTrackerCommand.hpp"
#include "EntitySelector.hpp"
#include "../network/ServerConnection.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "../player/ServerPlayer.hpp"
#include "common/entity/WardenSpawnTracker.hpp"

#include <cstdlib>
#include <string>

namespace Server {

    void WardenSpawnTrackerCommand::Register(CommandDispatcher& dispatcher) {
        namespace Cmd = Game::Cmd;
        dispatcher.RegisterCommand("warden_spawn_tracker", WardenSpawnTrackerCommand::Execute,
            Cmd::Root()
                .Then(Cmd::Literal("clear").Executes())
                .Then(Cmd::Literal("set").Then(
                    Cmd::Argument("warning_level", Cmd::Arg::Integer).Suggests({"0", "1", "2", "3", "4"}).Executes())));
    }

    void WardenSpawnTrackerCommand::Execute(const CommandSourceStack& source,
                                            const std::vector<std::string>& args,
                                            ServerConnection& connection,
                                            PlayerSessionManager& sessionManager) {
        (void)sessionManager;
        // MC getPlayerOrException: the executing entity must be a player.
        ServerPlayer* player = source.sender;
        if (source.entity) {
            player = source.entity->kind == SelectedEntity::Kind::Player ? source.entity->player : nullptr;
        }
        if (!player) {
            connection.SendChatMessage("A player is required to run this command here", 1);
            return;
        }
        const std::string name = player->getName();

        if (!args.empty() && args[0] == "clear" && args.size() == 1) {
            // MC resetTracker: WardenSpawnTracker.reset.
            player->getWardenSpawnTracker().Reset();
            connection.SendChatMessage("Cleared warden spawn tracker for " + name, 1);
            return;
        }
        if (args.size() == 2 && args[0] == "set") {
            char* end = nullptr;
            const long level = std::strtol(args[1].c_str(), &end, 10);
            if (!end || *end != '\0' || args[1].empty()) {
                connection.SendChatMessage("Invalid integer '" + args[1] + "'", 1);
                return;
            }
            // IntegerArgumentType.integer(0, 4).
            if (level < 0 || level > Game::WardenSpawnTracker::kMaxWarningLevel) {
                connection.SendChatMessage("Integer must be between 0 and 4, found " + args[1], 1);
                return;
            }
            // MC setWarningLevel: WardenSpawnTracker.setWarningLevel.
            player->getWardenSpawnTracker().SetWarningLevel(static_cast<int>(level));
            connection.SendChatMessage("Set warden spawn tracker warning level to " + std::to_string(level) +
                                       " for " + name, 1);
            return;
        }
        connection.SendChatMessage("Usage: /warden_spawn_tracker clear | set <warning_level>", 1);
    }

} // namespace Server

// File: src/server/commands/SpawnPointCommand.hpp
//
// MC SetSpawnCommand:
//
//   /spawnpoint [<targets> [<pos> [<rotation>]]]
//
// Sets each target player's respawn point to the block (the source's own
// block by default) in the source's dimension, FORCED: death sends them
// there whether or not a bed or anchor stands on it, as long as the two
// cells are free to stand in (ServerPlayer.findRespawnAndUseSpawnBlock's
// forced branch, PlayerSessionManager::OnPlayerRespawn).
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class SpawnPointCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server

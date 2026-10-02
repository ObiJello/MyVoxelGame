// File: src/server/commands/DataCommand.hpp
//
// MC net.minecraft.server.commands.data.DataCommands with its three
// accessors (EntityDataAccessor, BlockDataAccessor, StorageDataAccessor):
//
//   /data get    (block <pos> | entity <target> | storage <id>) [<path> [<scale>]]
//   /data merge  <target> <nbt>
//   /data remove <target> <path>
//   /data modify <target> <targetPath> (append | insert <index> | merge | prepend | set)
//         (from <source> [<sourcePath>] | string <source> [<sourcePath>] [<start>] [<end>]
//          | value <value>
//          | compute (default | block <pos> | entity <target>) (float | integer) <provider>)
//
// An entity's data is its saved compound (EntitySelector SavedEntityNbt — the
// world save's own writers); writing it back loads it through the same
// readers the save uses (EntityNbt ApplyMobNbt / ReadItem / ReadOrb), keeping
// the entity's UUID; a changed Pos moves it. Players are read-only, as in MC
// ("Unable to modify player data"). A block's data is its block entity's
// saved compound (BlockEntityNbt), written back by rebuilding the block
// entity from it (x, y, z and id kept). Storage is CommandStorage.hpp.
// Paths are NbtPath.hpp. Output is MC's: NbtUtils.toPrettyComponent's
// colours, en_us messages. `compute` evaluates MC's context number providers
// (ContextNumberProviders.hpp).
#pragma once
#include "CommandDispatcher.hpp"

namespace Server {

    class DataCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);
        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server

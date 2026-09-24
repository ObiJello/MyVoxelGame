// File: src/server/commands/StillnessCommand.hpp
//
// /stillness — debug control of the Hush's stillness (HushStillness.hpp).
// Not a vanilla command; an op command in spirit, gated like the other
// debug commands here (/tick, /time): this engine has no permission levels
// yet (ServerPlayer's PermissionLevel is a TODO).
//
// Usage:
//   /stillness                 begin one now, natural length (8-15 s)
//   /stillness <seconds>       begin one now for that long (1-60 s)
//   /stillness stop            lift the current one
//   /stillness query           is one on, and when is the next
//
// Always acts on the Hush level, wherever the sender stands (the Hush must
// have been visited this session — its level is created on first entry).
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class StillnessCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server

// File: src/server/commands/SpawnAllCommand.hpp
//
// /spawnall [adults|babies|both] [spacing] — a DEBUG line-up of every mob.
//
// One adult of every implemented mob type in a 12-wide grid centred on the
// sender: columns 2*spacing apart (default spacing 3), each row's babies
// 1.5*spacing in front of it (a type with no baby form leaves its slot empty,
// so the pairs stay index-aligned), the next pair 2*spacing beyond. All face
// north, are NoAI and persistent, and sit within ~50 blocks so one /tick step
// shows every one of them without anything turning. The log lists each type's
// row/column. Meant to be run under /tick freeze to eyeball models — the baby
// remodel in particular — not a vanilla command.
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class SpawnAllCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);
        static void Execute(ServerPlayer& sender,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server

// File: src/server/commands/AdvancementCommand.hpp
//
// MC AdvancementCommands:
//
//   /advancement (grant|revoke) <targets> everything
//   /advancement (grant|revoke) <targets> only <advancement> [<criterion>]
//   /advancement (grant|revoke) <targets> (from|through|until) <advancement>
//
// `only` is the advancement alone (or one of its criteria), `from` it and
// every descendant, `until` every ancestor and it, `through` both. `everything`
// grants without toasts (the client is told showAdvancements = false, MC's
// flushDirty bracketing). Feedback and failures are MC's
// commands.advancement.* lines.
//
// Commands work whatever the advancements_with_cheats rule says: the rule
// stops gameplay from awarding progress, not an operator.
#pragma once
#include "CommandDispatcher.hpp"

namespace Server {

    class AdvancementCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);
        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server

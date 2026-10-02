// File: src/server/commands/AttributeCommand.hpp
//
// MC AttributeCommand — `/attribute`, over the entity's attribute map:
//
//   /attribute <target> <attribute> get [<scale>]
//   /attribute <target> <attribute> base get [<scale>]
//   /attribute <target> <attribute> base set <value>
//   /attribute <target> <attribute> base reset
//   /attribute <target> <attribute> modifier add <id> <value>
//                                   (add_value|add_multiplied_base|add_multiplied_total)
//   /attribute <target> <attribute> modifier remove <id>
//   /attribute <target> <attribute> modifier value get <id> [<scale>]
//
// A player's base values and modifiers live on ServerPlayer::attributes()
// (saved with the player, synced to its client, mirrored onto its entity
// view); a mob's on its LivingEntity map (saved with the mob, synced to its
// watchers once changed). Reads see the whole instance MC would hold —
// a player's worn items, effects and creative reach included. `minecraft:
// step_height base set 1` is how vanilla gives a player full-block step-up.
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class AttributeCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);
        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server

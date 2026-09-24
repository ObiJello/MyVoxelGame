// File: src/server/commands/EffectCommand.hpp
//
// MC net.minecraft.server.commands.EffectCommands:
//
//   /effect give <targets> <effect> [<seconds>|infinite] [<amplifier>] [<hideParticles>]
//   /effect clear [<targets>] [<effect>]
//
// Same argument ranges (seconds 1..1000000, amplifier 0..255), the same
// duration rule (computeDurationInTicks: 30 s default, seconds for an
// instantaneous effect, -1 for `infinite`), MC's update rules through
// LivingEntity::AddEffect, and the en_us feedback from CommandResponseTracker
// (single vs. "<n> targets", counted over the targets the command actually
// changed; the failure message when it changed none). Targets take the
// EntityArgument.entities() selectors /kill and /heal use; a player target is
// reached through its PlayerEntityView, whose effect list is the player's.
#pragma once
#include "CommandDispatcher.hpp"

namespace Server {

    class EffectCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);
        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server

// File: src/server/commands/ParticleCommand.hpp
//
// MC net.minecraft.server.commands.ParticleCommand:
//
//   /particle <name> [<pos>] [<delta> <speed> <count> [force|normal] [<viewers>]]
//
// <name> is MC's ParticleArgument: a registry id with the type's options as
// SNBT — `dust{color:[1.0,0.0,0.0],scale:1.0}`, `block{block_state:
// "minecraft:stone"}` (or `{Name:"…",Properties:{…}}`), `item{item:
// "minecraft:diamond"}`, `vibration{destination:{type:"block",pos:[x,y,z]},
// arrival_in_ticks:20}`, `trail{target:[x,y,z],color:…,duration:…}`,
// `shriek{delay:0}`, `sculk_charge{roll:0.0}`, `entity_effect{color:…}`,
// `effect{color:…,power:…}`, `dragon_breath{power:…}`, `geyser{water_blocks:…}`.
// Colours take MC's RGB_COLOR_CODEC forms: an int or a float list.
//
// Sent per viewer exactly as ServerLevel.sendParticles(player, …): the
// viewer's level, within 32 blocks of their block centre (512 with `force`).
// Feedback is MC's en_us: "Displaying particle <id>", or "The particle was
// not visible for anybody".
#pragma once
#include "CommandDispatcher.hpp"

namespace Server {

    class ParticleCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);
        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server

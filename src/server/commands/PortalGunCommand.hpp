// File: src/server/commands/PortalGunCommand.hpp
// /portalgun — operator control over portal-gun pairs.
//
//   /portalgun list                  every open pair: gun id, owner, ends, where its gun is
//   /portalgun close <player>        close the pairs that player fired or whose gun they carry
//   /portalgun close all             close every pair
//   /portalgun close gun <id>        close one gun's pair
//   /portalgun sweep                 re-run the orphan sweep (close pairs whose gun exists nowhere)
//   /portalgun move <owner>|gun <id>|nearest <blue|orange|both>
//                   <left|right|up|down|forward|back> <blocks> [force]
//   /portalgun move <owner>|gun <id>|nearest <blue|orange|both> offset <dx> <dy> <dz> [force]
//                                    move open portals by whole blocks. Directions are as seen
//                                    standing in front of the portal looking at it (each portal
//                                    of `both` along its own axes); forward = into the surface.
//                                    Placement rules are checked unless `force`; see
//                                    PortalRegistry::MovePortals. `nearest` = the pair with a
//                                    portal closest to you within 32 blocks.
//
// Closing is PortalRegistry::ClearPair — the same fizzle burst, sound and
// PortalRemoveS2C every client gets when a gun clears its own pair.
#pragma once

#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include "CommandDispatcher.hpp"

namespace Server {

    class PortalGunCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server

#endif // ENABLE_PORTAL_GUN

// File: src/server/level/MobLevelTransfer.hpp
//
// Moving a mob, and every mob riding it, from one level to another — the
// ownership half of MC Entity.teleportCrossDimension. MC destroys the entity
// and recreates it in the new level (restoreFrom); here the same object moves
// between the two levels' MobManagers, so its riding links survive intact:
//
//   • the old level's tracker tells its watchers the entity is gone;
//   • it (and its mob passengers, depth first) leave the old MobManager and
//     join the new one, rebound to the new level's bridge;
//   • nothing of the old level is remembered across — the mob's goals, brain
//     and target keep raw pointers to entities of the level it left (other
//     mobs, the players' views), and the old level's mobs forget it.
//
// PLAYER passengers are not moved here: a player is no mob, and its crossing
// is its session's (PortalTravel's MovePlayer), with the ride re-made on
// arrival (Server::PlayerRiding). Callers unlink them first.
#pragma once

namespace Game { class Mob; }

namespace Server {

    class ServerLevel;

    namespace MobLevelTransfer {

        // Move `root` and its mob passengers from `from` to `to`. False (and
        // nothing moved) when the levels are the same or either lacks a mob
        // system; a passenger that cannot be moved is dismounted and left.
        bool TransferTree(ServerLevel& from, ServerLevel& to, Game::Mob& root);

    } // namespace MobLevelTransfer

} // namespace Server

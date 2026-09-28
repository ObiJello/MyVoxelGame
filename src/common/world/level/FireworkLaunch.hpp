// File: src/common/world/level/FireworkLaunch.hpp
//
// The server halves of the firework rocket and the crossbow, reachable from
// the shared item behaviours (common/entity/FireworkItems.cpp). Same bridge
// shape as HushItems.hpp / WorldMobSpawn.hpp: common declares, the server
// (server/items/FireworkItems.cpp) defines, and each answers "nothing
// happened" when there is no server behind it. Everything here needs the
// player's entity in its level (the rocket's owner, the crossbow's shooter)
// or the player's whole inventory, which only the server has.
#pragma once

#include "common/world/block/BlockInteraction.hpp"

#include <glm/glm.hpp>

#include <cstdint>

namespace Game {

    class ILevelWrite;
    class IUsePlayer;
    struct ItemStack;

    namespace FireworkLaunch {

        // FireworkRocketItem.useOn's spawn: a rocket at `at` (the click plus
        // the face offset), owned by `player` (may be null — a use with no
        // player), in `level`.
        void LaunchFromBlock(ILevelWrite* level, IUsePlayer* player, const ItemStack& stack,
                             const glm::dvec3& at);

        // FireworkRocketItem.use's spawn for a gliding player: their leads
        // drop (the lead-break sound when any did), and a rocket attached to
        // them goes up from their hand. True when it spawned.
        bool BoostPlayer(IUsePlayer& player, uint32_t hand, const ItemStack& stack);

        // CrossbowItem.use without a charge: start drawing when the player
        // has something to load (Player.getProjectile), else FAIL.
        UseResult CrossbowBeginDraw(IUsePlayer& player, uint32_t hand);
        // CrossbowItem.onUseTick: the loading sounds at 20 % / 50 % and, at
        // full charge, tryLoadProjectiles + the end sound.
        void CrossbowUseTick(IUsePlayer& player, ItemStack& crossbow, int remainingTicks);
        // CrossbowItem.performShooting: every loaded projectile fired (a
        // Multishot fan), the crossbow worn per shot, CHARGED_PROJECTILES
        // cleared.
        void CrossbowShoot(IUsePlayer& player, uint32_t hand, ItemStack& crossbow);

    } // namespace FireworkLaunch

} // namespace Game

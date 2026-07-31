// File: src/common/world/block/MiningSpeed.hpp
//
// MC-faithful per-tick mining progress math. Mirrors the chain:
//   BlockBehaviour.getDestroyProgress(state, player, level, pos)
//     -> Player.getDestroySpeed(state)
//     -> Item.getDestroySpeed(stack, state)
//
// Used by the client to advance the dig-progress state machine each tick, and
// (in future) by the server for sanity-validating client-reported finishes.
#pragma once

#include "../../entity/Item.hpp"
#include "BlockRegistry.hpp"

namespace Game {

    // Tool-speed lookup: returns the held item's miningSpeed against the
    // target block. Matches MC `Item.getDestroySpeed(stack, state)`:
    //   • Item has TOOL component AND tool's type matches the block's
    //     preferredTool → return tool.miningSpeed.
    //   • Otherwise (wrong tool or no tool) → 1.0.
    // Air / empty hand → 1.0 (bare-hand baseline).
    float GetItemDestroySpeed(ItemID held, const Block& target);

    // MC `Player.hasCorrectToolForDrops(state)`:
    //   • Block doesn't require a correct tool → always true.
    //   • Block requires a correct tool → held item must be the right
    //     ToolType AND meet the block's minTier.
    bool HasCorrectToolForDrops(ItemID held, const Block& target);

    // MC `Player.getDestroySpeed(state)` (the subset we model — no enchants,
    // no potion effects, no submerged check yet):
    //   speed = item.getDestroySpeed(state)
    //   if (!onGround) speed /= 5.0
    float GetPlayerDestroySpeed(ItemID held, const Block& target, bool onGround);

    // MC `BlockBehaviour.getDestroyProgress(state, player, level, pos)`:
    //   if (destroyTime < 0) return 0       (unbreakable)
    //   modifier = hasCorrectToolForDrops ? 30 : 100
    //   return playerDestroySpeed / destroyTime / modifier
    // Returned value is the per-tick progress increment; sum >= 1.0 → broken.
    float GetDestroyProgressPerTick(ItemID held, const Block& target, bool onGround);

    // MC `MultiPlayerGameMode.getDestroyStage()`:
    //   progress > 0 ? (int)(progress * 10) : -1, clamped to [0,9].
    int GetDestroyStage(float progress);

} // namespace Game

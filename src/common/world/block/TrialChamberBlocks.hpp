// File: src/common/world/block/TrialChamberBlocks.hpp
//
// The block halves of MC TrialSpawnerBlock and VaultBlock. Both blocks are
// driven by their block entities (TrialSpawnerBlockEntity,
// VaultBlockEntity); the block adds only the vault's key slot —
// VaultBlock.useItemOn: a non-empty stack used on an ACTIVE vault is offered
// to it (tryInsertKey, server side) and the click is consumed
// (SUCCESS_SERVER), anything else falls through to the empty hand.
// Placement (the vault faces away from the player) lives with the other
// horizontal-facing blocks in BlockPlacement.
#pragma once

#include "BlockRegistry.hpp"

#include <array>

namespace Game {

    // Wires the vault's useItemOn. Called from RegisterBlockBehaviors.
    void RegisterTrialChamberBlockBehaviors(std::array<Block, BlockRegistry::Size>& blocks);

} // namespace Game

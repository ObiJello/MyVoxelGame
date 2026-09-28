// File: src/common/world/block/MenuProviders.hpp
//
// MC BlockState.getMenuProvider(level, pos) != null, answered by block — for
// the one caller that needs the menu WITHOUT the rest of a right-click: a
// spectator. ServerPlayerGameMode.useItemOn's SPECTATOR branch opens the
// block's menu provider and does nothing else (no door toggles, no lever
// flips), so it cannot go through the block's useWithoutItem, which does
// both. The menu itself is still built by PlayerSession::FlushPendingMenuOpen
// — this only says WHICH menu the block provides.
//
// The vanilla providers: chests, barrels and every shulker box (a double
// chest is resolved to 9x6 when the menu opens), dispensers and droppers,
// hoppers, the furnace family, the crafter, and the menu-only blocks
// (crafting table, stonecutter, grindstone, cartography table, loom,
// smithing table, the three anvils, enchanting table, brewing stand,
// beacon), and a lectern holding a book. The ender chest is NOT one — its
// menu is the player's own ender inventory, opened only by its use.
#pragma once

#include "common/inventory/MenuType.hpp"
#include "common/world/block/BlockState.hpp"

#include <optional>

namespace Game {

    // The menu `state`'s block provides, or nullopt for a block with none.
    std::optional<MenuType> BlockMenuProviderType(BlockState state);

    // MC BlockBehaviour.showAsInteractableInSpectatorMode:
    // `state.getMenuProvider(level, pos) != null || block instanceof Portal`.
    // The spectator's crosshair and block outline show only on these.
    bool ShowAsInteractableInSpectatorMode(BlockState state);

} // namespace Game

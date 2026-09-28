// File: src/client/renderer/gui/CreativeHotbars.hpp
//
// Saved creative hotbars — mirrors net.minecraft.client.HotbarManager /
// player.inventory.Hotbar and CreativeModeInventoryScreen.
// handleHotbarLoadOrSave. Nine hotbars of nine stacks, kept in
// `hotbar.nbt` in the game directory (uncompressed NBT, vanilla's layout:
// DataVersion plus lists "0".."8" of item compounds, {} for an empty slot).
// Save hotbar N: hold the Save Hotbar Activator (C) and press hotbar key N;
// load: the Load Hotbar Activator (X). The Saved Hotbars creative tab shows
// them.
#pragma once

#include "common/entity/Item.hpp"
#include "common/network/packets/game/InventoryClickC2SPacket.hpp"

#include <array>
#include <string>
#include <vector>

namespace Game { class ClientPlayer; }

namespace Render::CreativeHotbars {

    inline constexpr int kHotbarCount = 9;   // HotbarManager.NUM_HOTBAR_GROUPS
    inline constexpr int kSlots       = 9;   // Inventory.getSelectionSize()

    using Hotbar = std::array<Game::ItemStack, kSlots>;

    // MC HotbarManager.get(index): read from disk on first use.
    const Hotbar& Get(int index);
    // MC Hotbar.isEmpty: every slot empty.
    bool IsEmpty(int index);

    // MC handleHotbarLoadOrSave(save): copies the player's hotbar into saved
    // hotbar `index`, writes the file, and returns the action-bar message
    // ("Item hotbar saved (restore with X+1)").
    std::string SaveFrom(int index, const Game::ClientPlayer& player);

    // MC handleHotbarLoadOrSave(load): the nine creative slot writes that put
    // saved hotbar `index` into the player's hotbar (inventory menu slots
    // 36..44), each already predicted on the local inventory menu — the
    // caller sends them as InventoryClickC2S.
    std::vector<Network::InventoryClickC2SPacket> BuildLoadClicks(int index, const Game::ClientPlayer& player);

} // namespace Render::CreativeHotbars

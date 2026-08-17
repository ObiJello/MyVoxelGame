// File: src/common/inventory/MenuType.hpp
//
// Mirrors net.minecraft.world.inventory.MenuType — the identity a server sends
// so a client knows which menu (and therefore which screen) to build. Travels
// in OpenScreenS2CPacket and in the full container snapshot.
//
// Numeric values are wire state: append, never renumber.
#pragma once

#include <cstdint>

namespace Game {

    enum class MenuType : uint8_t {
        // The player's own 46-slot menu. Always present behind whatever else is
        // open, so it is never *opened* by a packet — it is what the client
        // falls back to when a container menu closes.
        Inventory = 0,
        // A crafting table's 3x3 grid (MC MenuType.CRAFTING).
        Crafting  = 1,

        // ── Storage ───────────────────────────────────────────────────────
        // MC splits these by ROW COUNT rather than by block (GENERIC_9x1 …
        // GENERIC_9x6), because the screen only needs to know how tall to draw
        // the container half. A chest, a barrel and a shulker box are all
        // Generic9x3; a double chest is Generic9x6. Same here.
        Generic9x1 = 2,
        Generic9x2 = 3,
        Generic9x3 = 4,
        Generic9x4 = 5,
        Generic9x5 = 6,
        Generic9x6 = 7,
        Generic3x3 = 8,   // dispenser / dropper
        Hopper     = 9,   // 5 slots in a row

        // ── Furnace family (MC MenuType.FURNACE / BLAST_FURNACE / SMOKER) ──
        Furnace      = 10,
        BlastFurnace = 11,
        Smoker       = 12,

        // ── Utility ───────────────────────────────────────────────────────
        Stonecutter      = 13,
        Grindstone       = 14,
        CartographyTable = 15,
        Loom             = 16,
        Smithing         = 17,
        Anvil            = 18,

        // ── Systems with their own gameplay behind them ───────────────────
        Enchantment  = 19,
        BrewingStand = 20,
        Beacon       = 21,
        Crafter3x3   = 22,
    };

} // namespace Game

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
    };

} // namespace Game

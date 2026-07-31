// File: src/common/network/packets/common/PacketCommon.hpp
//
// Shared enums, sentinels, and constants referenced by multiple packets.
// Lives separately from any one packet so each per-packet header can include
// only what it needs without circular dependencies.

#pragma once

#include <cstdint>

namespace Network {

    // ── Block-action tag (BlockActionC2SPacket) ─────────────────────────────
    // Mirrors MC's net.minecraft.network.protocol.game.ServerboundPlayerActionPacket.Action.
    // BREAK is kept as an alias for STOP_DESTROY for backwards compat with any
    // caller that hasn't migrated to the staged START/STOP/ABORT protocol.
    enum class BlockActionType : uint8_t {
        BREAK         = 0,  // legacy alias — same as STOP_DESTROY
        PLACE         = 1,
        INTERACT      = 2,
        START_DESTROY = 3,  // player started mining a block
        ABORT_DESTROY = 4,  // player released LMB / looked away mid-mine
        STOP_DESTROY  = 5,  // mining completed; server should remove the block
    };

    // ── Relative-flag bitmask (ClientboundPlayerPositionPacket) ─────────────
    // Mirrors net.minecraft.world.entity.Relative. Each bit, when set, marks
    // that field as a delta added to the entity's current value instead of
    // replaced. An empty mask (0) means a fully absolute teleport.
    namespace Relative {
        constexpr int32_t X            = 1 << 0;
        constexpr int32_t Y            = 1 << 1;
        constexpr int32_t Z            = 1 << 2;
        constexpr int32_t Y_ROT        = 1 << 3;
        constexpr int32_t X_ROT        = 1 << 4;
        constexpr int32_t DELTA_X      = 1 << 5;
        constexpr int32_t DELTA_Y      = 1 << 6;
        constexpr int32_t DELTA_Z      = 1 << 7;
        constexpr int32_t ROTATE_DELTA = 1 << 8;
    }

    // ── ContainerInput action types (InventoryClickC2SPacket) ───────────────
    // Match MC AbstractContainerMenu.ContainerInput verbatim.
    enum class ContainerInput : uint8_t {
        PICKUP      = 0, // Left/right click on a slot
        QUICK_MOVE  = 1, // Shift+click; auto-move stack between regions
        SWAP        = 2, // Number key — swap with hotbar slot `button` (0..8)
        CLONE       = 3, // Middle click — clone full stack to cursor (creative)
        THROW       = 4, // Q — drop 1; Ctrl+Q drops stack; outside-click drops cursor
        QUICK_CRAFT = 5, // Drag-distribute (three phases: start/add/end)
        PICKUP_ALL  = 6, // Double-click — collect matching items into cursor
        // MC creative-only: shift-click on the destroy_item slot (creative survival tab).
        // Mirrors CreativeModeInventoryScreen.slotClicked() line 189-193: clears every slot
        // in the player's container (crafting + armor + main + hotbar + offhand).
        CREATIVE_DESTROY_ALL = 7,
        // Server-authoritative pick-block (P key). The client picks an item
        // off the world (the block being looked at) and asks the server to
        // place a full stack into `slotIndex` — matching MC's
        // ServerboundSetCreativeModeSlotPacket. Without going through the
        // server, the client's local inventory mutation never reaches the
        // authoritative inventory, so subsequent placements / clicks against
        // the slot all fail silently.
        CREATIVE_FILL_SLOT   = 8,
    };

    // Sentinel slot indices for InventoryClickC2SPacket.
    namespace InventorySlotSentinel {
        constexpr int16_t OUTSIDE       = -999; // matches MC AbstractContainerMenu.SLOT_CLICKED_OUTSIDE
        constexpr int16_t CREATIVE_GRID = -2;   // click on the search-tab creative source grid
    }

} // namespace Network

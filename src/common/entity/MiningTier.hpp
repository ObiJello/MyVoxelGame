// File: src/common/entity/MiningTier.hpp
//
// Shared enums for tool/block mining classification. Mirrors MC's
// world/item/Tier.java and the tool-type tags (mineable/pickaxe,
// mineable/axe, etc.) used by BlockBehaviour.requiresCorrectToolForDrops.
//
// Lives in common/entity/ so both blocks (preferredTool / minTier) and
// items (Tool data component) can include it without circular deps.
#pragma once

#include <cstdint>

namespace Game {

    enum class ToolType : uint8_t {
        None = 0,
        Pickaxe,
        Axe,
        Shovel,
        Hoe,
        Sword,
        Shears,
    };

    // Tool tier — used both for "is this the correct tool to drop loot" and
    // tier-gated mining (e.g., obsidian needs diamond+).
    //
    // Gold is listed as a distinct value but maps to the SAME mining-level
    // as Wood — gold tools mine FAST (×12 speed) but only count as wood-tier
    // for correct-tool checks (MC: Tiers.GOLD has level 0, same as WOOD).
    enum class MiningTier : uint8_t {
        Wood      = 0,
        Stone     = 1,
        Iron      = 2,
        Diamond   = 3,
        Netherite = 4,
        Gold      = 5,  // distinct enum, but mining level = Wood (see TierLevel)
        // The Hush's tier (docs/the-hush.md): one step above diamond. Its
        // level sits one above netherite, so a resonite tool mines every
        // vanilla block netherite does, and a block whose minTier is Resonite
        // (the echo core, via the `needs_resonite_tool` block tag) is opened
        // by a resonite pickaxe and by nothing vanilla — which is the
        // progression gate the design leans on.
        Resonite  = 6,
    };

    // Effective mining-level for the correct-tool check.
    // Gold tools, despite being a separate tier, mine at wood-level.
    constexpr uint8_t TierLevel(MiningTier t) {
        if (t == MiningTier::Gold)     return 0u;
        if (t == MiningTier::Resonite) return 5u;
        return static_cast<uint8_t>(t);
    }

} // namespace Game

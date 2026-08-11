// File: src/client/renderer/texture/ConnectedTextures.hpp
//
// Connected glass textures: adjacent blocks of the same type merge into one
// surface, with the frame drawn only around the OUTSIDE of the group.
//
// NOT vanilla behaviour. Minecraft draws glass.png's full 1px frame on every
// block face regardless of neighbours; connected textures are an OptiFine /
// Continuity resource-pack feature. This is a deliberate departure.
//
// Method: the standard 47-tile CTM. Vanilla glass.png is a 1px opaque frame
// around a mostly transparent interior, so a variant is just the base sprite
// with the frame erased wherever it faces an identical neighbour.
//
// Erasing has to consider DIAGONALS, not just the four edges. At a concave
// corner — an L of three blocks, say — both edges of the inner block connect
// while the diagonal cell is empty, and the corner pixel is what closes the
// outline around the notch. Judging on edges alone leaves a one-pixel hole
// there. Requiring the diagonal too is what takes the tile count from 16 to
// the classic 47.
//
// The mask is in TEXTURE space, not world space, because which world direction
// is "left" depends on the face (see Mesher::ConnectedTextureMask).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace Render::CTM {

    // Neighbour bits. Set = that cell holds an identical block.
    enum : uint8_t {
        LEFT   = 1u << 0,
        RIGHT  = 1u << 1,
        TOP    = 1u << 2,
        BOTTOM = 1u << 3,
        TL     = 1u << 4,   // diagonals, named for the texture-space corner
        TR     = 1u << 5,
        BL     = 1u << 6,
        BR     = 1u << 7,
    };

    // Upper bound for a per-block table; the real count is VariantCount() = 47.
    inline constexpr int kMaxVariants = 64;

    // A diagonal only matters when BOTH edges meeting at that corner are
    // connected — otherwise the corner pixel is part of a border that is still
    // being drawn and the diagonal cannot change it. Folding away the
    // irrelevant bits collapses 256 raw masks onto 47 distinct tiles.
    inline constexpr uint8_t Canonical(uint8_t m) {
        uint8_t out = static_cast<uint8_t>(m & 0x0Fu);
        if ((m & (LEFT   | TOP))    == (LEFT   | TOP)    && (m & TL)) out |= TL;
        if ((m & (RIGHT  | TOP))    == (RIGHT  | TOP)    && (m & TR)) out |= TR;
        if ((m & (LEFT   | BOTTOM)) == (LEFT   | BOTTOM) && (m & BL)) out |= BL;
        if ((m & (RIGHT  | BOTTOM)) == (RIGHT  | BOTTOM) && (m & BR)) out |= BR;
        return out;
    }

    // Dense slot numbering over the canonical masks, ascending. Both the atlas
    // (which bakes one tile per slot) and the mesher (which looks one up per
    // face) derive it from here, so they cannot disagree.
    struct TileTable {
        std::array<uint8_t, 256>          slotOf{};   // canonical mask -> slot
        std::array<uint8_t, kMaxVariants> maskOf{};   // slot -> canonical mask
        int count = 0;
    };

    inline const TileTable& Tiles() {
        static const TileTable table = [] {
            TileTable t;
            for (int m = 0; m < 256; ++m) {
                const uint8_t mask = static_cast<uint8_t>(m);
                if (Canonical(mask) != mask) continue;      // not a representative
                t.slotOf[static_cast<size_t>(mask)] = static_cast<uint8_t>(t.count);
                t.maskOf[static_cast<size_t>(t.count)] = mask;
                ++t.count;
            }
            return t;
        }();
        return table;
    }

    inline int VariantCount() { return Tiles().count; }              // 47

    // Raw 8-neighbour mask -> tile slot.
    inline int SlotFor(uint8_t rawMask) {
        return Tiles().slotOf[static_cast<size_t>(Canonical(rawMask))];
    }

    // The neighbour pattern a given slot's tile was baked for.
    inline uint8_t MaskForSlot(int slot) {
        return Tiles().maskOf[static_cast<size_t>(slot)];
    }

    // Blocks that participate. Vanilla's whole HalfTransparentBlock glass
    // family — the same set that culls against itself — minus ice, whose
    // texture has no frame to erase and so has nothing to connect.
    //
    // Adding a block here is enough: the atlas derives its tiles and the
    // mesher picks them up, both keyed off this one list.
    inline constexpr std::string_view kBlocks[] = {
        "glass", "tinted_glass",
        "white_stained_glass",      "orange_stained_glass",
        "magenta_stained_glass",    "light_blue_stained_glass",
        "yellow_stained_glass",     "lime_stained_glass",
        "pink_stained_glass",       "gray_stained_glass",
        "light_gray_stained_glass", "cyan_stained_glass",
        "purple_stained_glass",     "blue_stained_glass",
        "brown_stained_glass",      "green_stained_glass",
        "red_stained_glass",        "black_stained_glass",
    };

    // True when `name` (a bare block/texture name, no "block/" prefix) is in
    // the list above.
    inline bool IsConnected(std::string_view name) {
        for (std::string_view b : kBlocks) {
            if (b == name) return true;
        }
        return false;
    }

    // Atlas key for one tile SLOT (not a raw mask) — e.g.
    // VariantKey("block/glass", 5) -> "block/glass__ctm5".
    inline std::string VariantKey(std::string_view baseKey, int slot) {
        return std::string(baseKey) + "__ctm" + std::to_string(slot);
    }

} // namespace Render::CTM

// File: src/common/world/level/VeinMine.hpp
//
// Vein mining (engine feature): hold Sneak and the Vein Mine key while a
// block breaks and more blocks go with it. WHICH blocks is decided here, in
// common code, because two sides must agree on it exactly: the server breaks
// them (PlayerSession::StartVeinMine) and the client highlights them before
// the dig finishes (Client::VeinMineClient).
//
// ── Shapes ──────────────────────────────────────────────────────────────
//   Vein        every touching block of the dug kind (26-neighbourhood).
//               A cluster bigger than kVeinMassBlocks is ground, not a vein
//               (stone, dirt and sand run on for ever), and is mined as the
//               Tunnel shape at the chosen size, taking only that kind.
//   Tunnel      width x height cross-section, `length` deep, into the dug
//               face (down a shaft when the floor was dug).
//   Stairs Down the cross-section stepping one forward and one down per
//   Stairs Up   slice (or up), along the player's horizontal facing.
//   Shaft Down  the cross-section straight down from the dug block.
// Every shape takes only blocks of the dug kind — a tunnel through stone
// leaves the ores, dirt and gravel in its walls where they are. The sized
// shapes also pass over unbreakable blocks and blocks with a block entity
// (ShapeTakes). Outside creative the held item must be able to harvest the
// dug block, or nothing spreads (CanHarvest).
//
// ── Size ────────────────────────────────────────────────────────────────
// The sizes are the only limit: up to kMaxSize wide and tall, kMaxLength
// long; there is no block count. Width runs along the player's right, height along the screen's up (the
// world up for a horizontal dig; the player's forward when digging into
// the floor). The dug block is always the middle; an even size puts the
// extra row on the right / top side.
//
// ── Timing ──────────────────────────────────────────────────────────────
// Every target carries a ring — its distance from the dug block (BFS depth
// for a vein, Chebyshev distance for the rest) — and they are ordered
// nearest first. They break one after another, kBlockStepMs apart, so a
// small mine is quick (10 blocks: a quarter of a second); past
// kSpreadMaxMs / kBlockStepMs blocks the steps shrink so the whole mine
// still takes kSpreadMaxMs (one second): BreakTimeMs.
#pragma once

#include "common/world/block/Blocks.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/block/Direction.hpp"

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace Game {
    struct IBlockAccess;
    struct ItemStack;
}

namespace Game::VeinMine {

    enum class Shape : uint8_t { Vein = 0, Tunnel, StairsDown, StairsUp, ShaftDown, Count };

    constexpr int kMinSize   = 1;
    constexpr int kMaxSize   = 15;   // width, height
    constexpr int kMaxLength = 64;
    // Where a vein stops being one (see the header note): the largest trees
    // and ore veins are a few hundred blocks; a connected run of the same
    // block past this is ground. Not a limit on what breaks — the tunnel it
    // falls back to is sized like any other.
    constexpr int kVeinMassBlocks = 1024;

    struct Settings {
        Shape   shape  = Shape::Vein;
        uint8_t width  = 3;
        uint8_t height = 3;
        uint8_t length = 5;

        // In range, whatever arrived (the wire, a config file).
        Settings Clamped() const;
    };

    const char* ShapeName(Shape shape);
    // The vein follows its block and has no size.
    constexpr bool IsSized(Shape shape) { return shape != Shape::Vein; }
    Shape NextShape(Shape shape);

    struct Target {
        glm::ivec3 pos{0};
        int        ring = 0;   // distance from the dug block, >= 1
    };

    // What a vein mine from `origin` takes besides the origin itself,
    // nearest ring first. `kind` is what was dug (on
    // the server the origin is already air). `face` is the dug face in
    // RaycastHit::hitFace order (0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z; any
    // other value: unknown), `facing` the player's horizontal facing.
    //
    // `harvestTool` is the held stack of a survival / adventure player: no
    // vein mine at all when it cannot harvest the dug block (Game::
    // HasCorrectToolForDrops: a bare hand takes dirt and logs, stone wants a
    // pickaxe) — and as only that kind is taken, it can harvest the rest.
    // Null in creative, which takes anything.
    std::vector<Target> CollectTargets(const IBlockAccess& level, const glm::ivec3& origin, BlockID kind,
                                       int face, Direction facing, const Settings& settings,
                                       const ItemStack* harvestTool);

    // HasCorrectToolForDrops, or true for a null (creative) tool.
    bool CanHarvest(const ItemStack* harvestTool, BlockID block);

    // Whether a sized shape takes this block (see the header note).
    bool ShapeTakes(BlockState state);

    // The spread (see the header note).
    constexpr int kBlockStepMs = 25;
    constexpr int kSpreadMaxMs = 1000;
    // How long a mine of `count` blocks takes, start to last block.
    constexpr int SpreadMs(int count) {
        return count * kBlockStepMs < kSpreadMaxMs ? count * kBlockStepMs : kSpreadMaxMs;
    }
    // When target `index` (0 = nearest) of `count` breaks, in ms after the
    // dug block.
    constexpr int BreakTimeMs(int index, int count) {
        return count <= 0 ? 0
                          : static_cast<int>(static_cast<long long>(index + 1) * SpreadMs(count) / count);
    }

} // namespace Game::VeinMine

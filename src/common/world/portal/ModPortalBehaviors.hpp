// File: src/common/world/portal/ModPortalBehaviors.hpp
//
// The block side of the two mod portals (docs/mod-ports.md):
//
//   aether_portal   — The Aether's glowstone-frame portal. A third
//                     PortalFamily (PortalFamily.hpp), so the frame walk,
//                     the forcer and the immersive generator are the shared
//                     ones; what lives here is the block's own behaviour
//                     (AetherPortalBlock: updateShape, entityInside,
//                     animateTick) and the water-bucket ignition
//                     (DimensionHooks.createPortal).
//
//   twilight_portal — The Twilight Forest's pool portal. NOT a frame family:
//                     a flood-filled pool of water sources ringed by natural
//                     blocks, lit by throwing a diamond into it. The pool
//                     search (TwilightPortalShape, TFPortalBlock
//                     .recursivelyValidatePortal) and the block's behaviour
//                     (TFPortalBlock: neighborChanged, entityInside,
//                     animateTick) live here; the trigger (the item tick)
//                     and the teleporter are server-side
//                     (server/portal/TwilightTeleporter).
//
// Side-agnostic: reads through IBlockAccess, writes through ILevelWrite.
#pragma once

#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/block/Blocks.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <vector>
#include <glm/glm.hpp>

namespace Game {

    struct IBlockAccess;
    class  ILevelWrite;
    class  JavaRandom;

    // Wires aether_portal and twilight_portal (updateShape / neighborChanged,
    // entityInside, animateTick). Called once from
    // BlockRegistry_RegisterBehaviors.
    void RegisterModPortalBehaviors(std::array<Block, BlockRegistry::Size>& blocks);

    // ── The Aether ──────────────────────────────────────────────────────────
    namespace AetherPortalIgnition {

        // DimensionHooks.createPortal: water poured at `seedPos` (the cell
        // past the clicked face) lights an EMPTY glowstone frame around it —
        // AetherPortalShape.findEmptyAetherPortalShape, X axis first, then Z.
        // With the immersive feature compiled in the server's frame-lit
        // handler decides instead (any closed glowstone loop) — always: the
        // Aether ignores /gamerule immersive_portals, which is the nether's
        // switch. Only where DimensionAllowsAetherPortal.
        //
        // On the server the portal is created; on the client nothing is
        // written and the answer is the prediction ("the server will light
        // this"), so the caller can skip its own water placement.
        bool TryLight(ILevelWrite& level, const glm::ivec3& seedPos);

    } // namespace AetherPortalIgnition

    // ── The Twilight Forest ─────────────────────────────────────────────────
    namespace TwilightPortalBlocks {

        // #twilightforest:portal/edge — farmland, dirt path and the
        // overworld substrate (#minecraft:dirt here: this data pack has no
        // substrate_overworld tag; both are consulted). The ring a pool
        // needs and TFPortalBlock.neighborChanged keeps checking.
        bool IsEdge(BlockID id);

        // #twilightforest:portal/decoration — the plant standing on every
        // ring block (flowers, leaves, saplings, crops, grass, ferns, dead
        // bush, sugar cane, mushrooms, vines, moss carpet, pink petals, the
        // TF plants…). Slugs the engine does not know are skipped.
        bool IsDecoration(BlockID id);

        // TFTeleporter.randNatureBlock: a random member of
        // #twilightforest:portal/generated_decoration, short grass when none
        // resolves.
        BlockID RandomGeneratedDecoration(JavaRandom& random);

        // MC BlockState.isFaceSturdy(level, pos, Direction.UP).
        bool IsSturdyTop(BlockState state);

        // #twilightforest:portal/fluid — a water SOURCE (TF compares the
        // full state, so a flowing cell is not part of the pool).
        bool IsPoolBlock(BlockState state);

    } // namespace TwilightPortalBlocks

    // TFPortalBlock.tryToCreatePortal's validation half: the pool of water
    // sources around a start cell, every one on a sturdy block, every
    // orthogonal non-pool neighbour an edge block with a decoration above
    // it, 4 to 64 cells.
    class TwilightPortalShape {
    public:
        // TFPortalBlock.MIN_PORTAL_SIZE / TFConfig.maxPortalSize.
        static constexpr int kMinCells = 4;
        static constexpr int kMaxCells = 64;

        // Null when `start` is not in a valid pool.
        static std::optional<TwilightPortalShape> Find(const IBlockAccess& level,
                                                       const glm::ivec3& start);

        const std::vector<glm::ivec3>& Cells() const { return m_cells; }

        // TFPortalBlock.tryToCreatePortal's write: every pool cell becomes
        // twilight_portal with UPDATE_CLIENTS only (no neighbour updates, so
        // no half-built pool reverts itself mid-write).
        void CreatePortalBlocks(ILevelWrite& level) const;

    private:
        std::vector<glm::ivec3> m_cells;
    };

} // namespace Game

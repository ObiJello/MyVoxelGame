// File: src/common/world/block/HushBlocks.hpp
//
// The two blocks of the Hush's "tools of the deep" drop (docs/the-hush.md):
//
//   hanging_whisperfruit — a fruit that hangs under lantern leaves. Cocoa's
//                    `age` 0..2 and nothing else (no facing: it hangs straight
//                    down). Grows by random tick with no light gate (the Hush
//                    is dark), takes bone meal, falls when the leaf above goes.
//                    Right-click a ripe one to pick it: 1-2 whisperfruit and
//                    it starts over at age 0 (MC SweetBerryBushBlock's harvest,
//                    minus the thorns). Its item is the pure `whisperfruit`
//                    food, which plants it (placesBlock) under a lantern leaf.
//
//   echo_heart     — the choir heart set in stone: a glowing cube that, like a
//                    one-level beacon, gives every player within 16 blocks
//                    Regeneration I and Resistance I (refreshed every 4 s),
//                    keeps hostile mobs from spawning naturally within 16
//                    blocks, and pushes the Hush's hostiles (echo wraiths and
//                    echo mimics) out of that radius. A teal mote column
//                    rises above it (client animateTick).
//
// THE HEART'S CLOCK. There is no ticking block entity for a beacon in this
// engine, so the heart drives itself with its own scheduled block tick every
// kPushTicks (booked by onPlace, and re-booked by a random tick should a
// scheduled tick ever be lost). Scheduled ticks are saved with the chunk
// (block_ticks), so a heart in a reloaded chunk resumes on its own. Each tick
// renews the heart's entry in a server-side registry that the natural
// spawner asks (EchoHeart::SuppressesHostileSpawn); an entry that stops being
// renewed — the heart was mined, or its chunk unloaded — lapses by itself.
#pragma once

#include "common/world/block/BlockRegistry.hpp"
#include "common/world/level/DimensionId.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace Game {

    // Wires the whisperfruit's growth / harvest / support and the heart's
    // clock, and marks both emissive; also marks the Hush lighthouse lamp
    // emissive and gives it its motes (its beams are a block-entity
    // renderer's: HushLighthouseRenderer). Called from BlockRegistry::Init after
    // InitBlockStates (the fruit reads its `age`) and before the random-tick
    // table is published (both take random ticks).
    void BlockRegistry_RegisterHushBlocks(std::array<Block, BlockRegistry::Size>& blocks);

    namespace EchoHeart {
        inline constexpr int kRadius     = 16;   // blocks, each axis
        inline constexpr int kPulseTicks = 80;   // effects refresh (beacon: every 80)
        inline constexpr int kPushTicks  = 10;   // the heart's own tick period
        // MC BeaconBlockEntity.applyEffects: (9 + level * 2) * 20 at level 1.
        inline constexpr int kEffectTicks = (9 + 1 * 2) * 20;

        // True while a live echo heart in `dimension` is within kRadius (each
        // axis) of `pos`. Server-side; the natural spawner's hostile gate.
        bool SuppressesHostileSpawn(DimensionId dimension, const glm::ivec3& pos, int64_t gameTime);
    }

    namespace Whisperfruit {
        inline constexpr int kMaxAge = 2;
    }

} // namespace Game

// File: src/common/world/block/AurelithBlocks.hpp
//
// Block behaviour for Aurelith, the Lantern City (docs/the-hush.md, "Aurelith";
// docs/hush-lore.md). The city's material language is mostly plain building
// blocks; this file gives the ones that glow their `emissive` flag and the
// few that shed motes:
//
//   stave_stone, the lumen panels and strip, the crystal conduit, the choir
//                     lamp — full-bright (there is no block light engine:
//                     the glow is the texture drawn undimmed at night).
//   resonant_water  — the river Vesper's water: full-bright, no particles.
//   resonance_engine — the Heart's core: full-bright, motes drawn up and in
//                     toward it. Its hanging rings are AurelithHeartRenderer's.
//   voice_beacon    — a gate tower's lens: full-bright, motes rising up the
//                     beam. The beam is VoiceBeaconRenderer's.
//
// Nothing here ticks on the server; animateTick runs client-side for blocks
// near the player (ClientAnimateTick), exactly like the lighthouse lamp.
#pragma once

#include "common/world/block/BlockRegistry.hpp"

#include <array>

namespace Game {

    // Marks Aurelith's glowing blocks emissive and installs their animateTick
    // hooks. Called from BlockRegistry::Init next to the other Hush block
    // registrations (after the registry-slug pass).
    void BlockRegistry_RegisterAurelithBlocks(std::array<Block, BlockRegistry::Size>& blocks);

} // namespace Game

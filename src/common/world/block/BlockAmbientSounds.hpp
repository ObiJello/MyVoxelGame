// File: src/common/world/block/BlockAmbientSounds.hpp
//
// The SOUND half of MC's client-side animateTick, for every block (and fluid)
// that makes an ambient noise: fire's crackle, a lit furnace, a campfire, lit
// candles, a charged respawn anchor, bubble columns, the nether portal's hum,
// firefly bushes at night, pale hanging moss, open eyeblossoms, a creaking
// heart, dried ghasts, the desert's sand and dead bushes — and flowing water /
// lava (WaterFluid / LavaFluid.animateTick). Wet potent sulfur's hiss is not
// here: it is one half of the block's own animateTick with its bubbles
// (PotentSulfurBlock.cpp).
//
// Every one is MC's `level.playLocalSound(..., false)`: heard by this client
// only, never sent. The particles those animateTicks also spawn are the
// blocks' own business (where this engine has the particle); these hooks are
// chained AFTER any animateTick a block already has, so a falling block's dust
// and a portal's motes keep running.
#pragma once

#include "common/world/block/BlockRegistry.hpp"
#include "common/world/fluid/FluidState.hpp"

#include <array>

namespace Game {

    struct EntityLevel;
    class JavaRandom;

    // Wire the ambient-sound animateTicks into the block table. Called last by
    // BlockRegistry_RegisterBehaviors, so every other hook is already in place.
    void RegisterAmbientBlockSounds(std::array<Block, BlockRegistry::Size>& blocks);

    // Aurelith's river (resonant_water: the Vesper lapping at its quays).
    // Separate from RegisterAmbientBlockSounds because Aurelith's own
    // registration (BlockRegistry_RegisterAurelithBlocks, which gives the
    // water its motes) runs after the behaviours; BlockRegistry::Init calls
    // this once that has run, so the sound chains after the motes.
    void RegisterAurelithAmbientSounds(std::array<Block, BlockRegistry::Size>& blocks);

    // MC FluidState.animateTick → WaterFluid / LavaFluid.animateTick: the
    // fluid in a sampled cell, waterlogged blocks included (ClientLevel
    // .doAnimateTick runs it for any non-empty fluid state).
    void FluidAnimateTickSounds(EntityLevel& level, const glm::ivec3& pos, FluidState fluid,
                                JavaRandom& random);

} // namespace Game

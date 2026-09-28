// File: src/common/world/block/BlockAnimateParticles.hpp
//
// The PARTICLE half of MC's client-side Block.animateTick for every vanilla
// block that has one (26.3): torches and wall torches (soul / copper flames),
// redstone torches, ore, dust, repeaters and levers, fire and soul fire,
// campfires' lava pops, furnaces / smokers / blast furnaces, the brewing
// stand, lit candles and candle cakes, the enchanting table's glyphs, the
// ender chest, nether portal, end portal and end gateway, end rods, the
// respawn anchor, crying obsidian, wet sponge, beehives dripping honey,
// suspicious sand / gravel dust, bubble columns, mycelium, spore blossoms,
// firefly bushes, the leaves' rain drips and falling leaves (cherry, pale
// oak, the poplars, azalea and the biome-tinted ones), wither roses,
// active sculk sensors, dried ghasts, lightning rods in a storm and
// pointed dripstone's drips — plus FluidAnimateTickParticles for the
// sampled fluid (the lava pops and underwater motes live with the fluid
// sounds, BlockAmbientSounds.cpp).
//
// The sound halves stay in BlockAmbientSounds.cpp; both chain onto whatever
// animateTick a block already had (RegisterBlockAnimateParticles runs before
// RegisterAmbientBlockSounds). Everything here is client-only: it runs from
// the client's ClientLevel.animateTick sweep against the client bridge.
#pragma once

#include "common/world/block/BlockRegistry.hpp"

#include <array>

namespace Game {

    struct EntityLevel;

    void RegisterBlockAnimateParticles(std::array<Block, BlockRegistry::Size>& blocks);

} // namespace Game

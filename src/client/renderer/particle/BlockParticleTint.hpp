// File: src/client/renderer/particle/BlockParticleTint.hpp
//
// MC BlockColors (createDefault) + BlockTintSource for tint layer 0, as the
// particle code asks for it:
//
//   asTerrainParticle = true   BlockTintSource.colorAsTerrainParticle —
//                              TerrainParticle / FallingDustParticle's tint
//                              (grass_block answers -1: its debris is dirt;
//                              water and bubble columns answer the water
//                              colour).
//   asTerrainParticle = false  BlockTintSource.colorInWorld —
//                              ClientLevel.getClientLeafTintColor, the
//                              tinted leaves' falling-leaf colour.
//
// The biome colours are ClientLevel.calculateBlockTint's (2r+1)² average at
// the cell's height, r the Biome Blend option (Mesher::GetMeshOptions).
// Returns 0xRRGGBB, or -1 when the block has no tint source for layer 0.
#pragma once

#include "common/world/block/BlockState.hpp"

#include <glm/glm.hpp>

#include <cstdint>

namespace Game { struct IBlockAccess; }

namespace Render {

    int64_t BlockTintColor(Game::BlockState state, const Game::IBlockAccess* blocks, const glm::ivec3& pos,
                           bool asTerrainParticle);

} // namespace Render

// File: src/common/world/block/BlockStateModels.hpp
//
// Port of MC's blockstate → model dispatch (assets/minecraft/blockstates/*.json,
// parsed by BlockStateModelLoader into BlockModelDefinition/Variant).
//
// MC keys a baked model off the whole BlockState, so `facing=east` and
// `facing=north` are two different entries in one map pointing at two
// separately-baked quad sets (BlockModelShaper.modelByStateCache). We do the
// same thing, with our (BlockID, stateIndex) pair standing in for BlockState:
// at load we resolve each state to a model name, synthesise the rotated variant
// the JSON's `"x"`/`"y"` asks for, and register it under a derived name. After
// that the mesher just looks up a model per state and never thinks about
// orientation — same division of labour as vanilla.
//
// Only the `variants` form is handled. `multipart` (fences, walls, redstone
// wire) resolves to the block's plain model with a warning; those blocks derive
// their appearance from neighbours rather than from placement, so they are a
// separate piece of work.
#pragma once

#include "Blocks.hpp"
#include <string>

namespace Game {

    class BlockStateModels {
    public:
        // Loads every blockstate JSON and registers the rotated models it
        // implies. Must run AFTER BlockRegistry::Init (it needs the per-block
        // state definitions) and AFTER BlockModelRegistry::LoadModels (it
        // rotates already-resolved models). Missing directory is not an error —
        // every block simply keeps its default model, which is the behaviour
        // before blockstates existed here.
        static bool Load(const std::string& blockstatesPath = "assets/blockstates");

        // Model name for one state of one block. Falls back to the block's
        // plain `modelName` when the block has no blockstate JSON, no states,
        // or the state didn't match any variant.
        static const std::string& ModelNameFor(BlockID id, BlockStateIndex stateIndex);

        static void Clear();

    private:
        BlockStateModels() = delete;
    };

} // namespace Game

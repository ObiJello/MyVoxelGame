// File: src/common/world/block/PotentSulfurBlock.hpp
//
// Mirrors net.minecraft.world.level.block.PotentSulfurBlock (26.3) — the
// block half of the sulfur geyser: the `potent_sulfur_state` it derives from
// the water above and the block below (validBlockState, run from
// updateShape and getStateForPlacement), the block event + start sound when
// it begins to erupt (onPlace), the bubbles and noxious hiss while wet
// (animateTick), and the eruption clock the block event resets
// (triggerEvent). The tickers — nausea, the dormant/erupting countdown and
// the launch — are PotentSulfurBlockEntity's.
//
// The noxious-gas geometry (findNoxiousGasSourceBlock, canBeReachedBy-
// NoxiousGas, getUnobstructedBlockCount) lives here rather than on the block
// entity, because the client's NOXIOUS_GAS_CLOUD particle reads it too and
// has no block entity in hand.
#pragma once

#include "BlockRegistry.hpp"

#include <array>
#include <optional>
#include <glm/glm.hpp>

namespace Game {

    class ILevelWrite;

    namespace PotentSulfur {

        // MC PotentSulfurState, in its declaration order — which is the
        // value order of BlockStateProperties.POTENT_SULFUR_STATE and so the
        // property index (tools/gen_block_states.py).
        enum class State : uint8_t { Dry = 0, Wet = 1, Dormant = 2, Erupting = 3, Continuous = 4 };

        State StateOf(BlockState state);
        BlockState WithState(BlockState state, State value);

        // MC PotentSulfurBlock.ALLOWED_WATER_BLOCKS_ABOVE.
        constexpr int kAllowedWaterBlocksAbove = 4;

        // MC PotentSulfurBlock.validBlockState: DRY without a water source
        // above; with one, CONTINUOUS over a causes_continuous_geyser_eruptions
        // block, a geyser (DORMANT, or ERUPTING kept) over a
        // causes_periodic_geyser_eruptions block, WET otherwise. `blockEntities`
        // is the level whose PotentSulfurBlockEntity a new geyser's countdown
        // is reset on (MC reaches it through the LevelReader); null at
        // placement, when there is none yet.
        BlockState ValidBlockState(BlockState state, const IBlockAccess& level,
                                   const glm::ivec3& pos, ILevelWrite* blockEntities);

        // MC PotentSulfurBlockEntity.isGeyserPassableBlock: air and water
        // pass; anything else passes when its collision shape is empty.
        bool IsGeyserPassable(const IBlockAccess& level, const glm::ivec3& pos);

        // MC PotentSulfurBlockEntity.findNoxiousGasSourceBlock: the first
        // open cell above the water column standing on `origin` (at most
        // kAllowedWaterBlocksAbove + 1 up), or none.
        std::optional<glm::ivec3> FindNoxiousGasSourceBlock(const IBlockAccess& level,
                                                            const glm::ivec3& origin);

        // MC PotentSulfurBlockEntity.canBeReachedByNoxiousGas.
        bool CanBeReachedByNoxiousGas(const IBlockAccess& level, const glm::ivec3& sourceBlock,
                                      const glm::dvec3& pos);

        // MC PotentSulfurBlockEntity.getUnobstructedBlockCount: how many
        // passable cells rise from `pos`, up to 6 per water block.
        int GetUnobstructedBlockCount(const IBlockAccess& level, const glm::ivec3& pos,
                                      int waterBlocks);

    } // namespace PotentSulfur

    // Wires updateShape / onPlace / animateTick / triggerEvent onto
    // BlockID::PotentSulfur. Called from BlockRegistry_RegisterBehaviors.
    void RegisterPotentSulfurBehaviors(std::array<Block, BlockRegistry::Size>& blocks);

} // namespace Game

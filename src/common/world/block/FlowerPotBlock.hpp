// File: src/common/world/block/FlowerPotBlock.hpp
//
// Mirrors net.minecraft.world.level.block.FlowerPotBlock: one block class for
// the empty pot and every potted plant. A block item whose block has a potted
// twin (MC POTTED_BY_CONTENT) goes into an empty pot; an empty hand (or any
// item that cannot be potted) takes the plant back out; pick-block on a potted
// plant gives the plant (getCloneItemStack).
//
// MC builds POTTED_BY_CONTENT in the FlowerPotBlock constructor, one entry per
// `new FlowerPotBlock(content, ...)` in Blocks.java. Here the same map is
// derived from the registry slugs once at registration: every `potted_<x>`
// block holds `<x>` (azalea's two pots are the only vanilla names that do not
// follow the rule: potted_azalea_bush / potted_flowering_azalea_bush). A new
// potted block — vanilla or engine (the Hush's potted resonance bloom) — is
// therefore pottable the moment its BlockDefs row exists.
#pragma once

#include "BlockRegistry.hpp"

#include <array>

namespace Game {

    namespace FlowerPot {

        // MC FlowerPotBlock.potted — the plant a potted block holds, or Air
        // for the empty flower pot and for any block that is not a pot.
        BlockID ContentOf(BlockID pot);

        // MC POTTED_BY_CONTENT.getOrDefault(content, AIR) — the potted block
        // for a plant, or Air when the plant has no pot.
        BlockID PottedOf(BlockID content);

        // True for the empty pot and every potted plant (#flower_pots plus
        // flower_pot itself).
        bool IsFlowerPot(BlockID id);

        // MC FlowerPotBlock.getCloneItemStack's block: the plant for a potted
        // block, else the block itself (the empty pot picks the pot).
        BlockID CloneBlock(BlockID id);

    } // namespace FlowerPot

    void RegisterFlowerPotBehaviors(std::array<Block, BlockRegistry::Size>& blocks);

} // namespace Game

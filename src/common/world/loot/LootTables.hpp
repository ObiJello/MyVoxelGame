// File: src/common/world/loot/LootTables.hpp
//
// Block loot tables — the C++ counterpart of MC's LootTable / LootPool /
// LootPoolEntry evaluation for the `minecraft:block` parameter set.
//
// MC resolves a block's loot table by NAME CONVENTION, not registration:
// BlockBehaviour.Properties' constructor (state/BlockBehaviour.java:455) points
// every block at `blocks/<its registry id>`, and `noLootTable()` clears it.
// We keep that: kLootTables is keyed on the vanilla block slug, matched here
// against Block::registrySlug, and a block with no table drops nothing.
//
// The tables themselves are baked by tools/gen_loot_tables.py — see
// GeneratedLootTables.hpp for the row layout. This file is only the evaluator.
//
// NOT handled (deliberate, each documented at its use site): explosions,
// block-entity contents (copy_components / copy_state), and enchantments on
// tools, which the item system does not carry yet — so Silk Touch and Fortune
// evaluate at level 0, which is the vanilla no-enchantment branch.
#pragma once

#include "common/entity/Item.hpp"
#include "common/world/block/Blocks.hpp"
#include <glm/glm.hpp>
#include <vector>

namespace Game {

    class JavaRandom;
    class World;

    // MC's LootParams for LootContextParamSets.BLOCK, minus the parameters
    // nothing can supply yet (explosion radius, killer entity, block entity).
    struct LootContext {
        BlockID          block      = BlockID::Air;
        uint8_t          blockState = 0;
        const ItemStack* tool       = nullptr;   // held stack; null/empty = bare hand
        const World*     world      = nullptr;   // for location_check; optional
        glm::ivec3       pos{0, 0, 0};
        // MC's `this` entity parameter. True whenever a player/mob broke the
        // block — false would mean an explosion or a piston did, which nothing
        // produces yet.
        bool             brokenByEntity = true;
        JavaRandom*      rng        = nullptr;   // required
    };

    class LootTables {
    public:
        // Resolves every baked slug to a numeric id. Call AFTER BlockRegistry,
        // ItemRegistry and RecipeManager (whose slug map it borrows). Idempotent.
        static void Initialize();

        // MC Block.getDrops → BlockState.getDrops → LootTable.getRandomItems.
        // Returns the stacks to hand out, already split to respect each item's
        // max stack size (MC LootTable.createStackSplitter).
        static std::vector<ItemStack> GetDrops(const LootContext& ctx);

        // True when the block has a loot table at all. A block without one
        // (air, fire, bedrock, spawner…) is MC's `noLootTable()`.
        static bool HasLootTable(BlockID block);
    };

} // namespace Game

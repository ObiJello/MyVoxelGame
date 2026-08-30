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
// NOT handled (deliberate, each documented at its use site): block-entity
// contents (copy_components / copy_state) and enchantments on
// tools, which the item system does not carry yet — so Silk Touch and Fortune
// evaluate at level 0, which is the vanilla no-enchantment branch.
#pragma once

#include "common/entity/Item.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include <glm/glm.hpp>
#include <vector>

namespace Game {

    class JavaRandom;
    class World;

    // MC's LootParams for LootContextParamSets.BLOCK, minus the parameters
    // nothing can supply yet (explosion radius, killer entity, block entity).
    struct LootContext {
        BlockID          block      = BlockID::Air;
        BlockStateIndex          blockState = 0;
        const ItemStack* tool       = nullptr;   // held stack; null/empty = bare hand
        // For location_check. Typed as the READ interface rather than as
        // Game::World so every caller can actually supply one — the explosion
        // path holds an ILevelWrite (which is an IBlockAccess) and had to leave
        // this null, which silently short-circuited location_check to TRUE and
        // made tall_grass / large_fern drop against their own conditions.
        const IBlockAccess* blocks = nullptr;
        glm::ivec3       pos{0, 0, 0};
        // MC's `this` entity parameter. True whenever a player/mob broke the
        // block — false would mean an explosion or a piston did, which nothing
        // produces yet.
        bool             brokenByEntity = true;
        // MC LootContextParams.EXPLOSION_RADIUS. Negative means ABSENT, which
        // is the state for every ordinary block break and is what makes
        // survives_explosion pass and apply_explosion_decay a no-op.
        //
        // Supplied only for a DESTROY_WITH_DECAY explosion — so vanilla TNT
        // (whose decay gamerule defaults false) still drops everything, while a
        // creeper's blast decays at 1/3.
        float            explosionRadius = -1.0f;
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

        // MC Block.spawnAfterBreak's XP half: how much experience this block
        // pays when a player mines it. 0 for almost everything; the ore
        // family, the sculk family and the spawner are the exceptions, each
        // with the range its Blocks.java registration passes to
        // DropExperienceBlock (coal 0-2, diamond/emerald 3-7, lapis/quartz
        // 2-5, redstone 1-5, nether gold 0-1, sculk 1, sensors/shrieker/
        // catalyst 5, spawner 15 + nextInt(15) + nextInt(15)).
        //
        // Callers gate exactly like MC does: only on a player break that
        // passed hasCorrectToolForDrops, never in creative. The Silk Touch
        // zeroing (EnchantmentHelper.processBlockExperience) is applied here
        // through the same EnchantmentLevel hook the loot conditions use, so
        // it starts working the moment tools can carry enchantments — except
        // for the spawner, whose SpawnerBlock.spawnAfterBreak pays
        // unconditionally.
        static int RollBlockBreakExperience(BlockID block, const ItemStack* tool,
                                            JavaRandom& rng);
    };

} // namespace Game

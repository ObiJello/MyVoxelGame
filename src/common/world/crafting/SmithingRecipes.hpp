// File: src/common/world/crafting/SmithingRecipes.hpp
//
// Mirrors net.minecraft.world.item.crafting.SmithingTransformRecipe and
// SmithingTrimRecipe (26.3): the data pack's smithing recipes, read from
// data/<ns>/recipe/*.json on first use (the recipe generator's tables leave
// them out — their results are component edits, not item rows).
//
//   * smithing_transform — {template, base, addition, result: {id, count?,
//     components?}}: the base's own components carried onto the result item
//     (ItemStack.transmuteCopy), then the result's component patch.
//   * smithing_trim — {template, base, addition, pattern}: a copy of the
//     base with TRIM {the addition's PROVIDES_TRIM_MATERIAL, pattern};
//     nothing when the base already wears exactly that trim.
#pragma once

#include "common/entity/Item.hpp"

#include <string>
#include <vector>

namespace Game::SmithingRecipes {

    // MC SmithingRecipe.matches + assemble over the first matching recipe
    // (RecipeManager.getRecipeFor): the result, or an empty stack.
    ItemStack Assemble(const ItemStack& templateItem, const ItemStack& base, const ItemStack& addition);

    // The id of the recipe Assemble would use ("minecraft:netherite_sword_
    // smithing", "minecraft:rib_armor_trim_smithing_template_smithing_trim"),
    // empty when none matches — what RECIPE_CRAFTED reports.
    std::string MatchingRecipeId(const ItemStack& templateItem, const ItemStack& base, const ItemStack& addition);

    // RecipePropertySet.SMITHING_TEMPLATE / SMITHING_BASE / SMITHING_ADDITION:
    // what each input square accepts (some recipe's ingredient for it).
    bool IsTemplate(const ItemStack& stack);
    bool IsBase(const ItemStack& stack);
    bool IsAddition(const ItemStack& stack);

} // namespace Game::SmithingRecipes

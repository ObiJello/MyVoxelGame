#pragma once
#include <vector>
namespace console {
// One crafting recipe as the console crafting menu presents it: the result
// and the total ingredients it consumes (the console UI is ingredient based,
// not a free-form grid). `needsTable` marks recipes wider than the 2x2
// inventory grid, which require a crafting table.
struct CraftingIngredient { int id=0,damage=-1,count=1; }; // damage -1: any
struct CraftingRecipe {
    int id=0,count=1,damage=0;
    std::vector<CraftingIngredient> ingredients;
    bool needsTable=false;
    const char* group="";
};
// RECONSTRUCTED: Recipes.cpp and its ToolRecipies/WeaponRecipies/
// FoodRecipies/OreRecipies/StructureRecipies tables are not part of the
// supplied archive subset. This table reproduces the same-era recipe set
// by hand and must be replaced by an extraction once those files exist.
const std::vector<CraftingRecipe>& consoleCraftingRecipes();
}

// File: src/common/world/crafting/RecipeManager.hpp
//
// Mirrors net.minecraft.world.item.crafting.RecipeManager, narrowed to the
// crafting-table recipe types (see tools/gen_recipes.py for what is and isn't
// baked into the table).
//
// The recipe data itself is generated — GeneratedRecipeList.{hpp,cpp} — as
// slugs; Initialize() resolves those to ItemIDs once, after BlockRegistry and
// ItemRegistry are up. Slugs that no longer resolve (a data pack ahead of our
// block list) drop their recipe with a debug log rather than failing the load.
//
// Runs on both sides: the server for authority, the client so a crafting
// screen can show the result the instant a grid slot changes instead of after
// a round trip. Same code, same answer.
#pragma once

#include "GeneratedRecipeList.hpp"      // RecipeKind, the baked tables
#include "common/entity/Item.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace Game {

    // MC CraftingInput — a crafting grid trimmed to the bounding box of its
    // non-empty cells. Trimming is what lets a 2x2 recipe match anywhere in a
    // 3x3 grid: both sides shrink to the same shape before they are compared.
    class CraftingInput {
    public:
        // MC CraftingInput.ofPositioned. `left`/`top` receive the offset of the
        // trimmed box inside the original grid — ResultSlot needs them to know
        // which real slots to consume.
        static CraftingInput OfPositioned(int width, int height,
                                          const std::vector<ItemStack>& items,
                                          int& outLeft, int& outTop);
        static CraftingInput Of(int width, int height,
                                const std::vector<ItemStack>& items) {
            int l = 0, t = 0;
            return OfPositioned(width, height, items, l, t);
        }

        int Width()  const { return m_width; }
        int Height() const { return m_height; }
        int Size()   const { return static_cast<int>(m_items.size()); }
        // Number of non-empty cells. MC compares this against a recipe's own
        // ingredient count as the first, cheapest rejection.
        int IngredientCount() const { return m_ingredientCount; }
        bool IsEmpty() const { return m_ingredientCount == 0; }

        const ItemStack& GetItem(int index) const;
        const ItemStack& GetItem(int x, int y) const { return GetItem(x + y * m_width); }
        const std::vector<ItemStack>& Items() const { return m_items; }

    private:
        int m_width = 0;
        int m_height = 0;
        int m_ingredientCount = 0;
        std::vector<ItemStack> m_items;
    };

    // A resolved recipe. `cells` holds one ingredient index per cell for a
    // shaped pattern (-1 = the cell must be empty) or one per ingredient for
    // the unordered kinds; indices address RecipeManager's ingredient pool.
    struct CraftingRecipe {
        const char* id = "";
        RecipeKind  kind = RecipeKind::Shaped;
        int         width = 0;
        int         height = 0;
        ItemID      resultItem = Items::Air;
        int         resultCount = 1;
        std::vector<int32_t> cells;
        int         ingredientCount = 0;   // non-empty cells
        // MC ShapedRecipePattern.symmetrical — a mirrored pattern matches the
        // same input either way round, so the flipped pass can be skipped.
        bool        symmetrical = false;
    };

    // A resolved furnace-family recipe (MC AbstractCookingRecipe).
    struct CookingRecipe {
        const char* id = "";
        CookingKind kind = CookingKind::Smelting;
        int32_t     ingredient = -1;      // index into the ingredient pool
        ItemID      resultItem = Items::Air;
        int         resultCount = 1;
        int         cookingTime = 200;    // ticks
        float       experience = 0.0f;
    };

    // A resolved stonecutter recipe (MC StonecutterRecipe / SingleItemRecipe).
    struct StonecuttingRecipe {
        int32_t ingredient = -1;
        ItemID  resultItem = Items::Air;
        int     resultCount = 1;
    };

    class RecipeManager {
    public:
        // Resolve the generated tables. Call AFTER BlockRegistry::Initialize
        // and ItemRegistry::Initialize. Idempotent.
        static void Initialize();

        // MC RecipeManager.getRecipeFor(RecipeType.CRAFTING, input, level).
        // Null when nothing matches. The returned pointer is owned by the
        // manager and lives for the process.
        static const CraftingRecipe* Find(const CraftingInput& input);

        // MC CraftingRecipe.assemble — the output stack for a matched recipe.
        // `input` matters only for Transmute, whose result inherits the input
        // stack's components.
        static ItemStack Assemble(const CraftingRecipe& recipe, const CraftingInput& input);

        // MC CraftingRecipe.getRemainingItems — what stays behind in each grid
        // cell after one craft (the bucket from a milk bucket). Indexed like
        // `input`, i.e. over the TRIMMED grid. Empty stacks mean "nothing left".
        static std::vector<ItemStack> GetRemainingItems(const CraftingInput& input);

        // Registry-slug lookup ("oak_planks" -> the BlockItem's ItemID).
        // Items::Air when unknown. Built during Initialize.
        static ItemID ItemFromSlug(const std::string& slug);

        // MC RecipeManager.getRecipeFor(RecipeType.SMELTING, …). A furnace only
        // ever asks for Smelting, a blast furnace for Blasting, and so on — the
        // kind IS the block. Null when the input smelts into nothing.
        static const CookingRecipe* FindCooking(CookingKind kind, const ItemStack& input);

        // MC FuelValues.burnDuration — how many ticks one of this item keeps a
        // furnace lit. 0 means "not a fuel".
        static int GetFuelBurnTime(const ItemStack& stack);

        // Every stonecutter result `input` can become, in table order. MC's
        // StonecutterMenu builds the same list and shows it as the pick-grid.
        static std::vector<const StonecuttingRecipe*> FindStonecutting(const ItemStack& input);

        // Diagnostics.
        static size_t RecipeCount();
        static size_t CookingRecipeCount();

    private:
        // Does `stack` satisfy ingredient `index`? MC's Ingredient.test only
        // looks at the item, never the components.
        static bool IngredientMatches(int32_t index, const ItemStack& stack);
        static bool ShapedMatches(const CraftingRecipe& r, const CraftingInput& in);
        static bool UnorderedMatches(const CraftingRecipe& r, const CraftingInput& in);
    };

} // namespace Game

// File: src/common/entity/DyeColorUtil.cpp
#include "common/data/DataComponents.hpp"
#include "common/entity/DyeColorUtil.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/world/crafting/RecipeManager.hpp"

#include <vector>

namespace Game {

    namespace {
        // MC DyeColor's textureDiffuseColor column, in ordinal order
        // (DyeColor.java: WHITE 16383998 … BLACK 1908001).
        constexpr uint32_t kTextureDiffuse[16] = {
            0xF9FFFE,   // white
            0xF9801D,   // orange
            0xC74EBD,   // magenta
            0x3AB3DA,   // light_blue
            0xFED83D,   // yellow
            0x80C71F,   // lime
            0xF38BAA,   // pink
            0x474F52,   // gray
            0x9D9D97,   // light_gray
            0x169C9C,   // cyan
            0x8932B8,   // purple
            0x3C44AA,   // blue
            0x835432,   // brown
            0x5E7C16,   // green
            0xB02E26,   // red
            0x1D1D21,   // black
        };
    } // namespace

    int DyeColorOfItem(uint32_t itemId) {
        // The item's default DYE (Items.java gives the sixteen dyes theirs).
        return DyeColorOf(ItemStack(static_cast<ItemID>(itemId), 1));
    }

    uint32_t DyeItemOfColor(uint8_t color) {
        return Items::WhiteDye + (color & 0x0F);
    }

    uint32_t DyeTextureDiffuseColor(uint8_t color) {
        return kTextureDiffuse[color & 0x0F];
    }

    uint8_t GetMixedDyeColor(uint8_t a, uint8_t b, JavaRandom& random) {
        // MC findColorMixInRecipes: CraftingInput.of(2, 1, [dye(a), dye(b)])
        // against the crafting recipes; the result's DYE colour wins.
        const std::vector<ItemStack> grid = {
            ItemStack(DyeItemOfColor(a), 1),
            ItemStack(DyeItemOfColor(b), 1),
        };
        const CraftingInput input = CraftingInput::Of(2, 1, grid);
        if (const CraftingRecipe* recipe = RecipeManager::Find(input)) {
            const ItemStack result = RecipeManager::Assemble(*recipe, input);
            const int mixed = DyeColorOfItem(result.itemId);
            if (mixed >= 0) return static_cast<uint8_t>(mixed);
        }
        return random.NextBool() ? a : b;
    }

} // namespace Game

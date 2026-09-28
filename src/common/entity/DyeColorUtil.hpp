// File: src/common/entity/DyeColorUtil.hpp
//
// MC net.minecraft.world.item.DyeColor — the pieces the pet collars need.
//
// A colour is its DyeColor ORDINAL (white 0 … black 15), the same numbering
// the sheep's wool byte, the sign text colour (SignBlockEntity.hpp's
// Game::DyeColor) and MC's LEGACY_ID_CODEC byte ("CollarColor") use.
//
//   * DyeColorOfItem — MC `itemStack.get(DataComponents.DYE)`: the sixteen dye
//     items carry their colour; nothing else does. (ItemTags.DYES, which the
//     26.3 WOLF_COLLAR_DYES / CAT_COLLAR_DYES tags are built from, is exactly
//     those sixteen items.)
//   * DyeTextureDiffuseColor — DyeColor.getTextureDiffuseColor(), the tint a
//     collar layer (WolfCollarLayer / CatCollarLayer) multiplies its sheet by.
//     NOT the sheep's wool table, which is these scaled by 0.75.
//   * GetMixedDyeColor — DyeColor.getMixedColor: the crafting-recipe result of
//     the two parents' dyes when one exists (red + yellow = orange), else a
//     coin flip between the two — what a bred pup's collar gets.
#pragma once

#include <cstdint>

namespace Game {

    class JavaRandom;

    // MC DyeColor.RED — both pets' DEFAULT_COLLAR_COLOR.
    inline constexpr uint8_t kDyeColorRed = 14;

    // MC itemStack.get(DataComponents.DYE): the colour ordinal of a dye item,
    // or -1 for anything that is not one.
    int DyeColorOfItem(uint32_t itemId);

    // The dye item of a colour ordinal (white_dye … black_dye).
    uint32_t DyeItemOfColor(uint8_t color);

    // MC DyeColor.getTextureDiffuseColor(), as 0xRRGGBB.
    uint32_t DyeTextureDiffuseColor(uint8_t color);

    // MC DyeColor.getMixedColor(level, a, b). Server side (it consults the
    // crafting recipes); the coin flip draws from `random` only when no
    // recipe mixes the pair, exactly as MC's level.getRandom().nextBoolean().
    uint8_t GetMixedDyeColor(uint8_t a, uint8_t b, JavaRandom& random);

} // namespace Game

// File: src/client/renderer/texture/MipmapGenerator.hpp
//
// Minecraft's atlas mipmap pipeline, ported verbatim. Sources, in the order
// this file uses them:
//   net/minecraft/client/renderer/texture/MipmapGenerator
//   net/minecraft/client/renderer/texture/MipmapStrategy
//   com/mojang/blaze3d/platform/TextureUtil   (solidify, fillEmptyAreasWithDarkColor)
//   net/minecraft/util/ARGB                   (sRGB<->linear tables, meanLinear)
//
// Why this exists rather than glGenerateMipmap: the driver box-filters raw
// RGBA, which is wrong for the block atlas in two separate ways.
//
//  1. It averages the RGB sitting under alpha=0. Vanilla's cutout sprites
//     store black there (every one of oak_leaves.png's 84 transparent texels
//     is 0,0,0 against an opaque mean of 144,144,144), so a plain box filter
//     drags every leaf edge toward black as it minifies. MC runs `solidify`
//     first, flooding each transparent texel with its nearest opaque colour
//     while KEEPING alpha at 0, so the colour average sees no black.
//
//  2. It halves alpha-test coverage at every level. A cutout sprite is binary
//     alpha; averaging turns 2-of-4 opaque into 0.5, which sits exactly on the
//     terrain cutout threshold, so foliage either dissolves or fattens with
//     distance depending on which side of 0.5 it lands. MC measures the
//     coverage of level 0 and rescales each mip's alpha to match it
//     (scaleAlphaToCoverage), which is what keeps a distant canopy reading at
//     the same density as a near one.
//
// The strategy is per sprite, from `texture.mipmap_strategy` in the .mcmeta
// (defaulting to AUTO). It genuinely matters here: this asset set ships 27
// `strict_cutout`, 12 `dark_cutout` and 5 `mean` overrides, and EVERY leaf
// texture is `dark_cutout` — vanilla deliberately darkens leaf mips rather
// than letting them wash out, so leaves are exactly the sprites a naive box
// filter gets most wrong.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Render::Mipmap {

    // MC MipmapStrategy. AUTO resolves per sprite at generation time.
    enum class Strategy { Auto, Mean, Cutout, StrictCutout, DarkCutout };

    // Parses the .mcmeta spelling ("mean", "strict_cutout", …). Unknown or
    // empty -> Auto, matching MC's codec default.
    Strategy ParseStrategy(const std::string& name);

    // One RGBA8 image, row-major, tightly packed.
    struct Image {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> pixels;

        size_t ByteSize() const {
            return static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
        }
    };

    // MC MipmapGenerator.generateMipLevels.
    //
    // Returns levels 0..maxLevel inclusive (so maxLevel+1 images), each half
    // the previous size. `level0` is taken by value because the cutout
    // strategies rewrite it in place before it becomes result[0] — that
    // rewrite is part of the algorithm, not a side effect.
    //
    // `isItemTexture` mirrors MC's `name.getPath().startsWith("item/")` guard,
    // which skips the level-0 rewrite for item sprites.
    //
    // Sizes must be even down to maxLevel; the caller is responsible for that
    // (the block atlas packs everything at multiples of 16). A dimension that
    // cannot halve stops the chain early and the returned vector is shorter.
    std::vector<Image> GenerateMipLevels(Image level0,
                                         int maxLevel,
                                         Strategy strategy,
                                         float alphaCutoffBias,
                                         bool isItemTexture);

} // namespace Render::Mipmap

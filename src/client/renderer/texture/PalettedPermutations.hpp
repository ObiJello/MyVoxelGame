// File: src/client/renderer/texture/PalettedPermutations.hpp
//
// Mirrors net.minecraft.client.renderer.texture.atlas.sources.
// PalettedPermutations and client.resources.palette.PaletteMapping: a
// grey-scale base sprite recoloured through a palette — every pixel whose
// colour is the i-th entry of the KEY palette (trims/color_palettes/
// trim_palette) takes the i-th entry of a material's palette (iron,
// gold_darker …), its alpha multiplied by that entry's. Used by the atlas
// source of the same name (the trimmed armour item icons) and by the armour
// trim layer's per-material entity textures.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Render::PalettedPermutations {

    // A palette image's pixels in reading order, RGBA packed as 0xAARRGGBB;
    // empty when the file cannot be read.
    std::vector<uint32_t> LoadPalette(const std::string& file);

    // PaletteMapping.create(key, target).apply over every pixel of an RGBA8
    // image, in place. False when the palettes' sizes differ (MC throws).
    bool Apply(std::vector<unsigned char>& rgba, const std::vector<uint32_t>& keyPalette,
               const std::vector<uint32_t>& targetPalette);

    // The base image `file` recoloured by `targetPaletteFile` under
    // `keyPaletteFile`. False when any file is missing.
    bool Build(const std::string& file, const std::string& keyPaletteFile, const std::string& targetPaletteFile,
               int& width, int& height, std::vector<unsigned char>& rgba);

} // namespace Render::PalettedPermutations

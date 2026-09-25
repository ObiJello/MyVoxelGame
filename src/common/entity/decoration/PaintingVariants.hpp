// File: src/common/entity/decoration/PaintingVariants.hpp
//
// MC PaintingVariant + the minecraft:painting_variant registry — the
// data-driven canvases (data/<ns>/painting_variant/*.json): size in blocks,
// the texture's asset id, and an optional title / author line for the
// painting item's tooltip.
//
//   { "asset_id": "minecraft:aztec", "width": 1, "height": 1,
//     "title":  { "translate": "painting.minecraft.aztec.title",  "color": "yellow" },
//     "author": { "translate": "painting.minecraft.aztec.author", "color": "gray" } }
//
// A variant's INDEX in All() is its network id (the painting's variant byte
// on the wire): the registry is sorted by id, and client and server read the
// same data directory, so the two always agree. Placeable() is the
// #minecraft:placeable tag — the variants a plain painting item may pick
// from (MC PaintingVariantTags.PLACEABLE); the rest (the four
// "earth / wind / fire / water" elementals) only come from a painting item
// that names its variant, or from a saved world.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Game {

    struct PaintingVariant {
        // A tooltip line: MC's Component, reduced to what the variant JSON
        // uses — a translation key and a named colour.
        struct TextLine {
            std::string translate;
            std::string color;
        };

        std::string id;          // "minecraft:aztec"
        std::string assetId;     // "minecraft:aztec" — textures/painting/<path>.png
        int width  = 1;          // blocks, 1..16
        int height = 1;
        std::optional<TextLine> title;
        std::optional<TextLine> author;

        // MC PaintingVariant.area.
        int Area() const { return width * height; }
    };

    namespace PaintingVariants {
        // Every variant, sorted by id; the index is the network id. Loaded
        // on first use.
        const std::vector<PaintingVariant>& All();
        // nullptr for an index out of range.
        const PaintingVariant* Get(int index);
        // The index of "ns:path" (or a bare path, minecraft: assumed); -1
        // when the registry has no such variant.
        int IndexOf(std::string_view id);
        // #minecraft:placeable, as indices (nested tags followed).
        const std::vector<int>& Placeable();
        // The texture's asset path, "assets/textures/painting/<path>.png".
        std::string TexturePath(const PaintingVariant& variant);
    }

} // namespace Game

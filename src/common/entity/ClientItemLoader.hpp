// File: src/common/entity/ClientItemLoader.hpp
// Reads MC 1.21.4+ client-item-info JSON from `assets/items/{slug}.json` — the
// modern dispatch tree that supersedes the old `assets/models/item/{slug}.json`
// + `overrides[]` system. See:
//   minecraft_code_26.1-snapshot-1/decompiled_net/minecraft/client/renderer/item/ItemModel.java
//   minecraft_code_26.1-snapshot-1/decompiled_net/minecraft/client/renderer/item/{Conditional,
//                                                                 RangeSelect,
//                                                                 Select}ItemModel.java
//
// The schema is a tree of nodes:
//   {"type": "minecraft:model",          "model": "<ref>"}
//   {"type": "minecraft:range_dispatch", "property": "...", "entries": [...], "fallback": ...}
//   {"type": "minecraft:condition",      "on_true": ..., "on_false": ...}
//   {"type": "minecraft:select",         "cases": [...], "fallback": ...}
//   {"type": "minecraft:special",        "base": "<ref>", ...}     (BEWLR — chest/sign/etc.)
//
// We collapse this tree into a flat description by always taking the "default"
// branch (fallback / on_false / first case), recording the resting model ref,
// plus the full list of model refs hit by the most-specific range_dispatch (for
// animation frames). Conditional state we don't simulate (lodestone tracker,
// fishing cast, etc.) just renders the rest frame.
#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace Game {

    enum class ClientItemKind {
        Missing,      // items/{slug}.json doesn't exist
        FlatSprite,   // rest model is item/X — render as flat 2D sprite
        BlockModel,   // rest model is block/X — render via 3D iso using that block model
        Special,      // BEWLR (chest, sign, banner, shield…) — fall back to flat-sprite for now
        Composite,    // 26.x "minecraft:composite": several block models, each with its own
                      // transformation — baked into one synthetic block model (see
                      // ItemRegistry::BakeCompositeItemModels) named by restSlug
    };

    // One child of a composite item model: a block model plus the part of
    // its `transformation` a BlockModel can carry — whole quarter turns
    // (BlockModelRegistry::RotateModel) and an offset in model pixels. The
    // quarter turns pivot about the cell centre the way RotateModel does, so
    // `offsetPx` already includes the difference between that pivot and MC's
    // rotate-about-origin-then-translate (Transformation.compose).
    struct CompositeChild {
        std::string modelSlug;
        int         xQuarterTurns = 0;
        int         yQuarterTurns = 0;
        glm::vec3   offsetPx{0.0f};
    };

    // Which ItemTintSource a `tints` entry is. Only the kinds whose colour
    // depends on the STACK need telling apart from a fixed default; the rest
    // (constant, grass, foliage, dye's default) are baked into layerTints.
    //   Potion — MC ItemTintSources' "minecraft:potion": the stack's
    //            PotionContents.getColorOr(default), made opaque.
    //   Dye    — MC "minecraft:dye" (Dye.calculate): the stack's DYED_COLOR
    //            made opaque, else the default (LEATHER_COLOR on leather).
    enum class ItemTintKind : uint8_t { Fixed = 0, Potion = 1, Dye = 2 };

    struct ClientItemDesc {
        ClientItemKind kind = ClientItemKind::Missing;
        // Bare leaf slug for the "rest" model — e.g. "torch", "oak_trapdoor_bottom".
        // For FlatSprite this names the item/block sprite under textures/{item,block}/.
        // For BlockModel this names the entry in BlockModelRegistry to render.
        std::string restSlug;
        // For animated items: leaf slugs for each frame in threshold order. Empty
        // when the item isn't animated. Each entry is a flat-sprite reference.
        std::vector<std::string> frameSlugs;
        // Range-dispatch property name (e.g. "minecraft:time", "minecraft:compass/lodestone").
        // Used by the registry to pick the matching frame-selector function. Empty when no
        // range_dispatch was found in the tree.
        std::string property;
        // For kind == Special: which BEWLR-style renderer the item asks for and which
        // texture variant to use. Populated from the nested `model` object of a
        // "minecraft:special" node, e.g. {"type":"minecraft:chest","texture":"minecraft:trapped"}
        // → specialKind="chest", specialTexture="trapped". Lets one renderer
        // (ChestItemRenderer) cover normal/trapped/ender/christmas chests.
        std::string specialKind;
        std::string specialTexture;
        // Per-layer tint colors (ARGB) from the rest model's `tints` array.
        // Index N applies to `layerN` of the model; missing entries render
        // untinted (white). Mirrors MC's ItemTintSource — for dye-tinted items
        // (leather armor, etc.) we read the `default` value of each tint. A
        // value of 0 means "no tint" and is rendered as white. See
        //   minecraft_code_26.1-snapshot-1/.../client/renderer/item/properties/select/...
        // and the leather_chestplate.json `tints` array (default -6265536 =
        // 0xFFA06540, the brown leather color).
        std::vector<uint32_t> layerTints;
        // Parallel to layerTints: which tint source each entry came from.
        std::vector<ItemTintKind> layerTintKinds;
        // For kind == Composite: the children, in order (the first supplies
        // the display transforms, as MC's CompositeModel takes them).
        std::vector<CompositeChild> compositeChildren;
    };

    class ClientItemLoader {
    public:
        // Load + describe `assets/items/{slug}.json`. Returns kind=Missing if the file
        // is absent or unparseable; otherwise kind matches the resolved rest model.
        static ClientItemDesc Load(const std::string& slug);
    };

} // namespace Game

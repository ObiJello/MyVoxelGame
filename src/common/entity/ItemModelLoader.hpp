// File: src/common/entity/ItemModelLoader.hpp
// Loads MC-format item model JSON from `assets/models/item/{slug}.json` and populates
// an Item's textures (single sprite or animated frames) from it.
//
// The format mirrors Minecraft's vanilla item models:
//   {
//     "parent": "minecraft:item/generated",
//     "textures": { "layer0": "minecraft:item/compass_00" },
//     "overrides": [
//       { "predicate": { "angle": 0.015625 }, "model": "minecraft:item/compass_01" },
//       ...
//     ]
//   }
//
// We resolve the `model` reference for each override by recursively loading the referenced
// model file and extracting its `textures.layer0`. The resulting list of textures is sorted
// by predicate value and stored as `Item.spriteFrames` (with the base layer0 as frame 0).
//
// New items: drop a JSON in `assets/models/item/`, register the ItemID + slug in
// ItemRegistry::Initialize, and the loader populates the textures automatically. No C++
// edits to the rendering side.
#pragma once

#include "Item.hpp"
#include <string>

namespace Game {

    class ItemModelLoader {
    public:
        // Resolve and load `assets/models/item/{slug}.json`, populating `item.spriteName`
        // (single layer0) and `item.spriteFrames` (override variants, ordered by predicate).
        // Returns true on success; false if the model file is missing or unparseable
        // (caller should keep any pre-existing static spriteName as a fallback).
        static bool LoadInto(Item& item, const std::string& slug);
    };

} // namespace Game

// File: src/client/renderer/entity/ItemDisplayTransforms.hpp
//
// MC ItemTransforms — an item model's `display` block, per ItemDisplay-
// Context, read from the model JSONs under assets/models (item/…, block/…)
// the way BlockModel's deserializer merges them: the model's own entry for
// the context, else its parent's, up the chain; `thirdperson_lefthand`
// falls back to `thirdperson_righthand` (ItemTransforms.Deserializer), every
// other missing context is ItemTransform.NO_TRANSFORM.
//
// What the entity layers need to place a held, worn or carried item exactly
// as MC's ItemStackRenderState.submit does: ItemInHandLayer's
// THIRD_PERSON_*_HAND, CustomHeadLayer's HEAD, the fox / dolphin / panda /
// villager layers' GROUND.
#pragma once

#include <glm/glm.hpp>

#include <string>
#include <string_view>

namespace Render::ItemDisplay {

    // MC ItemDisplayContext, the ones an entity layer submits with.
    enum class Context {
        ThirdPersonLeftHand,
        ThirdPersonRightHand,
        Head,
        Ground,
        Fixed,
        Gui,
        // The first-person hands (FirstPersonHandsAndItemsRenderer's
        // FIRST_PERSON_*_HAND) — the held spear's and trident's.
        FirstPersonLeftHand,
        FirstPersonRightHand,
    };

    inline bool IsLeftHand(Context c) {
        return c == Context::ThirdPersonLeftHand || c == Context::FirstPersonLeftHand;
    }
    inline bool IsHand(Context c) {
        return c == Context::ThirdPersonLeftHand || c == Context::ThirdPersonRightHand;
    }

    // MC ItemTransform: rotation in degrees, translation in SIXTEENTHS of a
    // block (as authored — MC's deserializer multiplies by 0.0625 and clamps
    // to ±80; Apply does the multiply), scale.
    struct Transform {
        glm::vec3 rotation{0.0f};
        glm::vec3 translation{0.0f};
        glm::vec3 scale{1.0f};
        bool      identity = true;   // ItemTransform.NO_TRANSFORM
    };

    // The context's transform for a model ("item/bow", "minecraft:item/bow",
    // "block/stone"). A missing model answers NO_TRANSFORM. Cached; the
    // cache follows resource-pack reloads.
    const Transform& Get(std::string_view modelPath, Context context);

    // Whether assets/models/<modelPath>.json exists (after pack overlays).
    bool ModelExists(std::string_view modelPath);

    // Whether the model's parent chain reaches builtin/generated — a flat
    // item (the extruded sprite), not a block model.
    bool IsGenerated(std::string_view modelPath);

    // The model's `textures.layerN` (up the parent chain) as a sprite name —
    // the texture path's last segment ("block/torch" → "torch"); "" when
    // it has none.
    std::string SpriteLayer(std::string_view modelPath, int layer);

    // MC ItemTransform.apply(applyLeftHandFix, pose), onto `pose` (block
    // units): translate (x negated for the left hand), rotationXYZ (y and z
    // negated for the left hand), scale, then the model's centring
    // translate(-0.5, -0.5, -0.5) — after which the item's geometry is in
    // its own [0, 1]³ model cell.
    glm::mat4 Apply(const glm::mat4& pose, const Transform& t, bool leftHand);

} // namespace Render::ItemDisplay

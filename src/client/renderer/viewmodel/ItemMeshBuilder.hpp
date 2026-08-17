// File: src/client/renderer/viewmodel/ItemMeshBuilder.hpp
//
// Builds renderable 3D geometry for a BLOCK item — the mesh you see when a
// block is held in the hand, or lying on the ground as a dropped item.
//
// This used to live in an anonymous namespace inside HeldItemRenderer.cpp. It
// moved here when ItemEntityRenderer needed exactly the same geometry: a
// dropped cobblestone and a held cobblestone are the same model at different
// scales, and duplicating a few hundred lines of element walking and atlas UV
// resolution would have guaranteed the two drifted apart.
//
// SPRITE items are NOT handled here — those go through HeldItemSpriteMesh,
// which already had a shared, cached public interface.
//
// Coordinates are block-local [0,1]³, so the caller applies its own scale and
// the model's `display.ground` / `display.firstperson_*` transform.
#pragma once

#include "../backend/RenderTypes.hpp"
#include "common/world/block/Blocks.hpp"

#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace Render {

    // Vertex format shared with the block shader: position, atlas UV, and a
    // per-vertex tint that the fragment shader multiplies into the sampled
    // texel (`texColor * vertColor`) for grass-top biome colouring.
    struct ItemCubeVert {
        float x, y, z;
        float u, v;
        uint8_t r, g, b, a;
    };
    static_assert(sizeof(ItemCubeVert) == 24, "ItemCubeVert must be 24 bytes");

    // Upper bound on emitted geometry. The busiest vanilla block models —
    // fences, brewing stands, lanterns, chiseled bookshelves — run to a few
    // dozen quads; 256 is comfortably above any of them, and the builders clamp
    // rather than overrun if something exceeds it.
    constexpr uint32_t kItemCubeMaxQuads = 256;
    constexpr uint32_t kItemCubeMaxVerts = kItemCubeMaxQuads * 4;
    constexpr uint32_t kItemCubeMaxIdx   = kItemCubeMaxQuads * 6;

    // Real block-model geometry: walks the model's ELEMENTS and emits each face
    // at its true extents, which is what the chunk mesher and the inventory
    // icon both do.
    //
    // Returns false when the model has no usable geometry (water/lava and the
    // BEWLR blocks have models with no elements at all), in which case the
    // caller should fall back to BuildBlockCubeMesh.
    bool BuildBlockModelMesh(Game::BlockID b, const std::string& modelOverride,
                             std::vector<ItemCubeVert>& verts,
                             std::vector<uint32_t>& idx);

    // Fallback: force the block into a 1×1×1 cube and take only its TEXTURES
    // from the model. Correct for a full cube and wrong for everything else — a
    // button would come out as a plank cube — so this is the last resort, not
    // the default path.
    void BuildBlockCubeMesh(Game::BlockID b,
                            std::vector<ItemCubeVert>& verts,
                            std::vector<uint32_t>& idx);

} // namespace Render

// File: src/client/renderer/viewmodel/HeldItemSpriteMesh.hpp
// Builds a 3D "voxelized" mesh from a 2D item sprite (the chunky
// extruded look first-person held tools/items have). Algorithm:
// front + back faces always emitted; one side quad is emitted at every
// alpha-boundary between adjacent pixels on each row/column. Cached
// per (texturePath, size) so we only walk the pixels once per item.
#pragma once

#include "../backend/RenderTypes.hpp"
#include <string>
#include <vector>
#include <unordered_map>

namespace Render {

    class HeldItemSpriteMesh {
    public:
        struct Entry {
            MeshHandle    mesh        = INVALID_MESH;
            BufferHandle  vertexBuffer = INVALID_BUFFER;
            BufferHandle  indexBuffer  = INVALID_BUFFER;
            uint32_t      indexCount  = 0;
            // Texture used to sample colours during rendering. Cached
            // here so the renderer doesn't have to re-resolve the
            // sprite name → backend texture handle mapping each frame.
            TextureHandle texture     = INVALID_TEXTURE;
        };

        // One vertex of the extruded sprite, in the local space the builder
        // authors: x,y in [0,16] with Y UP, z in [-0.5,+0.5] with the front
        // face at +z. Layout-identical to the block vertex, so it uploads
        // through GetBlockVertexLayout() unchanged.
        struct Vertex {
            float x, y, z;
            float u, v;
            uint8_t r, g, b, a;
        };

        // CPU-side geometry only — builds nothing on the GPU.
        //
        // GetOrBuild is the right entry point when the sprite is drawn on its
        // own with its own matrix. This one exists for callers that have to
        // transform the sprite on the CPU and merge it into a larger vertex
        // stream: MobRenderer bakes the bow in a skeleton's hand this way,
        // because the mob geometry is already CPU-transformed and world-space,
        // so a separately-matrixed draw could not be batched with it.
        //
        // Returns false when the sprite PNG cannot be loaded or produced no
        // opaque pixels; `verts`/`idx` are APPENDED to, not cleared.
        static bool BuildGeometry(const std::string& spriteName, uint32_t tintARGB,
                                  std::vector<Vertex>& verts,
                                  std::vector<uint32_t>& idx);

        // Look up (or build on first miss) the extruded mesh for the
        // given item sprite. Returns nullptr only when the sprite PNG
        // can't be loaded; callers should fall back to a flat textured
        // quad in that case so the held-item slot isn't blank.
        //
        // `tintARGB` is the item's layer-0 tint (Item::layerTints), baked into
        // the mesh's vertex colour; 0 means untinted. MC authors plant sprites
        // GREYSCALE and colours them through this tint, so a bush or fern
        // renders as a grey smear without it — which is exactly what the GUI
        // path already avoids by applying the same value.
        //
        // The tint is part of the CACHE KEY. Two items can share a sprite while
        // tinting it differently, and keying on the name alone would hand the
        // second one the first one's colour.
        static const Entry* GetOrBuild(const std::string& spriteName,
                                       uint32_t tintARGB = 0);

        // Release every cached mesh + buffer. Called from the renderer's
        // Shutdown so we don't leak GPU handles after backend tear-down.
        static void ClearCache();

    private:
        // Cache key is the sprite path passed to GetOrBuild, suffixed with the
        // tint when there is one. The bare name is still the key for untinted
        // items — the same key GuiGraphics::LoadItemTexture uses, so we keep
        // sharing decode work with the GUI item renderer.
        static std::unordered_map<std::string, Entry> s_cache;
    };

} // namespace Render

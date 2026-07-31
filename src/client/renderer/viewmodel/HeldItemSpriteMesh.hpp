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

        // Look up (or build on first miss) the extruded mesh for the
        // given item sprite. Returns nullptr only when the sprite PNG
        // can't be loaded; callers should fall back to a flat textured
        // quad in that case so the held-item slot isn't blank.
        static const Entry* GetOrBuild(const std::string& spriteName);

        // Release every cached mesh + buffer. Called from the renderer's
        // Shutdown so we don't leak GPU handles after backend tear-down.
        static void ClearCache();

    private:
        // Cache key is the sprite path passed to GetOrBuild — same key
        // GuiGraphics::LoadItemTexture uses, so we share work with the
        // GUI item renderer (sprites are decoded from disk once and the
        // texture handle is reused).
        static std::unordered_map<std::string, Entry> s_cache;
    };

} // namespace Render

// File: src/client/renderer/viewmodel/HeldItemSpriteMesh.cpp
#include "HeldItemSpriteMesh.hpp"

#include "../backend/RenderBackend.hpp"
#include "../core/Vertex.hpp"
#include "common/core/Log.hpp"

#include <stb_image.h>
#include <filesystem>
#include <cstring>

namespace PlatformMain { std::string GetAssetPath(const std::string&); }

namespace Render {

    std::unordered_map<std::string, HeldItemSpriteMesh::Entry> HeldItemSpriteMesh::s_cache;

    namespace {
        // Local-space convention used by the held-item mesh:
        //   x in [0, 16]   — pixel columns left-to-right
        //   y in [0, 16]   — pixel rows BOTTOM-to-top (image origin flipped)
        //   z in [-0.5, +0.5] — front face at +0.5, back face at -0.5
        // The display-context transform applied at draw time treats the
        // mesh as living in a 16-unit-wide cube whose front face faces +z,
        // which matches the cube the item-model JSONs use.
        constexpr float kPixelDepth = 1.0f;
        constexpr uint8_t kAlphaThreshold = 1;       // any non-zero alpha = solid

        struct V { float x,y,z; float u,v; uint8_t r,g,b,a; };
        static_assert(sizeof(V) == 24, "HeldItemSprite vertex must be 24 bytes");

        // Triangulate a quad given its 4 corner positions + 4 UVs. Two
        // triangles, winding chosen so the supplied corner order goes
        // counter-clockwise when viewed from the OUTSIDE of the face
        // (so backface culling keeps the visible side).
        void emitQuad(std::vector<V>& verts, std::vector<uint32_t>& idx,
                      const V& a, const V& b, const V& c, const V& d) {
            uint32_t base = (uint32_t)verts.size();
            verts.push_back(a);
            verts.push_back(b);
            verts.push_back(c);
            verts.push_back(d);
            idx.push_back(base + 0);
            idx.push_back(base + 1);
            idx.push_back(base + 2);
            idx.push_back(base + 0);
            idx.push_back(base + 2);
            idx.push_back(base + 3);
        }

        // PNG loader — mirrors GuiGraphics::LoadItemTexture's lookup
        // rules so callers can pass a sprite name like "diamond_sword"
        // and we'll find it under textures/item/ or textures/block/.
        // Returns the raw pixels, width, height (caller frees via
        // stbi_image_free) and the path we ended up loading from.
        struct PixelImage {
            unsigned char* pixels = nullptr;
            int w = 0;
            int h = 0;
        };
        PixelImage LoadSpritePixels(const std::string& spriteName) {
            PixelImage out;
            std::string path = PlatformMain::GetAssetPath(
                "assets/textures/item/" + spriteName + ".png");
            if (!std::filesystem::exists(path)) {
                std::string alt = PlatformMain::GetAssetPath(
                    "assets/textures/block/" + spriteName + ".png");
                if (std::filesystem::exists(alt)) {
                    path = std::move(alt);
                } else {
                    return out;
                }
            }
            int ch = 0;
            stbi_set_flip_vertically_on_load(0);
            out.pixels = stbi_load(path.c_str(), &out.w, &out.h, &ch, STBI_rgb_alpha);
            return out;
        }

        // Helper: returns the alpha of pixel (px, py) in row-major
        // RGBA image, treating out-of-bounds as transparent.
        inline uint8_t alphaAt(const unsigned char* pixels, int w, int h,
                               int px, int py) {
            if (px < 0 || py < 0 || px >= w || py >= h) return 0;
            return pixels[(py * w + px) * 4 + 3];
        }
    } // namespace

    const HeldItemSpriteMesh::Entry* HeldItemSpriteMesh::GetOrBuild(
        const std::string& spriteName)
    {
        auto it = s_cache.find(spriteName);
        if (it != s_cache.end()) {
            // Negative cache: empty entries indicate a previous load
            // failure — don't keep retrying every frame.
            return it->second.mesh == INVALID_MESH ? nullptr : &it->second;
        }
        Entry& e = s_cache[spriteName];

        if (!g_renderBackend) return nullptr;

        PixelImage img = LoadSpritePixels(spriteName);
        if (!img.pixels) {
            Log::Warning("[HeldItemSpriteMesh] failed to load '%s'", spriteName.c_str());
            return nullptr;
        }

        // ── Build the extrusion mesh. We walk every pixel once;
        // for each opaque pixel we conditionally emit:
        //   • front quad   — always
        //   • back  quad   — always (winding flipped so cull works)
        //   • left/right/top/bottom side quad — only when the neighbour
        //     in that direction is transparent (this is the alpha-edge
        //     test that produces MC's chunky "voxelised sprite" look —
        //     interior opaque pixels contribute no side faces).
        // Coordinates go 0..16 across both axes regardless of the actual
        // texture resolution; UV per-pixel is computed so we sample the
        // exact pixel centre and avoid bleeding across pixel borders.
        std::vector<V> verts;
        std::vector<uint32_t> idx;
        verts.reserve(img.w * img.h * 6);     // upper-bound guess
        idx.reserve(img.w * img.h * 12);

        const float gridStepX = 16.0f / (float)img.w;
        const float gridStepY = 16.0f / (float)img.h;
        const float uStep = 1.0f / (float)img.w;
        const float vStep = 1.0f / (float)img.h;
        const float zFront = +kPixelDepth * 0.5f;
        const float zBack  = -kPixelDepth * 0.5f;
        const uint8_t W = 255;   // vertex colour = white (texture supplies real colour)

        for (int py = 0; py < img.h; ++py) {
            for (int px = 0; px < img.w; ++px) {
                if (alphaAt(img.pixels, img.w, img.h, px, py) < kAlphaThreshold) continue;

                // Image origin is TOP-LEFT (stbi convention, no flip);
                // we want world Y to increase UP, so flip the row index.
                const int    yFlipped = (img.h - 1) - py;
                const float x0 = (float)px       * gridStepX;
                const float x1 = (float)(px + 1) * gridStepX;
                const float y0 = (float)yFlipped * gridStepY;
                const float y1 = (float)(yFlipped + 1) * gridStepY;

                const float u0 = (float)px       * uStep;
                const float u1 = (float)(px + 1) * uStep;
                // Atlas Y is also top-down; UV.v for row py covers
                // (py..py+1) of the texture, no further flipping needed.
                const float v0 = (float)py       * vStep;
                const float v1 = (float)(py + 1) * vStep;

                // FRONT face (normal = +Z). CCW when viewed from +Z:
                //   bottom-left → bottom-right → top-right → top-left
                emitQuad(verts, idx,
                    {x0, y0, zFront, u0, v1, W,W,W,W},
                    {x1, y0, zFront, u1, v1, W,W,W,W},
                    {x1, y1, zFront, u1, v0, W,W,W,W},
                    {x0, y1, zFront, u0, v0, W,W,W,W});

                // BACK face (normal = -Z). CCW when viewed from -Z is
                // the reverse winding of the front. UVs mirrored on U so
                // text/details read correctly when seen from behind.
                emitQuad(verts, idx,
                    {x1, y0, zBack, u0, v1, W,W,W,W},
                    {x0, y0, zBack, u1, v1, W,W,W,W},
                    {x0, y1, zBack, u1, v0, W,W,W,W},
                    {x1, y1, zBack, u0, v0, W,W,W,W});

                // SIDE faces — only where neighbour is transparent.
                // Each side quad uses the SAME UV column/row as the
                // pixel it borders, sampled across the 1-pixel depth
                // so the side colour matches the edge pixel of the
                // sprite. CCW chosen so the outward normal faces the
                // transparent side.

                // Right side (neighbour px+1 transparent → normal +X)
                if (alphaAt(img.pixels, img.w, img.h, px + 1, py) < kAlphaThreshold) {
                    emitQuad(verts, idx,
                        {x1, y0, zFront, u1, v1, W,W,W,W},
                        {x1, y0, zBack,  u1, v1, W,W,W,W},
                        {x1, y1, zBack,  u1, v0, W,W,W,W},
                        {x1, y1, zFront, u1, v0, W,W,W,W});
                }
                // Left side (neighbour px-1 transparent → normal -X)
                if (alphaAt(img.pixels, img.w, img.h, px - 1, py) < kAlphaThreshold) {
                    emitQuad(verts, idx,
                        {x0, y0, zBack,  u0, v1, W,W,W,W},
                        {x0, y0, zFront, u0, v1, W,W,W,W},
                        {x0, y1, zFront, u0, v0, W,W,W,W},
                        {x0, y1, zBack,  u0, v0, W,W,W,W});
                }
                // Top side (neighbour py-1 in image = above on screen
                // since image is top-down; +Y in world-flipped frame).
                if (alphaAt(img.pixels, img.w, img.h, px, py - 1) < kAlphaThreshold) {
                    emitQuad(verts, idx,
                        {x0, y1, zFront, u0, v0, W,W,W,W},
                        {x1, y1, zFront, u1, v0, W,W,W,W},
                        {x1, y1, zBack,  u1, v0, W,W,W,W},
                        {x0, y1, zBack,  u0, v0, W,W,W,W});
                }
                // Bottom side (neighbour py+1 in image = below; -Y).
                if (alphaAt(img.pixels, img.w, img.h, px, py + 1) < kAlphaThreshold) {
                    emitQuad(verts, idx,
                        {x0, y0, zBack,  u0, v1, W,W,W,W},
                        {x1, y0, zBack,  u1, v1, W,W,W,W},
                        {x1, y0, zFront, u1, v1, W,W,W,W},
                        {x0, y0, zFront, u0, v1, W,W,W,W});
                }
            }
        }

        // Done with pixels.
        stbi_image_free(img.pixels);

        if (verts.empty() || idx.empty()) {
            Log::Warning("[HeldItemSpriteMesh] '%s' produced an empty mesh "
                         "(fully transparent texture?)", spriteName.c_str());
            return nullptr;
        }

        // Upload to GPU. We use BufferAccess::Static — the mesh is
        // immutable once built; the renderer just rebinds it per frame.
        e.vertexBuffer = g_renderBackend->CreateBuffer(
            BufferUsage::Vertex, verts.size() * sizeof(V),
            verts.data(), BufferAccess::Static);
        e.indexBuffer = g_renderBackend->CreateBuffer(
            BufferUsage::Index, idx.size() * sizeof(uint32_t),
            idx.data(), BufferAccess::Static);
        if (e.vertexBuffer == INVALID_BUFFER || e.indexBuffer == INVALID_BUFFER) {
            Log::Error("[HeldItemSpriteMesh] GPU buffer creation failed for '%s'",
                       spriteName.c_str());
            return nullptr;
        }
        e.mesh = g_renderBackend->CreateMesh(
            e.vertexBuffer, e.indexBuffer, GetBlockVertexLayout());
        e.indexCount = (uint32_t)idx.size();

        // Reload + upload the same sprite as a sampler-ready texture
        // (the GuiGraphics LoadItemTexture cache does this already, but
        // calling into it from this TU would pull the whole GUI header
        // chain; cheaper to re-stb_load and stash our own handle).
        PixelImage texImg = LoadSpritePixels(spriteName);
        if (texImg.pixels) {
            e.texture = g_renderBackend->CreateTexture2D(
                texImg.w, texImg.h, TextureFormat::RGBA8, texImg.pixels);
            if (e.texture != INVALID_TEXTURE) {
                g_renderBackend->SetTextureFilter(e.texture,
                    TextureFilter::Nearest, TextureFilter::Nearest);
                g_renderBackend->SetTextureWrap(e.texture,
                    TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            }
            stbi_image_free(texImg.pixels);
        }
        return &e;
    }

    void HeldItemSpriteMesh::ClearCache() {
        if (!g_renderBackend) {
            s_cache.clear();
            return;
        }
        for (auto& [_, e] : s_cache) {
            if (e.mesh != INVALID_MESH)         g_renderBackend->DestroyMesh(e.mesh);
            if (e.vertexBuffer != INVALID_BUFFER) g_renderBackend->DestroyBuffer(e.vertexBuffer);
            if (e.indexBuffer  != INVALID_BUFFER) g_renderBackend->DestroyBuffer(e.indexBuffer);
            if (e.texture != INVALID_TEXTURE)   g_renderBackend->DestroyTexture(e.texture);
        }
        s_cache.clear();
    }

} // namespace Render

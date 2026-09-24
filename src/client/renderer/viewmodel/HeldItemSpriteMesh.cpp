// File: src/client/renderer/viewmodel/HeldItemSpriteMesh.cpp
#include "HeldItemSpriteMesh.hpp"

#include "../backend/RenderBackend.hpp"
#include "../core/DiffuseLighting.hpp"
#include "../core/Vertex.hpp"
#include "common/core/Log.hpp"

#include <stb_image.h>
#include <algorithm>
#include <array>
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

        using V = HeldItemSpriteMesh::Vertex;
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

    bool HeldItemSpriteMesh::BuildGeometry(const std::string& spriteName, uint32_t tintARGB,
                                           std::vector<Vertex>& verts,
                                           std::vector<uint32_t>& idx)
    {
        PixelImage img = LoadSpritePixels(spriteName);
        if (!img.pixels) {
            Log::Warning("[HeldItemSpriteMesh] failed to load '%s'", spriteName.c_str());
            return false;
        }
        const bool ok = BuildGeometryFromPixels(img.pixels, img.w, img.h, tintARGB,
                                                spriteName, verts, idx);
        stbi_image_free(img.pixels);
        return ok;
    }

    bool HeldItemSpriteMesh::BuildGeometryFromPixels(const unsigned char* pixels, int w, int h,
                                                     uint32_t tintARGB, const std::string& label,
                                                     std::vector<Vertex>& verts,
                                                     std::vector<uint32_t>& idx)
    {
        const PixelImage img{const_cast<unsigned char*>(pixels), w, h};
        const std::string& spriteName = label;
        const size_t firstVert = verts.size();

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
        verts.reserve(verts.size() + static_cast<size_t>(img.w) * img.h * 6);
        idx.reserve(idx.size() + static_cast<size_t>(img.w) * img.h * 12);

        const float gridStepX = 16.0f / (float)img.w;
        const float gridStepY = 16.0f / (float)img.h;
        const float uStep = 1.0f / (float)img.w;
        const float vStep = 1.0f / (float)img.h;
        const float zFront = +kPixelDepth * 0.5f;
        const float zBack  = -kPixelDepth * 0.5f;
        // Vertex colour: white when untinted (the texture supplies the real
        // colour), otherwise the item's tint. MC authors plant sprites
        // greyscale and colours them through this, so skipping it renders a
        // bush or fern as a grey smear.
        const uint8_t tintR = tintARGB ? static_cast<uint8_t>((tintARGB >> 16) & 0xFF) : 255;
        const uint8_t tintG = tintARGB ? static_cast<uint8_t>((tintARGB >>  8) & 0xFF) : 255;
        const uint8_t tintB = tintARGB ? static_cast<uint8_t>( tintARGB        & 0xFF) : 255;

        // ── Diffuse shading (MC light.glsl minecraft_mix_light) ────────────
        //
        // Without this the extruded sprite is one flat colour from every angle
        // — front, back, the 1px sides and the top and bottom edges all read
        // identically, so a stick lying on the ground looks like a sticker
        // rather than a solid. MC lights items with two fixed directional
        // lights (Render::kLevelDiffuse0/1); every quad here is axis-aligned,
        // so the six results are constants. See DiffuseLighting.hpp for the
        // one place this diverges (the light is fixed to the model, not the
        // world, so a spinning item does not shimmer).
        const auto shaded = [&](float shade) {
            uint8_t r = tintR, g = tintG, b = tintB;
            ApplyShade(shade, r, g, b);
            return std::array<uint8_t, 3>{ r, g, b };
        };
        const auto faceZ  = shaded(DiffuseShadeZ());      // front / back
        const auto faceX  = shaded(DiffuseShadeX());      // left / right edges
        const auto faceUp = shaded(DiffuseShadeUp());     // top edge
        const auto faceDn = shaded(DiffuseShadeDown());   // bottom edge

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
                    {x0, y0, zFront, u0, v1, faceZ[0],faceZ[1],faceZ[2],255},
                    {x1, y0, zFront, u1, v1, faceZ[0],faceZ[1],faceZ[2],255},
                    {x1, y1, zFront, u1, v0, faceZ[0],faceZ[1],faceZ[2],255},
                    {x0, y1, zFront, u0, v0, faceZ[0],faceZ[1],faceZ[2],255});

                // BACK face (normal = -Z). CCW when viewed from -Z is
                // the reverse winding of the front. UVs mirrored on U so
                // text/details read correctly when seen from behind.
                emitQuad(verts, idx,
                    {x1, y0, zBack, u0, v1, faceZ[0],faceZ[1],faceZ[2],255},
                    {x0, y0, zBack, u1, v1, faceZ[0],faceZ[1],faceZ[2],255},
                    {x0, y1, zBack, u1, v0, faceZ[0],faceZ[1],faceZ[2],255},
                    {x1, y1, zBack, u0, v0, faceZ[0],faceZ[1],faceZ[2],255});

                // SIDE faces — only where neighbour is transparent.
                // Each side quad uses the SAME UV column/row as the
                // pixel it borders, sampled across the 1-pixel depth
                // so the side colour matches the edge pixel of the
                // sprite. CCW chosen so the outward normal faces the
                // transparent side.
                //
                // Sample the pixel CENTRE, not its edge. A side quad
                // collapses U (or V) to one value for all four corners,
                // so unlike the front/back faces there is no
                // interpolation to land it inside the texel — the shared
                // edge u1 = (px+1)/w resolves under nearest filtering to
                // texel px+1, which is the TRANSPARENT neighbour that
                // caused this side to be emitted in the first place, and
                // the alpha test then discards the whole quad. That
                // silently dropped every right-hand and bottom side face;
                // left and top only worked because their edge (u0 / v0)
                // rounds back into the opaque pixel.
                const float uMid = ((float)px + 0.5f) * uStep;
                const float vMid = ((float)py + 0.5f) * vStep;

                // Right side (neighbour px+1 transparent → normal +X)
                if (alphaAt(img.pixels, img.w, img.h, px + 1, py) < kAlphaThreshold) {
                    emitQuad(verts, idx,
                        {x1, y0, zFront, uMid, v1, faceX[0],faceX[1],faceX[2],255},
                        {x1, y0, zBack,  uMid, v1, faceX[0],faceX[1],faceX[2],255},
                        {x1, y1, zBack,  uMid, v0, faceX[0],faceX[1],faceX[2],255},
                        {x1, y1, zFront, uMid, v0, faceX[0],faceX[1],faceX[2],255});
                }
                // Left side (neighbour px-1 transparent → normal -X)
                if (alphaAt(img.pixels, img.w, img.h, px - 1, py) < kAlphaThreshold) {
                    emitQuad(verts, idx,
                        {x0, y0, zBack,  uMid, v1, faceX[0],faceX[1],faceX[2],255},
                        {x0, y0, zFront, uMid, v1, faceX[0],faceX[1],faceX[2],255},
                        {x0, y1, zFront, uMid, v0, faceX[0],faceX[1],faceX[2],255},
                        {x0, y1, zBack,  uMid, v0, faceX[0],faceX[1],faceX[2],255});
                }
                // Top side (neighbour py-1 in image = above on screen
                // since image is top-down; +Y in world-flipped frame).
                if (alphaAt(img.pixels, img.w, img.h, px, py - 1) < kAlphaThreshold) {
                    emitQuad(verts, idx,
                        {x0, y1, zFront, u0, vMid, faceUp[0],faceUp[1],faceUp[2],255},
                        {x1, y1, zFront, u1, vMid, faceUp[0],faceUp[1],faceUp[2],255},
                        {x1, y1, zBack,  u1, vMid, faceUp[0],faceUp[1],faceUp[2],255},
                        {x0, y1, zBack,  u0, vMid, faceUp[0],faceUp[1],faceUp[2],255});
                }
                // Bottom side (neighbour py+1 in image = below; -Y).
                if (alphaAt(img.pixels, img.w, img.h, px, py + 1) < kAlphaThreshold) {
                    emitQuad(verts, idx,
                        {x0, y0, zBack,  u0, vMid, faceDn[0],faceDn[1],faceDn[2],255},
                        {x1, y0, zBack,  u1, vMid, faceDn[0],faceDn[1],faceDn[2],255},
                        {x1, y0, zFront, u1, vMid, faceDn[0],faceDn[1],faceDn[2],255},
                        {x0, y0, zFront, u0, vMid, faceDn[0],faceDn[1],faceDn[2],255});
                }
            }
        }

        if (verts.size() == firstVert) {
            Log::Warning("[HeldItemSpriteMesh] '%s' produced an empty mesh "
                         "(fully transparent texture?)", spriteName.c_str());
            return false;
        }
        return true;
    }

    const HeldItemSpriteMesh::Entry* HeldItemSpriteMesh::GetOrBuild(
        const std::string& spriteName, uint32_t tintARGB)
    {
        // Untinted items keep the bare sprite name as their key so they still
        // share cache entries with everything that looks them up that way.
        const std::string key = tintARGB == 0
            ? spriteName
            : spriteName + "#" + std::to_string(tintARGB);

        auto it = s_cache.find(key);
        if (it != s_cache.end()) {
            // Negative cache: empty entries indicate a previous load
            // failure — don't keep retrying every frame.
            return it->second.mesh == INVALID_MESH ? nullptr : &it->second;
        }
        Entry& e = s_cache[key];

        if (!g_renderBackend) return nullptr;

        std::vector<V> verts;
        std::vector<uint32_t> idx;
        if (!BuildGeometry(spriteName, tintARGB, verts, idx)) return nullptr;

        // Reload the same sprite as a sampler-ready texture (the GuiGraphics
        // LoadItemTexture cache does this already, but calling into it from
        // this TU would pull the whole GUI header chain; cheaper to
        // re-stb_load and stash our own handle).
        PixelImage texImg = LoadSpritePixels(spriteName);
        const bool ok = Upload(e, verts, idx, texImg.pixels, texImg.w, texImg.h, spriteName);
        if (texImg.pixels) stbi_image_free(texImg.pixels);
        return ok ? &e : nullptr;
    }

    bool HeldItemSpriteMesh::Upload(Entry& e, const std::vector<Vertex>& verts,
                                    const std::vector<uint32_t>& idx,
                                    const unsigned char* texPixels, int texW, int texH,
                                    const std::string& label) {
        // BufferAccess::Static — the mesh is immutable once built; the
        // renderer just rebinds it per frame.
        e.vertexBuffer = g_renderBackend->CreateBuffer(
            BufferUsage::Vertex, verts.size() * sizeof(V),
            verts.data(), BufferAccess::Static);
        e.indexBuffer = g_renderBackend->CreateBuffer(
            BufferUsage::Index, idx.size() * sizeof(uint32_t),
            idx.data(), BufferAccess::Static);
        if (e.vertexBuffer == INVALID_BUFFER || e.indexBuffer == INVALID_BUFFER) {
            Log::Error("[HeldItemSpriteMesh] GPU buffer creation failed for '%s'",
                       label.c_str());
            return false;
        }
        e.mesh = g_renderBackend->CreateMesh(
            e.vertexBuffer, e.indexBuffer, GetBlockVertexLayout());
        e.indexCount = (uint32_t)idx.size();

        if (texPixels) {
            e.texture = g_renderBackend->CreateTexture2D(
                texW, texH, TextureFormat::RGBA8, texPixels);
            if (e.texture != INVALID_TEXTURE) {
                g_renderBackend->SetTextureFilter(e.texture,
                    TextureFilter::Nearest, TextureFilter::Nearest);
                g_renderBackend->SetTextureWrap(e.texture,
                    TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            }
        }
        return true;
    }

    const HeldItemSpriteMesh::Entry* HeldItemSpriteMesh::GetOrBuildForStack(
        const Game::ItemStack& stack)
    {
        const Game::Item& item = Game::ItemRegistry::Get(stack.itemId);
        if (item.spriteLayers.size() <= 1) {
            std::string name = item.spriteName;
            if (name.empty() && !item.spriteFrames.empty()) name = item.spriteFrames[0];
            if (name.empty() && !item.spriteLayers.empty()) name = item.spriteLayers[0];
            if (name.empty()) return nullptr;
            return GetOrBuild(name, Game::ResolveItemLayerTint(stack, 0));
        }

        // One key per (layer, tint) sequence: two potions share a mesh only
        // when their colours agree.
        std::string key = "layers";
        std::vector<uint32_t> tints;
        for (size_t i = 0; i < item.spriteLayers.size(); ++i) {
            const uint32_t tint = Game::ResolveItemLayerTint(stack, i);
            tints.push_back(tint);
            key += "|" + item.spriteLayers[i] + "#" + std::to_string(tint);
        }
        auto it = s_cache.find(key);
        if (it != s_cache.end()) return it->second.mesh == INVALID_MESH ? nullptr : &it->second;
        Entry& e = s_cache[key];
        if (!g_renderBackend) return nullptr;

        // Composite over the first layer's grid: each layer's texel times its
        // tint (RGB; untinted = white), alpha-over the layers below. A layer
        // of another resolution samples nearest onto that grid.
        int w = 0, h = 0;
        std::vector<unsigned char> out;
        for (size_t i = 0; i < item.spriteLayers.size(); ++i) {
            PixelImage img = LoadSpritePixels(item.spriteLayers[i]);
            if (!img.pixels) continue;
            if (out.empty()) {
                w = img.w; h = img.h;
                out.assign(static_cast<size_t>(w) * h * 4, 0);
            }
            const float tr = tints[i] ? ((tints[i] >> 16) & 0xFF) / 255.0f : 1.0f;
            const float tg = tints[i] ? ((tints[i] >>  8) & 0xFF) / 255.0f : 1.0f;
            const float tb = tints[i] ? ( tints[i]        & 0xFF) / 255.0f : 1.0f;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const int sx = x * img.w / w, sy = y * img.h / h;
                    const unsigned char* src = img.pixels + (static_cast<size_t>(sy) * img.w + sx) * 4;
                    unsigned char* dst = out.data() + (static_cast<size_t>(y) * w + x) * 4;
                    const float sa = src[3] / 255.0f;
                    if (sa <= 0.0f) continue;
                    const float da = dst[3] / 255.0f;
                    const float oa = sa + da * (1.0f - sa);
                    const float c[3] = { src[0] * tr, src[1] * tg, src[2] * tb };
                    for (int k = 0; k < 3; ++k) {
                        const float v = (c[k] * sa + dst[k] * da * (1.0f - sa)) / oa;
                        dst[k] = static_cast<unsigned char>(std::min(255.0f, v + 0.5f));
                    }
                    dst[3] = static_cast<unsigned char>(std::min(255.0f, oa * 255.0f + 0.5f));
                }
            }
            stbi_image_free(img.pixels);
        }
        if (out.empty()) return nullptr;

        // The tints are in the texture now; the mesh itself is untinted.
        std::vector<V> verts;
        std::vector<uint32_t> idx;
        if (!BuildGeometryFromPixels(out.data(), w, h, 0, key, verts, idx)) return nullptr;
        return Upload(e, verts, idx, out.data(), w, h, key) ? &e : nullptr;
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

// File: src/client/renderer/blockentity/BEModelMesh.hpp
//
// Small shared kit for the block-entity renderers that draw MC ModelPart
// geometry (banner, bell, copper golem statue): a ModelPart.Cube with MC's
// exact face / UV layout, placed by a pose matrix, shaded by MC's world-space
// entity lighting (EntityLighting) through a caller-given normal matrix, and
// uploaded as one mesh holding several lighting copies back to back (the
// renderer draws the copy for the orientation and light set it needs —
// ShulkerBoxRenderer's scheme). Plus a per-renderer texture cache.
#pragma once

#include "client/resource/ResourcePacks.hpp"
#include "../backend/RenderBackend.hpp"
#include "../backend/RenderTypes.hpp"
#include "../entity/EntityLighting.hpp"
#include "common/core/Log.hpp"

#include "stb_image.h"
#include <glm/glm.hpp>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render::BEModel {

    struct Vert {
        float x, y, z;
        float u, v;
        uint8_t r, g, b, a;
    };
    static_assert(sizeof(Vert) == 24, "match GetBlockVertexLayout");

    // ModelPart.Cube's visibleFaces, Direction order.
    enum FaceBit : uint8_t {
        kDown = 1, kUp = 2, kNorth = 4, kSouth = 8, kWest = 16, kEast = 32, kAll = 63
    };

    // One cube, MC pixel units: `from`/`to` are the box corners already grown
    // by its CubeDeformation (the UVs follow the ungrown w/h/d, as in MC).
    // Every corner goes through `pose` (pixels → the mesh's space); the shade
    // is EntityLighting's for (normalMat · model normal) in `set`. `mirror`
    // is CubeListBuilder.mirror (the x extents swap, windings reverse).
    inline void AddCube(std::vector<Vert>& verts, std::vector<uint32_t>& idx,
                        glm::vec3 from, glm::vec3 to,
                        float xTexOffs, float yTexOffs, float w, float h, float d,
                        float texW, float texH, uint8_t faces,
                        const glm::mat4& pose, const glm::mat3& normalMat,
                        EntityLighting::LightSet set, bool mirror = false) {
            if (mirror) std::swap(from.x, to.x);
            const float minX = from.x, minY = from.y, minZ = from.z;
            const float maxX = to.x,   maxY = to.y,   maxZ = to.z;
            const glm::vec3 t0(minX, minY, minZ), t1(maxX, minY, minZ);
            const glm::vec3 t2(maxX, maxY, minZ), t3(minX, maxY, minZ);
            const glm::vec3 l0(minX, minY, maxZ), l1(maxX, minY, maxZ);
            const glm::vec3 l2(maxX, maxY, maxZ), l3(minX, maxY, maxZ);

            const float u0 = xTexOffs;
            const float u1 = xTexOffs + d;
            const float u2 = xTexOffs + d + w;
            const float u22= xTexOffs + d + w + w;
            const float u3 = xTexOffs + d + w + d;
            const float u4 = xTexOffs + d + w + d + w;
            const float v0 = yTexOffs;
            const float v1 = yTexOffs + d;
            const float v2 = yTexOffs + d + h;

            auto shade = [&](glm::vec3 n) -> uint8_t {
                if (mirror) n.x = -n.x;
                return EntityLighting::ShadeByte(normalMat * n, set);
            };
            auto emit = [&](const glm::vec3 q[4], float U0, float V0, float U1, float V1, uint8_t sh) {
                const uint32_t base = static_cast<uint32_t>(verts.size());
                const float uv[4][2] = { {U1, V0}, {U0, V0}, {U0, V1}, {U1, V1} };
                for (int i = 0; i < 4; ++i) {
                    const glm::vec4 p = pose * glm::vec4(q[i], 1.0f);
                    verts.push_back({p.x, p.y, p.z, uv[i][0] / texW, uv[i][1] / texH, sh, sh, sh, 255});
                }
                if (mirror) {
                    idx.push_back(base + 0); idx.push_back(base + 2); idx.push_back(base + 1);
                    idx.push_back(base + 0); idx.push_back(base + 3); idx.push_back(base + 2);
                } else {
                    idx.push_back(base + 0); idx.push_back(base + 1); idx.push_back(base + 2);
                    idx.push_back(base + 0); idx.push_back(base + 2); idx.push_back(base + 3);
                }
            };
            const glm::vec3 fDown [4] = {l1, l0, t0, t1};
            const glm::vec3 fUp   [4] = {t2, t3, l3, l2};
            const glm::vec3 fNorth[4] = {t1, t0, t3, t2};
            const glm::vec3 fSouth[4] = {l0, l1, l2, l3};
            const glm::vec3 fWest [4] = {t0, l0, l3, t3};
            const glm::vec3 fEast [4] = {l1, t1, t2, l2};
            // A mirrored cube keeps each polygon's direction for the
            // visibleFaces test (ModelPart.Cube); only its winding and
            // normal's x flip (Polygon's mirror).
            const uint8_t westBit = kWest;
            const uint8_t eastBit = kEast;
            if (faces & kDown)  emit(fDown,  u1, v0, u2,  v1, shade({ 0, -1,  0}));
            if (faces & kUp)    emit(fUp,    u2, v1, u22, v0, shade({ 0,  1,  0}));
            if (faces & kNorth) emit(fNorth, u1, v1, u2,  v2, shade({ 0,  0, -1}));
            if (faces & kSouth) emit(fSouth, u3, v1, u4,  v2, shade({ 0,  0,  1}));
            if (faces & westBit) emit(fWest, u0, v1, u1,  v2, shade({-1,  0,  0}));
            if (faces & eastBit) emit(fEast, u2, v1, u3,  v2, shade({ 1,  0,  0}));
    }

    // ModelPart.translateAndRotate: translate(offset / 16), rotationZYX, then
    // scale — the part's pose in block units, for pixel-unit children.
    inline glm::mat4 PartPose(const glm::mat4& parent, glm::vec3 offset, glm::vec3 rot = glm::vec3(0.0f)) {
        glm::mat4 m = glm::translate(parent, offset / 16.0f);
        if (rot.z != 0.0f) m = glm::rotate(m, rot.z, glm::vec3(0, 0, 1));
        if (rot.y != 0.0f) m = glm::rotate(m, rot.y, glm::vec3(0, 1, 0));
        if (rot.x != 0.0f) m = glm::rotate(m, rot.x, glm::vec3(1, 0, 0));
        return m;
    }
    // A part pose's cube space: pixels → the part's frame.
    inline glm::mat4 CubeSpace(const glm::mat4& partPose) {
        return glm::scale(partPose, glm::vec3(1.0f / 16.0f));
    }

    struct Mesh {
        MeshHandle   mesh = INVALID_MESH;
        BufferHandle vb   = INVALID_BUFFER;
        BufferHandle ib   = INVALID_BUFFER;
        uint32_t     indexCount = 0;   // per copy
        uint32_t     copies = 1;
        bool Valid() const { return mesh != INVALID_MESH && indexCount > 0; }
    };

    // Upload `copies` equal-sized copies laid back to back.
    inline Mesh Upload(const std::vector<Vert>& verts, const std::vector<uint32_t>& idx, uint32_t copies) {
        Mesh m;
        if (!g_renderBackend || verts.empty() || idx.empty() || copies == 0) return m;
        m.vb = g_renderBackend->CreateBuffer(BufferUsage::Vertex, verts.size() * sizeof(Vert), verts.data());
        m.ib = g_renderBackend->CreateBuffer(BufferUsage::Index, idx.size() * sizeof(uint32_t), idx.data());
        m.mesh = g_renderBackend->CreateMesh(m.vb, m.ib, GetBlockVertexLayout());
        m.copies = copies;
        m.indexCount = static_cast<uint32_t>(idx.size() / copies);
        return m;
    }

    inline void Destroy(Mesh& m) {
        if (!g_renderBackend) return;
        if (m.mesh != INVALID_MESH) g_renderBackend->DestroyMesh(m.mesh);
        if (m.vb != INVALID_BUFFER) g_renderBackend->DestroyBuffer(m.vb);
        if (m.ib != INVALID_BUFFER) g_renderBackend->DestroyBuffer(m.ib);
        m = Mesh{};
    }

    inline void Draw(const Mesh& m, uint32_t copy) {
        if (!m.Valid() || copy >= m.copies) return;
        g_renderBackend->DrawIndexed(m.mesh, m.indexCount, m.indexCount * copy);
    }

    // An entity texture (assets/textures/entity/<rel>.png), cached; a pack
    // change drops the cache (Resources::CacheStale).
    class TextureCache {
    public:
        TextureHandle Get(const std::string& rel) {
            if (Resources::CacheStale(m_generation)) Clear();
            if (auto it = m_cache.find(rel); it != m_cache.end()) return it->second;
            TextureHandle tex = INVALID_TEXTURE;
            const std::string full = PlatformMain::GetAssetPath("assets/textures/entity/" + rel + ".png");
            if (g_renderBackend && std::filesystem::exists(full)) {
                int w = 0, h = 0, ch = 0;
                stbi_set_flip_vertically_on_load(0);
                if (unsigned char* px = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha)) {
                    tex = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, px);
                    stbi_image_free(px);
                    if (tex != INVALID_TEXTURE) {
                        g_renderBackend->SetTextureFilter(tex, TextureFilter::Nearest, TextureFilter::Nearest);
                        g_renderBackend->SetTextureWrap(tex, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
                    }
                }
            } else {
                Log::Warning("[BlockEntityRenderer] missing texture %s", full.c_str());
            }
            m_cache[rel] = tex;
            return tex;
        }
        void Clear() {
            if (g_renderBackend) {
                for (auto& [k, t] : m_cache) if (t != INVALID_TEXTURE) g_renderBackend->DestroyTexture(t);
            }
            m_cache.clear();
        }
    private:
        std::unordered_map<std::string, TextureHandle> m_cache;
        int m_generation = -1;
    };

} // namespace Render::BEModel

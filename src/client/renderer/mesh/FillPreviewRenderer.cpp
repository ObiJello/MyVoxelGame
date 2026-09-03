// File: src/client/renderer/mesh/FillPreviewRenderer.cpp
#include "FillPreviewRenderer.hpp"

#include "ChunkRenderer.hpp"
#include "../backend/RenderBackend.hpp"
#include "../backend/vulkan/VKBackend.hpp"
#include "../environment/EnvironmentState.hpp"
#include "../texture/AtlasBuilder.hpp"
#include "../viewmodel/ItemMeshBuilder.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace Render {

    FillPreviewRenderer g_fillPreviewRenderer;

    namespace {
        // Vertices a frame's mesh may hold: 65k quads — a 128×128 floor's
        // top and bottom with room over.
        constexpr uint32_t kMaxQuads = 65536;
        constexpr uint32_t kMaxVerts = kMaxQuads * 4;
        constexpr uint32_t kMaxIdx   = kMaxQuads * 6;
        // How see-through the preview is, and its cast.
        constexpr uint8_t   kAlpha = 185;
        const     glm::vec4 kTint(0.55f, 0.72f, 1.0f, 0.25f);

        // The axis a quad faces, from its first triangle: (axis, sign), or
        // axis −1 when the quad is not on a cell boundary along that axis.
        void FaceOf(const ItemCubeVert* v, const uint32_t* tri, int& axis, int& sign) {
            const glm::vec3 p0(v[tri[0]].x, v[tri[0]].y, v[tri[0]].z);
            const glm::vec3 p1(v[tri[1]].x, v[tri[1]].y, v[tri[1]].z);
            const glm::vec3 p2(v[tri[2]].x, v[tri[2]].y, v[tri[2]].z);
            const glm::vec3 n = glm::cross(p1 - p0, p2 - p0);
            axis = 0;
            if (std::abs(n.y) > std::abs(n[axis])) axis = 1;
            if (std::abs(n.z) > std::abs(n[axis])) axis = 2;
            sign = n[axis] >= 0.0f ? 1 : -1;
            // On the boundary only if the face sits at 0 or 1 along its axis.
            const float c = p0[axis];
            const float boundary = sign > 0 ? 1.0f : 0.0f;
            if (std::abs(c - boundary) > 1e-4f) axis = -1;
        }
    }

    bool FillPreviewRenderer::Initialize() {
        if (!g_renderBackend) return false;
        if (g_renderBackend->GetType() == BackendType::Vulkan) {
#ifdef HAS_VULKAN
            auto* vk = static_cast<VKBackend*>(g_renderBackend.get());
            m_shader = vk->CreateShaderFromFilesPortal("shaders/block.vert", "shaders/block.frag");
#endif
        } else {
            m_shader = g_renderBackend->CreateShaderFromFiles("shaders/block.vert", "shaders/block.frag");
        }
        if (m_shader == INVALID_SHADER) {
            Log::Warning("[FillPreviewRenderer] block shader failed to load - no fill preview");
            return false;
        }
        for (FrameBuffers& fb : m_frames) {
            fb.vb = g_renderBackend->CreateBuffer(BufferUsage::Vertex, kMaxVerts * sizeof(ItemCubeVert),
                                                  nullptr, BufferAccess::Streaming);
            fb.ib = g_renderBackend->CreateBuffer(BufferUsage::Index, kMaxIdx * sizeof(uint32_t),
                                                  nullptr, BufferAccess::Streaming);
            fb.mesh = g_renderBackend->CreateMesh(fb.vb, fb.ib, GetBlockVertexLayout());
        }
        m_initialized = true;
        return true;
    }

    void FillPreviewRenderer::Shutdown() {
        if (!g_renderBackend) return;
        for (FrameBuffers& fb : m_frames) {
            if (fb.mesh != INVALID_MESH) g_renderBackend->DestroyMesh(fb.mesh);
            if (fb.vb != INVALID_BUFFER) g_renderBackend->DestroyBuffer(fb.vb);
            if (fb.ib != INVALID_BUFFER) g_renderBackend->DestroyBuffer(fb.ib);
            fb = FrameBuffers{};
        }
        if (m_shader != INVALID_SHADER) { g_renderBackend->DestroyShader(m_shader); m_shader = INVALID_SHADER; }
        m_initialized = false;
    }

    void FillPreviewRenderer::Render(const glm::mat4& projection, const glm::mat4& view,
                                     const glm::vec3& cameraPos, Game::BlockState state,
                                     const glm::ivec3& lo, const glm::ivec3& hi) {
        if (!m_initialized || !g_renderBackend) return;
        PROFILE_ZONE_N("FillPreview");

        // The block's model, once.
        std::vector<ItemCubeVert> modelVerts;
        std::vector<uint32_t>     modelIdx;
        const Game::BlockModel& model = Game::BlockRegistry::GetBlockModel(state);
        if (!BuildBlockModelMeshFrom(model, modelVerts, modelIdx)) {
            BuildBlockCubeMesh(state.Block(), modelVerts, modelIdx);
        }
        if (modelIdx.size() < 3) return;
        const size_t quadCount = modelIdx.size() / 6;
        std::vector<int> faceAxis(quadCount), faceSign(quadCount);
        for (size_t q = 0; q < quadCount; ++q) FaceOf(modelVerts.data(), &modelIdx[q * 6], faceAxis[q], faceSign[q]);

        // Every cell on the box's shell; the inside is never seen.
        std::vector<ItemCubeVert> verts;
        std::vector<uint32_t>     idx;
        verts.reserve(std::min<size_t>(kMaxVerts, 4096));
        idx.reserve(std::min<size_t>(kMaxIdx, 6144));
        std::vector<uint32_t> remap(modelVerts.size());
        bool full = false;
        for (int z = lo.z; z <= hi.z && !full; ++z) {
            for (int y = lo.y; y <= hi.y && !full; ++y) {
                for (int x = lo.x; x <= hi.x; ++x) {
                    const bool shell = x == lo.x || x == hi.x || y == lo.y || y == hi.y || z == lo.z || z == hi.z;
                    if (!shell) continue;
                    std::fill(remap.begin(), remap.end(), UINT32_MAX);
                    for (size_t q = 0; q < quadCount; ++q) {
                        if (faceAxis[q] >= 0) {
                            glm::ivec3 n(x, y, z);
                            n[faceAxis[q]] += faceSign[q];
                            const bool neighbourInBox = n.x >= lo.x && n.x <= hi.x &&
                                                        n.y >= lo.y && n.y <= hi.y &&
                                                        n.z >= lo.z && n.z <= hi.z;
                            if (neighbourInBox) continue;
                        }
                        if (idx.size() + 6 > kMaxIdx || verts.size() + 6 > kMaxVerts) { full = true; break; }
                        for (int k = 0; k < 6; ++k) {
                            const uint32_t src = modelIdx[q * 6 + k];
                            if (remap[src] == UINT32_MAX) {
                                ItemCubeVert v = modelVerts[src];
                                v.x += static_cast<float>(x);
                                v.y += static_cast<float>(y);
                                v.z += static_cast<float>(z);
                                v.a = kAlpha;
                                remap[src] = static_cast<uint32_t>(verts.size());
                                verts.push_back(v);
                            }
                            idx.push_back(remap[src]);
                        }
                    }
                    if (full) break;
                }
            }
        }
        if (idx.empty()) return;

        m_parity ^= 1;
        FrameBuffers& fb = m_frames[m_parity];
        if (fb.mesh == INVALID_MESH) return;
        g_renderBackend->UpdateBuffer(fb.vb, 0, verts.size() * sizeof(ItemCubeVert), verts.data());
        g_renderBackend->UpdateBuffer(fb.ib, 0, idx.size() * sizeof(uint32_t), idx.data());

        PipelineState ps;
        ps.depthTestEnabled  = true;
        ps.depthWriteEnabled = false;   // see-through: nothing hides behind it
        ps.blendEnabled      = true;
        ps.cullMode          = CullMode::Back;
        ps.frontFace         = FrontFace::CounterClockwise;
        ps.primitiveType     = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(ps);

        g_renderBackend->BindShader(m_shader);
        if (g_atlasBuilder) g_renderBackend->BindTexture(g_atlasBuilder->GetBackendTextureHandle(), 0);
        g_renderBackend->SetUniformFloat(m_shader, "uAlphaTest", 0.01f);
        g_renderBackend->SetUniformVec4(m_shader, "uPortalClipPlane", glm::vec4(0.0f));
        const auto& env = EnvironmentState::Get().Frame();
        g_renderBackend->SetUniformFloat(m_shader, "uSkyBrightness", env.skyBrightness);
        g_renderBackend->SetUniformVec4(m_shader, "uFogColor", glm::vec4(env.fogColor, 1.0f));
        g_renderBackend->SetUniformVec4(m_shader, "uFogEnv",
            glm::vec4(env.fogEnvStart, env.fogEnvEnd, env.fogRdStart, env.fogRdEnd));
        g_renderBackend->SetUniformVec3(m_shader, "uCameraPos", cameraPos);
        g_renderBackend->SetUniformVec4(m_shader, "uOverlayColor", kTint);
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", projection * view);
        g_renderBackend->SetUniformMat4(m_shader, "uModel", glm::mat4(1.0f));   // world-space cells
        g_renderBackend->DrawIndexed(fb.mesh, static_cast<uint32_t>(idx.size()), 0);
        g_renderBackend->SetUniformVec4(m_shader, "uOverlayColor", glm::vec4(0.0f));
        g_renderBackend->UnbindMesh();

        PipelineState restore;
        restore.depthTestEnabled  = true;
        restore.depthWriteEnabled = true;
        restore.blendEnabled      = false;
        restore.cullMode          = CullMode::Back;
        g_renderBackend->SetPipelineState(restore);
    }

} // namespace Render

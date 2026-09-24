// File: src/client/renderer/entity/LightningBoltRenderer.cpp
//
// MC net.minecraft.client.renderer.entity.LightningBoltRenderer — see the
// header.
#include "LightningBoltRenderer.hpp"

#include "EntityCulling.hpp"
#include "../backend/RenderBackend.hpp"
#include "../core/RenderOrigin.hpp"
#include "../environment/EnvironmentState.hpp"
#include "../environment/EntityEnvironment.hpp"
#include "../mesh/ChunkRenderer.hpp"
#include "client/entity/ClientMobManager.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/entity/LightningBolt.hpp"

#ifdef HAS_VULKAN
#include "../backend/vulkan/VKBackend.hpp"
#endif

#include <cstdint>
#include <vector>

namespace Render {

    namespace {

        // MC BOLT_RED / BOLT_GREEN / BOLT_BLUE and the 0.3 alpha, through
        // ARGB.colorFromFloat (floor(f * 255)).
        constexpr uint8_t kBoltR = 114;   // 0.45
        constexpr uint8_t kBoltG = 114;   // 0.45
        constexpr uint8_t kBoltB = 127;   // 0.5
        constexpr uint8_t kBoltA = 76;    // 0.3

        constexpr int kSegmentCount       = 8;   // MC SEGMENT_COUNT
        constexpr int kLayerCount         = 4;   // MC LAYER_COUNT
        constexpr int kBranchCount        = 3;   // MC BRANCH_COUNT
        constexpr int kBranchSegmentCount = 3;   // MC BRANCH_SEGMENT_COUNT

        // MC shouldRenderAtSqrDistance: 64 × getViewScale().
        constexpr double kRenderDistance = 64.0;

        // MC LightningBoltRenderer.quad — one side of a segment box, from the
        // segment's bottom (start, y = seg*16) to its top (end, y = seg*16+16).
        void Quad(std::vector<ItemCubeVert>& out, const glm::vec3& origin,
                  float segmentStartX, float segmentStartZ,
                  float segmentEndX, float segmentEndZ, int currentSegment,
                  float topRadius, float bottomRadius,
                  bool rightXPositive, bool rightZPositive,
                  bool leftXPositive, bool leftZPositive) {
            const float y0 = static_cast<float>(currentSegment * 16);
            const float y1 = static_cast<float>((currentSegment + 1) * 16);
            const auto vert = [&](float x, float y, float z) {
                out.push_back({ origin.x + x, origin.y + y, origin.z + z,
                                0.5f, 0.5f, kBoltR, kBoltG, kBoltB, kBoltA });
            };
            vert(segmentStartX + (rightXPositive ? bottomRadius : -bottomRadius), y0,
                 segmentStartZ + (rightZPositive ? bottomRadius : -bottomRadius));
            vert(segmentEndX + (rightXPositive ? topRadius : -topRadius), y1,
                 segmentEndZ + (rightZPositive ? topRadius : -topRadius));
            vert(segmentEndX + (leftXPositive ? topRadius : -topRadius), y1,
                 segmentEndZ + (leftZPositive ? topRadius : -topRadius));
            vert(segmentStartX + (leftXPositive ? bottomRadius : -bottomRadius), y0,
                 segmentStartZ + (leftZPositive ? bottomRadius : -bottomRadius));
        }

    } // namespace

    bool LightningBoltRenderer::Initialize() {
        if (!g_renderBackend) return false;

        // The block shader (the XpOrbRenderer / ItemEntityRenderer choice, and
        // the same VK portal-layout caveat): texture × vertex colour, the
        // world's fog, the portal clip plane.
        if (g_renderBackend->GetType() == BackendType::Vulkan) {
#ifdef HAS_VULKAN
            auto* vk = static_cast<VKBackend*>(g_renderBackend.get());
            m_shader = vk->CreateShaderFromFilesPortal(
                "shaders/block.vert", "shaders/block.frag");
#endif
        } else {
            m_shader = g_renderBackend->CreateShaderFromFiles(
                "shaders/block.vert", "shaders/block.frag");
        }
        if (m_shader == INVALID_SHADER) {
            Log::Warning("[LightningBoltRenderer] failed to load block shader — "
                         "lightning will not render");
            return false;
        }

        // POSITION_COLOR has no texture; a 1×1 white texel makes the block
        // shader's `texture × vertex colour` the vertex colour alone.
        const uint8_t white[4] = { 255, 255, 255, 255 };
        m_white = g_renderBackend->CreateTexture2D(1, 1, TextureFormat::RGBA8, white);
        if (m_white == INVALID_TEXTURE) {
            g_renderBackend->DestroyShader(m_shader);
            m_shader = INVALID_SHADER;
            return false;
        }
        g_renderBackend->SetTextureFilter(m_white, TextureFilter::Nearest, TextureFilter::Nearest);
        g_renderBackend->SetTextureWrap(m_white, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        {
            std::vector<uint32_t> indices;
            indices.reserve(kMaxQuads * 6);
            for (uint32_t k = 0; k < kMaxQuads; ++k) {
                const uint32_t b = k * 4;
                indices.insert(indices.end(), { b, b + 1, b + 2, b, b + 2, b + 3 });
            }
            m_ib = g_renderBackend->CreateBuffer(
                BufferUsage::Index, indices.size() * sizeof(uint32_t),
                indices.data(), BufferAccess::Static);
        }
        for (FrameBuffers& fb : m_frames) {
            fb.vb = g_renderBackend->CreateBuffer(
                BufferUsage::Vertex, kMaxQuads * 4 * sizeof(ItemCubeVert),
                nullptr, BufferAccess::Streaming);
            fb.mesh = g_renderBackend->CreateMesh(fb.vb, m_ib, GetBlockVertexLayout());
        }
        m_verts.reserve(kQuadsPerBolt * 4);

        m_initialized = true;
        Log::Info("[LightningBoltRenderer] initialized");
        return true;
    }

    void LightningBoltRenderer::Shutdown() {
        if (!g_renderBackend) return;
        for (FrameBuffers& fb : m_frames) {
            if (fb.mesh != INVALID_MESH)  { g_renderBackend->DestroyMesh(fb.mesh); fb.mesh = INVALID_MESH; }
            if (fb.vb != INVALID_BUFFER)  { g_renderBackend->DestroyBuffer(fb.vb); fb.vb = INVALID_BUFFER; }
        }
        if (m_ib != INVALID_BUFFER)       { g_renderBackend->DestroyBuffer(m_ib); m_ib = INVALID_BUFFER; }
        if (m_white != INVALID_TEXTURE)   { g_renderBackend->DestroyTexture(m_white); m_white = INVALID_TEXTURE; }
        if (m_shader != INVALID_SHADER)   { g_renderBackend->DestroyShader(m_shader); m_shader = INVALID_SHADER; }
        m_initialized = false;
    }

    void LightningBoltRenderer::Render(const glm::mat4& projection, const glm::mat4& view,
                                       const glm::dvec3& cameraPos,
                                       const Client::ClientMobManager& mobs) {
        PROFILE_ZONE_N("LightningBoltRender");
        if (!m_initialized || !g_renderBackend) return;

        if (m_frameCursor.Advance()) m_quadCursor = 0;
        FrameBuffers& fb = m_frames[m_frameCursor.parity];
        if (fb.mesh == INVALID_MESH || m_quadCursor >= kMaxQuads) return;
        const size_t quadRoom = kMaxQuads - m_quadCursor;

        const double maxDist = kRenderDistance * EntityCulling::GetViewScale();
        const double maxDistSq = maxDist * maxDist;

        m_verts.clear();
        for (const Client::ClientMob* entry : mobs.ModelMobList()) {
            if (!entry || !entry->mob) continue;
            const Game::Mob& mob = *entry->mob;
            if (mob.GetType() != Game::EntityTypeId::LightningBolt) continue;
            if (mob.IsRemoved()) continue;
            if (m_verts.size() / 4 + kQuadsPerBolt > quadRoom) break;

            // A bolt never moves; its position is the strike point.
            const glm::dvec3 d = mob.position - cameraPos;
            if (glm::dot(d, d) >= maxDistSq) continue;

            const auto& bolt = static_cast<const Game::LightningBolt&>(mob);
            ++EntityCulling::g_renderedThisFrame;
            AppendBolt(bolt.GetSeed(), Render::ToRender(mob.position));
        }
        if (m_verts.empty()) return;

        const size_t quadCount = m_verts.size() / 4;
        g_renderBackend->UpdateBuffer(fb.vb, m_quadCursor * 4 * sizeof(ItemCubeVert),
                                      m_verts.size() * sizeof(ItemCubeVert), m_verts.data());
        const size_t firstIndex = m_quadCursor * 6;
        m_quadCursor += quadCount;

        // MC RenderPipelines.LIGHTNING: DepthStencilState.DEFAULT (test +
        // write), BlendFunction.LIGHTNING (SRC_ALPHA, ONE), culling on.
        PipelineState state;
        state.depthTestEnabled  = true;
        state.depthWriteEnabled = true;
        state.blendEnabled      = true;
        state.srcBlendFactor    = BlendFactor::SrcAlpha;
        state.dstBlendFactor    = BlendFactor::One;
        state.cullMode          = CullMode::Back;
        state.frontFace         = FrontFace::CounterClockwise;
        state.primitiveType     = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(state);

        g_renderBackend->BindShader(m_shader);
        g_renderBackend->BindTexture(m_white, 0);
        g_renderBackend->SetUniformFloat(m_shader, "uAlphaTest", 0.0f);
        g_renderBackend->SetUniformVec4(m_shader, "uPortalClipPlane",
                                        ::Render::ChunkRenderer::PortalEntityClipPlane());

        // rendertype_lightning has no lightmap: full brightness, day or night.
        // It is fogged like everything else — but toward BLACK: MC's
        // lightning.fsh scales the colour by (1 - fog) rather than mixing in
        // the fog colour, which with this additive blend (SRC_ALPHA, ONE) is
        // what makes a bolt fade out instead of adding a haze of fog colour.
        // Under Blindness a bolt past five blocks is gone.
        const auto& env = EnvironmentState::Get().Frame();
        // rendertype_lightning has no lightmap.
        EntityEnvironment::SetDrawLight(m_shader, glm::vec3(1.0f));
        g_renderBackend->SetUniformVec4(m_shader, "uFogColor", glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        g_renderBackend->SetUniformVec4(m_shader, "uFogEnv",
            glm::vec4(env.fogEnvStart, env.fogEnvEnd, env.fogRdStart, env.fogRdEnd));
        g_renderBackend->SetUniformVec3(m_shader, "uCameraPos", Render::ToRender(cameraPos));

        // Render-space quads and a render-space view: the MVP is the bare
        // view-projection, the model matrix the identity.
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", projection * view);
        g_renderBackend->SetUniformMat4(m_shader, "uModel", glm::mat4(1.0f));
        g_renderBackend->DrawIndexed(fb.mesh, static_cast<uint32_t>(quadCount * 6),
                                     static_cast<uint32_t>(firstIndex));
        g_renderBackend->UnbindMesh();
    }

    void LightningBoltRenderer::AppendBolt(int64_t seed, const glm::vec3& origin) {
        // MC submit: the trunk's per-segment drift, walked top-down from one
        // random seeded with the bolt's seed.
        float xOffsets[kSegmentCount];
        float zOffsets[kSegmentCount];
        float xOffset = 0.0f;
        float zOffset = 0.0f;
        {
            Game::JavaRandom random(seed);
            for (int heightSegmentIndex = kSegmentCount - 1; heightSegmentIndex >= 0;
                 --heightSegmentIndex) {
                xOffsets[heightSegmentIndex] = xOffset;
                zOffsets[heightSegmentIndex] = zOffset;
                xOffset += static_cast<float>(random.NextInt(11) - 5);
                zOffset += static_cast<float>(random.NextInt(11) - 5);
            }
        }

        for (int layer = 0; layer < kLayerCount; ++layer) {
            // A fresh random from the same seed per layer: every layer takes
            // the same path, only wider.
            Game::JavaRandom random(seed);

            for (int branchNumber = 0; branchNumber < kBranchCount; ++branchNumber) {
                const bool isTrunkBranch = branchNumber == 0;
                const int branchStartSegment = kSegmentCount - 1 - branchNumber;
                const int branchEndSegment =
                    isTrunkBranch ? 0 : branchStartSegment - kBranchSegmentCount + 1;
                float segmentStartX = xOffsets[branchStartSegment] - xOffset;
                float segmentStartZ = zOffsets[branchStartSegment] - zOffset;

                for (int currentSegment = branchStartSegment;
                     currentSegment >= branchEndSegment; --currentSegment) {
                    const float segmentEndX = segmentStartX;
                    const float segmentEndZ = segmentStartZ;
                    if (isTrunkBranch) {
                        segmentStartX += static_cast<float>(random.NextInt(11) - 5);
                        segmentStartZ += static_cast<float>(random.NextInt(11) - 5);
                    } else {
                        segmentStartX += static_cast<float>(random.NextInt(31) - 15);
                        segmentStartZ += static_cast<float>(random.NextInt(31) - 15);
                    }

                    float topRadius = 0.1f + static_cast<float>(layer) * 0.2f;
                    float bottomRadius = topRadius;
                    if (isTrunkBranch) {
                        topRadius *= static_cast<float>(currentSegment) * 0.1f + 1.0f;
                        bottomRadius *= static_cast<float>(currentSegment - 1) * 0.1f + 1.0f;
                    }

                    Quad(m_verts, origin, segmentStartX, segmentStartZ, segmentEndX, segmentEndZ,
                         currentSegment, topRadius, bottomRadius, false, false, true, false);
                    Quad(m_verts, origin, segmentStartX, segmentStartZ, segmentEndX, segmentEndZ,
                         currentSegment, topRadius, bottomRadius, true, false, true, true);
                    Quad(m_verts, origin, segmentStartX, segmentStartZ, segmentEndX, segmentEndZ,
                         currentSegment, topRadius, bottomRadius, true, true, false, true);
                    Quad(m_verts, origin, segmentStartX, segmentStartZ, segmentEndX, segmentEndZ,
                         currentSegment, topRadius, bottomRadius, false, true, false, false);
                }
            }
        }
    }

} // namespace Render

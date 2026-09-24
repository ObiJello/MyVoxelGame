// File: src/client/renderer/blockentity/BedRenderer.cpp
#include "client/resource/ResourcePacks.hpp"
#include "BedRenderer.hpp"

#include "../backend/RenderBackend.hpp"
#include "../core/RenderOrigin.hpp"
#include "../mesh/ChunkRenderer.hpp"
#include "BlockEntityShader.hpp"
#include "../../world/ClientChunkManager.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "common/world/block/BedBlock.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/Direction.hpp"
#include "common/core/Config.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"

#include "stb_image.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    BedRenderer g_bedRenderer;

    namespace {
        // The shared 24-byte block vertex layout (GetBlockVertexLayout).
        struct CubeVert {
            float x, y, z;
            float u, v;
            uint8_t r, g, b, a;
        };
        static_assert(sizeof(CubeVert) == 24, "vertex stride must match block layout");

        constexpr float kPi     = 3.14159265358979323846f;
        constexpr float kHalfPi = kPi * 0.5f;

        // MC Lighting.setupLevel: the two directional lights the entity
        // shader mixes (light.glsl minecraft_mix_light) —
        //   accum = min(1, (max(0, n·L0) + max(0, n·L1)) * 0.6 + 0.4)
        // with L0 = (0.2, 1, -0.7) and L1 = (-0.2, 1, 0.7), normalised.
        uint8_t ShadeFor(const glm::vec3& normal) {
            static const glm::vec3 kLight0 = glm::normalize(glm::vec3( 0.2f, 1.0f, -0.7f));
            static const glm::vec3 kLight1 = glm::normalize(glm::vec3(-0.2f, 1.0f,  0.7f));
            const glm::vec3 n = glm::normalize(normal);
            const float l0 = std::max(0.0f, glm::dot(kLight0, n));
            const float l1 = std::max(0.0f, glm::dot(kLight1, n));
            const float accum = std::min(1.0f, (l0 + l1) * 0.6f + 0.4f);
            return static_cast<uint8_t>(accum * 255.0f);
        }

        // MC ModelPart.translateAndRotate → Quaternionf.rotationZYX(z, y, x):
        // Rz · Ry · Rx applied to the vertex.
        glm::mat4 PartRotation(float xRot, float yRot, float zRot) {
            glm::mat4 m(1.0f);
            m = glm::rotate(m, zRot, glm::vec3(0, 0, 1));
            m = glm::rotate(m, yRot, glm::vec3(0, 1, 0));
            m = glm::rotate(m, xRot, glm::vec3(1, 0, 0));
            return m;
        }

        // MC BedRenderer.renderPiece's pose for one half at its own cell
        // (translateZ = false), in block units:
        //   translate(0, 0.5625, 0) · rotX(90°) · translate(.5,.5,.5)
        //   · rotZ(180° + facing.toYRot()) · translate(-.5,-.5,-.5)
        glm::mat4 PiecePose(Game::Direction facing) {
            glm::mat4 m(1.0f);
            m = glm::translate(m, glm::vec3(0.0f, 0.5625f, 0.0f));
            m = glm::rotate(m, glm::radians(90.0f), glm::vec3(1, 0, 0));
            m = glm::translate(m, glm::vec3(0.5f));
            m = glm::rotate(m, glm::radians(180.0f + Game::ToYRot(facing)), glm::vec3(0, 0, 1));
            m = glm::translate(m, glm::vec3(-0.5f));
            return m;
        }

        // One ModelPart.Cube: `from`/`to` in MC pixels, posed by `xform`
        // (block units — the /16 happens here), UVs per ModelPart.Polygon
        // over the 64×64 sheet, per-face shade from the posed normal. The
        // polygon layout is ChestRenderer::AddCube's — ModelPart.java's
        // vertex order per face and its high-u-first UV assignment.
        void AddCube(std::vector<CubeVert>& verts, std::vector<uint32_t>& idx,
                     const glm::mat4& xform,
                     glm::vec3 from, glm::vec3 to,
                     float xTexOffs, float yTexOffs, float w, float h, float d) {
            const glm::vec3 lo = from / 16.0f, hi = to / 16.0f;
            const glm::vec3 t0(lo.x, lo.y, lo.z), t1(hi.x, lo.y, lo.z);
            const glm::vec3 t2(hi.x, hi.y, lo.z), t3(lo.x, hi.y, lo.z);
            const glm::vec3 l0(lo.x, lo.y, hi.z), l1(hi.x, lo.y, hi.z);
            const glm::vec3 l2(hi.x, hi.y, hi.z), l3(lo.x, hi.y, hi.z);

            const float u0 = xTexOffs;
            const float u1 = xTexOffs + d;
            const float u2 = xTexOffs + d + w;
            const float u22= xTexOffs + d + w + w;
            const float u3 = xTexOffs + d + w + d;
            const float u4 = xTexOffs + d + w + d + w;
            const float v0 = yTexOffs;
            const float v1 = yTexOffs + d;
            const float v2 = yTexOffs + d + h;

            const glm::mat3 normalMat(xform);
            auto emit = [&](const glm::vec3 q[4], float U0, float V0, float U1, float V1,
                            const glm::vec3& localNormal) {
                const uint8_t sh = ShadeFor(normalMat * localNormal);
                const uint32_t base = static_cast<uint32_t>(verts.size());
                auto push = [&](const glm::vec3& p, float u, float v) {
                    const glm::vec3 q4 = glm::vec3(xform * glm::vec4(p, 1.0f));
                    verts.push_back({q4.x, q4.y, q4.z, u / 64.0f, v / 64.0f, sh, sh, sh, 255});
                };
                push(q[0], U1, V0);
                push(q[1], U0, V0);
                push(q[2], U0, V1);
                push(q[3], U1, V1);
                idx.push_back(base + 0); idx.push_back(base + 1); idx.push_back(base + 2);
                idx.push_back(base + 0); idx.push_back(base + 2); idx.push_back(base + 3);
            };

            { const glm::vec3 q[4] = {l1, l0, t0, t1}; emit(q, u1, v0, u2,  v1, { 0,-1, 0}); }  // DOWN
            { const glm::vec3 q[4] = {t2, t3, l3, l2}; emit(q, u2, v1, u22, v0, { 0, 1, 0}); }  // UP (v flipped, as MC)
            { const glm::vec3 q[4] = {t0, l0, l3, t3}; emit(q, u0, v1, u1,  v2, {-1, 0, 0}); }  // WEST
            { const glm::vec3 q[4] = {t1, t0, t3, t2}; emit(q, u1, v1, u2,  v2, { 0, 0,-1}); }  // NORTH
            { const glm::vec3 q[4] = {l1, t1, t2, l2}; emit(q, u2, v1, u3,  v2, { 1, 0, 0}); }  // EAST
            { const glm::vec3 q[4] = {l0, l1, l2, l3}; emit(q, u3, v1, u4,  v2, { 0, 0, 1}); }  // SOUTH
        }

        // MC BedRenderer.createHeadLayer / createFootLayer, verbatim:
        //   head: main texOffs(0,0) box(0,0,0, 16,16,6)
        //         left_leg  texOffs(50,6)  box(0,6,0, 3,3,3)     rotation(π/2, 0, π/2)
        //         right_leg texOffs(50,18) box(-16,6,0, 3,3,3)   rotation(π/2, 0, π)
        //   foot: main texOffs(0,22) box(0,0,0, 16,16,6)
        //         left_leg  texOffs(50,0)  box(0,6,-16, 3,3,3)   rotation(π/2, 0, 0)
        //         right_leg texOffs(50,12) box(-16,6,-16, 3,3,3) rotation(π/2, 0, 3π/2)
        void BuildPiece(int piece, const glm::mat4& pose,
                        std::vector<CubeVert>& verts, std::vector<uint32_t>& idx) {
            if (piece == 0) {
                AddCube(verts, idx, pose, {0, 0, 0}, {16, 16, 6}, 0, 0, 16, 16, 6);
                AddCube(verts, idx, pose * PartRotation(kHalfPi, 0.0f, kHalfPi),
                        {0, 6, 0}, {3, 9, 3}, 50, 6, 3, 3, 3);
                AddCube(verts, idx, pose * PartRotation(kHalfPi, 0.0f, kPi),
                        {-16, 6, 0}, {-13, 9, 3}, 50, 18, 3, 3, 3);
            } else {
                AddCube(verts, idx, pose, {0, 0, 0}, {16, 16, 6}, 0, 22, 16, 16, 6);
                AddCube(verts, idx, pose * PartRotation(kHalfPi, 0.0f, 0.0f),
                        {0, 6, -16}, {3, 9, -13}, 50, 0, 3, 3, 3);
                AddCube(verts, idx, pose * PartRotation(kHalfPi, 0.0f, 1.5f * kPi),
                        {-16, 6, -16}, {-13, 9, -13}, 50, 12, 3, 3, 3);
            }
        }
    } // namespace

    BedRenderer::~BedRenderer() { Shutdown(); }

    bool BedRenderer::Initialize() {
        if (!g_renderBackend) return false;

        // The shared block-entity shader (BlockEntityShader.hpp): lit and
        // fogged like the terrain, on both backends.
        m_shader = BlockEntityShader::Create();
        if (m_shader == INVALID_SHADER) {
            Log::Error("[BedRenderer] shader compile failed");
            return false;
        }

        // Eight meshes: each piece in each of the four horizontal facings,
        // posed once here so a draw is a translate. MC rotates the pose per
        // draw; with two cubes of legs per piece the baked form costs 8 × 108
        // indices and saves nothing worth a per-draw matrix.
        std::vector<CubeVert> verts;
        std::vector<uint32_t> idx;
        for (int piece = 0; piece < kPieces; ++piece) {
            for (int f = 0; f < kFacings; ++f) {
                const Game::Direction facing = Game::HorizontalFacingFromIndex(f);
                verts.clear(); idx.clear();
                BuildPiece(piece, PiecePose(facing), verts, idx);

                Mesh& m = m_meshes[piece][f];
                m.vb = g_renderBackend->CreateBuffer(BufferUsage::Vertex,
                    verts.size() * sizeof(CubeVert), verts.data());
                m.ib = g_renderBackend->CreateBuffer(BufferUsage::Index,
                    idx.size() * sizeof(uint32_t), idx.data());
                m.mesh = g_renderBackend->CreateMesh(m.vb, m.ib, GetBlockVertexLayout());
                m.indexCount = static_cast<uint32_t>(idx.size());
                if (m.vb == INVALID_BUFFER || m.ib == INVALID_BUFFER || m.mesh == INVALID_MESH) {
                    Log::Error("[BedRenderer] mesh creation failed");
                    Shutdown();
                    return false;
                }
            }
        }

        m_initialized = true;
        Log::Info("[BedRenderer] initialized");
        return true;
    }

    void BedRenderer::Shutdown() {
        if (!g_renderBackend) return;
        for (auto& [colour, tex] : m_textures) {
            if (tex != INVALID_TEXTURE) g_renderBackend->DestroyTexture(tex);
        }
        m_textures.clear();
        for (int piece = 0; piece < kPieces; ++piece) {
            for (int f = 0; f < kFacings; ++f) {
                Mesh& m = m_meshes[piece][f];
                if (m.mesh != INVALID_MESH)  { g_renderBackend->DestroyMesh(m.mesh);  m.mesh = INVALID_MESH; }
                if (m.vb   != INVALID_BUFFER){ g_renderBackend->DestroyBuffer(m.vb);  m.vb   = INVALID_BUFFER; }
                if (m.ib   != INVALID_BUFFER){ g_renderBackend->DestroyBuffer(m.ib);  m.ib   = INVALID_BUFFER; }
                m.indexCount = 0;
            }
        }
        if (m_shader != INVALID_SHADER) { g_renderBackend->DestroyShader(m_shader); m_shader = INVALID_SHADER; }
        m_visibleChunks.clear();
        m_visibleChunks.shrink_to_fit();
        m_seen.clear();
        m_initialized = false;
    }

    std::string BedRenderer::ColourOf(Game::BlockID id) {
        // "light_blue_bed" → "light_blue": the sheet is named by the dye.
        const std::string& slug = Game::BlockRegistry::Get(id).registrySlug;
        constexpr std::string_view kSuffix = "_bed";
        if (slug.size() > kSuffix.size() &&
            slug.compare(slug.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0) {
            return slug.substr(0, slug.size() - kSuffix.size());
        }
        return "red";
    }

    TextureHandle BedRenderer::LoadColourTexture(const std::string& colour) {
        if (Resources::CacheStale(m_packGeneration)) {
            // A resource pack change replaces every texture here.
            if (g_renderBackend) for (auto& [key, tex] : m_textures) if (tex != INVALID_TEXTURE) g_renderBackend->DestroyTexture(tex);
            m_textures.clear();
        }
        auto it = m_textures.find(colour);
        if (it != m_textures.end()) return it->second;

        const std::string rel  = "assets/textures/entity/bed/" + colour + ".png";
        const std::string full = PlatformMain::GetAssetPath(rel);
        if (!std::filesystem::exists(full)) {
            Log::Warning("[BedRenderer] missing %s", full.c_str());
            m_textures[colour] = INVALID_TEXTURE;
            return INVALID_TEXTURE;
        }
        int w = 0, h = 0, ch = 0;
        stbi_set_flip_vertically_on_load(0);
        unsigned char* px = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha);
        if (!px) {
            Log::Warning("[BedRenderer] stbi_load failed for %s: %s", full.c_str(), stbi_failure_reason());
            m_textures[colour] = INVALID_TEXTURE;
            return INVALID_TEXTURE;
        }
        TextureHandle tex = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, px);
        stbi_image_free(px);
        if (tex != INVALID_TEXTURE) {
            g_renderBackend->SetTextureFilter(tex, TextureFilter::Nearest, TextureFilter::Nearest);
            g_renderBackend->SetTextureWrap (tex, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
        }
        m_textures[colour] = tex;
        return tex;
    }

    void BedRenderer::RenderSingle(Game::BlockID bed, Game::Direction facing, const glm::dvec3& footWorldPos,
                                   const glm::mat4& projection, const glm::mat4& view,
                                   const glm::dvec3& cameraWorld) {
        if (!m_initialized || !g_renderBackend || !Game::IsBedBlock(bed)) return;
        const int f = Game::HorizontalFacingIndex(facing);
        if (f < 0 || f >= kFacings) return;
        const TextureHandle tex = LoadColourTexture(ColourOf(bed));
        if (tex == INVALID_TEXTURE) return;

        PipelineState s;
        s.depthTestEnabled  = true;
        s.depthWriteEnabled = true;
        s.blendEnabled      = false;
        s.cullMode          = CullMode::Back;
        s.primitiveType     = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(s);
        g_renderBackend->BindShader(m_shader);
        g_renderBackend->SetUniformFloat(m_shader, "uAlphaTest", 0.05f);
        g_renderBackend->BindTexture(tex, 0);

        const glm::mat4 viewProj = projection * view;
        const glm::dvec3 headWorldPos = footWorldPos +
            glm::dvec3(Game::StepX(facing), 0.0, Game::StepZ(facing));
        const glm::dvec3 pieces[kPieces] = { headWorldPos, footWorldPos };   // 0 = head, 1 = foot
        for (int piece = 0; piece < kPieces; ++piece) {
            const Mesh& mesh = m_meshes[piece][f];
            if (mesh.mesh == INVALID_MESH) continue;
            const glm::mat4 model = glm::translate(glm::mat4(1.0f), Render::ToRender(pieces[piece]));
            g_renderBackend->SetUniformMat4(m_shader, "uMVP", viewProj * model);
            BlockEntityShader::ApplyWorld(m_shader, model, glm::vec3(cameraWorld));
            g_renderBackend->DrawIndexed(mesh.mesh, mesh.indexCount);
        }
        g_renderBackend->UnbindMesh();
    }

    void BedRenderer::Render(Client::ClientChunkManager* chunkMgr,
                             const glm::mat4& projection, const glm::mat4& view,
                             const glm::vec3& cameraPos, float /*partialTick*/) {
        if (!m_initialized || !g_renderBackend || !chunkMgr || !Client::g_clientBlockAccess) return;

        PROFILE_ZONE_N("Beds");

        BlockEntityRenderDispatcher::CollectVisibleChunks(chunkMgr, m_visibleChunks, m_seen);
        const ChunkRenderer* sections = g_chunkRenderer;
        const float maxDistSq = kViewDistance * kViewDistance;
        const glm::mat4 viewProj = projection * view;

        bool pipelineSet = false;
        TextureHandle boundTexture = INVALID_TEXTURE;

        for (const Client::ClientChunk* chunk : m_visibleChunks) {
            if (!chunk || chunk->beds.empty()) continue;   // the cull that matters: a size() check

            for (const glm::ivec3& block : chunk->beds) {
                // The section gate, per block — a bed in a section that is
                // not drawn is not drawn either.
                if (sections &&
                    !sections->IsSectionVisible(chunk->position,
                                                (block.y - Config::MinY) >> 4)) {
                    continue;
                }
                // MC's cheap horizontal distance test, in world space.
                const float dx = static_cast<float>(block.x) + 0.5f - cameraPos.x;
                const float dz = static_cast<float>(block.z) + 0.5f - cameraPos.z;
                if (dx * dx + dz * dz > maxDistSq) continue;

                // Piece and facing off the BLOCK state, as vanilla reads
                // BedBlock.PART / FACING off the block entity's state.
                const Game::BlockState state =
                    Client::g_clientBlockAccess->GetBlockState(block.x, block.y, block.z);
                if (!Game::IsBedBlock(state.Block())) continue;   // the index lags a remesh by a frame at most
                const int piece = Game::IsBedHead(state) ? 0 : 1;
                const int f = Game::HorizontalFacingIndex(Game::BedFacing(state));
                if (f < 0 || f >= kFacings) continue;
                const Mesh& mesh = m_meshes[piece][f];

                TextureHandle tex = LoadColourTexture(ColourOf(state.Block()));
                if (tex == INVALID_TEXTURE) continue;

                if (!pipelineSet) {
                    PipelineState s;
                    s.depthTestEnabled  = true;
                    s.depthWriteEnabled = true;
                    s.blendEnabled      = false;
                    s.cullMode          = CullMode::Back;
                    s.primitiveType     = PrimitiveType::Triangles;
                    g_renderBackend->SetPipelineState(s);
                    g_renderBackend->BindShader(m_shader);
                    g_renderBackend->SetUniformFloat(m_shader, "uAlphaTest", 0.05f);
                    pipelineSet = true;
                }
                if (tex != boundTexture) {
                    g_renderBackend->BindTexture(tex, 0);
                    boundTexture = tex;
                }

                // The mesh is the cell's local [0,1]³ already posed for its
                // facing; the model is the cell's corner in RENDER space
                // (camera-relative, see RenderOrigin.hpp).
                const glm::mat4 model = glm::translate(glm::mat4(1.0f),
                                                       Render::ToRender(glm::dvec3(block)));
                g_renderBackend->SetUniformMat4(m_shader, "uMVP", viewProj * model);
                // The bed's light (its cell's) and the frame's fog.
                BlockEntityShader::ApplyWorld(m_shader, model, cameraPos, block);
                g_renderBackend->DrawIndexed(mesh.mesh, mesh.indexCount);
            }
        }

        if (pipelineSet) g_renderBackend->UnbindMesh();
    }

} // namespace Render

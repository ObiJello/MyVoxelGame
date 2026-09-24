// File: src/client/renderer/blockentity/LecternRenderer.cpp
//
// See header. The book, part for part (BookModel.createBodyLayer, 64×32):
//
//   left_lid    texOffs(0,0)   addBox(-6,-5,-0.005, 6,10,0.005)  offset(0,0,-1)
//   right_lid   texOffs(16,0)  addBox( 0,-5,-0.005, 6,10,0.005)  offset(0,0, 1)
//   seam        texOffs(12,0)  addBox(-1,-5, 0,     2,10,0.005)  rotation(0,π/2,0)
//   left_pages  texOffs(0,10)  addBox( 0,-4,-0.99,  5, 8,1)
//   right_pages texOffs(12,10) addBox( 0,-4,-0.01,  5, 8,1)
//   flip_page1  texOffs(24,10) addBox( 0,-4, 0,     5, 8,0.005)
//   flip_page2  texOffs(24,10) addBox( 0,-4, 0,     5, 8,0.005)
//
// posed by BookModel.setupAnim for LecternRenderer.BOOK_STATE =
// State.forAnimation(0, 0.1, 0.9, 1.2):
//
//   openness       = (sin(0 * 0.02) * 0.1 + 1.25) * 1.2 = 1.5
//   leftLid.yRot   = π + openness          rightLid.yRot  = -openness
//   leftPages.yRot = openness              rightPages.yRot = -openness
//   flipPage1.yRot = openness - openness * 2 * 0.1
//   flipPage2.yRot = openness - openness * 2 * 0.9
//   the four page parts' x = sin(openness)
//
// (Model.setupAnim resets every part to its initial pose first, so the lids
// keep their z offsets and the seam its quarter turn.) A part's pose is
// ModelPart.translateAndRotate: translate(x, y, z) then rotationZYX — only yRot
// is ever set here. The draw matrix replays LecternRenderer.submit:
//
//   translate(0.5, 1.0625, 0.5)
//   rotateDegrees(Y, -FACING.getClockWise().toYRot())
//   rotateDegrees(Z, 67.5)
//   translate(0, -0.125, 0)
//   scale(1/16)                   pixels → world units (ModelPart.Cube /16)
//
// Unlike the entity renderers there is no scale(-1,-1,1): a block-entity
// model is drawn straight in its pose.
#include "client/resource/ResourcePacks.hpp"
#include "LecternRenderer.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "../backend/RenderBackend.hpp"
#include "../core/RenderOrigin.hpp"
#include "../entity/EntityLighting.hpp"
#include "BlockEntityShader.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "common/core/Log.hpp"

#include "stb_image.h"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    namespace {
        struct CubeVert {
            float x, y, z;
            float u, v;
            uint8_t r, g, b, a;
        };
        static_assert(sizeof(CubeVert) == 24, "match GetBlockVertexLayout");

        constexpr float kTexW = 64.0f;   // LayerDefinition.create(mesh, 64, 32)
        constexpr float kTexH = 32.0f;

        // EnchantTableRenderer.BOOK_TEXTURE =
        // Sheets.BLOCK_ENTITIES_MAPPER.defaultNamespaceApply("enchantment/enchanting_table_book").
        constexpr const char* kTextureRel = "assets/textures/entity/enchantment/enchanting_table_book.png";

        struct FaceLighting {
            glm::mat4 outer{1.0f};
            EntityLighting::LightSet set = EntityLighting::LightSet::Default;
        };

        // One ModelPart.Cube: vertices, UV windows and per-face shade exactly
        // as SkullBlockRenderer's AddCube (the same Cube ctor port), without
        // the mirror flag the book never uses.
        void AddCube(std::vector<CubeVert>& verts, std::vector<uint32_t>& idx,
                     glm::vec3 from, glm::vec3 size, float xTexOffs, float yTexOffs,
                     const glm::mat4& xform, const FaceLighting& lighting) {
            const float w = size.x, h = size.y, d = size.z;
            const float minX = from.x, minY = from.y, minZ = from.z;
            const float maxX = from.x + w, maxY = from.y + h, maxZ = from.z + d;

            auto tp = [&](float x, float y, float z) {
                return glm::vec3(xform * glm::vec4(x, y, z, 1.0f));
            };
            const glm::vec3 t0 = tp(minX, minY, minZ), t1 = tp(maxX, minY, minZ);
            const glm::vec3 t2 = tp(maxX, maxY, minZ), t3 = tp(minX, maxY, minZ);
            const glm::vec3 l0 = tp(minX, minY, maxZ), l1 = tp(maxX, minY, maxZ);
            const glm::vec3 l2 = tp(maxX, maxY, maxZ), l3 = tp(minX, maxY, maxZ);

            const float u0 = xTexOffs;
            const float u1 = xTexOffs + d;
            const float u2 = xTexOffs + d + w;
            const float u22= xTexOffs + d + w + w;
            const float u3 = xTexOffs + d + w + d;
            const float u4 = xTexOffs + d + w + d + w;
            const float v0 = yTexOffs;
            const float v1 = yTexOffs + d;
            const float v2 = yTexOffs + d + h;

            const glm::mat3 normalMat = EntityLighting::NormalMatrix(lighting.outer * xform);
            auto shade = [&](const glm::vec3& modelNormal) -> uint8_t {
                return EntityLighting::ShadeByte(normalMat * modelNormal, lighting.set);
            };
            const uint8_t S_DOWN  = shade({ 0, -1,  0});
            const uint8_t S_UP    = shade({ 0,  1,  0});
            const uint8_t S_WEST  = shade({-1,  0,  0});
            const uint8_t S_EAST  = shade({ 1,  0,  0});
            const uint8_t S_NORTH = shade({ 0,  0, -1});
            const uint8_t S_SOUTH = shade({ 0,  0,  1});

            auto emit = [&](const glm::vec3 q[4], float U0, float V0, float U1, float V1, uint8_t sh) {
                const uint32_t base = static_cast<uint32_t>(verts.size());
                verts.push_back({q[0].x, q[0].y, q[0].z, U1 / kTexW, V0 / kTexH, sh, sh, sh, 255});
                verts.push_back({q[1].x, q[1].y, q[1].z, U0 / kTexW, V0 / kTexH, sh, sh, sh, 255});
                verts.push_back({q[2].x, q[2].y, q[2].z, U0 / kTexW, V1 / kTexH, sh, sh, sh, 255});
                verts.push_back({q[3].x, q[3].y, q[3].z, U1 / kTexW, V1 / kTexH, sh, sh, sh, 255});
                idx.push_back(base + 0);
                idx.push_back(base + 1);
                idx.push_back(base + 2);
                idx.push_back(base + 0);
                idx.push_back(base + 2);
                idx.push_back(base + 3);
            };

            { const glm::vec3 q[4] = {l1, l0, t0, t1}; emit(q, u1, v0, u2,  v1, S_DOWN); }
            { const glm::vec3 q[4] = {t2, t3, l3, l2}; emit(q, u2, v1, u22, v0, S_UP); }
            { const glm::vec3 q[4] = {t0, l0, l3, t3}; emit(q, u0, v1, u1,  v2, S_WEST); }
            { const glm::vec3 q[4] = {t1, t0, t3, t2}; emit(q, u1, v1, u2,  v2, S_NORTH); }
            { const glm::vec3 q[4] = {l1, t1, t2, l2}; emit(q, u2, v1, u3,  v2, S_EAST); }
            { const glm::vec3 q[4] = {l0, l1, l2, l3}; emit(q, u3, v1, u4,  v2, S_SOUTH); }
        }

        // FACING.getClockWise().toYRot(): N→E 270, E→S 0, S→W 90, W→N 180.
        float ClockwiseYRot(Game::Direction facing) {
            switch (facing) {
                case Game::Direction::North: return 270.0f;
                case Game::Direction::East:  return 0.0f;
                case Game::Direction::South: return 90.0f;
                case Game::Direction::West:  return 180.0f;
                default:                     return 0.0f;
            }
        }

        // Index of a lectern facing into the mesh's lighting copies.
        int FacingSlot(Game::Direction facing) {
            switch (facing) {
                case Game::Direction::East:  return 1;
                case Game::Direction::South: return 2;
                case Game::Direction::West:  return 3;
                default:                     return 0;   // North
            }
        }
        constexpr Game::Direction kFacings[4] = {
            Game::Direction::North, Game::Direction::East, Game::Direction::South, Game::Direction::West,
        };

        // LecternRenderer.submit's rotations (the translations do not turn
        // a normal).
        glm::mat4 SubmitRotation(Game::Direction facing) {
            glm::mat4 m = glm::rotate(glm::mat4(1.0f), glm::radians(-ClockwiseYRot(facing)), glm::vec3(0, 1, 0));
            return glm::rotate(m, glm::radians(67.5f), glm::vec3(0, 0, 1));
        }

    } // namespace

    LecternRenderer::LecternRenderer() = default;
    LecternRenderer::~LecternRenderer() { Shutdown(); }

    bool LecternRenderer::Initialize() {
        if (!g_renderBackend) return false;

        m_shader = BlockEntityShader::Create();
        if (m_shader == INVALID_SHADER) {
            Log::Error("[LecternRenderer] shader compile failed");
            return false;
        }

        // BookModel.setupAnim for BOOK_STATE (see the file header).
        const float openness = (std::sin(0.0f * 0.02f) * 0.1f + 1.25f) * 1.2f;
        const float pageFlip1 = 0.1f;
        const float pageFlip2 = 0.9f;
        const float pagesX = std::sin(openness);

        // ModelPart.translateAndRotate with only a y rotation.
        auto pose = [](glm::vec3 offset, float yRot) {
            glm::mat4 m = glm::translate(glm::mat4(1.0f), offset);
            if (yRot != 0.0f) m = glm::rotate(m, yRot, glm::vec3(0, 1, 0));
            return m;
        };
        const float pi = 3.1415927f;

        std::vector<CubeVert> verts;
        std::vector<uint32_t> idx;
        verts.reserve(7 * 24 * kLightingCount);
        idx.reserve(7 * 36 * kLightingCount);
        for (int lighting = 0; lighting < kLightingCount; ++lighting) {
            FaceLighting fl;
            fl.outer = SubmitRotation(kFacings[lighting / 2]);
            fl.set = (lighting % 2) ? EntityLighting::LightSet::Nether : EntityLighting::LightSet::Default;

            AddCube(verts, idx, {-6, -5, -0.005f}, {6, 10, 0.005f}, 0, 0,
                    pose({0, 0, -1}, pi + openness), fl);                                 // left_lid
            AddCube(verts, idx, {0, -5, -0.005f}, {6, 10, 0.005f}, 16, 0,
                    pose({0, 0, 1}, -openness), fl);                                      // right_lid
            AddCube(verts, idx, {-1, -5, 0}, {2, 10, 0.005f}, 12, 0,
                    pose({0, 0, 0}, 1.5707964f), fl);                                     // seam
            AddCube(verts, idx, {0, -4, -0.99f}, {5, 8, 1}, 0, 10,
                    pose({pagesX, 0, 0}, openness), fl);                                  // left_pages
            AddCube(verts, idx, {0, -4, -0.01f}, {5, 8, 1}, 12, 10,
                    pose({pagesX, 0, 0}, -openness), fl);                                 // right_pages
            AddCube(verts, idx, {0, -4, 0}, {5, 8, 0.005f}, 24, 10,
                    pose({pagesX, 0, 0}, openness - openness * 2.0f * pageFlip1), fl);    // flip_page1
            AddCube(verts, idx, {0, -4, 0}, {5, 8, 0.005f}, 24, 10,
                    pose({pagesX, 0, 0}, openness - openness * 2.0f * pageFlip2), fl);    // flip_page2
        }

        m_vb = g_renderBackend->CreateBuffer(BufferUsage::Vertex, verts.size() * sizeof(CubeVert), verts.data());
        m_ib = g_renderBackend->CreateBuffer(BufferUsage::Index, idx.size() * sizeof(uint32_t), idx.data());
        m_mesh = g_renderBackend->CreateMesh(m_vb, m_ib, GetBlockVertexLayout());
        m_indexCount = static_cast<uint32_t>(idx.size() / kLightingCount);
        m_geomBuilt = m_mesh != INVALID_MESH;
        return m_geomBuilt;
    }

    void LecternRenderer::Shutdown() {
        if (!g_renderBackend) return;
        if (m_tex  != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(m_tex); m_tex = INVALID_TEXTURE; }
        if (m_mesh != INVALID_MESH)    { g_renderBackend->DestroyMesh(m_mesh);   m_mesh = INVALID_MESH; }
        if (m_vb   != INVALID_BUFFER)  { g_renderBackend->DestroyBuffer(m_vb);   m_vb = INVALID_BUFFER; }
        if (m_ib   != INVALID_BUFFER)  { g_renderBackend->DestroyBuffer(m_ib);   m_ib = INVALID_BUFFER; }
        if (m_shader != INVALID_SHADER) { g_renderBackend->DestroyShader(m_shader); m_shader = INVALID_SHADER; }
        m_texTried = false;
        m_geomBuilt = false;
    }

    TextureHandle LecternRenderer::LoadTexture() {
        // Resource pack reload: read the sheet again on demand.
        if (Resources::CacheStale(m_packGeneration)) {
            if (m_tex != INVALID_TEXTURE && g_renderBackend) g_renderBackend->DestroyTexture(m_tex);
            m_tex = INVALID_TEXTURE;
            m_texTried = false;
        }
        if (m_texTried) return m_tex;
        m_texTried = true;

        const std::string full = PlatformMain::GetAssetPath(kTextureRel);
        if (!std::filesystem::exists(full)) {
            Log::Warning("[LecternRenderer] missing %s", full.c_str());
            return INVALID_TEXTURE;
        }
        int w = 0, h = 0, ch = 0;
        stbi_set_flip_vertically_on_load(0);
        unsigned char* px = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha);
        if (!px) return INVALID_TEXTURE;
        TextureHandle tex = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, px);
        stbi_image_free(px);
        if (tex != INVALID_TEXTURE) {
            g_renderBackend->SetTextureFilter(tex, TextureFilter::Nearest, TextureFilter::Nearest);
            g_renderBackend->SetTextureWrap (tex, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
        }
        m_tex = tex;
        return tex;
    }

    void LecternRenderer::Render(const Game::BlockEntity& be,
                                 float /*partialTick*/,
                                 const glm::mat4& proj,
                                 const glm::mat4& view,
                                 const glm::vec3& cameraPos) {
        PROFILE_ZONE_N("BE.Lectern");
        if (!m_geomBuilt || !g_renderBackend || !Client::g_clientBlockAccess) return;

        // LecternRenderer.extractRenderState: HAS_BOOK and FACING off the
        // BLOCK state; no book, nothing drawn.
        const glm::ivec3 pos = be.GetWorldPos();
        const Game::BlockState state = Client::g_clientBlockAccess->GetBlockState(pos.x, pos.y, pos.z);
        if (state.Block() != Game::BlockID::Lectern) return;
        if (!Game::BoolOf(state, Game::PropertyId::HAS_BOOK)) return;
        const Game::Direction facing = Game::HorizontalFacingOf(state);

        TextureHandle tex = LoadTexture();
        if (tex == INVALID_TEXTURE) return;

        // LecternRenderer.submit's pose stack (see the file header); the
        // translation is render-space — RenderOrigin.hpp.
        glm::mat4 model = glm::translate(glm::mat4(1.0f),
                                         Render::ToRender(glm::dvec3(pos)) + glm::vec3(0.5f, 1.0625f, 0.5f));
        model = model * SubmitRotation(facing);
        model = glm::translate(model, glm::vec3(0.0f, -0.125f, 0.0f));
        model = glm::scale(model, glm::vec3(1.0f / 16.0f));

        PipelineState s;
        s.depthTestEnabled  = true;
        s.depthWriteEnabled = true;
        s.blendEnabled      = false;
        // BookModel renders entitySolid. The lids and flip pages are 0.005 px
        // slabs seen from both sides, so they draw unculled.
        s.cullMode          = CullMode::None;
        s.primitiveType     = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(s);
        g_renderBackend->BindShader(m_shader);
        g_renderBackend->BindTexture(tex, 0);
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", proj * view * model);
        // entitySolid: no alpha test.
        g_renderBackend->SetUniformFloat(m_shader, "uAlphaTest", 0.0f);
        BlockEntityShader::ApplyWorld(m_shader, model, cameraPos, pos);
        const int lighting = FacingSlot(facing) * 2 +
            (EntityLighting::Current() == EntityLighting::LightSet::Nether ? 1 : 0);
        g_renderBackend->DrawIndexed(m_mesh, m_indexCount, m_indexCount * static_cast<uint32_t>(lighting));
        g_renderBackend->UnbindMesh();
    }

} // namespace Render

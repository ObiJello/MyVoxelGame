// File: src/client/renderer/blockentity/ChestRenderer.cpp
//
// In-world chest renderer. Mirrors MC `ChestRenderer.java` + `ChestModel.java`:
// the chest is built from three cuboids — bottom (14×10×14), lid (14×5×14),
// lock (2×4×1) — with UVs baked from the 64×64 chest entity atlas. Lid open
// animation is a Stage-4 concern; this renderer draws the static closed chest.
//
// One static mesh shared across all chest cells; the per-cell model matrix
// is a uniform updated per-draw. Texture is keyed by variant ("normal",
// "trapped", "ender") and loaded on first request from
// `assets/textures/entity/chest/{variant}.png`.
#include "ChestRenderer.hpp"
#include <functional>
#include "../backend/RenderBackend.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include "common/world/block/entity/ChestBlockEntity.hpp"
#include "common/world/block/entity/DoubleChest.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "common/core/Log.hpp"

#include "stb_image.h"
#include <glm/gtc/matrix_transform.hpp>
#include <array>
#include <filesystem>
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

        // GLSL: world-space MVP, sampler2D, vertex-colour modulation.
        // We do the [0,64]→[0,1] UV divide in the vertex shader so the
        // CPU-side UV math stays in MC's native pixel space.
        constexpr const char* kVS = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aUV;     // in 64-px atlas coords
layout(location=2) in vec4 aColor;
uniform mat4 uMVP;
out vec2 vUV;
out vec4 vColor;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vUV = aUV / 64.0;
    vColor = aColor;
}
)GLSL";
        constexpr const char* kFS = R"GLSL(
#version 330 core
in vec2 vUV;
in vec4 vColor;
out vec4 FragColor;
uniform sampler2D uTex;
void main() {
    vec4 t = texture(uTex, vUV);
    if (t.a < 0.05) discard;
    FragColor = t * vColor;
}
)GLSL";

        // Build the 6 faces of one cuboid into the running vertex/index buffer.
        // `from`/`to` in MC pixel-space (0..16 per block). `xTexOffs`/`yTexOffs`
        // are the cube's texOffs in the chest 64×64 atlas. `w`/`h`/`d` are the
        // cube extents in MC pixels.
        void AddCube(std::vector<CubeVert>& verts, std::vector<uint32_t>& idx,
                     glm::vec3 from, glm::vec3 to,
                     float xTexOffs, float yTexOffs, float w, float h, float d) {
            // Vertex names mirror ModelPart.Cube (MC ModelPart.java:268-275).
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

            // Per-face directional shading (MC's standard top=1.0, bottom=0.5,
            // NS=0.8, EW=0.6). Multiplied into vertex colour.
            auto shade = [](float s) -> uint8_t {
                return static_cast<uint8_t>(s * 255.0f);
            };
            const uint8_t S_UP    = shade(1.00f);
            const uint8_t S_DOWN  = shade(0.50f);
            const uint8_t S_NS    = shade(0.80f);
            const uint8_t S_EW    = shade(0.60f);

            // Helper: emit one face = 4 verts + 6 indices (two CCW tris).
            // UV layout is MC ModelPart.Polygon's constructor verbatim:
            //   vertices[0].remap(u1, v0)   ← HIGH u, low v
            //   vertices[1].remap(u0, v0)   ← LOW  u, low v
            //   vertices[2].remap(u0, v1)
            //   vertices[3].remap(u1, v1)
            // Note vertex 0 takes the HIGH u, not the low one. Assigning them
            // the intuitive way round mirrors every face horizontally. That is
            // invisible on the single chest — its front is very nearly
            // symmetric — but on a double chest it swaps each half's inner and
            // outer trim, which is exactly how it was found.
            // (The V order was already correct; swapping it flips faces
            // upside-down instead.)
            auto emit = [&](const glm::vec3 q[4], float U0, float V0, float U1, float V1, uint8_t sh) {
                const uint32_t base = static_cast<uint32_t>(verts.size());
                verts.push_back({q[0].x, q[0].y, q[0].z, U1, V0, sh, sh, sh, 255});
                verts.push_back({q[1].x, q[1].y, q[1].z, U0, V0, sh, sh, sh, 255});
                verts.push_back({q[2].x, q[2].y, q[2].z, U0, V1, sh, sh, sh, 255});
                verts.push_back({q[3].x, q[3].y, q[3].z, U1, V1, sh, sh, sh, 255});
                idx.push_back(base + 0);
                idx.push_back(base + 1);
                idx.push_back(base + 2);
                idx.push_back(base + 0);
                idx.push_back(base + 2);
                idx.push_back(base + 3);
            };

            // DOWN  — [l1, l0, t0, t1]   uv (u1..u2, v0..v1)
            { const glm::vec3 q[4] = {l1, l0, t0, t1};
              emit(q, u1, v0, u2,  v1, S_DOWN); }
            // UP    — [t2, t3, l3, l2]   uv (u2..u22, v0..v1) — v-flipped per MC
            { const glm::vec3 q[4] = {t2, t3, l3, l2};
              emit(q, u2, v1, u22, v0, S_UP); }
            // WEST  — [t0, l0, l3, t3]   uv (u0..u1, v1..v2)
            { const glm::vec3 q[4] = {t0, l0, l3, t3};
              emit(q, u0, v1, u1,  v2, S_EW); }
            // NORTH — [t1, t0, t3, t2]   uv (u1..u2, v1..v2)
            { const glm::vec3 q[4] = {t1, t0, t3, t2};
              emit(q, u1, v1, u2,  v2, S_NS); }
            // EAST  — [l1, t1, t2, l2]   uv (u2..u3, v1..v2)
            { const glm::vec3 q[4] = {l1, t1, t2, l2};
              emit(q, u2, v1, u3,  v2, S_EW); }
            // SOUTH — [l0, l1, l2, l3]   uv (u3..u4, v1..v2)
            { const glm::vec3 q[4] = {l0, l1, l2, l3};
              emit(q, u3, v1, u4,  v2, S_NS); }
        }
    } // namespace

    ChestRenderer::ChestRenderer() = default;
    ChestRenderer::~ChestRenderer() { Shutdown(); }

    bool ChestRenderer::Initialize() {
        if (!g_renderBackend) return false;

        m_shader = g_renderBackend->CreateShader(kVS, kFS);
        if (m_shader == INVALID_SHADER) {
            Log::Error("[ChestRenderer] shader compile failed");
            return false;
        }

        // Build all three chest meshes once. Coordinates are in MC pixel space
        // (1 block = 16 px); the model matrix at render time divides by 16 to
        // land in world units.
        //
        // Boxes are ChestModel.java verbatim, with each PartPose offset folded
        // into the absolute extents (the lid and lock both sit at offset
        // (0,9,1)). The two halves are DELIBERATELY 15 wide rather than 14 and
        // start one pixel off-centre: the right half spans x∈[1,16] and the
        // left x∈[0,15], so once they sit in adjacent cells the seam closes and
        // the pair reads as one 30-wide chest. Building them 14 wide like the
        // single leaves a visible 2px gutter down the middle.
        auto build = [&](Variant v,
                         const std::function<void(std::vector<CubeVert>&,
                                                  std::vector<uint32_t>&)>& emit) {
            std::vector<CubeVert> verts;
            std::vector<uint32_t> idx;
            verts.reserve(72); idx.reserve(108);
            emit(verts, idx);
            m_vb[v] = g_renderBackend->CreateBuffer(BufferUsage::Vertex,
                verts.size() * sizeof(CubeVert), verts.data());
            m_ib[v] = g_renderBackend->CreateBuffer(BufferUsage::Index,
                idx.size() * sizeof(uint32_t), idx.data());
            m_mesh[v] = g_renderBackend->CreateMesh(m_vb[v], m_ib[v], GetBlockVertexLayout());
            m_indexCount[v] = static_cast<uint32_t>(idx.size());
        };

        // createSingleBodyLayer
        build(kSingle, [](auto& verts, auto& idx) {
            AddCube(verts, idx, {1, 0, 1},  {15, 10, 15}, 0, 19, 14, 10, 14);
            AddCube(verts, idx, {1, 9, 1},  {15, 14, 15}, 0,  0, 14,  5, 14);
            AddCube(verts, idx, {7, 7, 15}, { 9, 11, 16}, 0,  0,  2,  4,  1);
        });
        // createDoubleBodyRightLayer — bottom/lid span x 1..16, lock at x 15..16
        build(kRight, [](auto& verts, auto& idx) {
            AddCube(verts, idx, {1, 0, 1},   {16, 10, 15}, 0, 19, 15, 10, 14);
            AddCube(verts, idx, {1, 9, 1},   {16, 14, 15}, 0,  0, 15,  5, 14);
            AddCube(verts, idx, {15, 7, 15}, {16, 11, 16}, 0,  0,  1,  4,  1);
        });
        // createDoubleBodyLeftLayer — bottom/lid span x 0..15, lock at x 0..1
        build(kLeft, [](auto& verts, auto& idx) {
            AddCube(verts, idx, {0, 0, 1},  {15, 10, 15}, 0, 19, 15, 10, 14);
            AddCube(verts, idx, {0, 9, 1},  {15, 14, 15}, 0,  0, 15,  5, 14);
            AddCube(verts, idx, {0, 7, 15}, { 1, 11, 16}, 0,  0,  1,  4,  1);
        });

        m_geomBuilt = (m_mesh[kSingle] != INVALID_MESH &&
                       m_mesh[kLeft]   != INVALID_MESH &&
                       m_mesh[kRight]  != INVALID_MESH);
        return m_geomBuilt;
    }

    void ChestRenderer::Shutdown() {
        if (!g_renderBackend) return;
        for (auto& [k, h] : m_textureCache) {
            if (h != INVALID_TEXTURE) g_renderBackend->DestroyTexture(h);
        }
        m_textureCache.clear();
        for (int v = 0; v < kVariantCount; ++v) {
            if (m_mesh[v] != INVALID_MESH)   { g_renderBackend->DestroyMesh(m_mesh[v]);   m_mesh[v] = INVALID_MESH; }
            if (m_vb[v]   != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(m_vb[v]);   m_vb[v]   = INVALID_BUFFER; }
            if (m_ib[v]   != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(m_ib[v]);   m_ib[v]   = INVALID_BUFFER; }
        }
        if (m_shader != INVALID_SHADER)  { g_renderBackend->DestroyShader(m_shader); m_shader = INVALID_SHADER; }
        m_geomBuilt = false;
    }

    TextureHandle ChestRenderer::LoadVariantTexture(const std::string& variant) {
        auto it = m_textureCache.find(variant);
        if (it != m_textureCache.end()) return it->second;

        const std::string rel  = "assets/textures/entity/chest/" + variant + ".png";
        const std::string full = PlatformMain::GetAssetPath(rel);
        if (!std::filesystem::exists(full)) {
            Log::Warning("[ChestRenderer] missing %s", full.c_str());
            m_textureCache[variant] = INVALID_TEXTURE;
            return INVALID_TEXTURE;
        }

        int w = 0, h = 0, ch = 0;
        stbi_set_flip_vertically_on_load(0);
        unsigned char* px = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha);
        if (!px) {
            m_textureCache[variant] = INVALID_TEXTURE;
            return INVALID_TEXTURE;
        }
        TextureHandle tex = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, px);
        stbi_image_free(px);
        if (tex != INVALID_TEXTURE) {
            g_renderBackend->SetTextureFilter(tex, TextureFilter::Nearest, TextureFilter::Nearest);
            g_renderBackend->SetTextureWrap (tex, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
        }
        m_textureCache[variant] = tex;
        return tex;
    }

    const char* ChestRenderer::VariantForBlock(Game::BlockID id) const {
        switch (id) {
            case Game::BlockID::TrappedChest: return "trapped";
            case Game::BlockID::EnderChest:   return "ender";
            default:                          return "normal";
        }
    }

    void ChestRenderer::Render(const Game::BlockEntity& be,
                                float /*partialTick*/,
                                const glm::mat4& proj,
                                const glm::mat4& view,
                                const glm::vec3& /*cameraPos*/) {
        if (!m_geomBuilt || !g_renderBackend) return;

        // Which of the three models, and which texture. MC ChestRenderer reads
        // ChestBlock.TYPE and asks Sheets.chooseMaterial for
        // normal / normal_left / normal_right; we derive the same split from
        // the world geometry (DoubleChest.hpp) because this engine's chest
        // state carries no `type` property.
        //
        // The SERVER resolves the pair the same way when opening the menu, so
        // a chest that opens as a double always draws as one — the two cannot
        // disagree, because both read the same rule off the same block states.
        Variant variant = kSingle;
        std::string texVariant = VariantForBlock(be.GetBlockId());
        if (Client::g_clientBlockAccess) {
            if (auto pair = Game::FindChestPartner(*Client::g_clientBlockAccess,
                                                   be.GetWorldPos())) {
                // Model and sheet MUST come from the same variant, as MC
                // pairs them (Sheets.chooseMaterial). Each sheet leaves the
                // half's SEAM-side face fully transparent — verified in the
                // pixels: normal_left's WEST region is empty, normal_right's
                // EAST region is empty — because that face is buried inside
                // the joined chest. Pair them crosswise and the transparent
                // region lands on the OUTER face instead, which makes the
                // side of the chest disappear.
                //
                // kLeft  is flush at minX (x 0..15), lock at minX  -> seam WEST
                // kRight is flush at maxX (x 1..16), lock at maxX  -> seam EAST
                // selfIsFirst means the partner sits at this chest's
                // counter-clockwise side, which is local +X, so that chest is
                // the one whose seam must be at maxX: kRight.
                variant     = pair->selfIsFirst ? kRight : kLeft;
                texVariant += pair->selfIsFirst ? "_right" : "_left";
            }
        }

        TextureHandle tex = LoadVariantTexture(texVariant);
        if (tex == INVALID_TEXTURE) return;

        // Per-cell model matrix. Sequence (read bottom-up, applied to vertex
        // first → last):
        //   1. Translate to the block's world origin + (0.5,0,0.5) so the
        //      rotation pivots around the chest's vertical axis at the cell
        //      centre rather than its NW corner.
        //   2. Rotate about Y by the chest's facing direction.
        //   3. Translate back by (-0.5,0,-0.5) (in BLOCK units before the
        //      scale undoes it).
        //   4. Scale by 1/16 so the MC-pixel-space [0,16]³ mesh lands in
        //      world block units.
        //
        // Facing comes off the BLOCK's state, not the block entity — exactly
        // what vanilla does (ChestRenderer.java:67:
        //   state.angle = blockState.getValue(ChestBlock.FACING).toYRot()
        // with the chest's BE carrying no orientation at all). Reading it here
        // rather than caching it on the BE means a chest picks up any state
        // change (placement, /setblock, world load) with no extra sync.
        float yRot = 0.0f;
        {
            const glm::ivec3 p = be.GetWorldPos();
            Game::BlockState state;
            if (Client::g_clientBlockAccess) {
                state = Client::g_clientBlockAccess->GetBlockState(p.x, p.y, p.z);
            }
            const std::string_view facing = state.GetValueByName("facing");
            // MC Direction.toYRot() is degrees clockwise from south; our model
            // is authored with its lock on +Z (south), so south is the zero.
            if      (facing == "east")  yRot =  1.5707963f;
            else if (facing == "north") yRot =  3.1415927f;
            else if (facing == "west")  yRot = -1.5707963f;
            else                        yRot =  0.0f;   // south / unknown
        }

        glm::mat4 model = glm::translate(glm::mat4(1.0f),
            glm::vec3(be.GetWorldPos()) + glm::vec3(0.5f, 0.0f, 0.5f));
        model = glm::rotate(model, yRot, glm::vec3(0, 1, 0));
        model = glm::translate(model, glm::vec3(-0.5f, 0.0f, -0.5f));
        model = glm::scale(model, glm::vec3(1.0f / 16.0f));
        glm::mat4 mvp = proj * view * model;

        PipelineState s;
        s.depthTestEnabled  = true;
        s.depthWriteEnabled = true;
        s.blendEnabled      = false;
        s.cullMode          = CullMode::Back;
        s.primitiveType     = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(s);
        g_renderBackend->BindShader(m_shader);
        g_renderBackend->BindTexture(tex, 0);
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp);
        g_renderBackend->DrawIndexed(m_mesh[variant], m_indexCount[variant]);
        g_renderBackend->UnbindMesh();
    }

    void ChestRenderer::RenderBEWLR(Game::BlockID blockId, const glm::mat4& mvp) {
        if (!m_geomBuilt || !g_renderBackend) return;
        TextureHandle tex = LoadVariantTexture(VariantForBlock(blockId));
        if (tex == INVALID_TEXTURE) return;
        // BEWLR path: caller (HeldItemRenderer or inventory icon) has
        // already baked the projection + view + display transform into
        // mvp; we just bind + draw.
        PipelineState s;
        s.depthTestEnabled  = true;
        s.depthWriteEnabled = true;
        s.depthCompareOp    = CompareOp::LessEqual;
        s.blendEnabled      = true;
        s.srcBlendFactor    = BlendFactor::SrcAlpha;
        s.dstBlendFactor    = BlendFactor::OneMinusSrcAlpha;
        s.cullMode          = CullMode::Back;
        s.primitiveType     = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(s);
        g_renderBackend->BindShader(m_shader);
        g_renderBackend->BindTexture(tex, 0);
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp);
        g_renderBackend->DrawIndexed(m_mesh[kSingle], m_indexCount[kSingle]);
        g_renderBackend->UnbindMesh();
    }

} // namespace Render

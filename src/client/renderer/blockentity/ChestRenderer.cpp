// File: src/client/renderer/blockentity/ChestRenderer.cpp
//
// In-world chest renderer. Mirrors MC `ChestRenderer.java` + `ChestModel.java`:
// the chest is built from three cuboids — bottom (14×10×14), lid (14×5×14),
// lock (2×4×1) — with UVs baked from the 64×64 chest entity atlas. The lid
// and lock swing open on MC's hinge as the block entity's lid controller says
// (ChestBlockEntity: server opener count → block event → client lid tick).
//
// Static meshes shared across all chest cells (body and lid per variant); the
// per-cell model matrix is a uniform updated per-draw. Texture is keyed by variant ("normal",
// "trapped", "ender") and loaded on first request from
// `assets/textures/entity/chest/{variant}.png`.
#include "client/resource/ResourcePacks.hpp"
#include "ChestRenderer.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include <functional>
#include "../backend/RenderBackend.hpp"
#include "../core/RenderOrigin.hpp"
#include "../entity/EntityLighting.hpp"
#include "BlockEntityShader.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include "common/world/block/entity/ChestBlockEntity.hpp"
#include "common/world/block/entity/DoubleChest.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "common/core/Log.hpp"

#include "stb_image.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <array>
#include <cmath>
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

        // Build the 6 faces of one cuboid into the running vertex/index buffer.
        // `from`/`to` in MC pixel-space (0..16 per block). `xTexOffs`/`yTexOffs`
        // are the cube's texOffs in the chest 64×64 atlas. `w`/`h`/`d` are the
        // cube extents in MC pixels.
        //
        // `lighting` picks the per-face shade: null = the item form's
        // block-model face table (the chest icon matches every other block
        // icon); otherwise MC's world-space entity lighting for the chest's
        // Y rotation `worldRot` (EntityLighting.hpp).
        struct FaceLighting {
            glm::mat3 worldRot{1.0f};
            EntityLighting::LightSet set = EntityLighting::LightSet::Default;
        };

        void AddCube(std::vector<CubeVert>& verts, std::vector<uint32_t>& idx,
                     glm::vec3 from, glm::vec3 to,
                     float xTexOffs, float yTexOffs, float w, float h, float d,
                     const FaceLighting* lighting) {
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

            // Per-face shade, multiplied into vertex colour. The model is
            // authored Y-up (no flip), so each face's model normal is its
            // ModelPart.Polygon direction as-is.
            auto shade = [&](const glm::vec3& modelNormal, float itemShade) -> uint8_t {
                if (!lighting) return static_cast<uint8_t>(itemShade * 255.0f);
                return EntityLighting::ShadeByte(lighting->worldRot * modelNormal, lighting->set);
            };
            const uint8_t S_UP    = shade({ 0, 1, 0}, 1.00f);
            const uint8_t S_DOWN  = shade({ 0,-1, 0}, 0.50f);
            const uint8_t S_NORTH = shade({ 0, 0,-1}, 0.80f);
            const uint8_t S_SOUTH = shade({ 0, 0, 1}, 0.80f);
            const uint8_t S_WEST  = shade({-1, 0, 0}, 0.60f);
            const uint8_t S_EAST  = shade({ 1, 0, 0}, 0.60f);

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
              emit(q, u0, v1, u1,  v2, S_WEST); }
            // NORTH — [t1, t0, t3, t2]   uv (u1..u2, v1..v2)
            { const glm::vec3 q[4] = {t1, t0, t3, t2};
              emit(q, u1, v1, u2,  v2, S_NORTH); }
            // EAST  — [l1, t1, t2, l2]   uv (u2..u3, v1..v2)
            { const glm::vec3 q[4] = {l1, t1, t2, l2};
              emit(q, u2, v1, u3,  v2, S_EAST); }
            // SOUTH — [l0, l1, l2, l3]   uv (u3..u4, v1..v2)
            { const glm::vec3 q[4] = {l0, l1, l2, l3};
              emit(q, u3, v1, u4,  v2, S_SOUTH); }
        }
        // MC ChestModel's lid/lock PartPose.offset(0, 9, 1): the hinge, in
        // the model's pixel space.
        const glm::vec3 kLidPivot(0.0f, 9.0f, 1.0f);

        // The lid part's pose at hinge angle `xRot` (MC ModelPart
        // translateAndRotate with the offset folded out of the boxes: turn
        // about the hinge line).
        glm::mat4 LidPose(float xRot) {
            glm::mat4 m = glm::translate(glm::mat4(1.0f), kLidPivot);
            m = glm::rotate(m, xRot, glm::vec3(1, 0, 0));
            return glm::translate(m, -kLidPivot);
        }

        // MC ChestModel.setupAnim: lid.xRot = lock.xRot = -(open · π/2).
        float LidXRot(float open) { return -(open * 1.5707964f); }
    } // namespace

    ChestRenderer::ChestRenderer() = default;
    ChestRenderer::~ChestRenderer() { Shutdown(); }

    bool ChestRenderer::Initialize() {
        if (!g_renderBackend) return false;

        // The shared block-entity shader (BlockEntityShader.hpp): lit and
        // fogged like the terrain, on both backends.
        m_shader = BlockEntityShader::Create();
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
        using Emit = std::function<void(std::vector<CubeVert>&, std::vector<uint32_t>&,
                                        const FaceLighting*)>;
        auto buildPart = [&](Variant v, Part part, const Emit& emit) {
            const int copies = kLightingCount[part];
            std::vector<CubeVert> verts;
            std::vector<uint32_t> idx;
            verts.reserve(48 * copies); idx.reserve(72 * copies);
            for (int lighting = 0; lighting < copies; ++lighting) {
                if (lighting == kItemLighting) {
                    emit(verts, idx, nullptr);
                    continue;
                }
                // Invert BodyLighting / LidLighting: facing (south 0, east
                // 90°, north 180°, west 270° — the Y turn Render applies),
                // light set, and for the lid the hinge angle whose shade this
                // copy carries. The geometry itself is always the closed pose;
                // Render swings it with the part's matrix.
                const int world  = lighting - 1;
                const int angles = (part == kLid) ? kLidAngleSteps + 1 : 1;
                const int setIdx = world / angles;
                const int step   = world % angles;
                const int facing = setIdx / 2;
                FaceLighting fl;
                glm::mat4 turn = glm::rotate(glm::mat4(1.0f), 1.5707963f * static_cast<float>(facing),
                                             glm::vec3(0, 1, 0));
                if (part == kLid) {
                    const float open = static_cast<float>(step) / static_cast<float>(kLidAngleSteps);
                    turn = glm::rotate(turn, LidXRot(open), glm::vec3(1, 0, 0));
                }
                fl.worldRot = glm::mat3(turn);
                fl.set = (setIdx % 2) ? EntityLighting::LightSet::Nether
                                      : EntityLighting::LightSet::Default;
                // AddCube indexes from verts.size(), so every copy's indices
                // already point at its own vertices.
                emit(verts, idx, &fl);
            }
            // AddCube works in the chest sheet's 64-px space; normalize here
            // so the shaders are divide-free.
            for (auto& vert : verts) { vert.u /= 64.0f; vert.v /= 64.0f; }
            m_vb[v][part] = g_renderBackend->CreateBuffer(BufferUsage::Vertex,
                verts.size() * sizeof(CubeVert), verts.data());
            m_ib[v][part] = g_renderBackend->CreateBuffer(BufferUsage::Index,
                idx.size() * sizeof(uint32_t), idx.data());
            m_mesh[v][part] = g_renderBackend->CreateMesh(m_vb[v][part], m_ib[v][part],
                                                          GetBlockVertexLayout());
            m_indexCount[v][part] = static_cast<uint32_t>(idx.size() / copies);
        };
        auto build = [&](Variant v, const Emit& body, const Emit& lid) {
            buildPart(v, kBody, body);
            buildPart(v, kLid, lid);
        };

        // createSingleBodyLayer
        build(kSingle,
              [](auto& verts, auto& idx, const FaceLighting* fl) {
                  AddCube(verts, idx, {1, 0, 1},  {15, 10, 15}, 0, 19, 14, 10, 14, fl);
              },
              [](auto& verts, auto& idx, const FaceLighting* fl) {
                  AddCube(verts, idx, {1, 9, 1},  {15, 14, 15}, 0,  0, 14,  5, 14, fl);
                  AddCube(verts, idx, {7, 7, 15}, { 9, 11, 16}, 0,  0,  2,  4,  1, fl);
              });
        // createDoubleBodyRightLayer — bottom/lid span x 1..16, lock at x 15..16
        build(kRight,
              [](auto& verts, auto& idx, const FaceLighting* fl) {
                  AddCube(verts, idx, {1, 0, 1},   {16, 10, 15}, 0, 19, 15, 10, 14, fl);
              },
              [](auto& verts, auto& idx, const FaceLighting* fl) {
                  AddCube(verts, idx, {1, 9, 1},   {16, 14, 15}, 0,  0, 15,  5, 14, fl);
                  AddCube(verts, idx, {15, 7, 15}, {16, 11, 16}, 0,  0,  1,  4,  1, fl);
              });
        // createDoubleBodyLeftLayer — bottom/lid span x 0..15, lock at x 0..1
        build(kLeft,
              [](auto& verts, auto& idx, const FaceLighting* fl) {
                  AddCube(verts, idx, {0, 0, 1},  {15, 10, 15}, 0, 19, 15, 10, 14, fl);
              },
              [](auto& verts, auto& idx, const FaceLighting* fl) {
                  AddCube(verts, idx, {0, 9, 1},  {15, 14, 15}, 0,  0, 15,  5, 14, fl);
                  AddCube(verts, idx, {0, 7, 15}, { 1, 11, 16}, 0,  0,  1,  4,  1, fl);
              });

        m_geomBuilt = true;
        for (int v = 0; v < kVariantCount; ++v)
            for (int part = 0; part < kPartCount; ++part)
                if (m_mesh[v][part] == INVALID_MESH) m_geomBuilt = false;
        return m_geomBuilt;
    }

    void ChestRenderer::Shutdown() {
        if (!g_renderBackend) return;
        for (auto& [k, h] : m_textureCache) {
            if (h != INVALID_TEXTURE) g_renderBackend->DestroyTexture(h);
        }
        m_textureCache.clear();
        for (int v = 0; v < kVariantCount; ++v) {
            for (int part = 0; part < kPartCount; ++part) {
                MeshHandle&   mesh = m_mesh[v][part];
                BufferHandle& vb   = m_vb[v][part];
                BufferHandle& ib   = m_ib[v][part];
                if (mesh != INVALID_MESH) { g_renderBackend->DestroyMesh(mesh);   mesh = INVALID_MESH; }
                if (vb   != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(vb); vb   = INVALID_BUFFER; }
                if (ib   != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(ib); ib   = INVALID_BUFFER; }
            }
        }
        if (m_shader != INVALID_SHADER)  { g_renderBackend->DestroyShader(m_shader); m_shader = INVALID_SHADER; }
        m_geomBuilt = false;
    }

    TextureHandle ChestRenderer::LoadVariantTexture(const std::string& variant) {
        if (Resources::CacheStale(m_textureCacheGeneration)) {
            // A resource pack change replaces every texture here.
            if (g_renderBackend) for (auto& [key, tex] : m_textureCache) if (tex != INVALID_TEXTURE) g_renderBackend->DestroyTexture(tex);
            m_textureCache.clear();
        }
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

    void ChestRenderer::DrawPart(Variant variant, Part part, int lighting) {
        g_renderBackend->DrawIndexed(m_mesh[variant][part], m_indexCount[variant][part],
                                     m_indexCount[variant][part] * static_cast<uint32_t>(lighting));
    }

    void ChestRenderer::Render(const Game::BlockEntity& be,
                                float partialTick,
                                const glm::mat4& proj,
                                const glm::mat4& view,
                                const glm::vec3& cameraPos) {
        PROFILE_ZONE_N("BE.Chest");
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
        // MC ChestBlock.opennessCombiner: a pair opens as one — the wider of
        // the two halves' lids.
        const auto* chest = dynamic_cast<const Game::ChestBlockEntity*>(&be);
        float open = chest ? chest->GetOpenNess(partialTick) : 0.0f;
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
                if (const auto* partner = dynamic_cast<const Game::ChestBlockEntity*>(
                        Client::g_clientBlockAccess->GetBlockEntity(pair->partnerPos))) {
                    open = std::max(open, partner->GetOpenNess(partialTick));
                }
            }
        }
        // MC ChestRenderer.submit's ease: 1 - (1 - open)³ — quick to lift,
        // settling as it reaches the top.
        open = 1.0f - open;
        open = 1.0f - open * open * open;

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
        int facingIndex = 0;   // south, east, north, west — WorldLighting's order
        {
            const glm::ivec3 p = be.GetWorldPos();
            Game::BlockState state;
            if (Client::g_clientBlockAccess) {
                state = Client::g_clientBlockAccess->GetBlockState(p.x, p.y, p.z);
            }
            const std::string_view facing = state.GetValueByName("facing");
            // MC Direction.toYRot() is degrees clockwise from south; our model
            // is authored with its lock on +Z (south), so south is the zero.
            if      (facing == "east")  { yRot =  1.5707963f; facingIndex = 1; }
            else if (facing == "north") { yRot =  3.1415927f; facingIndex = 2; }
            else if (facing == "west")  { yRot = -1.5707963f; facingIndex = 3; }
            else                        { yRot =  0.0f;       facingIndex = 0; }   // south / unknown
        }

        // Model translation in RENDER space (camera-relative, see
        // RenderOrigin.hpp): the block position minus the view's origin,
        // subtracted before the narrowing to float.
        glm::mat4 model = glm::translate(glm::mat4(1.0f),
            Render::ToRender(glm::dvec3(be.GetWorldPos())) + glm::vec3(0.5f, 0.0f, 0.5f));
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
        g_renderBackend->SetUniformFloat(m_shader, "uAlphaTest", 0.05f);
        // The chest's light (its cell's) and the frame's fog.
        BlockEntityShader::ApplyWorld(m_shader, model, cameraPos, be.GetWorldPos());
        // MC lights the chest from fixed world directions; the copies for this
        // facing and the drawn level's light set carry that shade.
        const bool nether = EntityLighting::Current() == EntityLighting::LightSet::Nether;
        DrawPart(variant, kBody, BodyLighting(facingIndex, nether));

        // The lid and lock on the hinge, lit for the nearest baked angle.
        const glm::mat4 lidModel = model * LidPose(LidXRot(open));
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", proj * view * lidModel);
        BlockEntityShader::ApplyWorld(m_shader, lidModel, cameraPos, be.GetWorldPos());
        const int step = std::clamp(static_cast<int>(std::lround(open * kLidAngleSteps)),
                                    0, kLidAngleSteps);
        DrawPart(variant, kLid, LidLighting(facingIndex, nether, step));
        g_renderBackend->UnbindMesh();
    }

    void ChestRenderer::RenderBEWLR(Game::BlockID blockId, const glm::mat4& mvp,
                                    const BEWLRLight& light) {
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
        g_renderBackend->SetUniformFloat(m_shader, "uAlphaTest", 0.05f);
        BlockEntityShader::ApplyItem(m_shader, light);
        // The item form is always closed: body and lid in their rest pose.
        DrawPart(kSingle, kBody, kItemLighting);
        DrawPart(kSingle, kLid,  kItemLighting);
        g_renderBackend->UnbindMesh();
    }

} // namespace Render

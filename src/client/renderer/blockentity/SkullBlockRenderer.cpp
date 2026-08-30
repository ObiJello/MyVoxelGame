// File: src/client/renderer/blockentity/SkullBlockRenderer.cpp
//
// See header. Geometry sources, cube for cube:
//   SkullModel.createMobHeadLayer      — skeleton / wither skeleton / creeper
//                                        (8×8×8 head, 64×32 sheet)
//   SkullModel.createHumanoidHeadLayer — player / zombie (head + inflated hat
//                                        overlay at texOffs 32,0; 64×64 sheet)
//   PiglinHeadModel / AbstractPiglinModel.addHead — 10×8×8 head, snout, two
//                                        tusks and the two rotated ears
//   DragonHeadModel.createHeadLayer    — 7 boxes (2 mirrored) on the 256×256
//                                        enderdragon sheet, jaw child part
//
// Meshes are baked in MC's model-pixel space EXACTLY as authored (y-down,
// face at -Z). The draw matrix then replays MC SkullBlockRenderer.submitSkull:
//
//   translate(0.5, 0, 0.5)                       floor skulls, or
//   translate(0.5 - stepX*0.25, 0.25,
//             0.5 - stepZ*0.25)                  wall skulls, then
//   scale(-1, -1, 1)                             MC's model-space flip, then
//   rotateY(rotationDegrees)                     head.yRot from setupAnim, then
//   scale(1/16)                                  pixels → world units
//
// so the y-down mesh needs no hand-conversion and every translation/angle can
// be checked against the Java line it came from.
#include "SkullBlockRenderer.hpp"
#include "../backend/RenderBackend.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "common/core/Log.hpp"

#include "stb_image.h"
#include <glm/gtc/matrix_transform.hpp>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
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

        // Same shape as ChestRenderer's shader, but the UV divisor is a
        // uniform: skull sheets come in 64×32 (mob heads), 64×64 (humanoid +
        // piglin) and 256×256 (dragon), where the chest atlas is always 64².
        constexpr const char* kVS = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aUV;     // in texture-pixel coords
layout(location=2) in vec4 aColor;
uniform mat4 uMVP;
uniform vec2 uTexSize;
out vec2 vUV;
out vec4 vColor;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vUV = aUV / uTexSize;
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
    if (t.a < 0.1) discard;
    FragColor = t * vColor;
}
)GLSL";

        // MC SkullBlockRenderer.SKIN_BY_TYPE, plus each sheet's dimensions
        // (needed for the UV divide — MC bakes them into the LayerDefinition).
        struct KindInfo {
            const char* texRel;   // under assets/textures/
            float texW, texH;
        };
        constexpr KindInfo kKindInfo[] = {
            { "entity/skeleton/skeleton.png",        64.0f,  32.0f },   // skeleton
            { "entity/skeleton/wither_skeleton.png", 64.0f,  32.0f },   // wither skeleton
            // MC PLAYER falls back to DefaultPlayerSkin when the head has no
            // owner profile; this port has no profile plumbing, so every
            // player head wears the default (steve) skin.
            { "entity/player/wide/steve.png",        64.0f,  64.0f },   // player
            { "entity/zombie/zombie.png",            64.0f,  64.0f },   // zombie
            { "entity/creeper/creeper.png",          64.0f,  32.0f },   // creeper
            { "entity/piglin/piglin.png",            64.0f,  64.0f },   // piglin
            { "entity/enderdragon/dragon.png",       256.0f, 256.0f },  // dragon
        };

        // Emit one cuboid, replicating ModelPart.Cube's vertex + UV layout
        // (ModelPart.java Cube ctor — the same code ChestRenderer.AddCube
        // ports; re-derived here rather than shared because this one adds the
        // per-cube pixel-space transform and the mirror flag the chest never
        // needs).
        //
        // `from`/`to` are the box corners AFTER any CubeDeformation growth;
        // `w`/`h`/`d` are the UNINFLATED extents, which is what the UV windows
        // are built from (growing a cube never moves its texture patch).
        // `xform` is the baked part pose (offset · rotation · part scale) in
        // pixel space. `mirror` swaps minX/maxX exactly as MC's Cube ctor
        // does — the UV windows stay put, so each face's texture flips
        // horizontally with its geometry. (MC also reverses the polygon's
        // vertex order, but that only flips winding and this renderer draws
        // skulls uncullled — entityCutoutNoCull — so it is dropped.)
        void AddCube(std::vector<CubeVert>& verts, std::vector<uint32_t>& idx,
                     glm::vec3 from, glm::vec3 to,
                     float xTexOffs, float yTexOffs, float w, float h, float d,
                     const glm::mat4& xform, bool mirror) {
            float minX = from.x, minY = from.y, minZ = from.z;
            float maxX = to.x,   maxY = to.y,   maxZ = to.z;
            if (mirror) std::swap(minX, maxX);

            auto tp = [&](float x, float y, float z) {
                return glm::vec3(xform * glm::vec4(x, y, z, 1.0f));
            };
            // Vertex names mirror ModelPart.Cube (t = minZ plane, l = maxZ).
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

            // Directional shade, chest-renderer style. The mesh is y-down and
            // the draw matrix applies scale(-1,-1,1), so the model-space minY
            // faces UP in the world: the "DOWN" polygon gets the top shade and
            // vice versa. NORTH keeps north (z is not flipped). Rotated parts
            // (piglin ears, dragon jaw) take the same per-face constants as an
            // approximation, exactly as the campfire food quads do.
            auto shade = [](float s) -> uint8_t {
                return static_cast<uint8_t>(s * 255.0f);
            };
            const uint8_t S_TOP    = shade(1.00f);   // model DOWN → world up
            const uint8_t S_BOTTOM = shade(0.50f);   // model UP   → world down
            const uint8_t S_NS     = shade(0.80f);
            const uint8_t S_EW     = shade(0.60f);

            // MC ModelPart.Polygon ctor verbatim: vertex 0 takes the HIGH u.
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
              emit(q, u1, v0, u2,  v1, S_TOP); }
            // UP    — [t2, t3, l3, l2]   uv (u2..u22, v1..v0) — v-flipped per MC
            { const glm::vec3 q[4] = {t2, t3, l3, l2};
              emit(q, u2, v1, u22, v0, S_BOTTOM); }
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

        // Wall-skull FACING → MC state.rotationDegrees:
        // RotationSegment.convertToDegrees(convertToSegment(facing.opposite)),
        // i.e. SegmentedAnglePrecision(4).fromDirection = get2DDataValue << 2
        // (S=0, W=4, N=8, E=12) read back at 22.5°/segment. The ±360 the
        // toDegrees normalisation adds is irrelevant to a rotation matrix.
        float WallFacingDegrees(std::string_view facing) {
            if (facing == "south") return 180.0f;   // opposite north  → seg 8
            if (facing == "west")  return 270.0f;   // opposite east   → seg 12
            if (facing == "east")  return  90.0f;   // opposite west   → seg 4
            return 0.0f;                            // north: opposite south → 0
        }

        int WallFacingStepX(std::string_view facing) {
            if (facing == "west") return -1;
            if (facing == "east") return  1;
            return 0;
        }
        int WallFacingStepZ(std::string_view facing) {
            if (facing == "north") return -1;
            if (facing == "south") return  1;
            return 0;
        }

    } // namespace

    SkullBlockRenderer::SkullBlockRenderer() = default;
    SkullBlockRenderer::~SkullBlockRenderer() { Shutdown(); }

    void SkullBlockRenderer::ClassifyBlock(Game::BlockID id, int& outKind, bool& outWall) {
        using B = Game::BlockID;
        switch (id) {
            case B::SkeletonSkull:           outKind = kSkeleton;       outWall = false; return;
            case B::SkeletonWallSkull:       outKind = kSkeleton;       outWall = true;  return;
            case B::WitherSkeletonSkull:     outKind = kWitherSkeleton; outWall = false; return;
            case B::WitherSkeletonWallSkull: outKind = kWitherSkeleton; outWall = true;  return;
            case B::PlayerHead:              outKind = kPlayer;         outWall = false; return;
            case B::PlayerWallHead:          outKind = kPlayer;         outWall = true;  return;
            case B::ZombieHead:              outKind = kZombie;         outWall = false; return;
            case B::ZombieWallHead:          outKind = kZombie;         outWall = true;  return;
            case B::CreeperHead:             outKind = kCreeper;        outWall = false; return;
            case B::CreeperWallHead:         outKind = kCreeper;        outWall = true;  return;
            case B::PiglinHead:              outKind = kPiglin;         outWall = false; return;
            case B::PiglinWallHead:          outKind = kPiglin;         outWall = true;  return;
            case B::DragonHead:              outKind = kDragon;         outWall = false; return;
            case B::DragonWallHead:          outKind = kDragon;         outWall = true;  return;
            default:                         outKind = -1;              outWall = false; return;
        }
    }

    bool SkullBlockRenderer::Initialize() {
        if (!g_renderBackend) return false;

        m_shader = g_renderBackend->CreateShader(kVS, kFS);
        if (m_shader == INVALID_SHADER) {
            Log::Error("[SkullRenderer] shader compile failed");
            return false;
        }

        auto build = [&](int kind, auto&& emit) {
            std::vector<CubeVert> verts;
            std::vector<uint32_t> idx;
            verts.reserve(7 * 24); idx.reserve(7 * 36);
            emit(verts, idx);
            m_vb[kind] = g_renderBackend->CreateBuffer(BufferUsage::Vertex,
                verts.size() * sizeof(CubeVert), verts.data());
            m_ib[kind] = g_renderBackend->CreateBuffer(BufferUsage::Index,
                idx.size() * sizeof(uint32_t), idx.data());
            m_mesh[kind] = g_renderBackend->CreateMesh(m_vb[kind], m_ib[kind],
                                                       GetBlockVertexLayout());
            m_indexCount[kind] = static_cast<uint32_t>(idx.size());
        };

        const glm::mat4 I(1.0f);

        // SkullModel.createHeadModel: one 8×8×8 cube, texOffs(0,0),
        // addBox(-4, -8, -4, 8, 8, 8) at PartPose.ZERO.
        auto mobHead = [&](std::vector<CubeVert>& v, std::vector<uint32_t>& i) {
            AddCube(v, i, {-4, -8, -4}, {4, 0, 4}, 0, 0, 8, 8, 8, I, false);
        };
        build(kSkeleton,       mobHead);
        build(kWitherSkeleton, mobHead);
        build(kCreeper,        mobHead);

        // SkullModel.createHumanoidHeadLayer: the head plus the "hat" overlay
        // — same 8×8×8 box grown by CubeDeformation(0.25), texOffs(32,0).
        auto humanoidHead = [&](std::vector<CubeVert>& v, std::vector<uint32_t>& i) {
            AddCube(v, i, {-4, -8, -4}, {4, 0, 4}, 0, 0, 8, 8, 8, I, false);
            AddCube(v, i, {-4.25f, -8.25f, -4.25f}, {4.25f, 0.25f, 4.25f},
                    32, 0, 8, 8, 8, I, false);
        };
        build(kPlayer, humanoidHead);
        build(kZombie, humanoidHead);

        // PiglinHeadModel.createHeadModel → AbstractPiglinModel.addHead:
        // 10×8×8 head, 4×4×1 snout, two 1×2×1 tusks, and the two ears.
        // The ears' PartPose carries zRot ∓π/6, but PiglinHeadModel.setupAnim
        // overwrites it every frame: at animationPos = 0 (an unpowered head —
        // the note-block wiggle isn't modelled here) that is
        //   leftEar.zRot  = -(cos(0) + 2.5) * 0.2 = -0.7
        //   rightEar.zRot = +(cos(0) + 2.5) * 0.2 = +0.7
        // so ±0.7 rad is what gets baked.
        build(kPiglin, [&](std::vector<CubeVert>& v, std::vector<uint32_t>& i) {
            AddCube(v, i, {-5, -8, -4}, {5, 0, 4},  0, 0, 10, 8, 8, I, false);
            AddCube(v, i, {-2, -4, -5}, {2, 0, -4}, 31, 1, 4, 4, 1, I, false);
            AddCube(v, i, { 2, -2, -5}, {3, 0, -4},  2, 4, 1, 2, 1, I, false);
            AddCube(v, i, {-3, -2, -5}, {-2, 0, -4}, 2, 0, 1, 2, 1, I, false);
            glm::mat4 leftEar = glm::translate(I, {4.5f, -6.0f, 0.0f});
            leftEar = glm::rotate(leftEar, -0.7f, glm::vec3(0, 0, 1));
            AddCube(v, i, {0, 0, -2}, {1, 5, 2}, 51, 6, 1, 5, 4, leftEar, false);
            glm::mat4 rightEar = glm::translate(I, {-4.5f, -6.0f, 0.0f});
            rightEar = glm::rotate(rightEar, 0.7f, glm::vec3(0, 0, 1));
            AddCube(v, i, {-1, 0, -2}, {0, 5, 2}, 39, 6, 1, 5, 4, rightEar, false);
        });

        // DragonHeadModel.createHeadLayer. The head part sits at
        // PartPose.offset(0, -7.986666, 0).scaled(0.75); the jaw is its child
        // at offset(0, 4, -8). setupAnim at animationPos = 0 leaves the jaw at
        // xRot = (sin(0) + 1) * 0.2 = 0.2 rad. The two "scale" spikes and the
        // left nostril are authored under .mirror(true).
        build(kDragon, [&](std::vector<CubeVert>& v, std::vector<uint32_t>& i) {
            glm::mat4 head = glm::translate(I, {0.0f, -7.986666f, 0.0f});
            head = glm::scale(head, glm::vec3(0.75f));
            AddCube(v, i, {-6, -1, -24}, {6, 4, -8},   176, 44, 12, 5, 16, head, false);
            AddCube(v, i, {-8, -8, -10}, {8, 8, 6},    112, 30, 16, 16, 16, head, false);
            AddCube(v, i, {-5, -12, -4}, {-3, -8, 2},    0, 0, 2, 4, 6, head, true);
            AddCube(v, i, {-5, -3, -22}, {-3, -1, -18}, 112, 0, 2, 2, 4, head, true);
            AddCube(v, i, { 3, -12, -4}, {5, -8, 2},     0, 0, 2, 4, 6, head, false);
            AddCube(v, i, { 3, -3, -22}, {5, -1, -18},  112, 0, 2, 2, 4, head, false);
            glm::mat4 jaw = glm::translate(head, {0.0f, 4.0f, -8.0f});
            jaw = glm::rotate(jaw, 0.2f, glm::vec3(1, 0, 0));
            AddCube(v, i, {-6, 0, -16}, {6, 4, 0}, 176, 65, 12, 4, 16, jaw, false);
        });

        m_geomBuilt = true;
        for (int k = 0; k < kKindCount; ++k) {
            if (m_mesh[k] == INVALID_MESH) m_geomBuilt = false;
        }
        return m_geomBuilt;
    }

    void SkullBlockRenderer::Shutdown() {
        if (!g_renderBackend) return;
        for (int k = 0; k < kKindCount; ++k) {
            if (m_tex[k]  != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(m_tex[k]); m_tex[k] = INVALID_TEXTURE; }
            if (m_mesh[k] != INVALID_MESH)    { g_renderBackend->DestroyMesh(m_mesh[k]);   m_mesh[k] = INVALID_MESH; }
            if (m_vb[k]   != INVALID_BUFFER)  { g_renderBackend->DestroyBuffer(m_vb[k]);   m_vb[k]   = INVALID_BUFFER; }
            if (m_ib[k]   != INVALID_BUFFER)  { g_renderBackend->DestroyBuffer(m_ib[k]);   m_ib[k]   = INVALID_BUFFER; }
            m_texTried[k] = false;
        }
        if (m_shader != INVALID_SHADER) { g_renderBackend->DestroyShader(m_shader); m_shader = INVALID_SHADER; }
        m_geomBuilt = false;
    }

    TextureHandle SkullBlockRenderer::LoadKindTexture(int kind) {
        if (kind < 0 || kind >= kKindCount) return INVALID_TEXTURE;
        if (m_texTried[kind]) return m_tex[kind];
        m_texTried[kind] = true;

        const std::string rel  = std::string("assets/textures/") + kKindInfo[kind].texRel;
        const std::string full = PlatformMain::GetAssetPath(rel);
        if (!std::filesystem::exists(full)) {
            Log::Warning("[SkullRenderer] missing %s", full.c_str());
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
        m_tex[kind] = tex;
        return tex;
    }

    void SkullBlockRenderer::Render(const Game::BlockEntity& be,
                                    float /*partialTick*/,
                                    const glm::mat4& proj,
                                    const glm::mat4& view,
                                    const glm::vec3& /*cameraPos*/) {
        if (!m_geomBuilt || !g_renderBackend) return;

        int  kind = -1;
        bool wall = false;
        ClassifyBlock(be.GetBlockId(), kind, wall);
        if (kind < 0) return;

        TextureHandle tex = LoadKindTexture(kind);
        if (tex == INVALID_TEXTURE) return;

        // Orientation comes off the BLOCK state, exactly like vanilla
        // (SkullBlockRenderer.extractRenderState reads WallSkullBlock.FACING /
        // SkullBlock.ROTATION off the blockstate; the BE stores none of it).
        const glm::ivec3 pos = be.GetWorldPos();
        Game::BlockState state;
        if (Client::g_clientBlockAccess) {
            state = Client::g_clientBlockAccess->GetBlockState(pos.x, pos.y, pos.z);
        }

        float rotDeg = 0.0f;
        glm::vec3 offset(0.5f, 0.0f, 0.5f);
        if (wall) {
            const std::string_view facing = state.GetValueByName("facing");
            rotDeg = WallFacingDegrees(facing);
            // MC submitSkull: translate(0.5 - stepX*0.25, 0.25, 0.5 - stepZ*0.25)
            // — a quarter block off centre AWAY from the facing, i.e. pressed
            // against the wall the skull hangs on, and a quarter block up.
            offset = glm::vec3(0.5f - WallFacingStepX(facing) * 0.25f,
                               0.25f,
                               0.5f - WallFacingStepZ(facing) * 0.25f);
        } else {
            // ROTATION is the 16-segment value itself; convertToDegrees is
            // segment * 360/16 = 22.5°.
            const std::string_view rot = state.GetValueByName("rotation");
            if (!rot.empty()) {
                rotDeg = 22.5f * static_cast<float>(std::atoi(std::string(rot).c_str()));
            }
        }

        // MC submitSkull's pose stack, in order (see file header).
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(pos) + offset);
        model = glm::scale(model, glm::vec3(-1.0f, -1.0f, 1.0f));
        model = glm::rotate(model, glm::radians(rotDeg), glm::vec3(0, 1, 0));
        model = glm::scale(model, glm::vec3(1.0f / 16.0f));

        PipelineState s;
        s.depthTestEnabled  = true;
        s.depthWriteEnabled = true;
        s.blendEnabled      = false;
        // MC renders skulls with entityCutoutNoCullZOffset: no face culling
        // (a transparent hat pixel must show the head beneath, and the two
        // mirrored dragon cubes have inverted winding), alpha-tested cutout.
        s.cullMode          = CullMode::None;
        s.primitiveType     = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(s);
        g_renderBackend->BindShader(m_shader);
        g_renderBackend->BindTexture(tex, 0);
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", proj * view * model);
        g_renderBackend->SetUniformVec2(m_shader, "uTexSize",
                                        glm::vec2(kKindInfo[kind].texW, kKindInfo[kind].texH));
        g_renderBackend->DrawIndexed(m_mesh[kind], m_indexCount[kind]);
        g_renderBackend->UnbindMesh();
    }

} // namespace Render

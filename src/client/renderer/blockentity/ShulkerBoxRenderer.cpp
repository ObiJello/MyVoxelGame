// File: src/client/renderer/blockentity/ShulkerBoxRenderer.cpp
#include "ShulkerBoxRenderer.hpp"
#include "../backend/RenderBackend.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/Direction.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "common/core/Log.hpp"

#include "stb_image.h"
#include <glm/gtc/matrix_transform.hpp>
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

        // Same pair as ChestRenderer: the [0,64] -> [0,1] UV divide happens in
        // the vertex shader so the CPU side stays in MC's pixel space.
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

        // Identical to ChestRenderer's AddCube — MC ModelPart.Cube's six faces
        // with its exact UV layout, including the quirk that vertex 0 takes the
        // HIGH u (getting that backwards mirrors every face horizontally).
        //
        // Deliberately duplicated rather than shared: the two renderers are the
        // only users, and hoisting it into a common header would mean exporting
        // CubeVert and the vertex layout assumption along with it. If a third
        // entity-model renderer appears, that is the moment to lift all three.
        void AddCube(std::vector<CubeVert>& verts, std::vector<uint32_t>& idx,
                     glm::vec3 from, glm::vec3 to,
                     float xTexOffs, float yTexOffs, float w, float h, float d) {
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

            auto shade = [](float s) -> uint8_t {
                return static_cast<uint8_t>(s * 255.0f);
            };
            const uint8_t S_UP   = shade(1.00f);
            const uint8_t S_DOWN = shade(0.50f);
            const uint8_t S_NS   = shade(0.80f);
            const uint8_t S_EW   = shade(0.60f);

            auto emit = [&](const glm::vec3 q[4], float U0, float V0, float U1, float V1, uint8_t sh) {
                const uint32_t base = static_cast<uint32_t>(verts.size());
                verts.push_back({q[0].x, q[0].y, q[0].z, U1, V0, sh, sh, sh, 255});
                verts.push_back({q[1].x, q[1].y, q[1].z, U0, V0, sh, sh, sh, 255});
                verts.push_back({q[2].x, q[2].y, q[2].z, U0, V1, sh, sh, sh, 255});
                verts.push_back({q[3].x, q[3].y, q[3].z, U1, V1, sh, sh, sh, 255});
                idx.push_back(base + 0); idx.push_back(base + 1); idx.push_back(base + 2);
                idx.push_back(base + 0); idx.push_back(base + 2); idx.push_back(base + 3);
            };

            // Face order and vertex winding verbatim from ModelPart.Cube's
            // polygon list (down, up, north(-Z), south(+Z), west(-X), east(+X)).
            const glm::vec3 fDown [4] = {l1, l0, t0, t1};
            const glm::vec3 fUp   [4] = {t2, t3, l3, l2};
            const glm::vec3 fNorth[4] = {t1, t0, t3, t2};
            const glm::vec3 fSouth[4] = {l0, l1, l2, l3};
            const glm::vec3 fWest [4] = {t0, l0, l3, t3};
            const glm::vec3 fEast [4] = {l1, t1, t2, l2};

            emit(fDown,  u1,  v0, u2,  v1, S_DOWN);
            emit(fUp,    u2,  v1, u22, v0, S_UP);
            emit(fNorth, u1,  v1, u2,  v2, S_NS);
            emit(fSouth, u3,  v1, u4,  v2, S_NS);
            emit(fWest,  u0,  v1, u1,  v2, S_EW);
            emit(fEast,  u2,  v1, u3,  v2, S_EW);
        }

        // MC Direction.getRotation() (Direction.java:138-150). Ordinals match
        // this engine's Direction enum exactly (Down=0 … East=5).
        //
        // JOML's rotationXYZ(ax, 0, az) composes as Rx * Rz — Z applied to the
        // vector first — so each entry below is built in that order. glm::rotate
        // post-multiplies, which gives exactly that when applied in sequence.
        glm::mat4 FacingRotation(Game::Direction facing) {
            constexpr float kPi     = 3.14159265358979323846f;
            constexpr float kHalfPi = kPi * 0.5f;
            const glm::vec3 X(1, 0, 0), Z(0, 0, 1);
            glm::mat4 m(1.0f);
            switch (facing) {
                case Game::Direction::Down:  m = glm::rotate(m, kPi, X); break;
                case Game::Direction::Up:    break;                       // identity
                case Game::Direction::North: m = glm::rotate(m, kHalfPi, X);
                                             m = glm::rotate(m, kPi, Z);      break;
                case Game::Direction::South: m = glm::rotate(m, kHalfPi, X);  break;
                case Game::Direction::West:  m = glm::rotate(m, kHalfPi, X);
                                             m = glm::rotate(m, kHalfPi, Z);  break;
                case Game::Direction::East:  m = glm::rotate(m, kHalfPi, X);
                                             m = glm::rotate(m, -kHalfPi, Z); break;
            }
            return m;
        }

        // MC ShulkerBoxBlock's default facing is UP (registerDefaultState),
        // which is also what ShulkerBoxRenderer falls back to
        // (getValueOrElse(FACING, Direction.UP)).
        Game::Direction FacingOf(Game::BlockState state) {
            const std::string_view f = state.GetValueByName("facing");
            if (f == "down")  return Game::Direction::Down;
            if (f == "up")    return Game::Direction::Up;
            if (f == "north") return Game::Direction::North;
            if (f == "south") return Game::Direction::South;
            if (f == "west")  return Game::Direction::West;
            if (f == "east")  return Game::Direction::East;
            return Game::Direction::Up;
        }
    } // namespace

    ShulkerBoxRenderer::ShulkerBoxRenderer() = default;
    ShulkerBoxRenderer::~ShulkerBoxRenderer() { Shutdown(); }

    bool ShulkerBoxRenderer::Initialize() {
        if (!g_renderBackend) return false;

        m_shader = g_renderBackend->CreateShader(kVS, kFS);
        if (m_shader == INVALID_SHADER) {
            Log::Error("[ShulkerBoxRenderer] shader compile failed");
            return false;
        }

        // Cubes are ShulkerModel.createShellMesh() verbatim, in RAW MODEL
        // units and RELATIVE TO THEIR PART ORIGIN — the PartPose offset is NOT
        // folded in the way ChestRenderer folds its lid's, because the lid's
        // offset is what the open animation changes. PartMatrix applies it.
        //
        // Keeping the coordinates in MC's own frame (rather than pre-flipping
        // them into world space) is deliberate: prepareModel's scale(1,-1,-1)
        // is what decides which texture region lands on the visible top face,
        // and reproducing it in the matrix means AddCube's UV layout stays
        // untouched. Bake the flip into the vertices instead and the world-up
        // face silently samples the atlas's DOWN region.
        auto build = [&](Part p, glm::vec3 from, glm::vec3 to,
                         float tx, float ty, float w, float h, float d) {
            std::vector<CubeVert> verts;
            std::vector<uint32_t> idx;
            verts.reserve(24); idx.reserve(36);
            AddCube(verts, idx, from, to, tx, ty, w, h, d);
            m_vb[p] = g_renderBackend->CreateBuffer(BufferUsage::Vertex,
                verts.size() * sizeof(CubeVert), verts.data());
            m_ib[p] = g_renderBackend->CreateBuffer(BufferUsage::Index,
                idx.size() * sizeof(uint32_t), idx.data());
            m_mesh[p] = g_renderBackend->CreateMesh(m_vb[p], m_ib[p], GetBlockVertexLayout());
            m_indexCount[p] = static_cast<uint32_t>(idx.size());
        };

        // base: texOffs(0, 28) addBox(-8, -8, -8, 16, 8, 16)
        build(kBase, {-8, -8, -8}, {8,  0, 8}, 0, 28, 16,  8, 16);
        // lid:  texOffs(0,  0) addBox(-8, -16, -8, 16, 12, 16)
        build(kLid,  {-8, -16, -8}, {8, -4, 8}, 0,  0, 16, 12, 16);

        m_geomBuilt = (m_mesh[kBase] != INVALID_MESH && m_mesh[kLid] != INVALID_MESH);
        return m_geomBuilt;
    }

    void ShulkerBoxRenderer::Shutdown() {
        if (!g_renderBackend) return;
        for (auto& [k, h] : m_textureCache) {
            if (h != INVALID_TEXTURE) g_renderBackend->DestroyTexture(h);
        }
        m_textureCache.clear();
        for (int p = 0; p < kPartCount; ++p) {
            if (m_mesh[p] != INVALID_MESH)   { g_renderBackend->DestroyMesh(m_mesh[p]);   m_mesh[p] = INVALID_MESH; }
            if (m_vb[p]   != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(m_vb[p]);   m_vb[p]   = INVALID_BUFFER; }
            if (m_ib[p]   != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(m_ib[p]);   m_ib[p]   = INVALID_BUFFER; }
        }
        if (m_shader != INVALID_SHADER) { g_renderBackend->DestroyShader(m_shader); m_shader = INVALID_SHADER; }
        m_geomBuilt = false;
    }

    const char* ShulkerBoxRenderer::TextureStemForBlock(Game::BlockID id) const {
        // MC Sheets.getShulkerBoxMaterial(DyeColor) -> shulker_<colour>;
        // the undyed box uses DEFAULT_SHULKER_TEXTURE_LOCATION -> shulker.
        switch (id) {
            case Game::BlockID::WhiteShulkerBox:     return "shulker_white";
            case Game::BlockID::OrangeShulkerBox:    return "shulker_orange";
            case Game::BlockID::MagentaShulkerBox:   return "shulker_magenta";
            case Game::BlockID::LightBlueShulkerBox: return "shulker_light_blue";
            case Game::BlockID::YellowShulkerBox:    return "shulker_yellow";
            case Game::BlockID::LimeShulkerBox:      return "shulker_lime";
            case Game::BlockID::PinkShulkerBox:      return "shulker_pink";
            case Game::BlockID::GrayShulkerBox:      return "shulker_gray";
            case Game::BlockID::LightGrayShulkerBox: return "shulker_light_gray";
            case Game::BlockID::CyanShulkerBox:      return "shulker_cyan";
            case Game::BlockID::PurpleShulkerBox:    return "shulker_purple";
            case Game::BlockID::BlueShulkerBox:      return "shulker_blue";
            case Game::BlockID::BrownShulkerBox:     return "shulker_brown";
            case Game::BlockID::GreenShulkerBox:     return "shulker_green";
            case Game::BlockID::RedShulkerBox:       return "shulker_red";
            case Game::BlockID::BlackShulkerBox:     return "shulker_black";
            default:                                 return "shulker";
        }
    }

    TextureHandle ShulkerBoxRenderer::LoadColourTexture(const std::string& stem) {
        auto it = m_textureCache.find(stem);
        if (it != m_textureCache.end()) return it->second;

        const std::string rel  = "assets/textures/entity/shulker/" + stem + ".png";
        const std::string full = PlatformMain::GetAssetPath(rel);
        if (!std::filesystem::exists(full)) {
            Log::Warning("[ShulkerBoxRenderer] missing %s", full.c_str());
            m_textureCache[stem] = INVALID_TEXTURE;
            return INVALID_TEXTURE;
        }

        int w = 0, h = 0, ch = 0;
        stbi_set_flip_vertically_on_load(0);
        unsigned char* px = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha);
        if (!px) {
            m_textureCache[stem] = INVALID_TEXTURE;
            return INVALID_TEXTURE;
        }
        TextureHandle tex = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, px);
        stbi_image_free(px);
        if (tex != INVALID_TEXTURE) {
            g_renderBackend->SetTextureFilter(tex, TextureFilter::Nearest, TextureFilter::Nearest);
            g_renderBackend->SetTextureWrap (tex, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
        }
        m_textureCache[stem] = tex;
        return tex;
    }

    // MC ShulkerBoxRenderer.prepareModel + the part's own PartPose, as one
    // matrix. Read bottom-up (the last line applies to the vertex first):
    //
    //   prepareModel:  translate(0.5,0.5,0.5) · scale(0.9995) · rot(facing)
    //                  · scale(1,-1,-1) · translate(0,-1,0)
    //   part pose:     translate(0, 24, 0)                         [base]
    //                  translate(0, 24 - progress*8, 0) · rotY(270°·progress)  [lid]
    //   cube units:    scale(1/16)   — ModelPart cubes are in 1/16 block units
    //
    // scale(1,-1,-1) is a 180-degree turn about X, so its determinant is +1 and
    // the winding — and therefore back-face culling — is unaffected.
    glm::mat4 ShulkerBoxRenderer::PartMatrix(bool lid, Game::Direction facing,
                                             float progress) const {
        glm::mat4 m(1.0f);
        m = glm::translate(m, glm::vec3(0.5f, 0.5f, 0.5f));
        m = glm::scale(m, glm::vec3(0.9995f));
        m = m * FacingRotation(facing);
        m = glm::scale(m, glm::vec3(1.0f, -1.0f, -1.0f));
        m = glm::translate(m, glm::vec3(0.0f, -1.0f, 0.0f));

        // Part pose. Both parts share the (0,24,0) origin; the lid's Y is what
        // the open animation drives, and it also spins a further 270 degrees.
        if (lid) {
            m = glm::translate(m, glm::vec3(0.0f, (24.0f - progress * 0.5f * 16.0f) / 16.0f, 0.0f));
            m = glm::rotate(m, glm::radians(270.0f) * progress, glm::vec3(0, 1, 0));
        } else {
            m = glm::translate(m, glm::vec3(0.0f, 24.0f / 16.0f, 0.0f));
        }

        // ModelPart cube coordinates are in 1/16 block units.
        m = glm::scale(m, glm::vec3(1.0f / 16.0f));
        return m;
    }

    void ShulkerBoxRenderer::Render(const Game::BlockEntity& be,
                                    float /*partialTick*/,
                                    const glm::mat4& proj,
                                    const glm::mat4& view,
                                    const glm::vec3& /*cameraPos*/) {
        if (!m_geomBuilt || !g_renderBackend) return;

        TextureHandle tex = LoadColourTexture(TextureStemForBlock(be.GetBlockId()));
        if (tex == INVALID_TEXTURE) return;

        const glm::ivec3 p = be.GetWorldPos();
        Game::BlockState state;
        if (Client::g_clientBlockAccess) {
            state = Client::g_clientBlockAccess->GetBlockState(p.x, p.y, p.z);
        }
        const Game::Direction facing = FacingOf(state);

        // Lid open amount. MC reads ShulkerBoxBlockEntity.getProgress(partialTick),
        // driven by the container-opener count. This engine's shulker box uses
        // the generic container block entity, which carries no open state, so
        // the box is drawn closed — the same place ChestRenderer's lid is.
        const float progress = 0.0f;

        const glm::mat4 toWorld = glm::translate(glm::mat4(1.0f), glm::vec3(p));

        PipelineState s;
        s.depthTestEnabled  = true;
        s.depthWriteEnabled = true;
        s.blendEnabled      = false;
        s.cullMode          = CullMode::Back;
        s.primitiveType     = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(s);
        g_renderBackend->BindShader(m_shader);
        g_renderBackend->BindTexture(tex, 0);

        for (int part = 0; part < kPartCount; ++part) {
            const glm::mat4 mvp = proj * view * toWorld *
                                  PartMatrix(part == kLid, facing, progress);
            g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp);
            g_renderBackend->DrawIndexed(m_mesh[part], m_indexCount[part]);
        }
        g_renderBackend->UnbindMesh();
    }

    void ShulkerBoxRenderer::RenderBEWLR(Game::BlockID blockId, const glm::mat4& mvp) {
        if (!m_geomBuilt || !g_renderBackend) return;
        TextureHandle tex = LoadColourTexture(TextureStemForBlock(blockId));
        if (tex == INVALID_TEXTURE) return;

        // The caller works in MC PIXELS: it composed
        // `display · scale(1/16) · translate(-8,-8,-8)`, so a point p given to
        // us lands at (p - 8) / 16 in display space — pixel 8 at the origin.
        //
        // PartMatrix emits BLOCK units, where the box occupies [0,1]. So the
        // only conversion needed is block -> pixel, i.e. scale by 16; the
        // caller's own -8 then does the centring.
        //
        // An earlier version also translated by (8,8,8) here, which double-
        // counted that centring and pushed the box half a block up, right and
        // forward of the hand.
        const glm::mat4 toItem = glm::scale(glm::mat4(1.0f), glm::vec3(16.0f));

        // An item always shows the upright, closed box regardless of which way
        // the placed block would face — MC's SpecialModelRenderer passes
        // Direction.UP for the item form.
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

        for (int part = 0; part < kPartCount; ++part) {
            const glm::mat4 m = mvp * toItem *
                                PartMatrix(part == kLid, Game::Direction::Up, 0.0f);
            g_renderBackend->SetUniformMat4(m_shader, "uMVP", m);
            g_renderBackend->DrawIndexed(m_mesh[part], m_indexCount[part]);
        }
        g_renderBackend->UnbindMesh();
    }

} // namespace Render

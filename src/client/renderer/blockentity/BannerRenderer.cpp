// File: src/client/renderer/blockentity/BannerRenderer.cpp
#include "BannerRenderer.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "../core/RenderOrigin.hpp"
#include "BlockEntityShader.hpp"
#include "common/world/block/entity/BannerBlockEntity.hpp"
#include "common/world/block/BlockState.hpp"
#include "client/world/ClientBlockAccess.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <string_view>

namespace Render {

    namespace {

        constexpr float kPi = 3.14159265358979323846f;

        // MC DyeColor.getTextureDiffuseColor.
        constexpr uint32_t kDyeDiffuse[16] = {
            0xF9FFFE, 0xF9801D, 0xC74EBD, 0x3AB3DA, 0xFED83D, 0x80C71F, 0xF38BAA, 0x474F52,
            0x9D9D97, 0x169C9C, 0x8932B8, 0x3C44AA, 0x835432, 0x5E7C16, 0xB02E26, 0x1D1D21,
        };
        glm::vec3 DyeRgb(int dye) {
            const uint32_t c = kDyeDiffuse[dye & 15];
            return glm::vec3(static_cast<float>((c >> 16) & 0xFF), static_cast<float>((c >> 8) & 0xFF),
                             static_cast<float>(c & 0xFF)) / 255.0f;
        }

        // BannerRenderer.modelTransformation(angle): translation (0.5, 0,
        // 0.5), YP.rotationDegrees(-angle), scale (2/3, -2/3, -2/3).
        glm::mat4 ModelTransformation(float angleDegrees) {
            glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(0.5f, 0.0f, 0.5f));
            m = glm::rotate(m, glm::radians(-angleDegrees), glm::vec3(0.0f, 1.0f, 0.0f));
            return glm::scale(m, glm::vec3(0.6666667f, -0.6666667f, -0.6666667f));
        }
        // Wall orientations in Direction 2D order (south, west, north, east):
        // Direction.toYRot = 90 × the data value.
        float WallAngle(int facing2D) { return 90.0f * static_cast<float>(facing2D); }
        float GroundAngle(int segment) { return 22.5f * static_cast<float>(segment); }   // RotationSegment

        int Facing2D(std::string_view f) {
            if (f == "south") return 0;
            if (f == "west")  return 1;
            if (f == "north") return 2;
            if (f == "east")  return 3;
            return 0;
        }

        // BannerFlagModel's flag part pose.
        glm::vec3 FlagOffset(bool standing) {
            return standing ? glm::vec3(0.0f, -44.0f, 0.0f) : glm::vec3(0.0f, -20.5f, 10.5f);
        }

    } // namespace

    BannerRenderer::~BannerRenderer() { Shutdown(); }

    bool BannerRenderer::Initialize() {
        if (!g_renderBackend) return false;
        m_shader = BlockEntityShader::Create();
        if (m_shader == INVALID_SHADER) {
            Log::Error("[BannerRenderer] shader compile failed");
            return false;
        }
        using namespace BEModel;
        const glm::mat4 cube = CubeSpace(glm::mat4(1.0f));

        // BannerModel.createBodyLayer(standing) and BannerFlagModel
        // .createFlagLayer(standing): 64x64 sheet.
        auto buildBody = [&](bool standing, int orientations) {
            std::vector<Vert> verts;
            std::vector<uint32_t> idx;
            for (int o = 0; o < orientations; ++o) {
                const float angle = standing ? GroundAngle(o) : WallAngle(o);
                const glm::mat3 normalMat = EntityLighting::NormalMatrix(ModelTransformation(angle));
                for (int set = 0; set < 2; ++set) {
                    const auto lightSet = set ? EntityLighting::LightSet::Nether : EntityLighting::LightSet::Default;
                    if (standing) {
                        AddCube(verts, idx, {-1, -42, -1}, {1, 0, 1}, 44, 0, 2, 42, 2, 64, 64, kAll,
                                cube, normalMat, lightSet);
                        AddCube(verts, idx, {-10, -44, -1}, {10, -42, 1}, 0, 42, 20, 2, 2, 64, 64, kAll,
                                cube, normalMat, lightSet);
                    } else {
                        AddCube(verts, idx, {-10, -20.5f, 9.5f}, {10, -18.5f, 11.5f}, 0, 42, 20, 2, 2, 64, 64, kAll,
                                cube, normalMat, lightSet);
                    }
                }
            }
            return Upload(verts, idx, static_cast<uint32_t>(orientations * 2));
        };
        auto buildFlag = [&](bool standing, int orientations) {
            std::vector<Vert> verts;
            std::vector<uint32_t> idx;
            for (int o = 0; o < orientations; ++o) {
                const float angle = standing ? GroundAngle(o) : WallAngle(o);
                // The sway's small xRot is left out of the baked shade.
                const glm::mat3 normalMat = EntityLighting::NormalMatrix(
                    PartPose(ModelTransformation(angle), FlagOffset(standing)));
                for (int set = 0; set < 2; ++set) {
                    const auto lightSet = set ? EntityLighting::LightSet::Nether : EntityLighting::LightSet::Default;
                    AddCube(verts, idx, {-10, 0, -2}, {10, 40, -1}, 0, 0, 20, 40, 1, 64, 64, kAll,
                            cube, normalMat, lightSet);
                }
            }
            return Upload(verts, idx, static_cast<uint32_t>(orientations * 2));
        };
        m_groundBody = buildBody(true, kGroundOrientations);
        m_wallBody   = buildBody(false, kWallOrientations);
        m_groundFlag = buildFlag(true, kGroundOrientations);
        m_wallFlag   = buildFlag(false, kWallOrientations);
        m_built = m_groundBody.Valid() && m_wallBody.Valid() && m_groundFlag.Valid() && m_wallFlag.Valid();
        return m_built;
    }

    void BannerRenderer::Shutdown() {
        BEModel::Destroy(m_groundBody);
        BEModel::Destroy(m_wallBody);
        BEModel::Destroy(m_groundFlag);
        BEModel::Destroy(m_wallFlag);
        m_textures.Clear();
        if (g_renderBackend && m_shader != INVALID_SHADER) g_renderBackend->DestroyShader(m_shader);
        m_shader = INVALID_SHADER;
        m_built = false;
    }

    void BannerRenderer::Render(const Game::BlockEntity& be,
                                float partialTick,
                                const glm::mat4& proj,
                                const glm::mat4& view,
                                const glm::vec3& cameraPos) {
        PROFILE_ZONE_N("BE.Banner");
        if (!m_built || !g_renderBackend) return;
        const auto* banner = dynamic_cast<const Game::BannerBlockEntity*>(&be);
        if (!banner) return;
        const TextureHandle baseTex = m_textures.Get("banner_base");
        if (baseTex == INVALID_TEXTURE) return;

        const glm::ivec3 p = be.GetWorldPos();
        const bool wall = Game::BannerBlockEntity::IsWallBanner(be.GetBlockId());
        int orientation = 0;
        if (Client::g_clientBlockAccess) {
            const Game::BlockState state = Client::g_clientBlockAccess->GetBlockState(p.x, p.y, p.z);
            orientation = wall ? Facing2D(state.GetValueByName("facing"))
                               : std::max(0, state.GetIndex(Game::PropertyId::ROTATION)) & 15;
        }
        const float angle = wall ? WallAngle(orientation) : GroundAngle(orientation);
        const bool nether = EntityLighting::Current() == EntityLighting::LightSet::Nether;
        const uint32_t copy = static_cast<uint32_t>(orientation * 2 + (nether ? 1 : 0));

        // extractRenderState's phase: floorMod(x*7 + y*9 + z*13 + gameTime,
        // 100) plus the partial tick, over 100 — the level's game time, so the
        // flag stands still while the game is paused or held.
        const int64_t gameTime = Client::g_clientBlockAccess ? Client::g_clientBlockAccess->GameTime() : 0;
        int64_t sum = static_cast<int64_t>(p.x) * 7 + static_cast<int64_t>(p.y) * 9 +
                      static_cast<int64_t>(p.z) * 13 + gameTime;
        sum = ((sum % 100) + 100) % 100;
        const float phase = (static_cast<float>(sum) + partialTick) / 100.0f;
        const float flagXRot = (-0.0125f + 0.01f * std::cos(2.0f * kPi * phase)) * kPi;

        const glm::mat4 body = glm::translate(glm::mat4(1.0f), Render::ToRender(glm::dvec3(p))) *
                               ModelTransformation(angle);
        const glm::mat4 flag = BEModel::PartPose(body, FlagOffset(!wall), glm::vec3(flagXRot, 0.0f, 0.0f));
        const glm::vec3 light = BlockEntityShader::LightAt(p);

        PipelineState s;
        s.depthTestEnabled  = true;
        s.depthWriteEnabled = true;
        s.blendEnabled      = false;
        s.cullMode          = CullMode::Back;
        s.primitiveType     = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(s);
        g_renderBackend->BindShader(m_shader);
        g_renderBackend->SetUniformFloat(m_shader, "uAlphaTest", 0.05f);
        g_renderBackend->BindTexture(baseTex, 0);

        // BannerModel then BannerFlagModel on banner_base (entitySolid).
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", proj * view * body);
        BlockEntityShader::ApplyWorld(m_shader, body, cameraPos, p);
        BEModel::Draw(wall ? m_wallBody : m_groundBody, copy);
        const BEModel::Mesh& flagMesh = wall ? m_wallFlag : m_groundFlag;
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", proj * view * flag);
        BlockEntityShader::ApplyWorld(m_shader, flag, cameraPos, p);
        BEModel::Draw(flagMesh, copy);

        // submitPatterns: the base colour, then the layers — each the flag
        // again, its sheet tinted by the dye (the draw's light carries the
        // tint), blended over what is there (bannerPattern is translucent,
        // no depth write).
        PipelineState blend = s;
        blend.depthWriteEnabled = false;
        blend.depthCompareOp    = CompareOp::LessEqual;
        blend.blendEnabled      = true;
        blend.srcBlendFactor    = BlendFactor::SrcAlpha;
        blend.dstBlendFactor    = BlendFactor::OneMinusSrcAlpha;
        g_renderBackend->SetPipelineState(blend);
        g_renderBackend->SetUniformFloat(m_shader, "uAlphaTest", 0.0f);
        auto layer = [&](const std::string& sheet, int dye) {
            const TextureHandle tex = m_textures.Get("banner/" + sheet);
            if (tex == INVALID_TEXTURE) return;
            g_renderBackend->BindTexture(tex, 0);
            BlockEntityShader::SetLight(m_shader, light * DyeRgb(dye));
            BEModel::Draw(flagMesh, copy);
        };
        layer("base", static_cast<int>(banner->GetBaseColor()));
        const auto& layers = banner->GetPatterns().layers;
        for (size_t i = 0; i < layers.size() && i < 16; ++i) {
            std::string path = layers[i].pattern;
            if (const size_t colon = path.find(':'); colon != std::string::npos) path = path.substr(colon + 1);
            layer(path, layers[i].color);
        }
        g_renderBackend->UnbindMesh();
    }

} // namespace Render

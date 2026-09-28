// File: src/client/renderer/blockentity/DecoratedPotRenderer.cpp
#include "client/resource/ResourcePacks.hpp"
#include "DecoratedPotRenderer.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "../backend/RenderBackend.hpp"
#include "../core/RenderOrigin.hpp"
#include "../entity/EntityLighting.hpp"
#include "BlockEntityShader.hpp"
#include "common/world/block/entity/DecoratedPotBlockEntity.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/entity/Item.hpp"
#include "common/data/DataComponents.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "common/core/Log.hpp"

#include "stb_image.h"
#include <glm/gtc/matrix_transform.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
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

        constexpr float kPi = 3.14159265358979323846f;

        // Four facings (Direction 2D data: south, west, north, east) × the
        // two light sets: the shade is baked per copy, as ShulkerBoxRenderer
        // does, because MC lights the pot from fixed world directions.
        constexpr int kLightingCount = 4 * 2;
        int LightingIndex(int facing2D, bool nether) { return facing2D * 2 + (nether ? 1 : 0); }

        // MC Cube face bits (its visibleFaces set), Direction order.
        enum FaceBit : uint8_t {
            kDown = 1, kUp = 2, kNorth = 4, kSouth = 8, kWest = 16, kEast = 32, kAll = 63
        };

        // The pot's turn: DecoratedPotRenderer.createModelTransformation —
        // rotateAround(Y, 180 - facing.toYRot(), 0.5, 0.5, 0.5), toYRot being
        // 90 × the 2D data value.
        glm::mat4 FacingMatrix(int facing2D) {
            glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(0.5f));
            m = glm::rotate(m, glm::radians(180.0f - 90.0f * static_cast<float>(facing2D)),
                            glm::vec3(0.0f, 1.0f, 0.0f));
            return glm::translate(m, glm::vec3(-0.5f));
        }

        // ModelPart.translateAndRotate (offset in pixels, then
        // rotationZYX(z, y, x) = Rz·Ry·Rx) and the cube's /16 — the part's
        // pixel-space cube into block units.
        glm::mat4 PartPose(glm::vec3 offset, glm::vec3 rot) {
            glm::mat4 m = glm::translate(glm::mat4(1.0f), offset / 16.0f);
            if (rot.z != 0.0f) m = glm::rotate(m, rot.z, glm::vec3(0, 0, 1));
            if (rot.y != 0.0f) m = glm::rotate(m, rot.y, glm::vec3(0, 1, 0));
            if (rot.x != 0.0f) m = glm::rotate(m, rot.x, glm::vec3(1, 0, 0));
            return glm::scale(m, glm::vec3(1.0f / 16.0f));
        }

        // One ModelPart.Cube: MC's six faces with its UV layout (vertex 0
        // takes the HIGH u — see ShulkerBoxRenderer's AddCube), restricted
        // to `faces`, positions baked through `pose` into block units, UVs
        // normalised by the sheet size, the shade from the world normal
        // through `facing · pose` for every lighting copy in turn.
        void AddCube(std::vector<CubeVert>& verts, std::vector<uint32_t>& idx,
                     glm::vec3 from, glm::vec3 to,
                     float xTexOffs, float yTexOffs, float w, float h, float d,
                     uint8_t faces, const glm::mat4& pose, float texW, float texH,
                     int lighting) {
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

            const int facing2D = lighting / 2;
            const auto set = (lighting % 2) ? EntityLighting::LightSet::Nether
                                            : EntityLighting::LightSet::Default;
            const glm::mat3 normalMat = EntityLighting::NormalMatrix(FacingMatrix(facing2D) * pose);
            auto shade = [&](const glm::vec3& modelNormal) -> uint8_t {
                return EntityLighting::ShadeByte(normalMat * modelNormal, set);
            };

            auto emit = [&](const glm::vec3 q[4], float U0, float V0, float U1, float V1, uint8_t sh) {
                const uint32_t base = static_cast<uint32_t>(verts.size());
                const float uv[4][2] = { {U1, V0}, {U0, V0}, {U0, V1}, {U1, V1} };
                for (int i = 0; i < 4; ++i) {
                    const glm::vec4 p = pose * glm::vec4(q[i], 1.0f);
                    verts.push_back({p.x, p.y, p.z, uv[i][0] / texW, uv[i][1] / texH, sh, sh, sh, 255});
                }
                idx.push_back(base + 0); idx.push_back(base + 1); idx.push_back(base + 2);
                idx.push_back(base + 0); idx.push_back(base + 2); idx.push_back(base + 3);
            };

            const glm::vec3 fDown [4] = {l1, l0, t0, t1};
            const glm::vec3 fUp   [4] = {t2, t3, l3, l2};
            const glm::vec3 fNorth[4] = {t1, t0, t3, t2};
            const glm::vec3 fSouth[4] = {l0, l1, l2, l3};
            const glm::vec3 fWest [4] = {t0, l0, l3, t3};
            const glm::vec3 fEast [4] = {l1, t1, t2, l2};

            if (faces & kDown)  emit(fDown,  u1, v0, u2,  v1, shade({ 0, -1,  0}));
            if (faces & kUp)    emit(fUp,    u2, v1, u22, v0, shade({ 0,  1,  0}));
            if (faces & kNorth) emit(fNorth, u1, v1, u2,  v2, shade({ 0,  0, -1}));
            if (faces & kSouth) emit(fSouth, u3, v1, u4,  v2, shade({ 0,  0,  1}));
            if (faces & kWest)  emit(fWest,  u0, v1, u1,  v2, shade({-1,  0,  0}));
            if (faces & kEast)  emit(fEast,  u2, v1, u3,  v2, shade({ 1,  0,  0}));
        }

        // The sherd's pattern texture stem (DecoratedPotPatterns: a
        // <name>_pottery_sherd provides <name>_pottery_pattern); the blank
        // side for a brick, an unknown item or no item at all.
        std::string SideStem(Game::ItemID item) {
            if (item != Game::Items::Air) {
                const std::string_view slug = Game::ItemRegistry::Slug(item);
                constexpr std::string_view kSuffix = "_pottery_sherd";
                if (slug.size() > kSuffix.size() &&
                    slug.compare(slug.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0) {
                    return std::string(slug.substr(0, slug.size() - kSuffix.size())) + "_pottery_pattern";
                }
            }
            return "decorated_pot_side";
        }

        int Facing2DOf(Game::BlockState state) {
            const std::string_view f = state.GetValueByName("facing");
            if (f == "south") return 0;
            if (f == "west")  return 1;
            if (f == "north") return 2;
            if (f == "east")  return 3;
            return 2;   // DecoratedPotRenderState's default: NORTH
        }

    } // namespace

    DecoratedPotRenderer::DecoratedPotRenderer() = default;
    DecoratedPotRenderer::~DecoratedPotRenderer() { Shutdown(); }

    bool DecoratedPotRenderer::Initialize() {
        if (!g_renderBackend) return false;
        m_shader = BlockEntityShader::Create();
        if (m_shader == INVALID_SHADER) {
            Log::Error("[DecoratedPotRenderer] shader compile failed");
            return false;
        }

        // DecoratedPotRenderer.createBaseLayer (32x32) and createSidesLayer
        // (16x16), verbatim: CubeDeformation grows the box but not its UVs.
        const glm::mat4 neckPose   = PartPose({0.0f, 37.0f, 16.0f}, {kPi, 0.0f, 0.0f});
        const glm::mat4 topPose    = PartPose({1.0f, 16.0f, 1.0f},  {0.0f, 0.0f, 0.0f});
        const glm::mat4 bottomPose = PartPose({1.0f, 0.0f, 1.0f},   {0.0f, 0.0f, 0.0f});
        const glm::mat4 sidePose[4] = {
            PartPose({15.0f, 16.0f, 1.0f},  {0.0f, 0.0f, kPi}),            // back
            PartPose({1.0f, 16.0f, 1.0f},   {0.0f, -kPi * 0.5f, kPi}),     // left
            PartPose({15.0f, 16.0f, 15.0f}, {0.0f, kPi * 0.5f, kPi}),      // right
            PartPose({1.0f, 16.0f, 15.0f},  {kPi, 0.0f, 0.0f}),            // front
        };

        auto upload = [&](Part p, const std::vector<CubeVert>& verts, const std::vector<uint32_t>& idx) {
            m_vb[p] = g_renderBackend->CreateBuffer(BufferUsage::Vertex,
                verts.size() * sizeof(CubeVert), verts.data());
            m_ib[p] = g_renderBackend->CreateBuffer(BufferUsage::Index,
                idx.size() * sizeof(uint32_t), idx.data());
            m_mesh[p] = g_renderBackend->CreateMesh(m_vb[p], m_ib[p], GetBlockVertexLayout());
            m_indexCount[p] = static_cast<uint32_t>(idx.size() / kLightingCount);
        };

        {
            std::vector<CubeVert> verts;
            std::vector<uint32_t> idx;
            for (int lighting = 0; lighting < kLightingCount; ++lighting) {
                // neck: texOffs(0,0) addBox(4,17,4, 8,3,8, deflate -0.1)
                //       texOffs(0,5) addBox(5,20,5, 6,1,6, inflate 0.2)
                AddCube(verts, idx, {4.1f, 17.1f, 4.1f}, {11.9f, 19.9f, 11.9f}, 0, 0, 8, 3, 8,
                        kAll, neckPose, 32.0f, 32.0f, lighting);
                AddCube(verts, idx, {4.8f, 19.8f, 4.8f}, {11.2f, 21.2f, 11.2f}, 0, 5, 6, 1, 6,
                        kAll, neckPose, 32.0f, 32.0f, lighting);
                // top / bottom: texOffs(-14,13) addBox(0,0,0, 14,0,14) — a
                // plane; its two broad faces are the whole of it.
                AddCube(verts, idx, {0, 0, 0}, {14, 0, 14}, -14, 13, 14, 0, 14,
                        kDown | kUp, topPose, 32.0f, 32.0f, lighting);
                AddCube(verts, idx, {0, 0, 0}, {14, 0, 14}, -14, 13, 14, 0, 14,
                        kDown | kUp, bottomPose, 32.0f, 32.0f, lighting);
            }
            upload(kBase, verts, idx);
        }
        for (int side = 0; side < 4; ++side) {
            std::vector<CubeVert> verts;
            std::vector<uint32_t> idx;
            for (int lighting = 0; lighting < kLightingCount; ++lighting) {
                // texOffs(1,0) addBox(0,0,0, 14,16,0, {NORTH})
                AddCube(verts, idx, {0, 0, 0}, {14, 16, 0}, 1, 0, 14, 16, 0,
                        kNorth, sidePose[side], 16.0f, 16.0f, lighting);
            }
            upload(static_cast<Part>(kBack + side), verts, idx);
        }

        m_geomBuilt = true;
        for (int p = 0; p < kPartCount; ++p) m_geomBuilt = m_geomBuilt && m_mesh[p] != INVALID_MESH;
        return m_geomBuilt;
    }

    void DecoratedPotRenderer::Shutdown() {
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

    TextureHandle DecoratedPotRenderer::LoadTexture(const std::string& stem) {
        if (Resources::CacheStale(m_textureCacheGeneration)) {
            // A resource pack change replaces every texture here.
            for (auto& [key, tex] : m_textureCache) if (tex != INVALID_TEXTURE) g_renderBackend->DestroyTexture(tex);
            m_textureCache.clear();
        }
        auto it = m_textureCache.find(stem);
        if (it != m_textureCache.end()) return it->second;

        const std::string full = PlatformMain::GetAssetPath("assets/textures/entity/decorated_pot/" + stem + ".png");
        TextureHandle tex = INVALID_TEXTURE;
        if (std::filesystem::exists(full)) {
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
            Log::Warning("[DecoratedPotRenderer] missing %s", full.c_str());
        }
        m_textureCache[stem] = tex;
        return tex;
    }

    void DecoratedPotRenderer::Render(const Game::BlockEntity& be,
                                      float partialTick,
                                      const glm::mat4& proj,
                                      const glm::mat4& view,
                                      const glm::vec3& cameraPos) {
        PROFILE_ZONE_N("BE.DecoratedPot");
        if (!m_geomBuilt || !g_renderBackend) return;
        const auto* pot = dynamic_cast<const Game::DecoratedPotBlockEntity*>(&be);
        if (!pot) return;

        const TextureHandle baseTex = LoadTexture("decorated_pot_base");
        if (baseTex == INVALID_TEXTURE) return;

        const glm::ivec3 p = be.GetWorldPos();
        int facing2D = 2;
        if (Client::g_clientBlockAccess) {
            facing2D = Facing2DOf(Client::g_clientBlockAccess->GetBlockState(p.x, p.y, p.z));
        }

        // The wobble. 26.3's extractRenderState fills wobbleProgress from the
        // entity's last style but never copies the style itself into the
        // render state, so submit always takes the NEGATIVE branch (a twist
        // about Y) — reproduced as shipped. Progress is ticks since the event
        // over the style's duration; the client clock stands in for game time.
        glm::mat4 wobble(1.0f);
        if (pot->LastWobbleStyle() >= 0) {
            const double now = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            const float ticks = static_cast<float>((now - pot->WobbleStartedAtSeconds()) * 20.0);
            (void)partialTick;
            const float duration = static_cast<float>(
                Game::DecoratedPotBlockEntity::kWobbleDuration[pot->LastWobbleStyle() & 1]);
            const float progress = ticks / duration;
            if (progress >= 0.0f && progress <= 1.0f) {
                const float amplitude = std::sin(-progress * 3.0f * kPi) * 0.125f;
                const float deltaTime = 1.0f - progress;
                wobble = glm::translate(glm::mat4(1.0f), glm::vec3(0.5f, 0.0f, 0.5f));
                wobble = glm::rotate(wobble, amplitude * deltaTime, glm::vec3(0.0f, 1.0f, 0.0f));
                wobble = glm::translate(wobble, glm::vec3(-0.5f, 0.0f, -0.5f));
            }
        }

        const glm::mat4 model = glm::translate(glm::mat4(1.0f), Render::ToRender(glm::dvec3(p)))
                              * FacingMatrix(facing2D) * wobble;
        const glm::mat4 mvp = proj * view * model;
        const int lighting = LightingIndex(
            facing2D, EntityLighting::Current() == EntityLighting::LightSet::Nether);

        PipelineState s;
        s.depthTestEnabled  = true;
        s.depthWriteEnabled = true;
        s.blendEnabled      = false;
        s.cullMode          = CullMode::Back;
        s.primitiveType     = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(s);
        g_renderBackend->BindShader(m_shader);
        g_renderBackend->SetUniformFloat(m_shader, "uAlphaTest", 0.05f);
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp);
        BlockEntityShader::ApplyWorld(m_shader, model, cameraPos, p);

        auto draw = [&](Part part, TextureHandle tex) {
            if (tex == INVALID_TEXTURE) return;
            g_renderBackend->BindTexture(tex, 0);
            g_renderBackend->DrawIndexed(m_mesh[part], m_indexCount[part],
                                         m_indexCount[part] * static_cast<uint32_t>(lighting));
        };
        draw(kBase, baseTex);
        draw(kFront, LoadTexture(SideStem(pot->GetSide(Game::DecoratedPotBlockEntity::kFront))));
        draw(kBack,  LoadTexture(SideStem(pot->GetSide(Game::DecoratedPotBlockEntity::kBack))));
        draw(kLeft,  LoadTexture(SideStem(pot->GetSide(Game::DecoratedPotBlockEntity::kLeft))));
        draw(kRight, LoadTexture(SideStem(pot->GetSide(Game::DecoratedPotBlockEntity::kRight))));
        g_renderBackend->UnbindMesh();
    }

    void DecoratedPotRenderer::DrawItem(const std::array<Game::ItemID, 4>& sides, const glm::mat4& mvp,
                                        const BEWLRLight& light) {
        if (!m_geomBuilt || !g_renderBackend) return;
        const TextureHandle baseTex = LoadTexture("decorated_pot_base");
        if (baseTex == INVALID_TEXTURE) return;
        // The caller works in MC pixels (display · scale(1/16) ·
        // translate(-8)); the pot's meshes are in block units.
        const glm::mat4 toItem = glm::scale(glm::mat4(1.0f), glm::vec3(16.0f));
        PipelineState s;
        s.depthTestEnabled  = true;
        s.depthWriteEnabled = true;
        s.depthCompareOp    = CompareOp::LessEqual;
        s.blendEnabled      = false;
        s.cullMode          = CullMode::Back;
        s.primitiveType     = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(s);
        g_renderBackend->BindShader(m_shader);
        g_renderBackend->SetUniformFloat(m_shader, "uAlphaTest", 0.05f);
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp * toItem);
        BlockEntityShader::ApplyItem(m_shader, light, toItem);
        // The north-facing copy: the special renderer draws the pot unturned.
        const int lighting = LightingIndex(2, false);
        auto draw = [&](Part part, TextureHandle tex) {
            if (tex == INVALID_TEXTURE) return;
            g_renderBackend->BindTexture(tex, 0);
            g_renderBackend->DrawIndexed(m_mesh[part], m_indexCount[part],
                                         m_indexCount[part] * static_cast<uint32_t>(lighting));
        };
        draw(kBase, baseTex);
        draw(kFront, LoadTexture(SideStem(sides[3])));
        draw(kBack,  LoadTexture(SideStem(sides[0])));
        draw(kLeft,  LoadTexture(SideStem(sides[1])));
        draw(kRight, LoadTexture(SideStem(sides[2])));
        g_renderBackend->UnbindMesh();
    }

    void DecoratedPotRenderer::RenderBEWLR(Game::BlockID /*blockId*/, const glm::mat4& mvp,
                                           const BEWLRLight& light) {
        DrawItem({Game::Items::Air, Game::Items::Air, Game::Items::Air, Game::Items::Air}, mvp, light);
    }

    void DecoratedPotRenderer::RenderBEWLRStack(Game::BlockID /*blockId*/, const Game::ItemStack& stack,
                                                const glm::mat4& mvp, const BEWLRLight& light) {
        const Game::PotDecorations decorations =
            stack.get(Game::DataComponents::POT_DECORATIONS).value_or(Game::PotDecorations{});
        DrawItem(decorations.sides, mvp, light);
    }

} // namespace Render

// File: src/client/renderer/gui/items/DecoratedPotItemRenderer.cpp
#include "client/resource/ResourcePacks.hpp"
#include "DecoratedPotItemRenderer.hpp"
#include "ItemLighting.hpp"
#include "../GuiGraphics.hpp"
#include "../GuiRenderState.hpp"
#include "../../backend/RenderBackend.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/Item.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/core/Log.hpp"

#include "stb_image.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    namespace {

        constexpr float kPi = 3.14159265358979323846f;
        constexpr float kGuiRotX = 30.0f;
        constexpr float kGuiRotY = 45.0f;

        TextureHandle LoadPotTexture(const std::string& stem) {
            static std::unordered_map<std::string, TextureHandle> s_cache;
            static int s_generation = -1;
            if (Resources::CacheStale(s_generation)) {
                if (g_renderBackend) for (auto& [k, t] : s_cache) if (t != INVALID_TEXTURE) g_renderBackend->DestroyTexture(t);
                s_cache.clear();
            }
            if (auto it = s_cache.find(stem); it != s_cache.end()) return it->second;
            TextureHandle tex = INVALID_TEXTURE;
            const std::string full = PlatformMain::GetAssetPath("assets/textures/entity/decorated_pot/" + stem + ".png");
            if (g_renderBackend && std::filesystem::exists(full)) {
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
            }
            s_cache[stem] = tex;
            return tex;
        }

        std::string SideStem(Game::ItemID item) {
            // DecoratedPotRenderer.getSideMaterial: the item's
            // PROVIDES_POTTERY_PATTERN, else the blank side.
            std::string stem = Game::PotteryPatternStem(item);
            return stem.empty() ? std::string("decorated_pot_side") : stem;
        }

        // display.gui: rotation [30, 45, 0], scale 0.6, then ItemTransform's
        // translate(-0.5).
        const glm::mat4& IsoMatrix() {
            static glm::mat4 m = [] {
                glm::mat4 mat(1.0f);
                mat = glm::rotate(mat, glm::radians(kGuiRotX), glm::vec3(1, 0, 0));
                mat = glm::rotate(mat, glm::radians(kGuiRotY), glm::vec3(0, 1, 0));
                mat = glm::scale(mat, glm::vec3(0.6f));
                return glm::translate(mat, glm::vec3(-0.5f));
            }();
            return m;
        }

        glm::mat4 PartPose(glm::vec3 offset, glm::vec3 rot) {
            glm::mat4 m = glm::translate(glm::mat4(1.0f), offset / 16.0f);
            if (rot.z != 0.0f) m = glm::rotate(m, rot.z, glm::vec3(0, 0, 1));
            if (rot.y != 0.0f) m = glm::rotate(m, rot.y, glm::vec3(0, 1, 0));
            if (rot.x != 0.0f) m = glm::rotate(m, rot.x, glm::vec3(1, 0, 0));
            return glm::scale(m, glm::vec3(1.0f / 16.0f));
        }

        struct Face {
            std::array<glm::vec3, 4> v;   // block space
            float u[4], vv[4];            // normalised
            uint32_t color;
            TextureHandle tex;
            float depth;
        };

        enum FaceBit : uint8_t { kDown = 1, kUp = 2, kNorth = 4, kSouth = 8, kWest = 16, kEast = 32, kAll = 63 };

        void AddCube(std::vector<Face>& out, const glm::mat4& pose, glm::vec3 from, glm::vec3 to,
                     float tu, float tv, float w, float h, float d, float texW, float texH,
                     uint8_t faces, TextureHandle tex) {
            const glm::vec3 t0(from.x, from.y, from.z), t1(to.x, from.y, from.z);
            const glm::vec3 t2(to.x, to.y, from.z),     t3(from.x, to.y, from.z);
            const glm::vec3 l0(from.x, from.y, to.z),   l1(to.x, from.y, to.z);
            const glm::vec3 l2(to.x, to.y, to.z),       l3(from.x, to.y, to.z);
            const float u0 = tu, u1 = tu + d, u2 = tu + d + w, u22 = tu + d + w + w;
            const float u3 = tu + d + w + d, u4 = tu + d + w + d + w;
            const float v0 = tv, v1 = tv + d, v2 = tv + d + h;
            const glm::mat3 normalMat = glm::mat3(pose);
            auto emit = [&](const glm::vec3 q[4], float U0, float V0, float U1, float V1, glm::vec3 n) {
                Face f;
                const float uv[4][2] = { {U1, V0}, {U0, V0}, {U0, V1}, {U1, V1} };
                float depth = 0.0f;
                for (int i = 0; i < 4; ++i) {
                    f.v[static_cast<size_t>(i)] = glm::vec3(pose * glm::vec4(q[i], 1.0f));
                    f.u[i] = uv[i][0] / texW;
                    f.vv[i] = uv[i][1] / texH;
                    depth += (IsoMatrix() * glm::vec4(f.v[static_cast<size_t>(i)], 1.0f)).z;
                }
                f.depth = depth * 0.25f;
                const glm::vec3 worldNormal = glm::normalize(normalMat * n);
                f.color = ItemLighting::ShadeAsColor(ItemLighting::ComputeShadeFlat(worldNormal, kGuiRotX, kGuiRotY));
                f.tex = tex;
                out.push_back(f);
            };
            const glm::vec3 fDown [4] = {l1, l0, t0, t1};
            const glm::vec3 fUp   [4] = {t2, t3, l3, l2};
            const glm::vec3 fNorth[4] = {t1, t0, t3, t2};
            const glm::vec3 fSouth[4] = {l0, l1, l2, l3};
            const glm::vec3 fWest [4] = {t0, l0, l3, t3};
            const glm::vec3 fEast [4] = {l1, t1, t2, l2};
            if (faces & kDown)  emit(fDown,  u1, v0, u2,  v1, { 0, -1,  0});
            if (faces & kUp)    emit(fUp,    u2, v1, u22, v0, { 0,  1,  0});
            if (faces & kNorth) emit(fNorth, u1, v1, u2,  v2, { 0,  0, -1});
            if (faces & kSouth) emit(fSouth, u3, v1, u4,  v2, { 0,  0,  1});
            if (faces & kWest)  emit(fWest,  u0, v1, u1,  v2, {-1,  0,  0});
            if (faces & kEast)  emit(fEast,  u2, v1, u3,  v2, { 1,  0,  0});
        }

        void RenderPotInventory(GuiGraphics& g, const Game::ItemStack& stack, int x, int y) {
            GuiRenderState* rs = g.GetRenderState();
            if (!rs) return;
            const TextureHandle base = LoadPotTexture("decorated_pot_base");
            if (base == INVALID_TEXTURE) return;
            const Game::PotDecorations decorations =
                stack.get(Game::DataComponents::POT_DECORATIONS).value_or(Game::PotDecorations{});

            // DecoratedPotRenderer.createBaseLayer / createSidesLayer.
            std::vector<Face> faces;
            const glm::mat4 neck = PartPose({0, 37, 16}, {kPi, 0, 0});
            AddCube(faces, neck, {4.1f, 17.1f, 4.1f}, {11.9f, 19.9f, 11.9f}, 0, 0, 8, 3, 8, 32, 32, kAll, base);
            AddCube(faces, neck, {4.8f, 19.8f, 4.8f}, {11.2f, 21.2f, 11.2f}, 0, 5, 6, 1, 6, 32, 32, kAll, base);
            AddCube(faces, PartPose({1, 16, 1}, {}), {0, 0, 0}, {14, 0, 14}, -14, 13, 14, 0, 14, 32, 32,
                    kDown | kUp, base);
            AddCube(faces, PartPose({1, 0, 1}, {}), {0, 0, 0}, {14, 0, 14}, -14, 13, 14, 0, 14, 32, 32,
                    kDown | kUp, base);
            struct SideDef { glm::vec3 offset, rot; int side; };
            const SideDef sides[4] = {
                { {15, 16, 1},  {0, 0, kPi},           0 },   // back
                { {1, 16, 1},   {0, -kPi * 0.5f, kPi}, 1 },   // left
                { {15, 16, 15}, {0, kPi * 0.5f, kPi},  2 },   // right
                { {1, 16, 15},  {kPi, 0, 0},           3 },   // front
            };
            for (const SideDef& side : sides) {
                const TextureHandle tex = LoadPotTexture(SideStem(decorations.sides[static_cast<size_t>(side.side)]));
                if (tex == INVALID_TEXTURE) continue;
                AddCube(faces, PartPose(side.offset, side.rot), {0, 0, 0}, {14, 16, 0}, 1, 0, 14, 16, 0, 16, 16,
                        kNorth, tex);
            }
            std::stable_sort(faces.begin(), faces.end(), [](const Face& a, const Face& b) { return a.depth < b.depth; });

            const float scale = 16.0f;
            const float cx = static_cast<float>(x) + 8.0f;
            const float cy = static_cast<float>(y) + 8.0f;
            for (const Face& f : faces) {
                QuadCommand q;
                q.texture = f.tex;
                q.color = f.color;
                for (int i = 0; i < 4; ++i) {
                    const glm::vec4 p = IsoMatrix() * glm::vec4(f.v[static_cast<size_t>(i)], 1.0f);
                    q.px[i] = cx + p.x * scale;
                    q.py[i] = cy - p.y * scale;
                    q.u[i] = f.u[i];
                    q.v[i] = f.vv[i];
                }
                rs->SubmitQuad(q);
            }
        }

    } // namespace

    void RegisterDecoratedPotItemRenderer() {
        GuiGraphics::RegisterCustomItemRenderer(Game::ItemRegistry::FromBlock(Game::BlockID::DecoratedPot),
                                                &RenderPotInventory);
    }

} // namespace Render

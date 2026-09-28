// File: src/client/map/ClientMaps.cpp
#include "client/map/ClientMaps.hpp"

#include "client/renderer/backend/RenderBackend.hpp"
#include "client/world/ClientLevel.hpp"
#include "common/core/Log.hpp"
#include "common/network/packets/game/MapItemDataS2CPacket.hpp"
#include "common/world/map/MapColor.hpp"
#include "common/world/map/MapItem.hpp"

#include "stb_image.h"

#include <glm/gtc/matrix_transform.hpp>

#include <filesystem>
#include <string>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Client::Maps {

    namespace {

        using Game::Maps::MapItemSavedData;
        using Render::TextureHandle;
        using Render::INVALID_TEXTURE;

        // GL: a map rewritten while a queued frame still samples it would
        // stall the driver, so each map rotates through this many textures
        // (as the lightmap does). Vulkan writes in place — its backend gives
        // an in-use texture per-frame copies.
        constexpr int kGlRing = 3;

        struct MapInstance {
            std::shared_ptr<MapItemSavedData> data;
            std::array<TextureHandle, kGlRing> textures{INVALID_TEXTURE, INVALID_TEXTURE, INVALID_TEXTURE};
            int current = 0;
            uint32_t uploadedRevision = 0;   // 0 = never uploaded
            const MapItemSavedData* uploadedFrom = nullptr;
        };

        std::unordered_map<int32_t, MapInstance>& Instances() {
            static std::unordered_map<int32_t, MapInstance> maps;
            return maps;
        }

        // ── Static textures ─────────────────────────────────────────────

        struct Decal {
            TextureHandle texture = INVALID_TEXTURE;
            bool tried = false;
        };

        TextureHandle LoadPng(const std::string& relativePath, int* outW = nullptr, int* outH = nullptr,
                              std::vector<unsigned char>* keepPixels = nullptr) {
            if (!Render::g_renderBackend) return INVALID_TEXTURE;
            const std::string full = PlatformMain::GetAssetPath(relativePath);
            if (!std::filesystem::exists(full)) {
                Log::Warning("[Maps] missing texture %s", relativePath.c_str());
                return INVALID_TEXTURE;
            }
            int w = 0, h = 0, ch = 0;
            stbi_set_flip_vertically_on_load(0);
            unsigned char* pixels = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha);
            if (!pixels) {
                Log::Warning("[Maps] failed to decode %s", relativePath.c_str());
                return INVALID_TEXTURE;
            }
            if (outW) *outW = w;
            if (outH) *outH = h;
            TextureHandle tex = INVALID_TEXTURE;
            if (keepPixels) {
                keepPixels->assign(pixels, pixels + static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
            } else {
                tex = Render::g_renderBackend->CreateTexture2D(w, h, Render::TextureFormat::RGBA8, pixels);
                Render::g_renderBackend->SetTextureFilter(tex, Render::TextureFilter::Nearest, Render::TextureFilter::Nearest);
                Render::g_renderBackend->SetTextureWrap(tex, Render::TextureWrap::ClampToEdge, Render::TextureWrap::ClampToEdge);
            }
            stbi_image_free(pixels);
            return tex;
        }

        Decal g_background, g_checkerboard;

        // The map_decorations atlas (assets/atlases/map_decorations.json:
        // every sprite under textures/map/decorations), packed 8 × 8 px per
        // cell, eight to a row, in registry order.
        constexpr int kCell = 8;
        constexpr int kColumns = 8;
        constexpr int kTypeCount = static_cast<int>(Game::Maps::DecorationType::Count);
        constexpr int kRows = (kTypeCount + kColumns - 1) / kColumns;
        Decal g_atlas;

        TextureHandle DecorationAtlas() {
            if (g_atlas.tried) return g_atlas.texture;
            g_atlas.tried = true;
            if (!Render::g_renderBackend) return INVALID_TEXTURE;
            const int width = kColumns * kCell, height = kRows * kCell;
            std::vector<unsigned char> atlas(static_cast<size_t>(width * height * 4), 0);
            for (int i = 0; i < kTypeCount; ++i) {
                const auto& info = Game::Maps::Info(static_cast<Game::Maps::DecorationType>(i));
                std::vector<unsigned char> sprite;
                int w = 0, h = 0;
                LoadPng("assets/textures/map/decorations/" + std::string(info.asset) + ".png", &w, &h, &sprite);
                if (sprite.empty()) continue;
                const int cx = (i % kColumns) * kCell, cy = (i / kColumns) * kCell;
                for (int y = 0; y < std::min(h, kCell); ++y) {
                    for (int x = 0; x < std::min(w, kCell); ++x) {
                        const size_t src = static_cast<size_t>((x * w / std::min(w, kCell)) + y * w) * 4u;
                        const size_t dst = static_cast<size_t>((cx + x) + (cy + y) * width) * 4u;
                        for (int c = 0; c < 4; ++c) atlas[dst + static_cast<size_t>(c)] = sprite[src + static_cast<size_t>(c)];
                    }
                }
            }
            g_atlas.texture = Render::g_renderBackend->CreateTexture2D(width, height, Render::TextureFormat::RGBA8, atlas.data());
            Render::g_renderBackend->SetTextureFilter(g_atlas.texture, Render::TextureFilter::Nearest, Render::TextureFilter::Nearest);
            Render::g_renderBackend->SetTextureWrap(g_atlas.texture, Render::TextureWrap::ClampToEdge, Render::TextureWrap::ClampToEdge);
            return g_atlas.texture;
        }

        // The sprite's UV rectangle (u0, v0, u1, v1) in the atlas.
        glm::vec4 DecorationUv(Game::Maps::DecorationType type) {
            const int i = static_cast<int>(type);
            const float w = static_cast<float>(kColumns * kCell), h = static_cast<float>(kRows * kCell);
            const float u0 = static_cast<float>((i % kColumns) * kCell) / w;
            const float v0 = static_cast<float>((i / kColumns) * kCell) / h;
            return glm::vec4(u0, v0, u0 + kCell / w, v0 + kCell / h);
        }

        // MapTextureManager.MapInstance.updateTextureIfNeeded.
        TextureHandle PrepareTexture(MapInstance& instance) {
            if (!Render::g_renderBackend || !instance.data) return INVALID_TEXTURE;
            const bool rotate = Render::g_renderBackend->GetType() == Render::BackendType::OpenGL;
            if (instance.textures[0] == INVALID_TEXTURE) {
                const int count = rotate ? kGlRing : 1;
                std::vector<unsigned char> clear(static_cast<size_t>(Game::Maps::kMapSize * Game::Maps::kMapSize * 4), 0);
                for (int i = 0; i < count; ++i) {
                    TextureHandle tex = Render::g_renderBackend->CreateTexture2D(
                        Game::Maps::kMapSize, Game::Maps::kMapSize, Render::TextureFormat::RGBA8, clear.data());
                    Render::g_renderBackend->SetTextureFilter(tex, Render::TextureFilter::Nearest, Render::TextureFilter::Nearest);
                    Render::g_renderBackend->SetTextureWrap(tex, Render::TextureWrap::ClampToEdge, Render::TextureWrap::ClampToEdge);
                    instance.textures[static_cast<size_t>(i)] = tex;
                }
            }
            const MapItemSavedData& data = *instance.data;
            if (instance.uploadedRevision != data.ColorRevision() || instance.uploadedFrom != &data) {
                std::vector<uint32_t> pixels(static_cast<size_t>(Game::Maps::kMapSize * Game::Maps::kMapSize));
                for (size_t i = 0; i < pixels.size(); ++i) {
                    // MapColor.getColorFromPackedId is ARGB; the texture is
                    // RGBA bytes in memory (little-endian ABGR word).
                    const uint32_t argb = Game::MapColors::ArgbFromPackedId(data.colors[i]);
                    const uint32_t a = (argb >> 24) & 0xFFu, r = (argb >> 16) & 0xFFu;
                    const uint32_t g = (argb >> 8) & 0xFFu, b = argb & 0xFFu;
                    pixels[i] = r | (g << 8) | (b << 16) | (a << 24);
                }
                const int next = rotate ? (instance.current + 1) % kGlRing : 0;
                Render::g_renderBackend->UpdateTexture2D(instance.textures[static_cast<size_t>(next)], 0, 0,
                                                         Game::Maps::kMapSize, Game::Maps::kMapSize, pixels.data());
                instance.current = next;
                instance.uploadedRevision = data.ColorRevision();
                instance.uploadedFrom = &data;
            }
            return instance.textures[static_cast<size_t>(instance.current)];
        }

        void DestroyTextures(MapInstance& instance) {
            if (!Render::g_renderBackend) return;
            for (TextureHandle& tex : instance.textures) {
                if (tex != INVALID_TEXTURE) Render::g_renderBackend->DeferredDestroyTexture(tex);
                tex = INVALID_TEXTURE;
            }
        }

        // MapItemBridge's client source (the cartography table and map
        // extending predict from the client's copy).
        bool ClientMapInfo(int32_t mapId, int& scale, bool& locked) {
            const auto& maps = Instances();
            const auto it = maps.find(mapId);
            if (it == maps.end() || !it->second.data) return false;
            scale = it->second.data->scale;
            locked = it->second.data->locked;
            return true;
        }

        void RegisterClientSource() {
            static bool registered = false;
            if (registered) return;
            registered = true;
            Game::MapItemBridge::SetClientMapInfoSource(&ClientMapInfo);
        }

        glm::vec3 Apply(const glm::mat4& m, float x, float y, float z) {
            return glm::vec3(m * glm::vec4(x, y, z, 1.0f));
        }
    }

    std::shared_ptr<MapItemSavedData> GetMapData(int32_t mapId) {
        const auto& maps = Instances();
        const auto it = maps.find(mapId);
        return it != maps.end() ? it->second.data : nullptr;
    }

    void HandleMapItemData(const Network::MapItemDataS2CPacket& packet) {
        RegisterClientSource();
        MapInstance& instance = Instances()[packet.mapId];
        if (!instance.data) {
            instance.data = MapItemSavedData::CreateForClient(packet.scale, packet.locked,
                                                              ClientLevels::PacketDimension());
        }
        // ClientboundMapItemDataPacket.applyToMap.
        if (packet.decorations) instance.data->AddClientSideDecorations(*packet.decorations);
        if (packet.colorPatch) packet.colorPatch->ApplyToMap(*instance.data);
        // MapTextureManager.update → forceUpload: the next draw re-uploads.
        instance.uploadedRevision = 0;
    }

    bool BuildMapGeometry(int32_t mapId, const glm::mat4& pose, bool showOnlyFrame, MapGeometry& out) {
        auto& maps = Instances();
        const auto it = maps.find(mapId);
        if (it == maps.end() || !it->second.data) return false;
        MapInstance& instance = it->second;

        out.mapTexture = PrepareTexture(instance);
        out.mapQuad.pos = {Apply(pose, 0.0f, 128.0f, -0.01f), Apply(pose, 128.0f, 128.0f, -0.01f),
                           Apply(pose, 128.0f, 0.0f, -0.01f), Apply(pose, 0.0f, 0.0f, -0.01f)};
        out.mapQuad.uv = {glm::vec2(0.0f, 1.0f), glm::vec2(1.0f, 1.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f, 0.0f)};

        out.decorations.clear();
        out.decorationTexture = DecorationAtlas();
        int count = 0;
        for (const auto& [key, decoration] : instance.data->Decorations()) {
            if (showOnlyFrame && !decoration.RenderOnFrame()) continue;
            glm::mat4 m = glm::translate(pose, glm::vec3(static_cast<float>(decoration.x) / 2.0f + 64.0f,
                                                         static_cast<float>(decoration.y) / 2.0f + 64.0f, -0.02f));
            m = glm::rotate(m, glm::radians(static_cast<float>(decoration.rot * 360) / 16.0f), glm::vec3(0.0f, 0.0f, 1.0f));
            m = glm::scale(m, glm::vec3(4.0f, 4.0f, 3.0f));
            m = glm::translate(m, glm::vec3(-0.125f, 0.125f, 0.0f));
            const float z = static_cast<float>(count) * -0.001f;
            const glm::vec4 uv = DecorationUv(decoration.type);
            MapQuad quad;
            quad.pos = {Apply(m, -1.0f, 1.0f, z), Apply(m, 1.0f, 1.0f, z), Apply(m, 1.0f, -1.0f, z), Apply(m, -1.0f, -1.0f, z)};
            quad.uv = {glm::vec2(uv.x, uv.y), glm::vec2(uv.z, uv.y), glm::vec2(uv.z, uv.w), glm::vec2(uv.x, uv.w)};
            out.decorations.push_back(quad);
            ++count;
        }
        return true;
    }

    TextureHandle MapBackgroundTexture(bool checkerboard) {
        Decal& decal = checkerboard ? g_checkerboard : g_background;
        if (!decal.tried) {
            decal.tried = true;
            decal.texture = LoadPng(checkerboard ? "assets/textures/map/map_background_checkerboard.png"
                                                 : "assets/textures/map/map_background.png");
        }
        return decal.texture;
    }

    void Clear() {
        for (auto& [id, instance] : Instances()) DestroyTextures(instance);
        Instances().clear();
    }

} // namespace Client::Maps

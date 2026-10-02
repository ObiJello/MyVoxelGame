// File: src/client/renderer/entity/ShieldTextures.cpp
#include "ShieldTextures.hpp"

#include "client/renderer/backend/RenderBackend.hpp"
#include "client/resource/ResourcePacks.hpp"
#include "common/core/Log.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/DyeColorUtil.hpp"
#include "common/world/banner/BannerPatterns.hpp"

#include "stb_image.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render::ShieldTextures {

    namespace {

        struct Image {
            int w = 0, h = 0;
            std::vector<unsigned char> rgba;
        };

        bool Load(const std::string& relative, Image& out) {
            const std::string full = PlatformMain::GetAssetPath(relative);
            if (!std::filesystem::exists(full)) return false;
            int ch = 0;
            stbi_set_flip_vertically_on_load(0);
            unsigned char* pixels = stbi_load(full.c_str(), &out.w, &out.h, &ch, STBI_rgb_alpha);
            if (!pixels) return false;
            out.rgba.assign(pixels, pixels + static_cast<size_t>(out.w) * static_cast<size_t>(out.h) * 4u);
            stbi_image_free(pixels);
            return true;
        }

        // One bannerPattern pass: the layer's texel times the tint, blended
        // over what is below by its alpha.
        void Blend(Image& dst, const Image& layer, uint32_t rgb) {
            if (layer.w != dst.w || layer.h != dst.h) return;
            const float tr = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
            const float tg = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
            const float tb = static_cast<float>(rgb & 0xFF) / 255.0f;
            for (size_t i = 0; i + 3 < dst.rgba.size(); i += 4) {
                const float a = static_cast<float>(layer.rgba[i + 3]) / 255.0f;
                if (a <= 0.0f) continue;
                const float da = static_cast<float>(dst.rgba[i + 3]) / 255.0f;
                const float sr = layer.rgba[i] * tr, sg = layer.rgba[i + 1] * tg, sb = layer.rgba[i + 2] * tb;
                dst.rgba[i]     = static_cast<unsigned char>(std::clamp(sr * a + dst.rgba[i] * (1.0f - a), 0.0f, 255.0f));
                dst.rgba[i + 1] = static_cast<unsigned char>(std::clamp(sg * a + dst.rgba[i + 1] * (1.0f - a), 0.0f, 255.0f));
                dst.rgba[i + 2] = static_cast<unsigned char>(std::clamp(sb * a + dst.rgba[i + 2] * (1.0f - a), 0.0f, 255.0f));
                dst.rgba[i + 3] = static_cast<unsigned char>(std::clamp((a + da * (1.0f - a)) * 255.0f, 0.0f, 255.0f));
            }
        }

        TextureHandle Upload(const Image& img) {
            if (!g_renderBackend || img.rgba.empty()) return INVALID_TEXTURE;
            TextureHandle tex = g_renderBackend->CreateTexture2D(img.w, img.h, TextureFormat::RGBA8, img.rgba.data());
            if (tex != INVALID_TEXTURE) {
                g_renderBackend->SetTextureFilter(tex, TextureFilter::Nearest, TextureFilter::Nearest);
                g_renderBackend->SetTextureWrap(tex, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            }
            return tex;
        }

        std::unordered_map<std::string, TextureHandle>& Cache() {
            static std::unordered_map<std::string, TextureHandle> cache;
            static int generation = -1;
            if (Resources::CacheStale(generation)) {
                if (g_renderBackend) {
                    for (auto& [key, tex] : cache) if (tex != INVALID_TEXTURE) g_renderBackend->DestroyTexture(tex);
                }
                cache.clear();
            }
            return cache;
        }

        std::string StripNamespace(const std::string& id) {
            const size_t colon = id.find(':');
            return colon == std::string::npos ? id : id.substr(colon + 1);
        }

    } // namespace

    TextureHandle ForStack(const Game::ItemStack& stack) {
        const auto patterns = stack.get(Game::DataComponents::BANNER_PATTERNS);
        const auto baseColor = stack.get(Game::DataComponents::BASE_COLOR);
        const bool hasPatterns = (patterns && !patterns->layers.empty()) || baseColor.has_value();

        std::string key = hasPatterns ? "base" : "plain";
        if (hasPatterns) {
            key += ":" + std::to_string(baseColor.value_or(0));
            if (patterns) {
                const size_t count = std::min<size_t>(patterns->layers.size(), 16);
                for (size_t i = 0; i < count; ++i) {
                    key += "|" + patterns->layers[i].pattern + "/" + std::to_string(patterns->layers[i].color);
                }
            }
        }
        auto& cache = Cache();
        if (auto it = cache.find(key); it != cache.end()) return it->second;

        TextureHandle tex = INVALID_TEXTURE;
        Image img;
        if (!hasPatterns) {
            if (Load("assets/textures/entity/shield_base_nopattern.png", img)) tex = Upload(img);
        } else if (Load("assets/textures/entity/shield_base.png", img)) {
            // Sheets.SHIELD_PATTERN_BASE in the base colour (white default).
            Image layer;
            if (Load("assets/textures/entity/shield/base.png", layer)) {
                Blend(img, layer, Game::DyeTextureDiffuseColor(static_cast<uint8_t>(baseColor.value_or(0) & 0x0F)));
            }
            if (patterns) {
                const size_t count = std::min<size_t>(patterns->layers.size(), 16);
                for (size_t i = 0; i < count; ++i) {
                    const Game::BannerPatternLayer& l = patterns->layers[i];
                    // Sheets.getShieldSprite: entity/shield/<asset path>.
                    const std::string asset = StripNamespace(Game::BannerPatterns::AssetOf(l.pattern));
                    Image pattern;
                    if (Load("assets/textures/entity/shield/" + asset + ".png", pattern)) {
                        Blend(img, pattern, Game::DyeTextureDiffuseColor(l.color));
                    }
                }
            }
            tex = Upload(img);
        }
        if (tex == INVALID_TEXTURE) Log::Warning("[ShieldTextures] could not build shield sheet %s", key.c_str());
        cache[key] = tex;
        return tex;
    }

} // namespace Render::ShieldTextures

// File: src/client/renderer/gui/GuiAtlas.cpp
#include "GuiAtlas.hpp"
#include "../backend/RenderBackend.hpp"
#include "common/core/Log.hpp"
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>

// stb_image for PNG loading
#include "../ext/stb_image/stb_image.h"

namespace fs = std::filesystem;

namespace Render {

    GuiAtlas::~GuiAtlas() {
        Shutdown();
    }

    bool GuiAtlas::Initialize(const std::string& spritesRootDir) {
        if (!g_renderBackend) {
            Log::Error("[GuiAtlas] No render backend available");
            return false;
        }

        if (!fs::exists(spritesRootDir)) {
            Log::Error("[GuiAtlas] Sprites directory not found: %s", spritesRootDir.c_str());
            return false;
        }

        // Collect all sprite PNGs recursively
        std::vector<RawSprite> sprites;
        CollectSprites(spritesRootDir, "", sprites);

        if (sprites.empty()) {
            Log::Warning("[GuiAtlas] No sprites found in %s", spritesRootDir.c_str());
            return false;
        }

        Log::Info("[GuiAtlas] Found %zu sprites, packing atlas...", sprites.size());

        // Pack into atlas and upload to GPU
        bool result = PackAndUpload(sprites);

        // Free pixel data
        for (auto& sprite : sprites) {
            if (sprite.pixels) {
                stbi_image_free(sprite.pixels);
                sprite.pixels = nullptr;
            }
        }

        if (result) {
            Log::Info("[GuiAtlas] Atlas created: %dx%d with %d sprites",
                     m_atlasWidth, m_atlasHeight, static_cast<int>(m_sprites.size()));
        }

        return result;
    }

    void GuiAtlas::Shutdown() {
        if (g_renderBackend && m_atlasTexture != INVALID_TEXTURE) {
            g_renderBackend->DestroyTexture(m_atlasTexture);
            m_atlasTexture = INVALID_TEXTURE;
        }
        m_sprites.clear();
        m_atlasWidth = m_atlasHeight = 0;
    }

    const SpriteInfo* GuiAtlas::GetSprite(const std::string& id) const {
        auto it = m_sprites.find(id);
        if (it != m_sprites.end()) {
            return &it->second;
        }
        return nullptr;
    }

    void GuiAtlas::CollectSprites(const std::string& rootDir, const std::string& prefix,
                                  std::vector<RawSprite>& outSprites) {
        for (const auto& entry : fs::recursive_directory_iterator(rootDir)) {
            if (!entry.is_regular_file()) continue;

            std::string ext = entry.path().extension().string();
            if (ext != ".png") continue;

            // Skip .mcmeta files (they're parsed alongside their PNG)
            std::string filename = entry.path().filename().string();
            if (filename.find(".mcmeta") != std::string::npos) continue;

            // Convert path to sprite ID: remove root dir prefix and .png extension
            std::string relativePath = fs::relative(entry.path(), rootDir).string();
            // Remove .png extension
            std::string spriteId = relativePath.substr(0, relativePath.length() - 4);
            // Normalize path separators to forward slash
            std::replace(spriteId.begin(), spriteId.end(), '\\', '/');

            // Load PNG — do NOT flip vertically for GUI sprites.
            // The ortho projection has Y top-to-bottom, so textures are loaded as-is.
            // (The block atlas flips for OpenGL 3D, but GUI doesn't need that.)
            stbi_set_flip_vertically_on_load(0);

            int width = 0, height = 0, channels = 0;
            unsigned char* pixels = stbi_load(entry.path().string().c_str(),
                                              &width, &height, &channels, STBI_rgb_alpha);
            if (!pixels) {
                Log::Warning("[GuiAtlas] Failed to load sprite: %s", entry.path().string().c_str());
                continue;
            }

            RawSprite sprite;
            sprite.id = spriteId;
            sprite.pixels = pixels;
            sprite.width = width;
            sprite.height = height;

            // Check for .mcmeta file
            std::string mcmetaPath = entry.path().string() + ".mcmeta";
            if (fs::exists(mcmetaPath)) {
                ParseMcMeta(mcmetaPath, sprite);
            }

            outSprites.push_back(std::move(sprite));
        }
    }

    void GuiAtlas::ParseMcMeta(const std::string& mcmetaPath, RawSprite& sprite) {
        try {
            std::ifstream file(mcmetaPath);
            if (!file.is_open()) return;

            nlohmann::json json;
            file >> json;

            if (json.contains("gui") && json["gui"].contains("scaling")) {
                auto& scaling = json["gui"]["scaling"];
                std::string type = scaling.value("type", "stretch");

                if (type == "tile") {
                    sprite.scaling = SpriteScaling::Tile;
                    sprite.tileWidth = scaling.value("width", sprite.width);
                    sprite.tileHeight = scaling.value("height", sprite.height);
                } else if (type == "nine_slice") {
                    sprite.scaling = SpriteScaling::NineSlice;
                    sprite.stretchInner = scaling.value("stretch_inner", true);
                    if (scaling.contains("border")) {
                        auto& border = scaling["border"];
                        if (border.is_number()) {
                            int b = border.get<int>();
                            sprite.border = { b, b, b, b };
                        } else {
                            sprite.border.left = border.value("left", 0);
                            sprite.border.top = border.value("top", 0);
                            sprite.border.right = border.value("right", 0);
                            sprite.border.bottom = border.value("bottom", 0);
                        }
                    }
                    // NineSlice original size from JSON (or fall back to image size)
                    if (scaling.contains("width")) {
                        sprite.tileWidth = scaling["width"].get<int>();
                    }
                    if (scaling.contains("height")) {
                        sprite.tileHeight = scaling["height"].get<int>();
                    }
                }
                // stretch is default, nothing to set
            }
        } catch (const std::exception& e) {
            Log::Warning("[GuiAtlas] Failed to parse mcmeta: %s - %s", mcmetaPath.c_str(), e.what());
        }
    }

    bool GuiAtlas::PackAndUpload(std::vector<RawSprite>& sprites) {
        // Sort sprites by height descending (simple shelf-packing heuristic)
        std::sort(sprites.begin(), sprites.end(), [](const RawSprite& a, const RawSprite& b) {
            return a.height > b.height;
        });

        // Determine atlas size — start at 512, grow if needed
        int atlasSize = 512;
        const int maxAtlasSize = 4096;
        const int padding = 1; // 1px padding between sprites

        bool packed = false;
        while (!packed && atlasSize <= maxAtlasSize) {
            // Try shelf-packing at this atlas size
            int shelfX = padding;
            int shelfY = padding;
            int shelfHeight = 0;
            bool fits = true;

            for (auto& sprite : sprites) {
                // Does this sprite fit on the current shelf?
                if (shelfX + sprite.width + padding > atlasSize) {
                    // Move to next shelf
                    shelfX = padding;
                    shelfY += shelfHeight + padding;
                    shelfHeight = 0;
                }

                if (shelfY + sprite.height + padding > atlasSize) {
                    fits = false;
                    break;
                }

                shelfHeight = std::max(shelfHeight, sprite.height);
                shelfX += sprite.width + padding;
            }

            if (fits) {
                packed = true;
            } else {
                atlasSize *= 2;
            }
        }

        if (!packed) {
            Log::Error("[GuiAtlas] Sprites don't fit in %dx%d atlas", maxAtlasSize, maxAtlasSize);
            return false;
        }

        m_atlasWidth = atlasSize;
        m_atlasHeight = atlasSize;

        // Allocate atlas pixel buffer (RGBA)
        std::vector<unsigned char> atlasPixels(atlasSize * atlasSize * 4, 0);

        // Pack sprites and record positions
        int shelfX = padding;
        int shelfY = padding;
        int shelfHeight = 0;

        for (auto& sprite : sprites) {
            if (shelfX + sprite.width + padding > atlasSize) {
                shelfX = padding;
                shelfY += shelfHeight + padding;
                shelfHeight = 0;
            }

            // Copy sprite pixels into atlas
            for (int y = 0; y < sprite.height; y++) {
                int srcRow = y;
                int dstRow = shelfY + y;
                int srcOffset = srcRow * sprite.width * 4;
                int dstOffset = (dstRow * atlasSize + shelfX) * 4;
                memcpy(&atlasPixels[dstOffset], &sprite.pixels[srcOffset], sprite.width * 4);
            }

            // Calculate UV coordinates
            float u0 = static_cast<float>(shelfX) / atlasSize;
            float v0 = static_cast<float>(shelfY) / atlasSize;
            float u1 = static_cast<float>(shelfX + sprite.width) / atlasSize;
            float v1 = static_cast<float>(shelfY + sprite.height) / atlasSize;

            SpriteInfo info;
            info.u0 = u0;
            info.v0 = v0;
            info.u1 = u1;
            info.v1 = v1;
            info.width = sprite.width;
            info.height = sprite.height;
            info.scaling = sprite.scaling;
            info.tileWidth = sprite.tileWidth;
            info.tileHeight = sprite.tileHeight;
            info.border = sprite.border;
            info.stretchInner = sprite.stretchInner;

            m_sprites[sprite.id] = info;

            shelfX += sprite.width + padding;
            shelfHeight = std::max(shelfHeight, sprite.height);
        }

        // Upload to GPU
        m_atlasTexture = g_renderBackend->CreateTexture2D(
            atlasSize, atlasSize, TextureFormat::RGBA8, atlasPixels.data());

        if (m_atlasTexture == INVALID_TEXTURE) {
            Log::Error("[GuiAtlas] Failed to create atlas GPU texture");
            return false;
        }

        // Pixel art filtering — nearest neighbor, no mipmaps
        g_renderBackend->SetTextureFilter(m_atlasTexture, TextureFilter::Nearest, TextureFilter::Nearest);
        g_renderBackend->SetTextureWrap(m_atlasTexture, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        return true;
    }

} // namespace Render

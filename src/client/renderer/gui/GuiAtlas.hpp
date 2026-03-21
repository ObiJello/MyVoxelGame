// File: src/client/renderer/gui/GuiAtlas.hpp
// GUI sprite atlas — loads all sprites from assets/textures/gui/sprites/ into one GPU texture.
// Matches MC's SpriteLoader + TextureAtlas for the GUI atlas.
#pragma once

#include "../backend/RenderTypes.hpp"
#include <string>
#include <unordered_map>

namespace Render {

    // Sprite scaling mode (MC's GuiSpriteScaling)
    enum class SpriteScaling {
        Stretch,    // Default — scale freely
        Tile,       // Repeat at fixed tile size
        NineSlice   // 9-patch borders
    };

    struct NineSliceBorder {
        int left = 0, top = 0, right = 0, bottom = 0;
    };

    struct SpriteInfo {
        float u0, v0, u1, v1;  // Normalized UV coordinates in atlas
        int width, height;      // Original sprite dimensions (pixels)
        SpriteScaling scaling = SpriteScaling::Stretch;
        // Tile mode
        int tileWidth = 0, tileHeight = 0;
        // NineSlice mode
        NineSliceBorder border;
        bool stretchInner = true;
    };

    class GuiAtlas {
    public:
        GuiAtlas() = default;
        ~GuiAtlas();

        // Initialize by loading all sprites from the given root directory
        bool Initialize(const std::string& spritesRootDir);
        void Shutdown();

        // Look up a sprite by ID (e.g., "hud/hotbar")
        // Returns nullptr if not found
        const SpriteInfo* GetSprite(const std::string& id) const;

        // Get the atlas GPU texture handle
        TextureHandle GetTextureHandle() const { return m_atlasTexture; }

        int GetAtlasWidth() const { return m_atlasWidth; }
        int GetAtlasHeight() const { return m_atlasHeight; }
        int GetSpriteCount() const { return static_cast<int>(m_sprites.size()); }

    private:
        TextureHandle m_atlasTexture = INVALID_TEXTURE;
        int m_atlasWidth = 0;
        int m_atlasHeight = 0;
        std::unordered_map<std::string, SpriteInfo> m_sprites;

        // Load a single sprite PNG, returns pixel data (caller frees)
        struct RawSprite {
            std::string id;
            unsigned char* pixels = nullptr;
            int width = 0, height = 0;
            SpriteScaling scaling = SpriteScaling::Stretch;
            int tileWidth = 0, tileHeight = 0;
            NineSliceBorder border;
            bool stretchInner = true;
        };

        void CollectSprites(const std::string& rootDir, const std::string& prefix,
                           std::vector<RawSprite>& outSprites);
        void ParseMcMeta(const std::string& mcmetaPath, RawSprite& sprite);
        bool PackAndUpload(std::vector<RawSprite>& sprites);
    };

} // namespace Render

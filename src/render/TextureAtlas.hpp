#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <glm/glm.hpp>
#include <glad/glad.h>

namespace Render {

    // UV coordinates for a single texture tile in the atlas
    struct AtlasTile {
        glm::vec2 uvMin;  // Bottom-left UV (0-1 range)
        glm::vec2 uvMax;  // Top-right UV (0-1 range)
        
        AtlasTile() : uvMin(0.0f), uvMax(0.0f) {}
        AtlasTile(glm::vec2 min, glm::vec2 max) : uvMin(min), uvMax(max) {}
    };

    class TextureAtlas {
    public:
        // Atlas configuration
        static constexpr int ATLAS_WIDTH = 256;         // 256 pixels wide
        static constexpr int ATLAS_HEIGHT = 1024;       // 1024 pixels tall
        static constexpr int TILE_SIZE = 16;            // 16x16 per tile
        static constexpr int TILES_PER_ROW = ATLAS_WIDTH / TILE_SIZE;   // 16 tiles per row
        static constexpr int TILES_PER_COLUMN = ATLAS_HEIGHT / TILE_SIZE; // 64 tiles per column
        static constexpr int MAX_TILES = TILES_PER_ROW * TILES_PER_COLUMN; // 1024 total tiles

        TextureAtlas();
        ~TextureAtlas();

        // Initialize the atlas by loading a single atlas texture file
        bool Initialize(const std::string& atlasPath = "assets/textures/atlas.png");

        // Get UV coordinates for a specific atlas index - CHANGED TO uint16_t
        AtlasTile GetTile(uint16_t atlasIndex) const;

        // Bind the atlas texture for rendering
        void Bind(GLenum textureUnit = GL_TEXTURE0) const;

        // Get the OpenGL texture ID
        GLuint GetTextureID() const { return textureID; }

        // Check if atlas is successfully loaded
        bool IsLoaded() const { return isLoaded; }

        // Get the number of loaded textures
        size_t GetLoadedTextureCount() const { return loadedTextures.size(); }

        // Register a texture manually (for dynamic textures) - CHANGED TO uint16_t
        uint16_t RegisterTexture(const std::string& name, const unsigned char* data, int width, int height);

    private:
        GLuint textureID;
        bool isLoaded;
        std::vector<std::string> loadedTextures;  // Track loaded texture names
        std::unordered_map<std::string, uint16_t> textureNameToIndex; // CHANGED TO uint16_t
        uint16_t nextAvailableIndex; // CHANGED TO uint16_t

        // Atlas pixel data (RGBA format)
        std::vector<unsigned char> atlasData;

        // Initialize the atlas texture with default/error patterns
        void InitializeAtlasData();

        // Load a single PNG file and copy it to the atlas at the specified index - CHANGED TO uint16_t
        bool LoadTextureToAtlas(const std::string& filePath, uint16_t atlasIndex);

        // Create a checkerboard error texture at the specified atlas index - CHANGED TO uint16_t
        void CreateErrorTexture(uint16_t atlasIndex);

        // Create a solid color texture at the specified atlas index - CHANGED TO uint16_t
        void CreateSolidTexture(uint16_t atlasIndex, unsigned char r, unsigned char g, unsigned char b, unsigned char a = 255);

        // Copy tile data into the atlas at the specified index - CHANGED TO uint16_t
        void CopyTileToAtlas(const unsigned char* tileData, uint16_t atlasIndex, int tileWidth, int tileHeight);

        // Upload the atlas data to OpenGL
        void UploadToGPU();

        // Utility: Get atlas pixel coordinates from index - CHANGED TO uint16_t
        void GetAtlasCoords(uint16_t atlasIndex, int& x, int& y) const;
    };

    // Global atlas instance (defined in TextureAtlas.cpp)
    extern TextureAtlas g_textureAtlas;

} // namespace Render
// File: src/client/renderer/texture/AtlasBuilder.hpp
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <glm/glm.hpp>
#include <glad/glad.h>
#include <nlohmann/json.hpp>

// Forward declaration for animation system
namespace Render {
    class TextureAnimator;
}

// Animation data structure
struct TextureAnimation {
    int width = 16;           // Frame width
    int height = 16;          // Frame height 
    int frameCount = 1;       // Total number of frames
    int frametime = 1;        // Ticks per frame (20 ticks = 1 second)
    bool interpolate = false; // Whether to interpolate between frames
    std::vector<int> frames;  // Custom frame sequence (empty = use all frames in order)
};

namespace Render {

    // Represents a single texture source from the atlas JSON
    struct TextureSource {
        std::string key;        // e.g. "minecraft:block/grass_block_side"
        std::string path;       // e.g. "assets/textures/block/grass_block_side.png"
        int width = 0;
        int height = 0;
        std::vector<unsigned char> data;  // RGBA pixel data
    };

    // UV coordinates for a texture in the atlas
    struct AtlasUVRect {
        glm::vec2 uvMin;
        glm::vec2 uvMax;

        AtlasUVRect() = default;
        AtlasUVRect(float u0, float v0, float u1, float v1)
            : uvMin(u0, v0), uvMax(u1, v1) {}
    };

    // Rectangle for bin packing
    struct PackRect {
        int x, y, width, height;
        int textureIndex;  // Index into TextureSource array

        PackRect() : x(0), y(0), width(0), height(0), textureIndex(-1) {}
        PackRect(int w, int h, int idx) : x(0), y(0), width(w), height(h), textureIndex(idx) {}
    };

    // Simple bin packing node for texture atlas
    struct PackNode {
        int x, y, width, height;
        bool used;
        std::unique_ptr<PackNode> left;
        std::unique_ptr<PackNode> right;

        PackNode(int x, int y, int w, int h)
            : x(x), y(y), width(w), height(h), used(false) {}
    };

    class AtlasBuilder {
    public:
        // Configuration
        static constexpr int DEFAULT_ATLAS_SIZE = 2048;  // 2048x2048 atlas
        static constexpr int MIN_ATLAS_SIZE = 512;
        static constexpr int MAX_ATLAS_SIZE = 8192;

        AtlasBuilder();
        ~AtlasBuilder();

        // Main build process - parses JSON and creates atlas
        bool BuildFromJSON(const std::string& atlasJsonPath,
                          const std::string& texturesRootPath = "assets/textures");

        // Get the packed atlas texture ID
        GLuint GetAtlasTextureID() const { return atlasTextureID; }

        // Get grass/foliage colormap texture IDs
        GLuint GetGrassColormapID() const { return grassColormapID; }
        GLuint GetFoliageColormapID() const { return foliageColormapID; }

        // Look up UV coordinates for a texture key
        bool GetUVRect(const std::string& textureKey, AtlasUVRect& uvRect) const;

        // Get atlas dimensions
        int GetAtlasWidth() const { return atlasWidth; }
        int GetAtlasHeight() const { return atlasHeight; }

        // Get statistics
        size_t GetTextureCount() const { return textureSources.size(); }
        size_t GetPackedCount() const { return textureKeyToUV.size(); }

        // **NEW**: Mipmap control
        void SetMipmapEnabled(bool enabled);
        bool IsMipmapEnabled() const { return mipmapEnabled; }

        // Debug: Save atlas to file
        bool SaveAtlasDebugImage(const std::string& outputPath) const;

        // **NEW**: Animation support
        void SetTextureAnimator(TextureAnimator* animator);
        TextureAnimator* GetTextureAnimator() const { return textureAnimator; }

    private:
        // OpenGL texture IDs
        GLuint atlasTextureID;
        GLuint grassColormapID;
        GLuint foliageColormapID;

        // Atlas dimensions
        int atlasWidth;
        int atlasHeight;

        // **NEW**: Mipmap state
        bool mipmapEnabled;

        // **NEW**: Animation support
        TextureAnimator* textureAnimator;
        
        // **NEW**: Animation data storage
        struct PendingAnimation {
            std::string textureKey;
            TextureAnimation animation;
            std::vector<std::vector<unsigned char>> frames;
        };
        std::vector<PendingAnimation> pendingAnimations;

        // Texture sources and packed data
        std::vector<TextureSource> textureSources;
        std::unordered_map<std::string, AtlasUVRect> textureKeyToUV;

        // Atlas pixel data (for debug saving)
        std::vector<unsigned char> atlasData;

        // Step 1: Parse the JSON atlas descriptor
        bool ParseAtlasJSON(const std::string& jsonPath,
                           const std::string& texturesRoot,
                           std::vector<TextureSource>& sources);

        // Step 2: Load biome colormaps
        bool LoadColormaps(const std::string& texturesRoot);

        // Step 3: Load all texture PNGs
        bool LoadAllTextures(std::vector<TextureSource>& sources);

        // Step 4: Pack textures into atlas
        bool PackTextures(const std::vector<TextureSource>& sources,
                         std::vector<PackRect>& packedRects,
                         int& outWidth, int& outHeight);

        // Step 5: Create atlas texture and upload to GPU
        bool CreateAtlasTexture(const std::vector<TextureSource>& sources,
                               const std::vector<PackRect>& packedRects);

        // **NEW**: Update texture parameters (for mipmap changes)
        void UpdateTextureParameters();

        // Helper: Process directory source from JSON
        void ProcessDirectorySource(const nlohmann::json& source,
                                   const std::string& texturesRoot,
                                   std::vector<TextureSource>& sources);

        // Helper: Process single source from JSON
        void ProcessSingleSource(const nlohmann::json& source,
                                const std::string& texturesRoot,
                                std::vector<TextureSource>& sources);

        // Helper: Scan directory for PNG files
        std::vector<std::string> ScanDirectoryForPNGs(const std::string& dirPath);

        // Helper: Load a single PNG file
        bool LoadPNG(const std::string& filePath,
                    int& width, int& height,
                    std::vector<unsigned char>& data);

        // Helper: Create and upload a colormap texture
        GLuint CreateColormapTexture(const std::vector<unsigned char>& data,
                                    int width, int height);

        // Bin packing algorithm
        PackNode* InsertRect(PackNode* node, int width, int height, int index);

        // Helper: Copy texture to atlas at specified position
        void CopyTextureToAtlas(const TextureSource& source,
                               int destX, int destY);

        // **NEW**: Animation helper methods
        bool ParseMcMetaFile(const std::string& mcmetaPath, TextureAnimation& animation);
        bool LoadAnimatedTexture(const std::string& texturePath, 
                                TextureSource& source,
                                TextureAnimation& animation,
                                std::vector<std::vector<unsigned char>>& frames);
    };

    // Global atlas builder instance (optional - can be created as needed)
    extern std::unique_ptr<AtlasBuilder> g_atlasBuilder;

} // namespace Render
// File: src/client/renderer/texture/AtlasBuilder.hpp
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>
#include <memory>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include "../backend/RenderTypes.hpp"

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
    // Per-entry durations, parallel to `frames` (MC AnimationFrame.time:
    // a frame object's "time", else the section's frametime). 0 = frametime.
    // Empty when the .mcmeta lists no timed frame.
    std::vector<int> frameTimes;

    // The .mcmeta's optional `width` / `height`, 0 when absent. MC's
    // AnimationMetadataSection.calculateFrameSize only falls back to a
    // min(spriteWidth, spriteHeight) SQUARE when neither is given; a lone
    // `width` keeps the full sprite height and vice versa.
    int declaredFrameWidth = 0;
    int declaredFrameHeight = 0;
};

namespace Render {

    // Represents a single texture source from the atlas JSON
    struct TextureSource {
        std::string key;        // e.g. "minecraft:block/grass_block_side"
        std::string path;       // e.g. "assets/textures/block/grass_block_side.png"
        int width = 0;
        int height = 0;
        std::vector<unsigned char> data;  // RGBA pixel data

        // From the sibling .mcmeta's `texture` section, feeding MC's mipmap
        // pipeline (see texture/MipmapGenerator.hpp). Empty strategy = "auto".
        // These are not cosmetic knobs: this asset set overrides the strategy
        // on 44 sprites, and every leaf texture asks for "dark_cutout".
        std::string mipmapStrategy;
        float       alphaCutoffBias = 0.0f;
    };

    // UV coordinates for a texture in the atlas
    struct AtlasUVRect {
        glm::vec2 uvMin;
        glm::vec2 uvMax;
        // Row in the sprite table (GetSpriteTableHandle): what a greedy-merged
        // terrain quad carries instead of this rect. Assigned by
        // BuildSpriteTable in key order, stable for the atlas's lifetime.
        uint16_t  spriteId = 0;

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

    // The sprite atlases, as MC 26 splits them (AtlasManager.KNOWN_ATLASES):
    // BLOCKS (block/ + a few entity sprites, mipmapped) and ITEMS (item/,
    // never mipmapped). items.json filters out the engine's 1254 px portal
    // gun icon: nothing samples it from an atlas (item icons, held and
    // dropped items all draw their own PNG — HeldItemSpriteMesh,
    // GuiGraphics::LoadItemTexture), and alone it would take the item sheet
    // from 1024x512 to 2048x2048.
    enum class AtlasId : uint8_t { Blocks = 0, Items = 1 };
    constexpr int kAtlasCount = 2;

    // MC AtlasManager.AtlasConfig plus what only the block atlas carries.
    struct AtlasConfig {
        const char* name = "blocks";                           // logs, debug dump name
        const char* definition = "assets/atlases/blocks.json"; // the sources
        bool createMipmaps = false;       // MC: blocks only
        bool colormaps = false;           // grass / foliage colormaps (blocks)
        bool connectedTextures = false;   // CTM variant sprites (blocks)
        bool spriteTable = false;         // greedy terrain's sprite table (blocks)
    };
    const AtlasConfig& GetAtlasConfig(AtlasId id);

    class AtlasBuilder {
    public:
        // MC's mipmap ceiling (Options.mipmapLevels max): the block atlas is
        // stitched for it, whatever the option — padding 16 — so the option
        // can change at run time without re-stitching (MC reloads instead).
        static constexpr int kMaxMipLevels = 4;
        // The largest atlas side the stitcher may grow to.
        static constexpr int kMaxAtlasSize = 16384;

        explicit AtlasBuilder(AtlasId id = AtlasId::Blocks);
        ~AtlasBuilder();

        AtlasId GetId() const { return m_id; }
        const AtlasConfig& GetConfig() const { return GetAtlasConfig(m_id); }
        // The padding ring every sprite carries (MC 1 << stitched mip level).
        int GetPadding() const { return m_padding; }
        // The deepest mip level the stitched layout supports.
        int GetStitchedMipLevel() const { return m_stitchMipLevel; }

        // Main build process - parses JSON and creates atlas
        bool BuildFromJSON(const std::string& atlasJsonPath,
                          const std::string& texturesRootPath = "assets/textures");

        // Get backend texture handle (works with both GL and Vulkan)
        Render::TextureHandle GetBackendTextureHandle() const { return m_atlasTexture; }

        // The sprite table: an RGBA32F texture, 256 sprites per row, texel
        // (id & 255, id >> 8) = (u0, v0, width, height) of sprite `id` in
        // atlas UV. The terrain fragment shaders texelFetch it for tiled
        // (greedy-merged) quads, whose TerrainVertex carries the id. Rebuilt
        // with the atlas; INVALID_TEXTURE before the first build.
        Render::TextureHandle GetSpriteTableHandle() const { return m_spriteTable; }

        // Get the native texture ID (for ImGui display / legacy code)
        uintptr_t GetAtlasTextureID() const;

        // Get grass/foliage colormap texture handles
        Render::TextureHandle GetGrassColormapHandle() const { return m_grassColormap; }
        Render::TextureHandle GetFoliageColormapHandle() const { return m_foliageColormap; }

        // Look up UV coordinates for a texture key
        bool GetUVRect(const std::string& textureKey, AtlasUVRect& uvRect) const;
        // MC SpriteContents.computeTransparency over the texels a quad's UV
        // rectangle covers (u0..v1 as fractions of the sprite, any order),
        // OR-ed over every frame of an animated sprite: bit 0 = some texel
        // has alpha 0 (transparent), bit 1 = some texel has 0 < alpha < 255
        // (translucent); 0 = fully opaque. 0xFF when the key is unknown.
        // Read by mesh workers like GetUVRect: built with the atlas, stable
        // until the next rebuild (which remeshes everything).
        static constexpr uint8_t kTexelTransparent = 1, kTexelTranslucent = 2;
        uint8_t QuadTransparency(const std::string& textureKey,
                                 float u0, float v0, float u1, float v1) const;
        // Every packed sprite, (key, rect), in no particular order.
        template <class F> void ForEachUVRect(F&& fn) const {
            for (const auto& [key, rect] : textureKeyToUV) fn(key, rect);
        }

        // Get atlas dimensions
        int GetAtlasWidth() const { return atlasWidth; }
        int GetAtlasHeight() const { return atlasHeight; }

        // Get statistics
        size_t GetTextureCount() const { return textureSources.size(); }
        size_t GetPackedCount() const { return textureKeyToUV.size(); }

        // **NEW**: Mipmap control
        void SetMipmapEnabled(bool enabled);
        bool IsMipmapEnabled() const { return mipmapEnabled; }
        void SetMipmapLevel(int level); // Control mipmap level (0-4)
        int GetMipmapLevel() const { return m_mipmapLevel; }
        // The Video Settings "Mipmap Levels" option (MC mipmapLevels 0..4):
        // 0 turns mipmapping OFF (nearest sampling, no chain), 1..4 is the
        // chain depth. One rebuild whichever fields change, unlike calling
        // SetMipmapEnabled + SetMipmapLevel in sequence. Safe before the
        // atlas exists — the values are picked up by BuildFromJSON.
        void SetMipmapLevels(int levels);
        // Re-read the sampling settings (mip filter, anisotropy) onto the
        // atlas — the Video Settings screen calls it on a change.
        void RefreshTextureParameters() { UpdateTextureParameters(); }
        
        // Border extrusion control (for toggling between rendering modes)
        void SetBorderExtrusionEnabled(bool enabled) { m_borderExtrusionEnabled = enabled; }
        bool IsBorderExtrusionEnabled() const { return m_borderExtrusionEnabled; }
        
        // Rebuild atlas with different rendering mode
        void RebuildAtlas(bool useMinecraftStyle);
        // Free the atlas, sprite table and colormap textures ahead of a
        // BuildFromJSON that replaces them (resource pack reload).
        void ReleaseGpuResources();

        // Debug: Save atlas to file
        bool SaveAtlasDebugImage(const std::string& outputPath) const;

        // This atlas's animated sprites (MC TextureAtlas.cycleAnimationFrames).
        TextureAnimator* GetTextureAnimator() const { return m_animator.get(); }
        // Once per frame: advances and draws this atlas's animated sprites.
        void UpdateAnimations(float deltaTime);

    private:
        // Backend texture handles
        Render::TextureHandle m_atlasTexture = Render::INVALID_TEXTURE;
        void DestroyAtlasTextures();
        Render::TextureHandle m_spriteTable  = Render::INVALID_TEXTURE;
        // Number sprites and (re)create m_spriteTable from textureKeyToUV.
        void BuildSpriteTable();
        Render::TextureHandle m_grassColormap = Render::INVALID_TEXTURE;
        Render::TextureHandle m_foliageColormap = Render::INVALID_TEXTURE;

        // Atlas dimensions
        int atlasWidth;
        int atlasHeight;

        // **NEW**: Mipmap state
        bool mipmapEnabled;
        int m_mipmapLevel = 4; // Default to max mipmap level
        bool m_borderExtrusionEnabled = true; // Enable border extrusion for mipmap padding

        AtlasId m_id = AtlasId::Blocks;
        int m_padding = 0;          // Stitcher padding (1 << m_stitchMipLevel)
        int m_stitchMipLevel = 0;   // MC SpriteLoader's mip level for this atlas

        // This atlas's animations; registered from pendingAnimations after
        // every (re)build of the atlas texture and of its mip chain.
        std::unique_ptr<TextureAnimator> m_animator;
        void RegisterAnimations();
        // The mip levels the atlas carries now: the option, capped by what
        // the stitched layout supports; 0 without mipmaps.
        int EffectiveMipLevels() const;
        
        // **NEW**: Animation data storage
        struct PendingAnimation {
            std::string textureKey;
            TextureAnimation animation;
            std::vector<std::vector<unsigned char>> frames;
            // Mirrors of the source's .mcmeta texture section, so each frame
            // upload rebuilds the same chain the still path would. kelp and
            // kelp_plant are animated AND carry alpha_cutoff_bias 0.1.
            std::string mipmapStrategy;
            float       alphaCutoffBias = 0.0f;
        };
        std::vector<PendingAnimation> pendingAnimations;

        // Texture sources and packed data
        std::vector<TextureSource> textureSources;
        std::unordered_map<std::string, AtlasUVRect> textureKeyToUV;

        // Atlas pixel data (for debug saving)
        std::vector<unsigned char> atlasData;
        std::vector<unsigned char> originalAtlasData; // Original data without border extrusion
        // Retained so the mip chain can be rebuilt when the debug UI toggles
        // mipmaps or changes the level — those paths recreate the texture and
        // would otherwise leave levels 1..N undefined.
        std::vector<PackRect> m_packedRects;
        // Per sprite: its frame size and one transparency code per texel
        // (the bits of QuadTransparency, OR-ed over animation frames).
        struct SpriteAlpha {
            int width = 0, height = 0;
            std::vector<uint8_t> bits;
        };
        std::unordered_map<std::string, SpriteAlpha> m_spriteAlpha;
        void BuildSpriteAlpha(const std::vector<TextureSource>& sources,
                              const std::vector<PackRect>& packedRects);

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
        // (relative path below dirPath, file to open), every enabled
        // resource pack overlaid on the vanilla directory (Core::Assets).
        std::vector<std::pair<std::string, std::string>> ScanDirectoryForPNGs(const std::string& dirPath);

        // Helper: Load a single PNG file
        bool LoadPNG(const std::string& filePath,
                    int& width, int& height,
                    std::vector<unsigned char>& data);

        // Helper: Create and upload a colormap texture
        Render::TextureHandle CreateColormapTexture(const std::vector<unsigned char>& data,
                                                    int width, int height);

        // Helper: Copy texture to atlas at specified position
        void CopyTextureToAtlas(const TextureSource& source,
                               int destX, int destY);
        
        // Builds the 16 edge-variant tiles each connected-texture block needs
        // and appends them to `sources` as ordinary entries. See
        // kConnectedTextureKeys and ConnectedTextureKey().
        void GenerateConnectedTextureVariants(std::vector<TextureSource>& sources);

        // Reads the `texture` section of a .mcmeta (mipmap_strategy /
        // alpha_cutoff_bias). Separate from ParseMcMetaFile because that one
        // only answers "is this animated", and the texture section exists on
        // plenty of still sprites.
        bool ParseTextureMeta(const std::string& mcmetaPath,
                              std::string& outStrategy, float& outBias) const;

        // Builds MC's per-sprite mip chain, stitches one atlas image per level
        // and uploads them. Replaces the driver's glGenerateMipmap for the
        // block atlas; see texture/MipmapGenerator.hpp for why.
        void BuildAndUploadMipChain(const std::vector<TextureSource>& sources,
                                    const std::vector<PackRect>& packedRects);

        // Helper: Extrude texture borders to prevent mipmap bleeding
        void ExtrudeTextureBorders(int textureX, int textureY,
                                  int textureWidth, int textureHeight);

        // Helper: Fill transparent pixel RGB with nearest opaque color (prevents dark mipmap fringes)
        void SolidifyTransparentPixels(int textureX, int textureY,
                                      int textureWidth, int textureHeight);

        // **NEW**: Animation helper methods
        bool ParseMcMetaFile(const std::string& mcmetaPath, TextureAnimation& animation);
        bool LoadAnimatedTexture(const std::string& texturePath, 
                                TextureSource& source,
                                TextureAnimation& animation,
                                std::vector<std::vector<unsigned char>>& frames);
    };

    // The atlases. g_atlasBuilder is the BLOCK atlas (terrain, block
    // models, block items); g_itemAtlasBuilder the item atlas.
    extern std::unique_ptr<AtlasBuilder> g_atlasBuilder;
    extern std::unique_ptr<AtlasBuilder> g_itemAtlasBuilder;
    AtlasBuilder* GetAtlas(AtlasId id);
    // The atlas's texture, INVALID_TEXTURE while it is not built.
    TextureHandle GetAtlasTexture(AtlasId id);

    // A model texture resolved to its atlas (MC Material.Baked: the sprite
    // knows its sheet, and the draw binds that sheet).
    struct AtlasSprite {
        AtlasId     atlas = AtlasId::Blocks;
        AtlasUVRect rect;
    };
    // MC MaterialBaker.bake: the item atlas first, then the block atlas. A
    // key names a sprite in exactly one atlas in practice ("item/..." vs
    // "block/..."), so the order only matters for a key both hold — and then
    // MC's order wins. Block models reach the item atlas only through a
    // particle texture (barrier, light_NN, structure_void).
    bool FindSprite(const std::string& textureKey, AtlasSprite& out);

} // namespace Render
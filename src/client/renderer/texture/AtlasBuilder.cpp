// File: src/client/renderer/texture/AtlasBuilder.cpp
#include "AtlasBuilder.hpp"
#include <unordered_map>
#include "TextureAnimator.hpp"
#include "common/core/AssetLocator.hpp"
#include "MipmapGenerator.hpp"
#include "ConnectedTextures.hpp"
#include "Stitcher.hpp"
#include "../backend/RenderBackend.hpp"
#include "common/core/Log.hpp"
#include "platform/GameDirectory.hpp"   // anisotropic filtering setting
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <iterator>
#include <regex>

// Include stb_image for PNG loading
#include "../../../ext/stb_image/stb_image.h"

// Include stb_image_write for debug output (optional)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../ext/stb_image/stb_image_write.h"

namespace Render {

    std::unique_ptr<AtlasBuilder> g_atlasBuilder = nullptr;
    std::unique_ptr<AtlasBuilder> g_itemAtlasBuilder = nullptr;

    const AtlasConfig& GetAtlasConfig(AtlasId id) {
        //                               name      definition                        mips   colormaps CTM    spriteTable
        static const AtlasConfig kBlocks{"blocks", "assets/atlases/blocks.json", true,  true,     true,  true};
        static const AtlasConfig kItems {"items",  "assets/atlases/items.json",  false, false,    false, false};
        return id == AtlasId::Items ? kItems : kBlocks;
    }

    AtlasBuilder* GetAtlas(AtlasId id) {
        return id == AtlasId::Items ? g_itemAtlasBuilder.get() : g_atlasBuilder.get();
    }

    TextureHandle GetAtlasTexture(AtlasId id) {
        const AtlasBuilder* atlas = GetAtlas(id);
        return atlas ? atlas->GetBackendTextureHandle() : INVALID_TEXTURE;
    }

    bool FindSprite(const std::string& textureKey, AtlasSprite& out) {
        for (AtlasId id : {AtlasId::Items, AtlasId::Blocks}) {
            const AtlasBuilder* atlas = GetAtlas(id);
            if (atlas && atlas->GetUVRect(textureKey, out.rect)) {
                out.atlas = id;
                return true;
            }
        }
        return false;
    }

    AtlasBuilder::AtlasBuilder(AtlasId id)
        : atlasWidth(0)
        , atlasHeight(0)
        , mipmapEnabled(GetAtlasConfig(id).createMipmaps)
        , m_id(id)
        , m_animator(std::make_unique<TextureAnimator>()) {
    }

    AtlasBuilder::~AtlasBuilder() {
        m_animator.reset();   // its GPU frames go before the backend-owned atlas
        if (Render::g_renderBackend) {
            DestroyAtlasTextures();
            if (m_spriteTable != Render::INVALID_TEXTURE)
                Render::g_renderBackend->DestroyTexture(m_spriteTable);
            if (m_grassColormap != Render::INVALID_TEXTURE)
                Render::g_renderBackend->DestroyTexture(m_grassColormap);
            if (m_foliageColormap != Render::INVALID_TEXTURE)
                Render::g_renderBackend->DestroyTexture(m_foliageColormap);
        }
    }

    uintptr_t AtlasBuilder::GetAtlasTextureID() const {
        if (m_atlasTexture != Render::INVALID_TEXTURE && Render::g_renderBackend) {
            return Render::g_renderBackend->GetNativeTextureID(m_atlasTexture);
        }
        return 0;
    }

    bool AtlasBuilder::BuildFromJSON(const std::string& atlasJsonPath,
                                    const std::string& texturesRootPath) {
        Log::Info("=== ATLAS BUILDER START ===");
        Log::Info("Building texture atlas from: %s", atlasJsonPath.c_str());
        Log::Info("Textures root: %s", texturesRootPath.c_str());

        // Step 1: Parse the JSON atlas descriptor. Everything a previous
        // build left behind goes first: this runs again on a resource pack
        // reload.
        textureSources.clear();
        textureKeyToUV.clear();
        pendingAnimations.clear();
        atlasData.clear();
        originalAtlasData.clear();
        if (!ParseAtlasJSON(atlasJsonPath, texturesRootPath, textureSources)) {
            Log::Error("Failed to parse atlas JSON");
            return false;
        }
        Log::Info("✓ Parsed atlas JSON - found %zu texture sources", textureSources.size());

        // Step 2: Load biome colormaps (the block atlas carries them)
        if (GetConfig().colormaps && !LoadColormaps(texturesRootPath)) {
            Log::Warning("Failed to load colormaps - continuing without biome tinting");
        }

        // Step 3: Load all texture PNGs
        if (!LoadAllTextures(textureSources)) {
            Log::Error("Failed to load texture PNGs");
            return false;
        }

        // Step 3b: Derive the connected-texture variants before packing, so
        // they are ordinary sources from here on (packed, mipmapped, animated
        // — all the same machinery).
        if (GetConfig().connectedTextures) GenerateConnectedTextureVariants(textureSources);

        // Step 4: Pack textures into atlas
        std::vector<PackRect> packedRects;
        if (!PackTextures(textureSources, packedRects, atlasWidth, atlasHeight)) {
            Log::Error("Failed to pack textures into atlas");
            return false;
        }
        Log::Info("✓ Packed %zu textures into %dx%d atlas",
                 packedRects.size(), atlasWidth, atlasHeight);

        // Step 5: Create atlas texture and upload to GPU
        if (!CreateAtlasTexture(textureSources, packedRects)) {
            Log::Error("Failed to create atlas texture");
            return false;
        }

        Log::Info("=== ATLAS BUILDER COMPLETE ===");
        Log::Info("Atlas texture handle: %u (%dx%d)", m_atlasTexture, atlasWidth, atlasHeight);
        Log::Info("Grass colormap ID: %u", m_grassColormap);
        Log::Info("Foliage colormap ID: %u", m_foliageColormap);
        Log::Info("Total textures: %zu", textureKeyToUV.size());

        return true;
    }

    bool AtlasBuilder::ParseAtlasJSON(const std::string& jsonPath,
                                     const std::string& texturesRoot,
                                     std::vector<TextureSource>& sources) {
        // Read JSON file
        std::ifstream file(jsonPath);
        if (!file.is_open()) {
            Log::Error("Cannot open atlas JSON: %s", jsonPath.c_str());
            return false;
        }

        nlohmann::json j;
        try {
            file >> j;
        } catch (const nlohmann::json::exception& e) {
            Log::Error("JSON parse error: %s", e.what());
            return false;
        }

        // Parse sources array
        if (!j.contains("sources") || !j["sources"].is_array()) {
            Log::Error("Atlas JSON missing 'sources' array");
            return false;
        }

        for (const auto& source : j["sources"]) {
            if (!source.contains("type")) {
                Log::Warning("Source missing 'type' field, skipping");
                continue;
            }

            std::string type = source["type"];

            auto colon = type.find(':');
            auto coreType = (colon==std::string::npos) ? type : type.substr(colon+1);

            if (coreType == "directory") {
                ProcessDirectorySource(source, texturesRoot, sources);
            } else if (coreType == "single") {
                ProcessSingleSource(source, texturesRoot, sources);
            } else if (coreType == "filter") {
                // MC SourceFilter: drop every sprite added so far whose id
                // the pattern matches. IdentifierPattern compiles each part
                // with Pattern.asPredicate — a regex FIND, not a full match;
                // an absent part matches anything.
                const nlohmann::json pattern = source.value("pattern", nlohmann::json::object());
                const std::string nsPattern = pattern.value("namespace", std::string());
                const std::string pathPattern = pattern.value("path", std::string());
                try {
                    const std::regex nsRe(nsPattern), pathRe(pathPattern);
                    const size_t before = sources.size();
                    sources.erase(std::remove_if(sources.begin(), sources.end(), [&](const TextureSource& t) {
                        const auto colon = t.key.find(':');
                        const std::string ns = colon == std::string::npos ? "minecraft" : t.key.substr(0, colon);
                        const std::string path = colon == std::string::npos ? t.key : t.key.substr(colon + 1);
                        return (nsPattern.empty() || std::regex_search(ns, nsRe)) &&
                               (pathPattern.empty() || std::regex_search(path, pathRe));
                    }), sources.end());
                    Log::Debug("Filter source removed %zu sprite(s)", before - sources.size());
                } catch (const std::regex_error& e) {
                    Log::Warning("Filter source: bad pattern (%s)", e.what());
                }
            } else {
                Log::Warning("Unknown source type: %s", coreType.c_str());
            }
        }

        return !sources.empty();
    }

    void AtlasBuilder::ProcessDirectorySource(const nlohmann::json& source,
                                             const std::string& texturesRoot,
                                             std::vector<TextureSource>& sources) {
        if (!source.contains("source") || !source.contains("prefix")) {
            Log::Warning("Directory source missing required fields");
            return;
        }

        std::string dirPath = source["source"];
        std::string prefix = source["prefix"];

        // Build full directory path
        std::string fullDirPath = texturesRoot + "/" + dirPath;

        Log::Debug("Processing directory: %s with prefix: %s",
                  fullDirPath.c_str(), prefix.c_str());

        // Scan directory for PNG files (vanilla + enabled resource packs)
        auto pngFiles = ScanDirectoryForPNGs(fullDirPath);

        for (const auto& [relative, pngFile] : pngFiles) {
            // The key comes from the path below the source directory; the
            // file may live in a pack, so the pair carries both.
            std::filesystem::path relativePath(relative);

            // Remove .png extension
            std::string textureName = relativePath.stem().string();

            // Handle subdirectories - convert path separators to forward slashes
            std::string fullTextureName = relativePath.parent_path().string();
            if (!fullTextureName.empty()) {
                std::replace(fullTextureName.begin(), fullTextureName.end(), '\\', '/');
                fullTextureName += "/";
            }
            fullTextureName += textureName;

            // Build texture key with prefix
            std::string textureKey = prefix + fullTextureName;

            // Create texture source
            TextureSource texSource;
            texSource.key = textureKey;
            texSource.path = pngFile;

            sources.push_back(texSource);

            //Log::Debug("  Added texture: %s -> %s", textureKey.c_str(), pngFile.c_str());
        }
    }

    void AtlasBuilder::ProcessSingleSource(const nlohmann::json& source,
                                          const std::string& texturesRoot,
                                          std::vector<TextureSource>& sources) {
        // MC SingleFile: `resource` names the texture file
        // (textures/<resource>.png); `sprite`, optional, the sprite id it is
        // registered under — the resource's own id when absent.
        if (!source.contains("resource") || !source["resource"].is_string()) {
            Log::Warning("Single source missing 'resource'");
            return;
        }
        auto stripNamespace = [](std::string id) {
            if (id.rfind("minecraft:", 0) == 0) id = id.substr(10);
            return id;
        };
        const std::string resource = stripNamespace(source["resource"].get<std::string>());
        const std::string sprite = source.contains("sprite") && source["sprite"].is_string()
                                       ? stripNamespace(source["sprite"].get<std::string>())
                                       : resource;

        // Build full path (a resource pack's copy when one is enabled)
        TextureSource texSource;
        texSource.key = sprite;   // bare, like the directory sources' "block/stone"
        texSource.path = Core::Assets::Locate(texturesRoot + "/" + resource + ".png");
        sources.push_back(texSource);

        Log::Debug("Added single texture: %s -> %s", texSource.key.c_str(), texSource.path.c_str());
    }

    std::vector<std::pair<std::string, std::string>> AtlasBuilder::ScanDirectoryForPNGs(const std::string& dirPath) {
        std::vector<std::pair<std::string, std::string>> pngFiles;
        if (!std::filesystem::exists(dirPath)) {
            Log::Warning("Directory does not exist: %s", dirPath.c_str());
        }
        // Sorted by relative path already — deterministic atlas packing
        // across runs, whatever pack a file comes from.
        for (const Core::Assets::Entry& e : Core::Assets::ListFiles(dirPath, ".png", true)) {
            pngFiles.emplace_back(e.relative, e.absolute);
        }
        return pngFiles;
    }

    bool AtlasBuilder::LoadColormaps(const std::string& texturesRoot) {
        Log::Info("Loading biome colormaps...");

        // Load grass colormap
        std::string grassPath = Core::Assets::Locate(texturesRoot + "/colormap/grass.png");
        int grassWidth, grassHeight;
        std::vector<unsigned char> grassData;

        if (LoadPNG(grassPath, grassWidth, grassHeight, grassData)) {
            if (grassWidth == 256 && grassHeight == 256) {
                m_grassColormap = CreateColormapTexture(grassData, grassWidth, grassHeight);
                Log::Info("✓ Loaded grass colormap: %dx%d", grassWidth, grassHeight);
            } else {
                Log::Warning("Grass colormap is %dx%d, expected 256x256",
                           grassWidth, grassHeight);
            }
        } else {
            Log::Warning("Failed to load grass colormap: %s", grassPath.c_str());
        }

        // Load foliage colormap
        std::string foliagePath = Core::Assets::Locate(texturesRoot + "/colormap/foliage.png");
        int foliageWidth, foliageHeight;
        std::vector<unsigned char> foliageData;

        if (LoadPNG(foliagePath, foliageWidth, foliageHeight, foliageData)) {
            if (foliageWidth == 256 && foliageHeight == 256) {
                m_foliageColormap = CreateColormapTexture(foliageData,
                                                         foliageWidth, foliageHeight);
                Log::Info("✓ Loaded foliage colormap: %dx%d", foliageWidth, foliageHeight);
            } else {
                Log::Warning("Foliage colormap is %dx%d, expected 256x256",
                           foliageWidth, foliageHeight);
            }
        } else {
            Log::Warning("Failed to load foliage colormap: %s", foliagePath.c_str());
        }

        return m_grassColormap != 0 || m_foliageColormap != 0;
    }

    bool AtlasBuilder::LoadAllTextures(std::vector<TextureSource>& sources) {
        Log::Info("Loading %zu texture files...", sources.size());

        size_t loadedCount = 0;
        size_t failedCount = 0;

        for (auto& source : sources) {
            // Check for .mcmeta file first
            std::string mcmetaPath = source.path + ".mcmeta";
            TextureAnimation animation;
            std::vector<std::vector<unsigned char>> animationFrames;
            
            // The `texture` section is independent of `animation` — a still
            // sprite can carry a mipmap strategy, and every leaf does.
            ParseTextureMeta(mcmetaPath, source.mipmapStrategy, source.alphaCutoffBias);

            bool hasAnimation = ParseMcMetaFile(mcmetaPath, animation);
            bool isAnimatedTexture = false;
            
            if (hasAnimation) {
                // Try to load as animated texture
                if (LoadAnimatedTexture(source.path, source, animation, animationFrames)) {
                    isAnimatedTexture = true;
                    loadedCount++;
                    
                    // Store animation data for later registration
                    PendingAnimation pending;
                    pending.textureKey = source.key;
                    pending.animation = animation;
                    pending.frames = animationFrames;
                    pending.mipmapStrategy = source.mipmapStrategy;
                    pending.alphaCutoffBias = source.alphaCutoffBias;
                    pendingAnimations.push_back(pending);
                    
                    //Log::Debug("Found animated texture: %s", source.key.c_str());
                }
            }
            
            // If not animated or animation loading failed, load as regular texture
            if (!isAnimatedTexture) {
                if (LoadPNG(source.path, source.width, source.height, source.data)) {
                    loadedCount++;
                } else {
                    Log::Warning("Failed to load texture: %s", source.path.c_str());
                    failedCount++;

                    // Create a magenta error texture
                    source.width = 16;
                    source.height = 16;
                    source.data.resize(16 * 16 * 4);
                    for (int i = 0; i < 16 * 16; ++i) {
                        source.data[i * 4 + 0] = 255; // R
                        source.data[i * 4 + 1] = 0;   // G
                        source.data[i * 4 + 2] = 255; // B
                        source.data[i * 4 + 3] = 255; // A
                    }
                }
            }

            // Log every 100th texture to avoid spam
            if (loadedCount % 100 == 0) {
                //Log::Debug("Loaded %zu textures...", loadedCount);
            }
        }

        Log::Info("✓ Loaded %zu/%zu textures (%zu failed)",
                 loadedCount, sources.size(), failedCount);

        return loadedCount > 0;
    }

    bool AtlasBuilder::LoadPNG(const std::string& filePath,
                               int& width, int& height,
                               std::vector<unsigned char>& data) {
        // Check if file exists
        if (!std::filesystem::exists(filePath)) {
            return false;
        }

        // Load with stb_image
        int channels;
        stbi_set_flip_vertically_on_load(0); // Don't flip
        unsigned char* pixels = stbi_load(filePath.c_str(), &width, &height,
                                         &channels, STBI_rgb_alpha);

        if (!pixels) {
            return false;
        }

        // Copy to vector
        size_t dataSize = width * height * 4;
        data.resize(dataSize);
        std::memcpy(data.data(), pixels, dataSize);

        stbi_image_free(pixels);
        return true;
    }

    bool AtlasBuilder::PackTextures(const std::vector<TextureSource>& sources,
                                   std::vector<PackRect>& packedRects,
                                   int& outWidth, int& outHeight) {
        // MC SpriteLoader.stitch + Stitcher, exactly (texture/Stitcher.hpp):
        // the mip level every sprite can halve to, padding 1 << that level,
        // slots rounded to that grid so every level's `>> k` is exact.
        std::vector<Stitcher::Entry> entries;
        entries.reserve(sources.size());
        for (const auto& source : sources) {
            entries.push_back({source.key, std::max(1, source.width), std::max(1, source.height)});
        }
        m_stitchMipLevel = Stitcher::ChooseMipLevel(
            entries, GetConfig().createMipmaps ? kMaxMipLevels : 0, GetConfig().name);
        const Stitcher::Result result = Stitcher::Stitch(entries, m_stitchMipLevel, kMaxAtlasSize);
        if (!result.ok) {
            Log::Error("Stitcher: %s atlas cannot fit '%s' within %dx%d",
                       GetConfig().name, result.failedEntry.c_str(), kMaxAtlasSize, kMaxAtlasSize);
            return false;
        }
        m_padding = result.padding;

        // A placement is the slot's corner; the sprite sits `padding` in.
        packedRects.clear();
        packedRects.reserve(result.placements.size());
        for (const Stitcher::Placement& place : result.placements) {
            const auto& source = sources[place.entry];
            PackRect rect;
            rect.x = place.x + m_padding;
            rect.y = place.y + m_padding;
            rect.width = source.width;
            rect.height = source.height;
            rect.textureIndex = static_cast<int>(place.entry);
            packedRects.push_back(rect);
        }
        outWidth = std::max(1, result.width);
        outHeight = std::max(1, result.height);
        Log::Info("Stitched %zu sprites into the %dx%d %s atlas (mip level %d, padding %d)",
                  packedRects.size(), outWidth, outHeight, GetConfig().name, m_stitchMipLevel, m_padding);
        return true;
    }

    bool AtlasBuilder::CreateAtlasTexture(const std::vector<TextureSource>& sources,
                                     const std::vector<PackRect>& packedRects) {
        Log::Info("Creating atlas texture...");

        // Allocate atlas pixel data
        atlasData.resize(atlasWidth * atlasHeight * 4, 0);

        // Clear to transparent
        std::fill(atlasData.begin(), atlasData.end(), 0);

        // Copy all textures to their packed positions
        for (const auto& rect : packedRects) {
            if (rect.textureIndex < 0 || rect.textureIndex >= sources.size()) {
                continue;
            }

            const auto& source = sources[rect.textureIndex];
            CopyTextureToAtlas(source, rect.x, rect.y);
            
            // Record UV coordinates (padding prevents bleeding)
            AtlasUVRect uvRect;
            uvRect.uvMin.x = static_cast<float>(rect.x) / atlasWidth;
            uvRect.uvMin.y = static_cast<float>(rect.y) / atlasHeight;
            uvRect.uvMax.x = static_cast<float>(rect.x + rect.width) / atlasWidth;
            uvRect.uvMax.y = static_cast<float>(rect.y + rect.height) / atlasHeight;

            textureKeyToUV[source.key] = uvRect;
        }
        if (GetConfig().spriteTable) BuildSpriteTable();
        BuildSpriteAlpha(sources, packedRects);
        
        // Save original atlas data before any modifications
        originalAtlasData = atlasData;

        // Pre-fill transparent pixels with nearest opaque color (prevents dark mipmap fringes)
        // This must happen before border extrusion so the extrusion also picks up solidified colors
        if (m_borderExtrusionEnabled) {
            for (const auto& rect : packedRects) {
                if (rect.textureIndex < 0 || rect.textureIndex >= (int)sources.size()) {
                    continue;
                }
                // Note: SolidifyTransparentPixels skipped here — it incorrectly fills
                // grass block side overlay transparency with green. The 16-pixel edge
                // extrusion is sufficient to prevent mipmap bleeding artifacts.
                // SolidifyTransparentPixels(rect.x, rect.y, rect.width, rect.height);
            }
        }

        // Apply border extrusion (clamp-to-edge padding) for mipmap safety
        if (m_borderExtrusionEnabled) {
            for (const auto& rect : packedRects) {
                if (rect.textureIndex < 0 || rect.textureIndex >= (int)sources.size()) {
                    continue;
                }
                ExtrudeTextureBorders(rect.x, rect.y, rect.width, rect.height);
            }
        }

        // Upload atlas to GPU through the backend
        if (!Render::g_renderBackend) {
            Log::Error("No render backend available for atlas texture upload");
            return false;
        }

        DestroyAtlasTextures();
        m_atlasTexture = Render::g_renderBackend->CreateTexture2D(
            atlasWidth, atlasHeight, Render::TextureFormat::RGBA8, atlasData.data());
        if (m_atlasTexture == Render::INVALID_TEXTURE) {
            Log::Error("Failed to create atlas texture via backend");
            return false;
        }

        UpdateTextureParameters();
        // MC's per-sprite mip chain, replacing the driver's whole-atlas box
        // filter. Must run after the texture exists and after the filter is
        // set, and it rewrites level 0 as well as adding the levels above.
        BuildAndUploadMipChain(sources, packedRects);
        m_packedRects = packedRects;   // kept for later rebuilds
        Log::Info("Created atlas texture (%dx%d, mipmaps: %s)",
                 atlasWidth, atlasHeight, mipmapEnabled ? "enabled" : "disabled");

        RegisterAnimations();

        // Atlas is now on GPU — free the CPU copy to save 64-256MB of RAM.
        // RebuildAtlas() regenerates from source textures if ever needed.
        size_t freedBytes = atlasData.size();
        atlasData.clear();
        atlasData.shrink_to_fit();
        Log::Info("Atlas CPU data freed (%.1f MB, GPU copy retained)",
                  static_cast<double>(freedBytes) / (1024.0 * 1024.0));

        return true;
    }

    void AtlasBuilder::SetMipmapEnabled(bool enabled) {
        enabled = enabled && GetConfig().createMipmaps;   // MC: only the block atlas is mipmapped
        if (mipmapEnabled == enabled) return;
        mipmapEnabled = enabled;
        if (m_atlasTexture != Render::INVALID_TEXTURE) {
            UpdateTextureParameters();
            BuildAndUploadMipChain(textureSources, m_packedRects);
            RegisterAnimations();   // the frames carry the new chain depth
            Log::Info("AtlasBuilder mipmaps %s", enabled ? "enabled" : "disabled");
        }
    }

    void AtlasBuilder::SetMipmapLevel(int level) {
        m_mipmapLevel = std::max(0, std::min(kMaxMipLevels, level));
        if (m_atlasTexture != Render::INVALID_TEXTURE && GetConfig().createMipmaps) {
            UpdateTextureParameters();
            // The chain is CPU-authored, so a new level count means rebuilding
            // it — the driver is not going to fill the extra levels for us.
            BuildAndUploadMipChain(textureSources, m_packedRects);
            RegisterAnimations();
            Log::Info("Set mipmap level to %d", m_mipmapLevel);
        }
    }

    void AtlasBuilder::SetMipmapLevels(int levels) {
        if (!GetConfig().createMipmaps) return;   // items / portal gun: never mipmapped (MC)
        levels = std::max(0, std::min(kMaxMipLevels, levels));
        const bool enabled = levels > 0;
        const int  level   = enabled ? levels : m_mipmapLevel;
        if (enabled == mipmapEnabled && level == m_mipmapLevel) return;
        mipmapEnabled = enabled;
        m_mipmapLevel = level;
        if (m_atlasTexture != Render::INVALID_TEXTURE) {
            UpdateTextureParameters();
            // The chain is CPU-authored (see SetMipmapLevel), so a deeper
            // chain has to be rebuilt; switching OFF only needs the sampler
            // change above, the unused levels can stay resident.
            if (enabled) BuildAndUploadMipChain(textureSources, m_packedRects);
            RegisterAnimations();
            Log::Info("AtlasBuilder mipmap levels -> %d (%s)", levels, enabled ? "on" : "off");
        }
    }

    int AtlasBuilder::EffectiveMipLevels() const {
        return mipmapEnabled ? std::min(m_mipmapLevel, m_stitchMipLevel) : 0;
    }

    void AtlasBuilder::UpdateTextureParameters() {
        if (m_atlasTexture == Render::INVALID_TEXTURE || !Render::g_renderBackend) return;

        if (mipmapEnabled) {
            Render::g_renderBackend->SetTextureFilter(m_atlasTexture,
                Render::TextureFilter::NearestMipmapLinear, Render::TextureFilter::Nearest);
            // Deliberately NOT GenerateMipmaps: the driver box-filters raw RGBA
            // across the whole atlas, which is exactly the behaviour
            // BuildAndUploadMipChain replaces. Calling it here would overwrite
            // the CPU-built chain with the wrong one.
        } else {
            Render::g_renderBackend->SetTextureFilter(m_atlasTexture,
                Render::TextureFilter::Nearest, Render::TextureFilter::Nearest);
        }

        Render::g_renderBackend->SetTextureWrap(m_atlasTexture,
            Render::TextureWrap::ClampToEdge, Render::TextureWrap::ClampToEdge);
        // Anisotropy applies with mipmaps (it selects across them); without
        // a chain there is nothing for it to smooth.
        Render::g_renderBackend->SetTextureAnisotropy(m_atlasTexture,
            mipmapEnabled ? static_cast<float>(Platform::g_gameSettings.GetAnisotropicFiltering()) : 1.0f);
    }

    void AtlasBuilder::DestroyAtlasTextures() {
        if (Render::g_renderBackend && m_atlasTexture != Render::INVALID_TEXTURE) {
            Render::g_renderBackend->DestroyTexture(m_atlasTexture);
        }
        m_atlasTexture = Render::INVALID_TEXTURE;
    }

    void AtlasBuilder::ReleaseGpuResources() {
        if (!Render::g_renderBackend) return;
        DestroyAtlasTextures();
        for (Render::TextureHandle* t : {&m_spriteTable, &m_grassColormap, &m_foliageColormap}) {
            if (*t != Render::INVALID_TEXTURE) { Render::g_renderBackend->DestroyTexture(*t); *t = Render::INVALID_TEXTURE; }
        }
    }

    void AtlasBuilder::RebuildAtlas(bool useMinecraftStyle) {
        if (m_atlasTexture == Render::INVALID_TEXTURE || !Render::g_renderBackend) {
            Log::Warning("Cannot rebuild atlas: no texture created yet");
            return;
        }

        if (originalAtlasData.empty()) {
            Log::Warning("Cannot rebuild atlas: no original data saved");
            return;
        }

        // Destroy existing texture
        DestroyAtlasTextures();

        // If Minecraft style, apply solidify + border extrusion to a copy of the data
        const unsigned char* uploadData = originalAtlasData.data();
        if (useMinecraftStyle) {
            atlasData = originalAtlasData;
            // Note: SolidifyTransparentPixels skipped — causes grass side overlay
            // to render all-green. Edge extrusion alone is sufficient for mipmap safety.

            // Extrude borders into padding region
            for (const auto& kvp : textureKeyToUV) {
                const AtlasUVRect& uvRect = kvp.second;
                int x = static_cast<int>(uvRect.uvMin.x * atlasWidth);
                int y = static_cast<int>(uvRect.uvMin.y * atlasHeight);
                int width = static_cast<int>((uvRect.uvMax.x - uvRect.uvMin.x) * atlasWidth);
                int height = static_cast<int>((uvRect.uvMax.y - uvRect.uvMin.y) * atlasHeight);
                ExtrudeTextureBorders(x, y, width, height);
            }
            uploadData = atlasData.data();
        }

        // Create new texture via backend
        // Use RGBA8 — all rendering is done in gamma space like Minecraft.
        // No sRGB decode on sample; shade values are direct gamma-space multipliers.
        Render::TextureFormat format = Render::TextureFormat::RGBA8;
        m_atlasTexture = Render::g_renderBackend->CreateTexture2D(
            atlasWidth, atlasHeight, format, uploadData);

        // Set filtering based on mode
        mipmapEnabled = useMinecraftStyle;
        m_borderExtrusionEnabled = useMinecraftStyle;
        UpdateTextureParameters();
        // Fresh texture object, so its levels above 0 start undefined — the
        // CPU chain has to be re-uploaded onto it.
        BuildAndUploadMipChain(textureSources, m_packedRects);

        // Update TextureAnimator with the new atlas handle
        RegisterAnimations();

        Log::Info("Atlas rebuilt with %s rendering mode",
                  useMinecraftStyle ? "Minecraft-style" : "Classic");
    }

    void AtlasBuilder::BuildAndUploadMipChain(const std::vector<TextureSource>& sources,
                                              const std::vector<PackRect>& packedRects) {
        if (m_atlasTexture == Render::INVALID_TEXTURE || !Render::g_renderBackend) return;
        if (EffectiveMipLevels() <= 0) return;
        if (packedRects.empty() || sources.empty()) return;

        // Level 0 is written back in here, so the CPU buffer has to exist. It
        // is released after the initial build to save the RAM, which means a
        // later rebuild (debug UI mipmap toggle) arrives with it empty.
        const size_t expected = static_cast<size_t>(atlasWidth) *
                                static_cast<size_t>(atlasHeight) * 4u;
        if (atlasData.size() != expected) {
            if (originalAtlasData.size() == expected) {
                atlasData = originalAtlasData;
            } else {
                Log::Warning("Mip chain skipped: no CPU atlas copy to rebuild from");
                return;
            }
        }

        // Every sprite in this atlas is a multiple of 16 in both axes and the
        // packer splits on exact rect boundaries from (0,0), so positions stay
        // 16-aligned and `>> level` is exact for all 4 levels. A sprite that
        // ever breaks that would land on a half-texel, so it is checked rather
        // than assumed.
        const int levels = EffectiveMipLevels();
        const int align = 1 << levels;

        // One image buffer per level above 0. Level k is the atlas at half
        // dimensions k times over.
        std::vector<std::vector<unsigned char>> levelData(static_cast<size_t>(levels) + 1);
        std::vector<int> levelW(static_cast<size_t>(levels) + 1);
        std::vector<int> levelH(static_cast<size_t>(levels) + 1);
        for (int k = 1; k <= levels; ++k) {
            levelW[static_cast<size_t>(k)] = atlasWidth >> k;
            levelH[static_cast<size_t>(k)] = atlasHeight >> k;
            levelData[static_cast<size_t>(k)].assign(
                static_cast<size_t>(levelW[static_cast<size_t>(k)]) *
                static_cast<size_t>(levelH[static_cast<size_t>(k)]) * 4u, 0);
        }

        // Blit one sprite mip into a level buffer.
        auto blit = [](std::vector<unsigned char>& dst, int dstW,
                       const Mipmap::Image& src, int dstX, int dstY) {
            for (int y = 0; y < src.height; ++y) {
                const size_t srcRow = static_cast<size_t>(y) * src.width * 4u;
                const size_t dstRow = (static_cast<size_t>(dstY + y) * dstW + dstX) * 4u;
                std::memcpy(dst.data() + dstRow, src.pixels.data() + srcRow,
                            static_cast<size_t>(src.width) * 4u);
            }
        };

        // Clamp-to-edge extrusion into the padding ring, per level. With
        // NEAREST_MIPMAP_LINEAR nothing interpolates inside a level, so this is
        // belt-and-braces — but it keeps the padding meaningful if the filter
        // is ever widened to linear or anisotropic.
        auto extrude = [](std::vector<unsigned char>& buf, int bufW, int bufH,
                          int x0, int y0, int w, int h, int pad) {
            if (w <= 0 || h <= 0) return;
            auto px = [&](int x, int y) -> unsigned char* {
                return buf.data() + (static_cast<size_t>(y) * bufW + x) * 4u;
            };
            for (int p = 1; p <= pad; ++p) {
                const int up = y0 - p, dn = y0 + h - 1 + p;
                for (int x = x0; x < x0 + w; ++x) {
                    if (up >= 0)   std::memcpy(px(x, up), px(x, y0), 4);
                    if (dn < bufH) std::memcpy(px(x, dn), px(x, y0 + h - 1), 4);
                }
            }
            for (int p = 1; p <= pad; ++p) {
                const int lf = x0 - p, rt = x0 + w - 1 + p;
                const int yStart = std::max(0, y0 - pad);
                const int yEnd   = std::min(bufH, y0 + h + pad);
                for (int y = yStart; y < yEnd; ++y) {
                    if (lf >= 0)   std::memcpy(px(lf, y), px(x0, y), 4);
                    if (rt < bufW) std::memcpy(px(rt, y), px(x0 + w - 1, y), 4);
                }
            }
        };

        size_t misaligned = 0;
        for (const auto& rect : packedRects) {
            if (rect.textureIndex < 0 ||
                rect.textureIndex >= static_cast<int>(sources.size())) continue;
            const auto& source = sources[static_cast<size_t>(rect.textureIndex)];
            if (source.data.empty() || source.width <= 0 || source.height <= 0) continue;

            if ((rect.x % align) || (rect.y % align) ||
                (source.width % align) || (source.height % align)) {
                ++misaligned;
                continue;
            }

            Mipmap::Image lvl0;
            lvl0.width  = source.width;
            lvl0.height = source.height;
            lvl0.pixels = source.data;

            // MC keys the item exemption off the sprite path, not the atlas.
            const bool isItem = source.key.rfind("item/", 0) == 0 ||
                                source.key.find(":item/") != std::string::npos;

            std::vector<Mipmap::Image> chain = Mipmap::GenerateMipLevels(
                std::move(lvl0), levels,
                Mipmap::ParseStrategy(source.mipmapStrategy),
                source.alphaCutoffBias, isItem);

            // Level 0 goes back into the atlas too: the cutout strategies
            // rewrite the colour under alpha=0, and that rewrite is the whole
            // point. Alpha is untouched, so what level 0 draws is unchanged.
            if (!chain.empty()) {
                blit(atlasData, atlasWidth, chain[0], rect.x, rect.y);
                extrude(atlasData, atlasWidth, atlasHeight,
                        rect.x, rect.y, chain[0].width, chain[0].height, m_padding);
            }
            for (size_t k = 1; k < chain.size(); ++k) {
                const int kk = static_cast<int>(k);
                blit(levelData[k], levelW[k], chain[k], rect.x >> kk, rect.y >> kk);
                extrude(levelData[k], levelW[k], levelH[k],
                        rect.x >> kk, rect.y >> kk,
                        chain[k].width, chain[k].height, m_padding >> kk);
            }
        }

        if (misaligned > 0) {
            Log::Warning("Mipmap chain skipped for %zu sprite(s) not aligned to %d px - "
                         "they will sample level 0 only", misaligned, align);
        }

        // Reserve first: Vulkan fixes an image's mip count at allocation and
        // reallocates here, discarding contents. Every level is uploaded below,
        // level 0 included, so nothing is lost.
        Render::g_renderBackend->ReserveTextureMipLevels(m_atlasTexture, levels);

        // Level 0 goes up again because the cutout strategies rewrote it.
        Render::g_renderBackend->UploadTextureMipLevel(
            m_atlasTexture, 0, atlasWidth, atlasHeight, atlasData.data());
        for (int k = 1; k <= levels; ++k) {
            Render::g_renderBackend->UploadTextureMipLevel(
                m_atlasTexture, k, levelW[static_cast<size_t>(k)],
                levelH[static_cast<size_t>(k)], levelData[static_cast<size_t>(k)].data());
        }

        // Keep the retained CPU copy in step with what the GPU now holds, so
        // RebuildAtlas and the debug dump show the real level 0.
        originalAtlasData = atlasData;

        Log::Info("Built MC-style mip chain: %d levels, %zu sprites", levels, packedRects.size());
    }

    void AtlasBuilder::CopyTextureToAtlas(const TextureSource& source,
                                         int destX, int destY) {
        for (int y = 0; y < source.height; ++y) {
            for (int x = 0; x < source.width; ++x) {
                int srcIdx = (y * source.width + x) * 4;
                int dstIdx = ((destY + y) * atlasWidth + (destX + x)) * 4;

                // Copy RGBA
                atlasData[dstIdx + 0] = source.data[srcIdx + 0];
                atlasData[dstIdx + 1] = source.data[srcIdx + 1];
                atlasData[dstIdx + 2] = source.data[srcIdx + 2];
                atlasData[dstIdx + 3] = source.data[srcIdx + 3];
            }
        }
    }
    
    void AtlasBuilder::ExtrudeTextureBorders(int textureX, int textureY,
                                            int textureWidth, int textureHeight) {
        // Extrude edges by m_padding pixels to prevent mipmap bleeding.
        // Each edge pixel is repeated outward into the padding region (clamp-to-edge pattern).

        // Top edge - repeat top row upward
        for (int p = 1; p <= m_padding; ++p) {
            int dstY = textureY - p;
            if (dstY < 0) break;
            for (int x = 0; x < textureWidth; ++x) {
                int srcIdx = (textureY * atlasWidth + (textureX + x)) * 4;
                int dstIdx = (dstY * atlasWidth + (textureX + x)) * 4;
                for (int c = 0; c < 4; ++c)
                    atlasData[dstIdx + c] = atlasData[srcIdx + c];
            }
        }

        // Bottom edge - repeat bottom row downward
        for (int p = 0; p < m_padding; ++p) {
            int dstY = textureY + textureHeight + p;
            if (dstY >= atlasHeight) break;
            for (int x = 0; x < textureWidth; ++x) {
                int srcIdx = ((textureY + textureHeight - 1) * atlasWidth + (textureX + x)) * 4;
                int dstIdx = (dstY * atlasWidth + (textureX + x)) * 4;
                for (int c = 0; c < 4; ++c)
                    atlasData[dstIdx + c] = atlasData[srcIdx + c];
            }
        }

        // Left edge - repeat left column to the left
        for (int p = 1; p <= m_padding; ++p) {
            int dstX = textureX - p;
            if (dstX < 0) break;
            for (int y = 0; y < textureHeight; ++y) {
                int srcIdx = ((textureY + y) * atlasWidth + textureX) * 4;
                int dstIdx = ((textureY + y) * atlasWidth + dstX) * 4;
                for (int c = 0; c < 4; ++c)
                    atlasData[dstIdx + c] = atlasData[srcIdx + c];
            }
        }

        // Right edge - repeat right column to the right
        for (int p = 0; p < m_padding; ++p) {
            int dstX = textureX + textureWidth + p;
            if (dstX >= atlasWidth) break;
            for (int y = 0; y < textureHeight; ++y) {
                int srcIdx = ((textureY + y) * atlasWidth + (textureX + textureWidth - 1)) * 4;
                int dstIdx = ((textureY + y) * atlasWidth + dstX) * 4;
                for (int c = 0; c < 4; ++c)
                    atlasData[dstIdx + c] = atlasData[srcIdx + c];
            }
        }

        // Corner regions - fill the four rectangular corner padding areas
        // Each corner extends m_padding in both directions, filled with the nearest corner pixel

        // Top-left corner block
        for (int py = 1; py <= m_padding; ++py) {
            int dstY = textureY - py;
            if (dstY < 0) continue;
            for (int px = 1; px <= m_padding; ++px) {
                int dstX = textureX - px;
                if (dstX < 0) continue;
                int srcIdx = (textureY * atlasWidth + textureX) * 4;
                int dstIdx = (dstY * atlasWidth + dstX) * 4;
                for (int c = 0; c < 4; ++c)
                    atlasData[dstIdx + c] = atlasData[srcIdx + c];
            }
        }

        // Top-right corner block
        for (int py = 1; py <= m_padding; ++py) {
            int dstY = textureY - py;
            if (dstY < 0) continue;
            for (int px = 0; px < m_padding; ++px) {
                int dstX = textureX + textureWidth + px;
                if (dstX >= atlasWidth) continue;
                int srcIdx = (textureY * atlasWidth + (textureX + textureWidth - 1)) * 4;
                int dstIdx = (dstY * atlasWidth + dstX) * 4;
                for (int c = 0; c < 4; ++c)
                    atlasData[dstIdx + c] = atlasData[srcIdx + c];
            }
        }

        // Bottom-left corner block
        for (int py = 0; py < m_padding; ++py) {
            int dstY = textureY + textureHeight + py;
            if (dstY >= atlasHeight) continue;
            for (int px = 1; px <= m_padding; ++px) {
                int dstX = textureX - px;
                if (dstX < 0) continue;
                int srcIdx = ((textureY + textureHeight - 1) * atlasWidth + textureX) * 4;
                int dstIdx = (dstY * atlasWidth + dstX) * 4;
                for (int c = 0; c < 4; ++c)
                    atlasData[dstIdx + c] = atlasData[srcIdx + c];
            }
        }

        // Bottom-right corner block
        for (int py = 0; py < m_padding; ++py) {
            int dstY = textureY + textureHeight + py;
            if (dstY >= atlasHeight) continue;
            for (int px = 0; px < m_padding; ++px) {
                int dstX = textureX + textureWidth + px;
                if (dstX >= atlasWidth) continue;
                int srcIdx = ((textureY + textureHeight - 1) * atlasWidth + (textureX + textureWidth - 1)) * 4;
                int dstIdx = (dstY * atlasWidth + dstX) * 4;
                for (int c = 0; c < 4; ++c)
                    atlasData[dstIdx + c] = atlasData[srcIdx + c];
            }
        }
    }

    void AtlasBuilder::SolidifyTransparentPixels(int textureX, int textureY,
                                               int textureWidth, int textureHeight) {
        // "Solidify" pass: for pixels with alpha == 0, fill RGB with the nearest opaque pixel's color.
        // This prevents dark/black fringes when mipmaps blend transparent and opaque pixels together.
        // Uses an iterative flood-fill approach: each pass expands opaque colors one pixel outward
        // into adjacent transparent pixels.

        // Work on a local copy of just this sprite region to avoid cross-sprite contamination
        const int w = textureWidth;
        const int h = textureHeight;
        std::vector<unsigned char> region(w * h * 4);

        // Copy sprite region from atlas
        for (int y = 0; y < h; ++y) {
            int srcRow = ((textureY + y) * atlasWidth + textureX) * 4;
            int dstRow = (y * w) * 4;
            std::memcpy(&region[dstRow], &atlasData[srcRow], w * 4);
        }

        // Check if this sprite has any transparent pixels at all
        bool hasTransparent = false;
        for (int i = 0; i < w * h; ++i) {
            if (region[i * 4 + 3] == 0) {
                hasTransparent = true;
                break;
            }
        }
        if (!hasTransparent) return;

        // Track which pixels have been filled (start with opaque pixels marked as filled)
        std::vector<bool> filled(w * h, false);
        for (int i = 0; i < w * h; ++i) {
            if (region[i * 4 + 3] > 0) {
                filled[i] = true;
            }
        }

        // Iterative expansion: each pass fills transparent pixels adjacent to filled pixels
        // Do enough passes to cover the whole sprite (worst case is max(w, h) passes,
        // but typically only a few are needed)
        const int maxPasses = std::max(w, h);
        const int dx[] = {-1, 1, 0, 0, -1, -1, 1, 1};
        const int dy[] = {0, 0, -1, 1, -1, 1, -1, 1};

        for (int pass = 0; pass < maxPasses; ++pass) {
            bool anyChanged = false;
            std::vector<bool> newFilled = filled;

            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    int idx = y * w + x;
                    if (filled[idx]) continue; // Already has color

                    // Look at 8 neighbors for the nearest filled pixel
                    int totalR = 0, totalG = 0, totalB = 0, count = 0;
                    for (int d = 0; d < 8; ++d) {
                        int nx = x + dx[d];
                        int ny = y + dy[d];
                        if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
                        int nIdx = ny * w + nx;
                        if (filled[nIdx]) {
                            totalR += region[nIdx * 4 + 0];
                            totalG += region[nIdx * 4 + 1];
                            totalB += region[nIdx * 4 + 2];
                            count++;
                        }
                    }

                    if (count > 0) {
                        region[idx * 4 + 0] = static_cast<unsigned char>(totalR / count);
                        region[idx * 4 + 1] = static_cast<unsigned char>(totalG / count);
                        region[idx * 4 + 2] = static_cast<unsigned char>(totalB / count);
                        // Keep alpha = 0 so the pixel stays transparent for rendering
                        newFilled[idx] = true;
                        anyChanged = true;
                    }
                }
            }

            filled = newFilled;
            if (!anyChanged) break; // All transparent pixels have been filled
        }

        // Write solidified region back to atlas
        for (int y = 0; y < h; ++y) {
            int dstRow = ((textureY + y) * atlasWidth + textureX) * 4;
            int srcRow = (y * w) * 4;
            std::memcpy(&atlasData[dstRow], &region[srcRow], w * 4);
        }
    }

    Render::TextureHandle AtlasBuilder::CreateColormapTexture(const std::vector<unsigned char>& data,
                                                              int width, int height) {
        if (!Render::g_renderBackend) return Render::INVALID_TEXTURE;

        auto handle = Render::g_renderBackend->CreateTexture2D(width, height,
            Render::TextureFormat::RGBA8, data.data());
        if (handle != Render::INVALID_TEXTURE) {
            Render::g_renderBackend->SetTextureFilter(handle,
                Render::TextureFilter::Nearest, Render::TextureFilter::Nearest);
            Render::g_renderBackend->SetTextureWrap(handle,
                Render::TextureWrap::ClampToEdge, Render::TextureWrap::ClampToEdge);
        }
        return handle;
    }

    void AtlasBuilder::BuildSpriteTable() {
        // Deterministic ids: sorted key order, so a rebuild with the same
        // sprite set numbers them identically (meshes carry the ids).
        std::vector<std::string> keys;
        keys.reserve(textureKeyToUV.size());
        for (const auto& kv : textureKeyToUV) keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());
        if (keys.size() > 65535) {
            Log::Error("AtlasBuilder: %zu sprites exceed the 16-bit sprite id; tiled quads past 65535 will mis-sample",
                       keys.size());
        }

        constexpr int kRowWidth = 256;
        const int rows = std::max(1, static_cast<int>((keys.size() + kRowWidth - 1) / kRowWidth));
        std::vector<float> texels(static_cast<size_t>(kRowWidth) * rows * 4, 0.0f);
        uint16_t id = 0;
        for (const std::string& key : keys) {
            AtlasUVRect& r = textureKeyToUV[key];
            r.spriteId = id;
            float* t = &texels[static_cast<size_t>(id) * 4];
            t[0] = r.uvMin.x;
            t[1] = r.uvMin.y;
            t[2] = r.uvMax.x - r.uvMin.x;
            t[3] = r.uvMax.y - r.uvMin.y;
            if (id == 65535) break;
            ++id;
        }

        if (!Render::g_renderBackend) return;
        if (m_spriteTable != Render::INVALID_TEXTURE) {
            Render::g_renderBackend->DestroyTexture(m_spriteTable);
            m_spriteTable = Render::INVALID_TEXTURE;
        }
        m_spriteTable = Render::g_renderBackend->CreateTexture2D(
            kRowWidth, rows, Render::TextureFormat::RGBA32F, texels.data());
        if (m_spriteTable == Render::INVALID_TEXTURE) {
            Log::Error("AtlasBuilder: failed to create the sprite table texture");
        } else {
            Log::Info("Atlas sprite table: %zu sprites in %d row(s)", keys.size(), rows);
        }
    }

    void AtlasBuilder::BuildSpriteAlpha(const std::vector<TextureSource>& sources,
                                        const std::vector<PackRect>& packedRects) {
        m_spriteAlpha.clear();
        auto code = [](unsigned char alpha) -> uint8_t {
            return alpha == 0 ? kTexelTransparent : alpha != 255 ? kTexelTranslucent : 0;
        };
        for (const PackRect& rect : packedRects) {
            if (rect.textureIndex < 0 || static_cast<size_t>(rect.textureIndex) >= sources.size()) continue;
            const TextureSource& source = sources[static_cast<size_t>(rect.textureIndex)];
            SpriteAlpha a;
            a.width = source.width;
            a.height = source.height;
            const size_t texels = static_cast<size_t>(a.width) * static_cast<size_t>(a.height);
            if (texels == 0 || source.data.size() < texels * 4) continue;
            a.bits.assign(texels, 0);
            const bool animated = std::any_of(pendingAnimations.begin(), pendingAnimations.end(),
                [&](const PendingAnimation& p) { return p.textureKey == source.key; });
            if (!animated)
                for (size_t i = 0; i < texels; ++i) a.bits[i] = code(source.data[i * 4 + 3]);
            // MC ORs every unique frame the animation shows
            // (AnimatedTexture.uniqueFrames): the listed frames, or all of
            // them when the .mcmeta lists none.
            for (const PendingAnimation& anim : pendingAnimations) {
                if (anim.textureKey != source.key) continue;
                auto orFrame = [&](size_t index) {
                    if (index >= anim.frames.size() || anim.frames[index].size() < texels * 4) return;
                    const auto& frame = anim.frames[index];
                    for (size_t i = 0; i < texels; ++i) a.bits[i] |= code(frame[i * 4 + 3]);
                };
                if (anim.animation.frames.empty()) {
                    for (size_t f = 0; f < anim.frames.size(); ++f) orFrame(f);
                } else {
                    for (int f : anim.animation.frames) if (f >= 0) orFrame(static_cast<size_t>(f));
                }
            }
            m_spriteAlpha[source.key] = std::move(a);
        }
    }

    uint8_t AtlasBuilder::QuadTransparency(const std::string& textureKey,
                                           float u0, float v0, float u1, float v1) const {
        auto it = m_spriteAlpha.find(textureKey);
        if (it == m_spriteAlpha.end()) return 0xFF;
        const SpriteAlpha& a = it->second;
        // SpriteContents.computeTransparency: floor the min corner, ceil the
        // max corner, in texels.
        const float lu = std::min(u0, u1), hu = std::max(u0, u1);
        const float lv = std::min(v0, v1), hv = std::max(v0, v1);
        const int x0 = std::clamp(static_cast<int>(std::floor(lu * static_cast<float>(a.width))), 0, a.width);
        const int y0 = std::clamp(static_cast<int>(std::floor(lv * static_cast<float>(a.height))), 0, a.height);
        const int x1 = std::clamp(static_cast<int>(std::ceil(hu * static_cast<float>(a.width))), 0, a.width);
        const int y1 = std::clamp(static_cast<int>(std::ceil(hv * static_cast<float>(a.height))), 0, a.height);
        uint8_t bits = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
                bits |= a.bits[static_cast<size_t>(y) * static_cast<size_t>(a.width) + static_cast<size_t>(x)];
        return bits;
    }

    bool AtlasBuilder::GetUVRect(const std::string& textureKey, AtlasUVRect& uvRect) const {
        auto it = textureKeyToUV.find(textureKey);
        if (it != textureKeyToUV.end()) {
            uvRect = it->second;
            return true;
        }
        return false;
    }

    bool AtlasBuilder::SaveAtlasDebugImage(const std::string& outputPath) const {
        // `atlasData` is freed once the sheet is on the GPU (BuildFromJSON);
        // `originalAtlasData` is the level-0 copy kept for RebuildAtlas, and
        // is what a dump after startup has to read. F3+S always said "could
        // not save" because it only looked at the freed one.
        const std::vector<unsigned char>& pixels = atlasData.empty() ? originalAtlasData : atlasData;
        if (pixels.empty() || atlasWidth <= 0 || atlasHeight <= 0 ||
            pixels.size() < static_cast<size_t>(atlasWidth) * static_cast<size_t>(atlasHeight) * 4) {
            Log::Warning("No atlas data to save");
            return false;
        }

        // Save as PNG
        int result = stbi_write_png(outputPath.c_str(),
                                   atlasWidth, atlasHeight, 4,
                                   pixels.data(), atlasWidth * 4);

        if (result) {
            Log::Info("Saved atlas debug image to: %s", outputPath.c_str());
            return true;
        } else {
            Log::Error("Failed to save atlas debug image");
            return false;
        }
    }

    void AtlasBuilder::RegisterAnimations() {
        if (!m_animator) return;
        // A fresh start on the (new) texture: drops the previous frames.
        m_animator->Initialize(m_atlasTexture);
        if (m_atlasTexture == Render::INVALID_TEXTURE) return;
        size_t registered = 0;
        for (const auto& pending : pendingAnimations) {
            auto uvIt = textureKeyToUV.find(pending.textureKey);
            if (uvIt == textureKeyToUV.end()) continue;
            const AtlasUVRect& uvRect = uvIt->second;
            // Rounded, not truncated: uvMin is x / atlasWidth in float, and
            // x / w * w can land a hair under x.
            const int atlasX = static_cast<int>(std::lround(uvRect.uvMin.x * atlasWidth));
            const int atlasY = static_cast<int>(std::lround(uvRect.uvMin.y * atlasHeight));
            m_animator->RegisterAnimatedTexture(pending.textureKey, pending.animation, pending.frames,
                                                atlasX, atlasY, m_padding,
                                                pending.mipmapStrategy, pending.alphaCutoffBias,
                                                EffectiveMipLevels());
            ++registered;
        }
        if (registered > 0) Log::Info("Registered %zu animated textures in the %s atlas", registered, GetConfig().name);
    }

    void AtlasBuilder::UpdateAnimations(float deltaTime) {
        if (m_animator) m_animator->UpdateAnimations(deltaTime);
    }

    namespace {
        // The most common RGBA pixel strictly inside a sprite's 1px frame —
        // what a connected edge is painted with. Falls back to the whole
        // sprite for one too small to have an interior.
        uint32_t InteriorFillPixel(const TextureSource& src) {
            const int w = src.width, h = src.height;
            const int x0 = w > 2 ? 1 : 0, x1 = w > 2 ? w - 1 : w;
            const int y0 = h > 2 ? 1 : 0, y1 = h > 2 ? h - 1 : h;
            std::unordered_map<uint32_t, int> counts;
            uint32_t best = 0;
            int bestCount = -1;
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    uint32_t px = 0;
                    std::memcpy(&px, &src.data[(static_cast<size_t>(y) * w + x) * 4u], 4u);
                    const int n = ++counts[px];
                    if (n > bestCount) { bestCount = n; best = px; }
                }
            }
            return best;
        }
    } // namespace

    void AtlasBuilder::GenerateConnectedTextureVariants(std::vector<TextureSource>& sources) {
        // Variants are accumulated separately and appended once at the end.
        // Pushing into `sources` while iterating it would reallocate the
        // vector out from under the `base` reference below and read freed
        // memory on the next mask — which is exactly what it did.
        const size_t originalCount = sources.size();
        std::vector<TextureSource> variants;

        for (size_t i = 0; i < originalCount; ++i) {
            const TextureSource& base = sources[i];
            if (base.data.empty() || base.width <= 0 || base.height <= 0) continue;

            // Keys look like "block/glass"; match on the trailing name.
            std::string_view name = base.key;
            if (const size_t slash = name.rfind('/'); slash != std::string_view::npos) {
                name.remove_prefix(slash + 1);
            }
            if (!CTM::IsConnected(name)) continue;

            const int w = base.width, h = base.height;

            for (int slot = 0; slot < CTM::VariantCount(); ++slot) {
                const uint8_t mask = CTM::MaskForSlot(slot);

                // Copies key/path/dimensions/pixels AND the mcmeta-derived
                // mipmap settings, so a variant is mipmapped exactly like the
                // sprite it came from.
                TextureSource variant = base;
                variant.key = CTM::VariantKey(base.key, slot);

                // Replace the 1px frame along every edge that abuts an
                // identical block with the sprite's interior FILL — the most
                // common pixel inside the frame. For plain glass that is the
                // transparent pixel, so the frame simply vanishes; for tinted
                // and stained glass the interior is a translucent colour, and
                // clearing the frame to alpha 0 (what this used to do) cut a
                // clear 1px line between every two panes, so the tint never
                // read as one sheet. The mode, not the pixel next door, so a
                // highlight streak touching the frame cannot leak into it.
                const uint32_t fill = InteriorFillPixel(base);
                auto clearPixel = [&](int x, int y) {
                    std::memcpy(&variant.data[(static_cast<size_t>(y) * w + x) * 4u], &fill, 4u);
                };

                const bool l = (mask & CTM::LEFT)   != 0;
                const bool r = (mask & CTM::RIGHT)  != 0;
                const bool t = (mask & CTM::TOP)    != 0;
                const bool b = (mask & CTM::BOTTOM) != 0;

                // Edge INTERIORS only — the four corner pixels are shared
                // between two edges and are decided separately below. Clearing
                // a whole column would take the corners with it and punch a
                // 2px notch out of the perpendicular border wherever two tiles
                // meet, leaving the group's outline visibly dashed.
                if (l) for (int y = 1; y < h - 1; ++y) clearPixel(0,     y);
                if (r) for (int y = 1; y < h - 1; ++y) clearPixel(w - 1, y);
                if (t) for (int x = 1; x < w - 1; ++x) clearPixel(x, 0);
                if (b) for (int x = 1; x < w - 1; ++x) clearPixel(x, h - 1);

                // A corner survives unless both edges meeting there are
                // connected AND the diagonal cell is filled too. Drop the
                // diagonal test and a concave corner — the inside of an L —
                // loses the single pixel that closes the outline around the
                // notch, which is the difference between 16 tiles and 47.
                if (l && t && (mask & CTM::TL)) clearPixel(0,     0);
                if (r && t && (mask & CTM::TR)) clearPixel(w - 1, 0);
                if (l && b && (mask & CTM::BL)) clearPixel(0,     h - 1);
                if (r && b && (mask & CTM::BR)) clearPixel(w - 1, h - 1);

                variants.push_back(std::move(variant));
            }
        }

        if (!variants.empty()) {
            const size_t blocks = variants.size() / static_cast<size_t>(CTM::VariantCount());
            Log::Info("Connected textures: derived %zu variant tiles for %zu block(s)",
                      variants.size(), blocks);
            sources.insert(sources.end(),
                           std::make_move_iterator(variants.begin()),
                           std::make_move_iterator(variants.end()));
        }
    }

    bool AtlasBuilder::ParseTextureMeta(const std::string& mcmetaPath,
                                        std::string& outStrategy, float& outBias) const {
        if (!std::filesystem::exists(mcmetaPath)) return false;

        std::ifstream file(mcmetaPath);
        if (!file.is_open()) return false;

        nlohmann::json mcmeta;
        try {
            file >> mcmeta;
        } catch (const nlohmann::json::exception&) {
            // ParseMcMetaFile logs the same failure a moment later; staying
            // quiet here avoids a duplicate warning per bad file.
            return false;
        }

        if (!mcmeta.contains("texture") || !mcmeta["texture"].is_object()) return false;
        const auto& tex = mcmeta["texture"];

        // Both fields are optional and independent — cactus_side.png.mcmeta
        // sets only the bias, dandelion.png.mcmeta only the strategy.
        if (tex.contains("mipmap_strategy") && tex["mipmap_strategy"].is_string()) {
            outStrategy = tex["mipmap_strategy"].get<std::string>();
        }
        if (tex.contains("alpha_cutoff_bias") && tex["alpha_cutoff_bias"].is_number()) {
            outBias = tex["alpha_cutoff_bias"].get<float>();
        }
        return true;
    }

    bool AtlasBuilder::ParseMcMetaFile(const std::string& mcmetaPath, TextureAnimation& animation) {
        if (!std::filesystem::exists(mcmetaPath)) {
            return false;
        }

        std::ifstream file(mcmetaPath);
        if (!file.is_open()) {
            return false;
        }

        nlohmann::json mcmeta;
        try {
            file >> mcmeta;
        } catch (const nlohmann::json::exception& e) {
            Log::Warning("Failed to parse .mcmeta file %s: %s", mcmetaPath.c_str(), e.what());
            return false;
        }

        // Check for animation section
        if (!mcmeta.contains("animation")) {
            return false;
        }

        const auto& animData = mcmeta["animation"];

        // Parse frametime (default 1)
        animation.frametime = animData.value("frametime", 1);

        // Parse interpolate flag (default false)
        animation.interpolate = animData.value("interpolate", false);

        // Explicit frame dimensions. Absent on every vanilla block sprite, but
        // they are the first branch of MC's calculateFrameSize, so a resource
        // pack that sets them has to win over the min(w,h) fallback.
        animation.declaredFrameWidth  = animData.value("width", 0);
        animation.declaredFrameHeight = animData.value("height", 0);

        // Parse custom frame sequence if present
        // MC AnimationMetadataSection.frames: a list of AnimationFrame, each
        // a bare index or {index, time}; a frame without its own time runs
        // for the section's frametime (FrameInfo(index, time.orElse(
        // defaultFrameTime))). Times are kept parallel to the indices; a
        // non-positive time is MC's codec error, read here as the default.
        if (animData.contains("frames") && animData["frames"].is_array()) {
            animation.frames.clear();
            animation.frameTimes.clear();
            bool anyTimed = false;
            for (const auto& frame : animData["frames"]) {
                if (frame.is_number_integer()) {
                    animation.frames.push_back(frame.get<int>());
                    animation.frameTimes.push_back(0);
                } else if (frame.is_object() && frame.contains("index")) {
                    animation.frames.push_back(frame["index"].get<int>());
                    const int time = frame.value("time", 0);
                    animation.frameTimes.push_back(time > 0 ? time : 0);
                    anyTimed = anyTimed || time > 0;
                }
            }
            if (!anyTimed) animation.frameTimes.clear();
        }

        /*Log::Debug("Parsed .mcmeta: frametime=%d, interpolate=%s, custom_frames=%zu",
                  animation.frametime, animation.interpolate ? "true" : "false",
                  animation.frames.size());*/

        return true;
    }

    bool AtlasBuilder::LoadAnimatedTexture(const std::string& texturePath,
                                         TextureSource& source,
                                         TextureAnimation& animation,
                                         std::vector<std::vector<unsigned char>>& frames) {
        
        // First, load the full texture strip
        int fullWidth, fullHeight;
        std::vector<unsigned char> fullData;
        
        if (!LoadPNG(texturePath, fullWidth, fullHeight, fullData)) {
            return false;
        }

        // Frame layout, per MC AnimationMetadataSection.calculateFrameSize:
        // an explicit `width`/`height` wins, a lone one keeps the sprite's
        // other dimension, and with neither the frame is a
        // min(spriteWidth, spriteHeight) SQUARE.
        //
        // This is not the same as "frames are 16x16". lava_flow is 32x512 and
        // water_flow is 32x1024, so their frames are 32x32 — assuming 16
        // sliced each real frame into four quarter-tiles, which then animated
        // as if they were consecutive frames and packed a sprite at half the
        // texel density of every other block.
        int frameW = animation.declaredFrameWidth;
        int frameH = animation.declaredFrameHeight;
        if (frameW <= 0 && frameH <= 0) {
            frameW = frameH = std::min(fullWidth, fullHeight);
        } else if (frameW <= 0) {
            frameW = fullWidth;
        } else if (frameH <= 0) {
            frameH = fullHeight;
        }

        if (frameW <= 0 || frameH <= 0 || frameW > fullWidth || frameH > fullHeight) {
            Log::Warning("Animated texture %s: bad frame size %dx%d for a %dx%d sprite",
                         texturePath.c_str(), frameW, frameH, fullWidth, fullHeight);
            return false;
        }

        animation.width = frameW;
        animation.height = frameH;

        // Calculate how many columns and rows of frames we have
        int columns = fullWidth / frameW;
        int rows = fullHeight / frameH;
        int totalFrames = columns * rows;

        if (totalFrames <= 1) {
            // Not an animated texture
            return false;
        }

        animation.frameCount = totalFrames;

        // Extract frames in ROW-major order. MC indexes a frame as
        // (index % frameRowSize, index / frameRowSize) — SpriteContents.java
        // getFrameX/getFrameY — so frame N is the Nth cell reading left to
        // right, then top to bottom. Column-major only agrees with that for
        // the single-column strips, which is why it survived: every animated
        // block sprite except the two *_flow ones is one frame wide.
        frames.clear();
        frames.reserve(totalFrames);

        for (int row = 0; row < rows; ++row) {
            for (int column = 0; column < columns; ++column) {
                std::vector<unsigned char> frameData(static_cast<size_t>(frameW) * frameH * 4);

                // Calculate source position for this frame
                int srcStartX = column * frameW;
                int srcStartY = row * frameH;

                // Copy frame data from full texture
                for (int y = 0; y < frameH; ++y) {
                    for (int x = 0; x < frameW; ++x) {
                        int srcX = srcStartX + x;
                        int srcY = srcStartY + y;
                        int srcIdx = (srcY * fullWidth + srcX) * 4;
                        int dstIdx = (y * frameW + x) * 4;

                        frameData[dstIdx + 0] = fullData[srcIdx + 0]; // R
                        frameData[dstIdx + 1] = fullData[srcIdx + 1]; // G
                        frameData[dstIdx + 2] = fullData[srcIdx + 2]; // B
                        frameData[dstIdx + 3] = fullData[srcIdx + 3]; // A
                    }
                }

                frames.push_back(std::move(frameData));
            }
        }

        // Set up source with only the first frame
        source.width = animation.width;
        source.height = animation.height;
        source.data = frames[0]; // Use first frame for atlas

        /*Log::Info("Loaded animated texture: %s (%d columns × %d rows = %d frames, %dx%d each)",
                 texturePath.c_str(), columns, rows, totalFrames, animation.width, animation.height);*/

        return true;
    }

} // namespace Render
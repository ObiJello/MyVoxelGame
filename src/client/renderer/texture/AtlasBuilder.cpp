// File: src/client/renderer/texture/AtlasBuilder.cpp
#include "AtlasBuilder.hpp"
#include "TextureAnimator.hpp"
#include "MipmapGenerator.hpp"
#include "ConnectedTextures.hpp"
#include "../backend/RenderBackend.hpp"
#include "common/core/Log.hpp"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <iterator>

// Include stb_image for PNG loading
#include "../../../ext/stb_image/stb_image.h"

// Include stb_image_write for debug output (optional)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../ext/stb_image/stb_image_write.h"

namespace Render {

    // Global instance (optional)
    std::unique_ptr<AtlasBuilder> g_atlasBuilder = nullptr;

    AtlasBuilder::AtlasBuilder()
        : atlasWidth(DEFAULT_ATLAS_SIZE)
        , atlasHeight(DEFAULT_ATLAS_SIZE)
        , mipmapEnabled(true)
        , textureAnimator(nullptr) {
    }

    AtlasBuilder::~AtlasBuilder() {
        if (Render::g_renderBackend) {
            if (m_atlasTexture != Render::INVALID_TEXTURE)
                Render::g_renderBackend->DestroyTexture(m_atlasTexture);
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

        // Step 1: Parse the JSON atlas descriptor
        textureSources.clear();
        if (!ParseAtlasJSON(atlasJsonPath, texturesRootPath, textureSources)) {
            Log::Error("Failed to parse atlas JSON");
            return false;
        }
        Log::Info("✓ Parsed atlas JSON - found %zu texture sources", textureSources.size());

        // Step 2: Load biome colormaps
        if (!LoadColormaps(texturesRootPath)) {
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
        GenerateConnectedTextureVariants(textureSources);

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

        // Scan directory for PNG files
        auto pngFiles = ScanDirectoryForPNGs(fullDirPath);

        for (const auto& pngFile : pngFiles) {
            // Extract relative path from directory
            std::filesystem::path filePath(pngFile);
            std::filesystem::path relativePath =
                std::filesystem::relative(filePath, fullDirPath);

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
        if (!source.contains("resource") || !source.contains("sprite")) {
            Log::Warning("Single source missing required fields");
            return;
        }

        std::string resource = source["resource"];
        std::string sprite = source["sprite"];

        // Remove "minecraft:" prefix if present and add .png extension
        if (sprite.find("minecraft:") == 0) {
            sprite = sprite.substr(10); // Remove "minecraft:"
        }

        // Build full path
        std::string fullPath = texturesRoot + "/" + sprite + ".png";

        // Create texture source
        TextureSource texSource;
        texSource.key = resource;
        texSource.path = fullPath;

        sources.push_back(texSource);

        Log::Debug("Added single texture: %s -> %s",
                  resource.c_str(), fullPath.c_str());
    }

    std::vector<std::string> AtlasBuilder::ScanDirectoryForPNGs(const std::string& dirPath) {
        std::vector<std::string> pngFiles;

        if (!std::filesystem::exists(dirPath)) {
            Log::Warning("Directory does not exist: %s", dirPath.c_str());
            return pngFiles;
        }

        try {
            for (const auto& entry :
                 std::filesystem::recursive_directory_iterator(dirPath)) {
                if (entry.is_regular_file()) {
                    auto path = entry.path();
                    if (path.extension() == ".png") {
                        pngFiles.push_back(path.string());
                    }
                }
            }
        } catch (const std::exception& e) {
            Log::Error("Error scanning directory %s: %s", dirPath.c_str(), e.what());
        }

        // Sort for deterministic atlas packing across runs
        std::sort(pngFiles.begin(), pngFiles.end());

        return pngFiles;
    }

    bool AtlasBuilder::LoadColormaps(const std::string& texturesRoot) {
        Log::Info("Loading biome colormaps...");

        // Load grass colormap
        std::string grassPath = texturesRoot + "/colormap/grass.png";
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
        std::string foliagePath = texturesRoot + "/colormap/foliage.png";
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
        // Start with default size
        int currentSize = DEFAULT_ATLAS_SIZE;

        while (currentSize <= MAX_ATLAS_SIZE) {
            // Create root packing node
            auto root = std::make_unique<PackNode>(0, 0, currentSize, currentSize);

            packedRects.clear();
            bool allPacked = true;

            // Try to pack all textures
            for (size_t i = 0; i < sources.size(); ++i) {
                const auto& source = sources[i];

                // Add MIPMAP_PADDING pixels on each side (32 total) for mipmap-safe padding
                int paddedWidth = source.width + MIPMAP_PADDING * 2;
                int paddedHeight = source.height + MIPMAP_PADDING * 2;

                PackNode* node = InsertRect(root.get(), paddedWidth, paddedHeight, i);

                if (node) {
                    PackRect rect;
                    rect.x = node->x + MIPMAP_PADDING; // Account for padding
                    rect.y = node->y + MIPMAP_PADDING;
                    rect.width = source.width;
                    rect.height = source.height;
                    rect.textureIndex = i;
                    packedRects.push_back(rect);
                } else {
                    allPacked = false;
                    break;
                }
            }

            if (allPacked) {
                outWidth = currentSize;
                outHeight = currentSize;
                Log::Info("Successfully packed %zu textures into %dx%d atlas",
                         packedRects.size(), currentSize, currentSize);
                return true;
            }

            // Try next power of 2
            currentSize *= 2;
        }

        Log::Error("Failed to pack %zu textures even at max size %dx%d",
                  sources.size(), MAX_ATLAS_SIZE, MAX_ATLAS_SIZE);
        return false;
    }

    PackNode* AtlasBuilder::InsertRect(PackNode* node, int width, int height, int index) {
        if (!node) {
            return nullptr;
        }
        if (node->used) {
            // Try inserting into children
            PackNode* newNode = InsertRect(node->left.get(), width, height, index);
            if (newNode) return newNode;

            return InsertRect(node->right.get(), width, height, index);
        }

        // If this node is too small, return
        if (width > node->width || height > node->height) {
            return nullptr;
        }

        // If it's a perfect fit, use this node
        if (width == node->width && height == node->height) {
            node->used = true;
            return node;
        }

        // Otherwise, split this node
        node->used = true;

        // Decide which way to split
        int dw = node->width - width;
        int dh = node->height - height;

        if (dw > dh) {
            // Split vertically
            node->left = std::make_unique<PackNode>(
                node->x, node->y, width, node->height);
            node->right = std::make_unique<PackNode>(
                node->x + width, node->y, dw, node->height);
        } else {
            // Split horizontally
            node->left = std::make_unique<PackNode>(
                node->x, node->y, node->width, height);
            node->right = std::make_unique<PackNode>(
                node->x, node->y + height, node->width, dh);
        }

        // Insert into first child
        return InsertRect(node->left.get(), width, height, index);
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

        // Register pending animations with texture animator
        if (textureAnimator && !pendingAnimations.empty() && m_atlasTexture != Render::INVALID_TEXTURE) {
            textureAnimator->Initialize(m_atlasTexture);

            for (const auto& pending : pendingAnimations) {
                auto uvIt = textureKeyToUV.find(pending.textureKey);
                if (uvIt != textureKeyToUV.end()) {
                    const AtlasUVRect& uvRect = uvIt->second;
                    int atlasX = static_cast<int>(uvRect.uvMin.x * atlasWidth);
                    int atlasY = static_cast<int>(uvRect.uvMin.y * atlasHeight);

                    textureAnimator->RegisterAnimatedTexture(
                        pending.textureKey, pending.animation, pending.frames,
                        atlasX, atlasY,
                        pending.mipmapStrategy, pending.alphaCutoffBias,
                        (mipmapEnabled ? m_mipmapLevel : 0)
                    );
                }
            }
            Log::Info("Registered %zu animated textures", pendingAnimations.size());
        }

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
        if (mipmapEnabled == enabled) return;
        mipmapEnabled = enabled;
        if (m_atlasTexture != Render::INVALID_TEXTURE) {
            UpdateTextureParameters();
            BuildAndUploadMipChain(textureSources, m_packedRects);
            Log::Info("AtlasBuilder mipmaps %s", enabled ? "enabled" : "disabled");
        }
    }

    void AtlasBuilder::SetMipmapLevel(int level) {
        m_mipmapLevel = std::max(0, std::min(4, level));
        if (m_atlasTexture != Render::INVALID_TEXTURE) {
            UpdateTextureParameters();
            // The chain is CPU-authored, so a new level count means rebuilding
            // it — the driver is not going to fill the extra levels for us.
            BuildAndUploadMipChain(textureSources, m_packedRects);
            Log::Info("Set mipmap level to %d", m_mipmapLevel);
        }
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
        Render::g_renderBackend->DestroyTexture(m_atlasTexture);

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
        if (textureAnimator) {
            textureAnimator->Initialize(m_atlasTexture);
        }

        Log::Info("Atlas rebuilt with %s rendering mode",
                  useMinecraftStyle ? "Minecraft-style" : "Classic");
    }

    void AtlasBuilder::BuildAndUploadMipChain(const std::vector<TextureSource>& sources,
                                              const std::vector<PackRect>& packedRects) {
        if (m_atlasTexture == Render::INVALID_TEXTURE || !Render::g_renderBackend) return;
        if (!mipmapEnabled || m_mipmapLevel <= 0) return;
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
        const int levels = m_mipmapLevel;
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
                        rect.x, rect.y, chain[0].width, chain[0].height, MIPMAP_PADDING);
            }
            for (size_t k = 1; k < chain.size(); ++k) {
                const int kk = static_cast<int>(k);
                blit(levelData[k], levelW[k], chain[k], rect.x >> kk, rect.y >> kk);
                extrude(levelData[k], levelW[k], levelH[k],
                        rect.x >> kk, rect.y >> kk,
                        chain[k].width, chain[k].height, MIPMAP_PADDING >> kk);
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
        // Extrude edges by MIPMAP_PADDING pixels to prevent mipmap bleeding.
        // Each edge pixel is repeated outward into the padding region (clamp-to-edge pattern).

        // Top edge - repeat top row upward
        for (int p = 1; p <= MIPMAP_PADDING; ++p) {
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
        for (int p = 0; p < MIPMAP_PADDING; ++p) {
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
        for (int p = 1; p <= MIPMAP_PADDING; ++p) {
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
        for (int p = 0; p < MIPMAP_PADDING; ++p) {
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
        // Each corner extends MIPMAP_PADDING in both directions, filled with the nearest corner pixel

        // Top-left corner block
        for (int py = 1; py <= MIPMAP_PADDING; ++py) {
            int dstY = textureY - py;
            if (dstY < 0) continue;
            for (int px = 1; px <= MIPMAP_PADDING; ++px) {
                int dstX = textureX - px;
                if (dstX < 0) continue;
                int srcIdx = (textureY * atlasWidth + textureX) * 4;
                int dstIdx = (dstY * atlasWidth + dstX) * 4;
                for (int c = 0; c < 4; ++c)
                    atlasData[dstIdx + c] = atlasData[srcIdx + c];
            }
        }

        // Top-right corner block
        for (int py = 1; py <= MIPMAP_PADDING; ++py) {
            int dstY = textureY - py;
            if (dstY < 0) continue;
            for (int px = 0; px < MIPMAP_PADDING; ++px) {
                int dstX = textureX + textureWidth + px;
                if (dstX >= atlasWidth) continue;
                int srcIdx = (textureY * atlasWidth + (textureX + textureWidth - 1)) * 4;
                int dstIdx = (dstY * atlasWidth + dstX) * 4;
                for (int c = 0; c < 4; ++c)
                    atlasData[dstIdx + c] = atlasData[srcIdx + c];
            }
        }

        // Bottom-left corner block
        for (int py = 0; py < MIPMAP_PADDING; ++py) {
            int dstY = textureY + textureHeight + py;
            if (dstY >= atlasHeight) continue;
            for (int px = 1; px <= MIPMAP_PADDING; ++px) {
                int dstX = textureX - px;
                if (dstX < 0) continue;
                int srcIdx = ((textureY + textureHeight - 1) * atlasWidth + textureX) * 4;
                int dstIdx = (dstY * atlasWidth + dstX) * 4;
                for (int c = 0; c < 4; ++c)
                    atlasData[dstIdx + c] = atlasData[srcIdx + c];
            }
        }

        // Bottom-right corner block
        for (int py = 0; py < MIPMAP_PADDING; ++py) {
            int dstY = textureY + textureHeight + py;
            if (dstY >= atlasHeight) continue;
            for (int px = 0; px < MIPMAP_PADDING; ++px) {
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

    bool AtlasBuilder::GetUVRect(const std::string& textureKey, AtlasUVRect& uvRect) const {
        auto it = textureKeyToUV.find(textureKey);
        if (it != textureKeyToUV.end()) {
            uvRect = it->second;
            return true;
        }
        return false;
    }

    bool AtlasBuilder::SaveAtlasDebugImage(const std::string& outputPath) const {
        if (atlasData.empty()) {
            Log::Warning("No atlas data to save");
            return false;
        }

        // Save as PNG
        int result = stbi_write_png(outputPath.c_str(),
                                   atlasWidth, atlasHeight, 4,
                                   atlasData.data(), atlasWidth * 4);

        if (result) {
            Log::Info("Saved atlas debug image to: %s", outputPath.c_str());
            return true;
        } else {
            Log::Error("Failed to save atlas debug image");
            return false;
        }
    }

    // **NEW**: Animation support methods
    void AtlasBuilder::SetTextureAnimator(TextureAnimator* animator) {
        textureAnimator = animator;
        if (textureAnimator && m_atlasTexture != Render::INVALID_TEXTURE) {
            textureAnimator->Initialize(m_atlasTexture);
        }
    }

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

                // Erase the 1px frame along every edge that abuts an identical
                // block. Alpha only — the RGB is left alone so the mipmap
                // solidify pass still has real colour to flood outward.
                auto clearPixel = [&](int x, int y) {
                    variant.data[(static_cast<size_t>(y) * w + x) * 4u + 3u] = 0;
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

        // Parse custom frame sequence if present
        if (animData.contains("frames") && animData["frames"].is_array()) {
            animation.frames.clear();
            for (const auto& frame : animData["frames"]) {
                if (frame.is_number_integer()) {
                    animation.frames.push_back(frame.get<int>());
                } else if (frame.is_object() && frame.contains("index")) {
                    // Frame object with index and optional time
                    animation.frames.push_back(frame["index"].get<int>());
                    // TODO: Handle per-frame timing if needed
                }
            }
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

        // Calculate frame layout from texture dimensions
        // Standard Minecraft frame size is 16x16
        const int FRAME_SIZE = 16;
        animation.width = FRAME_SIZE;
        animation.height = FRAME_SIZE;
        
        // Calculate how many columns and rows of frames we have
        int columns = fullWidth / FRAME_SIZE;
        int rows = fullHeight / FRAME_SIZE;
        int totalFrames = columns * rows;

        if (totalFrames <= 1) {
            // Not an animated texture
            return false;
        }

        animation.frameCount = totalFrames;

        // Extract frames in column-major order (left to right, top to bottom within each column)
        frames.clear();
        frames.reserve(totalFrames);

        for (int column = 0; column < columns; ++column) {
            for (int row = 0; row < rows; ++row) {
                std::vector<unsigned char> frameData(FRAME_SIZE * FRAME_SIZE * 4);
                
                // Calculate source position for this frame
                int srcStartX = column * FRAME_SIZE;
                int srcStartY = row * FRAME_SIZE;
                
                // Copy frame data from full texture
                for (int y = 0; y < FRAME_SIZE; ++y) {
                    for (int x = 0; x < FRAME_SIZE; ++x) {
                        int srcX = srcStartX + x;
                        int srcY = srcStartY + y;
                        int srcIdx = (srcY * fullWidth + srcX) * 4;
                        int dstIdx = (y * FRAME_SIZE + x) * 4;
                        
                        frameData[dstIdx + 0] = fullData[srcIdx + 0]; // R
                        frameData[dstIdx + 1] = fullData[srcIdx + 1]; // G
                        frameData[dstIdx + 2] = fullData[srcIdx + 2]; // B
                        frameData[dstIdx + 3] = fullData[srcIdx + 3]; // A
                    }
                }
                
                frames.push_back(frameData);
            }
        }

        // Set up source with only the first frame (16x16)
        source.width = animation.width;
        source.height = animation.height;
        source.data = frames[0]; // Use first frame for atlas

        /*Log::Info("Loaded animated texture: %s (%d columns × %d rows = %d frames, %dx%d each)",
                 texturePath.c_str(), columns, rows, totalFrames, animation.width, animation.height);*/

        return true;
    }

} // namespace Render
// File: src/client/renderer/texture/AtlasBuilder.cpp
#include "AtlasBuilder.hpp"
#include "TextureAnimator.hpp"
#include "common/core/Log.hpp"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <cmath>

// Include stb_image for PNG loading
#include "../../../ext/stb_image/stb_image.h"

// Include stb_image_write for debug output (optional)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../ext/stb_image/stb_image_write.h"

namespace Render {

    // Global instance (optional)
    std::unique_ptr<AtlasBuilder> g_atlasBuilder = nullptr;

    AtlasBuilder::AtlasBuilder()
        : atlasTextureID(0)
        , grassColormapID(0)
        , foliageColormapID(0)
        , atlasWidth(DEFAULT_ATLAS_SIZE)
        , atlasHeight(DEFAULT_ATLAS_SIZE)
        , mipmapEnabled(false)
        , textureAnimator(nullptr) {
    }

    AtlasBuilder::~AtlasBuilder() {
        if (atlasTextureID != 0) {
            glDeleteTextures(1, &atlasTextureID);
        }
        if (grassColormapID != 0) {
            glDeleteTextures(1, &grassColormapID);
        }
        if (foliageColormapID != 0) {
            glDeleteTextures(1, &foliageColormapID);
        }
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
        Log::Info("Atlas texture ID: %u (%dx%d)", atlasTextureID, atlasWidth, atlasHeight);
        Log::Info("Grass colormap ID: %u", grassColormapID);
        Log::Info("Foliage colormap ID: %u", foliageColormapID);
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
                grassColormapID = CreateColormapTexture(grassData, grassWidth, grassHeight);
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
                foliageColormapID = CreateColormapTexture(foliageData,
                                                         foliageWidth, foliageHeight);
                Log::Info("✓ Loaded foliage colormap: %dx%d", foliageWidth, foliageHeight);
            } else {
                Log::Warning("Foliage colormap is %dx%d, expected 256x256",
                           foliageWidth, foliageHeight);
            }
        } else {
            Log::Warning("Failed to load foliage colormap: %s", foliagePath.c_str());
        }

        return grassColormapID != 0 || foliageColormapID != 0;
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

                // Add 2 pixel padding to avoid bleeding
                int paddedWidth = source.width + 2;
                int paddedHeight = source.height + 2;

                PackNode* node = InsertRect(root.get(), paddedWidth, paddedHeight, i);

                if (node) {
                    PackRect rect;
                    rect.x = node->x + 1; // Account for padding
                    rect.y = node->y + 1;
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

            // Record UV coordinates
            AtlasUVRect uvRect;
            uvRect.uvMin.x = static_cast<float>(rect.x) / atlasWidth;
            uvRect.uvMin.y = static_cast<float>(rect.y) / atlasHeight;
            uvRect.uvMax.x = static_cast<float>(rect.x + rect.width) / atlasWidth;
            uvRect.uvMax.y = static_cast<float>(rect.y + rect.height) / atlasHeight;

            textureKeyToUV[source.key] = uvRect;
        }

        // Create OpenGL texture
        glGenTextures(1, &atlasTextureID);
        glBindTexture(GL_TEXTURE_2D, atlasTextureID);

        // Upload atlas data
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, atlasWidth, atlasHeight, 0,
                    GL_RGBA, GL_UNSIGNED_BYTE, atlasData.data());

        // Use the new parameter system instead of hardcoded values
        UpdateTextureParameters();

        glBindTexture(GL_TEXTURE_2D, 0);

        Log::Info("✓ Created atlas texture ID %u (%dx%d, mipmaps: %s)",
                 atlasTextureID, atlasWidth, atlasHeight, mipmapEnabled ? "enabled" : "disabled");

        // **NEW**: Register pending animations with texture animator
        if (textureAnimator && !pendingAnimations.empty()) {
            textureAnimator->Initialize(atlasTextureID);
            
            for (const auto& pending : pendingAnimations) {
                // Find atlas position for this texture
                auto uvIt = textureKeyToUV.find(pending.textureKey);
                if (uvIt != textureKeyToUV.end()) {
                    const AtlasUVRect& uvRect = uvIt->second;
                    
                    // Convert UV coordinates back to pixel coordinates
                    int atlasX = static_cast<int>(uvRect.uvMin.x * atlasWidth);
                    int atlasY = static_cast<int>(uvRect.uvMin.y * atlasHeight);
                    
                    // Register animated texture
                    textureAnimator->RegisterAnimatedTexture(
                        pending.textureKey,
                        pending.animation,
                        pending.frames,
                        atlasX, atlasY
                    );
                    
                    Log::Debug("Registered animated texture: %s at atlas pos (%d,%d)",
                              pending.textureKey.c_str(), atlasX, atlasY);
                }
            }
            
            Log::Info("✓ Registered %zu animated textures", pendingAnimations.size());
        }

        return true;
    }

    // **NEW**: Mipmap control implementation
    void AtlasBuilder::SetMipmapEnabled(bool enabled) {
        if (mipmapEnabled == enabled) {
            return; // No change needed
        }

        mipmapEnabled = enabled;

        if (atlasTextureID != 0) {
            UpdateTextureParameters();
            Log::Info("AtlasBuilder mipmaps %s", enabled ? "enabled" : "disabled");
        }
    }

    // **NEW**: Update texture parameters
    void AtlasBuilder::UpdateTextureParameters() {
        if (atlasTextureID == 0) {
            return;
        }

        glBindTexture(GL_TEXTURE_2D, atlasTextureID);

        if (mipmapEnabled) {
            // Mipmap enabled
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

            // Regenerate mipmaps with current data
            glGenerateMipmap(GL_TEXTURE_2D);
        } else {
            // Mipmap disabled
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }

        // Keep wrap mode unchanged
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindTexture(GL_TEXTURE_2D, 0);
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

    GLuint AtlasBuilder::CreateColormapTexture(const std::vector<unsigned char>& data,
                                              int width, int height) {
        GLuint textureID;
        glGenTextures(1, &textureID);
        glBindTexture(GL_TEXTURE_2D, textureID);

        // Upload colormap data
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                    GL_RGBA, GL_UNSIGNED_BYTE, data.data());

        // Use nearest filtering for pixel-perfect lookup
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindTexture(GL_TEXTURE_2D, 0);

        return textureID;
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
        if (textureAnimator && atlasTextureID != 0) {
            textureAnimator->Initialize(atlasTextureID);
        }
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
#include "TextureAtlas.hpp"
#include "../core/Log.hpp"
#include <filesystem>
#include <algorithm>
#include <cstring>  // for std::memcpy

// Use stb_image for PNG loading
#define STB_IMAGE_IMPLEMENTATION
#include "../../ext/stb_image/stb_image.h"

namespace Render {

    // Global atlas instance
    TextureAtlas g_textureAtlas;

    TextureAtlas::TextureAtlas()
        : textureID(0), isLoaded(false), nextAvailableIndex(0) // Don't reserve index 0 anymore
    {
        // Pre-allocate atlas data (RGBA format) for 256x1024 atlas
        atlasData.resize(ATLAS_WIDTH * ATLAS_HEIGHT * 4, 0);
    }

    TextureAtlas::~TextureAtlas() {
        if (textureID != 0) {
            glDeleteTextures(1, &textureID);
        }
    }

    bool TextureAtlas::Initialize(const std::string& atlasPath) {
        Log::Info("Initializing texture atlas from file: %s", atlasPath.c_str());

        // Check if atlas file exists
        if (!std::filesystem::exists(atlasPath)) {
            Log::Warning("Atlas file does not exist: %s", atlasPath.c_str());
            Log::Info("Creating atlas with default textures only");

            // Initialize with default patterns only
            InitializeAtlasData();
        } else {
            // Load the atlas file directly
            int width, height, channels;
            stbi_set_flip_vertically_on_load(0); // DON'T flip - keep original orientation - FIXED!
            unsigned char* data = stbi_load(atlasPath.c_str(), &width, &height, &channels, STBI_rgb_alpha);

            if (!data) {
                Log::Error("Failed to load atlas file: %s - %s", atlasPath.c_str(), stbi_failure_reason());
                Log::Info("Falling back to default textures");
                InitializeAtlasData();
            } else {
                Log::Info("Loaded atlas file: %dx%d pixels, %d channels", width, height, channels);

                // Validate atlas dimensions
                if (width != ATLAS_WIDTH || height != ATLAS_HEIGHT) {
                    Log::Warning("Atlas file is %dx%d, expected %dx%d. This may cause UV coordinate issues.",
                                width, height, ATLAS_WIDTH, ATLAS_HEIGHT);
                }

                // Copy the loaded data to our atlas buffer
                size_t expectedSize = ATLAS_WIDTH * ATLAS_HEIGHT * 4;
                if (width == ATLAS_WIDTH && height == ATLAS_HEIGHT) {
                    // Perfect match - direct copy
                    std::memcpy(atlasData.data(), data, expectedSize);
                } else {
                    // Size mismatch - resize using nearest neighbor sampling
                    Log::Info("Resizing atlas from %dx%d to %dx%d", width, height, ATLAS_WIDTH, ATLAS_HEIGHT);

                    for (int y = 0; y < ATLAS_HEIGHT; ++y) {
                        for (int x = 0; x < ATLAS_WIDTH; ++x) {
                            // Map atlas coordinates to source coordinates
                            int srcX = (x * width) / ATLAS_WIDTH;
                            int srcY = (y * height) / ATLAS_HEIGHT;

                            // Clamp to source bounds
                            srcX = std::min(srcX, width - 1);
                            srcY = std::min(srcY, height - 1);

                            // Copy RGBA pixel
                            int dstIdx = (y * ATLAS_WIDTH + x) * 4;
                            int srcIdx = (srcY * width + srcX) * 4;

                            atlasData[dstIdx + 0] = data[srcIdx + 0]; // R
                            atlasData[dstIdx + 1] = data[srcIdx + 1]; // G
                            atlasData[dstIdx + 2] = data[srcIdx + 2]; // B
                            atlasData[dstIdx + 3] = data[srcIdx + 3]; // A
                        }
                    }
                }

                stbi_image_free(data);

                // Update loaded texture count (assume all tiles are potentially used)
                loadedTextures.clear();
                for (int i = 0; i < MAX_TILES; ++i) {
                    loadedTextures.push_back("atlas_tile_" + std::to_string(i));
                }
                nextAvailableIndex = MAX_TILES; // Atlas is full
            }
        }

        // Create OpenGL texture
        glGenTextures(1, &textureID);
        UploadToGPU();

        isLoaded = true;
        Log::Info("Texture atlas initialized successfully");
        return true;
    }

    void TextureAtlas::InitializeAtlasData() {
        // Fill with transparent pixels initially
        std::fill(atlasData.begin(), atlasData.end(), 0);

        // Create some default textures for fallback when no atlas file is found
        // Note: Index 0 is now available for your dirt texture from the atlas file

        // Air now uses index 1008, so create a transparent texture there
        CreateSolidTexture(1008, 0, 0, 0, 0); // Fully transparent for air

        if (nextAvailableIndex < MAX_TILES) {
            // Index 1: Stone-like texture (gray with some noise) - if no atlas file
            CreateSolidTexture(1, 128, 128, 128);
            textureNameToIndex["stone"] = 1;
            loadedTextures.push_back("default_stone");
            nextAvailableIndex = std::max(nextAvailableIndex, static_cast<uint16_t>(2));
        }

        if (nextAvailableIndex < MAX_TILES) {
            // Index 2: Dirt-like texture (brown) - fallback if no atlas file
            CreateSolidTexture(2, 139, 69, 19);
            textureNameToIndex["dirt"] = 2;
            loadedTextures.push_back("default_dirt");
            nextAvailableIndex = std::max(nextAvailableIndex, static_cast<uint16_t>(3));
        }

        if (nextAvailableIndex < MAX_TILES) {
            // Index 3: Grass top texture (green)
            CreateSolidTexture(3, 34, 139, 34);
            textureNameToIndex["grass_top"] = 3;
            loadedTextures.push_back("default_grass_top");
            nextAvailableIndex = std::max(nextAvailableIndex, static_cast<uint16_t>(4));
        }

        if (nextAvailableIndex < MAX_TILES) {
            // Index 4: Grass side texture (green-brown gradient effect)
            CreateSolidTexture(4, 107, 142, 35);
            textureNameToIndex["grass_side"] = 4;
            loadedTextures.push_back("default_grass_side");
            nextAvailableIndex = std::max(nextAvailableIndex, static_cast<uint16_t>(5));
        }
    }

    bool TextureAtlas::LoadTextureToAtlas(const std::string& filePath, uint16_t atlasIndex) {
        int width, height, channels;
        unsigned char* data = stbi_load(filePath.c_str(), &width, &height, &channels, STBI_rgb_alpha);

        if (!data) {
            Log::Error("Failed to load texture: %s - %s", filePath.c_str(), stbi_failure_reason());
            CreateErrorTexture(atlasIndex); // Create error texture as fallback
            return false;
        }

        // Validate dimensions
        if (width != TILE_SIZE || height != TILE_SIZE) {
            Log::Warning("Texture %s is %dx%d, expected %dx%d. Resizing may cause quality loss.",
                        filePath.c_str(), width, height, TILE_SIZE, TILE_SIZE);
        }

        // Copy to atlas (this will handle resizing if needed)
        CopyTileToAtlas(data, atlasIndex, width, height);

        stbi_image_free(data);
        return true;
    }

    void TextureAtlas::CreateErrorTexture(uint16_t atlasIndex) {
        // Create a magenta/black checkerboard pattern for missing textures
        std::vector<unsigned char> errorData(TILE_SIZE * TILE_SIZE * 4);

        for (int y = 0; y < TILE_SIZE; ++y) {
            for (int x = 0; x < TILE_SIZE; ++x) {
                int idx = (y * TILE_SIZE + x) * 4;
                bool isEven = ((x / 2) + (y / 2)) % 2 == 0;

                if (isEven) {
                    // Magenta
                    errorData[idx + 0] = 255; // R
                    errorData[idx + 1] = 0;   // G
                    errorData[idx + 2] = 255; // B
                    errorData[idx + 3] = 255; // A
                } else {
                    // Black
                    errorData[idx + 0] = 0;   // R
                    errorData[idx + 1] = 0;   // G
                    errorData[idx + 2] = 0;   // B
                    errorData[idx + 3] = 255; // A
                }
            }
        }

        CopyTileToAtlas(errorData.data(), atlasIndex, TILE_SIZE, TILE_SIZE);
    }

    void TextureAtlas::CreateSolidTexture(uint16_t atlasIndex, unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
        std::vector<unsigned char> solidData(TILE_SIZE * TILE_SIZE * 4);

        for (int i = 0; i < TILE_SIZE * TILE_SIZE; ++i) {
            solidData[i * 4 + 0] = r;
            solidData[i * 4 + 1] = g;
            solidData[i * 4 + 2] = b;
            solidData[i * 4 + 3] = a;
        }

        CopyTileToAtlas(solidData.data(), atlasIndex, TILE_SIZE, TILE_SIZE);
    }

    void TextureAtlas::CopyTileToAtlas(const unsigned char* tileData, uint16_t atlasIndex, int tileWidth, int tileHeight) {
        int atlasX, atlasY;
        GetAtlasCoords(atlasIndex, atlasX, atlasY);

        // Simple nearest-neighbor sampling if size mismatch
        for (int y = 0; y < TILE_SIZE; ++y) {
            for (int x = 0; x < TILE_SIZE; ++x) {
                // Map from atlas tile coords to source texture coords
                int srcX = (x * tileWidth) / TILE_SIZE;
                int srcY = (y * tileHeight) / TILE_SIZE;

                // Clamp to source bounds
                srcX = std::min(srcX, tileWidth - 1);
                srcY = std::min(srcY, tileHeight - 1);

                // Calculate indices
                int atlasIdx = ((atlasY + y) * ATLAS_WIDTH + (atlasX + x)) * 4;
                int srcIdx = (srcY * tileWidth + srcX) * 4;

                // Copy RGBA
                atlasData[atlasIdx + 0] = tileData[srcIdx + 0]; // R
                atlasData[atlasIdx + 1] = tileData[srcIdx + 1]; // G
                atlasData[atlasIdx + 2] = tileData[srcIdx + 2]; // B
                atlasData[atlasIdx + 3] = tileData[srcIdx + 3]; // A
            }
        }
    }

    void TextureAtlas::GetAtlasCoords(uint16_t atlasIndex, int& x, int& y) const {
        x = (atlasIndex % TILES_PER_ROW) * TILE_SIZE;
        y = (atlasIndex / TILES_PER_ROW) * TILE_SIZE;
    }

    void TextureAtlas::UploadToGPU() {
        glBindTexture(GL_TEXTURE_2D, textureID);

        // Upload the atlas data (256x1024)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ATLAS_WIDTH, ATLAS_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, atlasData.data());

        // Set texture parameters
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); // Pixel-perfect for voxels
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Generate mipmaps for distant chunks (optional)
        glGenerateMipmap(GL_TEXTURE_2D);

        glBindTexture(GL_TEXTURE_2D, 0);

        Log::Info("Uploaded %dx%d texture atlas to GPU (Texture ID: %u)", ATLAS_WIDTH, ATLAS_HEIGHT, textureID);
    }

    AtlasTile TextureAtlas::GetTile(uint16_t atlasIndex) const {
        if (atlasIndex >= MAX_TILES) {
            Log::Warning("Atlas index %d out of range (max %d), using index 1008 (air)", atlasIndex, MAX_TILES - 1);
            atlasIndex = 1008; // Return air texture instead of index 0
        }

        // Calculate UV coordinates for 16x64 grid
        int tileX = atlasIndex % TILES_PER_ROW;    // 0-15
        int tileY = atlasIndex / TILES_PER_ROW;    // 0-63

        float uvPerTileX = 1.0f / static_cast<float>(TILES_PER_ROW);     // 1/16 = 0.0625
        float uvPerTileY = 1.0f / static_cast<float>(TILES_PER_COLUMN);  // 1/64 = 0.015625

        glm::vec2 uvMin(
            static_cast<float>(tileX) * uvPerTileX,
            static_cast<float>(tileY) * uvPerTileY
        );

        glm::vec2 uvMax(
            static_cast<float>(tileX + 1) * uvPerTileX,
            static_cast<float>(tileY + 1) * uvPerTileY
        );

        return AtlasTile(uvMin, uvMax);
    }

    void TextureAtlas::Bind(GLenum textureUnit) const {
        glActiveTexture(textureUnit);
        glBindTexture(GL_TEXTURE_2D, textureID);
    }

    uint16_t TextureAtlas::RegisterTexture(const std::string& name, const unsigned char* data, int width, int height) {
        if (nextAvailableIndex >= MAX_TILES) {
            Log::Warning("Atlas is full, cannot register texture: %s", name.c_str());
            return 1008; // Return air index instead of 0
        }

        uint16_t index = nextAvailableIndex++;
        CopyTileToAtlas(data, index, width, height);
        
        textureNameToIndex[name] = index;
        loadedTextures.push_back(name);
        
        // Re-upload to GPU
        UploadToGPU();
        
        Log::Info("Registered dynamic texture '%s' at atlas index %d", name.c_str(), index);
        return index;
    }

} // namespace Render
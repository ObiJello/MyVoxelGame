// File: src/client/renderer/texture/TextureAnimator.hpp
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <glad/glad.h>
#include "AtlasBuilder.hpp"

namespace Render {

    // Stores all frames for an animated texture
    struct AnimatedTexture {
        std::string textureKey;            // e.g. "block/water_still"
        TextureAnimation animation;        // Animation metadata
        std::vector<std::vector<unsigned char>> frameData; // RGBA data for each frame
        
        // Atlas position where this texture is located
        int atlasX = 0;
        int atlasY = 0;
        
        // Runtime animation state
        int currentFrame = 0;              // Current frame index
        float timer = 0.0f;                // Animation timer
        
        AnimatedTexture() = default;
    };

    class TextureAnimator {
    public:
        TextureAnimator();
        ~TextureAnimator();

        // Initialize with the atlas texture ID
        void Initialize(GLuint atlasTextureID);

        // Register an animated texture with its frame data
        void RegisterAnimatedTexture(const std::string& textureKey,
                                   const TextureAnimation& animation,
                                   const std::vector<std::vector<unsigned char>>& frames,
                                   int atlasX, int atlasY);

        // Update all animations - call this every frame/tick
        void UpdateAnimations(float deltaTime);

        // Check if a texture is animated
        bool IsAnimated(const std::string& textureKey) const;

        // Get current frame index for a texture
        int GetCurrentFrame(const std::string& textureKey) const;

        // Get animation info (for debugging)
        const TextureAnimation* GetAnimation(const std::string& textureKey) const;

        // Get statistics
        size_t GetAnimatedTextureCount() const { return animatedTextures.size(); }

        // Enable/disable animation updates
        void SetAnimationEnabled(bool enabled) { animationEnabled = enabled; }
        bool IsAnimationEnabled() const { return animationEnabled; }

    private:
        GLuint atlasTextureID;
        bool animationEnabled;
        
        // Map from texture key to animated texture data
        std::unordered_map<std::string, std::unique_ptr<AnimatedTexture>> animatedTextures;

        // Update a single texture's animation
        void UpdateSingleAnimation(AnimatedTexture& animTex, float deltaTime);

        // Upload frame data to atlas using glTexSubImage2D
        void UploadFrameToAtlas(const AnimatedTexture& animTex, int frameIndex);
    };

    // Global texture animator instance
    extern std::unique_ptr<TextureAnimator> g_textureAnimator;

} // namespace Render
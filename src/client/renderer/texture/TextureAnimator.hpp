// File: src/client/renderer/texture/TextureAnimator.hpp
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include "../backend/RenderTypes.hpp"
#include "AtlasBuilder.hpp"

namespace Render {

    // Stores all frames for an animated texture
    struct AnimatedTexture {
        std::string textureKey;
        TextureAnimation animation;
        std::vector<std::vector<unsigned char>> frameData;

        // Atlas position where this texture is located
        int atlasX = 0;
        int atlasY = 0;

        // Runtime animation state
        int currentFrame = 0;
        float timer = 0.0f;
    };

    class TextureAnimator {
    public:
        TextureAnimator();
        ~TextureAnimator();

        // Initialize with the atlas texture handle
        void Initialize(TextureHandle atlasTexture);

        // Register an animated texture with its frame data
        void RegisterAnimatedTexture(const std::string& textureKey,
                                   const TextureAnimation& animation,
                                   const std::vector<std::vector<unsigned char>>& frames,
                                   int atlasX, int atlasY);

        // Update all animations - call this every frame/tick
        void UpdateAnimations(float deltaTime);

        bool IsAnimated(const std::string& textureKey) const;
        int GetCurrentFrame(const std::string& textureKey) const;
        const TextureAnimation* GetAnimation(const std::string& textureKey) const;

        size_t GetAnimatedTextureCount() const { return animatedTextures.size(); }
        void SetAnimationEnabled(bool enabled) { animationEnabled = enabled; }
        bool IsAnimationEnabled() const { return animationEnabled; }

    private:
        TextureHandle m_atlasTexture = INVALID_TEXTURE;
        bool animationEnabled;

        std::unordered_map<std::string, std::unique_ptr<AnimatedTexture>> animatedTextures;

        void UpdateSingleAnimation(AnimatedTexture& animTex, float deltaTime);
        void UploadFrameToAtlas(const AnimatedTexture& animTex, int frameIndex);
    };

    extern std::unique_ptr<TextureAnimator> g_textureAnimator;

} // namespace Render

// File: src/client/renderer/texture/TextureAnimator.cpp
#include "TextureAnimator.hpp"
#include "AtlasBuilder.hpp"
#include "common/core/Log.hpp"

namespace Render {

    // Global instance
    std::unique_ptr<TextureAnimator> g_textureAnimator = nullptr;

    TextureAnimator::TextureAnimator()
        : atlasTextureID(0)
        , animationEnabled(true) {
    }

    TextureAnimator::~TextureAnimator() {
        // Clean up is automatic with smart pointers
    }

    void TextureAnimator::Initialize(GLuint textureID) {
        atlasTextureID = textureID;
        Log::Info("TextureAnimator initialized with atlas texture ID: %u", textureID);
    }

    void TextureAnimator::RegisterAnimatedTexture(const std::string& textureKey,
                                                const TextureAnimation& animation,
                                                const std::vector<std::vector<unsigned char>>& frames,
                                                int atlasX, int atlasY) {
        
        auto animTex = std::make_unique<AnimatedTexture>();
        animTex->textureKey = textureKey;
        animTex->animation = animation;
        animTex->frameData = frames;
        animTex->atlasX = atlasX;
        animTex->atlasY = atlasY;

        // Initialize animation state
        animTex->currentFrame = 0;
        animTex->timer = 0.0f;

        // If no custom frame sequence specified, use default 0,1,2,3...
        if (animTex->animation.frames.empty()) {
            for (int i = 0; i < animTex->animation.frameCount; ++i) {
                animTex->animation.frames.push_back(i);
            }
        }

        Log::Info("Registered animated texture: %s (%d frames, frametime: %d, atlas pos: %d,%d)",
                 textureKey.c_str(), animTex->animation.frameCount, 
                 animTex->animation.frametime, atlasX, atlasY);

        // Upload initial frame (frame 0)
        if (!frames.empty()) {
            UploadFrameToAtlas(*animTex, 0);
        }

        animatedTextures[textureKey] = std::move(animTex);
    }

    void TextureAnimator::UpdateAnimations(float deltaTime) {
        if (!animationEnabled || atlasTextureID == 0) {
            return;
        }

        // Convert deltaTime to tick-based time
        // Assuming 20 ticks per second like Minecraft
        const float TICKS_PER_SECOND = 20.0f;
        float tickDelta = deltaTime * TICKS_PER_SECOND;

        for (auto& pair : animatedTextures) {
            UpdateSingleAnimation(*pair.second, tickDelta);
        }
    }

    void TextureAnimator::UpdateSingleAnimation(AnimatedTexture& animTex, float deltaTime) {
        const TextureAnimation& anim = animTex.animation;

        // Advance timer
        animTex.timer += deltaTime;

        // Check if we should advance to next frame
        if (animTex.timer >= anim.frametime) {
            animTex.timer -= anim.frametime;

            // Advance to next frame in sequence
            int sequenceIndex = animTex.currentFrame;
            sequenceIndex = (sequenceIndex + 1) % anim.frames.size();
            animTex.currentFrame = sequenceIndex;

            // Get the actual frame number from the sequence
            int actualFrame = anim.frames[sequenceIndex];

            // Upload new frame to atlas
            if (actualFrame >= 0 && actualFrame < animTex.frameData.size()) {
                UploadFrameToAtlas(animTex, actualFrame);
            }
        }
    }

    void TextureAnimator::UploadFrameToAtlas(const AnimatedTexture& animTex, int frameIndex) {
        if (frameIndex < 0 || frameIndex >= animTex.frameData.size()) {
            return;
        }

        const auto& frameData = animTex.frameData[frameIndex];
        const TextureAnimation& anim = animTex.animation;

        // Bind atlas texture
        glBindTexture(GL_TEXTURE_2D, atlasTextureID);

        // Upload frame data to the correct atlas position
        glTexSubImage2D(GL_TEXTURE_2D, 0,
                       animTex.atlasX, animTex.atlasY,  // Atlas position
                       anim.width, anim.height,         // Frame dimensions
                       GL_RGBA, GL_UNSIGNED_BYTE,       // Format
                       frameData.data());               // Frame data

        glBindTexture(GL_TEXTURE_2D, 0);

        // Debug logging (can be disabled for performance)
        static int logCounter = 0;
        if (++logCounter % 100 == 0) {  // Log every 100th update
            /*Log::Debug("Updated %s frame %d at atlas pos (%d,%d)",
                      animTex.textureKey.c_str(), frameIndex,
                      animTex.atlasX, animTex.atlasY);*/
        }
    }

    bool TextureAnimator::IsAnimated(const std::string& textureKey) const {
        return animatedTextures.find(textureKey) != animatedTextures.end();
    }

    int TextureAnimator::GetCurrentFrame(const std::string& textureKey) const {
        auto it = animatedTextures.find(textureKey);
        if (it != animatedTextures.end()) {
            const AnimatedTexture& animTex = *it->second;
            const TextureAnimation& anim = animTex.animation;
            if (animTex.currentFrame < anim.frames.size()) {
                return anim.frames[animTex.currentFrame];
            }
        }
        return 0;
    }

    const TextureAnimation* TextureAnimator::GetAnimation(const std::string& textureKey) const {
        auto it = animatedTextures.find(textureKey);
        if (it != animatedTextures.end()) {
            return &it->second->animation;
        }
        return nullptr;
    }

} // namespace Render
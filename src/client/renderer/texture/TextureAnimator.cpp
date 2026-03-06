// File: src/client/renderer/texture/TextureAnimator.cpp
#include "TextureAnimator.hpp"
#include "../backend/RenderBackend.hpp"
#include "common/core/Log.hpp"

namespace Render {

    std::unique_ptr<TextureAnimator> g_textureAnimator = nullptr;

    TextureAnimator::TextureAnimator()
        : animationEnabled(true) {
    }

    TextureAnimator::~TextureAnimator() = default;

    void TextureAnimator::Initialize(TextureHandle atlasTexture) {
        m_atlasTexture = atlasTexture;
        Log::Info("TextureAnimator initialized with atlas texture handle: %u", atlasTexture);
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
        animTex->currentFrame = 0;
        animTex->timer = 0.0f;

        if (animTex->animation.frames.empty()) {
            for (int i = 0; i < animTex->animation.frameCount; ++i) {
                animTex->animation.frames.push_back(i);
            }
        }

        Log::Info("Registered animated texture: %s (%d frames, frametime: %d, atlas pos: %d,%d)",
                 textureKey.c_str(), animTex->animation.frameCount,
                 animTex->animation.frametime, atlasX, atlasY);

        if (!frames.empty()) {
            UploadFrameToAtlas(*animTex, 0);
        }

        animatedTextures[textureKey] = std::move(animTex);
    }

    void TextureAnimator::UpdateAnimations(float deltaTime) {
        if (!animationEnabled || m_atlasTexture == INVALID_TEXTURE) return;

        const float TICKS_PER_SECOND = 20.0f;
        float tickDelta = deltaTime * TICKS_PER_SECOND;

        for (auto& pair : animatedTextures) {
            UpdateSingleAnimation(*pair.second, tickDelta);
        }
    }

    void TextureAnimator::UpdateSingleAnimation(AnimatedTexture& animTex, float deltaTime) {
        const TextureAnimation& anim = animTex.animation;
        animTex.timer += deltaTime;

        if (animTex.timer >= anim.frametime) {
            animTex.timer -= anim.frametime;

            int sequenceIndex = (animTex.currentFrame + 1) % static_cast<int>(anim.frames.size());
            animTex.currentFrame = sequenceIndex;

            int actualFrame = anim.frames[sequenceIndex];
            if (actualFrame >= 0 && actualFrame < static_cast<int>(animTex.frameData.size())) {
                UploadFrameToAtlas(animTex, actualFrame);
            }
        }
    }

    void TextureAnimator::UploadFrameToAtlas(const AnimatedTexture& animTex, int frameIndex) {
        if (frameIndex < 0 || frameIndex >= static_cast<int>(animTex.frameData.size())) return;
        if (m_atlasTexture == INVALID_TEXTURE || !g_renderBackend) return;

        const auto& frameData = animTex.frameData[frameIndex];
        const TextureAnimation& anim = animTex.animation;

        g_renderBackend->UpdateTexture2D(m_atlasTexture,
            animTex.atlasX, animTex.atlasY,
            anim.width, anim.height,
            frameData.data());
    }

    bool TextureAnimator::IsAnimated(const std::string& textureKey) const {
        return animatedTextures.find(textureKey) != animatedTextures.end();
    }

    int TextureAnimator::GetCurrentFrame(const std::string& textureKey) const {
        auto it = animatedTextures.find(textureKey);
        if (it != animatedTextures.end()) {
            const AnimatedTexture& animTex = *it->second;
            if (animTex.currentFrame < static_cast<int>(animTex.animation.frames.size())) {
                return animTex.animation.frames[animTex.currentFrame];
            }
        }
        return 0;
    }

    const TextureAnimation* TextureAnimator::GetAnimation(const std::string& textureKey) const {
        auto it = animatedTextures.find(textureKey);
        return (it != animatedTextures.end()) ? &it->second->animation : nullptr;
    }

} // namespace Render

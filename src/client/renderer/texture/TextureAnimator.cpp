// File: src/client/renderer/texture/TextureAnimator.cpp
#include "TextureAnimator.hpp"
#include "../backend/RenderBackend.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace Render {

    namespace {
        // MC's client tick rate, and DeltaTracker's cap on ticks run per frame.
        constexpr float kTicksPerSecond = 20.0f;
        constexpr int   kMaxTicksPerFrame = 10;
    }

    TextureAnimator::TextureAnimator()
        : animationEnabled(true) {
    }

    TextureAnimator::~TextureAnimator() = default;

    SpriteUploadMode TextureAnimator::Mode() {
        static const SpriteUploadMode mode = [] {
            const bool vulkan = g_renderBackend && g_renderBackend->GetType() == BackendType::Vulkan;
            SpriteUploadMode m = vulkan ? SpriteUploadMode::Cpu : SpriteUploadMode::Staged;
            const char* legacy = std::getenv("OBEY_CPU_SPRITE_ANIM");
            if (legacy && std::strcmp(legacy, "0") != 0) m = SpriteUploadMode::Cpu;
            if (const char* v = std::getenv("OBEY_SPRITE_ANIM")) {
                if (std::strcmp(v, "cpu") == 0)      m = SpriteUploadMode::Cpu;
                else if (std::strcmp(v, "pbo") == 0) m = SpriteUploadMode::Staged;
                else Log::Warning("[TexAnim] OBEY_SPRITE_ANIM='%s' unknown (cpu, pbo)", v);
            }
            Log::Info("[TexAnim] animated sprites reach the atlas via '%s'",
                      m == SpriteUploadMode::Cpu ? "cpu" : "pbo");
            return m;
        }();
        return mode;
    }

    void TextureAnimator::Initialize(TextureHandle atlasTexture) {
        m_atlasTexture = atlasTexture;
        // A rebuilt atlas (resource pack reload) registers its animations
        // afresh; the previous atlas's entries would draw into freed memory
        // at old offsets.
        animatedTextures.clear();
        m_tickAccumulator = 0.0f;
        Log::Info("TextureAnimator initialized with atlas texture handle: %u", atlasTexture);
    }

    // ── Registration ────────────────────────────────────────────────────────

    void TextureAnimator::RegisterAnimatedTexture(const std::string& textureKey,
                                                  const TextureAnimation& animation,
                                                  const std::vector<std::vector<unsigned char>>& frames,
                                                  int atlasX, int atlasY, int padding,
                                                  const std::string& mipmapStrategy,
                                                  float alphaCutoffBias,
                                                  int mipLevels) {
        if (frames.empty() || animation.width <= 0 || animation.height <= 0) return;
        PROFILE_ZONE_N("TexAnim.Register");

        auto anim = std::make_unique<AnimatedTexture>();
        anim->textureKey = textureKey;
        anim->animation = animation;
        anim->atlasX = atlasX;
        anim->atlasY = atlasY;
        anim->padding = padding;
        if (anim->animation.frames.empty()) {
            for (int i = 0; i < anim->animation.frameCount; ++i) anim->animation.frames.push_back(i);
        }

        // Every frame's chain, built the way the atlas build builds a still
        // sprite's (MC SpriteContents keeps byMipLevel for the whole sheet).
        const Mipmap::Strategy strategy = Mipmap::ParseStrategy(mipmapStrategy);
        const bool isItem = textureKey.rfind("item/", 0) == 0 ||
                            textureKey.find(":item/") != std::string::npos;
        const size_t frameBytes = static_cast<size_t>(animation.width) * animation.height * 4u;
        anim->levels = std::max(0, mipLevels) + 1;
        anim->frameChains.reserve(frames.size());
        for (const auto& pixels : frames) {
            Mipmap::Image level0;
            level0.width = animation.width;
            level0.height = animation.height;
            level0.pixels = pixels;
            if (level0.pixels.size() != frameBytes) level0.pixels.resize(frameBytes, 0);
            std::vector<Mipmap::Image> chain;
            if (mipLevels > 0) {
                chain = Mipmap::GenerateMipLevels(std::move(level0), mipLevels, strategy, alphaCutoffBias, isItem);
            } else {
                chain.push_back(std::move(level0));
            }
            // A size that cannot halve all the way stops the chain early.
            anim->levels = std::min(anim->levels, static_cast<int>(chain.size()));
            anim->frameChains.push_back(std::move(chain));
        }
        if (anim->levels <= 0) return;

        Log::Info("Registered animated texture: %s (%zu frames, frametime: %d%s, atlas pos: %d,%d)",
                  textureKey.c_str(), anim->frameChains.size(), animation.frametime,
                  animation.interpolate ? ", interpolated" : "", atlasX, atlasY);

        // isDirty / drawPending start true: the next UpdateAnimations draws
        // the initial state (MC uploadInitialContents -> uploadAnimationFrames).
        animatedTextures[textureKey] = std::move(anim);
    }

    // ── MC AnimationState ───────────────────────────────────────────────────

    int TextureAnimator::FrameIndex(const AnimatedTexture& anim, int entry) {
        const auto& list = anim.animation.frames;
        if (list.empty()) return 0;
        const int index = list[static_cast<size_t>(entry) % list.size()];
        return std::clamp(index, 0, static_cast<int>(anim.frameChains.size()) - 1);
    }

    int TextureAnimator::FrameTime(const AnimatedTexture& anim, int entry) {
        // MC FrameInfo.time: the frame's own "time", else the section's
        // frametime (at least one tick).
        const auto& times = anim.animation.frameTimes;
        const int t = entry < static_cast<int>(times.size()) ? times[static_cast<size_t>(entry)] : 0;
        return t > 0 ? t : std::max(1, anim.animation.frametime);
    }

    void TextureAnimator::Tick(AnimatedTexture& anim) {
        const int count = static_cast<int>(anim.animation.frames.size());
        if (count == 0) return;
        ++anim.subFrame;
        anim.isDirty = false;
        if (anim.subFrame >= FrameTime(anim, anim.frame)) {
            const int oldFrame = FrameIndex(anim, anim.frame);
            anim.frame = (anim.frame + 1) % count;
            anim.subFrame = 0;
            if (oldFrame != FrameIndex(anim, anim.frame)) anim.isDirty = true;
        }
    }

    void TextureAnimator::UpdateAnimations(float deltaTime) {
        if (!animationEnabled || m_atlasTexture == INVALID_TEXTURE || animatedTextures.empty()) return;

        // MC runs cycleAnimationFrames once per client tick.
        m_tickAccumulator += std::max(0.0f, deltaTime) * kTicksPerSecond;
        int ticks = static_cast<int>(m_tickAccumulator);
        m_tickAccumulator -= static_cast<float>(ticks);
        ticks = std::min(ticks, kMaxTicksPerFrame);

        std::vector<AnimatedTexture*> toDraw;
        for (auto& [key, animPtr] : animatedTextures) {
            AnimatedTexture& anim = *animPtr;
            for (int t = 0; t < ticks; ++t) {
                Tick(anim);
                if (NeedsToDraw(anim)) anim.drawPending = true;
            }
            if (anim.drawPending) {
                toDraw.push_back(&anim);
                anim.drawPending = false;
            }
        }
        if (toDraw.empty()) return;
        PROFILE_ZONE_N("TexAnim.Draw");
        for (const AnimatedTexture* anim : toDraw) UploadSprite(*anim);
    }

    // ── Drawing ─────────────────────────────────────────────────────────────

    void TextureAnimator::UploadSprite(const AnimatedTexture& anim) {
        // MC's animate_sprite pixels, made here: the current frame (blended
        // toward the next for an interpolated sprite, as the shader's mix and
        // its UNORM write round), with the padding ring filled by clamping to
        // the edge texels — the same texels MC's clamped sampler picks.
        const bool staged = Mode() == SpriteUploadMode::Staged;
        const TextureHandle target = m_atlasTexture;
        PROFILE_ZONE_N("AnimFrameUpload");
        const int current = FrameIndex(anim, anim.frame);
        int next = current;
        int progress = 0;   // thousandths (MC passes the progress as (int)(p * 1000))
        if (anim.animation.interpolate) {
            const int count = static_cast<int>(anim.animation.frames.size());
            next = FrameIndex(anim, (anim.frame + 1) % std::max(1, count));
            const float p = static_cast<float>(anim.subFrame) / static_cast<float>(FrameTime(anim, anim.frame));
            progress = static_cast<int>(p * 1000.0f);
        }
        const bool blend = next != current && progress > 0;

        static thread_local std::vector<uint8_t> blended, padded;
        for (int level = 0; level < anim.levels; ++level) {
            const Mipmap::Image& cur = anim.frameChains[static_cast<size_t>(current)][static_cast<size_t>(level)];
            const int lw = cur.width, lh = cur.height;
            const size_t rowBytes = static_cast<size_t>(lw) * 4u;

            // The interior: the frame, or mix(cur, next, t) rounded to UNORM.
            // round((a(1000-p) + b p) / 1000) exactly, in integers — the
            // shader's float mix and round-to-nearest write, without a float
            // or a lround per channel.
            const uint8_t* interior = cur.pixels.data();
            if (blend) {
                const Mipmap::Image& nxt = anim.frameChains[static_cast<size_t>(next)][static_cast<size_t>(level)];
                const size_t n = rowBytes * static_cast<size_t>(lh);
                blended.resize(n);
                const uint32_t wb = static_cast<uint32_t>(progress), wa = 1000u - wb;
                const uint8_t* a = cur.pixels.data();
                const uint8_t* b = nxt.pixels.data();
                for (size_t i = 0; i < n; ++i) {
                    blended[i] = static_cast<uint8_t>((a[i] * wa + b[i] * wb + 500u) / 1000u);
                }
                interior = blended.data();
            }

            // The padding ring: each row's edge texels repeated outward, the
            // first and last rows repeated up and down.
            const int pad = anim.padding >> level;
            const int pw = lw + 2 * pad, ph = lh + 2 * pad;
            const size_t paddedRow = static_cast<size_t>(pw) * 4u;
            padded.resize(paddedRow * static_cast<size_t>(ph));
            for (int y = 0; y < lh; ++y) {
                const uint8_t* src = interior + static_cast<size_t>(y) * rowBytes;
                uint8_t* row = padded.data() + static_cast<size_t>(y + pad) * paddedRow;
                for (int x = 0; x < pad; ++x) std::memcpy(row + static_cast<size_t>(x) * 4u, src, 4);
                std::memcpy(row + static_cast<size_t>(pad) * 4u, src, rowBytes);
                const uint8_t* last = src + rowBytes - 4u;
                for (int x = pad + lw; x < pw; ++x) std::memcpy(row + static_cast<size_t>(x) * 4u, last, 4);
            }
            for (int y = 0; y < pad; ++y) {
                std::memcpy(padded.data() + static_cast<size_t>(y) * paddedRow,
                            padded.data() + static_cast<size_t>(pad) * paddedRow, paddedRow);
                std::memcpy(padded.data() + static_cast<size_t>(pad + lh + y) * paddedRow,
                            padded.data() + static_cast<size_t>(pad + lh - 1) * paddedRow, paddedRow);
            }

            const int x = (anim.atlasX >> level) - pad, y = (anim.atlasY >> level) - pad;
            if (staged) g_renderBackend->UpdateTexture2DLevelStaged(target, level, x, y, pw, ph, padded.data());
            else        g_renderBackend->UpdateTexture2DLevel(target, level, x, y, pw, ph, padded.data());
        }
    }

    // ── Queries ─────────────────────────────────────────────────────────────

    bool TextureAnimator::IsAnimated(const std::string& textureKey) const {
        return animatedTextures.find(textureKey) != animatedTextures.end();
    }

    int TextureAnimator::GetCurrentFrame(const std::string& textureKey) const {
        auto it = animatedTextures.find(textureKey);
        return it != animatedTextures.end() ? FrameIndex(*it->second, it->second->frame) : 0;
    }

    const TextureAnimation* TextureAnimator::GetAnimation(const std::string& textureKey) const {
        auto it = animatedTextures.find(textureKey);
        return (it != animatedTextures.end()) ? &it->second->animation : nullptr;
    }

} // namespace Render

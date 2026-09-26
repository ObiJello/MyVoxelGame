// File: src/client/renderer/texture/TextureAnimator.hpp
//
// MC's animated atlas sprites (SpriteContents.AnimatedTexture/AnimationState
// + TextureAtlas.cycleAnimationFrames/uploadAnimationFrames).
//
// Each animated sprite keeps every frame of its sheet with the frame's mip
// chain, built once at registration. Once per client tick the animation
// states advance exactly as MC's AnimationState.tick, and every sprite that
// changed has its pixels written into the atlas, every level, the padding
// ring included. "interpolate": true sprites (magma, prismarine, sculk, the
// stems, campfire logs, command blocks...) blend the current and next frame
// by the frame's progress and are rewritten every tick, as MC's
// animate_sprite_interpolate does.
//
// MC draws those pixels into the atlas on the GPU. That was built and
// measured here (2026-09-25 replay A/B): +0.7 ms per tick frame on Vulkan
// and a freeze while generating, +5 ms on OpenGL — writing a texture that
// queued frames still sample. So the pixels are made on the CPU (the same
// pixels: frames, integer blend of the shader's mix, clamped padding) and
// only the changed rectangles are written; OBEY_SPRITE_ANIM picks how:
//   cpu   UpdateTexture2DLevel — Vulkan stages it into the next frame
//         (default there); on OpenGL a plain glTexSubImage2D, which Apple's
//         driver makes the CPU wait on (+8.6 ms per tick frame measured)
//   pbo   UpdateTexture2DLevelStaged — OpenGL copies through a rotating
//         pixel-unpack buffer, so the copy is the GPU's (default on OpenGL;
//         +1.8 ms per tick frame, the best of gpu / cpu / pbo / an atlas
//         ring measured)
// OBEY_CPU_SPRITE_ANIM=1 still means cpu.
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../backend/RenderTypes.hpp"
#include "AtlasBuilder.hpp"
#include "MipmapGenerator.hpp"

namespace Render {

    // One animated sprite: its frames and MC's AnimationState.
    struct AnimatedTexture {
        std::string textureKey;
        TextureAnimation animation;

        // Where the sprite sits in the atlas: its interior's top-left in
        // level 0, and the padding ring around it (AtlasBuilder::GetPadding()).
        int atlasX = 0;
        int atlasY = 0;
        int padding = 0;

        // Every frame of the sheet, level by level: frameChains[frame][level].
        // Built once, with the sprite's .mcmeta mipmap strategy, so a frame
        // carries the same chain the atlas build would give it.
        std::vector<std::vector<Mipmap::Image>> frameChains;
        int levels = 1;                          // levels every frame has (chain length)

        // MC AnimationState: the entry in animation.frames, the ticks spent
        // on it, and whether the last tick moved to a different frame.
        int  frame = 0;
        int  subFrame = 0;
        bool isDirty = true;                     // MC: drawn once at creation
        // Some tick since the last draw asked for one (NeedsToDraw). MC draws
        // after every tick; with several ticks in one frame only the last
        // state is visible, so it is drawn once — but a frame change on an
        // earlier tick of the batch must not be lost.
        bool drawPending = true;
    };

    enum class SpriteUploadMode { Cpu, Staged };

    class TextureAnimator {
    public:
        TextureAnimator();
        ~TextureAnimator();

        // OBEY_SPRITE_ANIM, resolved against the backend (read once).
        static SpriteUploadMode Mode();

        // A (re)built atlas: drops the previous atlas's sprites — they would
        // write into freed memory at old offsets.
        void Initialize(TextureHandle atlasTexture);

        // `frames` are the sheet's frames in order, animation.width x height
        // each. `mipLevels` 0 = the atlas has no chain (level 0 only).
        void RegisterAnimatedTexture(const std::string& textureKey,
                                     const TextureAnimation& animation,
                                     const std::vector<std::vector<unsigned char>>& frames,
                                     int atlasX, int atlasY, int padding,
                                     const std::string& mipmapStrategy = "",
                                     float alphaCutoffBias = 0.0f,
                                     int mipLevels = 0);

        // Once per rendered frame: runs the client ticks `deltaTime` covers
        // (MC's cycleAnimationFrames per tick, at most 10 per call like MC's
        // tick catch-up), then draws every sprite that changed.
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
        float m_tickAccumulator = 0.0f;
        std::unordered_map<std::string, std::unique_ptr<AnimatedTexture>> animatedTextures;

        // The sprite's current pixels into the atlas, every level.
        void UploadSprite(const AnimatedTexture& anim);

        // MC FrameInfo accessors over animation.frames / frameTimes.
        static int FrameIndex(const AnimatedTexture& anim, int entry);
        static int FrameTime(const AnimatedTexture& anim, int entry);
        // MC AnimationState.tick.
        static void Tick(AnimatedTexture& anim);
        static bool NeedsToDraw(const AnimatedTexture& anim) {
            return anim.animation.interpolate || anim.isDirty;
        }
    };

} // namespace Render

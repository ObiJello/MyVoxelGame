// File: src/client/renderer/post/PostChain.hpp
//
// MC's post-effect system — PostChain / PostChainConfig / PostPass and the
// part of GameRenderer that picks and runs them (preparePostEffects,
// applyPostEffects, checkEntityPostEffect) — on both backends.
//
// A chain is a post_effect JSON (assets/post_effect/<id>.json, resource-pack
// overridable, the vanilla files verbatim): named internal targets and an
// ordered list of passes, each a full-screen quad through a vertex +
// fragment shader, reading named targets through named samplers and writing
// one target. "minecraft:main" is the frame itself: read, it is a copy of
// the frame as it stands (RenderBackend::CopyFramebufferToRenderTarget);
// written, the pass draws straight onto the frame.
//
// Shaders: "<ns>:<dir>/<name>" -> shaders/<dir == core ? "post" : dir>_<name>
// .vert/.frag (and their _vk.*.spv twins) — post_screenquad, post_rotscale,
// post_blit, post_bits, post_box_blur, post_color_convolve, post_invert,
// post_spiderclip. A pass's config uniforms (MC's std140 blocks, in the
// JSON's order, each block starting on a vec4) are packed into sixteen
// floats — the shaders read them as uPostParams' four columns — and
// SamplerInfo is uInSize / uOutSize. The sampler named "In" is texture
// slot 0, any other the one extra slot 1. Every target is linear-filtered
// and clamped; an input not marked "bilinear" is sampled at texel centres
// in the shader, which is nearest filtering without a second sampler.
//
// Frame overlap (CLAUDE.md, Vulkan): every target — the internal ones and
// the copy of main — exists twice and consecutive runs alternate between
// the two sets, so no run writes an image the previous frame may still be
// reading.
//
// Where it runs: GameRenderer.render applies the post effects after
// renderLevel (the hand included) and the entity-outline blit, before the
// GUI — PlatformMain calls PostEffects::Apply right after
// EntityOutline::Composite. Spectating a creeper, spider (cave spiders
// included) or enderman selects creeper / spider / invert
// (checkEntityPostEffect); anything else, or your own eyes, none.
//
// Shader packs (GL only): the chain is skipped, with one log line, while a
// pack is active. That is OptiFine's behaviour — a loaded pack owns the
// whole post-processing stage through its composite/final programs, and the
// vanilla entity shaders are not run on top of it. (Whether Iris does the
// same could not be checked here; OptiFine's is the documented behaviour.)
#pragma once

#include "client/renderer/backend/RenderTypes.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Render {

    // MC PostChainConfig, parsed.
    struct PostChainConfig {
        struct Target {
            std::string name;
            int width  = 0;   // 0 = the frame's size
            int height = 0;
        };
        struct Input {
            std::string samplerName;   // "In", "Blur", …
            std::string target;        // an internal target or "minecraft:main"
            bool        bilinear = false;
        };
        struct Pass {
            std::string vertexShader;    // "minecraft:core/screenquad"
            std::string fragmentShader;  // "minecraft:post/blit"
            std::vector<Input> inputs;
            std::string output;
            std::array<float, 16> params{};   // the uniform blocks, packed
        };
        std::vector<Target> targets;
        std::vector<Pass>   passes;

        // False (with `error`) on a malformed file.
        static bool Parse(const std::string& json, PostChainConfig& out, std::string& error);
    };

    class PostChain {
    public:
        static constexpr const char* kMainTarget = "minecraft:main";

        PostChain(std::string id, PostChainConfig config);
        ~PostChain();
        PostChain(const PostChain&) = delete;
        PostChain& operator=(const PostChain&) = delete;

        const std::string& Id() const { return m_id; }

        // MC PostChain.process: every pass, in order, onto the frame being
        // drawn (the default framebuffer must be bound). `quad` is the
        // full-screen quad, `shaders` the resolved program of each pass.
        // False when it could not run (targets unavailable, the frame
        // could not be copied); the frame is then untouched or, past the
        // first pass that wrote it, partly processed.
        bool Process(int width, int height, MeshHandle quad, const std::vector<ShaderHandle>& shaders,
                     float menuBlurRadius);

        // MC closePersistentTargets: frees both target sets.
        void ReleaseTargets();

        const PostChainConfig& Config() const { return m_config; }

    private:
        struct TargetSet {
            std::unordered_map<std::string, RenderTargetHandle> targets;
            RenderTargetHandle mainCopy = INVALID_RENDER_TARGET;
            int width = 0, height = 0;
        };

        bool EnsureTargets(TargetSet& set, int width, int height);
        static RenderTargetHandle MakeTarget(int width, int height);
        static void ConfigureTexture(RenderTargetHandle target);

        std::string     m_id;
        PostChainConfig m_config;
        std::array<TargetSet, 2> m_sets;
        int m_nextSet = 0;
    };

    // The GameRenderer half: which chains run this frame, their shaders and
    // the quad, loading on demand and remembering what failed.
    class PostEffects {
    public:
        static PostEffects& Get();

        // MC checkEntityPostEffect / clearSpectatedEntityPostEffect, per
        // frame: the entity the camera is in (-1 = your own eyes) and its
        // effect ("" = none). A new camera entity is MC's setCameraEntity:
        // an effect is (re)loaded ACTIVE, and no effect clears it inactive.
        void SetSpectatedEntity(int32_t cameraEntityId, const std::string& effectId) {
            if (cameraEntityId == m_cameraEntity && effectId == m_spectated) return;
            m_cameraEntity = cameraEntityId;
            m_spectated    = effectId;
            m_active       = !effectId.empty();
        }
        const std::string& SpectatedEntityPostEffect() const { return m_spectated; }
        // MC GameRenderer.toggleSpectatorPostEffect (key.toggleSpectator
        // ShaderEffects): flips whether the loaded effect is applied.
        void ToggleSpectatorPostEffect() { m_active = !m_active; }
        bool SpectatorPostEffectActive() const { return m_active; }

        // MC preparePostEffects + applyPostEffects. After the world, the
        // hand and the entity outline, before the GUI; leaves the default
        // framebuffer bound with the full-frame viewport.
        void Apply(int framebufferWidth, int framebufferHeight, bool shaderPackActive);

        // A resource reload: chains are re-read on next use and the failed
        // list is forgotten (MC shouldResetFailedPostEffects).
        void Reload();
        void Shutdown();

    private:
        PostEffects() = default;
        PostChain* GetChain(const std::string& id);
        ShaderHandle GetShader(const std::string& vertexId, const std::string& fragmentId);
        bool EnsureQuad();

        std::string m_spectated;
        int32_t     m_cameraEntity = -1;
        bool        m_active = false;   // MC spectatedEntityEffectActive
        std::unordered_map<std::string, std::unique_ptr<PostChain>> m_chains;
        // Per chain: each pass's program, resolved when the chain loads.
        std::unordered_map<std::string, std::vector<ShaderHandle>> m_chainShaders;
        std::unordered_set<std::string> m_failed;
        std::unordered_map<std::string, ShaderHandle> m_shaders;   // "vert|frag"
        // Frames since each loaded chain last ran (MC's pooled targets age out).
        std::unordered_map<std::string, int> m_idleFrames;
        BufferHandle m_quadVB   = INVALID_BUFFER;
        MeshHandle   m_quadMesh = INVALID_MESH;
        bool m_loggedPackSkip = false;
    };

} // namespace Render

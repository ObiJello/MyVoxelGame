// File: src/client/renderer/portal/BloomPipeline.hpp
//
// Bloom post-process for the portal feature's HDR output. Algorithm:
//   1. Bright-pass: extract HDR pixels above threshold into a half-res
//      bright RT.
//   2. Downsample chain: progressively half-resolution copies (4-5
//      levels) — gives us a mip pyramid of blurred bright pixels.
//   3. Separable Gaussian blur (horizontal then vertical) at each level.
//   4. Upsample chain: starting from the smallest, additively combine
//      with each larger level back to full res.
//   5. The final combined bloom texture is returned for the tone-map
//      composite to add to the HDR color.
//
// Reference: `Portal code/sp/src/materialsystem/stdshaders/Bloom.cpp`,
// `SDK_Bloom_ps2x.fxc`, `SDK_bloomadd_ps2x.fxc`.
//
// Entirely gated on ENABLE_PORTAL_GUN. When the feature is off, the
// pipeline doesn't exist and HDRPipeline::EndHDRPassAndComposite skips
// the bloom add (tone-mapped HDR rendered without bloom).

#pragma once

#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include "../backend/RenderTypes.hpp"

namespace Render {

    class BloomPipeline {
    public:
        BloomPipeline();
        ~BloomPipeline();

        bool Initialize();
        void Shutdown();
        bool IsActive() const { return m_active; }

        // Apply the bloom chain to `hdrSourceTexture` (the HDR scene
        // texture from HDRPipeline). Returns the full-resolution combined
        // bloom texture for the tone-map composite to add. Returns
        // INVALID_TEXTURE if pipeline is inactive.
        TextureHandle Apply(TextureHandle hdrSourceTexture, int fbW, int fbH);

        // Tunables.
        void  SetThreshold(float t) { m_threshold = t; }
        float GetThreshold() const  { return m_threshold; }
        void  SetSoftKnee(float k)  { m_softKnee = k; }
        void  SetIntensity(float i) { m_intensity = i; }
        float GetIntensity() const  { return m_intensity; }

    private:
        static constexpr int kBloomLevels = 5;

        bool          m_active   = false;

        // 5-level mip pyramid of two RTs each (ping-pong between bright +
        // blurred). RT pairs at sizes [fb/2, fb/4, fb/8, fb/16, fb/32].
        RenderTargetHandle m_brightRT[kBloomLevels] = {};   // bright-pass + horizontal blur write
        RenderTargetHandle m_blurredRT[kBloomLevels] = {};  // vertical blur write + upsample combine
        int                m_currentWidth  = 0;
        int                m_currentHeight = 0;

        ShaderHandle  m_brightShader   = INVALID_SHADER;
        ShaderHandle  m_blurShader     = INVALID_SHADER;
        ShaderHandle  m_upsampleShader = INVALID_SHADER;

        MeshHandle    m_fsTriangleMesh = INVALID_MESH;
        BufferHandle  m_fsTriangleVB   = INVALID_BUFFER;

        // Subtle Portal-style bloom — only the brightest rim crest
        // pixels glow, and the additive contribution is dialed way down
        // so the bloom never washes out the disc. Tuned by inspection:
        // threshold 0.75 catches only the inner rim's highest-luminance
        // pixels (~0.7 linear); intensity 0.25 keeps the added halo
        // light. Bump intensity if you want a more dramatic glow,
        // bump threshold to make the bloom rarer.
        float         m_threshold = 0.75f;
        float         m_softKnee  = 0.5f;
        float         m_intensity = 0.25f;

        bool          AllocateRTs(int w, int h);
        void          DestroyRTs();
    };

    extern BloomPipeline g_bloomPipeline;

} // namespace Render

#endif // ENABLE_PORTAL_GUN

// File: src/client/renderer/environment/Lightmap.hpp
//
// MC's lightmap (26.x Lightmap + LightmapRenderStateExtractor + the
// core/lightmap.fsh pass): a 16x16 RGBA8 texture, x = block light level,
// y = sky light level, whose texel is the colour a surface lit that way is
// multiplied by this frame. Computed on the CPU (256 texels, the shader's
// arithmetic verbatim) and uploaded when it changes; LINEAR-filtered and
// clamped, sampled exactly as MC's sample_lightmap:
//
//     texture(lightmap, clamp(uv / 256 + 0.5 / 16, 0.5 / 16, 15.5 / 16))
//
// with uv the MC light coords (level * 16; smooth lighting's fractions land
// between texels and blend). Terrain samples it per vertex (terrain*.vert);
// entities, block entities, items, particles and the held item are lit per
// draw from the CPU copy (Sample / SampleCoords) — MC gives each one its
// own packed light, the renderers here batch per draw.
//
// Inputs, as MC extracts them:
//   skyFactor          SKY_LIGHT_FACTOR  — EnvironmentFrame::skyLightFactor
//                      (the day timeline 1.0 .. 0.24, a lightning flash 1)
//   skyLightColor      SKY_LIGHT_COLOR   — white by day, #7a7aff by night
//   ambientColor       AMBIENT_LIGHT_COLOR — the dimension's (#0a0a0a
//                      overworld, #302821 nether, #3f473f end)
//   blockLightTint     BLOCK_LIGHT_TINT  — #ffd88c
//   blockFactor        1.4 + the block-light flicker (random walk per tick)
//   nightVision        NIGHT_VISION intensity (or conduit water vision) and
//                      NIGHT_VISION_COLOR #999999
//   darknessScale      DARKNESS's pulse (MobEffectView::darknessLightmap)
//   brightness         the Brightness option (gamma) minus DARKNESS's blend
//
// OBEY_LIGHT=0 is the A/B kill switch: the texels become the pre-light-engine
// look (everything times the frame's skyBrightness, FULL_BRIGHT cells at 1),
// entities go back to EntityEnvironment's frame-wide values.
#pragma once

#include "client/renderer/backend/RenderTypes.hpp"

#include <array>
#include <cstdint>
#include <glm/glm.hpp>

namespace Render {

    struct EnvironmentFrame;

    class Lightmap {
    public:
        static Lightmap& Get();

        // OBEY_LIGHT (read once). False: the old uniform night dim.
        static bool Enabled();

        bool Initialize();
        void Shutdown();

        // Once per frame, after EnvironmentState::UpdateFrame. Recomputes the
        // main texture from the composed frame (and advances the flicker one
        // step per elapsed client tick of `gameTime`).
        void Update(const EnvironmentFrame& frame, int64_t gameTime);

        // The texture for a frame: the main one for the camera's own frame,
        // a second one for a portal's far-side frame (composed from that
        // frame, visible from the next frame on — portal frames are steady).
        TextureHandle TextureFor(const EnvironmentFrame& frame);
        TextureHandle MainTexture() const { return m_texture[0][m_current[0]]; }

        // CPU lookups on the main texture's texels, for per-draw lighting.
        // Integer levels (MC packed light): exactly one texel.
        glm::vec3 Sample(int blockLevel, int skyLevel) const;
        // The same lookup on the lightmap of `frame` — the view being drawn
        // (EnvironmentState::Get().Frame()): a portal's far side lights its
        // entities with its own dimension's lightmap, as its terrain.
        glm::vec3 SampleFor(const EnvironmentFrame& frame, int blockLevel, int skyLevel);
        // MC light coords 0..240 each (fractions blend bilinearly, like the
        // GPU's LINEAR sample).
        glm::vec3 SampleCoords(float block16, float sky16) const;
        // An entity's per-draw light at the main frame: the texel for the
        // packed light at its position (see EntityLightAt in
        // EntityEnvironment.hpp).
        const std::array<uint8_t, 16 * 16 * 4>& Texels() const { return m_texels[0]; }

    private:
        Lightmap() = default;
        void Compute(const EnvironmentFrame& frame, std::array<uint8_t, 16 * 16 * 4>& out) const;
        void Upload(int slot);

        // Each slot rotates through kRing textures: an upload writes the one
        // the fewest recent frames have drawn with, then becomes current.
        // Writing the texture the in-flight frames still sample made Apple's
        // GL driver stall the CPU until the GPU finished them (Tracy
        // 2026-09-25: Lightmap.Update >5 ms on 190 frames of 5,500). Four
        // covers GL's queued frames plus the one being recorded. GL only:
        // Vulkan stages its updates (no stall) and writes in place — see
        // Upload. Callers must look the texture up per bind (TextureFor),
        // never keep it.
        static constexpr int kRing = 4;
        TextureHandle m_texture[2][kRing] = {
            {INVALID_TEXTURE, INVALID_TEXTURE, INVALID_TEXTURE, INVALID_TEXTURE},
            {INVALID_TEXTURE, INVALID_TEXTURE, INVALID_TEXTURE, INVALID_TEXTURE}};
        int m_current[2] = {0, 0};
        std::array<uint8_t, 16 * 16 * 4> m_texels[2]{};
        std::array<uint8_t, 16 * 16 * 4> m_uploaded[2]{};
        bool m_hasUploaded[2] = {false, false};
        const EnvironmentFrame* m_mainFrame = nullptr;
        const EnvironmentFrame* m_farFrame = nullptr;   // what m_texels[1] was composed from
        uint64_t m_updateSerial = 0;                     // bumped by every Update
        uint64_t m_farSerial = 0;                        // the Update m_texels[1] belongs to

        // LightmapRenderStateExtractor.blockLightFlicker and its RNG.
        float    m_flicker = 0.0f;
        int64_t  m_lastTick = INT64_MIN;
        uint64_t m_rng = 0x2545F4914F6CDD1DULL;
        float NextFloat();
    };

} // namespace Render

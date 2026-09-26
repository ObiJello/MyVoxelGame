// File: src/client/renderer/environment/Lightmap.cpp
#include "Lightmap.hpp"

#include "EnvironmentState.hpp"
#include "MobEffectEnvironment.hpp"
#include "client/renderer/backend/RenderBackend.hpp"
#include "platform/GameDirectory.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace Render {

    Lightmap& Lightmap::Get() {
        static Lightmap s_instance;
        return s_instance;
    }

    bool Lightmap::Enabled() {
        static const bool enabled = [] {
            const char* v = std::getenv("OBEY_LIGHT");
            const bool on = !(v && std::strcmp(v, "0") == 0);
            if (!on) Log::Info("[Light] OBEY_LIGHT=0: lightmap off — terrain and entities use the old uniform night dim");
            return on;
        }();
        return enabled;
    }

    bool Lightmap::Initialize() {
        if (!g_renderBackend) return false;
        for (int slot = 0; slot < 2; ++slot) {
            if (m_texture[slot][0] != INVALID_TEXTURE) continue;
            // White until the first Update: a draw before it is simply unlit.
            std::array<uint8_t, 16 * 16 * 4> white{};
            white.fill(255);
            for (int r = 0; r < kRing; ++r) {
                TextureHandle& tex = m_texture[slot][r];
                tex = g_renderBackend->CreateTexture2D(16, 16, TextureFormat::RGBA8, white.data());
                if (tex == INVALID_TEXTURE) {
                    Log::Error("[Light] could not create the lightmap texture");
                    return false;
                }
                // MC: the lightmap is sampled LINEAR, clamped (a smooth-lit
                // vertex's fractional level blends the two texels around it).
                g_renderBackend->SetTextureFilter(tex, TextureFilter::Linear, TextureFilter::Linear);
                g_renderBackend->SetTextureWrap(tex, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            }
            m_current[slot] = 0;
            m_texels[slot] = white;
            m_hasUploaded[slot] = false;
        }
        return true;
    }

    void Lightmap::Shutdown() {
        if (!g_renderBackend) return;
        for (int slot = 0; slot < 2; ++slot) {
            for (TextureHandle& tex : m_texture[slot]) {
                if (tex != INVALID_TEXTURE) g_renderBackend->DestroyTexture(tex);
                tex = INVALID_TEXTURE;
            }
            m_current[slot] = 0;
            m_hasUploaded[slot] = false;
        }
        m_mainFrame = nullptr;
        m_farFrame = nullptr;
    }

    float Lightmap::NextFloat() {
        // xorshift64*: any decent uniform stream serves — MC's flicker is a
        // RandomSource.create() the game never seeds reproducibly either.
        m_rng ^= m_rng >> 12;
        m_rng ^= m_rng << 25;
        m_rng ^= m_rng >> 27;
        const uint64_t r = m_rng * 0x2545F4914F6CDD1DULL;
        return static_cast<float>(r >> 40) / static_cast<float>(1u << 24);
    }

    namespace {
        // lightmap.fsh get_brightness.
        inline float Brightness(float level) { return level / (4.0f - 3.0f * level); }
        // lightmap.fsh notGamma. (The shader divides 0 by 0 for a black
        // texel; the result is black either way.)
        inline glm::vec3 NotGamma(const glm::vec3& c) {
            const float maxComponent = std::max(std::max(c.x, c.y), c.z);
            if (maxComponent <= 0.0f) return glm::vec3(0.0f);
            const float inv = 1.0f - maxComponent;
            const float maxScaled = 1.0f - inv * inv * inv * inv;
            return c * (maxScaled / maxComponent);
        }
        inline float Parabolic(float level) { return (2.0f * level - 1.0f) * (2.0f * level - 1.0f); }
        inline uint8_t ToByte(float f) {
            return static_cast<uint8_t>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }

    void Lightmap::Compute(const EnvironmentFrame& frame, std::array<uint8_t, 16 * 16 * 4>& out) const {
        if (!Enabled()) {
            // The pre-light-engine look: one uniform night dim, full-bright
            // (emissiveRendering) faces left alone.
            for (int sky = 0; sky < 16; ++sky) {
                for (int block = 0; block < 16; ++block) {
                    const float v = (sky == 15 && block == 15) ? 1.0f : frame.skyBrightness;
                    uint8_t* t = &out[static_cast<size_t>((sky * 16 + block) * 4)];
                    t[0] = t[1] = t[2] = ToByte(v);
                    t[3] = 255;
                }
            }
            return;
        }

        // LightmapRenderStateExtractor.extract.
        const MobEffectView& fx = GetMobEffectView();
        const float darknessOption = Platform::g_gameSettings.GetDarknessEffectScale();
        const float gamma = Platform::g_gameSettings.GetGamma();
        const float blockFactor = m_flicker + 1.4f;
        const float skyFactor = frame.skyLightFactor;
        const float brightness = std::max(0.0f, gamma - fx.darknessBlend * darknessOption);
        const float darknessScale = fx.darknessLightmap;          // calculateDarknessScale * option
        const float nightVision = fx.nightVisionIntensity;
        const glm::vec3 nightVisionColor(0x99 / 255.0f);          // NIGHT_VISION_COLOR default
        const float bossDarkening = 0.0f;                         // no darken-screen boss bars yet

        // core/lightmap.fsh, per texel.
        for (int sky = 0; sky < 16; ++sky) {
            for (int block = 0; block < 16; ++block) {
                const float blockLevel = static_cast<float>(block) / 15.0f;
                const float skyLevel = static_cast<float>(sky) / 15.0f;
                const float blockBrightness = Brightness(blockLevel) * blockFactor;
                const float skyBrightness = Brightness(skyLevel) * skyFactor;

                glm::vec3 color = glm::max(frame.ambientLightColor, nightVisionColor * nightVision);
                color += frame.skyLightColor * skyBrightness;
                const glm::vec3 blockColor = glm::mix(frame.blockLightTint, glm::vec3(1.0f),
                                                      0.9f * Parabolic(blockLevel));
                color += blockColor * blockBrightness;
                color = glm::mix(color, color * glm::vec3(0.7f, 0.6f, 0.6f), bossDarkening);
                color -= glm::vec3(darknessScale);
                color = glm::clamp(color, glm::vec3(0.0f), glm::vec3(1.0f));
                color = glm::mix(color, NotGamma(color), brightness);

                uint8_t* t = &out[static_cast<size_t>((sky * 16 + block) * 4)];
                t[0] = ToByte(color.x);
                t[1] = ToByte(color.y);
                t[2] = ToByte(color.z);
                t[3] = 255;
            }
        }
    }

    void Lightmap::Upload(int slot) {
        if (m_texture[slot][0] == INVALID_TEXTURE || !g_renderBackend) return;
        if (m_hasUploaded[slot] && m_uploaded[slot] == m_texels[slot]) return;
        // GL: the next texture in the ring — the one drawn with longest ago.
        // Vulkan writes in place: it stages the copy into the NEXT frame's
        // command buffer (no stall to avoid), so a freshly rotated handle
        // bound this frame would show the texels of four uploads ago.
        const bool rotate = g_renderBackend->GetType() == BackendType::OpenGL;
        const int next = rotate ? (m_current[slot] + 1) % kRing : m_current[slot];
        g_renderBackend->UpdateTexture2D(m_texture[slot][next], 0, 0, 16, 16, m_texels[slot].data());
        m_current[slot] = next;
        m_uploaded[slot] = m_texels[slot];
        m_hasUploaded[slot] = true;
    }

    void Lightmap::Update(const EnvironmentFrame& frame, int64_t gameTime) {
        PROFILE_ZONE_N("Lightmap.Update");
        // LightmapRenderStateExtractor.tick, once per client tick.
        if (m_lastTick == INT64_MIN || gameTime < m_lastTick) m_lastTick = gameTime;
        const int64_t steps = std::min<int64_t>(gameTime - m_lastTick, 20);
        for (int64_t i = 0; i < steps; ++i) {
            m_flicker += (NextFloat() - NextFloat()) * NextFloat() * NextFloat() * 0.1f;
            m_flicker *= 0.9f;
        }
        m_lastTick = gameTime;

        m_mainFrame = &frame;
        ++m_updateSerial;
        Compute(frame, m_texels[0]);
        Upload(0);
    }

    TextureHandle Lightmap::TextureFor(const EnvironmentFrame& frame) {
        if (&frame == m_mainFrame || m_texture[1][0] == INVALID_TEXTURE) return m_texture[0][m_current[0]];
        // A portal's far-side frame: its own lightmap in the second slot.
        // The upload lands at the next frame's start (Vulkan stages texture
        // updates), a frame's latency on a view that barely changes.
        Compute(frame, m_texels[1]);
        Upload(1);
        m_farFrame = &frame;
        m_farSerial = m_updateSerial;
        return m_texture[1][m_current[1]];
    }

    glm::vec3 Lightmap::SampleFor(const EnvironmentFrame& frame, int blockLevel, int skyLevel) {
        if (&frame == m_mainFrame || m_mainFrame == nullptr) return Sample(blockLevel, skyLevel);
        // A far-side view whose terrain has not composed its texels yet
        // (TextureFor runs at its first terrain bind) composes them here.
        if (&frame != m_farFrame || m_farSerial != m_updateSerial) {
            Compute(frame, m_texels[1]);
            m_farFrame = &frame;
            m_farSerial = m_updateSerial;
        }
        const int b = std::clamp(blockLevel, 0, 15), s = std::clamp(skyLevel, 0, 15);
        const uint8_t* t = &m_texels[1][static_cast<size_t>((s * 16 + b) * 4)];
        return glm::vec3(t[0], t[1], t[2]) / 255.0f;
    }

    glm::vec3 Lightmap::Sample(int blockLevel, int skyLevel) const {
        const int b = std::clamp(blockLevel, 0, 15), s = std::clamp(skyLevel, 0, 15);
        const uint8_t* t = &m_texels[0][static_cast<size_t>((s * 16 + b) * 4)];
        return glm::vec3(t[0], t[1], t[2]) / 255.0f;
    }

    glm::vec3 Lightmap::SampleCoords(float block16, float sky16) const {
        // sample_lightmap: uv/256 + 0.5/16, clamped to the texel centres,
        // LINEAR — in texel units, level = coord / 16, blended between the
        // two texels around it.
        const float fb = std::clamp(block16 / 16.0f, 0.0f, 15.0f);
        const float fs = std::clamp(sky16 / 16.0f, 0.0f, 15.0f);
        const int b0 = static_cast<int>(fb), s0 = static_cast<int>(fs);
        const int b1 = std::min(b0 + 1, 15), s1 = std::min(s0 + 1, 15);
        const float tb = fb - static_cast<float>(b0), ts = fs - static_cast<float>(s0);
        const glm::vec3 c00 = Sample(b0, s0), c10 = Sample(b1, s0);
        const glm::vec3 c01 = Sample(b0, s1), c11 = Sample(b1, s1);
        return glm::mix(glm::mix(c00, c10, tb), glm::mix(c01, c11, tb), ts);
    }

} // namespace Render

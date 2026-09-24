// File: src/client/renderer/environment/HushAtmosphere.cpp
//
// See the header. All of this is the Hush's own look — there is no MC source
// to follow; the numbers are tuned against the constant composition
// (kHushFog / kHushSky / the 0.42 night) so each layer reads as a change of
// mood rather than a different dimension.
#include "HushAtmosphere.hpp"

#include "EnvironmentState.hpp"
#include "client/world/ClientChunkManager.hpp"
#include "client/world/HushStillnessState.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/world/level/DimensionId.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <string_view>
#include <vector>

namespace Render {

    namespace {

        using Clock = std::chrono::steady_clock;

        glm::vec3 Rgb24(uint32_t rgb) {
            return glm::vec3(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f,
                             (rgb & 0xFF) / 255.0f);
        }

        float Smoothstep(float edge0, float edge1, float x) {
            const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        // ── Blend rates ─────────────────────────────────────────────────────
        // Exponential approach time constants (seconds): ~95 % of the way in
        // three of them. Biomes ease over "a few seconds"; the depth term a
        // little faster, since it follows the player down a shaft.
        constexpr float kBiomeTau = 1.6f;
        constexpr float kDepthTau = 1.0f;
        // The stillness ramps linearly (then smoothstepped): falls over 2 s,
        // lifts over 3 s — the "soft reverse".
        constexpr float kStillInSeconds  = 2.0f;
        constexpr float kStillOutSeconds = 3.0f;
        // Grace past the server's stated end before the client lifts it on
        // its own (a lifted packet that never came — a dropped session).
        constexpr float kStillDeadlineGrace = 3.0f;

        // ── Cavern fog ──────────────────────────────────────────────────────
        // Teal-tinted, a touch greener and brighter than the surface fog
        // (#0B2A2E), so crystal light hangs in it rather than sinking.
        const glm::vec3 kCavernFog = Rgb24(0x0D3C3C);
        // Full cavern: fog from 2 to 56 blocks — the far wall of a big
        // chamber is a silhouette, the one across the tunnel still reads.
        constexpr float kCavernFogStart = 2.0f;
        constexpr float kCavernFogEnd   = 56.0f;
        // The general depth term reaches at most this much of the cavern
        // look: being deep is murky, the caverns are murkier.
        constexpr float kDepthWeight = 0.55f;
        // Depth probe: how far up the column is walked, how many non-air
        // blocks overhead count as "under cover" (a canopy is a handful; a
        // hillside is dozens), and how often it runs.
        constexpr int   kDepthProbeHeight = 64;
        constexpr int   kCoverThreshold   = 8;
        constexpr float kDepthProbeSeconds = 0.25f;
        // Below this Y (a little over the Hush's sea level of 50) the depth
        // term ramps in over 40 blocks.
        constexpr float kDepthSurfaceY = 56.0f;
        constexpr float kDepthRampBlocks = 40.0f;

        // ── Underwater ──────────────────────────────────────────────────────
        // The Hush's water fog is pulled toward a luminous teal and lifted:
        // sculk-lit water, not the Overworld's navy. In sunken_choir the
        // biome's own (already luminous) water fog keeps more of its say and
        // glows brighter still, and the water reads a little clearer.
        const glm::vec3 kHushWaterTeal = Rgb24(0x178A80);
        const glm::vec3 kChoirGlow     = Rgb24(0x2BD4C0);
        constexpr float kWaterTealMix     = 0.65f;
        constexpr float kChoirTealMix     = 0.35f;
        constexpr float kWaterBrighten    = 1.18f;
        constexpr float kChoirGlowMix     = 0.30f;
        constexpr float kChoirClarity     = 0.20f;   // fog end × (1 + this)

        // ── Stillness ───────────────────────────────────────────────────────
        // "Slightly": the render-distance fade starts much nearer and the
        // environmental fog ends nearer; the sky disc flattens most of the
        // way to the fog; the stars and auroras mostly go out.
        constexpr float kStillRdStartScale = 0.55f;
        constexpr float kStillEnvEndScale  = 0.55f;
        constexpr float kStillSkyFlatten   = 0.7f;
        constexpr float kStillStarDim      = 0.7f;
        constexpr float kStillAuroraDim    = 0.85f;

        // Auroras hidden by being underground (the sky pass still runs behind
        // the fogged cave walls) and dulled seen from underwater.
        constexpr float kUnderwaterAurora = 0.35f;

        // What a biome contributes. Keyed on the biome NAME (with or without
        // a namespace) so the three new Hush biomes work the moment the
        // worldgen table carries them, whatever ids they are given.
        enum BiomeRole : uint8_t { kRoleNone = 0, kRoleSteppe = 1, kRoleCavern = 2, kRoleChoir = 3 };

        BiomeRole RoleForName(std::string_view name) {
            const size_t colon = name.find(':');
            if (colon != std::string_view::npos) name = name.substr(colon + 1);
            if (name == "aurora_steppe")                             return kRoleSteppe;
            if (name == "crystal_caverns" || name == "hollow_deep")  return kRoleCavern;
            if (name == "sunken_choir")                              return kRoleChoir;
            return kRoleNone;
        }

        BiomeRole RoleFor(Game::BiomeId biome) {
            static const std::vector<BiomeRole> table = [] {
                std::vector<BiomeRole> t(Game::BiomeRegistry::Count());
                for (Game::BiomeId i = 0; i < t.size(); ++i) {
                    t[i] = RoleForName(Game::BiomeRegistry::Get(i).name);
                }
                return t;
            }();
            return biome < table.size() ? table[biome] : kRoleNone;
        }

        float Approach(float value, float target, float dt, float tau) {
            return target + (value - target) * std::exp(-dt / tau);
        }

    } // namespace

    HushAtmosphere& HushAtmosphere::Get() {
        static HushAtmosphere instance;
        return instance;
    }

    void HushAtmosphere::SetInHush(bool inHush) {
        if (inHush == m_inHush) return;
        m_inHush = inHush;
        // Arriving: the first step lands on the arrival spot's values.
        // Leaving: whatever stillness was on belonged to the Hush (the server
        // also sends it lifted).
        m_snap = true;
        m_stillOn = false;
        m_stillRamp = 0.0f;
        m_stillSerial = ::Client::HushStillnessState::g_serial.load(std::memory_order_acquire);
        if (inHush && ::Client::HushStillnessState::g_active.load(std::memory_order_relaxed)) {
            // A stillness reported before the dimension switch reached the
            // sky (the server syncs arrivals on its next tick; the order of
            // the two is not guaranteed).
            m_stillOn = true;
            m_stillDeadline = Clock::now() + std::chrono::milliseconds(static_cast<int64_t>(
                (::Client::HushStillnessState::g_remainingTicks.load(std::memory_order_relaxed) / 20.0f +
                 kStillDeadlineGrace) * 1000.0f));
        }
    }

    float HushAtmosphere::Stillness() const {
        return Smoothstep(0.0f, 1.0f, m_stillRamp);
    }

    float HushAtmosphere::SampleDepthTarget(const glm::dvec3& cameraPos) const {
        const ::Client::ClientChunkManager* chunks = ::Client::g_clientChunkManager;
        if (!chunks) return 0.0f;
        const int x = static_cast<int>(std::floor(cameraPos.x));
        const int y = static_cast<int>(std::floor(cameraPos.y));
        const int z = static_cast<int>(std::floor(cameraPos.z));
        const int minY = Game::DimensionMinY(Game::DimensionId::Hush);
        const int maxY = minY + Game::DimensionLogicalHeight(Game::DimensionId::Hush) - 1;
        // Non-air blocks in the column overhead: the cheap stand-in for
        // "can see the sky" (this client keeps no sky light).
        int cover = 0;
        const int top = std::min(y + kDepthProbeHeight, maxY);
        for (int by = std::max(y + 1, minY); by <= top; ++by) {
            if (chunks->GetBlockAt({x, by, z}) != Game::BlockID::Air) ++cover;
        }
        if (cover < kCoverThreshold) return 0.0f;
        // Under cover: how far below the surface band, and how thick the
        // roof is — a cave in a mountain above sea level still counts by its
        // roof.
        const float belowSurface = Smoothstep(0.0f, kDepthRampBlocks,
                                              kDepthSurfaceY - static_cast<float>(cameraPos.y));
        const float roof = std::clamp(static_cast<float>(cover - kCoverThreshold) / 24.0f, 0.0f, 1.0f);
        return std::max(belowSurface, roof);
    }

    void HushAtmosphere::Step(const Inputs& in) {
        const Clock::time_point now = Clock::now();
        float dt = 0.0f;
        if (m_lastStep.time_since_epoch().count() != 0) {
            dt = std::clamp(std::chrono::duration<float>(now - m_lastStep).count(), 0.0f, 0.5f);
        }
        m_lastStep = now;

        // ── Biomes at the camera ────────────────────────────────────────────
        BiomeRole role = kRoleNone;
        if (in.hasCameraPos && ::Client::g_clientChunkManager) {
            role = RoleFor(::Client::g_clientChunkManager->BiomeAtWorld(
                static_cast<int>(std::floor(in.cameraPos.x)),
                static_cast<int>(std::floor(in.cameraPos.y)),
                static_cast<int>(std::floor(in.cameraPos.z))));
        }
        const float steppeTarget = role == kRoleSteppe ? 1.0f : 0.0f;
        const float cavernTarget = role == kRoleCavern ? 1.0f : 0.0f;
        const float choirTarget  = role == kRoleChoir  ? 1.0f : 0.0f;

        // ── Depth under cover (a few times a second) ────────────────────────
        const bool probeDue = m_snap ||
            std::chrono::duration<float>(now - m_lastDepthProbe).count() >= kDepthProbeSeconds;
        if (probeDue && in.hasCameraPos) {
            m_depthTarget = SampleDepthTarget(in.cameraPos);
            m_lastDepthProbe = now;
        }

        if (m_snap) {
            m_steppe = steppeTarget;
            m_cavern = cavernTarget;
            m_choir  = choirTarget;
            m_depth  = m_depthTarget;
            m_snap   = false;
        } else {
            m_steppe = Approach(m_steppe, steppeTarget, dt, kBiomeTau);
            m_cavern = Approach(m_cavern, cavernTarget, dt, kBiomeTau);
            m_choir  = Approach(m_choir,  choirTarget,  dt, kBiomeTau);
            m_depth  = Approach(m_depth,  m_depthTarget, dt, kDepthTau);
        }

        // ── Stillness ───────────────────────────────────────────────────────
        const uint32_t serial = ::Client::HushStillnessState::g_serial.load(std::memory_order_acquire);
        if (serial != m_stillSerial) {
            m_stillSerial = serial;
            m_stillOn = ::Client::HushStillnessState::g_active.load(std::memory_order_relaxed);
            if (m_stillOn) {
                const float seconds =
                    ::Client::HushStillnessState::g_remainingTicks.load(std::memory_order_relaxed) / 20.0f;
                m_stillDeadline = now + std::chrono::milliseconds(
                    static_cast<int64_t>((seconds + kStillDeadlineGrace) * 1000.0f));
            }
        }
        if (m_stillOn && now >= m_stillDeadline) m_stillOn = false;
        if (m_stillOn) m_stillRamp = std::min(1.0f, m_stillRamp + dt / kStillInSeconds);
        else           m_stillRamp = std::max(0.0f, m_stillRamp - dt / kStillOutSeconds);
    }

    void HushAtmosphere::Compose(EnvironmentFrame& frame, const Inputs& in) {
        // A view of the Hush from another dimension: the defaults only.
        if (!m_inHush) {
            frame.auroraStrength = kBaseAurora;
            return;
        }
        Step(in);

        const float still  = Stillness();
        const bool  water  = in.cameraFluid == EnvironmentState::kFluidWater;
        const bool  lava   = in.cameraFluid == EnvironmentState::kFluidLava;
        // How far "underground" the look is: the cavern biomes fully, the
        // general depth term partly.
        const float cavern = std::clamp(std::max(m_cavern, m_depth * kDepthWeight), 0.0f, 1.0f);

        // ── Fog ─────────────────────────────────────────────────────────────
        if (water) {
            // The fluid fog already replaced the atmosphere (colour from the
            // biome's water fog, distances from the eyes' adjustment).
            const float tealMix = kWaterTealMix + (kChoirTealMix - kWaterTealMix) * m_choir;
            glm::vec3 c = glm::mix(frame.fogColor, kHushWaterTeal, tealMix) * kWaterBrighten;
            c = glm::mix(c, kChoirGlow, kChoirGlowMix * m_choir);
            frame.fogColor = glm::min(c, glm::vec3(1.0f));
            frame.fogEnvEnd *= 1.0f + kChoirClarity * m_choir;
            frame.fogSkyEnd   = frame.fogEnvEnd;
            frame.fogCloudEnd = frame.fogEnvEnd;
        } else if (!lava) {
            if (cavern > 0.0f) {
                frame.fogColor = glm::mix(frame.fogColor, kCavernFog, cavern);
                // Distances only with the Fog option on — the attribute-fog
                // rule the Twilight Forest's spooky forest follows. The end
                // is interpolated geometrically (1024 → 56 is a factor of
                // ~18): a linear blend would jump to a thick fog at the
                // slightest cavern weight.
                if (in.fogEnabled) {
                    frame.fogEnvStart = glm::mix(frame.fogEnvStart, kCavernFogStart, cavern);
                    frame.fogEnvEnd   = std::pow(std::max(frame.fogEnvEnd, 1.0f), 1.0f - cavern) *
                                        std::pow(kCavernFogEnd, cavern);
                    frame.fogEnvEnd   = std::max(frame.fogEnvEnd, frame.fogEnvStart + 1.0f);
                }
                frame.fogSkyEnd = std::min(frame.fogSkyEnd,
                                           std::max(frame.fogEnvEnd, kCavernFogEnd));
            }
        }

        // ── Stillness ───────────────────────────────────────────────────────
        if (still > 0.0f) {
            if (!lava) {
                const float envScale = 1.0f + (kStillEnvEndScale - 1.0f) * still;
                frame.fogEnvEnd = std::max(frame.fogEnvStart + 1.0f, frame.fogEnvEnd * envScale);
                if (frame.fogRdStart < 1e8f) {
                    const float rdScale = 1.0f + (kStillRdStartScale - 1.0f) * still;
                    frame.fogRdStart *= rdScale;
                }
                frame.fogSkyEnd = std::min(frame.fogSkyEnd, std::max(frame.fogEnvEnd, 16.0f));
            }
            frame.skyColor       = glm::mix(frame.skyColor, frame.fogColor, kStillSkyFlatten * still);
            frame.starBrightness *= 1.0f - kStillStarDim * still;
        }

        // ── Auroras ─────────────────────────────────────────────────────────
        float aurora = kBaseAurora + (1.0f - kBaseAurora) * m_steppe;
        aurora *= 1.0f - kStillAuroraDim * still;
        aurora *= 1.0f - std::max(m_cavern, m_depth);
        if (water) aurora *= kUnderwaterAurora;
        if (lava)  aurora = 0.0f;
        frame.auroraStrength = std::clamp(aurora, 0.0f, 1.0f);
    }

} // namespace Render

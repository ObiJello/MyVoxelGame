// File: src/client/renderer/environment/EnvironmentState.cpp
//
// All constants below are verbatim from the vendored MC decompile:
//   minecraft_code/.../world/timeline/Timelines.java        (keyframe tracks)
//   minecraft_code/.../util/EasingType.java                 (cubic bezier)
//   minecraft_code/.../util/KeyframeTrackSampler.java       (sampling)
//   minecraft_code/.../data/worldgen/DimensionTypes.java    (base colors)
//   minecraft_code/.../data/worldgen/biome/OverworldBiomes.java (sky color)
//   minecraft_code/.../client/renderer/fog/environment/AtmosphericFogEnvironment.java
//   minecraft_code/.../client/renderer/fog/FogData.java     (fog distances)
#include "EnvironmentState.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace Render {

    namespace {

        constexpr double kDayLength = 24000.0;

        // floored modulo (Java Math.floorMod semantics) for times that can be
        // negative or huge via /time set.
        double FloorMod(double value, double modulus) {
            double r = std::fmod(value, modulus);
            if (r < 0.0) r += modulus;
            return r;
        }

        int64_t FloorDiv(int64_t a, int64_t b) {
            int64_t q = a / b;
            if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
            return q;
        }

        int64_t FloorModI(int64_t a, int64_t b) {
            int64_t r = a % b;
            if (r < 0) r += b;
            return r;
        }

        // ── EasingType.symmetricCubicBezier(0.362, 0.241) ───────────────────
        // CubicCurve coefficients from control values v1, v2:
        //   a = 3v1 - 3v2 + 1, b = -6v1 + 3v2, c = 3v1
        // x-curve controls (0.362, 0.638), y-curve controls (0.241, 0.759).
        struct CubicCurve {
            float a, b, c;
            constexpr CubicCurve(float v1, float v2)
                : a(3.0f * v1 - 3.0f * v2 + 1.0f), b(-6.0f * v1 + 3.0f * v2), c(3.0f * v1) {}
            float Sample(float t) const { return ((a * t + b) * t + c) * t; }
            float Gradient(float t) const { return (3.0f * a * t + 2.0f * b) * t + c; }
        };

        float SkyAngleEase(float x) {
            constexpr float v1x = 0.362f, v1y = 0.241f;
            static constexpr CubicCurve xCurve(v1x, 1.0f - v1x);
            static constexpr CubicCurve yCurve(v1y, 1.0f - v1y);
            // 4 Newton-Raphson iterations solving the x-curve for t.
            float t = x;
            for (int i = 0; i < 4; ++i) {
                float gradient = xCurve.Gradient(t);
                if (gradient < 1.0e-5f) break;
                float error = xCurve.Sample(t) - x;
                t -= error / gradient;
            }
            return yCurve.Sample(t);
        }

        // ── Keyframe tracks (period 24000, LINEAR ease, wrap-around) ────────
        struct FloatKey { int tick; float value; };
        struct ColorKey { int tick; uint32_t argb; };

        // KeyframeTrackSampler.sample: find the segment containing t, with
        // synthetic wrap segments [last-period → first] and [last → first+period].
        template <typename Key, typename Lerp>
        auto SampleTrack(const Key* keys, size_t count, double ticks, Lerp lerp)
            -> decltype(lerp(0.0f, keys[0], keys[0])) {
            const double t = FloorMod(ticks, kDayLength);
            const Key& first = keys[0];
            const Key& last = keys[count - 1];

            // Before the first keyframe: wrap segment last→first.
            if (t <= first.tick) {
                const double from = last.tick - kDayLength;
                const double span = first.tick - from;
                const float alpha = span > 0 ? static_cast<float>((t - from) / span) : 1.0f;
                return lerp(alpha, last, first);
            }
            // Between consecutive keyframes.
            for (size_t i = 0; i + 1 < count; ++i) {
                if (t <= keys[i + 1].tick) {
                    const double from = keys[i].tick;
                    const double span = keys[i + 1].tick - from;
                    const float alpha = span > 0 ? static_cast<float>((t - from) / span) : 1.0f;
                    return lerp(alpha, keys[i], keys[i + 1]);
                }
            }
            // After the last keyframe: wrap segment last→first+period.
            const double from = last.tick;
            const double span = (first.tick + kDayLength) - from;
            const float alpha = span > 0 ? static_cast<float>((t - from) / span) : 1.0f;
            return lerp(alpha, last, first);
        }

        float SampleFloat(const FloatKey* keys, size_t count, double ticks) {
            return SampleTrack(keys, count, ticks,
                [](float alpha, const FloatKey& a, const FloatKey& b) {
                    return a.value + alpha * (b.value - a.value);
                });
        }

        // ARGB.srgbLerp semantics: independent per-channel lerp of the sRGB
        // 0-255 ints (no gamma conversion). Done in float here — identical
        // up to rounding.
        glm::vec4 ArgbToVec4(uint32_t argb) {
            return {
                ((argb >> 16) & 0xFF) / 255.0f,
                ((argb >> 8) & 0xFF) / 255.0f,
                (argb & 0xFF) / 255.0f,
                ((argb >> 24) & 0xFF) / 255.0f,
            };
        }

        glm::vec4 SampleColor(const ColorKey* keys, size_t count, double ticks) {
            return SampleTrack(keys, count, ticks,
                [](float alpha, const ColorKey& a, const ColorKey& b) {
                    return glm::mix(ArgbToVec4(a.argb), ArgbToVec4(b.argb), alpha);
                });
        }

        // ── Timelines.java DAY tracks (verbatim keyframes) ──────────────────
        constexpr uint32_t kWhite = 0xFFFFFFFFu;
        constexpr uint32_t kBlack = 0xFF000000u;
        // ARGB.colorFromFloat floors each channel (Mth.floor(f * 255)):
        // (1, 0.06, 0.06, 0.09) — night fog multiplier
        constexpr uint32_t kNightFog = 0xFF0F0F16u;
        // (1, 0.1, 0.1, 0.15) — night cloud multiplier
        constexpr uint32_t kNightCloud = 0xFF191926u;

        constexpr ColorKey kSkyColorTrack[] = {
            {133, kWhite}, {11867, kWhite}, {13670, kBlack}, {22330, kBlack}};
        constexpr ColorKey kFogColorTrack[] = {
            {133, kWhite}, {11867, kWhite}, {13670, kNightFog}, {22330, kNightFog}};
        constexpr ColorKey kCloudColorTrack[] = {
            {133, kWhite}, {11867, kWhite}, {13670, kNightCloud}, {22330, kNightCloud}};
        constexpr FloatKey kSkyLightTrack[] = {
            {133, 1.0f}, {11867, 1.0f}, {13670, 0.26666668f}, {22330, 0.26666668f}};
        constexpr FloatKey kStarBrightnessTrack[] = {
            {92, 0.037f},   {627, 0.0f},    {11373, 0.0f},   {11732, 0.016f},
            {11959, 0.044f}, {12399, 0.143f}, {12729, 0.258f}, {13228, 0.5f},
            {22772, 0.5f},   {23032, 0.364f}, {23356, 0.225f}, {23758, 0.101f}};
        // SUNRISE_SUNSET_COLOR — signed int32 constants from Timelines.java,
        // stored as the equivalent unsigned ARGB.
        constexpr ColorKey kSunriseColorTrack[] = {
            {71, 0x5FEFA333u},    {310, 0x29F5BA33u},   {565, 0x06FBD433u},
            {730, 0x00FFE533u},   {11270, 0x00FFE533u}, {11397, 0x04FCD833u},
            {11522, 0x0FF9CB33u}, {11690, 0x29F5BA33u}, {11929, 0x5FEFA333u},
            {12243, 0xB1E78733u}, {12358, 0xCCE47E33u}, {12512, 0xE9E07233u},
            {12613, 0xF6DD6B33u}, {12732, 0xFEDA6333u}, {12841, 0xFED75C33u},
            {13035, 0xECD25133u}, {13252, 0xC1CC4733u}, {13775, 0x36BE3733u},
            {13888, 0x1FBB3533u}, {14039, 0x09B73333u}, {14192, 0x00B33333u},
            {21807, 0x00B23333u}, {21961, 0x09B73333u}, {22112, 0x1FBB3533u},
            {22225, 0x36BE3733u}, {22748, 0xC1CC4733u}, {22965, 0xECD25133u},
            {23159, 0xFED75C33u}, {23272, 0xFEDA6333u}, {23488, 0xE9E07233u},
            {23642, 0xCCE47E33u}, {23757, 0xB1E78733u}};

        template <typename T, size_t N>
        constexpr size_t CountOf(const T (&)[N]) { return N; }

        // ── Base colors ─────────────────────────────────────────────────────
        // Mth.hsvToRgb — standard 6-sector HSV, all inputs 0..1.
        glm::vec3 HsvToRgb(float hue, float saturation, float value) {
            int i = static_cast<int>(hue * 6.0f) % 6;
            float f = hue * 6.0f - static_cast<float>(static_cast<int>(hue * 6.0f));
            float p = value * (1.0f - saturation);
            float q = value * (1.0f - f * saturation);
            float t = value * (1.0f - (1.0f - f) * saturation);
            switch (i) {
                case 0: return {value, t, p};
                case 1: return {q, value, p};
                case 2: return {p, value, t};
                case 3: return {p, q, value};
                case 4: return {t, p, value};
                default: return {value, p, q};
            }
        }

        // OverworldBiomes.calculateSkyColor(0.8) — the plains/default sky.
        glm::vec3 BaseSkyColor() {
            float temp = std::clamp(0.8f / 3.0f, -1.0f, 1.0f);
            return HsvToRgb(0.62222224f - temp * 0.05f, 0.5f + temp * 0.1f, 1.0f);
        }

        // DimensionTypes overworld FOG_COLOR = 0xFFC0D8FF.
        const glm::vec3 kBaseFogColor = glm::vec3(ArgbToVec4(0xFFC0D8FFu));
        // CLOUD_COLOR = ARGB.white(0.8) = 0xCCFFFFFF.
        const glm::vec4 kBaseCloudColor{1.0f, 1.0f, 1.0f, 0.8f};

        float ClampedLerp(float alpha, float from, float to) {
            return from + std::clamp(alpha, 0.0f, 1.0f) * (to - from);
        }

    } // namespace

    EnvironmentState& EnvironmentState::Get() {
        static EnvironmentState instance;
        return instance;
    }

    void EnvironmentState::OnTimeSync(uint64_t gameTime, uint64_t dayTime, bool doDaylightCycle) {
        m_pendingGameTime.store(static_cast<int64_t>(gameTime), std::memory_order_relaxed);
        m_pendingDayTime.store(static_cast<int64_t>(dayTime), std::memory_order_relaxed);
        m_pendingRule.store(doDaylightCycle, std::memory_order_relaxed);
        m_hasPending.store(true, std::memory_order_release);
    }

    void EnvironmentState::ApplyPendingSync() {
        if (m_hasPending.exchange(false, std::memory_order_acquire)) {
            m_gameTime = m_pendingGameTime.load(std::memory_order_relaxed);
            m_dayTime = m_pendingDayTime.load(std::memory_order_relaxed);
            m_doDaylightCycle = m_pendingRule.load(std::memory_order_relaxed);
        }
    }

    void EnvironmentState::TickClient() {
        ApplyPendingSync();
        // ClientLevel.tickTime mirror; server resync lands every 20 ticks.
        m_gameTime++;
        if (m_doDaylightCycle) {
            m_dayTime++;
        }
    }

    void EnvironmentState::ResetSession() {
        m_hasPending.store(false, std::memory_order_relaxed);
        m_gameTime = 0;
        m_dayTime = 6000;
        m_doDaylightCycle = false;
    }

    double EnvironmentState::DayTimeF(float partialTick) const {
        return static_cast<double>(m_dayTime) +
               (m_doDaylightCycle ? static_cast<double>(partialTick) : 0.0);
    }

    double EnvironmentState::GameTimeF(float partialTick) const {
        return static_cast<double>(m_gameTime) + static_cast<double>(partialTick);
    }

    void EnvironmentState::UpdateFrame(float partialTick, const glm::vec3& cameraForward,
                                       float cameraY, int renderDistChunks, bool fogEnabled) {
        ApplyPendingSync();
        const double dayTimeF = DayTimeF(partialTick);

        // ── Celestial angles: SUN_ANGLE track's closed form ─────────────────
        // alpha 0 at noon (tick 6000); eased with symmetricCubicBezier.
        const float alphaDay = static_cast<float>(FloorMod(dayTimeF - 6000.0, kDayLength) / kDayLength);
        m_frame.sunAngleDeg = 360.0f * SkyAngleEase(alphaDay);
        m_frame.moonAngleDeg = m_frame.sunAngleDeg + 180.0f;
        m_frame.starAngleDeg = m_frame.sunAngleDeg;
        m_frame.moonPhase = static_cast<int>(FloorModI(FloorDiv(m_dayTime, 24000), 8));

        // ── Attribute colors: base × timeline multiplier ────────────────────
        static const glm::vec3 baseSky = BaseSkyColor();
        const glm::vec4 skyMul = SampleColor(kSkyColorTrack, CountOf(kSkyColorTrack), dayTimeF);
        const glm::vec3 skyColor = baseSky * glm::vec3(skyMul);

        const glm::vec4 fogMul = SampleColor(kFogColorTrack, CountOf(kFogColorTrack), dayTimeF);
        glm::vec3 fogColor = kBaseFogColor * glm::vec3(fogMul);

        const glm::vec4 cloudMul = SampleColor(kCloudColorTrack, CountOf(kCloudColorTrack), dayTimeF);
        m_frame.cloudColor = kBaseCloudColor * cloudMul;

        m_frame.skyBrightness = SampleFloat(kSkyLightTrack, CountOf(kSkyLightTrack), dayTimeF);
        m_frame.starBrightness = SampleFloat(kStarBrightnessTrack, CountOf(kStarBrightnessTrack), dayTimeF);
        m_frame.sunriseColor = SampleColor(kSunriseColorTrack, CountOf(kSunriseColorTrack), dayTimeF);
        m_frame.skyColor = skyColor;

        // ── Fog color ───────────────────────────────────────────────────────
        if (m_skyboxActive) {
            // Cubemap skybox: fog fades toward the skybox's horizon color
            // instead of the timeline sky composition. Static mode keeps it
            // constant (End-style); darken modes ride the night fog curve.
            m_frame.fogColor = (m_skyboxMode == 0)
                ? m_skyboxFogBase
                : m_skyboxFogBase * glm::vec3(fogMul);
        } else {
            // AtmosphericFogEnvironment.getBaseColor.
            const float sunAngleRad = glm::radians(m_frame.sunAngleDeg);
            if (renderDistChunks >= 4) {
                const float sunX = std::sin(sunAngleRad) > 0.0f ? -1.0f : 1.0f;
                const float facingSun = glm::dot(cameraForward, glm::vec3(sunX, 0.0f, 0.0f));
                const float sunriseAlpha = m_frame.sunriseColor.a;
                if (facingSun > 0.0f && sunriseAlpha > 0.0f) {
                    fogColor = glm::mix(fogColor, glm::vec3(m_frame.sunriseColor),
                                        facingSun * sunriseAlpha);
                }
            }
            // Blend fog toward the sky color by render distance.
            const float skyFogEndChunks =
                std::min(512.0f / 16.0f, static_cast<float>(renderDistChunks));
            float mixFactor = ClampedLerp(skyFogEndChunks / 32.0f, 0.25f, 1.0f);
            mixFactor = 1.0f - std::pow(mixFactor, 0.25f);
            m_frame.fogColor = glm::mix(fogColor, skyColor, mixFactor);
        }

        // ── Fog distances (FogData; blocks) ─────────────────────────────────
        const float renderDistBlocks = static_cast<float>(renderDistChunks) * 16.0f;
        m_frame.fogSkyEnd = std::min(renderDistBlocks, 512.0f);
        m_frame.fogCloudEnd = std::min(renderDistBlocks, 2048.0f);
        if (fogEnabled) {
            const float span = std::clamp(renderDistBlocks / 10.0f, 4.0f, 64.0f);
            m_frame.fogEnvStart = 0.0f;
            m_frame.fogEnvEnd = 1024.0f;
            m_frame.fogRdStart = renderDistBlocks - span;
            m_frame.fogRdEnd = renderDistBlocks;
        } else {
            // Fog OFF: push all terrain fog out of reach (no shader branch).
            m_frame.fogEnvStart = 1e9f;
            m_frame.fogEnvEnd = 1e9f;
            m_frame.fogRdStart = 1e9f;
            m_frame.fogRdEnd = 1e9f;
        }

        // Dark disc below the horizon line (ClientLevel.getHorizonHeight = 63).
        m_showDarkDisc = (cameraY - 63.0f) < 0.0f;
    }

} // namespace Render

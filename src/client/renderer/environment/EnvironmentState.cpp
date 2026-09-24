// File: src/client/renderer/environment/EnvironmentState.cpp
//
// All constants below are verbatim from the vendored MC decompile:
//   minecraft_code_26.1-snapshot-1/.../world/timeline/Timelines.java        (keyframe tracks)
//   minecraft_code_26.1-snapshot-1/.../util/EasingType.java                 (cubic bezier)
//   minecraft_code_26.1-snapshot-1/.../util/KeyframeTrackSampler.java       (sampling)
//   minecraft_code_26.1-snapshot-1/.../data/worldgen/DimensionTypes.java    (base colors)
//   minecraft_code_26.1-snapshot-1/.../data/worldgen/biome/OverworldBiomes.java (sky color)
//   minecraft_code_26.1-snapshot-1/.../client/renderer/fog/environment/AtmosphericFogEnvironment.java
//   minecraft_code_26.1-snapshot-1/.../client/renderer/fog/FogData.java     (fog distances)
//
// The two ported mod dimensions (docs/mod-ports.md), from their sources under
// mods_reference/ (see the Atmosphere enum):
//   twilightforest/.../client/TwilightForestRenderInfo.java, renderer/TFSkyRenderer.java,
//     event/FogHandler.java; data/twilightforest/dimension_type/twilight_forest_type.json
//     and worldgen/biome/*.json (the attributes); MC 26.1 GaussianSampler /
//     EnvironmentAttributeProbe (how the biome attributes blend)
//   aether/.../client/renderer/level/AetherSkyRenderEffects.java,
//     client/event/hooks/DimensionClientHooks.java, attachment/AetherTimeAttachment.java,
//     mixin/mixins/common/DimensionTypeMixin.java; MC 1.21.1 FogRenderer.setupColor,
//     Level.getSkyDarken / getStarBrightness, LightTexture (the base the Aether edits)
#include "EnvironmentState.hpp"
#include "Lightmap.hpp"
#include "SkyRenderer.hpp"   // the Hush's fog and sky constants (kHushFog / kHushSky)
#include "HushAtmosphere.hpp" // the Hush's auroras, cavern fog and stillness
#include "MobEffectEnvironment.hpp" // the local player's effect fog / night vision / darkness
#include "client/world/ClientChunkManager.hpp"   // biomes around the camera (Twilight Forest)
#include <string_view>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace Render {

    glm::vec3 WaterFogColorForBiome(Game::BiomeId biome);

    // Underwater in resonant water (the river Vesper): a luminous violet,
    // #5E46BC — the river's sprite colour deepened, bright enough to read as
    // lit water, not a dark violet murk.
    const glm::vec3 kResonantWaterFogColor{0x5E / 255.0f, 0x46 / 255.0f, 0xBC / 255.0f};

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
        // The Hush's pinned sky brightness (see the fixed-night block in
        // UpdateFrame). Overworld midnight is 0.2667, noon 1.0.
        constexpr float kHushSkyBrightness = 0.42f;

        constexpr FloatKey kSkyLightTrack[] = {
            {133, 1.0f}, {11867, 1.0f}, {13670, 0.26666668f}, {22330, 0.26666668f}};
        // The DAY timeline's lightmap tracks (26.x timeline/day.json):
        // visual/sky_light_factor and visual/sky_light_color.
        constexpr FloatKey kSkyLightFactorTrack[] = {
            {730, 1.0f}, {11270, 1.0f}, {13140, 0.24f}, {22860, 0.24f}};
        constexpr ColorKey kSkyLightColorTrack[] = {
            {730, 0xFFFFFFFFu}, {11270, 0xFFFFFFFFu}, {13140, 0xFF7A7AFFu}, {22860, 0xFF7A7AFFu}};
        // The Hush's sky light at its fixed midnight: lifted from the
        // Overworld's 0.24 floor to the same dusk-like level its old terrain
        // dim sat at (kHushSkyBrightness) — still clearly night under the
        // moonlit #7a7aff, with the glowing flora and crystals carrying the
        // scene — so a world meant for hours of exploring stays readable.
        constexpr float kHushSkyLightFactor = 0.42f;
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

        glm::vec3 Rgb24(uint32_t rgb) {
            return glm::vec3(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f,
                             (rgb & 0xFF) / 255.0f);
        }

        // AtmosphericFogEnvironment's sky-colour mix: fog → sky by how far
        // the sky fog reaches (SKY_FOG_END_DISTANCE / 16, capped by the
        // render distance), 1 − (clampedLerp(end/32, 0.25, 1))^0.25.
        float SkyFogMixFactor(float skyFogEndBlocks, int renderDistChunks) {
            const float endChunks = std::min(skyFogEndBlocks / 16.0f, static_cast<float>(renderDistChunks));
            const float m = ClampedLerp(endChunks / 32.0f, 0.25f, 1.0f);
            return 1.0f - std::pow(m, 0.25f);
        }

        // ── Twilight Forest: the biome attributes ────────────────────────────
        //
        // data/twilightforest/worldgen/biome/*.json `attributes` over the
        // dimension type's (twilight_forest_type.json: sky_light_factor 0.35,
        // no fog/sky colour of its own, no cloud colour — so no clouds, the
        // attribute's default being transparent). Only the attributes the
        // sky and fog read.
        struct TwilightLook {
            glm::vec3 fog{0.0f};
            glm::vec3 sky{0.0f};
            float skyLightFactor = 0.35f;   // twilight_forest_type.json
            float fogStart       = 0.0f;    // FOG_START_DISTANCE default
            float fogEnd         = 1024.0f; // FOG_END_DISTANCE default
            float skyFogEnd      = 512.0f;  // SKY_FOG_END_DISTANCE default
        };

        TwilightLook TwilightLookForName(std::string_view name) {
            TwilightLook look;
            // The common look: clearing, dense(_mushroom)_forest, enchanted,
            // final_plateau, firefly_forest, forest, highlands(_underground),
            // lake, mushroom_forest, oak_savannah, stream, thornlands,
            // underground — fog #c0ffd8, sky #20224a.
            look.fog = Rgb24(0xC0FFD8);
            look.sky = Rgb24(0x20224A);
            if (name == "twilightforest:dark_forest") {
                look.fog = Rgb24(0x000000); look.sky = Rgb24(0x000000); look.skyLightFactor = 0.0f;
            } else if (name == "twilightforest:dark_forest_center") {
                look.fog = Rgb24(0x493000); look.sky = Rgb24(0x000000); look.skyLightFactor = 0.24f;
            } else if (name == "twilightforest:fire_swamp") {
                look.fog = Rgb24(0x380A00); look.sky = Rgb24(0x002112);
            } else if (name == "twilightforest:glacier") {
                look.fog = Rgb24(0x361F88); look.sky = Rgb24(0x130D28);
            } else if (name == "twilightforest:snowy_forest") {
                look.fog = Rgb24(0xFFFFFF); look.sky = Rgb24(0x808080);
            } else if (name == "twilightforest:spooky_forest") {
                look.fog = Rgb24(0x827391);
                look.fogStart = 16.0f; look.fogEnd = 64.0f; look.skyFogEnd = 64.0f;
            } else if (name == "twilightforest:swamp") {
                look.fog = Rgb24(0x003F21); look.sky = Rgb24(0x002112);
            }
            // Anything else — the plains an unloaded chunk answers with
            // (kFallbackBiomeId) — reads as the TF forest rather than as a
            // biome with no fog colour, which would flash the edge of the
            // loaded world black.
            return look;
        }

        const TwilightLook& TwilightLookFor(Game::BiomeId biome) {
            static const std::vector<TwilightLook> table = [] {
                std::vector<TwilightLook> t(Game::BiomeRegistry::Count());
                for (Game::BiomeId i = 0; i < t.size(); ++i) {
                    t[i] = TwilightLookForName(Game::BiomeRegistry::Get(i).name);
                }
                return t;
            }();
            static const TwilightLook kDefault = TwilightLookForName("twilightforest:forest");
            return biome < table.size() ? table[biome] : kDefault;
        }

        // MC 26.1 EnvironmentAttributeProbe.tick + GaussianSampler.sample: the
        // biomes of the 6×6×6 quart cells around position/4, weighted by the
        // kernel {0, 1, 4, 6, 4, 1, 0} lerped by the fractional position, and
        // SpatialAttributeInterpolator's running lerp — the weighted mean.
        // That blend is what eases the dark forest's black in and out as the
        // camera crosses its border.
        TwilightLook SampleTwilightLook(const glm::dvec3& position) {
            static constexpr double kKernel[7] = {0.0, 1.0, 4.0, 6.0, 4.0, 1.0, 0.0};
            const glm::dvec3 p = position * 0.25 - glm::dvec3(0.5);
            const int ix = static_cast<int>(std::floor(p.x));
            const int iy = static_cast<int>(std::floor(p.y));
            const int iz = static_cast<int>(std::floor(p.z));
            const double rx = p.x - ix, ry = p.y - iy, rz = p.z - iz;
            double total = 0.0;
            glm::dvec3 fog(0.0), sky(0.0);
            double factor = 0.0, fogStart = 0.0, fogEnd = 0.0, skyFogEnd = 0.0;
            for (int z = 0; z < 6; ++z) {
                const double wz = kKernel[z + 1] + rz * (kKernel[z] - kKernel[z + 1]);
                for (int x = 0; x < 6; ++x) {
                    const double wx = kKernel[x + 1] + rx * (kKernel[x] - kKernel[x + 1]);
                    for (int y = 0; y < 6; ++y) {
                        const double wy = kKernel[y + 1] + ry * (kKernel[y] - kKernel[y + 1]);
                        const double w = wx * wy * wz;
                        if (w <= 0.0) continue;
                        // getNoiseBiomeAtQuart: quart q is blocks 4q..4q+3.
                        const Game::BiomeId biome = ::Client::g_clientChunkManager->BiomeAtWorld(
                            (ix - 2 + x) * 4, (iy - 2 + y) * 4, (iz - 2 + z) * 4);
                        const TwilightLook& look = TwilightLookFor(biome);
                        total     += w;
                        fog       += w * glm::dvec3(look.fog);
                        sky       += w * glm::dvec3(look.sky);
                        factor    += w * look.skyLightFactor;
                        fogStart  += w * look.fogStart;
                        fogEnd    += w * look.fogEnd;
                        skyFogEnd += w * look.skyFogEnd;
                    }
                }
            }
            if (total <= 0.0) return TwilightLookFor(Game::kFallbackBiomeId);
            TwilightLook out;
            out.fog            = glm::vec3(fog / total);
            out.sky            = glm::vec3(sky / total);
            out.skyLightFactor = static_cast<float>(factor / total);
            out.fogStart       = static_cast<float>(fogStart / total);
            out.fogEnd         = static_cast<float>(fogEnd / total);
            out.skyFogEnd      = static_cast<float>(skyFogEnd / total);
            return out;
        }

        // TF FogHandler.colorFog's `daylight`: DimensionType.timeOfDay's
        // curve at the dimension's fixed time 13000, fed through the old
        // brightness curve — ≈ 0.379, a dusk.
        float TwilightFogDaylight() {
            const double time = 13000.0;
            double d0 = time / 24000.0 - 0.25;
            d0 -= std::floor(d0);                                   // Mth.frac
            const double d1 = 0.5 - std::cos(d0 * 3.141592653589793) / 2.0;
            const double d2 = static_cast<float>(d0 * 2.0 + d1) / 3.0f;
            const float c = static_cast<float>(std::cos(d2 * 6.283185307179586));
            return std::clamp(c * 2.0f + 0.5f, 0.0f, 1.0f);
        }

        // ── The Aether (1.21.1 formulas) ─────────────────────────────────────
        //
        // All four Aether biomes share the one look: sky_color 12632319
        // (#C0C0FF), fog_color 9671612 (#9393BC) — so the 1.21.1 biome
        // blend (CubicSampler.gaussianSampleVec3) is the constant itself.
        const glm::vec3 kAetherSky = Rgb24(0xC0C0FF);
        const glm::vec3 kAetherFog = Rgb24(0x9393BC);
        // AetherTimeAttachment.getTicksPerDay with the default config
        // (neither normal_length_aether_time nor sync_aether_time): 3 days.
        constexpr double kAetherTicksPerDay = 24000.0 * 3.0;
        // AetherSkyRenderEffects(9.5F, …) — DimensionSpecialEffects.cloudLevel
        // — plus the 0.33 renderClouds adds under it.
        constexpr float kAetherCloudBottomY = 9.5f + 0.33f;

        // DimensionTypeMixin.timeOfDay: DimensionType.timeOfDay over the
        // Aether's day length.
        float AetherTimeOfDay(double dayTime) {
            double d0 = dayTime / kAetherTicksPerDay - 0.25;
            d0 -= std::floor(d0);                                   // Mth.frac
            const double d1 = 0.5 - std::cos(d0 * 3.141592653589793) / 2.0;
            return static_cast<float>(d0 * 2.0 + d1) / 3.0f;
        }

        // 1.21.1's day-brightness curve, clamp(cos(t·2π)·2 + 0.5, 0, 1): the
        // sky, cloud and fog brightness of ClientLevel.getSkyColor /
        // getCloudColor and FogRenderer.setupColor.
        float AetherDayBrightness(float timeOfDay) {
            return std::clamp(std::cos(timeOfDay * 6.2831855f) * 2.0f + 0.5f, 0.0f, 1.0f);
        }

        // Luminance-weighted darkening toward grey, the shape every 1.21.1
        // weather adjustment takes: c·k + lum·s·(1 − k) with k = 1 − level·r.
        glm::vec3 WeatherGrey(const glm::vec3& c, float level, float greyScale, float reach) {
            if (level <= 0.0f) return c;
            const float grey = (c.r * 0.3f + c.g * 0.59f + c.b * 0.11f) * greyScale;
            const float keep = 1.0f - level * reach;
            return c * keep + glm::vec3(grey) * (1.0f - keep);
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
        // MC LocalPlayer.aiStep: the eyes adjust to water one tick at a
        // time (ten for a spectator), and readjust ten a tick out of it.
        if (m_cameraFluid == kFluidWater) {
            m_waterVisionTime = glm::clamp(m_waterVisionTime + (m_cameraSpectator ? 10 : 1), 0, 600);
        } else if (m_waterVisionTime > 0) {
            m_waterVisionTime = glm::clamp(m_waterVisionTime - 10, 0, 600);
        }
        ApplyPendingSync();
        // ClientLevel.tickTime mirror; server resync lands every 20 ticks.
        m_gameTime++;
        if (m_doDaylightCycle) {
            m_dayTime++;
        }
        // MC ClientLevel.tick: `if (skyFlashTime > 0) --skyFlashTime`.
        if (m_skyFlashTime > 0) --m_skyFlashTime;
    }

    void EnvironmentState::ResetSession() {
        m_hasPending.store(false, std::memory_order_relaxed);
        m_gameTime = 0;
        m_dayTime = 6000;
        m_doDaylightCycle = false;
        m_skyFlashTime = 0;
    }

    double EnvironmentState::DayTimeF(float partialTick) const {
        return static_cast<double>(m_dayTime) +
               (m_doDaylightCycle ? static_cast<double>(partialTick) : 0.0);
    }

    double EnvironmentState::GameTimeF(float partialTick) const {
        return static_cast<double>(m_gameTime) + static_cast<double>(partialTick);
    }

    EnvironmentFrame EnvironmentState::FrameForDimension(Game::DimensionId dimension, int renderDistChunks) {
        // Recompose with the dimension's rules in place of the current
        // ones, capture, and put everything back. The Nether and the End
        // values are the ones SkyRenderer::ApplyDimensionSky installs.
        const bool      savedAmbient  = m_constantAmbientLight;
        const bool      savedNight    = m_fixedNight;
        const bool      savedSkybox   = m_skyboxActive;
        const glm::vec3 savedFogBase  = m_skyboxFogBase;
        const int       savedMode     = m_skyboxMode;
        const bool      savedDarkDisc = m_showDarkDisc;
        const Atmosphere savedAtmosphere = m_atmosphere;
        const Game::DimensionId savedLightDimension = m_lightDimension;
        m_lightDimension = dimension;
        const bool      savedForeign  = m_composingForeign;
        const EnvironmentFrame savedFrame = m_frame;
        const EnvironmentFrame* savedOverride = m_frameOverride;
        m_frameOverride = nullptr;
        // The far side of a portal is composed as seen through the portal,
        // not from inside whatever fluid this camera happens to be in.
        const int savedCameraFluid = m_cameraFluid;
        m_cameraFluid = kFluidNone;

        switch (dimension) {
            case Game::DimensionId::Nether:
                m_constantAmbientLight = true;
                m_fixedNight = false;
                m_skyboxActive = true;
                m_skyboxFogBase = glm::vec3(0x33 / 255.0f, 0x08 / 255.0f, 0x08 / 255.0f);
                m_skyboxMode = 0;
                break;
            case Game::DimensionId::End:
                m_constantAmbientLight = true;
                m_fixedNight = false;
                m_skyboxActive = true;
                // SkyRenderer's End fog: the end_sky tint (0x28) scaled 0.35.
                m_skyboxFogBase = glm::vec3(0x28 / 255.0f) * 0.35f;
                m_skyboxMode = 0;
                break;
            case Game::DimensionId::Hush:
                // An open sky frozen at midnight: no cycle to ride (constant
                // ambient), pinned to the night floor (fixed night), and a
                // constant teal fog in place of the timeline composition —
                // the same three SkyRenderer::ApplyDimensionSky installs.
                m_constantAmbientLight = true;
                m_fixedNight = true;
                m_skyboxActive = true;
                m_skyboxFogBase = glm::vec3(SkyRenderer::kHushFog[0], SkyRenderer::kHushFog[1],
                                            SkyRenderer::kHushFog[2]);
                m_skyboxMode = 0;
                break;
            // The two mod dimensions: an open sky with their own composition
            // (see Atmosphere) — what SkyRenderer::ApplyDimensionSky installs.
            case Game::DimensionId::TwilightForest:
            case Game::DimensionId::Aether:
            case Game::DimensionId::Overworld:
                m_constantAmbientLight = false;
                m_fixedNight = false;
                // The vanilla sky: a cubemap chosen in settings only exists
                // while the overworld is the active level.
                m_skyboxActive = false;
                break;
        }
        const Atmosphere target =
            dimension == Game::DimensionId::TwilightForest ? Atmosphere::TwilightForest
          : dimension == Game::DimensionId::Aether         ? Atmosphere::Aether
                                                           : Atmosphere::Vanilla;
        // The far dimension's biomes are not around this camera: unless it
        // is the dimension the camera is in, its attributes are its defaults.
        m_composingForeign = (target != savedAtmosphere);
        m_atmosphere = target;
        const int savedRenderDist = m_lastRenderDistChunks;
        m_recomposing = true;
        UpdateFrame(m_lastPartialTick, m_lastCameraForward, m_lastCameraY,
                    renderDistChunks > 0 ? renderDistChunks : m_lastRenderDistChunks, m_lastFogEnabled);
        m_recomposing = false;
        const EnvironmentFrame result = m_frame;

        m_lastRenderDistChunks = savedRenderDist;
        m_cameraFluid = savedCameraFluid;
        m_constantAmbientLight = savedAmbient;
        m_fixedNight = savedNight;
        m_skyboxActive = savedSkybox;
        m_skyboxFogBase = savedFogBase;
        m_skyboxMode = savedMode;
        m_showDarkDisc = savedDarkDisc;
        m_atmosphere = savedAtmosphere;
        m_lightDimension = savedLightDimension;
        m_composingForeign = savedForeign;
        m_frame = savedFrame;
        m_frameOverride = savedOverride;
        return result;
    }

    void EnvironmentState::UpdateFrame(float partialTick, const glm::vec3& cameraForward,
                                       float cameraY, int renderDistChunks, bool fogEnabled) {
        m_lastPartialTick      = partialTick;
        m_lastCameraForward    = cameraForward;
        m_lastCameraY          = cameraY;
        m_lastRenderDistChunks = renderDistChunks;
        m_lastFogEnabled       = fogEnabled;
        ApplyPendingSync();
        const double dayTimeF = DayTimeF(partialTick);

        // ── Celestial angles: SUN_ANGLE track's closed form ─────────────────
        // alpha 0 at noon (tick 6000); eased with symmetricCubicBezier.
        const float alphaDay = static_cast<float>(FloorMod(dayTimeF - 6000.0, kDayLength) / kDayLength);
        m_frame.sunAngleDeg = 360.0f * SkyAngleEase(alphaDay);
        m_frame.moonAngleDeg = m_frame.sunAngleDeg + 180.0f;
        m_frame.starAngleDeg = m_frame.sunAngleDeg;
        m_frame.moonPhase = static_cast<int>(FloorModI(FloorDiv(m_dayTime, 24000), 8));
        // Vanilla values; the Aether's composition below replaces them.
        m_frame.sunAlpha     = 1.0f;
        m_frame.moonAlpha    = 1.0f;
        m_frame.cloudBottomY = 192.33f;   // DimensionTypes overworld CLOUD_HEIGHT
        m_frame.auroraStrength = 0.0f;    // the Hush's layer below sets it

        // ── Attribute colors: base × timeline multiplier ────────────────────
        static const glm::vec3 baseSky = BaseSkyColor();
        const glm::vec4 skyMul = SampleColor(kSkyColorTrack, CountOf(kSkyColorTrack), dayTimeF);
        glm::vec3 skyColor = baseSky * glm::vec3(skyMul);
        // MC ClientLevel's SKY_COLOR layer while a lightning bolt flashes:
        // ARGB.srgbLerp(0.22, skyColor, (0.8, 0.8, 1.0)) — a plain lerp.
        if (m_skyFlashTime > 0) skyColor = glm::mix(skyColor, glm::vec3(0.8f, 0.8f, 1.0f), 0.22f);

        const glm::vec4 fogMul = SampleColor(kFogColorTrack, CountOf(kFogColorTrack), dayTimeF);
        glm::vec3 fogColor = kBaseFogColor * glm::vec3(fogMul);

        const glm::vec4 cloudMul = SampleColor(kCloudColorTrack, CountOf(kCloudColorTrack), dayTimeF);
        m_frame.cloudColor = kBaseCloudColor * cloudMul;

        m_frame.skyBrightness = SampleFloat(kSkyLightTrack, CountOf(kSkyLightTrack), dayTimeF);
        ComposeLightAttributes(dayTimeF);
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

        // The mod dimensions' own sky and fog, in place of the vanilla
        // composition above (see Atmosphere); the fluid fog below still
        // replaces the atmospheric fog in both.
        if (m_atmosphere == Atmosphere::TwilightForest) {
            ComposeTwilightForest(renderDistChunks, fogEnabled);
        } else if (m_atmosphere == Atmosphere::Aether) {
            ComposeAether(partialTick, cameraForward, renderDistChunks, fogEnabled);
        }

        // ── Fluid fog (MC FogRenderer: WaterFogEnvironment / LavaFogEnvironment)
        //
        // Replaces the atmospheric fog outright while the camera is in a
        // fluid: colour from the biome's WATER_FOG_COLOR (a flat 0x991900
        // for lava), distances from the attributes — water -8..96 blocks
        // scaled by how far the eyes have adjusted, lava 0.25..1 (a
        // spectator sees -8..half the render distance). The sky and cloud
        // fades close to the same distance. Not gated on the Fog option:
        // vanilla's toggle only pushes the render-distance fog away.
        if (m_cameraFluid == kFluidWater) {
            m_frame.fogColor    = WaterFogColorForBiome(m_cameraBiome);
            m_frame.fogEnvStart = -8.0f;
            m_frame.fogEnvEnd   = 96.0f * std::max(0.25f, WaterVision());
            // TF's fire swamp, spooky forest and swamp: WATER_FOG_END_DISTANCE
            // {"modifier": "multiply", "argument": 0.85}.
            {
                const std::string_view biomeName = Game::BiomeRegistry::Get(m_cameraBiome).name;
                if (biomeName == "twilightforest:fire_swamp" ||
                    biomeName == "twilightforest:spooky_forest" ||
                    biomeName == "twilightforest:swamp") {
                    m_frame.fogEnvEnd *= 0.85f;
                }
            }
            m_frame.fogSkyEnd   = m_frame.fogEnvEnd;
            m_frame.fogCloudEnd = m_frame.fogEnvEnd;
        } else if (m_cameraFluid == kFluidLava) {
            m_frame.fogColor = glm::vec3(0x99 / 255.0f, 0x19 / 255.0f, 0x00 / 255.0f);   // ARGB -6743808
            if (m_cameraSpectator) {
                m_frame.fogEnvStart = -8.0f;
                m_frame.fogEnvEnd   = renderDistBlocks * 0.5f;
            } else {
                // FIRE_RESISTANCE would widen this to 0..5; no effect state
                // reaches the client's environment yet.
                m_frame.fogEnvStart = 0.25f;
                m_frame.fogEnvEnd   = 1.0f;
            }
            m_frame.fogSkyEnd   = m_frame.fogEnvEnd;
            m_frame.fogCloudEnd = m_frame.fogEnvEnd;
        }

        // TF FogHandler.colorFog runs on ComputeFogColor, after the fluid
        // fog chose its colour — so it has the last word in every medium.
        if (m_atmosphere == Atmosphere::TwilightForest) ApplyTwilightFogHandler();

        // Nether / End: no day-night cycle at all. Applied after the whole
        // timeline composition rather than by branching around it, so the one
        // place that decides "does time of day affect the look" is here.
        if (m_constantAmbientLight) {
            m_frame.skyBrightness  = 1.0f;
            m_frame.starBrightness = 0.0f;
            m_frame.sunriseColor   = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
            // the_nether.json / the_end.json: sky_light_factor 0 — the Nether
            // has no sky light to scale, the End's shows only in its flashes.
            m_frame.skyLightFactor = 0.0f;
        }

        // The Hush: a fixed night rather than a fixed noon. The Overworld's
        // clock still ran the composition above (it is the only clock the
        // client has), so the time-dependent outputs are re-pinned to what
        // the DAY timeline gives at the dimension's fixed time —
        // DimensionFixedTime(Hush) = 18000, MC midnight, where SKY_LIGHT sits
        // at its 0.26666668 floor and STAR_BRIGHTNESS at its 0.5 ceiling —
        // and the sky disc to the Hush's own colour, which shader packs read
        // through the frame (ShaderPipeline's `skyColor`) as SkyRenderer
        // draws it. Fog is already the constant kHushFog via the skybox
        // override, mode 0. After the constant-ambient block on purpose:
        // both are set for the Hush, and this one is the refinement.
        if (m_fixedNight) {
            const double fixedTime = static_cast<double>(
                Game::DimensionFixedTime(Game::DimensionId::Hush).value_or(18000));
            // The Overworld's midnight floor (0.2667) is too dark for a world
            // you are meant to explore for hours: the Hush is lifted to a
            // dusk-like 0.42 — well below day (1.0), still clearly night, and
            // the glowing flora and crystals still carry the scene. The
            // server's sky darken (spawning) is untouched.
            m_frame.skyBrightness  = std::max(
                SampleFloat(kSkyLightTrack, CountOf(kSkyLightTrack), fixedTime), kHushSkyBrightness);
            m_frame.starBrightness = SampleFloat(kStarBrightnessTrack, CountOf(kStarBrightnessTrack), fixedTime);
            m_frame.sunriseColor   = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
            m_frame.skyColor       = glm::vec3(SkyRenderer::kHushSky[0], SkyRenderer::kHushSky[1],
                                               SkyRenderer::kHushSky[2]);
            // The lightmap at the fixed midnight (see kHushSkyLightFactor).
            m_frame.skyLightFactor = kHushSkyLightFactor;
            m_frame.skyLightColor  = glm::vec3(SampleColor(kSkyLightColorTrack, CountOf(kSkyLightColorTrack), fixedTime));
            // What changes over that constant night: the auroras, the cavern
            // fog, the teal water and the stillness (HushAtmosphere.hpp).
            // After the fluid fog, which it refines rather than replaces.
            HushAtmosphere::Inputs hush;
            hush.cameraPos    = m_cameraPos;
            hush.hasCameraPos = m_hasCameraPos && !m_composingForeign;
            hush.cameraFluid  = m_cameraFluid;
            hush.cameraBiome  = m_cameraBiome;
            hush.fogEnabled   = fogEnabled;
            HushAtmosphere::Get().Compose(m_frame, hush);
        }

        // Aurelith's river (resonant water): its own underwater colour, a
        // luminous violet, in every dimension. After the Hush's refinement
        // (which would pull any water toward its teal) so the river keeps
        // its colour; the distances — MC's water curve, as the Hush adjusted
        // them — are left alone. Before the mob effects, which outrank it.
        if (m_cameraResonant && m_cameraFluid == kFluidWater) {
            m_frame.fogColor = kResonantWaterFogColor;
        }

        // MC ClientLevel's SKY_LIGHT_FACTOR layer while a lightning bolt
        // flashes: forced to 1 — the whole world lights up at night. Last, so
        // it wins over every dimension rule above.
        if (m_skyFlashTime > 0) {
            m_frame.skyBrightness  = 1.0f;
            m_frame.skyLightFactor = 1.0f;
        }

        // The local player's status effects (BLINDNESS / DARKNESS fog, NIGHT
        // VISION, the lava FIRE_RESISTANCE fog) — MobEffectEnvironment.hpp.
        // Last of all: MC's mob-effect fog environments outrank the fluid
        // and atmospheric ones, and the lightmap applies them on top of the
        // sky factor.
        ApplyMobEffectsToFrame(m_frame, static_cast<float>(renderDistChunks) * 16.0f, m_cameraFluid);

        // Dark disc below the horizon line (ClientLevel.getHorizonHeight = 63).
        // Never in a dimension with no sky — there is no horizon to be below.
        // TF: TFSkyRenderer.shouldDarkenSky lowers the line to the
        // dimension's floor (eye Y − level.getMinY() < 0, minY −32). The
        // Aether: AetherSkyRenderEffects.renderSky draws no dark disc.
        float horizonY = 63.0f;
        if (m_atmosphere == Atmosphere::TwilightForest) {
            horizonY = static_cast<float>(Game::DimensionMinY(Game::DimensionId::TwilightForest));
        }
        m_showDarkDisc = !m_constantAmbientLight && m_atmosphere != Atmosphere::Aether &&
                         (cameraY - horizonY) < 0.0f;

        // The camera's lightmap for this frame (not a portal's recomposition,
        // whose frame gets its own texture through Lightmap::TextureFor).
        if (!m_recomposing) {
            Lightmap& lightmap = Lightmap::Get();
            if (lightmap.MainTexture() == INVALID_TEXTURE) lightmap.Initialize();
            lightmap.Update(m_frame, m_gameTime);
        }
    }

    void EnvironmentState::ComposeLightAttributes(double dayTimeF) {
        // The DAY timeline (Overworld and every dimension riding its clock),
        // and the dimension's AMBIENT_LIGHT_COLOR / SKY_LIGHT_COLOR
        // (26.x dimension_type attributes). The constant-ambient, fixed-night
        // and mod compositions refine these later in UpdateFrame.
        m_frame.skyLightFactor = SampleFloat(kSkyLightFactorTrack, CountOf(kSkyLightFactorTrack), dayTimeF);
        m_frame.skyLightColor  = glm::vec3(SampleColor(kSkyLightColorTrack, CountOf(kSkyLightColorTrack), dayTimeF));
        m_frame.blockLightTint = glm::vec3(1.0f, 216.0f / 255.0f, 140.0f / 255.0f);
        switch (m_lightDimension) {
            case Game::DimensionId::Nether:
                m_frame.ambientLightColor = glm::vec3(0x30, 0x28, 0x21) / 255.0f;
                m_frame.skyLightColor     = glm::vec3(0x7A, 0x7A, 0xFF) / 255.0f;
                break;
            case Game::DimensionId::End:
                m_frame.ambientLightColor = glm::vec3(0x3F, 0x47, 0x3F) / 255.0f;
                m_frame.skyLightColor     = glm::vec3(0xAC, 0x60, 0xCD) / 255.0f;
                break;
            default:
                m_frame.ambientLightColor = glm::vec3(0x0A) / 255.0f;
                break;
        }
    }

    void EnvironmentState::ComposeTwilightForest(int renderDistChunks, bool fogEnabled) {
        // The biome attributes around the camera (MC EnvironmentAttributeProbe),
        // or the dimension's defaults for a view from outside it.
        const TwilightLook look = (!m_composingForeign && m_hasCameraPos && ::Client::g_clientChunkManager)
            ? SampleTwilightLook(m_cameraPos)
            : TwilightLookFor(Game::kFallbackBiomeId);

        // The dimension's timelines are "#minecraft:universal" — no DAY
        // track — so every time-driven attribute sits at its default: the
        // sun, moon and star angles 0, moon phase 0 (full), no sunrise
        // colour. TFSkyRenderer draws no sun, moon or sunrise glow anyway,
        // and its stars (twice vanilla's, at full brightness — the
        // dimension's star_brightness is 1.0) turn with no time at all.
        m_frame.sunAngleDeg    = 0.0f;
        m_frame.moonAngleDeg   = 0.0f;
        m_frame.starAngleDeg   = 0.0f;
        m_frame.moonPhase      = 0;
        m_frame.sunriseColor   = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
        m_frame.starBrightness = 1.0f;
        m_frame.sunAlpha       = 0.0f;
        m_frame.moonAlpha      = 0.0f;
        // CLOUD_COLOR's default is transparent and neither the dimension
        // type nor any TF biome sets one: MC's addCloudsPass skips the
        // layer, so the Twilight Forest has no clouds.
        m_frame.cloudColor     = glm::vec4(0.0f);

        // SKY_COLOR (biome), with ClientLevel's lightning-flash layer.
        glm::vec3 sky = look.sky;
        if (m_skyFlashTime > 0) sky = glm::mix(sky, glm::vec3(0.8f, 0.8f, 1.0f), 0.22f);
        m_frame.skyColor = sky;

        // SKY_LIGHT_FACTOR: 0.35 across the dimension, the dark forest 0 and
        // its centre 0.24. This engine has no light engine — the factor dims
        // the whole terrain, torch-lit or not — so it is held at the
        // Overworld's night floor instead of going to black; the dark
        // forest's black comes from its fog and sky.
        m_frame.skyBrightness = std::max(look.skyLightFactor, 0.26666668f);
        // With a light engine the lightmap takes the factor as TF gives it:
        // torches, glowing flora and the fireflies light the dark forest.
        m_frame.skyLightFactor = look.skyLightFactor;
        m_frame.skyLightColor  = glm::vec3(1.0f);

        // AtmosphericFogEnvironment.getBaseColor: FOG_COLOR (no sunrise
        // blend — the colour is transparent), mixed toward the
        // weather-darkened sky by the sky fog's reach. FogHandler's dimming
        // follows in ApplyTwilightFogHandler.
        glm::vec3 skyForFog = sky;
        if (m_rainLevel > 0.0f) {
            skyForFog *= glm::vec3(1.0f - m_rainLevel * 0.5f, 1.0f - m_rainLevel * 0.5f,
                                   1.0f - m_rainLevel * 0.4f);
        }
        if (m_thunderLevel > 0.0f) skyForFog *= 1.0f - m_thunderLevel * 0.5f;
        m_frame.fogColor = glm::mix(look.fog, skyForFog,
                                    SkyFogMixFactor(look.skyFogEnd, renderDistChunks));

        // AtmosphericFogEnvironment.setupFog: the environmental fog from
        // FOG_START/END_DISTANCE (the spooky forest's 16..64 — a close,
        // purple murk), the sky fog from SKY_FOG_END_DISTANCE.
        const float renderDistBlocks = static_cast<float>(renderDistChunks) * 16.0f;
        m_frame.fogSkyEnd = std::min(renderDistBlocks, look.skyFogEnd);
        if (fogEnabled) {
            m_frame.fogEnvStart = look.fogStart;
            m_frame.fogEnvEnd   = look.fogEnd;
        }
    }

    void EnvironmentState::ApplyTwilightFogHandler() {
        // TF FogHandler.colorFog. spookyPercent steps 0.005 per fog-colour
        // computation — once a rendered frame — toward 1 while the player
        // stands in the spooky forest and toward 0 elsewhere; stepped here by
        // elapsed time at 60 frames a second so it eases at the mod's rate
        // whatever the frame rate. Outside the spooky forest the fog is
        // dimmed to the fixed dusk (c · daylight · 0.94 + 0.06, blue 0.91 /
        // 0.09); inside it the biome's own purple shows at full strength.
        if (!m_composingForeign) {
            const auto now = std::chrono::steady_clock::now();
            float frames = 1.0f;
            if (m_lastSpookyStep.time_since_epoch().count() != 0) {
                const float seconds = std::chrono::duration<float>(now - m_lastSpookyStep).count();
                frames = std::clamp(seconds * 60.0f, 0.0f, 60.0f);
            }
            m_lastSpookyStep = now;
            bool spooky = false;
            if (m_hasCameraPos && ::Client::g_clientChunkManager) {
                const Game::BiomeId biome = ::Client::g_clientChunkManager->BiomeAtWorld(
                    static_cast<int>(std::floor(m_cameraPos.x)),
                    static_cast<int>(std::floor(m_cameraPos.y)),
                    static_cast<int>(std::floor(m_cameraPos.z)));
                spooky = Game::BiomeRegistry::Get(biome).name == "twilightforest:spooky_forest";
            }
            m_spookyPercent = std::clamp(m_spookyPercent + (spooky ? 0.005f : -0.005f) * frames,
                                         0.0f, 1.0f);
        }
        static const float daylight = TwilightFogDaylight();
        const float spookyPercent = m_composingForeign ? 0.0f : m_spookyPercent;
        const glm::vec3 c = m_frame.fogColor;
        const glm::vec3 dimmed(c.r * daylight * 0.94f + 0.06f,
                               c.g * daylight * 0.94f + 0.06f,
                               c.b * daylight * 0.91f + 0.09f);
        m_frame.fogColor = glm::mix(dimmed, c, spookyPercent);   // Mth.clampedLerp
    }

    void EnvironmentState::ComposeAether(float partialTick, const glm::vec3& cameraForward,
                                         int renderDistChunks, bool fogEnabled) {
        // The Aether's clock (AetherTimeAttachment): held at a quarter of
        // its day — noon — while the day is eternal, otherwise the client
        // clock read against the Aether's 72,000-tick day.
        const double dayTime = m_aetherEternalDay ? kAetherTicksPerDay / 4.0 : DayTimeF(partialTick);
        const int64_t dayTicks = static_cast<int64_t>(std::floor(dayTime));
        const float timeOfDay = AetherTimeOfDay(dayTime);
        const float cosDay = std::cos(timeOfDay * 6.2831855f);
        const float brightness = AetherDayBrightness(timeOfDay);
        const float rain = m_rainLevel, thunder = m_thunderLevel;

        // 1.21.1 celestial angles: the sun at timeOfDay · 360°, the moon
        // opposite, the stars with the sun; DimensionTypeMixin.moonPhase.
        m_frame.sunAngleDeg  = timeOfDay * 360.0f;
        m_frame.moonAngleDeg = m_frame.sunAngleDeg + 180.0f;
        m_frame.starAngleDeg = m_frame.sunAngleDeg;
        m_frame.moonPhase    = static_cast<int>(FloorModI(FloorDiv(dayTicks,
                                   static_cast<int64_t>(kAetherTicksPerDay)), 8));

        // AetherSkyRenderEffects.getSkyColor: the biome sky × the day curve,
        // lighter than the Overworld in weather, then the lightning flash
        // (ClientLevel's 1.21.1 form: 0.45 toward (0.8, 0.8, 1.0)).
        const float flash = m_skyFlashTime > 0
            ? std::min(static_cast<float>(m_skyFlashTime) - partialTick, 1.0f) * 0.45f : 0.0f;
        auto flashed = [&](glm::vec3 c) {
            if (flash > 0.0f) c = c * (1.0f - flash) + glm::vec3(0.8f, 0.8f, 1.0f) * flash;
            return c;
        };
        glm::vec3 sky = kAetherSky * brightness;
        sky = WeatherGrey(sky, rain, 0.61f, 0.2f);
        sky = WeatherGrey(sky, thunder, 0.48f, 0.21f);
        m_frame.skyColor = flashed(sky);

        // getSunriseColor (the default, not green_sunset): a glow while
        // cos(timeOfDay·2π) is within ±0.4.
        m_frame.sunriseColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
        if (cosDay >= -0.4f && cosDay <= 0.4f) {
            const float f3 = cosDay / 0.4f * 0.5f + 0.5f;
            float alpha = 1.0f - (1.0f - std::sin(f3 * 3.1415927f)) * 0.99f;
            alpha *= alpha;
            m_frame.sunriseColor = glm::vec4(f3 * 0.3f + 0.65f, f3 * f3 * 0.7f + 0.25f,
                                             f3 * f3 * 0.0f + 0.4f, alpha);
        }

        // renderSky: stars at Level.getStarBrightness × (1 − rain).
        {
            const float f1 = std::clamp(1.0f - (cosDay * 2.0f + 0.25f), 0.0f, 1.0f);
            m_frame.starBrightness = f1 * f1 * 0.5f * (1.0f - rain);
        }

        // drawCelestialBodies: the sun fades out over 600 ticks from 12,800
        // (×3) and back in from 23,800 (×3), the moon the other way; rain
        // takes from both.
        {
            constexpr int64_t kDusk = 12800 * 3, kDawn = 23800 * 3;
            int64_t t = FloorModI(dayTicks, static_cast<int64_t>(kAetherTicksPerDay));
            float sun = 1.0f, moon = 0.0f;
            if (t > kDawn) {
                t -= kDawn;
                sun  = std::min(static_cast<float>(t) * 0.00167f, 1.0f);
                moon = std::max(1.0f - static_cast<float>(t) * 0.00167f, 0.0f);
            } else if (t > kDusk) {
                t -= kDusk;
                sun  = std::max(1.0f - static_cast<float>(t) * 0.00167f, 0.0f);
                moon = std::min(static_cast<float>(t) * 0.00167f, 1.0f);
            }
            m_frame.sunAlpha  = std::max(sun - rain, 0.0f);
            m_frame.moonAlpha = std::max(moon - rain, 0.0f);
        }

        // Terrain dim: 1.21.1 Level.getSkyDarken through LightTexture's
        // f · 0.95 + 0.05 (the flash's 1.0 is applied at the end of
        // UpdateFrame for every dimension).
        {
            float d = std::clamp(1.0f - (cosDay * 2.0f + 0.2f), 0.0f, 1.0f);
            d = 1.0f - d;
            d *= 1.0f - rain * 5.0f / 16.0f;
            d *= 1.0f - thunder * 5.0f / 16.0f;
            m_frame.skyBrightness = (d * 0.8f + 0.2f) * 0.95f + 0.05f;
            // 1.21.1's LightTexture takes the same f * 0.95 + 0.05 as its sky
            // factor, in white.
            m_frame.skyLightFactor = m_frame.skyBrightness;
            m_frame.skyLightColor  = glm::vec3(1.0f);
        }

        // renderClouds: vanilla clouds at the Aether's height in
        // getCloudColor's colour (lighter than the Overworld's in weather),
        // with 1.21.1's 0.8 cloud alpha.
        {
            glm::vec3 cloud(1.0f);
            cloud = WeatherGrey(cloud, rain, 0.725f, 0.8f);
            cloud *= glm::vec3(brightness * 0.9f + 0.1f, brightness * 0.9f + 0.1f,
                               brightness * 0.85f + 0.15f);
            cloud = WeatherGrey(cloud, thunder, 0.5f, 0.7f);
            m_frame.cloudColor   = glm::vec4(cloud, 0.8f);
            m_frame.cloudBottomY = kAetherCloudBottomY;
        }

        // 1.21.1 FogRenderer.setupColor: the biome fog through the
        // Overworld's getBrightnessDependentFogColor, the sunrise glow when
        // looking toward the sun, the mix toward ClientLevel.getSkyColor —
        // vanilla's weather darkening there, not the Aether's — and the
        // weather darkening of the fog itself.
        glm::vec3 fog = kAetherFog * glm::vec3(brightness * 0.94f + 0.06f, brightness * 0.94f + 0.06f,
                                               brightness * 0.91f + 0.09f);
        if (renderDistChunks >= 4) {
            const float sunX = std::sin(glm::radians(m_frame.sunAngleDeg)) > 0.0f ? -1.0f : 1.0f;
            float facing = glm::dot(cameraForward, glm::vec3(sunX, 0.0f, 0.0f));
            if (facing < 0.0f) facing = 0.0f;
            if (facing > 0.0f && m_frame.sunriseColor.a > 0.0f) {
                facing *= m_frame.sunriseColor.a;
                fog = fog * (1.0f - facing) + glm::vec3(m_frame.sunriseColor) * facing;
            }
        }
        glm::vec3 vanillaSky = kAetherSky * brightness;
        vanillaSky = WeatherGrey(vanillaSky, rain, 0.6f, 0.75f);
        vanillaSky = WeatherGrey(vanillaSky, thunder, 0.2f, 0.75f);
        vanillaSky = flashed(vanillaSky);
        const float mixDist = 0.25f + 0.75f * std::min(static_cast<float>(renderDistChunks), 32.0f) / 32.0f;
        fog += (vanillaSky - fog) * (1.0f - std::pow(mixDist, 0.25f));
        if (rain > 0.0f) fog *= glm::vec3(1.0f - rain * 0.5f, 1.0f - rain * 0.5f, 1.0f - rain * 0.4f);
        if (thunder > 0.0f) fog *= 1.0f - thunder * 0.5f;
        // DimensionClientHooks.renderFogColors undoes the void darkening
        // (which this engine does not apply); adjustWeatherFogColors lifts
        // the weather fog back up and caps every channel at the biome fog.
        if (rain > 0.0f) fog *= glm::vec3(1.0f + rain * 0.8f, 1.0f + rain * 0.8f, 1.0f + rain * 0.56f);
        if (thunder > 0.0f) fog *= glm::vec3(1.0f + thunder * 0.66f, 1.0f + thunder * 0.66f,
                                             1.0f + thunder * 0.76f);
        m_frame.fogColor = glm::min(fog, kAetherFog);

        // DimensionClientHooks.renderNearFog: the terrain fog starts at half
        // the far distance (1.21.1's max(render distance, 32 blocks)) — the
        // beta Aether's hazy distance. The sky fog is vanilla's.
        if (fogEnabled) {
            const float far = std::max(static_cast<float>(renderDistChunks) * 16.0f, 32.0f);
            m_frame.fogRdStart = far * 0.5f;
        }
    }

    // MC EnvironmentAttributes.WATER_FOG_COLOR: default 0x050533
    // (-16448205), overridden per biome in OverworldBiomes. The nether and
    // end biomes keep the default.
    glm::vec3 WaterFogColorForBiome(Game::BiomeId biome) {
        struct Entry { std::string_view name; int32_t argb; };
        static constexpr Entry kOverrides[] = {
            { "lukewarm_ocean",      -16509389 },
            { "deep_lukewarm_ocean", -16509389 },
            { "warm_ocean",          -16507085 },
            { "dark_forest",         -11179648 },
            { "swamp",               -14474473 },
            { "mangrove_swamp",      -11699616 },
            { "cherry_grove",        -10635281 },
            { "sulfur_caves",        -15248324 },
            { "dappled_forest",      -13151916 },
            // Twilight Forest (worldgen/biome/*.json water_fog_color).
            { "twilightforest:dense_forest",  static_cast<int32_t>(0xFF005522u) },
            { "twilightforest:fire_swamp",    static_cast<int32_t>(0xFF6C2C2Cu) },
            { "twilightforest:spooky_forest", static_cast<int32_t>(0xFFBC8857u) },
            { "twilightforest:swamp",         static_cast<int32_t>(0xFF95B55Fu) },
        };
        int32_t argb = -16448205;
        const std::string_view name = Game::BiomeRegistry::Get(biome).name;
        for (const Entry& e : kOverrides) {
            if (e.name == name) { argb = e.argb; break; }
        }
        const uint32_t rgb = static_cast<uint32_t>(argb);
        return glm::vec3(((rgb >> 16) & 0xFF) / 255.0f,
                         ((rgb >>  8) & 0xFF) / 255.0f,
                         ( rgb        & 0xFF) / 255.0f);
    }

    void EnvironmentState::SetCameraFluid(int fluid, Game::BiomeId biomeAtEye, bool spectator,
                                          bool resonantWater) {
        m_cameraFluid     = fluid;
        m_cameraBiome     = biomeAtEye;
        m_cameraSpectator = spectator;
        m_cameraResonant  = fluid == kFluidWater && resonantWater;
    }

    float EnvironmentState::WaterVision() const {
        // MC LocalPlayer.getWaterVision.
        if (m_cameraFluid != kFluidWater) return 0.0f;
        if (m_waterVisionTime >= 600) return 1.0f;
        const float a = glm::clamp(static_cast<float>(m_waterVisionTime) / 100.0f, 0.0f, 1.0f);
        const float b = m_waterVisionTime < 100
            ? 0.0f
            : glm::clamp((static_cast<float>(m_waterVisionTime) - 100.0f) / 500.0f, 0.0f, 1.0f);
        return a * 0.6f + b * 0.39999998f;
    }

} // namespace Render

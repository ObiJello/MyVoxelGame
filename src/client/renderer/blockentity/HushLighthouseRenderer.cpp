// File: src/client/renderer/blockentity/HushLighthouseRenderer.cpp
//
// See the header for the design. Layout of this file:
//   the lamp's character   (a seed from its position → period, phase, turn)
//   the sweep              (strokes, easing, hitches: a pure function of time)
//   the light              (breathing, shimmer, guttering)
//   noticing the viewer    (a monotone warp of the sweep round their bearing)
//   the look               (the beams' and the glare's VolumetricBeam values)
//   the draw               (two cones and a glare handed to VolumetricBeam)
#include "HushLighthouseRenderer.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/core/Log.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include "common/world/block/entity/HushLighthouseLampBlockEntity.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "../backend/RenderBackend.hpp"
#include "../effects/VolumetricBeam.hpp"
#include "../environment/EnvironmentState.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Render {

    namespace {

        constexpr double kPi     = 3.14159265358979323846;
        constexpr double kTwoPi  = 2.0 * kPi;
        constexpr double kHalfPi = 0.5 * kPi;
        constexpr double kDeg    = kPi / 180.0;

        // ── tuning ───────────────────────────────────────────────────────
        // A stroke is a quarter turn; its length is seeded per lamp in
        // [kStrokeMinTicks, kStrokeMinTicks + kStrokeSpreadTicks): a full
        // turn every 30..42 s.
        constexpr double kStrokeMinTicks    = 150.0;
        constexpr double kStrokeSpreadTicks = 60.0;
        constexpr double kHitchChance       = 0.6;    // strokes that stall part-way
        constexpr double kShudderChance     = 0.6;    // stalls that also jerk back
        constexpr double kBasePitch         = -3.0 * kDeg;   // beams lean a little down

        // Noticing: the warp window round the viewer's bearing, in the sweep's
        // own direction — it slows over kApproach, holds for kHold of the base
        // sweep (~1.2 s at the average 12.5 deg/s; longer if the stroke is
        // pausing anyway), then catches up by kRelease.
        constexpr double kApproach = 24.0 * kDeg;
        constexpr double kHold     = 16.0 * kDeg;
        constexpr double kRelease  = 34.0 * kDeg;
        constexpr double kTremble  = 0.4 * kDeg;      // the quiver while it stares
        // Horizontal distance: full attention inside kNoticeFull, none past
        // kNoticeNone (a beam's reach); and none right at the foot of the
        // tower, where dipping to the viewer would aim the beam at the wall.
        constexpr double kNoticeFull   = 40.0;
        constexpr double kNoticeNone   = 56.0;
        constexpr double kNoticeNearLo = 4.0;
        constexpr double kNoticeNearHi = 9.0;
        constexpr double kMinDip   = -50.0 * kDeg;
        constexpr double kMaxLift  =  15.0 * kDeg;

        // Guiding (Lighthouses that guide): in one turn of every
        // kGuideEveryMin..+kGuideEverySpread (seeded per lamp), the first
        // stroke whose sweep passes the bearing to the nearest Aurelith eases
        // onto it, holds for kGuideHoldTicks and moves on. The hold never
        // takes more than kGuideHoldMax of its stroke, so the stroke still
        // lands on its quarter on time.
        constexpr int    kGuideEveryMin    = 4;
        constexpr int    kGuideEverySpread = 3;
        constexpr double kGuideHoldTicks   = 72.0;     // ~3.6 s
        constexpr double kGuideHoldMax     = 0.46;
        constexpr double kGuideMinRange    = 16.0;     // a lamp in the city points at nothing
        constexpr double kGuideLift        = 2.5 * kDeg;   // the guiding beam levels toward the horizon

        // MC BeaconRenderer.BEAM_SCALE_THRESHOLD: past this the beam widens
        // with distance so a far one stays visible.
        constexpr double kWidenFrom = 96.0;

        double SmoothStep(double e0, double e1, double x) {
            const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
            return t * t * (3.0 - 2.0 * t);
        }

        // Wrap to [-pi, pi) and to [-pi/2, pi/2) — the latter is the offset
        // to the NEARER of two opposed beams.
        double WrapTurn(double a)     { return a - kTwoPi * std::floor((a + kPi) / kTwoPi); }
        double WrapHalfTurn(double a) { return a - kPi * std::floor((a + kHalfPi) / kPi); }

        // ── hashing ──────────────────────────────────────────────────────
        // SplitMix64's finaliser: every draw below is a hash of (lamp,
        // stroke or slot index, salt), so any moment is computed directly —
        // nothing accumulates, nothing is stored.
        uint64_t Mix64(uint64_t z) {
            z += 0x9E3779B97F4A7C15ull;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        }
        double Unit(uint64_t h) {                               // [0, 1)
            return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
        }
        double Draw(uint64_t seed, int64_t n, uint64_t salt) {
            return Unit(Mix64(seed ^ Mix64(static_cast<uint64_t>(n) * 0x9E3779B97F4A7C15ull
                                           ^ salt * 0xD1B54A32D192ED03ull)));
        }

        // MC Mth.getSeed(x, y, z) — the per-position seed vanilla keys block
        // model variants and offsets on. Java's int/long overflow wraps, so
        // the arithmetic runs unsigned.
        uint64_t PositionSeed(const glm::ivec3& p) {
            const int32_t xs = static_cast<int32_t>(static_cast<uint32_t>(p.x) * 3129871u);
            uint64_t seed = static_cast<uint64_t>(static_cast<int64_t>(xs))
                          ^ (static_cast<uint64_t>(static_cast<int64_t>(p.z)) * 116129781ull)
                          ^ static_cast<uint64_t>(static_cast<int64_t>(p.y));
            seed = seed * seed * 42317861ull + seed * 11ull;
            return static_cast<uint64_t>(static_cast<int64_t>(seed) >> 16);
        }

        // ── the lamp's character ─────────────────────────────────────────
        struct LampTraits {
            uint64_t seed;
            double   strokeTicks;   // one quarter turn
            double   phaseTicks;    // where in its turn the lamp is at tick 0
            double   yaw0;
            double   dir;           // +1 or -1: which way it turns
            double   breathA, breathB;
        };

        LampTraits TraitsFor(const glm::ivec3& pos) {
            LampTraits t{};
            t.seed        = Mix64(PositionSeed(pos));
            t.strokeTicks = kStrokeMinTicks + kStrokeSpreadTicks * Draw(t.seed, 0, 1);
            t.phaseTicks  = 4.0 * t.strokeTicks * Draw(t.seed, 0, 2);
            t.yaw0        = kTwoPi * Draw(t.seed, 0, 3);
            t.dir         = Draw(t.seed, 0, 4) < 0.5 ? 1.0 : -1.0;
            t.breathA     = kTwoPi * Draw(t.seed, 0, 5);
            t.breathB     = kTwoPi * Draw(t.seed, 0, 6);
            return t;
        }

        // ── the sweep ────────────────────────────────────────────────────
        // A symmetric sigmoid: eases in and out, steeper as k grows (its
        // slope at the middle is k).
        double Ease(double s, double k) {
            if (s <= 0.0) return 0.0;
            if (s >= 1.0) return 1.0;
            const double a = std::pow(s, k);
            const double b = std::pow(1.0 - s, k);
            return a / (a + b);
        }

        int64_t FloorDiv(int64_t a, int64_t b) {
            const int64_t q = a / b;
            return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
        }

        // The guide, as the sweep sees it: a bearing (radians, the sweep's
        // yaw convention: atan2(dz, dx)) or none.
        struct Guide { bool active = false; double bearing = 0.0; };

        // Is stroke n this lamp's guide stroke? If so, `phi` is how far into
        // the stroke's quarter (0..pi/2, in the turning direction) the
        // bearing lies, and `beam` which of the two opposed beams passes it.
        // A guide turn comes once in every G turns (G seeded 4..6, phase
        // seeded); in it, the FIRST stroke that carries either beam across
        // the bearing is the guide stroke. Strokes are quarter turns and the
        // beams opposed, so exactly two strokes of each turn qualify.
        bool IsGuideStroke(const LampTraits& lamp, int64_t n, const Guide& guide,
                           double& phi, int& beam) {
            if (!guide.active) return false;
            const int64_t every = kGuideEveryMin
                                + static_cast<int64_t>(kGuideEverySpread * Draw(lamp.seed, 0, 30));
            const int64_t phase = static_cast<int64_t>(static_cast<double>(every) * Draw(lamp.seed, 0, 31));
            const int64_t turn = FloorDiv(n, 4);
            if (((turn % every) + every) % every != phase) return false;
            for (int64_t k = turn * 4; k < turn * 4 + 4; ++k) {
                const double start = lamp.yaw0 + lamp.dir * static_cast<double>(((k % 4) + 4) % 4) * kHalfPi;
                for (int i = 0; i < 2; ++i) {
                    double off = lamp.dir * (guide.bearing - start - kPi * i);
                    off -= kTwoPi * std::floor(off / kTwoPi);           // [0, 2pi)
                    if (off < kHalfPi) {
                        if (k != n) return false;                        // an earlier stroke has it
                        phi = off;
                        beam = i;
                        return true;
                    }
                }
            }
            return false;
        }

        // The sweep's base yaw (radians) at `gameTicks`. Stroke n is a quarter
        // turn: a pause, the move, a pause. When it hitches, progress stops
        // dead at s0 for sw of the stroke (and may shudder back a degree or
        // two meanwhile), then resumes where it stopped; the rest of the
        // stroke is stretched to still land on the quarter. Every stroke ends
        // exactly where the next begins, so the angle is continuous.
        //
        // A GUIDE stroke (IsGuideStroke) replaces the hitch with a hold: the
        // move is split at the bearing — eased onto it, held for
        // kGuideHoldTicks, eased on to the quarter — so the stroke still
        // starts and ends where every other stroke does and the angle stays
        // continuous. `guideHold` (0..1, softened at the hold's edges) and
        // `guideBeam` report it to the light and the pitch.
        double SweepYaw(const LampTraits& lamp, double gameTicks, const Guide& guide,
                        double& guideHold, int& guideBeam) {
            guideHold = 0.0;
            guideBeam = -1;
            const double strokes = (gameTicks + lamp.phaseTicks) / lamp.strokeTicks;
            const double nf = std::floor(strokes);
            const int64_t n = static_cast<int64_t>(nf);
            const double u = strokes - nf;
            auto r = [&](uint64_t salt) { return Draw(lamp.seed, n, salt); };

            double phi = 0.0;
            int beam = -1;
            if (IsGuideStroke(lamp, n, guide, phi, beam)) {
                const double lead = 0.03 + 0.10 * r(10);
                const double tail = 0.02 + 0.05 * r(11);
                const double hold = std::min(kGuideHoldMax, kGuideHoldTicks / lamp.strokeTicks);
                const double moves = std::max(0.05, 1.0 - lead - tail - hold);
                const double f = phi / kHalfPi;                         // the bearing's share of the quarter
                const double m1 = moves * (0.15 + 0.70 * f);
                const double m2 = moves - m1;
                const double k = 1.6 + 0.6 * r(12);
                double progress;
                if (u < lead) {
                    progress = 0.0;
                } else if (u < lead + m1) {
                    progress = f * Ease((u - lead) / m1, k);
                } else if (u < lead + m1 + hold) {
                    progress = f;
                    const double w = (u - lead - m1) / hold;
                    guideHold = SmoothStep(0.0, 0.12, w) * (1.0 - SmoothStep(0.85, 1.0, w));
                    guideBeam = beam;
                } else if (u < lead + m1 + hold + m2) {
                    progress = f + (1.0 - f) * Ease((u - lead - m1 - hold) / m2, k);
                } else {
                    progress = 1.0;
                }
                const double quarters = static_cast<double>(((n % 4) + 4) % 4) + progress;
                return lamp.yaw0 + lamp.dir * quarters * kHalfPi;
            }

            const double lead = 0.03 + 0.15 * r(10);
            const double tail = 0.02 + 0.06 * r(11);
            const double k    = 1.4 + 0.8 * r(12);
            double s = std::clamp((u - lead) / (1.0 - lead - tail), 0.0, 1.0);

            double shudder = 0.0;
            if (r(13) < kHitchChance) {
                const double s0  = 0.25 + 0.45 * r(14);
                const double sw  = 0.05 + 0.10 * r(15);
                const double amp = r(16) < kShudderChance ? (0.8 + 1.6 * r(17)) * kDeg : 0.0;
                if (s >= s0 && s < s0 + sw) {
                    const double b = std::sin(kPi * (s - s0) / sw);
                    shudder = amp * b * b;
                    s = s0;
                } else if (s >= s0 + sw) {
                    s -= sw;
                }
                s /= (1.0 - sw);
            }
            const double quarters = static_cast<double>(((n % 4) + 4) % 4) + Ease(s, k);
            return lamp.yaw0 + lamp.dir * (quarters * kHalfPi - shudder);
        }

        // ── the light ────────────────────────────────────────────────────
        double ValueNoise(uint64_t seed, double x, uint64_t salt) {
            const double f = std::floor(x);
            const int64_t i = static_cast<int64_t>(f);
            const double t = x - f;
            const double a = Draw(seed, i, salt);
            const double b = Draw(seed, i + 1, salt);
            return a + (b - a) * t * t * (3.0 - 2.0 * t);
        }

        // Brightness in (0, 1]: a slow breathing, a ~7 Hz shimmer, and in
        // about one half-second in thirty a gutter — the light sags and
        // recovers within a quarter second.
        double LampLight(const LampTraits& lamp, double gameTicks) {
            const double sec = gameTicks / 20.0;
            double f = 0.90 + 0.05 * std::sin(sec * 2.3 + lamp.breathA) * std::sin(sec * 0.71 + lamp.breathB);
            f += 0.07 * (ValueNoise(lamp.seed, sec * 7.0, 20) - 0.5);
            const double slots = sec * 2.0;
            const double sf = std::floor(slots);
            const int64_t slot = static_cast<int64_t>(sf);
            const double x = slots - sf;
            if (x < 0.5 && Draw(lamp.seed, slot, 21) < 0.035) {
                const double depth = 0.35 + 0.35 * Draw(lamp.seed, slot, 22);
                const double b = std::sin(kPi * x / 0.5);
                f *= 1.0 - depth * b * b;
            }
            return std::clamp(f, 0.15, 1.0);
        }

        // ── noticing the viewer ──────────────────────────────────────────
        // d: how far the sweep is past the viewer's bearing, in its own turning
        // direction. Returns the warped offset (identity outside the window)
        // and how hard the beam is holding on the viewer (0..1). Both pieces
        // are cubic Hermite segments matched in value and slope to the
        // identity at the window's edges and flat at the hold, so the warp
        // is C1 and never runs backwards.
        struct Warp { double d; double hold; };

        Warp NoticeWarp(double d) {
            if (d <= -kApproach || d >= kRelease) return {d, 0.0};
            if (d < 0.0) {                            // slowing onto the viewer
                const double t = (d + kApproach) / kApproach;
                const double h00 = 2.0 * t * t * t - 3.0 * t * t + 1.0;
                const double h10 = t * t * t - 2.0 * t * t + t;
                return {-kApproach * h00 + kApproach * h10, t * t * (3.0 - 2.0 * t)};
            }
            if (d <= kHold) return {0.0, 1.0};        // holding
            const double span = kRelease - kHold;     // swinging off, fast
            const double t = (d - kHold) / span;
            const double h01 = -2.0 * t * t * t + 3.0 * t * t;
            const double h11 = t * t * t - t * t;
            return {kRelease * h01 + span * h11, 1.0 - t * t * (3.0 - 2.0 * t)};
        }

        // The local player's eyes, when this view is in the player's world.
        // A portal view is not (its frame override is set), and its level's
        // block access has no player of its own.
        bool LocalViewerEye(glm::dvec3& out) {
            if (EnvironmentState::Get().FrameOverride()) return false;
            if (!Client::g_clientBlockAccess) return false;
            glm::dvec3 mn, mx;
            if (!Client::g_clientBlockAccess->GetLocalPlayerBox(mn, mx)) return false;
            out = glm::dvec3((mn.x + mx.x) * 0.5, mn.y + (mx.y - mn.y) * 0.9, (mn.z + mx.z) * 0.5);
            return true;
        }

        // ── the look ─────────────────────────────────────────────────────
        // Tuned against the Hush's night: fog #0B2A2E, terrain dimmed to
        // 0.42. A pale cyan core in a teal falloff; haze thick enough that
        // the shaft reads side-on as a soft teal band, while looking back
        // down it at the lens saturates to a cyan-white glare.
        constexpr float     kStartRadius   = 0.30f;
        constexpr float     kEndRadius     = 5.2f;
        const glm::vec3     kCoreColor     {0.80f, 1.00f, 0.97f};
        const glm::vec3     kEdgeColor     {0.12f, 0.66f, 0.62f};
        constexpr float     kHaze          = 0.42f;
        constexpr float     kExtinction    = 0.010f;
        constexpr float     kAnisotropy    = 0.62f;
        constexpr float     kMist          = 0.65f;
        constexpr float     kFadeIn        = 1.6f;
        constexpr float     kHalfIntensity = 26.0f;
        constexpr float     kFadeOutStart  = 0.5f;
        // The lantern room: a 5x5 ring of glass and four mullions round the
        // lamp. The probe starts past it, so a mullion does not cut the beam.
        constexpr float     kTerrainStart  = 3.5f;
        // The lens glare: at rest a small glow; flaring when a beam points
        // at the viewer (within ~16 deg, full within ~2.5).
        const glm::vec3     kGlareColor    {0.78f, 1.00f, 0.97f};
        constexpr float     kGlareRest     = 0.9f;
        constexpr float     kGlareFlare    = 2.4f;

    } // namespace

    HushLighthouseRenderer::~HushLighthouseRenderer() { Shutdown(); }

    bool HushLighthouseRenderer::Initialize() {
        if (m_initialized) return true;
        if (!VolumetricBeam::Acquire()) {
            Log::Error("[HushLighthouseRenderer] volumetric beam resources unavailable");
            return false;
        }
        m_initialized = true;
        return true;
    }

    void HushLighthouseRenderer::Shutdown() {
        if (!m_initialized) return;
        m_initialized = false;
        VolumetricBeam::Release();
    }

    void HushLighthouseRenderer::Render(const Game::BlockEntity& be,
                                        float partialTick,
                                        const glm::mat4& proj,
                                        const glm::mat4& view,
                                        const glm::vec3& /*cameraPos*/) {
        PROFILE_ZONE_N("BE.HushLighthouse");
        if (!m_initialized || !g_renderBackend) return;

        const glm::ivec3 pos = be.GetWorldPos();
        const glm::dvec3 centre = glm::dvec3(pos) + glm::dvec3(0.5);
        const LampTraits lamp = TraitsFor(pos);
        const double ticks = EnvironmentState::Get().GameTimeF(partialTick);

        // The nearest Aurelith, when the server has told this lamp one
        // (HushLighthouseLampBlockEntity, filled in by LighthouseGuide).
        Guide guide;
        if (const auto* lampBe = dynamic_cast<const Game::HushLighthouseLampBlockEntity*>(&be);
            lampBe && lampBe->HasGuide()) {
            const double gx = lampBe->GuideX() + 0.5 - centre.x;
            const double gz = lampBe->GuideZ() + 0.5 - centre.z;
            if (std::hypot(gx, gz) >= kGuideMinRange) {
                guide.active = true;
                guide.bearing = std::atan2(gz, gx);
            }
        }

        double guideHold = 0.0;
        int guideBeam = -1;
        double yaw = SweepYaw(lamp, ticks, guide, guideHold, guideBeam);
        double light = LampLight(lamp, ticks);
        double pitch[2] = {kBasePitch, kBasePitch};
        // Guiding: the light steadies and brightens a touch (a held note, not
        // a gutter), and the guiding beam levels toward the horizon.
        if (guideHold > 0.0 && guideBeam >= 0) {
            light += (1.0 - light) * 0.75 * guideHold;
            light *= 1.0 + 0.12 * guideHold;
            pitch[guideBeam] += kGuideLift * guideHold;
        }

        // It notices you: warp the sweep round your bearing, dip the beam
        // that holds you to your eyes, and steady its light while it stares.
        glm::dvec3 eye;
        if (LocalViewerEye(eye)) {
            const glm::dvec3 v = eye - centre;
            const double dist = std::hypot(v.x, v.z);
            const double reach = (1.0 - SmoothStep(kNoticeFull, kNoticeNone, dist))
                               * SmoothStep(kNoticeNearLo, kNoticeNearHi, dist);
            if (reach > 0.0) {
                const double bearing = std::atan2(v.z, v.x);
                const double d = lamp.dir * WrapHalfTurn(yaw - bearing);
                const Warp w = NoticeWarp(d);
                const double hold = w.hold * reach;
                yaw -= lamp.dir * reach * (d - w.d);
                yaw += lamp.dir * hold * kTremble * std::sin(ticks / 20.0 * 11.0);
                const int facing = std::abs(WrapTurn(yaw - bearing)) < kHalfPi ? 0 : 1;
                const double target = std::clamp(std::atan2(v.y, dist), kMinDip, kMaxLift);
                pitch[facing] += (target - kBasePitch) * hold;
                light += (1.0 - light) * hold;
            }
        }

        // The view's own eye (exact, render-space view), for the widening
        // and the glare — a portal view's too.
        const glm::dvec3 viewEye = VolumetricBeam::EyeFromView(view);

        // MC BeaconRenderer.extract: beamRadiusScale = max(1, distance / 96).
        const double camDist = std::hypot(viewEye.x - centre.x, viewEye.z - centre.z);
        const float widen = static_cast<float>(std::max(1.0, camDist / kWidenFrom));

        BeamCone cones[2];
        for (int i = 0; i < 2; ++i) {
            const double beamYaw = WrapTurn(yaw + kPi * i);
            const glm::dvec3 dir(std::cos(pitch[i]) * std::cos(beamYaw), std::sin(pitch[i]),
                                 std::cos(pitch[i]) * std::sin(beamYaw));
            BeamCone& c = cones[i];
            c.apex        = centre + dir * static_cast<double>(kBeamStart);
            c.direction   = glm::vec3(dir);
            c.length      = kBeamLength - kBeamStart;
            c.startRadius = kStartRadius * widen;
            c.endRadius   = kEndRadius * widen;
            c.coreColor   = kCoreColor;
            c.edgeColor   = kEdgeColor;
            c.intensity   = static_cast<float>(light);
            c.haze        = kHaze;
            c.extinction  = kExtinction;
            c.anisotropy  = kAnisotropy;
            c.mist        = kMist;
            c.seed        = static_cast<uint32_t>(lamp.seed >> 32) + static_cast<uint32_t>(i) * 7919u;
            c.fadeIn      = kFadeIn;
            c.halfIntensityDistance = kHalfIntensity;
            c.fadeOutStart = kFadeOutStart;
            c.terrainStart = kTerrainStart - kBeamStart;
            c.cacheKey    = Mix64(lamp.seed + static_cast<uint64_t>(i) + 1u) | 1u;
        }

        // The lens glare: a sprite just in front of the lamp, toward the eye,
        // flaring as either beam comes round to point at the viewer.
        BeamGlare glare;
        size_t glareCount = 0;
        const glm::dvec3 toEye = viewEye - centre;
        const double eyeRange = glm::length(toEye);
        if (eyeRange > 1.2) {
            const float flare = std::max(VolumetricBeam::Flare(cones[0], viewEye),
                                         VolumetricBeam::Flare(cones[1], viewEye));
            glare.position  = centre + toEye / eyeRange * 0.75;
            glare.color     = kGlareColor;
            glare.size      = (kGlareRest + kGlareFlare * flare) * widen;
            glare.intensity = static_cast<float>(light) * (0.35f + 1.3f * flare);
            glare.streak    = 0.25f + 0.75f * flare;
            glare.streakLength = 4.0f;
            glareCount = 1;
        }

        VolumetricBeam::Get().Draw(cones, 2, &glare, glareCount, proj, view);
    }

} // namespace Render

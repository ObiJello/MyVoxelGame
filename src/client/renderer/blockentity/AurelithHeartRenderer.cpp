// File: src/client/renderer/blockentity/AurelithHeartRenderer.cpp
//
// See the header for the design. Layout of this file:
//   the rings' character   (radius, staves, tilt, period — per ring, seeded)
//   the turning            (strokes, easing, stalls: a pure function of time)
//   the meshes/textures    (baked once: staves, nodes, halos, core)
//   the light              (VolumetricBeam: the shaft, the dais pool)
//   noticing the viewer    (a per-viewer eased clock, rejoining the shared one)
//   the draw
#include "AurelithHeartRenderer.hpp"
#include "BlockEntityShader.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/core/Log.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include "../backend/RenderBackend.hpp"
#include "../core/RenderOrigin.hpp"
#include "../environment/EntityEnvironment.hpp"
#include "../environment/EnvironmentState.hpp"
#include "client/renderer/effects/VolumetricBeam.hpp"
#include "client/world/AurelithState.hpp"
#include "client/world/ClientLevel.hpp"
#include "common/world/level/AurelithQuest.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

namespace Render {

    using namespace Aurelith;

    namespace {

        // ── the rings' character ─────────────────────────────────────────
        // radius, stave count, tube half-thickness, tilt, seconds per turn,
        // turning direction, and how violet the ring's crystal runs (0..1).
        struct RingSpec {
            float  radius;
            int    staves;
            float  tube;
            double tiltDeg;
            double periodSec;
            double dir;
            double violet;
        };
        constexpr RingSpec kRings[AurelithHeartRenderer::kRingCount] = {
            { 2.6f, 10, 0.28f, 40.0,  41.0,  1.0, 0.55 },
            { 5.0f, 16, 0.42f,  8.0,  58.0, -1.0, 0.15 },
            { 8.0f, 24, 0.60f, 23.0,  83.0,  1.0, 0.25 },
            {12.0f, 34, 0.80f, 52.0, 117.0, -1.0, 0.10 },
        };

        // Colours (docs/hush-lore.md: the Chord is cyan, the river violet).
        constexpr Rgb kCyan   {95, 243, 255};   // #5FF3FF resonant cyan
        constexpr Rgb kViolet {183, 124, 255};  // #B77CFF violet accent
        constexpr Rgb kCoreCol{207, 255, 255};  // #CFFFFF cyan-white core
        constexpr Rgb kWhite  {236, 255, 255};

        constexpr int    kStrokesPerTurn = 12;       // a stroke is 30 degrees
        constexpr double kHitchChance    = 0.35;     // strokes that stall part-way
        constexpr double kShudderChance  = 0.5;      // stalls that also jerk back

        // Noticing: full within kNoticeFull blocks (horizontal), none past
        // kNoticeNone; eased with these time constants (ticks).
        constexpr double kNoticeFull   = 20.0;
        constexpr double kNoticeNone   = 28.0;
        constexpr double kNoticeHeight = 40.0;       // and within this much height
        constexpr double kStillSpeed   = 0.04;       // the rings' pace while they watch you
        constexpr double kSpeedEase    = 14.0;       // ~2 s to settle
        constexpr double kNoticeEase   = 20.0;
        constexpr double kCatchUpTicks = 400.0;      // lag at which the catch-up peaks
        constexpr double kCatchUpMax   = 1.5;        // at most 2.5x pace while catching up
        // The notice pulse: rolls out from the core at kPulseSpeed blocks/s
        // for kPulseSeconds.
        constexpr double kPulseSeconds = 6.0;
        constexpr double kPulseSpeed   = 3.2;

        struct RingTraits {
            uint64_t seed;
            double   strokeTicks;   // one twelfth of a turn
            double   phaseTicks;
            double   spin0;
            double   precess0;      // the tilt axis's bearing at tick 0
            double   precessRate;   // radians per tick (very slow)
        };

        RingTraits TraitsFor(uint64_t engineSeed, int ring) {
            RingTraits t{};
            t.seed = Mix64(engineSeed ^ (0xA0E1117Bull * static_cast<uint64_t>(ring + 1)));
            const double period = kRings[ring].periodSec * 20.0 * (0.9 + 0.2 * Draw(t.seed, 0, 1));
            t.strokeTicks = period / kStrokesPerTurn;
            t.phaseTicks  = period * Draw(t.seed, 0, 2);
            t.spin0       = kTwoPi * Draw(t.seed, 0, 3);
            t.precess0    = kTwoPi * Draw(t.seed, 0, 4);
            // A full precession every 6..12 minutes, either way.
            const double precessPeriod = (6.0 + 6.0 * Draw(t.seed, 0, 5)) * 60.0 * 20.0;
            t.precessRate = (Draw(t.seed, 0, 6) < 0.5 ? 1.0 : -1.0) * kTwoPi / precessPeriod;
            return t;
        }

        // ── the turning ──────────────────────────────────────────────────
        // A ring's spin (radians) at clock `tau`: the lighthouse's stroke
        // machine with a twelfth of a turn per stroke. Each stroke is a pause,
        // an eased move and, in a third of them, a stall at s0 for sw of the
        // stroke — sometimes shuddering back — then it resumes where it
        // stopped; every stroke ends exactly where the next begins.
        double RingSpin(const RingTraits& ring, double dir, double tau) {
            const double strokes = (tau + ring.phaseTicks) / ring.strokeTicks;
            const double nf = std::floor(strokes);
            const int64_t n = static_cast<int64_t>(nf);
            const double u = strokes - nf;
            auto r = [&](uint64_t salt) { return Draw(ring.seed, n, salt); };

            const double lead = 0.02 + 0.10 * r(10);
            const double tail = 0.01 + 0.05 * r(11);
            const double k    = 1.0 + 0.7 * r(12);
            double s = std::clamp((u - lead) / (1.0 - lead - tail), 0.0, 1.0);

            double shudder = 0.0;
            if (r(13) < kHitchChance) {
                const double s0  = 0.25 + 0.5 * r(14);
                const double sw  = 0.08 + 0.14 * r(15);
                const double amp = r(16) < kShudderChance ? (0.6 + 1.2 * r(17)) * kDeg : 0.0;
                if (s >= s0 && s < s0 + sw) {
                    const double b = std::sin(kPi * (s - s0) / sw);
                    shudder = amp * b * b;
                    s = s0;
                } else if (s >= s0 + sw) {
                    s -= sw;
                }
                s /= (1.0 - sw);
            }
            const double strokesDone = static_cast<double>(((n % kStrokesPerTurn) + kStrokesPerTurn) % kStrokesPerTurn)
                                     + Ease(s, k);
            return ring.spin0 + dir * (strokesDone * (kTwoPi / kStrokesPerTurn) - shudder);
        }

        // Brightness breathing in (0.85, 1]: two slow sines per engine, so
        // the light swells like a held breath and never quite settles.
        double Breath(uint64_t seed, double ticks) {
            const double sec = ticks / 20.0;
            const double a = kTwoPi * Draw(seed, 0, 40);
            const double b = kTwoPi * Draw(seed, 0, 41);
            return 0.925 + 0.075 * std::sin(sec * 0.61 + a) * std::sin(sec * 0.23 + b);
        }

        // ── the meshes ───────────────────────────────────────────────────
        // A fixed key light in each mesh's own frame; the shading turns with
        // the ring, so the facets catch and lose it as it rotates.
        const glm::dvec3 kKeyLight = glm::normalize(glm::dvec3(0.35, 1.0, 0.25));

        Rgb FacetColour(const glm::dvec3& normal, const glm::dvec3& outward, Rgb base, Rgb accent,
                        double accentBias) {
            const double key = std::max(0.0, glm::dot(normal, kKeyLight));
            // Inner facets (facing the ring's centre) take the accent colour:
            // the ring looks violet from inside and cyan from outside.
            const double inward = std::max(0.0, -glm::dot(normal, outward));
            const Rgb tint = Mix(base, accent, std::clamp(accentBias + 0.8 * inward, 0.0, 1.0));
            const double shade = 0.50 + 0.42 * key + 0.10 * std::max(0.0, normal.y);
            // Top facets bloom toward white.
            return Mix(Scale(tint, shade), kWhite, 0.30 * key * key);
        }

        // The facet normal of a flat face, turned to point away from
        // `interior` (a point inside the solid): only the shading reads it,
        // so the vertex winding never matters.
        glm::dvec3 FacetNormal(const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c,
                               const glm::dvec3& faceCentre, const glm::dvec3& interior) {
            glm::dvec3 n = glm::cross(b - a, c - a);
            const double len = glm::length(n);
            n = len > 1e-9 ? n / len : glm::dvec3(0.0, 1.0, 0.0);
            return glm::dot(n, faceCentre - interior) < 0.0 ? -n : n;
        }

        // One flat-shaded quad (its own four vertices). u/v span the given
        // rect of the crystal texture.
        void FlatQuad(std::vector<Vert>& verts, std::vector<uint32_t>& idx,
                      const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c, const glm::dvec3& d,
                      const glm::dvec3& interior, const glm::dvec3& outward,
                      Rgb base, Rgb accent, double accentBias,
                      float u0, float u1, float v0, float v1) {
            const glm::dvec3 n = FacetNormal(a, b, d, (a + b + c + d) * 0.25, interior);
            const Rgb col = FacetColour(n, outward, base, accent, accentBias);
            const uint32_t base0 = static_cast<uint32_t>(verts.size());
            auto push = [&](const glm::dvec3& p, float u, float v) {
                verts.push_back({static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z),
                                 u, v, col.r, col.g, col.b, 255});
            };
            push(a, u0, v0); push(b, u1, v0); push(c, u1, v1); push(d, u0, v1);
            PushQuad(idx, base0, base0 + 1, base0 + 2, base0 + 3);
        }

        void FlatTri(std::vector<Vert>& verts, std::vector<uint32_t>& idx,
                     const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c,
                     const glm::dvec3& interior, const glm::dvec3& outward,
                     Rgb base, Rgb accent, double accentBias) {
            const glm::dvec3 n = FacetNormal(a, b, c, (a + b + c) / 3.0, interior);
            const Rgb col = FacetColour(n, outward, base, accent, accentBias);
            const uint32_t i0 = static_cast<uint32_t>(verts.size());
            verts.push_back({static_cast<float>(a.x), static_cast<float>(a.y), static_cast<float>(a.z), 0.5f, 0.0f, col.r, col.g, col.b, 255});
            verts.push_back({static_cast<float>(b.x), static_cast<float>(b.y), static_cast<float>(b.z), 0.0f, 1.0f, col.r, col.g, col.b, 255});
            verts.push_back({static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.z), 1.0f, 1.0f, col.r, col.g, col.b, 255});
            for (uint32_t k : {i0, i0 + 1, i0 + 2}) idx.push_back(k);
        }

        // A point on stave cross-section j (of six) at arc angle `theta`,
        // scaled by `s`: an elongated hexagon in the (radial, up) plane.
        glm::dvec3 SectionPoint(double radius, double tube, double theta, int j, double s) {
            const double phi = kTwoPi * j / 6.0 + kPi / 6.0;
            const double radial = radius + tube * s * std::cos(phi);
            return {radial * std::cos(theta), tube * 1.3 * s * std::sin(phi), radial * std::sin(theta)};
        }

        // One ring in the XZ plane: its staves (hexagonal prisms tapering to
        // points at both ends, flat-shaded) and violet nodes. `ringSeed`
        // picks which staves are missing or snapped (fixed per ring index, so
        // every Heart is the same ruin).
        void BuildRing(const RingSpec& spec, uint64_t ringSeed,
                       std::vector<Vert>& verts, std::vector<uint32_t>& idx) {
            const double step = kTwoPi / spec.staves;
            const double gap = 0.16 * step;
            // Stations along a stave: arc fraction and section scale. The
            // ends taper to crystal points; the middle swells slightly.
            constexpr int kStations = 6;
            constexpr double kFrac [kStations] = {0.00, 0.10, 0.30, 0.70, 0.90, 1.00};
            constexpr double kScale[kStations] = {0.15, 0.85, 1.00, 1.00, 0.85, 0.15};

            for (int k = 0; k < spec.staves; ++k) {
                const double roll = Draw(ringSeed, k, 50);
                if (roll < 0.07) continue;                          // a stave lost
                const bool snapped = roll < 0.16;                   // broken short
                const double a0 = k * step + gap * 0.5;
                const double span = (step - gap) * (snapped ? 0.45 + 0.25 * Draw(ringSeed, k, 51) : 1.0);
                // Per-stave colour drift: some staves run more violet.
                const double bias = std::clamp(spec.violet + 0.25 * (Draw(ringSeed, k, 52) - 0.5), 0.0, 1.0);
                const float vRow = static_cast<float>(Draw(ringSeed, k, 53) * 0.5);

                for (int st = 0; st + 1 < kStations; ++st) {
                    const double t0 = a0 + span * kFrac[st];
                    const double t1 = a0 + span * kFrac[st + 1];
                    const double s0 = kScale[st];
                    // A snapped stave ends raggedly: its far end stays thick.
                    const double s1 = (snapped && st + 1 == kStations - 1) ? 0.7 : kScale[st + 1];
                    const glm::dvec3 outward(std::cos(0.5 * (t0 + t1)), 0.0, std::sin(0.5 * (t0 + t1)));
                    const glm::dvec3 axis = outward * static_cast<double>(spec.radius);
                    for (int j = 0; j < 6; ++j) {
                        const int jn = (j + 1) % 6;
                        FlatQuad(verts, idx,
                                 SectionPoint(spec.radius, spec.tube, t0, j, s0),
                                 SectionPoint(spec.radius, spec.tube, t1, j, s1),
                                 SectionPoint(spec.radius, spec.tube, t1, jn, s1),
                                 SectionPoint(spec.radius, spec.tube, t0, jn, s0),
                                 axis, outward, kCyan, kViolet, bias,
                                 static_cast<float>(kFrac[st]), static_cast<float>(kFrac[st + 1]),
                                 vRow + j / 12.0f, vRow + (j + 1) / 12.0f);
                    }
                }
                // End caps (a snapped end shows its broken face).
                for (int end = 0; end < 2; ++end) {
                    const double theta = end == 0 ? a0 : a0 + span;
                    const double s = end == 0 ? kScale[0] : (snapped ? 0.7 : kScale[kStations - 1]);
                    const glm::dvec3 centre(spec.radius * std::cos(theta), 0.0, spec.radius * std::sin(theta));
                    const glm::dvec3 outward(std::cos(theta), 0.0, std::sin(theta));
                    // A point on the axis just inside the stave from this end.
                    const double into = theta + (end == 0 ? 0.25 : -0.25) * span;
                    const glm::dvec3 interior(spec.radius * std::cos(into), 0.0, spec.radius * std::sin(into));
                    for (int j = 0; j < 6; ++j) {
                        FlatTri(verts, idx, centre,
                                SectionPoint(spec.radius, spec.tube, theta, j, s),
                                SectionPoint(spec.radius, spec.tube, theta, (j + 1) % 6, s),
                                interior, outward, kCyan, kViolet, bias);
                    }
                }
                // A violet node on every third whole stave: an elongated
                // octahedron jutting outward from the stave's middle.
                if (!snapped && k % 3 == 0) {
                    const double theta = a0 + span * 0.5;
                    const glm::dvec3 rad(std::cos(theta), 0.0, std::sin(theta));
                    const glm::dvec3 tan(-std::sin(theta), 0.0, std::cos(theta));
                    const glm::dvec3 up(0.0, 1.0, 0.0);
                    const glm::dvec3 c = rad * static_cast<double>(spec.radius + spec.tube * 0.6);
                    const double out = spec.tube * (2.4 + 1.2 * Draw(ringSeed, k, 54));
                    const double w = spec.tube * 0.75;
                    const glm::dvec3 tip = c + rad * out;
                    const glm::dvec3 root = c - rad * (spec.tube * 0.5);
                    const glm::dvec3 ring4[4] = {c + up * w, c + tan * w, c - up * w, c - tan * w};
                    for (int q = 0; q < 4; ++q) {
                        const glm::dvec3& p0 = ring4[q];
                        const glm::dvec3& p1 = ring4[(q + 1) % 4];
                        FlatTri(verts, idx, tip, p0, p1, c, rad, kViolet, kCyan, 0.1);
                        FlatTri(verts, idx, root, p1, p0, c, rad, kViolet, kCyan, 0.1);
                    }
                }
            }
        }

        // The ring's glow: an annulus in its plane (a halo seen from above or
        // below) and a short band round it (the halo seen edge-on).
        void BuildRingGlow(const RingSpec& spec, std::vector<Vert>& verts, std::vector<uint32_t>& idx) {
            const int segments = std::max(48, spec.staves * 4);
            const Rgb col = Mix(kCyan, kViolet, spec.violet);
            AppendAnnulus(verts, idx, segments, spec.radius - spec.tube * 3.0f,
                          spec.radius + spec.tube * 3.0f, 0.0f, col, 0.30);
            AppendBand(verts, idx, segments, spec.radius, spec.tube * 2.6f, col, 0.22);
        }

        // The core: a hexagonal bipyramid, pale and bright.
        void BuildCore(std::vector<Vert>& verts, std::vector<uint32_t>& idx) {
            constexpr double kRadius = 0.8;
            constexpr double kHalf   = 1.5;
            const glm::dvec3 top(0.0, kHalf, 0.0), bottom(0.0, -kHalf, 0.0);
            for (int j = 0; j < 6; ++j) {
                const double a0 = kTwoPi * j / 6.0, a1 = kTwoPi * (j + 1) / 6.0;
                const glm::dvec3 p0(kRadius * std::cos(a0), 0.0, kRadius * std::sin(a0));
                const glm::dvec3 p1(kRadius * std::cos(a1), 0.0, kRadius * std::sin(a1));
                const glm::dvec3 outward = glm::normalize(p0 + p1);
                FlatTri(verts, idx, top, p1, p0, glm::dvec3(0.0), outward, kCoreCol, kViolet, 0.0);
                FlatTri(verts, idx, bottom, p0, p1, glm::dvec3(0.0), outward, kCoreCol, kViolet, 0.15);
            }
        }

        // Crystal texture: pale, faintly striated along the staves (u), with
        // a bright edge at each facet's border (v bands of 1/12).
        std::vector<uint8_t> CrystalPixels(int w, int h) {
            std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const double u = (x + 0.5) / w;
                    const double v = (y + 0.5) / h;
                    const double band = std::fmod(v * 12.0, 1.0);
                    const double edge = std::pow(std::max(0.0, 1.0 - std::min(band, 1.0 - band) * 7.0), 2.0);
                    const double streak = ValueNoise(0x48454152544Cull, u * 14.0 + v * 3.0, 60);
                    const double inner = ValueNoise(0x48454152544Dull, u * 5.0 - v * 7.0, 61);
                    const double lum = std::clamp(0.70 + 0.16 * streak + 0.10 * inner + 0.30 * edge, 0.0, 1.0);
                    const size_t o = (static_cast<size_t>(y) * w + x) * 4;
                    px[o + 0] = px[o + 1] = px[o + 2] = Alpha8(lum);
                    px[o + 3] = 255;
                }
            }
            return px;
        }

        // ── the awakened clock ───────────────────────────────────────────
        // Where every player's rings should be: the level's game time plus
        // the integral of (RingPace - 1) over the city's current stage
        // (Client::AurelithState). A pure function of the record and the
        // time, so all viewers agree; the per-viewer clock eases toward it.
        // Stage boundaries restart the integral: the viewer's clock bridges
        // the step (UpdateViewState), a large one under the stage's flash.
        double SharedRingClock(const std::optional<Client::AurelithState::City>& city, double ticks) {
            namespace S = Client::AurelithState;
            if (!city || city->state == Game::Aurelith::CityState::Dormant) return ticks;
            const double t = S::StageTicks(*city, ticks);
            const double start = static_cast<double>(city->stageStartTick);
            auto integrate = [&](double upTo, auto&& f) {
                constexpr double kStep = 2.0;
                double sum = 0.0;
                for (double u = 0.0; u < upTo; u += kStep) {
                    const double h = std::min(kStep, upTo - u);
                    sum += f(u + 0.5 * h) * h;
                }
                return sum;
            };
            switch (city->state) {
                case Game::Aurelith::CityState::Awakening:
                    return ticks + integrate(std::min(t, 900.0), [&](double u) {
                        return S::RingPace(*city, start + u) - 1.0;
                    });
                case Game::Aurelith::CityState::Contested:
                    return ticks + (S::RingPace(*city, ticks) - 1.0) * t;
                case Game::Aurelith::CityState::Awakened:
                    return ticks + 2.0 * t + integrate(std::min(t, static_cast<double>(Game::Aurelith::kResolveTicks)),
                                                       [&](double u) { return 3.0 * S::Resolution(*city, start + u); });
                default:
                    return ticks;
            }
        }

        uint64_t PosKey(const glm::ivec3& p) {
            return (static_cast<uint64_t>(static_cast<uint32_t>(p.x)) << 38)
                 ^ (static_cast<uint64_t>(static_cast<uint32_t>(p.z)) << 12)
                 ^ static_cast<uint64_t>(static_cast<uint32_t>(p.y) & 0xFFFu);
        }

    } // namespace

    AurelithHeartRenderer::~AurelithHeartRenderer() { Shutdown(); }

    bool AurelithHeartRenderer::Initialize() {
        if (!g_renderBackend) return false;
        if (m_initialized) return true;

        // The shared block-entity shader: fogged like the terrain; every draw
        // here passes an emissive light (the Heart is its own light).
        m_shader = BlockEntityShader::Create();
        if (m_shader == INVALID_SHADER) {
            Log::Error("[AurelithHeartRenderer] shader compile failed");
            return false;
        }

        for (int i = 0; i < kRingCount; ++i) {
            std::vector<Vert> verts;
            std::vector<uint32_t> idx;
            BuildRing(kRings[i], Mix64(0x415552454C495448ull + static_cast<uint64_t>(i)), verts, idx);
            if (!BuildMesh(m_rings[static_cast<size_t>(i)], verts, idx)) {
                Log::Error("[AurelithHeartRenderer] ring %d mesh creation failed", i);
                Shutdown();
                return false;
            }
            verts.clear();
            idx.clear();
            BuildRingGlow(kRings[i], verts, idx);
            if (!BuildMesh(m_ringGlow[static_cast<size_t>(i)], verts, idx)) {
                Log::Error("[AurelithHeartRenderer] ring %d glow mesh creation failed", i);
                Shutdown();
                return false;
            }
        }

        bool ok = true;
        {
            std::vector<Vert> v; std::vector<uint32_t> ix;
            BuildCore(v, ix);
            ok = ok && BuildMesh(m_core, v, ix);
        }
        {
            std::vector<Vert> v; std::vector<uint32_t> ix;
            AppendBillboard(v, ix, kCoreCol, 0.95);
            ok = ok && BuildMesh(m_billboard, v, ix);
        }
        {
            std::vector<Vert> v; std::vector<uint32_t> ix;
            AppendAnnulus(v, ix, 96, 0.86f, 1.14f, 0.0f, Rgb{180, 250, 255}, 0.9);
            ok = ok && BuildMesh(m_pulse, v, ix);
        }
        if (!ok) {
            Log::Error("[AurelithHeartRenderer] mesh creation failed");
            Shutdown();
            return false;
        }

        m_crystalTex = MakeTexture(64, 64, CrystalPixels(64, 64));
        m_bellTex    = MakeTexture(32, 64, BellPixels(32, 64, 0x4155524C4245ull), /*repeatV=*/true);
        m_glowTex    = MakeTexture(64, 64, GlowPixels(64));
        if (m_crystalTex == INVALID_TEXTURE || m_bellTex == INVALID_TEXTURE || m_glowTex == INVALID_TEXTURE) {
            Log::Error("[AurelithHeartRenderer] texture creation failed");
            Shutdown();
            return false;
        }

        // The shaft and the dais pool are the shared volumetric-beam
        // module's. Without it the Heart still turns, glows and notices you;
        // it just casts no light.
        m_beamAcquired = VolumetricBeam::Acquire();
        if (!m_beamAcquired) {
            Log::Warning("[AurelithHeartRenderer] VolumetricBeam unavailable; drawing without the light shaft");
        }

        m_initialized = true;
        return true;
    }

    void AurelithHeartRenderer::Shutdown() {
        if (!g_renderBackend) return;
        for (auto& m : m_rings)    DestroyMesh(m);
        for (auto& m : m_ringGlow) DestroyMesh(m);
        DestroyMesh(m_core);
        DestroyMesh(m_billboard);
        DestroyMesh(m_pulse);
        for (TextureHandle* t : {&m_crystalTex, &m_bellTex, &m_glowTex}) {
            if (*t != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(*t); *t = INVALID_TEXTURE; }
        }
        if (m_shader != INVALID_SHADER) { g_renderBackend->DestroyShader(m_shader); m_shader = INVALID_SHADER; }
        if (m_beamAcquired) { VolumetricBeam::Release(); m_beamAcquired = false; }
        m_views.clear();
        m_initialized = false;
    }

    // ── noticing the viewer ──────────────────────────────────────────────
    // The viewer's clock for this engine. Its speed eases (time constant
    // kSpeedEase) toward kStillSpeed while the viewer is near, and toward
    // 1 + a catch-up term while they are not — the catch-up grows with how
    // far behind the shared clock tau has fallen, so the rings rejoin it
    // smoothly and never overshoot it. Velocity is continuous throughout.
    AurelithHeartRenderer::ViewState& AurelithHeartRenderer::UpdateViewState(
            const glm::ivec3& pos, double ticks, double sharedClock, double pace,
            bool viewerNear, double nearAmount) {
        // Forget engines not drawn for a minute (other cities, other worlds).
        if (m_views.size() > 16) {
            for (auto it = m_views.begin(); it != m_views.end();) {
                if (std::abs(ticks - it->second.lastTicks) > 1200.0) it = m_views.erase(it);
                else ++it;
            }
        }
        ViewState& v = m_views[PosKey(pos)];
        const double dtRaw = ticks - v.lastTicks;
        if (!v.init || dtRaw < 0.0 || dtRaw > 200.0 || std::abs(sharedClock - v.tau) > 2400.0) {
            // New, or the clock jumped (world switch, /time, a long pause, a
            // stage change far behind or ahead): start on the shared clock.
            v = ViewState{};
            v.tau = sharedClock;
            v.lastTicks = ticks;
            v.speed = pace;
            v.init = true;
            return v;
        }
        const double dt = dtRaw;
        const double near = viewerNear ? nearAmount : 0.0;
        // Behind the shared clock: a little faster than the city's pace
        // until caught up (never past it); ahead of it (a stage restarted
        // the integral): a little slower. Velocity stays continuous.
        const double lag = sharedClock - v.tau;
        const double correction = std::clamp(kCatchUpMax * lag / kCatchUpTicks, -0.5 * pace, kCatchUpMax);
        const double target = (pace + correction) * (1.0 - near) + kStillSpeed * near;
        v.speed += (target - v.speed) * (1.0 - std::exp(-dt / kSpeedEase));
        const double next = v.tau + dt * v.speed;
        v.tau = lag > 0.0 ? std::min(sharedClock, next) : next;

        const double prevNotice = v.notice;
        v.notice += (near - v.notice) * (1.0 - std::exp(-dt / kNoticeEase));
        if (prevNotice < 0.5 && v.notice >= 0.5) v.pulseStart = ticks;   // it has seen you
        v.lastTicks = ticks;
        return v;
    }

    void AurelithHeartRenderer::DrawLightShaft(const glm::ivec3& pos, const glm::dvec3& blockCentre,
                                               double light, double notice, double voice, double sour,
                                               double pillar, const glm::mat4& proj, const glm::mat4& view) {
        if (!m_beamAcquired) return;
        const uint64_t key = Mix64(PositionSeed(pos) ^ 0x48454152545348ull);

        std::array<BeamCone, 3> cones;
        size_t coneCount = 2;
        // The column: from the engine's top face up through the rings to
        // above the outer ring's highest point — a faint, drifting haze,
        // white-cyan at its heart and violet at its edge, swelling a little
        // while the Heart watches you. Nothing to clip against (the plaza
        // above the dais is open, and the rings are not terrain).
        BeamCone& up = cones[0];
        up.apex        = blockCentre + glm::dvec3(0.0, 0.5, 0.0);
        up.direction   = glm::vec3(0.0f, 1.0f, 0.0f);
        up.length      = kCoreHeight + 10.0f;
        up.startRadius = 0.7f;
        up.endRadius   = static_cast<float>(1.6 + 0.6 * notice + 1.4 * voice);
        up.coreColor   = glm::mix(glm::vec3(0.86f, 1.0f, 1.0f), glm::vec3(0.62f, 0.40f, 0.95f), static_cast<float>(sour));
        up.edgeColor   = glm::mix(glm::vec3(0.52f, 0.62f, 1.0f), glm::vec3(0.30f, 0.12f, 0.55f), static_cast<float>(sour));
        up.intensity   = static_cast<float>(0.45 * light + 0.35 * notice + 0.9 * voice);
        up.haze        = 0.24f;
        up.anisotropy  = 0.55f;
        up.mist        = 0.6f;
        up.seed        = static_cast<uint32_t>(key);
        up.fadeIn      = 2.0f;
        up.halfIntensityDistance = 18.0f;
        up.fadeOutStart = 0.6f;
        up.terrainClip = false;
        up.lightPool   = false;
        up.cacheKey    = key | 1ull;

        // The dais: a faint wide cone from the core down onto the floor round
        // the engine, there for its pool — the Heart lighting the ground it
        // hangs over. The axis runs straight through the engine and the
        // resonant-crystal pedestal under it (the Aurelith template: engine
        // at y, pedestal at y-1, dais top block at y-2, so the dais floor is
        // kDaisDrop below the engine's top face). The probe starts just past
        // that floor line (terrainStart), in the dais block, so the pool
        // lands on the dais round the pedestal instead of on the engine's
        // top. The apex does NOT bob (the rings' bob would move the floor
        // line in and out of the pedestal), and it is exact: block centre +
        // 11.5 is representable, so the start point is too.
        constexpr float kDaisDrop = 2.0f;
        BeamCone& down = cones[1];
        down.apex        = blockCentre + glm::dvec3(0.0, kCoreHeight, 0.0);
        down.direction   = glm::vec3(0.0f, -1.0f, 0.0f);
        down.length      = kCoreHeight + 2.0f;
        down.startRadius = 0.5f;
        down.endRadius   = 4.5f;
        down.coreColor   = glm::vec3(0.80f, 1.0f, 1.0f);
        down.edgeColor   = glm::vec3(0.37f, 0.95f, 1.0f);
        down.intensity   = static_cast<float>(0.18 * light + 0.22 * notice);
        down.haze        = 0.10f;
        down.anisotropy  = 0.5f;
        down.mist        = 0.4f;
        down.seed        = static_cast<uint32_t>(key >> 32);
        down.fadeIn      = 1.5f;
        down.halfIntensityDistance = 16.0f;
        down.terrainClip   = true;
        down.lightPool     = true;
        down.terrainStart  = (kCoreHeight - 0.5f) + kDaisDrop + 0.02f;
        down.poolIntensity = static_cast<float>(0.55 + 0.45 * notice);
        down.cacheKey      = (key ^ 0x9E3779B97F4A7C15ull) | 1ull;

        // The pillar (the city awake, or waking): out of the rings to where
        // the four gate beams meet over the Heart, and on into the sky — the
        // four voices become one. Violet and guttering under the Undersong.
        if (pillar > 0.001) {
            BeamCone& p = cones[2];
            p.apex        = blockCentre + glm::dvec3(0.0, kCoreHeight + 12.0, 0.0);
            p.direction   = glm::vec3(0.0f, 1.0f, 0.0f);
            p.length      = 240.0f;
            p.startRadius = static_cast<float>(0.6 + 1.2 * pillar);
            p.endRadius   = static_cast<float>(1.0 + 1.8 * pillar);
            p.coreColor   = glm::mix(glm::vec3(0.94f, 1.0f, 1.0f), glm::vec3(0.70f, 0.45f, 1.0f), static_cast<float>(sour));
            p.edgeColor   = glm::mix(glm::vec3(0.45f, 0.90f, 1.0f), glm::vec3(0.35f, 0.15f, 0.70f), static_cast<float>(sour));
            p.intensity   = static_cast<float>(EntityEnvironment::kEmissive * pillar * (1.0 - 0.45 * sour)
                                               * (0.85 + 0.15 * light));
            p.haze        = 0.36f;
            p.extinction  = 0.002f;
            p.anisotropy  = 0.6f;
            p.mist        = 0.5f;
            p.seed        = static_cast<uint32_t>(key >> 16);
            p.fadeIn      = 6.0f;
            p.halfIntensityDistance = 150.0f;
            p.fadeOutStart = 0.6f;
            p.fogFactor   = 0.35f;
            p.terrainClip = false;
            p.lightPool   = false;
            p.cacheKey    = (key ^ 0xC0FFEE1234567ull) | 1ull;
            coneCount = 3;
        }

        VolumetricBeam::Get().Draw(cones.data(), coneCount, nullptr, 0, proj, view);
    }

    void AurelithHeartRenderer::Render(const Game::BlockEntity& be,
                                       float partialTick,
                                       const glm::mat4& proj,
                                       const glm::mat4& view,
                                       const glm::vec3& cameraPos) {
        PROFILE_ZONE_N("BE.AurelithHeart");
        if (!m_initialized || !g_renderBackend) return;

        const glm::ivec3 pos = be.GetWorldPos();
        const glm::dvec3 centre = glm::dvec3(pos) + glm::dvec3(0.5);
        const uint64_t seed = Mix64(PositionSeed(pos));
        const double ticks = EnvironmentState::Get().GameTimeF(partialTick);

        // The clock the rings run on: the shared game time, or — in the
        // player's own view — the player's eased clock (It notices you).
        // The city this Heart belongs to (AurelithS2C), if the server told us.
        const std::optional<Client::AurelithState::City> city =
            Client::AurelithState::CityAt(Client::ClientLevels::ActiveDimension(), pos);
        const bool waking = city && city->state != Game::Aurelith::CityState::Dormant;
        const double voice = city ? Client::AurelithState::Voice(*city, ticks) : 0.0;
        const double sour = city ? Client::AurelithState::Sourness(*city, ticks) : 0.0;
        const double pillar = city ? Client::AurelithState::BeamConvergence(*city, ticks) : 0.0;
        const double resolution = city ? Client::AurelithState::Resolution(*city, ticks) : 0.0;
        const double pace = city ? Client::AurelithState::RingPace(*city, ticks) : 1.0;
        const double shared = SharedRingClock(city, ticks);

        double tau = shared;
        double notice = 0.0;
        double pulseAge = 1e9;   // seconds since the notice pulse began
        glm::dvec3 eye;
        if (LocalViewerEye(eye)) {
            const glm::dvec3 d = eye - centre;
            const double dist = std::hypot(d.x, d.z);
            const double nearAmount = 1.0 - SmoothStep(kNoticeFull, kNoticeNone, dist);
            // A waking Heart is singing, not watching.
            const bool near = !waking && nearAmount > 0.0 && std::abs(d.y) < kNoticeHeight;
            const ViewState& v = UpdateViewState(pos, ticks, shared, pace, near, nearAmount);
            tau = v.tau;
            notice = v.notice;
            pulseAge = (ticks - v.pulseStart) / 20.0;
        }
        // The Undersong's stutter: a shared, noisy tremble of the light and
        // a shudder in every ring.
        const double gutter = sour * (0.5 + 0.5 * ValueNoise(seed, ticks * 0.45, 70));

        const double breath = Breath(seed, ticks);
        // The whole assembly bobs a little on the viewer's clock.
        const double bob = 0.18 * std::sin(tau / 20.0 * (kTwoPi / 9.0) + kTwoPi * Draw(seed, 0, 42));
        const glm::dvec3 ringCentre = centre + glm::dvec3(0.0, kCoreHeight + bob, 0.0);
        const glm::vec3 ringCentreR = ToRender(ringCentre);
        const glm::mat4 viewProj = proj * view;

        // The notice pulse's radius (blocks) and strength.
        double pulseRadius = -100.0, pulseStrength = 0.0;
        if (pulseAge >= 0.0 && pulseAge < kPulseSeconds) {
            pulseRadius = 0.8 + pulseAge * kPulseSpeed;
            pulseStrength = std::pow(1.0 - pulseAge / kPulseSeconds, 1.5);
        }

        // Ring model matrices: precess the tilt axis about +Y, tilt, then
        // spin in the ring's own plane.
        std::array<glm::mat4, kRingCount> ringModel;
        std::array<double, kRingCount> ringBoost{};
        for (int i = 0; i < kRingCount; ++i) {
            const RingTraits tr = TraitsFor(seed, i);
            double spin = RingSpin(tr, kRings[i].dir, tau);
            if (sour > 0.0) {
                spin += sour * 0.07 * (ValueNoise(tr.seed, ticks * 0.6, 71) - 0.5) * kTwoPi;
            }
            const double precess = tr.precess0 + tr.precessRate * tau;
            glm::mat4 m = glm::translate(glm::mat4(1.0f), ringCentreR);
            m = glm::rotate(m, static_cast<float>(precess), glm::vec3(0.0f, 1.0f, 0.0f));
            m = glm::rotate(m, static_cast<float>(kRings[i].tiltDeg * kDeg), glm::vec3(1.0f, 0.0f, 0.0f));
            m = glm::rotate(m, static_cast<float>(spin), glm::vec3(0.0f, 1.0f, 0.0f));
            ringModel[static_cast<size_t>(i)] = m;
            const double dr = (kRings[i].radius - pulseRadius) / 1.8;
            ringBoost[static_cast<size_t>(i)] = pulseStrength * std::exp(-dr * dr);
        }

        const float baseLight = static_cast<float>(EntityEnvironment::kEmissive * breath * (0.88 + 0.12 * notice)
                                                   * (1.0 + 0.45 * voice + 0.6 * resolution) * (1.0 - 0.4 * gutter));

        // ── the crystal (opaque, depth-writing) ──────────────────────────
        g_renderBackend->SetPipelineState(SolidPipeline());
        g_renderBackend->BindShader(m_shader);
        g_renderBackend->SetUniformFloat(m_shader, "uAlphaTest", 0.0f);
        g_renderBackend->BindTexture(m_crystalTex, 0);
        for (int i = 0; i < kRingCount; ++i) {
            const auto& mesh = m_rings[static_cast<size_t>(i)];
            if (!mesh.Valid()) continue;
            const glm::mat4& m = ringModel[static_cast<size_t>(i)];
            g_renderBackend->SetUniformMat4(m_shader, "uMVP", viewProj * m);
            BlockEntityShader::ApplyWorld(m_shader, m, cameraPos);
            BlockEntityShader::SetLight(m_shader, baseLight + static_cast<float>(0.35 * ringBoost[static_cast<size_t>(i)]));
            g_renderBackend->DrawIndexed(mesh.mesh, mesh.indexCount);
        }
        // The core crystal spins on the viewer's clock (a turn every ~10 s).
        if (m_core.Valid()) {
            glm::mat4 m = glm::translate(glm::mat4(1.0f), ringCentreR);
            m = glm::rotate(m, static_cast<float>(tau / 20.0 * (kTwoPi / 10.0)), glm::vec3(0.0f, 1.0f, 0.0f));
            g_renderBackend->SetUniformMat4(m_shader, "uMVP", viewProj * m);
            BlockEntityShader::ApplyWorld(m_shader, m, cameraPos);
            BlockEntityShader::SetLight(m_shader, static_cast<float>(baseLight * (1.0 + 0.2 * notice)));
            g_renderBackend->DrawIndexed(m_core.mesh, m_core.indexCount);
        }

        // ── the light (additive, no depth write) ─────────────────────────
        g_renderBackend->SetPipelineState(GlowPipeline());
        g_renderBackend->BindShader(m_shader);
        g_renderBackend->SetUniformFloat(m_shader, "uAlphaTest", 0.0f);

        // Ring halos, brightened as the pulse passes.
        g_renderBackend->BindTexture(m_bellTex, 0);
        for (int i = 0; i < kRingCount; ++i) {
            const auto& mesh = m_ringGlow[static_cast<size_t>(i)];
            if (!mesh.Valid()) continue;
            const glm::mat4& m = ringModel[static_cast<size_t>(i)];
            g_renderBackend->SetUniformMat4(m_shader, "uMVP", viewProj * m);
            BlockEntityShader::ApplyWorld(m_shader, m, cameraPos);
            BlockEntityShader::SetLight(m_shader, static_cast<float>(
                ((0.55 + 0.25 * notice) * breath + 1.6 * ringBoost[static_cast<size_t>(i)]
                 + 0.7 * voice + 1.4 * resolution) * (1.0 - 0.5 * gutter)));
            g_renderBackend->DrawIndexed(mesh.mesh, mesh.indexCount);
        }

        // The notice pulse: a ring of light rolling out through the rings in
        // the horizontal plane of the core.
        if (pulseStrength > 0.0 && m_pulse.Valid()) {
            glm::mat4 m = glm::translate(glm::mat4(1.0f), ringCentreR);
            m = glm::scale(m, glm::vec3(static_cast<float>(pulseRadius), 1.0f, static_cast<float>(pulseRadius)));
            g_renderBackend->SetUniformMat4(m_shader, "uMVP", viewProj * m);
            BlockEntityShader::ApplyWorld(m_shader, m, cameraPos);
            BlockEntityShader::SetLight(m_shader, static_cast<float>(pulseStrength));
            g_renderBackend->DrawIndexed(m_pulse.mesh, m_pulse.indexCount);
        }

        // The core's glow billboard: breathing, swelling while it watches
        // you, flashing as the pulse leaves it.
        if (m_billboard.Valid()) {
            const double flash = pulseAge >= 0.0 && pulseAge < 1.5 ? std::sin(kPi * pulseAge / 1.5) : 0.0;
            const float size = static_cast<float>(4.2 + 1.2 * (breath - 0.925) / 0.075 + 2.6 * notice + 3.0 * flash
                                                  + 3.5 * voice + 12.0 * resolution);
            const glm::mat4 m = BillboardModel(view, ringCentreR, size);
            g_renderBackend->BindTexture(m_glowTex, 0);
            g_renderBackend->SetUniformMat4(m_shader, "uMVP", viewProj * m);
            BlockEntityShader::ApplyWorld(m_shader, m, cameraPos);
            BlockEntityShader::SetLight(m_shader, static_cast<float>(
                (0.85 * breath + 0.3 * notice + 0.4 * flash + 0.5 * voice + resolution) * (1.0 - 0.45 * gutter)));
            g_renderBackend->DrawIndexed(m_billboard.mesh, m_billboard.indexCount);
        }
        g_renderBackend->UnbindMesh();

        // The light: last, because the beam module leaves its own pipeline
        // state and shader bound.
        DrawLightShaft(pos, centre, breath, notice, voice, sour, pillar, proj, view);
    }

} // namespace Render

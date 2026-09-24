// File: src/client/renderer/blockentity/VoiceBeaconRenderer.cpp
//
// See the header for the design. Layout of this file:
//   the voices     (facing -> voice -> colour)
//   the beacon     (a seed from its position: phase, pulse spacing, breath)
//   the meshes     (baked once per voice: the rising pulse ring)
//   the beam       (VolumetricBeam: the column and the lens glare)
//   the draw
#include "VoiceBeaconRenderer.hpp"
#include "BlockEntityShader.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/core/Log.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include "client/world/ClientBlockAccess.hpp"
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
#include <string_view>
#include <vector>

namespace Render {

    using namespace Aurelith;

    namespace {

        // ── the voices ───────────────────────────────────────────────────
        enum Voice : int { Soprano = 0, Alto = 1, Tenor = 2, Bass = 3 };

        constexpr Rgb kVoiceColour[VoiceBeaconRenderer::kVoiceCount] = {
            {223, 255, 255},   // Soprano #DFFFFF
            { 95, 243, 255},   // Alto    #5FF3FF
            {183, 124, 255},   // Tenor   #B77CFF
            {111, 123, 255},   // Bass    #6F7BFF
        };
        constexpr Rgb kWhite{245, 255, 255};

        // MC BeaconRenderer.BEAM_SCALE_THRESHOLD: past this the beam widens
        // with distance.
        constexpr double kWidenFrom = 96.0;

        // The rising note: a ring every kPulseSpacing ticks (seeded ±20 %)
        // travelling kPulseSpeed blocks per tick up the column, fading as it
        // climbs.
        constexpr double kPulseSpacing = 70.0;     // 3.5 s
        constexpr double kPulseSpeed   = 1.1;      // 22 blocks/s
        constexpr int    kMaxPulses    = 4;

        // The block's `facing` picks the voice: north Soprano, east Alto,
        // south Tenor, west Bass (the gates' order in docs/hush-lore.md).
        int VoiceFor(const Game::BlockEntity& be, uint64_t seed) {
            if (Client::g_clientBlockAccess) {
                const glm::ivec3 p = be.GetWorldPos();
                const Game::BlockState state = Client::g_clientBlockAccess->GetBlockState(p.x, p.y, p.z);
                // Only trust the state when it is this beacon's (a portal
                // view's beacon lives in another level than the one read).
                if (state.Block() == be.GetBlockId()) {
                    const std::string_view facing = state.GetValueByName("facing");
                    if (facing == "north") return Soprano;
                    if (facing == "east")  return Alto;
                    if (facing == "south") return Tenor;
                    if (facing == "west")  return Bass;
                }
            }
            return static_cast<int>(Draw(seed, 0, 1) * VoiceBeaconRenderer::kVoiceCount)
                   % VoiceBeaconRenderer::kVoiceCount;
        }

        // Light in (0.8, 1]: a slow breath, a faint shimmer.
        double BeaconLight(uint64_t seed, double ticks) {
            const double sec = ticks / 20.0;
            const double a = kTwoPi * Draw(seed, 0, 2);
            const double b = kTwoPi * Draw(seed, 0, 3);
            double f = 0.92 + 0.06 * std::sin(sec * 0.9 + a) * std::sin(sec * 0.37 + b);
            f += 0.04 * (ValueNoise(seed, sec * 5.0, 4) - 0.5);
            return std::clamp(f, 0.8, 1.0);
        }

        glm::vec3 ToVec(Rgb c) {
            return glm::vec3(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f);
        }

        // The beacon's world facing as a direction index (0 N, 1 E, 2 S,
        // 3 W) — its gate's outward direction — or -1 when its state is not
        // readable (a portal view's beacon).
        int WorldFacing(const Game::BlockEntity& be) {
            if (!Client::g_clientBlockAccess) return -1;
            const glm::ivec3 p = be.GetWorldPos();
            const Game::BlockState state = Client::g_clientBlockAccess->GetBlockState(p.x, p.y, p.z);
            if (state.Block() != be.GetBlockId()) return -1;
            const std::string_view facing = state.GetValueByName("facing");
            if (facing == "north") return 0;
            if (facing == "east")  return 1;
            if (facing == "south") return 2;
            if (facing == "west")  return 3;
            return -1;
        }

        // The Heart of the beacon's city: kBeaconDistance back along the
        // gate's outward direction, kBeaconAboveHeart below the lens.
        glm::ivec3 HeartOf(const glm::ivec3& beacon, int worldDir) {
            static constexpr int kDx[4] = {0, 1, 0, -1};
            static constexpr int kDz[4] = {-1, 0, 1, 0};
            const int d = Game::Aurelith::kBeaconDistance;
            return glm::ivec3(beacon.x - kDx[worldDir] * d, beacon.y - Game::Aurelith::kBeaconAboveHeart,
                              beacon.z - kDz[worldDir] * d);
        }

        // A point on the quadratic arc a -> (control c) -> b.
        glm::dvec3 Arc(const glm::dvec3& a, const glm::dvec3& c, const glm::dvec3& b, double t) {
            const double u = 1.0 - t;
            return a * (u * u) + c * (2.0 * u * t) + b * (t * t);
        }

    } // namespace

    VoiceBeaconRenderer::~VoiceBeaconRenderer() { Shutdown(); }

    bool VoiceBeaconRenderer::Initialize() {
        if (!g_renderBackend) return false;
        if (m_initialized) return true;

        // The shared block-entity shader: fogged like the terrain, emissive.
        m_shader = BlockEntityShader::Create();
        if (m_shader == INVALID_SHADER) {
            Log::Error("[VoiceBeaconRenderer] shader compile failed");
            return false;
        }

        for (int voice = 0; voice < kVoiceCount; ++voice) {
            const Rgb colour = kVoiceColour[voice];
            const size_t i = static_cast<size_t>(voice);
            // A thin ring of light round the beam, unit radius.
            std::vector<Vert> v; std::vector<uint32_t> ix;
            AppendAnnulus(v, ix, 48, 0.55f, 1.45f, 0.0f, Mix(colour, kWhite, 0.4), 0.8);
            if (!BuildMesh(m_pulse[i], v, ix)) {
                Log::Error("[VoiceBeaconRenderer] mesh creation failed (voice %d)", voice);
                Shutdown();
                return false;
            }
        }

        m_bellTex = MakeTexture(32, 64, BellPixels(32, 64, 0x564F494345ull), /*repeatV=*/true);
        if (m_bellTex == INVALID_TEXTURE) {
            Log::Error("[VoiceBeaconRenderer] texture creation failed");
            Shutdown();
            return false;
        }

        // The column and the glare are the shared volumetric-beam module's.
        // Without it the rising rings still draw; there is just no column.
        m_beamAcquired = VolumetricBeam::Acquire();
        if (!m_beamAcquired) {
            Log::Warning("[VoiceBeaconRenderer] VolumetricBeam unavailable; drawing without the sky beam");
        }
        // The wall's travelling pulse (a quarter per gate). Optional: the
        // beams draw without it.
        m_wallPulseReady = m_wallPulse.Initialize();
        if (!m_wallPulseReady) {
            Log::Warning("[VoiceBeaconRenderer] AurelithWallPulse unavailable; the wall pulse is off");
        }

        m_initialized = true;
        return true;
    }

    void VoiceBeaconRenderer::Shutdown() {
        if (!g_renderBackend) return;
        for (auto& m : m_pulse) DestroyMesh(m);
        if (m_bellTex != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(m_bellTex); m_bellTex = INVALID_TEXTURE; }
        if (m_shader != INVALID_SHADER)   { g_renderBackend->DestroyShader(m_shader);   m_shader = INVALID_SHADER; }
        if (m_beamAcquired) { VolumetricBeam::Release(); m_beamAcquired = false; }
        if (m_wallPulseReady) { m_wallPulse.Shutdown(); m_wallPulseReady = false; }
        m_initialized = false;
    }

    void VoiceBeaconRenderer::DrawSkyBeam(int voice, const glm::ivec3& pos, const glm::dvec3& base,
                                          float widen, float light, double bend, const glm::dvec3& meet,
                                          double sour, const glm::mat4& proj, const glm::mat4& view) {
        if (!m_beamAcquired) return;
        const Rgb colour = Mix(kVoiceColour[voice], Rgb{58, 24, 96}, 0.55 * sour);
        const uint64_t key = Mix64(PositionSeed(pos) ^ 0x564F494345424D00ull);

        // The column, straight up — or, while the Chord is sung, the arc to
        // where the four voices meet over the Heart: up off the lens, then
        // leaning inward (a quadratic whose control point is straight over
        // the lens at the meeting height).
        const glm::dvec3 straightEnd = base + glm::dvec3(0.0, kBeamHeight, 0.0);
        const glm::dvec3 end = glm::mix(straightEnd, meet, bend);
        const glm::dvec3 control = glm::mix(base + glm::dvec3(0.0, kBeamHeight * 0.5, 0.0),
                                            glm::dvec3(base.x, meet.y + 12.0, base.z), bend);
        constexpr int kMaxSegments = 12;
        const int segments = bend < 0.01 ? 1 : kMaxSegments;
        std::array<BeamCone, kMaxSegments> cones;
        double along = 0.0;
        for (int i = 0; i < segments; ++i) {
            const glm::dvec3 a = Arc(base, control, end, static_cast<double>(i) / segments);
            const glm::dvec3 b = Arc(base, control, end, static_cast<double>(i + 1) / segments);
            const glm::dvec3 d = b - a;
            const double len = glm::length(d);
            BeamCone& cone = cones[static_cast<size_t>(i)];
            cone.apex        = a;
            cone.direction   = len > 1e-6 ? glm::vec3(d / len) : glm::vec3(0.0f, 1.0f, 0.0f);
            cone.length      = static_cast<float>(len + (i + 1 < segments ? 0.35 : 0.0));   // overlap the joints
            // A column (equal radii), widened with distance so it stays a line.
            cone.startRadius = 0.65f * widen * static_cast<float>(1.0 + 0.5 * bend);
            cone.endRadius   = cone.startRadius;
            cone.coreColor   = ToVec(Mix(colour, kWhite, 0.65 * (1.0 - 0.6 * sour)));
            cone.edgeColor   = ToVec(colour);
            // A chain restarts the module's falloff at every apex: carry the
            // continuous beam's by hand.
            const double falloff = (150.0 * 150.0) / (150.0 * 150.0 + along * along);
            cone.intensity   = static_cast<float>(light * (segments == 1 ? 1.0 : falloff)
                                                  * (1.0 + 0.35 * bend) * (1.0 - 0.35 * sour));
            cone.haze        = 0.38f;
            cone.extinction  = 0.002f;               // it has to reach the clouds' height
            cone.anisotropy  = 0.6f;
            cone.mist        = 0.45f;
            cone.seed        = static_cast<uint32_t>(key) + static_cast<uint32_t>(i);
            cone.fadeIn      = i == 0 ? 2.0f : 0.0f;
            cone.halfIntensityDistance = 150.0f;
            // Only the free end fades; an arc that has reached the meeting
            // point hands on to the Heart's pillar without a gap.
            cone.fadeOutStart = (i + 1 == segments && bend < 0.999) ? (segments == 1 ? 0.55f : 0.35f) : 1.0f;
            // A landmark: fogged, but only partly, so the four voices still
            // read from far across the Hush when the hills under them are gone.
            cone.fogFactor   = 0.35f;
            // Straight, anything built over the lens stops the beam (a cached
            // raycast); bent, it is the sky it crosses. No pool either way.
            cone.terrainClip  = segments == 1;
            cone.lightPool    = false;
            cone.terrainStart = 0.5f;
            cone.cacheKey     = (key + static_cast<uint64_t>(i) * 0x9E3779B97F4A7C15ull) | 1ull;
            along += len;
        }

        BeamGlare glare;
        glare.position     = base + glm::dvec3(0.0, 0.35, 0.0);
        glare.color        = ToVec(Mix(colour, kWhite, 0.5));
        glare.size         = 1.4f * widen;
        glare.intensity    = static_cast<float>(light * (0.45 + VolumetricBeam::Flare(cones[0], VolumetricBeam::EyeFromView(view))));
        glare.streak       = 0.35f;
        glare.fogFactor    = 0.35f;

        VolumetricBeam::Get().Draw(cones.data(), static_cast<size_t>(segments), &glare, 1, proj, view);
    }

    void VoiceBeaconRenderer::Render(const Game::BlockEntity& be,
                                     float partialTick,
                                     const glm::mat4& proj,
                                     const glm::mat4& view,
                                     const glm::vec3& cameraPos) {
        PROFILE_ZONE_N("BE.VoiceBeacon");
        if (!m_initialized || !g_renderBackend) return;

        const glm::ivec3 pos = be.GetWorldPos();
        const uint64_t seed = Mix64(PositionSeed(pos));
        int voice = VoiceFor(be, seed);
        const double ticks = EnvironmentState::Get().GameTimeF(partialTick);
        double light = BeaconLight(seed, ticks);

        // The beacon's city: the voice by the gate's design direction, the
        // awakening's bend and colour, the wall pulse's light.
        const int worldDir = WorldFacing(be);
        glm::ivec3 heart(0);
        std::optional<Client::AurelithState::City> city;
        if (worldDir >= 0) {
            heart = HeartOf(pos, worldDir);
            city = Client::AurelithState::CityAt(Client::ClientLevels::ActiveDimension(), heart);
        }
        double bend = 0.0, sour = 0.0, cityLight = 0.0, cityVoice = 0.0;
        if (city) {
            const int designVoice = Client::AurelithState::BeaconVoice(*city, pos);
            if (designVoice >= 0) voice = designVoice;
            bend = Client::AurelithState::BeamConvergence(*city, ticks);
            // Ease the bend in and out (the record's value is already eased;
            // this shapes the arc's travel).
            bend = bend * bend * (3.0 - 2.0 * bend);
            sour = Client::AurelithState::Sourness(*city, ticks);
            cityLight = Client::AurelithState::LightLevel(*city, ticks);
            cityVoice = Client::AurelithState::Voice(*city, ticks);
            light *= 1.0 + 0.25 * cityVoice;
        }
        const size_t vi = static_cast<size_t>(voice);

        // The beam leaves the lens's top face.
        const glm::dvec3 base = glm::dvec3(pos) + glm::dvec3(0.5, 1.0, 0.5);
        const glm::vec3 baseR = ToRender(base);
        const glm::mat4 viewProj = proj * view;

        // MC BeaconRenderer.extract: beamRadiusScale = max(1, distance / 96).
        const double camDist = std::hypot(cameraPos.x - base.x, cameraPos.z - base.z);
        const float widen = static_cast<float>(std::max(1.0, camDist / kWidenFrom));

        g_renderBackend->SetPipelineState(GlowPipeline());
        g_renderBackend->BindShader(m_shader);
        g_renderBackend->SetUniformFloat(m_shader, "uAlphaTest", 0.0f);

        // The note rising up the column: rings at a seeded spacing and phase,
        // each fading as it climbs and swelling a little.
        const auto& pulse = m_pulse[vi];
        if (pulse.Valid()) {
            const double spacing = kPulseSpacing * (0.8 + 0.4 * Draw(seed, 0, 5));
            const double phase = spacing * Draw(seed, 0, 6);
            const double newest = std::fmod(ticks + phase, spacing);      // age of the newest ring
            g_renderBackend->BindTexture(m_bellTex, 0);
            for (int k = 0; k < kMaxPulses; ++k) {
                const double age = newest + spacing * k;
                const double h = age * kPulseSpeed;
                if (h >= kBeamHeight) break;
                const double t = h / kBeamHeight;
                const double fade = SmoothStep(0.0, 4.0, h) * std::pow(1.0 - t, 1.4);
                if (fade <= 0.002) continue;
                const float radius = static_cast<float>((0.9 + 0.8 * t) * widen);
                glm::mat4 model = glm::translate(glm::mat4(1.0f), baseR + glm::vec3(0.0f, static_cast<float>(h), 0.0f));
                model = glm::scale(model, glm::vec3(radius, 1.0f, radius));
                g_renderBackend->SetUniformMat4(m_shader, "uMVP", viewProj * model);
                BlockEntityShader::ApplyWorld(m_shader, model, cameraPos);
                BlockEntityShader::SetLight(m_shader, static_cast<float>(0.75 * fade * light));
                g_renderBackend->DrawIndexed(pulse.mesh, pulse.indexCount);
            }
        }

        g_renderBackend->UnbindMesh();

        // The wall's travelling pulse, this gate's quarter of the circuit.
        if (m_wallPulseReady && worldDir >= 0) {
            m_wallPulse.DrawQuadrant(heart, worldDir, ticks, cityLight, cityVoice, proj, view, cameraPos);
        }

        // The column and the lens glare: last, because the beam module
        // leaves its own pipeline state and shader bound.
        const glm::dvec3 meet(heart.x + 0.5, heart.y + Game::Aurelith::kConvergeAboveHeart, heart.z + 0.5);
        DrawSkyBeam(voice, pos, base, widen,
                    static_cast<float>(EntityEnvironment::kEmissive * light), bend, meet, sour, proj, view);
    }

} // namespace Render

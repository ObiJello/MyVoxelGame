// File: src/client/world/AurelithState.cpp
//
// See AurelithState.hpp.
#include "client/world/AurelithState.hpp"

#include "client/entity/ClientMobManager.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "common/entity/EntityLevel.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace Client::AurelithState {

    namespace {

        namespace A = Game::Aurelith;

        std::mutex& Mutex() {
            static std::mutex m;
            return m;
        }
        std::vector<City>& Cities() {
            static std::vector<City> cities;
            return cities;
        }

        struct PendingBurst {
            Game::DimensionId dimension = Game::DimensionId::Hush;
            glm::dvec3 origin{0.0};
            Network::AurelithS2CPacket::BurstStyle style{};
            uint32_t colour = 0xFFFFFF;
        };
        std::vector<PendingBurst>& Bursts() {
            static std::vector<PendingBurst> bursts;
            return bursts;
        }

        double Smooth(double e0, double e1, double x) {
            const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
            return t * t * (3.0 - 2.0 * t);
        }

        double HorizontalDistance(const glm::ivec3& heart, const glm::dvec3& p) {
            return std::hypot(p.x - (heart.x + 0.5), p.z - (heart.z + 0.5));
        }

    } // namespace

    // ── The record ───────────────────────────────────────────────────────

    void OnPacket(const Network::AurelithS2CPacket& packet) {
        const Game::DimensionId dim = Game::DimensionFromRaw(packet.dimension);
        std::lock_guard<std::mutex> lock(Mutex());
        if (packet.kind == Network::AurelithS2CPacket::Kind::Burst) {
            // Bounded: a burst the main thread never drained (a paused
            // client) must not grow without end.
            if (Bursts().size() < 64) {
                Bursts().push_back({ dim, packet.origin, packet.style, packet.colour });
            }
            return;
        }
        auto& cities = Cities();
        auto it = std::find_if(cities.begin(), cities.end(), [&](const City& c) {
            return c.dimension == dim && c.heart == packet.heart;
        });
        if (packet.kind == Network::AurelithS2CPacket::Kind::CityForget) {
            if (it != cities.end()) cities.erase(it);
            return;
        }
        City c;
        c.dimension = dim;
        c.heart = packet.heart;
        c.rotation = packet.rotation;
        c.state = packet.state;
        c.stageStartTick = packet.stageStartTick;
        c.awakenedTick = packet.awakenedTick;
        if (it != cities.end()) *it = c;
        else cities.push_back(c);
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(Mutex());
        Cities().clear();
        Bursts().clear();
    }

    std::vector<City> Snapshot(Game::DimensionId dimension) {
        std::lock_guard<std::mutex> lock(Mutex());
        std::vector<City> out;
        for (const City& c : Cities()) {
            if (c.dimension == dimension) out.push_back(c);
        }
        return out;
    }

    std::optional<City> CityAt(Game::DimensionId dimension, const glm::ivec3& heart) {
        std::lock_guard<std::mutex> lock(Mutex());
        for (const City& c : Cities()) {
            if (c.dimension == dimension && c.heart == heart) return c;
        }
        return std::nullopt;
    }

    std::optional<City> Nearest(Game::DimensionId dimension, const glm::dvec3& pos, double maxDistance) {
        std::lock_guard<std::mutex> lock(Mutex());
        std::optional<City> best;
        double bestD = maxDistance;
        for (const City& c : Cities()) {
            if (c.dimension != dimension) continue;
            const double d = HorizontalDistance(c.heart, pos);
            if (d <= bestD) { bestD = d; best = c; }
        }
        return best;
    }

    std::optional<City> InsideWalls(Game::DimensionId dimension, const glm::dvec3& pos) {
        std::lock_guard<std::mutex> lock(Mutex());
        for (const City& c : Cities()) {
            if (c.dimension == dimension && A::InsideWalls(c.heart, pos)) return c;
        }
        return std::nullopt;
    }

    // ── Bursts ───────────────────────────────────────────────────────────

    void SpawnPendingBursts(Game::DimensionId dimension) {
        std::vector<PendingBurst> bursts;
        {
            std::lock_guard<std::mutex> lock(Mutex());
            bursts.swap(Bursts());
        }
        if (bursts.empty() || !g_clientMobManager) return;
        Game::EntityLevel& level = g_clientMobManager->Level();
        Game::JavaRandom& rng = level.Random();
        using Style = Network::AurelithS2CPacket::BurstStyle;
        constexpr double kTau = 6.283185307179586;

        for (const PendingBurst& b : bursts) {
            if (b.dimension != dimension) continue;
            const float r = ((b.colour >> 16) & 0xFF) / 255.0f;
            const float g = ((b.colour >> 8) & 0xFF) / 255.0f;
            const float bl = (b.colour & 0xFF) / 255.0f;
            // A tinted mote (MobParticleSystem: a non-white tint replaces the
            // mote's own cyan). Pure white is nudged under 1 so it still
            // counts as a tint.
            auto mote = [&](const glm::dvec3& p, const glm::dvec3& v) {
                level.AddColorParticle(Game::ParticleKind::HushMote, p.x, p.y, p.z, v.x, v.y, v.z,
                                       std::min(r, 0.999f), std::min(g, 0.999f), std::min(bl, 0.999f), 1.0f);
            };
            const glm::dvec3& o = b.origin;
            switch (b.style) {
                case Style::KeySeat:
                case Style::Pedestal: {
                    // A ring of the voice's motes lifting off the socket.
                    const int n = b.style == Style::KeySeat ? 20 : 12;
                    for (int i = 0; i < n; ++i) {
                        const double a = kTau * i / n + rng.NextDouble() * 0.2;
                        const double c = std::cos(a), s = std::sin(a);
                        mote(o + glm::dvec3(c * 0.45, 0.1, s * 0.45),
                             glm::dvec3(c * 0.02, 0.05 + rng.NextDouble() * 0.04, s * 0.02));
                    }
                    break;
                }
                case Style::Discord: {
                    // Shards flung outward and down, violet and dark: the
                    // wrong chord breaking apart.
                    for (int i = 0; i < 36; ++i) {
                        const double a = rng.NextDouble() * kTau;
                        const double sp = 0.12 + rng.NextDouble() * 0.18;
                        mote(o + glm::dvec3(0.0, 0.5, 0.0),
                             glm::dvec3(std::cos(a) * sp, 0.05 + rng.NextDouble() * 0.12, std::sin(a) * sp));
                    }
                    for (int i = 0; i < 10; ++i) {
                        level.AddParticle(Game::ParticleKind::LargeSmoke,
                                          o.x + (rng.NextDouble() - 0.5) * 3.0, o.y + rng.NextDouble(),
                                          o.z + (rng.NextDouble() - 0.5) * 3.0, 0.0, 0.02, 0.0);
                    }
                    break;
                }
                case Style::Cabinet: {
                    // The cabinet breathes out a plume of motes as it opens.
                    for (int i = 0; i < 40; ++i) {
                        const double a = rng.NextDouble() * kTau;
                        mote(o + glm::dvec3(std::cos(a) * 0.3, rng.NextDouble() * 0.8, std::sin(a) * 0.3),
                             glm::dvec3(std::cos(a) * 0.03, 0.06 + rng.NextDouble() * 0.08, std::sin(a) * 0.03));
                    }
                    break;
                }
                case Style::HeartBloom: {
                    // The Heart's motes burst outward in every direction,
                    // a sphere of light 20 blocks across in a few seconds.
                    for (int i = 0; i < 160; ++i) {
                        const double u = rng.NextDouble() * 2.0 - 1.0;
                        const double a = rng.NextDouble() * kTau;
                        const double k = std::sqrt(std::max(0.0, 1.0 - u * u));
                        const glm::dvec3 dir(k * std::cos(a), u, k * std::sin(a));
                        const double sp = 0.12 + rng.NextDouble() * 0.2;
                        mote(o + dir * (0.5 + rng.NextDouble()), dir * sp);
                    }
                    break;
                }
                case Style::Resolve: {
                    // The resolution: a slow, wide bloom — rings of motes
                    // rising round the Heart like a held breath let go.
                    for (int ring = 0; ring < 4; ++ring) {
                        const int n = 48;
                        const double rad = 2.0 + ring * 3.0;
                        for (int i = 0; i < n; ++i) {
                            const double a = kTau * i / n + ring * 0.3;
                            const double c = std::cos(a), s = std::sin(a);
                            mote(o + glm::dvec3(c * rad, -ring * 1.5, s * rad),
                                 glm::dvec3(c * 0.04, 0.06 + ring * 0.015, s * 0.04));
                        }
                    }
                    break;
                }
            }
        }
    }

    void TickParticles(Game::DimensionId dimension, const glm::dvec3& camera, int64_t gameTick) {
        static int64_t s_lastTick = -1;
        if (gameTick == s_lastTick || !g_clientMobManager) return;
        s_lastTick = gameTick;
        const std::vector<City> cities = Snapshot(dimension);
        if (cities.empty()) return;
        Game::EntityLevel& level = g_clientMobManager->Level();
        Game::JavaRandom& rng = level.Random();
        const double ticks = static_cast<double>(gameTick);
        constexpr double kTau = 6.283185307179586;
        for (const City& c : cities) {
            if (c.state == A::CityState::Dormant) continue;
            const glm::dvec3 heart(c.heart.x + 0.5, c.heart.y + 0.5, c.heart.z + 0.5);
            if (glm::length(glm::dvec2(camera.x - heart.x, camera.z - heart.z)) > 200.0) continue;
            const glm::dvec3 rings = heart + glm::dvec3(0.0, 11.5, 0.0);
            const double voice = Voice(c, ticks);
            const double sour = Sourness(c, ticks);
            const bool awakeningSung = c.state == A::CityState::Awakening &&
                                       StageTicks(c, ticks) >= A::kMotesBurst;
            // Motes out of the Heart: up through the rings and away.
            int n = 0;
            if (c.state == A::CityState::Awakened) n = 2;
            if (awakeningSung) n = 5;
            if (c.state == A::CityState::Contested) n = 1;
            for (int i = 0; i < n; ++i) {
                const double a = rng.NextDouble() * kTau;
                const double r = rng.NextDouble() * 3.0;
                const glm::dvec3 p = rings + glm::dvec3(std::cos(a) * r, (rng.NextDouble() - 0.5) * 4.0, std::sin(a) * r);
                const glm::dvec3 v(std::cos(a) * 0.03, 0.05 + 0.06 * rng.NextDouble() * voice, std::sin(a) * 0.03);
                level.AddColorParticle(Game::ParticleKind::HushMote, p.x, p.y, p.z, v.x, v.y, v.z,
                                       0.86f, 0.999f, 0.999f, 1.0f);
            }
            // The Undersong: dark motes sinking round the dais.
            if (sour > 0.05 && rng.NextDouble() < sour) {
                const double a = rng.NextDouble() * kTau;
                const double r = 3.0 + rng.NextDouble() * 6.0;
                level.AddColorParticle(Game::ParticleKind::HushMote,
                                       heart.x + std::cos(a) * r, heart.y + 1.0 + rng.NextDouble() * 5.0,
                                       heart.z + std::sin(a) * r, 0.0, -0.03, 0.0, 0.32f, 0.12f, 0.55f, 1.0f);
            }
        }
    }

    // ── The animation ────────────────────────────────────────────────────

    double StageTicks(const City& c, double ticks) {
        return std::max(0.0, ticks - static_cast<double>(c.stageStartTick));
    }

    double WaveRadius(const City& c, double ticks) {
        switch (c.state) {
            case A::CityState::Dormant:   return 0.0;
            case A::CityState::Awakening: return A::WaveRadiusAt(StageTicks(c, ticks));
            default:                      return A::kWaveReach;
        }
    }

    double LightLevel(const City& c, double ticks) {
        switch (c.state) {
            case A::CityState::Dormant:   return 0.0;
            case A::CityState::Awakening: return WaveRadius(c, ticks) / A::kWaveReach;
            default:                      return 1.0;
        }
    }

    bool WaveLitAt(const City& c, double ticks, const glm::dvec3& pos) {
        if (c.state == A::CityState::Dormant) return false;
        if (c.state != A::CityState::Awakening) return true;
        return HorizontalDistance(c.heart, pos) <= WaveRadius(c, ticks);
    }

    double Voice(const City& c, double ticks) {
        const double t = StageTicks(c, ticks);
        switch (c.state) {
            case A::CityState::Dormant:   return 0.0;
            case A::CityState::Awakening:
                return Smooth(A::kSpinUpStart, A::kSpinUpStart + A::kSpinUpTicks, t)
                       * (1.0 - 0.25 * Sourness(c, ticks));
            case A::CityState::Contested: return 0.75;
            case A::CityState::Awakened:  return 1.0;
        }
        return 0.0;
    }

    double Sourness(const City& c, double ticks) {
        const double t = StageTicks(c, ticks);
        switch (c.state) {
            case A::CityState::Dormant:   return 0.0;
            case A::CityState::Awakening: return Smooth(A::kUndersong, A::kUnsungRises, t);
            case A::CityState::Contested: return 1.0;
            case A::CityState::Awakened:  return 1.0 - Smooth(0.0, A::kResolveTicks, t);
        }
        return 0.0;
    }

    double RingPace(const City& c, double ticks) {
        const double t = StageTicks(c, ticks);
        switch (c.state) {
            case A::CityState::Dormant:
                return 1.0;
            case A::CityState::Awakening: {
                // Up to six times the dormant pace as the Chord swells, then
                // faltering back toward half of that as the Undersong
                // answers — the Heart straining against the second note.
                const double up = 1.0 + 5.0 * Smooth(A::kSpinUpStart, A::kSpinUpStart + A::kSpinUpTicks, t);
                return up * (1.0 - 0.5 * Sourness(c, ticks));
            }
            case A::CityState::Contested:
                return 3.0;
            case A::CityState::Awakened:
                // The resolution rushes (a last flourish) and settles to a
                // steady three times the dormant pace: the Chord held again.
                return 3.0 + 3.0 * Resolution(c, ticks);
        }
        return 1.0;
    }

    double BeamConvergence(const City& c, double ticks) {
        switch (c.state) {
            case A::CityState::Dormant: return 0.0;
            case A::CityState::Awakening: {
                const double s = Smooth(A::kBeamBendStart, A::kBeamBendStart + A::kBeamBendTicks,
                                        StageTicks(c, ticks));
                return s;
            }
            default: return 1.0;
        }
    }

    double Resolution(const City& c, double ticks) {
        if (c.state != A::CityState::Awakened) return 0.0;
        return 1.0 - Smooth(0.0, A::kResolveTicks, StageTicks(c, ticks));
    }

    int BeaconVoice(const City& c, const glm::ivec3& beaconPos) {
        if (!c.KnowsRotation()) return -1;
        const glm::ivec2 world(beaconPos.x - c.heart.x, beaconPos.z - c.heart.z);
        const glm::ivec2 d = A::UnrotateOffset(world, c.rotation);
        if (std::abs(d.y) >= std::abs(d.x)) {
            return d.y < 0 ? static_cast<int>(A::Voice::Soprano) : static_cast<int>(A::Voice::Tenor);
        }
        return d.x > 0 ? static_cast<int>(A::Voice::Alto) : static_cast<int>(A::Voice::Bass);
    }

} // namespace Client::AurelithState

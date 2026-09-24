// File: src/client/world/HushSignalState.cpp
//
// See HushSignalState.hpp.
#include "client/world/HushSignalState.hpp"

#include "client/entity/ClientMobManager.hpp"
#include "client/renderer/debug/Gizmos.hpp"
#include "common/entity/EntityLevel.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <vector>

namespace Client::HushSignalState {

    namespace {

        using Clock = std::chrono::steady_clock;
        using Packet = Network::HushSignalS2CPacket;

        struct Ping {
            Game::DimensionId dimension = Game::DimensionId::Overworld;
            Clock::time_point expires;
            std::vector<Packet::Target> targets;
        };

        struct Ring {
            Game::DimensionId dimension = Game::DimensionId::Overworld;
            glm::dvec3 origin{0.0};
            float speed = 0.0f;   // blocks per tick, outward
            int count = 0;
        };

        std::mutex g_mutex;
        std::vector<Ping> g_pings;
        std::vector<Ring> g_rings;
        bool g_compassValid = false;
        Game::DimensionId g_compassDimension = Game::DimensionId::Overworld;
        double g_compassX = 0.0;
        double g_compassZ = 0.0;

        // The outline colours, ARGB. Ores cyan, the echo ore a paler cyan
        // (the shard is the thing you came for), mobs the wraith-red warning
        // colour, and the vault/tomb entrance violet — the Hush palette
        // (docs/the-hush.md), opaque so the lines read through the fog.
        uint32_t ColourOf(Packet::TargetClass cls) {
            switch (cls) {
                case Packet::TargetClass::Ore:       return 0xFF2BD4C0u;
                case Packet::TargetClass::EchoOre:   return 0xFFB8F7F0u;
                case Packet::TargetClass::Mob:       return 0xFFFF6B6Bu;
                case Packet::TargetClass::Structure: return 0xFFB39DDBu;
            }
            return 0xFFFFFFFFu;
        }

        // One horizontal ring of motes, pushed outward at `speed`.
        void SpawnRing(const Ring& ring) {
            if (!g_clientMobManager || ring.count <= 0) return;
            for (int i = 0; i < ring.count; ++i) {
                const double a = 6.283185307179586 * static_cast<double>(i) / ring.count;
                const double c = std::cos(a), s = std::sin(a);
                g_clientMobManager->Level().AddParticle(Game::ParticleKind::HushMote,
                                                ring.origin.x + c * 0.6, ring.origin.y,
                                                ring.origin.z + s * 0.6,
                                                c * ring.speed, 0.0, s * ring.speed);
            }
        }

    } // namespace

    void OnPacket(const Network::HushSignalS2CPacket& packet) {
        const Game::DimensionId dim = Game::DimensionFromRaw(packet.dimension);
        std::lock_guard<std::mutex> lock(g_mutex);
        switch (packet.kind) {
            case Packet::Kind::ResonancePing: {
                Ping ping;
                ping.dimension = dim;
                ping.expires = Clock::now() + std::chrono::milliseconds(
                    static_cast<int64_t>(packet.durationTicks) * 50);
                ping.targets = packet.targets;
                g_pings.push_back(std::move(ping));
                // The ping's ring: wide and slow — it reads as the sound
                // going out from the crystal.
                g_rings.push_back({ dim, packet.origin + glm::dvec3(0.0, 0.5, 0.0), 0.35f, 48 });
                break;
            }
            case Packet::Kind::SonicBurst:
                g_rings.push_back({ dim, packet.origin, std::max(0.1f, packet.radius * 0.12f), 24 });
                break;
            case Packet::Kind::CompassTarget:
                g_compassValid = packet.hasTarget;
                g_compassDimension = dim;
                g_compassX = packet.origin.x;
                g_compassZ = packet.origin.z;
                break;
        }
    }

    void DrawAndSpawn(Game::DimensionId dimension, float framebufferWidth) {
        std::vector<Ring> rings;
        std::vector<Packet::Target> outlines;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            const Clock::time_point now = Clock::now();
            g_pings.erase(std::remove_if(g_pings.begin(), g_pings.end(),
                                         [&](const Ping& p) {
                                             return p.expires <= now || p.dimension != dimension;
                                         }),
                          g_pings.end());
            for (const Ping& p : g_pings) {
                outlines.insert(outlines.end(), p.targets.begin(), p.targets.end());
            }
            for (const Ring& r : g_rings) {
                if (r.dimension == dimension) rings.push_back(r);
            }
            g_rings.clear();
        }

        // MC's line width rule (Window.getAppropriateLineWidth) as the block
        // outline uses it, a touch heavier so a far ore still reads.
        const float width = std::max(2.5f, framebufferWidth / 1920.0f * 3.0f);
        for (const Packet::Target& t : outlines) {
            // Inflated a hair so an ore's outline does not z-fight its own
            // faces where the depth test would have run (it is off, but the
            // hair keeps the lines off the block edges visually too).
            Render::Gizmos::Cuboid(glm::dvec3(t.min) - glm::dvec3(0.02),
                                   glm::dvec3(t.max) + glm::dvec3(0.02),
                                   ColourOf(t.cls), width, /*alwaysOnTop=*/true);
        }
        for (const Ring& r : rings) SpawnRing(r);
    }

    bool CompassTarget(Game::DimensionId dimension, double& x, double& z) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_compassValid || g_compassDimension != dimension) return false;
        x = g_compassX;
        z = g_compassZ;
        return true;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pings.clear();
        g_rings.clear();
        g_compassValid = false;
    }

} // namespace Client::HushSignalState

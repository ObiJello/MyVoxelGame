// File: src/client/renderer/particle/ParticleTicks.cpp
//
// MC Particle.tick and every override (client/particle/*.java), plus the
// render-time rules each type overrides: getQuadSize, getLightCoords, the
// sprite a setSpriteFromAge walk shows.

#include "MobParticleSystem.hpp"

#include "../environment/EntityEnvironment.hpp"
#include "client/entity/RemotePlayerManager.hpp"
#include "client/sound/ClientSounds.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "common/physics/Physics.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/PotentSulfurBlock.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/fluid/FluidState.hpp"
#include "common/world/lighting/LightCoords.hpp"

#include <algorithm>
#include <cmath>

namespace Render {

    namespace {

        using K = Game::ParticleKind;

        glm::ivec3 Containing(double x, double y, double z) {
            return glm::ivec3(static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)),
                              static_cast<int>(std::floor(z)));
        }

        bool FluidIs(const Game::IBlockAccess* blocks, const glm::ivec3& p, Game::FluidType type) {
            return blocks && Game::GetFluidState(*blocks, p).Is(type);
        }

        // FireflyParticle.getFadeAmount.
        float FadeAmount(float progress, float fadeInTime, float fadeOutTime) {
            if (progress >= 1.0f - fadeInTime) return (1.0f - progress) / fadeInTime;
            return progress <= fadeOutTime ? progress / fadeOutTime : 1.0f;
        }

        // The top of the collision shape at (x, z) within `pos`'s cell,
        // relative to the cell floor — VoxelShape.max(Axis.Y, x - px, z - pz).
        double CollisionTopAt(const Game::IBlockAccess& blocks, const glm::ivec3& pos, double x, double z) {
            Game::PhysicsContext ctx;
            ctx.blockAccess = &blocks;
            Game::AABBd region;
            region.min = glm::dvec3(pos);
            region.max = glm::dvec3(pos) + glm::dvec3(1.0);
            static thread_local std::vector<Game::AABBd> boxes;
            Game::CollectBlockColliders(region, ctx, boxes);
            double top = 0.0;
            bool any = false;
            for (const Game::AABBd& b : boxes) {
                if (x < b.min.x || x > b.max.x || z < b.min.z || z > b.max.z) continue;
                if (b.min.y >= pos.y + 1.0 || b.max.y <= pos.y) continue;
                const double t = b.max.y - pos.y;
                top = any ? std::max(top, t) : t;
                any = true;
            }
            return any ? top : 0.0;
        }

        // A vibration's entity destination (EntityPositionSource): a mob, a
        // remote player or the local player, at its feet + yOffset.
        bool EntityPosition(int32_t id, float yOffset, glm::dvec3& out) {
            if (Client::g_clientMobManager) {
                if (const Client::ClientMob* mob = Client::g_clientMobManager->GetMob(id)) {
                    if (mob->mob) {
                        out = mob->mob->position + glm::dvec3(0.0, static_cast<double>(yOffset), 0.0);
                        return true;
                    }
                }
            }
            if (Client::g_remotePlayerManager) {
                const auto& players = Client::g_remotePlayerManager->GetPlayers();
                const auto it = players.find(static_cast<uint32_t>(id));
                if (it != players.end()) {
                    out = it->second.position + glm::dvec3(0.0, static_cast<double>(yOffset), 0.0);
                    return true;
                }
            }
            return false;
        }

    } // namespace

    // ── Particle.tick and the overrides ────────────────────────────────────

    void MobParticleSystem::TickParticle(Particle& p, const Game::IBlockAccess* blocks,
                                         std::vector<ChildSpawn>& spawns) {
        const K kind = p.kind;
        const auto remove = [&]() { p.removed = true; };
        const auto savePrev = [&]() { p.xo = p.x; p.yo = p.y; p.zo = p.z; };

        // ── The unrendered seeds (NO_RENDER) ─────────────────────────────
        switch (kind) {
            case K::ExplosionEmitter: {
                // HugeExplosionSeedParticle.tick: 6 EXPLOSION at ±4 blocks,
                // `size` = age / lifetime, removed at age == lifetime.
                savePrev();
                for (int i = 0; i < 6; ++i) {
                    const double xx = p.x + (m_rng.NextDouble() - m_rng.NextDouble()) * 4.0;
                    const double yy = p.y + (m_rng.NextDouble() - m_rng.NextDouble()) * 4.0;
                    const double zz = p.z + (m_rng.NextDouble() - m_rng.NextDouble()) * 4.0;
                    spawns.push_back(Child(K::Explosion, xx, yy, zz,
                                           static_cast<double>(static_cast<float>(p.age) / static_cast<float>(p.lifetime)),
                                           0.0, 0.0));
                }
                ++p.age;
                if (p.age == p.lifetime) remove();
                return;
            }
            case K::GustEmitterLarge:
            case K::GustEmitterSmall: {
                // GustSeedParticle.tick.
                if (p.age % (p.seedDelay + 1) == 0) {
                    for (int i = 0; i < 3; ++i) {
                        const double gx = p.x + (m_rng.NextDouble() - m_rng.NextDouble()) * p.seedScale;
                        const double gy = p.y + (m_rng.NextDouble() - m_rng.NextDouble()) * p.seedScale;
                        const double gz = p.z + (m_rng.NextDouble() - m_rng.NextDouble()) * p.seedScale;
                        spawns.push_back(Child(K::Gust, gx, gy, gz,
                                               static_cast<double>(static_cast<float>(p.age) / static_cast<float>(p.lifetime)),
                                               0.0, 0.0));
                    }
                }
                if (p.age++ == p.lifetime) remove();
                return;
            }
            case K::NoxiousGasCloud:
            case K::Geyser: {
                // The 26.3 seeds: Particle.tick ages them out at the lifetime,
                // and their own tick runs after it regardless.
                savePrev();
                if (p.age++ >= p.lifetime) remove();
                if (kind == K::NoxiousGasCloud) {
                    // NoxiousGasCloudParticle.tick: every other tick, a random
                    // point up to 3 blocks out and a quarter block down; a
                    // NOXIOUS_GAS puff there when the gas can reach it.
                    if (p.age % 2 == 0 && blocks) {
                        const glm::ivec3 source = Containing(p.x, p.y, p.z);
                        glm::dvec3 dir(static_cast<double>(m_rng.NextFloat() - 0.5f), 0.0,
                                       static_cast<double>(m_rng.NextFloat() - 0.5f));
                        const double length = std::sqrt(dir.x * dir.x + dir.z * dir.z);
                        dir = length < static_cast<double>(1.0e-5f) ? glm::dvec3(0.0) : dir / length;
                        const float distance = m_rng.NextFloat() * 3.0f;
                        const glm::dvec3 spawn =
                            glm::dvec3(source.x + 0.5, source.y + 0.5, source.z + 0.5) +
                            dir * static_cast<double>(distance) - glm::dvec3(0.0, 0.25, 0.0);
                        if (Game::PotentSulfur::CanBeReachedByNoxiousGas(*blocks, source, spawn)) {
                            ChildSpawn c = Child(K::NoxiousGas, spawn.x, spawn.y, spawn.z, 0.0, 0.0, 0.0);
                            c.q.alwaysShow = true;   // addAlwaysVisibleParticle
                            spawns.push_back(std::move(c));
                        }
                    }
                } else {
                    // GeyserEruptionParticle.tick: two GEYSER_BASE every other
                    // tick, waterBlocks + 2 GEYSER_PLUME every tick, twenty
                    // GEYSER_POOF every tenth.
                    const auto emit = [&](K childKind, float burstBase) {
                        ChildSpawn c = Child(childKind, p.x, p.y, p.z, p.xd, p.yd, p.zd);
                        c.q.hasOptions = true;
                        c.q.options = childKind == K::GeyserPlume
                            ? Game::ParticleOptions::Geyser(childKind, p.waterBlocks)
                            : Game::ParticleOptions::GeyserBase(childKind, p.waterBlocks, burstBase);
                        spawns.push_back(std::move(c));
                    };
                    if (p.age % 2 == 0) {
                        for (int i = 0; i < 2; ++i) emit(K::GeyserBase, 1.5f);
                    }
                    for (int i = 0; i < p.waterBlocks + 2; ++i) emit(K::GeyserPlume, 0.0f);
                    if (p.age % 10 == 0) {
                        for (int i = 0; i < 20; ++i) emit(K::GeyserPoof, 2.0f);
                    }
                }
                return;
            }
            case K::FireworkStarter: {
                // FireworkParticles.Starter.tick.
                const auto& explosions = *p.explosions;
                const auto farFromCamera = [&]() {
                    const glm::dvec3 d = glm::dvec3(p.x, p.y, p.z) - m_camera;
                    return glm::dot(d, d) >= 256.0;
                };
                if (p.life == 0 && p.playSound) {
                    const bool far = farFromCamera();
                    bool large = explosions.size() >= 3;
                    for (const Game::FireworkExplosion& e : explosions) {
                        if (e.shape == Game::FireworkExplosion::Shape::LargeBall) { large = true; break; }
                    }
                    const char* sound = large ? (far ? Game::SoundEvents::FIREWORK_ROCKET_LARGE_BLAST_FAR
                                                     : Game::SoundEvents::FIREWORK_ROCKET_LARGE_BLAST)
                                              : (far ? Game::SoundEvents::FIREWORK_ROCKET_BLAST_FAR
                                                     : Game::SoundEvents::FIREWORK_ROCKET_BLAST);
                    Client::Sounds::PlayLocal(glm::dvec3(p.x, p.y, p.z), sound, Game::SoundSource::Ambient, 20.0f,
                                              0.95f + m_rng.NextFloat() * 0.1f, true);
                }
                if (p.life % 2 == 0 && p.life / 2 < static_cast<int>(explosions.size())) {
                    const Game::FireworkExplosion& e = explosions[static_cast<size_t>(p.life / 2)];
                    std::vector<int32_t> colors = e.colors;
                    if (colors.empty()) colors.push_back(1973019);   // DyeColor.BLACK.getFireworkColor()
                    const std::vector<int32_t>& fades = e.fadeColors;

                    // Starter.createParticle: engine.createParticle(FIREWORK)
                    // — the provider, no limiter — then the spark's options.
                    const auto spark = [&](double sx, double sy, double sz, double vx, double vy, double vz) {
                        Client::ClientLevelBridge::QueuedParticle q;
                        q.kind = K::Firework;
                        q.x = sx; q.y = sy; q.z = sz;
                        q.vx = vx; q.vy = vy; q.vz = vz;
                        ChildSpawn c;
                        c.direct = true;
                        if (!MakeParticle(c.particle, q, Game::ParticleOptions(K::Firework), nullptr)) return;
                        Particle& s = c.particle;
                        s.trail = e.hasTrail;
                        s.twinkle = e.hasTwinkle;
                        s.alpha = 0.99f;
                        const uint32_t rgb = static_cast<uint32_t>(colors[static_cast<size_t>(m_rng.NextInt(static_cast<int>(colors.size())))]);
                        s.rCol = static_cast<float>((rgb >> 16) & 255) / 255.0f;
                        s.gCol = static_cast<float>((rgb >> 8) & 255) / 255.0f;
                        s.bCol = static_cast<float>(rgb & 255) / 255.0f;
                        if (!fades.empty()) {
                            const uint32_t f = static_cast<uint32_t>(fades[static_cast<size_t>(m_rng.NextInt(static_cast<int>(fades.size())))]);
                            s.fadeR = static_cast<float>((f >> 16) & 255) / 255.0f;
                            s.fadeG = static_cast<float>((f >> 8) & 255) / 255.0f;
                            s.fadeB = static_cast<float>(f & 255) / 255.0f;
                            s.hasFade = true;
                        }
                        spawns.push_back(std::move(c));
                    };
                    const auto ball = [&](double baseSpeed, int steps) {
                        for (int yStep = -steps; yStep <= steps; ++yStep) {
                            for (int xStep = -steps; xStep <= steps; ++xStep) {
                                for (int zStep = -steps; zStep <= steps; ++zStep) {
                                    const double bx = xStep + (m_rng.NextDouble() - m_rng.NextDouble()) * 0.5;
                                    const double by = yStep + (m_rng.NextDouble() - m_rng.NextDouble()) * 0.5;
                                    const double bz = zStep + (m_rng.NextDouble() - m_rng.NextDouble()) * 0.5;
                                    const double len = std::sqrt(bx * bx + by * by + bz * bz) / baseSpeed +
                                                       m_rng.NextGaussian() * 0.05;
                                    spark(p.x, p.y, p.z, bx / len, by / len, bz / len);
                                    if (yStep != -steps && yStep != steps && xStep != -steps && xStep != steps) {
                                        zStep += steps * 2 - 1;
                                    }
                                }
                            }
                        }
                    };
                    const auto shape = [&](double baseSpeed, const double (*coords)[2], int n, bool flat) {
                        const double sx = coords[0][0], sy = coords[0][1];
                        spark(p.x, p.y, p.z, sx * baseSpeed, sy * baseSpeed, 0.0);
                        const float baseAngle = m_rng.NextFloat() * 3.1415927f;
                        const double angleMod = flat ? 0.034 : 0.34;
                        for (int angleStep = 0; angleStep < 3; ++angleStep) {
                            const double angle = static_cast<double>(baseAngle) +
                                                 static_cast<double>(static_cast<float>(angleStep) * 3.1415927f) * angleMod;
                            double ox = sx, oy = sy;
                            for (int c = 1; c < n; ++c) {
                                const double tx = coords[c][0], ty = coords[c][1];
                                for (double subStep = 0.25; subStep <= 1.0; subStep += 0.25) {
                                    double vx = (ox + (tx - ox) * subStep) * baseSpeed;
                                    const double vy = (oy + (ty - oy) * subStep) * baseSpeed;
                                    const double vz = vx * std::sin(angle);
                                    vx *= std::cos(angle);
                                    for (double flip = -1.0; flip <= 1.0; flip += 2.0) {
                                        spark(p.x, p.y, p.z, vx * flip, vy, vz * flip);
                                    }
                                }
                                ox = tx;
                                oy = ty;
                            }
                        }
                    };
                    static constexpr double kCreeper[][2] = {{0.0, 0.2}, {0.2, 0.2}, {0.2, 0.6}, {0.6, 0.6},
                                                             {0.6, 0.2}, {0.2, 0.2}, {0.2, 0.0}, {0.4, 0.0},
                                                             {0.4, -0.6}, {0.2, -0.6}, {0.2, -0.4}, {0.0, -0.4}};
                    static constexpr double kStar[][2] = {{0.0, 1.0}, {0.3455, 0.309}, {0.9511, 0.309},
                                                          {0.3795918367346939, -0.12653061224489795},
                                                          {0.6122448979591837, -0.8040816326530612},
                                                          {0.0, -0.35918367346938773}};
                    switch (e.shape) {
                        case Game::FireworkExplosion::Shape::SmallBall: ball(0.25, 2); break;
                        case Game::FireworkExplosion::Shape::LargeBall: ball(0.5, 4); break;
                        case Game::FireworkExplosion::Shape::Star:      shape(0.5, kStar, 6, false); break;
                        case Game::FireworkExplosion::Shape::Creeper:   shape(0.5, kCreeper, 12, true); break;
                        case Game::FireworkExplosion::Shape::Burst: {
                            const double baseOffX = m_rng.NextGaussian() * 0.05;
                            const double baseOffZ = m_rng.NextGaussian() * 0.05;
                            for (int i = 0; i < 70; ++i) {
                                const double vx = p.xd * 0.5 + m_rng.NextGaussian() * 0.15 + baseOffX;
                                const double vz = p.zd * 0.5 + m_rng.NextGaussian() * 0.15 + baseOffZ;
                                const double vy = p.yd * 0.5 + m_rng.NextDouble() * 0.5;
                                spark(p.x, p.y, p.z, vx, vy, vz);
                            }
                            break;
                        }
                    }
                    // engine.createParticle(ColorParticleOption.create(FLASH, colors[0])).
                    {
                        Client::ClientLevelBridge::QueuedParticle q;
                        q.kind = K::Flash;
                        q.x = p.x; q.y = p.y; q.z = p.z;
                        ChildSpawn c;
                        c.direct = true;
                        const Game::ParticleOptions flash =
                            Game::ParticleOptions::Color(K::Flash, 0xFF000000u | static_cast<uint32_t>(colors[0]));
                        if (MakeParticle(c.particle, q, flash, nullptr)) spawns.push_back(std::move(c));
                    }
                }
                ++p.life;
                if (p.life > p.lifetime) {
                    if (p.twinkleDelay && p.playSound) {
                        const bool far = farFromCamera();
                        Client::Sounds::PlayLocal(glm::dvec3(p.x, p.y, p.z),
                                                  far ? Game::SoundEvents::FIREWORK_ROCKET_TWINKLE_FAR
                                                      : Game::SoundEvents::FIREWORK_ROCKET_TWINKLE,
                                                  Game::SoundSource::Ambient, 20.0f, 0.9f + m_rng.NextFloat() * 0.15f, true);
                    }
                    remove();
                }
                return;
            }
            default:
                break;
        }

        // ── Types with a tick of their own (no Particle.tick) ─────────────
        switch (kind) {
            case K::Explosion:
            case K::SonicBoom:
            case K::SweepAttack:
                // HugeExplosionParticle / AttackSweepParticle.tick: age only.
                savePrev();
                if (p.age++ >= p.lifetime) remove();
                return;
            case K::Gust:
            case K::SmallGust:
                // GustParticle.tick (no xo update: it never moves).
                if (p.age++ >= p.lifetime) remove();
                return;

            case K::HushMote: {
                savePrev();
                if (p.age++ >= p.lifetime) { remove(); return; }
                // Damped drift with a slow bob; alpha rises over the first
                // eighth of the life and falls over the last quarter.
                p.x += p.xd; p.y += p.yd; p.z += p.zd;
                p.xd *= 0.985; p.yd *= 0.985; p.zd *= 0.985;
                p.yd += 0.00015 * std::sin(static_cast<double>(p.age) * 0.12);
                const float t = static_cast<float>(p.age) / static_cast<float>(p.lifetime);
                p.alpha = std::clamp(std::min(t * 8.0f, (1.0f - t) * 4.0f), 0.0f, 0.9f);
                return;
            }
            case K::HushMist: {
                savePrev();
                if (p.age++ >= p.lifetime) { remove(); return; }
                p.x += p.xd; p.y += p.yd; p.z += p.zd;
                p.xd *= 0.992; p.zd *= 0.992; p.yd *= 0.98;
                const double wander = static_cast<double>(p.age) * 0.035 + p.spriteFrame;
                p.xd += 0.0004 * std::sin(wander);
                p.zd += 0.0004 * std::cos(wander * 0.8);
                const float t = static_cast<float>(p.age) / static_cast<float>(p.lifetime);
                p.alpha = 0.2f * std::clamp(std::min(t * 5.0f, (1.0f - t) * 2.0f), 0.0f, 1.0f);
                return;
            }
            case K::VesperGlint: {
                savePrev();
                if (p.age++ >= p.lifetime) { remove(); return; }
                p.x += p.xd; p.y += p.yd; p.z += p.zd;
                p.xd *= 0.96; p.yd *= 0.9; p.zd *= 0.96;
                const float t = static_cast<float>(p.age) / static_cast<float>(p.lifetime);
                const float twinkle = 0.55f + 0.45f * std::sin(static_cast<float>(p.age) * 1.7f +
                                                               static_cast<float>(p.spriteFrame) * 0.1f);
                p.alpha = std::clamp(std::min(t * 6.0f, (1.0f - t) * 3.0f), 0.0f, 1.0f) * twinkle;
                return;
            }

            case K::Portal:
            case K::HushPortal:
            case K::AetherPortal: {
                // PortalParticle.tick: a closed curve from the start — out
                // along the velocity, back home, rising a block.
                savePrev();
                if (p.age++ >= p.lifetime) { remove(); return; }
                float pos = static_cast<float>(p.age) / static_cast<float>(p.lifetime);
                const float a = pos;
                pos = -pos + pos * pos * 2.0f;
                pos = 1.0f - pos;
                p.x = p.xStart + p.xd * static_cast<double>(pos);
                p.y = p.yStart + p.yd * static_cast<double>(pos) + static_cast<double>(1.0f - a);
                p.z = p.zStart + p.zd * static_cast<double>(pos);
                return;
            }
            case K::ReversePortal: {
                savePrev();
                if (p.age++ >= p.lifetime) { remove(); return; }
                const float speedMultiplier = static_cast<float>(p.age) / static_cast<float>(p.lifetime);
                p.x += p.xd * static_cast<double>(speedMultiplier);
                p.y += p.yd * static_cast<double>(speedMultiplier);
                p.z += p.zd * static_cast<double>(speedMultiplier);
                return;
            }
            case K::Enchant:
            case K::Nautilus:
            case K::VaultConnection: {
                // FlyTowardsPositionParticle.tick.
                savePrev();
                if (p.age++ >= p.lifetime) { remove(); return; }
                float pos = static_cast<float>(p.age) / static_cast<float>(p.lifetime);
                pos = 1.0f - pos;
                float pp = 1.0f - pos;
                pp *= pp;
                pp *= pp;
                p.x = p.xStart + p.xd * static_cast<double>(pos);
                p.y = p.yStart + p.yd * static_cast<double>(pos) - static_cast<double>(pp * 1.2f);
                p.z = p.zStart + p.zd * static_cast<double>(pos);
                return;
            }
            case K::OminousSpawning: {
                // FlyStraightTowardsParticle.tick: straight in, the colour
                // lerped in sRGB (ARGB.srgbLerp — Mth.lerpInt per channel).
                savePrev();
                if (p.age++ >= p.lifetime) { remove(); return; }
                const float t = static_cast<float>(p.age) / static_cast<float>(p.lifetime);
                const float posAlpha = 1.0f - t;
                p.x = p.xStart + p.xd * static_cast<double>(posAlpha);
                p.y = p.yStart + p.yd * static_cast<double>(posAlpha);
                p.z = p.zStart + p.zd * static_cast<double>(posAlpha);
                const auto lerpInt = [t](int a, int b) {
                    return a + static_cast<int>(std::floor(t * static_cast<float>(b - a)));
                };
                const auto ch = [](uint32_t c, int s) { return static_cast<int>((c >> s) & 255u); };
                p.rCol = static_cast<float>(lerpInt(ch(p.startColor, 16), ch(p.endColor, 16))) / 255.0f;
                p.gCol = static_cast<float>(lerpInt(ch(p.startColor, 8), ch(p.endColor, 8))) / 255.0f;
                p.bCol = static_cast<float>(lerpInt(ch(p.startColor, 0), ch(p.endColor, 0))) / 255.0f;
                p.alpha = static_cast<float>(lerpInt(ch(p.startColor, 24), ch(p.endColor, 24))) / 255.0f;
                return;
            }
            case K::FallingDust: {
                // FallingDustParticle.tick: move first, then its own clamped
                // acceleration — a steady trickle, not a fall.
                savePrev();
                if (p.age++ >= p.lifetime) { remove(); return; }
                p.oRoll = p.roll;
                p.roll += 3.1415927f * p.rollSpeed * 2.0f;
                if (p.onGround) p.oRoll = p.roll = 0.0f;
                MoveParticle(p, p.xd, p.yd, p.zd, blocks);
                p.yd -= 0.003000000026077032;
                p.yd = std::max(p.yd, -0.14000000059604645);
                return;
            }
            case K::Bubble: {
                // BubbleParticle.tick: counts its lifetime down.
                savePrev();
                if (p.lifetime-- <= 0) { remove(); return; }
                p.yd += 0.002;
                MoveParticle(p, p.xd, p.yd, p.zd, blocks);
                p.xd *= 0.8500000238418579; p.yd *= 0.8500000238418579; p.zd *= 0.8500000238418579;
                if (!FluidIs(blocks, Containing(p.x, p.y, p.z), Game::FluidType::Water)) remove();
                return;
            }
            case K::Rain:
            case K::Splash: {
                // WaterDropParticle.tick.
                savePrev();
                if (p.lifetime-- <= 0) { remove(); return; }
                p.yd -= static_cast<double>(p.gravity);
                MoveParticle(p, p.xd, p.yd, p.zd, blocks);
                p.xd *= 0.9800000190734863; p.yd *= 0.9800000190734863; p.zd *= 0.9800000190734863;
                if (p.onGround) {
                    if (m_rng.NextFloat() < 0.5f) remove();
                    p.xd *= 0.699999988079071;
                    p.zd *= 0.699999988079071;
                }
                if (blocks) {
                    const glm::ivec3 pos = Containing(p.x, p.y, p.z);
                    const Game::FluidState fluid = Game::GetFluidState(*blocks, pos);
                    const double fluidTop = fluid.IsEmpty() ? 0.0 : static_cast<double>(Game::FluidHeight(*blocks, pos, fluid));
                    const double offset = std::max(CollisionTopAt(*blocks, pos, p.x, p.z), fluidTop);
                    if (offset > 0.0 && p.y < static_cast<double>(pos.y) + offset) remove();
                }
                return;
            }
            case K::Fishing: {
                // WakeParticle.tick.
                savePrev();
                const int life = 60 - p.lifetime;
                if (p.lifetime-- <= 0) { remove(); return; }
                p.yd -= static_cast<double>(p.gravity);
                MoveParticle(p, p.xd, p.yd, p.zd, blocks);
                p.xd *= 0.9800000190734863; p.yd *= 0.9800000190734863; p.zd *= 0.9800000190734863;
                const float size = static_cast<float>(life) * 0.001f;
                SetSize(p, size, size);
                p.sprite = m_sprites.Get(p.spriteSet, life % 4, 4);
                return;
            }
            case K::HappyVillager:
            case K::EggCrack:
            case K::Composter:
            case K::Dolphin:
            case K::Mycelium: {
                // SuspendedTownParticle.tick.
                savePrev();
                if (p.lifetime-- <= 0) { remove(); return; }
                MoveParticle(p, p.xd, p.yd, p.zd, blocks);
                p.xd *= 0.99; p.yd *= 0.99; p.zd *= 0.99;
                return;
            }
            case K::BubblePop: {
                savePrev();
                if (p.age++ >= p.lifetime) { remove(); return; }
                p.yd -= static_cast<double>(p.gravity);
                MoveParticle(p, p.xd, p.yd, p.zd, blocks);
                return;
            }
            case K::CurrentDown: {
                savePrev();
                if (p.age++ >= p.lifetime) { remove(); return; }
                p.xd += static_cast<double>(0.6f * std::cos(p.angle));
                p.zd += static_cast<double>(0.6f * std::sin(p.angle));
                p.xd *= 0.07;
                p.zd *= 0.07;
                MoveParticle(p, p.xd, p.yd, p.zd, blocks);
                if (!FluidIs(blocks, Containing(p.x, p.y, p.z), Game::FluidType::Water) || p.onGround) remove();
                p.angle += 0.08f;
                return;
            }
            case K::DragonBreath: {
                savePrev();
                if (p.age++ >= p.lifetime) { remove(); return; }
                if (p.onGround) {
                    p.yd = 0.0;
                    p.hasHitGround = true;
                }
                if (p.hasHitGround) p.yd += 0.002;
                MoveParticle(p, p.xd, p.yd, p.zd, blocks);
                if (p.y == p.yo) {
                    p.xd *= 1.1;
                    p.zd *= 1.1;
                }
                p.xd *= static_cast<double>(p.friction);
                p.zd *= static_cast<double>(p.friction);
                if (p.hasHitGround) p.yd *= static_cast<double>(p.friction);
                return;
            }
            case K::CampfireCosySmoke:
            case K::CampfireSignalSmoke: {
                savePrev();
                if (p.age++ < p.lifetime && !(p.alpha <= 0.0f)) {
                    p.xd += static_cast<double>(m_rng.NextFloat() / 5000.0f * static_cast<float>(m_rng.NextBool() ? 1 : -1));
                    p.zd += static_cast<double>(m_rng.NextFloat() / 5000.0f * static_cast<float>(m_rng.NextBool() ? 1 : -1));
                    p.yd -= static_cast<double>(p.gravity);
                    MoveParticle(p, p.xd, p.yd, p.zd, blocks);
                    if (p.age >= p.lifetime - 60 && p.alpha > 0.01f) p.alpha -= 0.015f;
                } else {
                    remove();
                }
                return;
            }
            case K::Trail: {
                savePrev();
                if (p.age++ >= p.lifetime) { remove(); return; }
                const int ticksRemaining = p.lifetime - p.age;
                const double alpha = 1.0 / static_cast<double>(ticksRemaining);
                p.x += (p.target.x - p.x) * alpha;
                p.y += (p.target.y - p.y) * alpha;
                p.z += (p.target.z - p.z) * alpha;
                return;
            }
            case K::Vibration: {
                savePrev();
                if (p.age++ >= p.lifetime) { remove(); return; }
                glm::dvec3 destination = p.target;
                if (p.targetEntity >= 0 && !EntityPosition(p.targetEntity, p.targetYOffset, destination)) {
                    remove();
                    return;
                }
                const int ticksRemaining = p.lifetime - p.age;
                const double alpha = 1.0 / static_cast<double>(ticksRemaining);
                p.x += (destination.x - p.x) * alpha;
                p.y += (destination.y - p.y) * alpha;
                p.z += (destination.z - p.z) * alpha;
                const double dx = p.x - destination.x, dy = p.y - destination.y, dz = p.z - destination.z;
                p.rotO = p.rot;
                p.rot = static_cast<float>(std::atan2(dx, dz));
                p.pitchO = p.pitch;
                p.pitch = static_cast<float>(std::atan2(dy, std::sqrt(dx * dx + dz * dz)));
                return;
            }
            case K::CherryLeaves:
            case K::PaleOakLeaves:
            case K::RedPoplarLeaves:
            case K::OrangePoplarLeaves:
            case K::YellowPoplarLeaves:
            case K::TintedLeaves: {
                // FallingParticle.tick.
                savePrev();
                if (p.lifetime-- <= 0) remove();
                if (p.removed) return;
                const float aliveTicks = static_cast<float>(300 - p.lifetime);
                const float relativeAge = std::min(aliveTicks / 300.0f, 1.0f);
                double fxa = 0.0, fza = 0.0;
                if (p.flowAway) {
                    fxa += p.xaFlowScale * std::pow(static_cast<double>(relativeAge), 1.25);
                    fza += p.zaFlowScale * std::pow(static_cast<double>(relativeAge), 1.25);
                }
                if (p.swirl) {
                    fxa += static_cast<double>(relativeAge) * std::cos(static_cast<double>(relativeAge) * p.swirlPeriod) *
                           static_cast<double>(p.windBig);
                    fza += static_cast<double>(relativeAge) * std::sin(static_cast<double>(relativeAge) * p.swirlPeriod) *
                           static_cast<double>(p.windBig);
                }
                p.xd += fxa * 0.0024999999441206455;
                p.zd += fza * 0.0024999999441206455;
                p.yd -= static_cast<double>(p.gravity);
                p.rotSpeed += p.spinAcceleration / 20.0f;
                p.oRoll = p.roll;
                p.roll += p.rotSpeed / 20.0f;
                MoveParticle(p, p.xd, p.yd, p.zd, blocks);
                if (p.onGround || (p.lifetime < 299 && (p.xd == 0.0 || p.zd == 0.0))) remove();
                if (!p.removed) {
                    p.xd *= static_cast<double>(p.friction);
                    p.yd *= static_cast<double>(p.friction);
                    p.zd *= static_cast<double>(p.friction);
                }
                return;
            }
            default:
                break;
        }

        // ── DripParticle.tick ─────────────────────────────────────────────
        if (p.drip != Drip::None) {
            savePrev();
            // preMoveUpdate.
            if (p.drip == Drip::CoolingHang) {
                p.rCol = 1.0f;
                p.gCol = 16.0f / static_cast<float>(40 - p.lifetime + 16);
                p.bCol = 4.0f / static_cast<float>(40 - p.lifetime + 8);
            }
            if (p.lifetime-- <= 0) {
                remove();
                if (p.drip == Drip::Hang || p.drip == Drip::CoolingHang) {
                    spawns.push_back(Child(p.chainKind, p.x, p.y, p.z, p.xd, p.yd, p.zd));
                }
            }
            if (p.removed) return;
            p.yd -= static_cast<double>(p.gravity);
            MoveParticle(p, p.xd, p.yd, p.zd, blocks);
            // postMoveUpdate.
            switch (p.drip) {
                case Drip::Hang:
                case Drip::CoolingHang:
                    p.xd *= 0.02; p.yd *= 0.02; p.zd *= 0.02;
                    break;
                case Drip::Falling:
                    if (p.onGround) remove();
                    break;
                case Drip::FallAndLand:
                case Drip::DripstoneFallAndLand:
                case Drip::HoneyFallAndLand:
                    if (p.onGround) {
                        remove();
                        spawns.push_back(Child(p.chainKind, p.x, p.y, p.z, 0.0, 0.0, 0.0));
                        const float volume = m_rng.NextFloat() * (1.0f - 0.3f) + 0.3f;   // Mth.randomBetween(0.3, 1)
                        if (p.drip == Drip::DripstoneFallAndLand) {
                            Client::Sounds::PlayLocal(glm::dvec3(p.x, p.y, p.z),
                                                      p.dripFluid == DripFluid::Lava
                                                          ? Game::SoundEvents::POINTED_DRIPSTONE_DRIP_LAVA
                                                          : Game::SoundEvents::POINTED_DRIPSTONE_DRIP_WATER,
                                                      Game::SoundSource::Blocks, volume, 1.0f, false);
                        } else if (p.drip == Drip::HoneyFallAndLand) {
                            Client::Sounds::PlayLocal(glm::dvec3(p.x, p.y, p.z), Game::SoundEvents::BEEHIVE_DRIP,
                                                      Game::SoundSource::Blocks, volume, 1.0f, false);
                        }
                    }
                    break;
                default:
                    break;
            }
            if (p.removed) return;
            p.xd *= 0.9800000190734863; p.yd *= 0.9800000190734863; p.zd *= 0.9800000190734863;
            if (p.dripFluid != DripFluid::Empty && blocks) {
                const glm::ivec3 pos = Containing(p.x, p.y, p.z);
                const Game::FluidState fluid = Game::GetFluidState(*blocks, pos);
                const Game::FluidType want = p.dripFluid == DripFluid::Lava ? Game::FluidType::Lava : Game::FluidType::Water;
                if (fluid.Is(want) &&
                    p.y < static_cast<double>(static_cast<float>(pos.y) + Game::FluidHeight(*blocks, pos, fluid))) {
                    remove();
                }
            }
            return;
        }

        // ── Pre-tick tweaks ───────────────────────────────────────────────
        if (kind == K::DustPlume) {
            // DustPlumeParticle.tick: the pull and the drag fade each tick.
            p.gravity = 0.88f * p.gravity;
            p.friction = 0.92f * p.friction;
        }
        if (kind == K::Shriek && p.delay > 0) {
            --p.delay;
            return;
        }

        // ── MC Particle.tick ──────────────────────────────────────────────
        savePrev();
        if (p.age++ >= p.lifetime) {
            remove();
            return;
        }
        p.yd -= 0.04 * static_cast<double>(p.gravity);
        MoveParticle(p, p.xd, p.yd, p.zd, blocks);
        if (p.speedUpWhenYMotionIsBlocked && p.y == p.yo) {
            p.xd *= 1.1;
            p.zd *= 1.1;
        }
        p.xd *= static_cast<double>(p.friction);
        p.yd *= static_cast<double>(p.friction);
        p.zd *= static_cast<double>(p.friction);
        if (p.onGround) {
            p.xd *= 0.699999988079071;
            p.zd *= 0.699999988079071;
        }

        // ── The subclass halves that run after super.tick() ──────────────
        switch (kind) {
            case K::SulfurBubbles: {
                // SulfurBubbleParticle.tick.
                const bool inWater = blocks &&
                    Game::GetFluidState(*blocks, Containing(p.x, p.y, p.z)).IsSourceOf(Game::FluidType::Water);
                if (!p.removed && !inWater) remove();
                if (!p.removed && p.y >= p.yEnd) remove();
                if (!p.removed && p.y <= p.yPrev) remove();
                const auto wiggle = [&]() {
                    return static_cast<double>(m_rng.NextFloat() * 0.003f *
                                               static_cast<float>(m_rng.NextBool() ? 1 : -1)) * 0.5;
                };
                p.xd += wiggle();
                p.zd += wiggle();
                MoveParticle(p, p.xd, 0.0, p.zd, blocks);
                const float travelProgress = static_cast<float>((p.y - p.yStart) / (p.yEnd - p.yStart));
                p.quadSize = p.sizeMin + travelProgress * (0.15f - p.sizeMin);
                p.yPrev = p.y;
                break;
            }
            case K::GeyserPlume: {
                if (!p.done && (p.yd < 0.0 || p.y > p.yEnd || p.y == p.yo)) {
                    p.lifetime = std::min(p.lifetime, p.age + 5);
                    p.friction = 0.0f;
                    p.done = true;
                }
                const double yProgressLinear = std::clamp((p.y - p.yStart) / (p.yEnd - p.yStart), 0.0, 1.0);
                const double yProgressExponential = std::pow(yProgressLinear, 3.0);
                p.gravity = p.propulsion * static_cast<float>(yProgressExponential) * 0.12f;
                p.xd = yProgressLinear * static_cast<double>(p.sprayX);
                p.zd = yProgressLinear * static_cast<double>(p.sprayZ);
                p.quadSize = p.sizeMin + static_cast<float>(yProgressLinear * static_cast<double>(p.sizeMax - p.sizeMin));
                break;
            }
            case K::Crit:
            case K::EnchantedHit:
            case K::DamageIndicator:
                // CritParticle.tick: the spark cools toward red.
                p.gCol *= 0.96f;
                p.bCol *= 0.9f;
                break;
            case K::NoxiousGas:
                if (static_cast<float>(p.age) > p.fadeStart) {
                    const float framesSinceFadeOutStart = static_cast<float>(p.age) - p.fadeStart;
                    p.alpha = (static_cast<float>(p.lifetime) - framesSinceFadeOutStart) / static_cast<float>(p.lifetime);
                }
                break;
            case K::BubbleColumnUp:
                if (!p.removed && !FluidIs(blocks, Containing(p.x, p.y, p.z), Game::FluidType::Water)) remove();
                break;
            case K::EntityEffect:
            case K::WitchMagic:
            case K::Effect:
            case K::InstantEffect:
            case K::Infested:
            case K::RaidOmen:
            case K::TrialOmen:
                // SpellParticle.tick (the spyglass hide is not ported: the
                // engine has no scoping player).
                p.alpha = p.alpha + 0.05f * (p.originalAlpha - p.alpha);
                break;
            case K::EndRod:
            case K::TotemOfUndying:
            case K::SquidInk:
            case K::GlowSquidInk:
            case K::Firework: {
                // SimpleAnimatedParticle.tick.
                if (p.age > p.lifetime / 2) {
                    p.alpha = 1.0f - (static_cast<float>(p.age) - static_cast<float>(p.lifetime / 2)) /
                                     static_cast<float>(p.lifetime);
                    if (p.hasFade) {
                        p.rCol += (p.fadeR - p.rCol) * 0.2f;
                        p.gCol += (p.fadeG - p.gCol) * 0.2f;
                        p.bCol += (p.fadeB - p.bCol) * 0.2f;
                    }
                }
                if ((kind == K::SquidInk || kind == K::GlowSquidInk) && !p.removed) {
                    // SquidInkParticle.tick.
                    if (p.age > p.lifetime / 2) {
                        p.alpha = 1.0f - (static_cast<float>(p.age) - static_cast<float>(p.lifetime / 2)) /
                                         static_cast<float>(p.lifetime);
                    }
                    const glm::ivec3 pos = Containing(p.x, p.y, p.z);
                    if (blocks && blocks->GetBlock(pos.x, pos.y, pos.z) == Game::BlockID::Air) {
                        p.yd -= 0.007400000002235174;
                    }
                }
                if (kind == K::Firework && p.trail && p.age < p.lifetime / 2 && (p.age + p.lifetime) % 2 == 0) {
                    // SparkParticle.tick's trail: a copy at half its life,
                    // added to the engine directly.
                    Client::ClientLevelBridge::QueuedParticle q;
                    q.kind = K::Firework;
                    q.x = p.x; q.y = p.y; q.z = p.z;
                    ChildSpawn c;
                    c.direct = true;
                    if (MakeParticle(c.particle, q, Game::ParticleOptions(K::Firework), nullptr)) {
                        Particle& s = c.particle;
                        s.alpha = 0.99f;
                        s.rCol = p.rCol; s.gCol = p.gCol; s.bCol = p.bCol;
                        s.age = s.lifetime / 2;
                        if (p.hasFade) {
                            s.hasFade = true;
                            s.fadeR = p.fadeR; s.fadeG = p.fadeG; s.fadeB = p.fadeB;
                        }
                        s.twinkle = p.twinkle;
                        spawns.push_back(std::move(c));
                    }
                }
                break;
            }
            case K::Lava:
                // LavaParticle.tick: smoke trails, thinning with age.
                if (!p.removed) {
                    const float odds = static_cast<float>(p.age) / static_cast<float>(p.lifetime);
                    if (m_rng.NextFloat() > odds) spawns.push_back(Child(K::Smoke, p.x, p.y, p.z, p.xd, p.yd, p.zd));
                }
                break;
            case K::Snowflake:
                p.xd *= 0.949999988079071;
                p.yd *= 0.8999999761581421;
                p.zd *= 0.949999988079071;
                break;
            case K::Firefly: {
                // FireflyParticle.tick.
                const glm::ivec3 pos = Containing(p.x, p.y, p.z);
                if (blocks && blocks->GetBlock(pos.x, pos.y, pos.z) != Game::BlockID::Air) {
                    remove();
                    break;
                }
                p.alpha = FadeAmount(std::clamp(static_cast<float>(p.age) / static_cast<float>(p.lifetime), 0.0f, 1.0f),
                                     0.3f, 0.5f);
                if (m_rng.NextFloat() > 0.95f || p.age == 1) {
                    p.xd = static_cast<double>(-0.05f + 0.1f * m_rng.NextFloat());
                    p.yd = static_cast<double>(-0.05f + 0.1f * m_rng.NextFloat());
                    p.zd = static_cast<double>(-0.05f + 0.1f * m_rng.NextFloat());
                }
                break;
            }
            case K::Cloud:
            case K::Sneeze: {
                // PlayerCloudParticle.tick: pulled toward a player within 2
                // blocks that it floats above (the local player's box).
                if (p.removed) break;
                glm::dvec3 mn, mx, delta;
                bool flying = false;
                if (Client::g_clientBlockAccess && Client::g_clientBlockAccess->GetLocalPlayerBox(mn, mx) &&
                    Client::g_clientBlockAccess->GetLocalPlayerMovement(delta, flying)) {
                    const glm::dvec3 feet((mn.x + mx.x) * 0.5, mn.y, (mn.z + mx.z) * 0.5);
                    const glm::dvec3 d = feet - glm::dvec3(p.x, p.y, p.z);
                    if (glm::dot(d, d) < 4.0 && p.y > feet.y) {
                        p.y += (feet.y - p.y) * 0.2;
                        p.yd += (delta.y - p.yd) * 0.2;
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    // ── Render-time rules ──────────────────────────────────────────────────

    int MobParticleSystem::SpriteFor(const Particle& p) const {
        if (p.walk && p.spriteSet >= 0) return m_sprites.Get(p.spriteSet, std::min(p.age, p.lifetime), p.lifetime);
        return p.sprite;
    }

    float MobParticleSystem::QuadSizeFor(const Particle& p, float partialTick) const {
        const float t = (static_cast<float>(p.age) + partialTick) / static_cast<float>(std::max(p.lifetime, 1));
        switch (p.kind) {
            // getQuadSize ramped in over the first 1/32 of the life
            // (HeartParticle, BaseAshSmokeParticle, FallingDustParticle,
            // CritParticle, NoteParticle, DragonBreathParticle,
            // DustParticleBase, PlayerCloudParticle,
            // TrialSpawnerDetectionParticle).
            case K::Heart: case K::AngryVillager:
            case K::Smoke: case K::LargeSmoke: case K::WhiteSmoke: case K::Ash: case K::WhiteAsh:
            case K::DustPlume: case K::NoxiousGas: case K::GeyserBase: case K::GeyserPoof:
            case K::FallingDust:
            case K::Crit: case K::EnchantedHit: case K::DamageIndicator:
            case K::Note: case K::DragonBreath:
            case K::Dust: case K::DustColorTransition:
            case K::Cloud: case K::Sneeze:
            case K::TrialSpawnerDetection: case K::TrialSpawnerDetectionOminous:
                return p.quadSize * std::clamp(t * 32.0f, 0.0f, 1.0f);
            case K::Portal: case K::HushPortal: case K::AetherPortal: {
                // PortalParticle.getQuadSize: 1 - (1 - t)².
                float s = 1.0f - t;
                s *= s;
                return p.quadSize * (1.0f - s);
            }
            case K::ReversePortal:
                return p.quadSize * (1.0f - (static_cast<float>(p.age) + partialTick) /
                                                (static_cast<float>(p.lifetime) * 1.5f));
            case K::Flame: case K::SoulFireFlame: case K::CopperFireFlame: case K::SmallFlame:
                return p.quadSize * (1.0f - t * t * 0.5f);
            case K::Lava:
                return p.quadSize * (1.0f - t * t);
            case K::Shriek:
                return p.quadSize * std::clamp(t * 0.75f, 0.0f, 1.0f);
            case K::Flash:
                return 7.1f * std::sin((static_cast<float>(p.age) + partialTick - 1.0f) * 0.25f * 3.1415927f);
            case K::HushMist:
                return p.quadSize * (1.0f + 0.5f * t);
            default:
                return p.quadSize;
        }
    }

    int MobParticleSystem::LightCoordsFor(const Particle& p, float partialTick) const {
        namespace LC = Game::Lighting::LightCoords;
        if (p.light == Light::FullBright) return LC::kFullBright;
        // Particle.getLightCoords: the cell at the particle's position (15728640
        // — sky 15 — where the chunk is not loaded, which PackedLightAt's
        // missing-level answer already is).
        const int world = EntityEnvironment::PackedLightAt(glm::dvec3(p.x, p.y, p.z));
        // LightCoordsUtil.addSmoothBlockEmission.
        const auto addEmission = [](int coords, float emission) {
            emission = std::clamp(emission, 0.0f, 1.0f);
            const int emitted = static_cast<int>(emission * 240.0f);
            const int block = std::min(LC::SmoothBlock(coords) + emitted, 240);
            return LC::SmoothPack(block, LC::SmoothSky(coords));
        };
        switch (p.light) {
            case Light::World:
                return world;
            case Light::BlockFull:
                return LC::WithBlock(world, 15);
            case Light::AgeEmission:
                return addEmission(world, (static_cast<float>(p.age) + partialTick) / static_cast<float>(std::max(p.lifetime, 1)));
            case Light::AgeEmissionPow4: {
                float b = static_cast<float>(p.age) / static_cast<float>(std::max(p.lifetime, 1));
                b *= b;
                b *= b;
                return addEmission(world, b);
            }
            case Light::Firefly: {
                const float progress = std::clamp((static_cast<float>(p.age) + partialTick) /
                                                      static_cast<float>(std::max(p.lifetime, 1)), 0.0f, 1.0f);
                return static_cast<int>(255.0f * FadeAmount(progress, 0.1f, 0.3f));
            }
            case Light::FullBright:
                break;
        }
        return LC::kFullBright;
    }

    bool MobParticleSystem::TranslucentFor(const Particle& p) const {
        return p.translucent;
    }

} // namespace Render

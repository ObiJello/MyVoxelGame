// File: src/client/world/ClientLevelEvents.cpp
#include "client/world/ClientLevelEvents.hpp"

#include "client/entity/ClientMobManager.hpp"
#include "client/sound/ClientSounds.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityType.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/network/packets/game/LevelParticlePackets.hpp"
#include "common/particle/ParticleOptions.hpp"
#include "common/sound/LevelEventSounds.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/Direction.hpp"
#include "common/world/block/entity/VaultBlockEntity.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/fluid/FluidState.hpp"
#include "common/world/level/DimensionId.hpp"

#include <algorithm>
#include <cmath>

namespace Client::LevelEvents {

    namespace {

        using Game::ParticleKind;
        using Game::ParticleOptions;
        namespace LE = Game::LevelEvent;

        ClientLevelBridge* Sink() {
            return g_clientMobManager ? &g_clientMobManager->Level() : nullptr;
        }

        glm::dvec3 Centre(const glm::ivec3& p) { return glm::dvec3(p) + glm::dvec3(0.5); }

        // Mth.nextDouble(random, min, max): min >= max ? min : nextDouble() * (max - min) + min.
        double NextDouble(Game::JavaRandom& r, double min, double max) {
            return min >= max ? min : r.NextDouble() * (max - min) + min;
        }

        // Mth.randomBetweenInclusive / UniformInt.sample.
        int Uniform(Game::JavaRandom& r, int min, int max) {
            return r.NextInt(max - min + 1) + min;
        }

        void PlayLocal(const glm::ivec3& pos, std::string_view event, Game::SoundSource source,
                       float volume, float pitch, bool distanceDelay = false) {
            Sounds::PlayLocal(Centre(pos), event, source, volume, pitch, distanceDelay);
        }

        // The outline shape's top at the column (x, z) within the cell —
        // VoxelShape.max(Axis.Y, x, z). 0 for an empty shape.
        double ShapeMaxYAt(const Game::IBlockAccess& blocks, const glm::ivec3& pos, Game::BlockState state,
                           double fx, double fz) {
            double top = 0.0;
            bool any = false;
            for (const auto& box : Game::BlockRegistry::GetBlockShapeSetAt(blocks, pos, state)) {
                if (fx < box.min.x || fx > box.max.x || fz < box.min.z || fz > box.max.z) continue;
                top = any ? std::max(top, static_cast<double>(box.max.y)) : static_cast<double>(box.max.y);
                any = true;
            }
            return any ? top : 0.0;
        }

        double ShapeMaxY(const Game::IBlockAccess& blocks, const glm::ivec3& pos, Game::BlockState state) {
            double top = 0.0;
            for (const auto& box : Game::BlockRegistry::GetBlockShapeSetAt(blocks, pos, state)) {
                top = std::max(top, static_cast<double>(box.max.y));
            }
            return top;
        }

        // ── ParticleUtils ─────────────────────────────────────────────────

        // ParticleUtils.spawnParticleOnFace.
        void SpawnParticleOnFace(ClientLevelBridge& level, const glm::ivec3& pos, Game::Direction face,
                                 const ParticleOptions& particle, const glm::dvec3& speed, double stepFactor) {
            Game::JavaRandom& random = level.Random();
            const glm::dvec3 c = Centre(pos);
            const int sx = Game::StepX(face), sy = Game::StepY(face), sz = Game::StepZ(face);
            const double x = c.x + (sx == 0 ? NextDouble(random, -0.5, 0.5) : static_cast<double>(sx) * stepFactor);
            const double y = c.y + (sy == 0 ? NextDouble(random, -0.5, 0.5) : static_cast<double>(sy) * stepFactor);
            const double z = c.z + (sz == 0 ? NextDouble(random, -0.5, 0.5) : static_cast<double>(sz) * stepFactor);
            level.AddParticle(particle, x, y, z,
                              sx == 0 ? speed.x : 0.0, sy == 0 ? speed.y : 0.0, sz == 0 ? speed.z : 0.0);
        }

        // ParticleUtils.spawnParticlesOnBlockFaces: every face, a
        // UniformInt(min, max) count each, speed in ±0.5.
        void SpawnParticlesOnBlockFaces(ClientLevelBridge& level, const glm::ivec3& pos,
                                        const ParticleOptions& particle, int minCount, int maxCount) {
            Game::JavaRandom& random = level.Random();
            for (int d = 0; d < 6; ++d) {
                const Game::Direction face = static_cast<Game::Direction>(d);
                const int count = Uniform(random, minCount, maxCount);
                for (int i = 0; i < count; ++i) {
                    const glm::dvec3 speed(NextDouble(random, -0.5, 0.5), NextDouble(random, -0.5, 0.5),
                                           NextDouble(random, -0.5, 0.5));
                    SpawnParticleOnFace(level, pos, face, particle, speed, 0.55);
                }
            }
        }

        // ParticleUtils.spawnParticlesAlongAxis.
        void SpawnParticlesAlongAxis(ClientLevelBridge& level, Game::Axis axis, const glm::ivec3& pos,
                                     double radius, const ParticleOptions& particle, int minCount, int maxCount) {
            Game::JavaRandom& random = level.Random();
            const glm::dvec3 c = Centre(pos);
            const bool stepX = axis == Game::Axis::X, stepY = axis == Game::Axis::Y, stepZ = axis == Game::Axis::Z;
            const int count = Uniform(random, minCount, maxCount);
            for (int i = 0; i < count; ++i) {
                const double x = c.x + NextDouble(random, -1.0, 1.0) * (stepX ? 0.5 : radius);
                const double y = c.y + NextDouble(random, -1.0, 1.0) * (stepY ? 0.5 : radius);
                const double z = c.z + NextDouble(random, -1.0, 1.0) * (stepZ ? 0.5 : radius);
                const double xs = stepX ? NextDouble(random, -1.0, 1.0) : 0.0;
                const double ys = stepY ? NextDouble(random, -1.0, 1.0) : 0.0;
                const double zs = stepZ ? NextDouble(random, -1.0, 1.0) : 0.0;
                level.AddParticle(particle, x, y, z, xs, ys, zs);
            }
        }

        // ParticleUtils.spawnParticles.
        void SpawnParticles(ClientLevelBridge& level, const glm::ivec3& pos, int count, double spreadWidth,
                            double spreadHeight, bool allowFloatingParticles, const ParticleOptions& particle) {
            Game::JavaRandom& random = level.Random();
            const Game::IBlockAccess* blocks = level.Blocks();
            for (int i = 0; i < count; ++i) {
                const double xv = random.NextGaussian() * 0.02;
                const double yv = random.NextGaussian() * 0.02;
                const double zv = random.NextGaussian() * 0.02;
                const double start = 0.5 - spreadWidth;
                const double x = static_cast<double>(pos.x) + start + random.NextDouble() * spreadWidth * 2.0;
                const double y = static_cast<double>(pos.y) + random.NextDouble() * spreadHeight;
                const double z = static_cast<double>(pos.z) + start + random.NextDouble() * spreadWidth * 2.0;
                if (!allowFloatingParticles) {
                    // !getBlockState(BlockPos.containing(x, y, z).below()).isAir()
                    if (!blocks) continue;
                    const int bx = static_cast<int>(std::floor(x));
                    const int by = static_cast<int>(std::floor(y)) - 1;
                    const int bz = static_cast<int>(std::floor(z));
                    if (blocks->GetBlock(bx, by, bz) == Game::BlockID::Air) continue;
                }
                level.AddParticle(particle, x, y, z, xv, yv, zv);
            }
        }

        // ParticleUtils.spawnParticleInBlock.
        void SpawnParticleInBlock(ClientLevelBridge& level, const glm::ivec3& pos, int count,
                                  const ParticleOptions& particle) {
            const Game::IBlockAccess* blocks = level.Blocks();
            double spreadHeight = 1.0;
            if (blocks) {
                const Game::BlockState state = blocks->GetBlockState(pos.x, pos.y, pos.z);
                if (state.Block() != Game::BlockID::Air) spreadHeight = ShapeMaxY(*blocks, pos, state);
            }
            SpawnParticles(level, pos, count, 0.5, spreadHeight, true, particle);
        }

        // ParticleUtils.spawnSmashAttackParticles (the mace's landing).
        void SpawnSmashAttackParticles(ClientLevelBridge& level, const glm::ivec3& pos, int count) {
            const Game::IBlockAccess* blocks = level.Blocks();
            if (!blocks) return;
            Game::JavaRandom& random = level.Random();
            const glm::dvec3 center = Centre(pos) + glm::dvec3(0.0, 0.5, 0.0);
            const ParticleOptions particle =
                ParticleOptions::Block(ParticleKind::DustPillar, blocks->GetBlockState(pos.x, pos.y, pos.z));
            for (int i = 0; static_cast<float>(i) < static_cast<float>(count) / 3.0f; ++i) {
                const double x = center.x + random.NextGaussian() / 2.0;
                const double y = center.y;
                const double z = center.z + random.NextGaussian() / 2.0;
                const double xd = random.NextGaussian() * 0.20000000298023224;
                const double yd = random.NextGaussian() * 0.20000000298023224;
                const double zd = random.NextGaussian() * 0.20000000298023224;
                level.AddParticle(particle, x, y, z, xd, yd, zd);
            }
            for (int i = 0; static_cast<float>(i) < static_cast<float>(count) / 1.5f; ++i) {
                const double x = center.x + 3.5 * std::cos(static_cast<double>(i)) + random.NextGaussian() / 2.0;
                const double y = center.y;
                const double z = center.z + 3.5 * std::sin(static_cast<double>(i)) + random.NextGaussian() / 2.0;
                const double xd = random.NextGaussian() * 0.05000000074505806;
                const double yd = random.NextGaussian() * 0.05000000074505806;
                const double zd = random.NextGaussian() * 0.05000000074505806;
                level.AddParticle(particle, x, y, z, xd, yd, zd);
            }
        }

        // ── The per-event helpers ─────────────────────────────────────────

        // BoneMealItem.addGrowthParticles.
        void AddGrowthParticles(ClientLevelBridge& level, const glm::ivec3& pos, int count) {
            const Game::IBlockAccess* blocks = level.Blocks();
            if (!blocks) return;
            const Game::BlockState state = blocks->GetBlockState(pos.x, pos.y, pos.z);
            const Game::BlockID id = state.Block();
            const Game::Block& def = Game::BlockRegistry::Get(id);
            if (def.performBonemeal) {
                // BonemealableBlock.getParticlePos: below for the mangrove
                // leaves (the propagule hangs under them) and rooted dirt
                // (the hanging roots).
                // BonemealableBlock.getType: NEIGHBOR_SPREADER for the blocks
                // that spread onto their neighbours (GrassBlock, NyliumBlock,
                // NetherrackBlock, BonemealableFeaturePlacerBlock — the moss
                // blocks); GROWER for everything else.
                const bool spreader = id == Game::BlockID::Grass || id == Game::BlockID::CrimsonNylium ||
                                      id == Game::BlockID::WarpedNylium || id == Game::BlockID::Netherrack ||
                                      id == Game::BlockID::MossBlock || id == Game::BlockID::PaleMossBlock;
                // BonemealableBlock.getParticlePos: the default is the cell
                // ABOVE a spreader (the plants come up on top of it) and the
                // block's own cell for a grower; the mangrove leaves and the
                // rooted dirt override it with the cell below.
                glm::ivec3 particlePos = spreader ? pos + glm::ivec3(0, 1, 0) : pos;
                if (id == Game::BlockID::MangroveLeaves || id == Game::BlockID::RootedDirt) {
                    particlePos = pos - glm::ivec3(0, 1, 0);
                }
                if (spreader) {
                    SpawnParticles(level, particlePos, count * 3, 3.0, 1.0, false, ParticleKind::HappyVillager);
                } else {
                    SpawnParticleInBlock(level, particlePos, count, ParticleKind::HappyVillager);
                }
            } else if (id == Game::BlockID::Water) {
                SpawnParticles(level, pos, count * 3, 3.0, 1.0, false, ParticleKind::HappyVillager);
            }
        }

        // ComposterBlock.handleFill.
        void ComposterFill(ClientLevelBridge& level, const glm::ivec3& pos, bool success) {
            const Game::IBlockAccess* blocks = level.Blocks();
            if (!blocks) return;
            const Game::BlockState state = blocks->GetBlockState(pos.x, pos.y, pos.z);
            PlayLocal(pos, success ? Game::SoundEvents::COMPOSTER_FILL_SUCCESS : Game::SoundEvents::COMPOSTER_FILL,
                      Game::SoundSource::Blocks, 1.0f, 1.0f);
            const double centerHeight = ShapeMaxYAt(*blocks, pos, state, 0.5, 0.5) + 0.03125;
            Game::JavaRandom& random = level.Random();
            for (int i = 0; i < 10; ++i) {
                const double xa = random.NextGaussian() * 0.02;
                const double ya = random.NextGaussian() * 0.02;
                const double za = random.NextGaussian() * 0.02;
                const double x = static_cast<double>(pos.x) + 0.1875 + 0.625 * static_cast<double>(random.NextFloat());
                const double y = static_cast<double>(pos.y) + centerHeight +
                                 static_cast<double>(random.NextFloat()) * (1.0 - centerHeight);
                const double z = static_cast<double>(pos.z) + 0.1875 + 0.625 * static_cast<double>(random.NextFloat());
                level.AddParticle(ParticleOptions(ParticleKind::Composter), x, y, z, xa, ya, za);
            }
        }

        // PointedDripstoneBlock.spawnDripParticle(level, tipPos, tipState):
        // the drip under a stalactite whose root sits under a fluid (or mud).
        void DripstoneDrip(ClientLevelBridge& level, const glm::ivec3& tip) {
            const Game::IBlockAccess* blocks = level.Blocks();
            if (!blocks) return;
            const Game::BlockState tipState = blocks->GetBlockState(tip.x, tip.y, tip.z);
            if (!tipState.Is(Game::BlockID::PointedDripstone) ||
                tipState.GetValueByName("vertical_direction") != "down") {
                return;
            }
            // findRootBlock: up through the same stalactite (max 11 steps).
            glm::ivec3 root = tip;
            bool found = false;
            for (int i = 1; i < 11; ++i) {
                const glm::ivec3 p = tip + glm::ivec3(0, i, 0);
                const Game::BlockState s = blocks->GetBlockState(p.x, p.y, p.z);
                if (!s.Is(Game::BlockID::PointedDripstone)) { root = p - glm::ivec3(0, 1, 0); found = true; break; }
                if (s.GetValueByName("vertical_direction") != "down") break;
            }
            if (!found) return;
            const glm::ivec3 above = root + glm::ivec3(0, 1, 0);
            const Game::BlockState aboveState = blocks->GetBlockState(above.x, above.y, above.z);
            Game::FluidType fluid;
            // WATER_EVAPORATES is the nether's dimension attribute.
            const bool evaporates = level.Dimension() == Game::DimensionId::Nether;
            if (aboveState.Is(Game::BlockID::Mud) && !evaporates) fluid = Game::FluidType::Water;
            else fluid = Game::GetFluidState(*blocks, above).type;
            // getDripParticle: DEFAULT_DRIPSTONE_PARTICLE (the nether drips
            // lava, everywhere else water) with nothing above.
            ParticleKind kind;
            if (fluid == Game::FluidType::Empty) {
                kind = evaporates ? ParticleKind::DrippingDripstoneLava : ParticleKind::DrippingDripstoneWater;
            } else {
                kind = fluid == Game::FluidType::Lava ? ParticleKind::DrippingDripstoneLava
                                                      : ParticleKind::DrippingDripstoneWater;
            }
            const glm::vec3 offset = Game::BlockRegistry::GetBlockOffset(Game::BlockID::PointedDripstone, tip.x, tip.z);
            // STALACTITE_DRIP_START_PIXEL = SHAPE_TIP_DOWN.min(Y) = 5/16.
            level.AddParticle(ParticleOptions(kind),
                              static_cast<double>(tip.x) + 0.5 + static_cast<double>(offset.x),
                              static_cast<double>(tip.y) + 0.3125 - 0.0625,
                              static_cast<double>(tip.z) + 0.5 + static_cast<double>(offset.z),
                              0.0, 0.0, 0.0);
        }

        // LevelEventHandler.shootParticles (dispenser smoke 2000, crafter
        // white smoke 2010): ten puffs out of the front face.
        void ShootParticles(ClientLevelBridge& level, int data, const glm::ivec3& pos, ParticleKind particle) {
            Game::JavaRandom& random = level.Random();
            // Direction.from3DDataValue: abs(data % 6).
            const Game::Direction d = static_cast<Game::Direction>(std::abs(data % 6));
            const int nx = Game::StepX(d), ny = Game::StepY(d), nz = Game::StepZ(d);
            for (int i = 0; i < 10; ++i) {
                const double pow = random.NextDouble() * 0.2 + 0.01;
                const double x = pos.x + nx * 0.6 + 0.5 + nx * 0.01 + (random.NextDouble() - 0.5) * nz * 0.5;
                const double y = pos.y + ny * 0.6 + 0.5 + ny * 0.01 + (random.NextDouble() - 0.5) * ny * 0.5;
                const double z = pos.z + nz * 0.6 + 0.5 + nz * 0.01 + (random.NextDouble() - 0.5) * nx * 0.5;
                const double vx = nx * pow + random.NextGaussian() * 0.01;
                const double vy = ny * pow + random.NextGaussian() * 0.01;
                const double vz = nz * pow + random.NextGaussian() * 0.01;
                level.AddParticle(ParticleOptions(particle), x, y, z, vx, vy, vz);
            }
        }

        // LevelEventHandler.potionSplashParticles (2002 / 2007).
        void PotionSplash(ClientLevelBridge& level, ParticleKind type, const glm::ivec3& pos, int data) {
            Game::JavaRandom& random = level.Random();
            const glm::dvec3 at(pos.x + 0.5, pos.y, pos.z + 0.5);   // Vec3.atBottomCenterOf
            const ParticleOptions glass = ParticleOptions::Item(Game::Items::SplashPotion);
            for (int i = 0; i < 8; ++i) {
                level.AddParticle(glass, at.x, at.y, at.z, random.NextGaussian() * 0.15,
                                  random.NextDouble() * 0.2, random.NextGaussian() * 0.15);
            }
            const float red = static_cast<float>((data >> 16) & 255) / 255.0f;
            const float green = static_cast<float>((data >> 8) & 255) / 255.0f;
            const float blue = static_cast<float>(data & 255) / 255.0f;
            for (int i = 0; i < 100; ++i) {
                const double dist = random.NextDouble() * 4.0;
                const double angle = random.NextDouble() * 3.141592653589793 * 2.0;
                const double vx = std::cos(angle) * dist;
                const double vy = 0.01 + random.NextDouble() * 0.5;
                const double vz = std::sin(angle) * dist;
                const float brightness = 0.75f + random.NextFloat() * 0.25f;
                const ParticleOptions particle = ParticleOptions::Spell(type, red * brightness, green * brightness,
                                                                        blue * brightness, static_cast<float>(dist));
                level.AddParticle(particle, at.x + vx * 0.1, at.y + 0.3, at.z + vz * 0.1, vx, vy, vz);
            }
        }

        // LevelEventHandler.singleBlockTeleportationParticles.
        void TeleportParticles(ClientLevelBridge& level, const glm::ivec3& pos, const glm::ivec3& to) {
            Game::JavaRandom& random = level.Random();
            for (int i = 0; i < 128; ++i) {
                const double t = random.NextDouble();
                const float vx = (random.NextFloat() - 0.5f) * 0.2f;
                const float vy = (random.NextFloat() - 0.5f) * 0.2f;
                const float vz = (random.NextFloat() - 0.5f) * 0.2f;
                const double x = to.x + (pos.x - to.x) * t + (random.NextDouble() - 0.5) + 0.5;
                const double y = to.y + (pos.y - to.y) * t + random.NextDouble() - 0.5;
                const double z = to.z + (pos.z - to.z) * t + (random.NextDouble() - 0.5) + 0.5;
                level.AddParticle(ParticleOptions(ParticleKind::Portal), x, y, z, vx, vy, vz);
            }
        }

        // BlockUtil.unpackDifferenceInPosition(pos, data, xMax, yMax, zMax):
        // three signed offsets packed into one int by (2·max + 1) radices.
        // MC BlockUtil.unpackDifferenceInPosition: one byte per axis,
        // x << 16 | y << 8 | z, each offset by its radius.
        glm::ivec3 UnpackDifference(const glm::ivec3& pos, int data, int xMax, int yMax, int zMax) {
            const int dx = ((data >> 16) & 255) - xMax;
            const int dy = ((data >> 8) & 255) - yMax;
            const int dz = (data & 255) - zMax;
            return pos + glm::ivec3(dx, dy, dz);
        }

        // TrialSpawner.addSpawnParticles.
        void TrialSpawnParticles(ClientLevelBridge& level, const glm::ivec3& pos, ParticleKind flame) {
            Game::JavaRandom& random = level.Random();
            for (int i = 0; i < 20; ++i) {
                const double x = pos.x + 0.5 + (random.NextDouble() - 0.5) * 2.0;
                const double y = pos.y + 0.5 + (random.NextDouble() - 0.5) * 2.0;
                const double z = pos.z + 0.5 + (random.NextDouble() - 0.5) * 2.0;
                level.AddParticle(ParticleOptions(ParticleKind::Smoke), x, y, z, 0.0, 0.0, 0.0);
                level.AddParticle(ParticleOptions(flame), x, y, z, 0.0, 0.0, 0.0);
            }
        }

        // TrialSpawner.addDetectPlayerParticles.
        void TrialDetectParticles(ClientLevelBridge& level, const glm::ivec3& pos, int data, ParticleKind type) {
            Game::JavaRandom& random = level.Random();
            const int count = 30 + std::min(data, 10) * 5;
            for (int i = 0; i < count; ++i) {
                const double sx = static_cast<double>(2.0f * random.NextFloat() - 1.0f) * 0.65;
                const double sz = static_cast<double>(2.0f * random.NextFloat() - 1.0f) * 0.65;
                const double x = pos.x + 0.5 + sx;
                const double y = pos.y + 0.1 + static_cast<double>(random.NextFloat()) * 0.8;
                const double z = pos.z + 0.5 + sz;
                level.AddParticle(ParticleOptions(type), x, y, z, 0.0, 0.0, 0.0);
            }
        }

        // TrialSpawner.addEjectItemParticles.
        void TrialEjectParticles(ClientLevelBridge& level, const glm::ivec3& pos) {
            Game::JavaRandom& random = level.Random();
            for (int i = 0; i < 20; ++i) {
                const double x = pos.x + 0.4 + random.NextDouble() * 0.2;
                const double y = pos.y + 0.4 + random.NextDouble() * 0.2;
                const double z = pos.z + 0.4 + random.NextDouble() * 0.2;
                const double xa = random.NextGaussian() * 0.02;
                const double ya = random.NextGaussian() * 0.02;
                const double za = random.NextGaussian() * 0.02;
                level.AddParticle(ParticleOptions(ParticleKind::SmallFlame), x, y, z, xa, ya, za * 0.25);
                level.AddParticle(ParticleOptions(ParticleKind::Smoke), x, y, z, xa, ya, za);
            }
        }

        // TrialSpawner.addBecomeOminousParticles.
        void TrialBecomeOminousParticles(ClientLevelBridge& level, const glm::ivec3& pos) {
            Game::JavaRandom& random = level.Random();
            for (int i = 0; i < 20; ++i) {
                const double x = pos.x + 0.5 + (random.NextDouble() - 0.5) * 2.0;
                const double y = pos.y + 0.5 + (random.NextDouble() - 0.5) * 2.0;
                const double z = pos.z + 0.5 + (random.NextDouble() - 0.5) * 2.0;
                const double xa = random.NextGaussian() * 0.02;
                const double ya = random.NextGaussian() * 0.02;
                const double za = random.NextGaussian() * 0.02;
                level.AddParticle(ParticleOptions(ParticleKind::TrialOmen), x, y, z, xa, ya, za);
                level.AddParticle(ParticleOptions(ParticleKind::SoulFireFlame), x, y, z, xa, ya, za);
            }
        }

        // TrialSpawner.FlameParticle.decode (MC's `data <= values.length`
        // off-by-one is clamped rather than reproduced as a crash).
        ParticleKind TrialFlame(int data) {
            return data == 1 ? ParticleKind::SoulFireFlame : ParticleKind::Flame;
        }

        // VaultBlockEntity.Client.emitActivationParticles's cage burst (the
        // connection motes need the vault's shared data, which a level event
        // does not carry).
        void VaultActivation(ClientLevelBridge& level, const glm::ivec3& pos, ParticleKind flame) {
            Game::JavaRandom& random = level.Random();
            for (int i = 0; i < 20; ++i) {
                const double x = pos.x + NextDouble(random, 0.1, 0.9);
                const double y = pos.y + NextDouble(random, 0.25, 0.75);
                const double z = pos.z + NextDouble(random, 0.1, 0.9);
                level.AddParticle(ParticleOptions(ParticleKind::Smoke), x, y, z, 0.0, 0.0, 0.0);
                level.AddParticle(ParticleOptions(flame), x, y, z, 0.0, 0.0, 0.0);
            }
        }

        // VaultBlockEntity.Client.emitDeactivationParticles.
        void VaultDeactivation(ClientLevelBridge& level, const glm::ivec3& pos, ParticleKind flame) {
            Game::JavaRandom& random = level.Random();
            for (int i = 0; i < 20; ++i) {
                const double x = pos.x + NextDouble(random, 0.4, 0.6);
                const double y = pos.y + NextDouble(random, 0.4, 0.6);
                const double z = pos.z + NextDouble(random, 0.4, 0.6);
                const double dx = random.NextGaussian() * 0.02;
                const double dy = random.NextGaussian() * 0.02;
                const double dz = random.NextGaussian() * 0.02;
                level.AddParticle(ParticleOptions(flame), x, y, z, dx, dy, dz);
            }
        }

        // LevelEventHandler 3006: a sculk charge spreading (data = charge
        // << 6 | the faces of a sculk vein it sits on).
        void SculkCharge(ClientLevelBridge& level, const glm::ivec3& pos, int data) {
            Game::JavaRandom& random = level.Random();
            const Game::IBlockAccess* blocks = level.Blocks();
            const int count = data >> 6;
            if (count > 0) {
                if (random.NextFloat() < 0.3f + static_cast<float>(count) * 0.1f) {
                    const float volume = 0.15f + 0.02f * static_cast<float>(count) * static_cast<float>(count) * random.NextFloat();
                    const float pitch = 0.4f + 0.3f * static_cast<float>(count) * random.NextFloat();
                    PlayLocal(pos, Game::SoundEvents::SCULK_BLOCK_CHARGE, Game::SoundSource::Blocks, volume, pitch);
                }
                const uint8_t faces = static_cast<uint8_t>(data & 63);
                const auto speed = [&random]() {
                    return glm::dvec3(NextDouble(random, -0.004999999888241291, 0.004999999888241291),
                                      NextDouble(random, -0.004999999888241291, 0.004999999888241291),
                                      NextDouble(random, -0.004999999888241291, 0.004999999888241291));
                };
                for (int d = 0; d < 6; ++d) {
                    const Game::Direction dir = static_cast<Game::Direction>(d);
                    float roll;
                    double factor;
                    if (faces == 0) {
                        roll = dir == Game::Direction::Down ? 3.1415927f : 0.0f;
                        factor = Game::AxisOf(dir) == Game::Axis::Y ? 0.65 : 0.57;
                    } else {
                        if ((faces & static_cast<uint8_t>(1u << d)) == 0) continue;
                        roll = dir == Game::Direction::Up ? 3.1415927f : 0.0f;
                        factor = 0.35;
                    }
                    // spawnParticlesOnBlockFace with UniformInt(0, count).
                    const int n = Uniform(random, 0, count);
                    for (int i = 0; i < n; ++i) {
                        SpawnParticleOnFace(level, pos, dir, ParticleOptions::SculkCharge(roll), speed(), factor);
                    }
                }
                return;
            }
            PlayLocal(pos, Game::SoundEvents::SCULK_BLOCK_CHARGE, Game::SoundSource::Blocks, 1.0f, 1.0f);
            // isCollisionShapeFullBlock (the variable MC names isWaterlogged).
            bool fullBlock = false;
            if (blocks) {
                const Game::BlockState state = blocks->GetBlockState(pos.x, pos.y, pos.z);
                fullBlock = Game::BlockRegistry::GetBlockCollisionShapeSet(state).IsFullCube();
            }
            const int particleCount = fullBlock ? 40 : 20;
            const float spread = fullBlock ? 0.45f : 0.25f;
            for (int i = 0; i < particleCount; ++i) {
                const float vx = 2.0f * random.NextFloat() - 1.0f;
                const float vy = 2.0f * random.NextFloat() - 1.0f;
                const float vz = 2.0f * random.NextFloat() - 1.0f;
                level.AddParticle(ParticleOptions(ParticleKind::SculkChargePop),
                                  pos.x + 0.5 + static_cast<double>(vx * spread),
                                  pos.y + 0.5 + static_cast<double>(vy * spread),
                                  pos.z + 0.5 + static_cast<double>(vz * spread),
                                  static_cast<double>(vx * 0.07f), static_cast<double>(vy * 0.07f),
                                  static_cast<double>(vz * 0.07f));
            }
        }

    } // namespace

    // ── ClientLevel.addDestroyBlockEffect ─────────────────────────────────

    void AddDestroyBlockEffect(const glm::ivec3& pos, Game::BlockState state) {
        ClientLevelBridge* level = Sink();
        if (!level) return;
        const Game::BlockID id = state.Block();
        // !isAir && shouldSpawnTerrainParticles (noTerrainParticles: barrier,
        // structure void; a moving piston never has a sprite of its own).
        if (id == Game::BlockID::Air || id == Game::BlockID::Barrier || id == Game::BlockID::StructureVoid ||
            id == Game::BlockID::MovingPiston) {
            return;
        }
        const Game::IBlockAccess* blocks = level->Blocks();
        const auto shapes = blocks ? Game::BlockRegistry::GetBlockShapeSetAt(*blocks, pos, state)
                                   : Game::BlockRegistry::GetBlockShapeSet(state);
        const ParticleOptions particle = ParticleOptions::Block(state);
        // shape.forAllBoxes, density 0.25: 2..4 particles per box axis, on a
        // regular grid, each flung out from the box centre.
        for (const auto& box : shapes) {
            const double x1 = box.min.x, y1 = box.min.y, z1 = box.min.z;
            const double widthX = std::min(1.0, static_cast<double>(box.max.x) - x1);
            const double widthY = std::min(1.0, static_cast<double>(box.max.y) - y1);
            const double widthZ = std::min(1.0, static_cast<double>(box.max.z) - z1);
            const int countX = std::max(2, static_cast<int>(std::ceil(widthX / 0.25)));
            const int countY = std::max(2, static_cast<int>(std::ceil(widthY / 0.25)));
            const int countZ = std::max(2, static_cast<int>(std::ceil(widthZ / 0.25)));
            for (int xx = 0; xx < countX; ++xx) {
                for (int yy = 0; yy < countY; ++yy) {
                    for (int zz = 0; zz < countZ; ++zz) {
                        const double relX = (xx + 0.5) / countX;
                        const double relY = (yy + 0.5) / countY;
                        const double relZ = (zz + 0.5) / countZ;
                        const double x = relX * widthX + x1;
                        const double y = relY * widthY + y1;
                        const double z = relZ * widthZ + z1;
                        // particleEngine.add(new TerrainParticle(...)): straight
                        // into the engine, no limiter.
                        ClientLevelBridge::QueuedParticle q;
                        q.kind = ParticleKind::Block;
                        q.x = pos.x + x; q.y = pos.y + y; q.z = pos.z + z;
                        q.vx = relX - 0.5; q.vy = relY - 0.5; q.vz = relZ - 0.5;
                        q.blockState = particle.blockState;
                        q.hasOptions = true;
                        q.overrideLimiter = true;
                        q.options = particle;
                        q.hasBlockPos = true;   // TerrainParticle(..., blockState, pos)
                        q.blockPos = pos;
                        level->QueueParticle(std::move(q));
                    }
                }
            }
        }
    }

    // ── ClientLevel.addBreakingBlockEffects ───────────────────────────────

    void AddBreakingBlockEffects(const glm::ivec3& pos, int face, bool playSound) {
        ClientLevelBridge* level = Sink();
        if (!level) return;
        const Game::IBlockAccess* blocks = level->Blocks();
        if (!blocks) return;
        const Game::BlockState state = blocks->GetBlockState(pos.x, pos.y, pos.z);
        const Game::BlockID id = state.Block();
        if (id == Game::BlockID::Air) return;
        if (playSound) {
            // ClientLevel.playBreakingSound: the hit sound, (volume + 1) / 8,
            // pitch * 0.5.
            const Game::SoundType& type = Game::SoundTypeOf(state);
            if (!Game::IsEmptySound(type.hitSound)) {
                Sounds::PlayLocal(Centre(pos), type.hitSound, Game::SoundSource::Blocks,
                                  (type.volume + 1.0f) / 8.0f, type.pitch * 0.5f);
            }
        }
        // getRenderShape() != INVISIBLE && shouldSpawnTerrainParticles.
        if (id == Game::BlockID::Barrier || id == Game::BlockID::StructureVoid || id == Game::BlockID::MovingPiston ||
            id == Game::BlockID::Light) {
            return;
        }
        const auto shapes = Game::BlockRegistry::GetBlockShapeSetAt(*blocks, pos, state);
        if (shapes.count == 0) return;
        // shape.bounds()
        glm::dvec3 mn(1.0), mx(0.0);
        for (const auto& b : shapes) {
            mn = glm::min(mn, glm::dvec3(b.min));
            mx = glm::max(mx, glm::dvec3(b.max));
        }
        Game::JavaRandom& random = level->Random();
        double xp = pos.x + random.NextDouble() * (mx.x - mn.x - 0.20000000298023224) + 0.10000000149011612 + mn.x;
        double yp = pos.y + random.NextDouble() * (mx.y - mn.y - 0.20000000298023224) + 0.10000000149011612 + mn.y;
        double zp = pos.z + random.NextDouble() * (mx.z - mn.z - 0.20000000298023224) + 0.10000000149011612 + mn.z;
        switch (static_cast<Game::Direction>(std::clamp(face, 0, 5))) {
            case Game::Direction::Down:  yp = pos.y + mn.y - 0.10000000149011612; break;
            case Game::Direction::Up:    yp = pos.y + mx.y + 0.10000000149011612; break;
            case Game::Direction::North: zp = pos.z + mn.z - 0.10000000149011612; break;
            case Game::Direction::South: zp = pos.z + mx.z + 0.10000000149011612; break;
            case Game::Direction::West:  xp = pos.x + mn.x - 0.10000000149011612; break;
            case Game::Direction::East:  xp = pos.x + mx.x + 0.10000000149011612; break;
        }
        // particleEngine.add(new TerrainParticle(this, xp, yp, zp, 0, 0, 0,
        // state, pos).setPower(0.2F).scale(0.6F)).
        ClientLevelBridge::QueuedParticle q;
        q.kind = ParticleKind::Block;
        q.x = xp; q.y = yp; q.z = zp;
        q.blockState = state.RawId();
        q.hasOptions = true;
        q.overrideLimiter = true;
        q.options = ParticleOptions::Block(state);
        q.hasBlockPos = true;   // TerrainParticle(..., state, pos)
        q.blockPos = pos;
        q.power = 0.2f;
        q.scale = 0.6f;
        level->QueueParticle(std::move(q));
    }

    // ── ClientPacketListener.handleParticleEvent ──────────────────────────

    void LevelParticles(const Network::LevelParticlesS2CPacket& packet) {
        ClientLevelBridge* level = Sink();
        if (!level) return;
        Game::JavaRandom& random = level->Random();
        const Game::ParticleOptions& options = packet.options;
        const bool override = packet.overrideLimiter;
        const bool always = packet.alwaysShow;
        if (packet.count == 0) {
            const double xa = static_cast<double>(packet.maxSpeed.x * packet.dist.x);
            const double ya = static_cast<double>(packet.maxSpeed.y * packet.dist.y);
            const double za = static_cast<double>(packet.maxSpeed.z * packet.dist.z);
            level->AddParticle(options, override, always, packet.pos.x, packet.pos.y, packet.pos.z, xa, ya, za);
            return;
        }
        using R = Game::Particles::Randomization;
        if (packet.randomization == R::EffectCloud) {
            // MC AreaEffectCloud.clientTick, for the cloud at packet.pos.
            const bool waiting = packet.dist.y > 0.0f;
            const float radius = packet.dist.x;
            if (waiting && random.NextBool()) return;
            const int particleCount = waiting ? 2 : static_cast<int>(std::ceil(3.1415927f * radius * radius));
            const float particleRadius = waiting ? 0.2f : radius;
            const bool entityEffect = options.kind == ParticleKind::EntityEffect;
            for (int i = 0; i < std::min(particleCount, 16384); ++i) {
                const float angle = random.NextFloat() * 6.2831855f;
                const float distance = std::sqrt(random.NextFloat()) * particleRadius;
                const double x = packet.pos.x + static_cast<double>(std::cos(angle) * distance);
                const double y = packet.pos.y;
                const double z = packet.pos.z + static_cast<double>(std::sin(angle) * distance);
                if (entityEffect) {
                    if (waiting && random.NextBool()) {
                        // DEFAULT_PARTICLE: ENTITY_EFFECT in opaque white.
                        level->AddAlwaysVisibleParticle(ParticleOptions::Color(ParticleKind::EntityEffect, 0xFFFFFFFFu),
                                                        x, y, z, 0.0, 0.0, 0.0);
                    } else {
                        level->AddAlwaysVisibleParticle(options, x, y, z, 0.0, 0.0, 0.0);
                    }
                } else if (waiting) {
                    level->AddAlwaysVisibleParticle(options, x, y, z, 0.0, 0.0, 0.0);
                } else {
                    const double xa = (0.5 - random.NextDouble()) * 0.15;
                    const double za = (0.5 - random.NextDouble()) * 0.15;
                    level->AddAlwaysVisibleParticle(options, x, y, z, xa, 0.009999999776482582, za);
                }
            }
            return;
        }
        // A burst cannot outgrow the engine's own cap in one packet.
        const int count = std::min(packet.count, 16384);
        if (packet.randomization != R::Default) {
            for (int i = 0; i < count; ++i) {
                const double xv = random.NextDouble() * packet.dist.x;
                const double yv = random.NextDouble() * packet.dist.y;
                const double zv = random.NextDouble() * packet.dist.z;
                double xa = packet.maxSpeed.x, ya = packet.maxSpeed.y, za = packet.maxSpeed.z;
                if (packet.randomization == R::AlternativeWithSpeed) {
                    xa *= random.NextDouble();
                    ya *= random.NextDouble();
                    za *= random.NextDouble();
                }
                level->AddParticle(options, override, always, packet.pos.x + xv, packet.pos.y + yv,
                                   packet.pos.z + zv, xa, ya, za);
            }
            return;
        }
        for (int i = 0; i < count; ++i) {
            const double xv = random.NextGaussian() * packet.dist.x;
            const double yv = random.NextGaussian() * packet.dist.y;
            const double zv = random.NextGaussian() * packet.dist.z;
            const double xa = random.NextGaussian() * packet.maxSpeed.x;
            const double ya = random.NextGaussian() * packet.maxSpeed.y;
            const double za = random.NextGaussian() * packet.maxSpeed.z;
            level->AddParticle(options, override, always, packet.pos.x + xv, packet.pos.y + yv, packet.pos.z + zv,
                               xa, ya, za);
        }
    }

    // ── LevelEventHandler.levelEvent ──────────────────────────────────────

    void LevelEvent(int type, const glm::ivec3& pos, int data) {
        ClientLevelBridge* levelPtr = Sink();
        if (!levelPtr) return;
        ClientLevelBridge& level = *levelPtr;
        Game::JavaRandom& random = level.Random();
        const Game::IBlockAccess* blocks = level.Blocks();
        // The sounds MC's handler plays that this engine does NOT already send
        // as their own sound packet (PlayLevelEventSound): played here.
        const bool soundHere = !Game::LevelEventSoundIsNetworked(type);
        const auto spread = [&random]() { const float a = random.NextFloat(); return a - random.NextFloat(); };

        switch (type) {
            case LE::SOUND_FIREWORK_SHOOT:
                if (soundHere) PlayLocal(pos, Game::SoundEvents::FIREWORK_ROCKET_SHOOT, Game::SoundSource::Neutral, 1.0f, 1.2f);
                break;
            case LE::SOUND_SPELL_POTION_SPLASH:
            case LE::SOUND_INSTANT_POTION_SPLASH:
                if (soundHere) {
                    PlayLocal(pos, Game::SoundEvents::SPLASH_POTION_BREAK, Game::SoundSource::Neutral, 1.0f,
                              random.NextFloat() * 0.1f + 0.9f);
                }
                break;
            case LE::COMPOSTER_FILL:
                ComposterFill(level, pos, data > 0);
                break;
            case LE::LAVA_FIZZ:
                // (The fizz itself is a sound packet.)
                for (int i = 0; i < 8; ++i) {
                    level.AddParticle(ParticleOptions(ParticleKind::LargeSmoke), pos.x + random.NextDouble(),
                                      pos.y + 1.2, pos.z + random.NextDouble(), 0.0, 0.0, 0.0);
                }
                break;
            case LE::REDSTONE_TORCH_BURNOUT:
                for (int i = 0; i < 5; ++i) {
                    const double x = pos.x + random.NextDouble() * 0.6 + 0.2;
                    const double y = pos.y + random.NextDouble() * 0.6 + 0.2;
                    const double z = pos.z + random.NextDouble() * 0.6 + 0.2;
                    level.AddParticle(ParticleOptions(ParticleKind::Smoke), x, y, z, 0.0, 0.0, 0.0);
                }
                break;
            case LE::END_PORTAL_FRAME_FILL:
                for (int i = 0; i < 16; ++i) {
                    const double x = pos.x + (5.0 + random.NextDouble() * 6.0) / 16.0;
                    const double y = pos.y + 0.8125;
                    const double z = pos.z + (5.0 + random.NextDouble() * 6.0) / 16.0;
                    level.AddParticle(ParticleOptions(ParticleKind::Smoke), x, y, z, 0.0, 0.0, 0.0);
                }
                break;
            case LE::DRIPSTONE_DRIP:
                DripstoneDrip(level, pos);
                break;
            case LE::PARTICLES_AND_SOUND_PLANT_GROWTH:
                AddGrowthParticles(level, pos, data);
                break;
            case LE::PARTICLES_SHOOT_SMOKE:
                ShootParticles(level, data, pos, ParticleKind::Smoke);
                break;
            case LE::PARTICLES_DESTROY_BLOCK:
            case LE::PARTICLES_DESTROY_BLOCK_ONLY:
                AddDestroyBlockEffect(pos, Game::BlockState::FromRawId(static_cast<uint32_t>(data)));
                break;
            case LE::PARTICLES_SPELL_POTION_SPLASH:
                PotionSplash(level, ParticleKind::Effect, pos, data);
                break;
            case LE::PARTICLES_INSTANT_POTION_SPLASH:
                PotionSplash(level, ParticleKind::InstantEffect, pos, data);
                break;
            case LE::PARTICLES_EYE_OF_ENDER_DEATH: {
                const double x = pos.x + 0.5, y = pos.y, z = pos.z + 0.5;
                const ParticleOptions eye = ParticleOptions::Item(Game::Items::EnderEye);
                for (int i = 0; i < 8; ++i) {
                    level.AddParticle(eye, x, y, z, random.NextGaussian() * 0.15, random.NextDouble() * 0.2,
                                      random.NextGaussian() * 0.15);
                }
                for (double angle = 0.0; angle < 6.283185307179586; angle += 0.15707963267948966) {
                    level.AddParticle(ParticleOptions(ParticleKind::Portal), x + std::cos(angle) * 5.0, y - 0.4,
                                      z + std::sin(angle) * 5.0, std::cos(angle) * -5.0, 0.0, std::sin(angle) * -5.0);
                    level.AddParticle(ParticleOptions(ParticleKind::Portal), x + std::cos(angle) * 5.0, y - 0.4,
                                      z + std::sin(angle) * 5.0, std::cos(angle) * -7.0, 0.0, std::sin(angle) * -7.0);
                }
                break;
            }
            case LE::PARTICLES_MOBBLOCK_SPAWN:
                for (int i = 0; i < 20; ++i) {
                    const double x = pos.x + 0.5 + (random.NextDouble() - 0.5) * 2.0;
                    const double y = pos.y + 0.5 + (random.NextDouble() - 0.5) * 2.0;
                    const double z = pos.z + 0.5 + (random.NextDouble() - 0.5) * 2.0;
                    level.AddParticle(ParticleOptions(ParticleKind::Smoke), x, y, z, 0.0, 0.0, 0.0);
                    level.AddParticle(ParticleOptions(ParticleKind::Flame), x, y, z, 0.0, 0.0, 0.0);
                }
                break;
            case LE::PARTICLES_DRAGON_FIREBALL_SPLASH:
                for (int i = 0; i < 200; ++i) {
                    const float volume = random.NextFloat() * 4.0f;
                    const float angle = random.NextFloat() * 6.2831855f;
                    const double vx = static_cast<double>(std::cos(angle) * volume);
                    const double vy = 0.01 + random.NextDouble() * 0.5;
                    const double vz = static_cast<double>(std::sin(angle) * volume);
                    level.AddParticle(ParticleOptions::Power(ParticleKind::DragonBreath, volume),
                                      pos.x + vx * 0.1, pos.y + 0.3, pos.z + vz * 0.1, vx, vy, vz);
                }
                if (data == 1 && soundHere) {
                    PlayLocal(pos, Game::SoundEvents::DRAGON_FIREBALL_EXPLODE, Game::SoundSource::Hostile, 1.0f,
                              random.NextFloat() * 0.1f + 0.9f);
                }
                break;
            case LE::PARTICLES_DRAGON_BLOCK_BREAK:
                level.AddParticle(ParticleOptions(ParticleKind::Explosion), pos.x + 0.5, pos.y + 0.5, pos.z + 0.5,
                                  0.0, 0.0, 0.0);
                break;
            case LE::PARTICLES_WATER_EVAPORATING:
                for (int i = 0; i < 8; ++i) {
                    level.AddParticle(ParticleOptions(ParticleKind::Cloud), pos.x + random.NextDouble(), pos.y + 1.2,
                                      pos.z + random.NextDouble(), 0.0, 0.0, 0.0);
                }
                break;
            case LE::PARTICLES_SHOOT_WHITE_SMOKE:
                ShootParticles(level, data, pos, ParticleKind::WhiteSmoke);
                break;
            case LE::PARTICLES_BEE_GROWTH:
            case LE::PARTICLES_TURTLE_EGG_PLACEMENT:
                SpawnParticleInBlock(level, pos, data, ParticleKind::HappyVillager);
                break;
            case LE::PARTICLES_SMASH_ATTACK:
                SpawnSmashAttackParticles(level, pos, data);
                break;
            case LE::PARTICLES_DRAGON_EGG_TELEPORT:
                TeleportParticles(level, pos, UnpackDifference(pos, data, 16, 8, 16));
                break;
            case LE::PARTICLES_SHULKER_TELEPORT:
                TeleportParticles(level, pos, UnpackDifference(pos, data, 8, 8, 8));
                break;
            case LE::PARTICLES_CONSUME_EFFECT_TELEPORT:
                TeleportParticles(level, pos, UnpackDifference(pos, data, 127, 127, 127));
                break;
            case LE::PARTICLES_ENDERMAN_TELEPORT: {
                // EntityTypes.ENDERMAN: 0.6 wide, 2.9 tall.
                const float width = Game::GetEntityTypeInfo(Game::EntityTypeId::Enderman).width;
                const float height = Game::GetEntityTypeInfo(Game::EntityTypeId::Enderman).height;
                const glm::ivec3 to = UnpackDifference(pos, data, 127, 127, 127);
                for (int i = 0; i < 128; ++i) {
                    const double t = random.NextDouble();
                    const float vx = (random.NextFloat() - 0.5f) * 0.2f;
                    const float vy = (random.NextFloat() - 0.5f) * 0.2f;
                    const float vz = (random.NextFloat() - 0.5f) * 0.2f;
                    const double x = to.x + (pos.x - to.x) * t + (random.NextDouble() - 0.5) * width * 2.0;
                    const double y = to.y + (pos.y - to.y) * t + random.NextDouble() * height;
                    const double z = to.z + (pos.z - to.z) * t + (random.NextDouble() - 0.5) * width * 2.0;
                    level.AddParticle(ParticleOptions(ParticleKind::Portal), x, y, z, vx, vy, vz);
                }
                break;
            }
            case LE::PARTICLES_DESTROY_PROGRESS:
            case LE::PARTICLES_AND_SOUND_DESTROY_PROGRESS:
                AddBreakingBlockEffects(pos, std::clamp(data, 0, 5),
                                        type == LE::PARTICLES_AND_SOUND_DESTROY_PROGRESS);
                break;
            case LE::ANIMATION_END_GATEWAY_SPAWN:
                level.AddAlwaysVisibleParticle(ParticleOptions(ParticleKind::ExplosionEmitter), pos.x + 0.5,
                                               pos.y + 0.5, pos.z + 0.5, 0.0, 0.0, 0.0);
                if (soundHere) {
                    PlayLocal(pos, Game::SoundEvents::END_GATEWAY_SPAWN, Game::SoundSource::Blocks, 10.0f,
                              (1.0f + spread() * 0.2f) * 0.7f);
                }
                break;
            case LE::ANIMATION_DRAGON_SUMMON_ROAR:
                if (soundHere) {
                    PlayLocal(pos, Game::SoundEvents::ENDER_DRAGON_GROWL, Game::SoundSource::Hostile, 64.0f,
                              0.8f + random.NextFloat() * 0.3f);
                }
                break;
            case LE::PARTICLES_ELECTRIC_SPARK:
                if (data >= 0 && data < 3) {
                    SpawnParticlesAlongAxis(level, static_cast<Game::Axis>(data), pos, 0.125,
                                            ParticleKind::ElectricSpark, 10, 19);
                } else {
                    SpawnParticlesOnBlockFaces(level, pos, ParticleKind::ElectricSpark, 3, 5);
                }
                break;
            case LE::PARTICLES_WAX_ON:
                SpawnParticlesOnBlockFaces(level, pos, ParticleKind::WaxOn, 3, 5);
                break;
            case LE::PARTICLES_WAX_OFF:
                SpawnParticlesOnBlockFaces(level, pos, ParticleKind::WaxOff, 3, 5);
                break;
            case LE::PARTICLES_SCRAPE:
                SpawnParticlesOnBlockFaces(level, pos, ParticleKind::Scrape, 3, 5);
                break;
            case LE::PARTICLES_SCULK_CHARGE:
                SculkCharge(level, pos, data);
                break;
            case LE::PARTICLES_SCULK_SHRIEK: {
                // SculkShriekerBlock.TOP_Y = 0.5 (the collision column).
                for (int i = 0; i < 10; ++i) {
                    level.AddParticle(ParticleOptions::Shriek(i * 5), pos.x + 0.5, pos.y + 0.5, pos.z + 0.5,
                                      0.0, 0.0, 0.0);
                }
                bool waterlogged = false;
                if (blocks) {
                    waterlogged = blocks->GetBlockState(pos.x, pos.y, pos.z).GetValueByName("waterlogged") == "true";
                }
                if (!waterlogged && soundHere) {
                    Sounds::PlayLocal(glm::dvec3(pos.x + 0.5, pos.y + 0.5, pos.z + 0.5),
                                      Game::SoundEvents::SCULK_SHRIEKER_SHRIEK, Game::SoundSource::Blocks, 2.0f,
                                      0.6f + random.NextFloat() * 0.4f);
                }
                break;
            }
            case LE::PARTICLES_AND_SOUND_BRUSH_BLOCK_COMPLETE: {
                const Game::BlockState brushed = Game::BlockState::FromRawId(static_cast<uint32_t>(data));
                if (soundHere) {
                    if (brushed.Is(Game::BlockID::SuspiciousSand)) {
                        PlayLocal(pos, Game::SoundEvents::BRUSH_SAND_COMPLETED, Game::SoundSource::Players, 1.0f, 1.0f);
                    } else if (brushed.Is(Game::BlockID::SuspiciousGravel)) {
                        PlayLocal(pos, Game::SoundEvents::BRUSH_GRAVEL_COMPLETED, Game::SoundSource::Players, 1.0f, 1.0f);
                    }
                }
                AddDestroyBlockEffect(pos, brushed);
                break;
            }
            case LE::PARTICLES_EGG_CRACK:
                SpawnParticlesOnBlockFaces(level, pos, ParticleKind::EggCrack, 3, 6);
                break;
            case LE::PARTICLES_TRIAL_SPAWNER_SPAWN:
                TrialSpawnParticles(level, pos, TrialFlame(data));
                break;
            case LE::PARTICLES_TRIAL_SPAWNER_SPAWN_MOB_AT:
                if (soundHere) {
                    PlayLocal(pos, Game::SoundEvents::TRIAL_SPAWNER_SPAWN_MOB, Game::SoundSource::Blocks, 1.0f,
                              spread() * 0.2f + 1.0f, true);
                }
                TrialSpawnParticles(level, pos, TrialFlame(data));
                break;
            case LE::PARTICLES_TRIAL_SPAWNER_DETECT_PLAYER:
                if (soundHere) {
                    PlayLocal(pos, Game::SoundEvents::TRIAL_SPAWNER_DETECT_PLAYER, Game::SoundSource::Blocks, 1.0f,
                              spread() * 0.2f + 1.0f, true);
                }
                TrialDetectParticles(level, pos, data, ParticleKind::TrialSpawnerDetection);
                break;
            case LE::ANIMATION_TRIAL_SPAWNER_EJECT_ITEM:
                if (soundHere) {
                    PlayLocal(pos, Game::SoundEvents::TRIAL_SPAWNER_EJECT_ITEM, Game::SoundSource::Blocks, 1.0f,
                              spread() * 0.2f + 1.0f, true);
                }
                TrialEjectParticles(level, pos);
                break;
            case LE::ANIMATION_VAULT_ACTIVATE:
                if (blocks && blocks->GetBlock(pos.x, pos.y, pos.z) == Game::BlockID::Vault) {
                    // emitActivationParticles: the connection motes toward
                    // every connected player first (the vault's own shared
                    // data, on this client's copy of it), then the cage burst.
                    if (auto* levelWrite = dynamic_cast<Game::ILevelWrite*>(const_cast<Game::IBlockAccess*>(blocks))) {
                        if (const auto* vault = dynamic_cast<const Game::VaultBlockEntity*>(
                                levelWrite->GetBlockEntity(pos))) {
                            vault->EmitConnectionParticlesForNearbyPlayers(*levelWrite);
                        }
                    }
                    VaultActivation(level, pos, data == 0 ? ParticleKind::SmallFlame : ParticleKind::SoulFireFlame);
                    if (soundHere) {
                        PlayLocal(pos, Game::SoundEvents::VAULT_ACTIVATE, Game::SoundSource::Blocks, 1.0f,
                                  spread() * 0.2f + 1.0f, true);
                    }
                }
                break;
            case LE::ANIMATION_VAULT_DEACTIVATE:
                VaultDeactivation(level, pos, data == 0 ? ParticleKind::SmallFlame : ParticleKind::SoulFireFlame);
                if (soundHere) {
                    PlayLocal(pos, Game::SoundEvents::VAULT_DEACTIVATE, Game::SoundSource::Blocks, 1.0f,
                              spread() * 0.2f + 1.0f, true);
                }
                break;
            case LE::ANIMATION_VAULT_EJECT_ITEM:
                TrialEjectParticles(level, pos);
                break;
            case LE::ANIMATION_SPAWN_COBWEB:
                for (int i = 0; i < 10; ++i) {
                    const double vx = random.NextGaussian() * 0.02;
                    const double vy = random.NextGaussian() * 0.02;
                    const double vz = random.NextGaussian() * 0.02;
                    level.AddParticle(ParticleOptions(ParticleKind::Poof), pos.x + random.NextDouble(),
                                      pos.y + random.NextDouble(), pos.z + random.NextDouble(), vx, vy, vz);
                }
                if (soundHere) {
                    PlayLocal(pos, Game::SoundEvents::COBWEB_PLACE, Game::SoundSource::Blocks, 1.0f,
                              spread() * 0.2f + 1.0f, true);
                }
                break;
            case LE::PARTICLES_TRIAL_SPAWNER_DETECT_PLAYER_OMINOUS:
                if (soundHere) {
                    PlayLocal(pos, Game::SoundEvents::TRIAL_SPAWNER_DETECT_PLAYER, Game::SoundSource::Blocks, 1.0f,
                              spread() * 0.2f + 1.0f, true);
                }
                TrialDetectParticles(level, pos, data, ParticleKind::TrialSpawnerDetectionOminous);
                break;
            case LE::PARTICLES_TRIAL_SPAWNER_BECOME_OMINOUS:
                if (soundHere) {
                    PlayLocal(pos, Game::SoundEvents::TRIAL_SPAWNER_OMINOUS_ACTIVATE, Game::SoundSource::Blocks,
                              data == 0 ? 0.3f : 1.0f, spread() * 0.2f + 1.0f, true);
                }
                TrialDetectParticles(level, pos, 0, ParticleKind::TrialSpawnerDetectionOminous);
                TrialBecomeOminousParticles(level, pos);
                break;
            case LE::PARTICLES_TRIAL_SPAWNER_SPAWN_ITEM:
                if (soundHere) {
                    PlayLocal(pos, Game::SoundEvents::TRIAL_SPAWNER_SPAWN_ITEM, Game::SoundSource::Blocks, 1.0f,
                              spread() * 0.2f + 1.0f, true);
                }
                TrialSpawnParticles(level, pos, TrialFlame(data));
                break;
            default:
                // Sound-only events arrive as sound packets; the jukebox's
                // song events are ClientPacketHandler's.
                break;
        }
    }

    void SpawnItemParticles(const glm::dvec3& eye, float yRot, float xRot, uint32_t itemId, int count) {
        ClientLevelBridge* level = Sink();
        if (!level || itemId == 0) return;
        Game::JavaRandom& r = level->Random();
        const ParticleOptions options = ParticleOptions::Item(itemId);
        // Vec3.xRot(-xRot) then Vec3.yRot(-yRot), in radians.
        const float xa = -xRot * 0.017453292f;
        const float ya = -yRot * 0.017453292f;
        const double xc = std::cos(xa), xs = std::sin(xa);
        const double yc = std::cos(ya), ys = std::sin(ya);
        const auto rotate = [&](glm::dvec3 v) {
            v = glm::dvec3(v.x, v.y * xc + v.z * xs, v.z * xc - v.y * xs);
            return glm::dvec3(v.x * yc + v.z * ys, v.y, v.z * yc - v.x * ys);
        };
        for (int i = 0; i < count; ++i) {
            const glm::dvec3 d = rotate(glm::dvec3((static_cast<double>(r.NextFloat()) - 0.5) * 0.1,
                                                   static_cast<double>(r.NextFloat()) * 0.1 + 0.1, 0.0));
            const double y1 = static_cast<double>(-r.NextFloat()) * 0.6 - 0.3;
            const glm::dvec3 p = rotate(glm::dvec3((static_cast<double>(r.NextFloat()) - 0.5) * 0.3, y1, 0.6)) + eye;
            level->AddParticle(options, p.x, p.y, p.z, d.x, d.y + 0.05, d.z);
        }
    }

} // namespace Client::LevelEvents

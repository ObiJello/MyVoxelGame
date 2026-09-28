// File: src/client/world/ClientParticleTicks.cpp
#include "client/world/ClientParticleTicks.hpp"

#include "client/entity/ClientMobManager.hpp"
#include "client/entity/Player.hpp"
#include "client/entity/RemotePlayerManager.hpp"
#include "client/renderer/environment/EnvironmentState.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "client/world/ClientChunkManager.hpp"
#include "client/world/ClientLevel.hpp"
#include "client/world/ClientWeather.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/particle/ParticleOptions.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/entity/CampfireBlockEntity.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/World.hpp"
#include "common/world/lighting/ChunkLight.hpp"
#include "common/world/tags/DataTags.hpp"
#include "platform/GameDirectory.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace Client::ParticleTicks {

    namespace {

        using Game::ParticleKind;
        using Game::ParticleOptions;

        // ── Collision-shape queries (VoxelShape.max / min / isFullBlock) ──

        struct ShapeExtent {
            bool   empty = true;
            double minY = 0.0, maxY = 0.0;
            double minX = 0.0, maxX = 0.0, minZ = 0.0, maxZ = 0.0;
        };

        ShapeExtent CollisionExtent(Game::BlockState state) {
            ShapeExtent e;
            if (state.Block() == Game::BlockID::Air) return e;
            // A block without collision (a flower, a sapling, fluid) has an
            // empty collision shape.
            if (!Game::BlockRegistry::HasCollision(state.Block())) return e;
            for (const auto& b : Game::BlockRegistry::GetBlockCollisionShapeSet(state)) {
                if (e.empty) {
                    e.minX = b.min.x; e.minY = b.min.y; e.minZ = b.min.z;
                    e.maxX = b.max.x; e.maxY = b.max.y; e.maxZ = b.max.z;
                    e.empty = false;
                } else {
                    e.minX = std::min(e.minX, static_cast<double>(b.min.x));
                    e.minY = std::min(e.minY, static_cast<double>(b.min.y));
                    e.minZ = std::min(e.minZ, static_cast<double>(b.min.z));
                    e.maxX = std::max(e.maxX, static_cast<double>(b.max.x));
                    e.maxY = std::max(e.maxY, static_cast<double>(b.max.y));
                    e.maxZ = std::max(e.maxZ, static_cast<double>(b.max.z));
                }
            }
            return e;
        }

        // VoxelShape.max(Axis.Y, x, z): the top of the boxes over the column.
        double CollisionTopAt(Game::BlockState state, double fx, double fz) {
            if (state.Block() == Game::BlockID::Air || !Game::BlockRegistry::HasCollision(state.Block())) return 0.0;
            double top = 0.0;
            bool any = false;
            for (const auto& b : Game::BlockRegistry::GetBlockCollisionShapeSet(state)) {
                if (fx < b.min.x || fx > b.max.x || fz < b.min.z || fz > b.max.z) continue;
                top = any ? std::max(top, static_cast<double>(b.max.y)) : static_cast<double>(b.max.y);
                any = true;
            }
            return any ? top : 0.0;
        }

        bool CollisionFullBlock(Game::BlockState state) {
            if (state.Block() == Game::BlockID::Air || !Game::BlockRegistry::HasCollision(state.Block())) return false;
            return Game::BlockRegistry::GetBlockCollisionShapeSet(state).IsFullCube();
        }

        // BlockState.isFaceSturdy(level, pos, DOWN).
        bool BottomSturdy(const Game::IBlockAccess& blocks, const glm::ivec3& pos, Game::BlockState state) {
            if (state.Block() == Game::BlockID::Air) return false;
            constexpr float e = 0.0001f;
            for (const auto& b : Game::BlockRegistry::GetBlockShapeSetAt(blocks, pos, state)) {
                if (b.min.y <= e && b.min.x <= e && b.max.x >= 1.0f - e && b.min.z <= e && b.max.z >= 1.0f - e) {
                    return true;
                }
            }
            return false;
        }

        bool IsImpermeable(Game::BlockID id) {
            static std::vector<uint8_t> table;
            if (table.empty()) {
                table.assign(Game::BlockRegistry::Size, 0);
                for (size_t i = 0; i < Game::BlockRegistry::Size; ++i) {
                    const std::string& slug = Game::BlockRegistry::Get(static_cast<Game::BlockID>(i)).registrySlug;
                    if (slug.empty()) continue;
                    const auto& tags = Game::DataTags::TagsFor(Game::DataTags::Registry::Block, slug);
                    table[i] = std::find(tags.begin(), tags.end(), "#minecraft:impermeable") != tags.end() ? 1 : 0;
                }
            }
            const size_t i = static_cast<size_t>(id);
            return i < table.size() && table[i] != 0;
        }

        // ── The biome ambient-particle table ──────────────────────────────

        struct AmbientParticle {
            ParticleOptions particle;
            float probability = 0.0f;
        };

        std::filesystem::path DataRoot() {
            if (const char* env = std::getenv("MC_DATA_ROOT")) return env;
            return "data";
        }

        const std::vector<std::vector<AmbientParticle>>& AmbientTable() {
            static std::vector<std::vector<AmbientParticle>> table = [] {
                std::vector<std::vector<AmbientParticle>> t(Game::BiomeRegistry::Count());
                for (Game::BiomeId id = 0; id < Game::BiomeRegistry::Count(); ++id) {
                    const std::string_view name = Game::BiomeRegistry::Get(id).name;
                    std::string ns = "minecraft", path(name);
                    if (const size_t colon = name.find(':'); colon != std::string_view::npos) {
                        ns = std::string(name.substr(0, colon));
                        path = std::string(name.substr(colon + 1));
                    }
                    const std::filesystem::path file = DataRoot() / ns / "worldgen" / "biome" / (path + ".json");
                    std::ifstream in(file, std::ios::binary);
                    if (!in) continue;
                    std::stringstream ss;
                    ss << in.rdbuf();
                    try {
                        const nlohmann::json j = nlohmann::json::parse(ss.str());
                        const auto attrs = j.find("attributes");
                        if (attrs == j.end() || !attrs->is_object()) continue;
                        const auto list = attrs->find("minecraft:visual/ambient_particles");
                        if (list == attrs->end() || !list->is_array()) continue;
                        for (const auto& entry : *list) {
                            if (!entry.is_object() || !entry.contains("particle")) continue;
                            const auto& particle = entry["particle"];
                            const std::string type = particle.is_string() ? particle.get<std::string>()
                                                                          : particle.value("type", std::string());
                            const auto kind = Game::ParticleTypes::FromName(type);
                            if (!kind) continue;   // another mod's particle type
                            AmbientParticle ap;
                            ap.particle = ParticleOptions(*kind);
                            ap.probability = entry.value("probability", 0.0f);
                            t[id].push_back(ap);
                        }
                    } catch (const std::exception& e) {
                        Log::Warning("[ParticleTicks] bad biome JSON %s: %s", file.string().c_str(), e.what());
                    }
                }
                return t;
            }();
            return table;
        }

    } // namespace

    // ClientLevel.doAnimateTick: `dripParticle != null && random.nextInt(10)
    // == 0` → trySpawnDripParticles(below, belowState, drip, watertight).
    void FluidDrip(const glm::ivec3& pos, Game::BlockState state, const Game::FluidState& fluid,
                   const Game::IBlockAccess& blocks, ClientLevelBridge& sink, Game::JavaRandom& random) {
        ParticleKind drip;
        if (fluid.Is(Game::FluidType::Water)) drip = ParticleKind::DrippingWater;
        else if (fluid.Is(Game::FluidType::Lava)) drip = ParticleKind::DrippingLava;
        else return;
        if (random.NextInt(10) != 0) return;
        const bool watertightBottom = BottomSturdy(blocks, pos, state);
        const glm::ivec3 below = pos - glm::ivec3(0, 1, 0);
        const Game::BlockState belowState = blocks.GetBlockState(below.x, below.y, below.z);
        if (!Game::FluidStateOf(belowState).IsEmpty()) return;
        const ShapeExtent shape = CollisionExtent(belowState);
        const auto spawnFluidParticle = [&](double x1, double x2, double z1, double z2, double y) {
            const double x = x1 + random.NextDouble() * (x2 - x1);
            const double z = z1 + random.NextDouble() * (z2 - z1);
            sink.AddParticle(ParticleOptions(drip), x, y, z, 0.0, 0.0, 0.0);
        };
        const auto spawnParticle = [&](double y) {
            spawnFluidParticle(below.x + shape.minX, below.x + shape.maxX, below.z + shape.minZ, below.z + shape.maxZ, y);
        };
        // An empty shape's max is -infinity: always "below 1".
        const double topSideHeight = shape.empty ? -1.0 : shape.maxY;
        if (topSideHeight < 1.0) {
            if (watertightBottom) {
                spawnFluidParticle(below.x, below.x + 1, below.z, below.z + 1, below.y + 1 - 0.05);
            }
        } else if (!IsImpermeable(belowState.Block())) {
            const double bottomSideHeight = shape.minY;
            if (bottomSideHeight > 0.0) {
                spawnParticle(below.y + bottomSideHeight - 0.05);
            } else {
                const glm::ivec3 below2 = below - glm::ivec3(0, 1, 0);
                const Game::BlockState below2State = blocks.GetBlockState(below2.x, below2.y, below2.z);
                const ShapeExtent below2Shape = CollisionExtent(below2State);
                const double below2Top = below2Shape.empty ? -1.0 : below2Shape.maxY;
                if (below2Top < 1.0 && Game::FluidStateOf(below2State).IsEmpty()) {
                    spawnParticle(below.y - 0.05);
                }
            }
        }
    }

    // ClientLevel.doAnimateTick's last step: the biome's ambient particles
    // (EnvironmentAttributes.AMBIENT_PARTICLES — a noise-biome attribute)
    // in any cell whose collision is not a full block.
    void AmbientParticles(const glm::ivec3& pos, Game::BlockState state, ClientLevelBridge& sink,
                          Game::JavaRandom& random) {
        if (CollisionFullBlock(state)) return;
        if (!g_clientChunkManager) return;
        const auto& table = AmbientTable();
        const Game::BiomeId biome = g_clientChunkManager->BiomeAtWorld(pos.x, pos.y, pos.z);
        if (biome >= table.size()) return;
        for (const AmbientParticle& ap : table[biome]) {
            // AmbientParticle.canSpawn: random.nextFloat() <= probability.
            if (random.NextFloat() <= ap.probability) {
                sink.AddParticle(ap.particle, pos.x + random.NextDouble(), pos.y + random.NextDouble(),
                                 pos.z + random.NextDouble(), 0.0, 0.0, 0.0);
            }
        }
    }

    namespace {
        // MC ClientLevel.rainSoundTime — the rain sound's pacing counter.
        int s_rainSoundTime = 0;
    }

    // MC ClientLevel.tickWeatherEffects: the rain splashes on the ground
    // around the camera and, from the last column that got one, the rain
    // sound. Heights are the client chunks' MOTION_BLOCKING heightmaps and
    // the precipitation is ClientLevel.getPrecipitationAt (ClientWeather).
    void TickWeather(const glm::dvec3& camera, const Game::IBlockAccess& blocks, ClientLevelBridge& sink) {
        const int particles = Platform::g_gameSettings.GetParticles();   // 0 all, 1 decreased, 2 minimal
        const int weatherRadius = Platform::g_gameSettings.GetWeatherRadius();
        const float rainLevel = ClientWeather::RainLevel(1.0f);
        if (rainLevel <= 0.0f) return;
        // RandomSource.createThreadLocalInstance(gameTime * 312987231L).
        Game::JavaRandom random(sink.GetGameTime() * 312987231LL);
        // BlockPos.containing(mainCamera().position()).
        const glm::ivec3 cam(static_cast<int>(std::floor(camera.x)), static_cast<int>(std::floor(camera.y)),
                             static_cast<int>(std::floor(camera.z)));
        const int minY = Game::DimensionMinY(ClientLevels::ActiveDimension());
        bool haveRainPosition = false;
        glm::ivec3 rainPosition(0);
        const int diameter = 2 * weatherRadius + 1;
        const int area = diameter * diameter;
        const int rainParticles = static_cast<int>(0.225f * static_cast<float>(area) * rainLevel * rainLevel) /
                                  (particles == 1 ? 2 : 1);
        for (int i = 0; i < rainParticles; ++i) {
            const int x = random.NextInt(diameter) - weatherRadius;
            const int z = random.NextInt(diameter) - weatherRadius;
            // getHeightmapPos(MOTION_BLOCKING, cameraPosition.offset(x, 0, z)).
            const glm::ivec3 heightmapPos(cam.x + x, ClientWeather::MotionBlockingHeight(cam.x + x, cam.z + z),
                                          cam.z + z);
            if (heightmapPos.y <= minY || heightmapPos.y > cam.y + 10 || heightmapPos.y < cam.y - 10) continue;
            if (ClientWeather::BiomePrecipitationAt(blocks, heightmapPos) !=
                static_cast<int>(Game::BiomeRegistry::Precipitation::Rain)) {
                continue;
            }
            rainPosition = heightmapPos - glm::ivec3(0, 1, 0);
            haveRainPosition = true;
            if (particles == 2) break;   // MINIMAL
            const double bx = random.NextDouble();
            const double bz = random.NextDouble();
            const Game::BlockState block = blocks.GetBlockState(rainPosition.x, rainPosition.y, rainPosition.z);
            const Game::FluidState fluid = Game::GetFluidState(blocks, rainPosition);
            const double blockTop = CollisionTopAt(block, bx, bz);
            const double fluidTop = fluid.IsEmpty() ? 0.0 : static_cast<double>(Game::FluidHeight(blocks, rainPosition, fluid));
            const double y = std::max(blockTop, fluidTop);
            const bool litCampfire = (block.Is(Game::BlockID::Campfire) || block.Is(Game::BlockID::SoulCampfire)) &&
                                     block.GetValueByName("lit") == "true";
            const bool smoke = fluid.Is(Game::FluidType::Lava) || block.Is(Game::BlockID::MagmaBlock) || litCampfire;
            sink.AddParticle(ParticleOptions(smoke ? ParticleKind::Smoke : ParticleKind::Rain),
                             rainPosition.x + bx, rainPosition.y + y, rainPosition.z + bz, 0.0, 0.0, 0.0);
        }

        // The sound half, from the same random stream.
        if (haveRainPosition && random.NextInt(3) < s_rainSoundTime++) {
            s_rainSoundTime = 0;
            // playLocalSound(BlockPos, ...) plays at the cell's centre.
            const glm::dvec3 at = glm::dvec3(rainPosition) + glm::dvec3(0.5);
            // Overhead rain — the column's rain is above the camera and the
            // camera's own column is roofed (its heightmap is above the eye's
            // block): the muffled weather.rain.above.
            if (rainPosition.y > cam.y + 1 &&
                ClientWeather::MotionBlockingHeight(cam.x, cam.z) > static_cast<int>(std::floor(static_cast<float>(camera.y)))) {
                sink.PlayLocalSound(at, Game::SoundEvents::WEATHER_RAIN_ABOVE, Game::SoundSource::Weather,
                                    0.1f, 0.5f, false);
            } else {
                sink.PlayLocalSound(at, Game::SoundEvents::WEATHER_RAIN, Game::SoundSource::Weather,
                                    0.2f, 1.0f, false);
            }
        }
    }

    // CampfireBlockEntity.particleTick (the client ticker of a LIT campfire).
    void TickBlockEntities(ClientChunkManager& chunks, const Game::IBlockAccess& blocks, ClientLevelBridge& sink) {
        Game::JavaRandom& random = sink.Random();
        chunks.ForEachLoadedChunkPos([&](Game::Math::ChunkPos cp) {
            const ClientChunk* chunk = chunks.GetChunk(cp);
            if (!chunk || chunk->campfires.empty()) return;
            for (const glm::ivec3& pos : chunk->campfires) {
                const Game::BlockState state = blocks.GetBlockState(pos.x, pos.y, pos.z);
                if (state.GetValueByName("lit") != "true") continue;
                const bool signal = state.GetValueByName("signal_fire") == "true";
                // CampfireBlock.makeParticles(level, pos, signal, false).
                if (random.NextFloat() < 0.11f) {
                    for (int i = 0; i < random.NextInt(2) + 2; ++i) {
                        const double x = pos.x + 0.5 + random.NextDouble() / 3.0 * (random.NextBool() ? 1 : -1);
                        const double y = pos.y + random.NextDouble() + random.NextDouble();
                        const double z = pos.z + 0.5 + random.NextDouble() / 3.0 * (random.NextBool() ? 1 : -1);
                        sink.AddParticle(ParticleOptions(signal ? ParticleKind::CampfireSignalSmoke
                                                                : ParticleKind::CampfireCosySmoke),
                                         /*overrideLimiter=*/true, /*alwaysShow=*/true, x, y, z, 0.0, 0.07, 0.0);
                    }
                }
                // The food cooking on it smokes.
                if (!g_clientBlockAccess) continue;
                const auto* campfire = dynamic_cast<const Game::CampfireBlockEntity*>(
                    g_clientBlockAccess->GetBlockEntity(pos));
                if (!campfire) continue;
                // Direction.get2DDataValue: SOUTH 0, WEST 1, NORTH 2, EAST 3.
                const std::string_view facing = state.GetValueByName("facing");
                const int rotation = facing == "west" ? 1 : facing == "north" ? 2 : facing == "east" ? 3 : 0;
                static constexpr int kStepX[4] = {0, -1, 0, 1};   // S, W, N, E
                static constexpr int kStepZ[4] = {1, 0, -1, 0};
                for (int slot = 0; slot < Game::CampfireBlockEntity::SLOT_COUNT; ++slot) {
                    if (campfire->GetItem(slot).IsEmpty() || !(random.NextFloat() < 0.2f)) continue;
                    const int d = ((slot + rotation) % 4 + 4) % 4;
                    const int cw = (d + 1) % 4;   // getClockWise in the 2D order
                    const double x = pos.x + 0.5 - static_cast<double>(static_cast<float>(kStepX[d]) * 0.3125f) +
                                     static_cast<double>(static_cast<float>(kStepX[cw]) * 0.3125f);
                    const double y = pos.y + 0.5;
                    const double z = pos.z + 0.5 - static_cast<double>(static_cast<float>(kStepZ[d]) * 0.3125f) +
                                     static_cast<double>(static_cast<float>(kStepZ[cw]) * 0.3125f);
                    for (int i = 0; i < 4; ++i) {
                        sink.AddParticle(ParticleOptions(ParticleKind::Smoke), x, y, z, 0.0, 5.0E-4, 0.0);
                    }
                }
            }
        });
    }

    namespace {

        constexpr double kPlayerWidth = 0.6;

        bool IsInvisibleRenderShape(Game::BlockID id) {
            using B = Game::BlockID;
            return id == B::Air || id == B::Barrier ||
                   id == B::StructureVoid || id == B::Light || id == B::MovingPiston;
        }

        // MC Entity.spawnSprintParticle: a BLOCK particle of the block under
        // the feet (getOnPosLegacy: 0.2 down), thrown back against the motion.
        void SprintParticle(const glm::dvec3& pos, const glm::dvec3& motion, double width,
                            const Game::IBlockAccess& blocks, ClientLevelBridge& sink) {
            const glm::ivec3 on(static_cast<int>(std::floor(pos.x)), static_cast<int>(std::floor(pos.y - 0.2)),
                                static_cast<int>(std::floor(pos.z)));
            const Game::BlockState state = blocks.GetBlockState(on.x, on.y, on.z);
            if (IsInvisibleRenderShape(state.Block())) return;
            Game::JavaRandom& rng = sink.Random();
            double x = pos.x + (rng.NextDouble() - 0.5) * width;
            double z = pos.z + (rng.NextDouble() - 0.5) * width;
            if (static_cast<int>(std::floor(pos.x)) != on.x) x = std::clamp(x, static_cast<double>(on.x), on.x + 1.0);
            if (static_cast<int>(std::floor(pos.z)) != on.z) z = std::clamp(z, static_cast<double>(on.z), on.z + 1.0);
            sink.AddParticle(ParticleOptions::Block(state), x, pos.y + 0.1, z, motion.x * -4.0, 1.5, motion.z * -4.0);
        }

        // MC Entity.doWaterSplashEffect's particles: 1 + width·20 BUBBLEs and
        // as many SPLASHes on the water line, carrying the entity's motion.
        void WaterSplashParticles(const glm::dvec3& pos, const glm::dvec3& d, double width, ClientLevelBridge& sink) {
            Game::JavaRandom& rng = sink.Random();
            const double yt = std::floor(pos.y) + 1.0;
            const float count = 1.0f + static_cast<float>(width) * 20.0f;
            for (int i = 0; static_cast<float>(i) < count; ++i) {
                const double xo = (rng.NextDouble() * 2.0 - 1.0) * width;
                const double zo = (rng.NextDouble() * 2.0 - 1.0) * width;
                sink.AddParticle(ParticleOptions(ParticleKind::Bubble), pos.x + xo, yt, pos.z + zo,
                                 d.x, d.y - rng.NextDouble() * 0.20000000298023224, d.z);
            }
            for (int i = 0; static_cast<float>(i) < count; ++i) {
                const double xo = (rng.NextDouble() * 2.0 - 1.0) * width;
                const double zo = (rng.NextDouble() * 2.0 - 1.0) * width;
                sink.AddParticle(ParticleOptions(ParticleKind::Splash), pos.x + xo, yt, pos.z + zo, d.x, d.y, d.z);
            }
        }

        // A remote copy carries no fluid state: "in water" is water in the
        // cell at the feet or the one above them (the body's lower half).
        bool CellHasWater(const Game::IBlockAccess& blocks, const glm::dvec3& p, double dy) {
            const Game::BlockState s = blocks.GetBlockState(static_cast<int>(std::floor(p.x)),
                                                            static_cast<int>(std::floor(p.y + dy)),
                                                            static_cast<int>(std::floor(p.z)));
            return Game::FluidStateOf(s).Is(Game::FluidType::Water);
        }

        struct PlayerParticleState {
            glm::dvec3 lastPos{0.0};
            bool wasInWater = false;
            bool primed = false;
            bool seen = false;
        };

    } // namespace

    namespace {
        bool s_localPlayerInvisible = false;
    }

    void SetLocalPlayerInvisible(bool invisible) { s_localPlayerInvisible = invisible; }
    bool IsLocalPlayerInvisible() { return s_localPlayerInvisible; }

    void TickPlayers(const Game::ClientPlayer& local, const Game::IBlockAccess& blocks, ClientLevelBridge& sink) {
        // ── The local player: its own physics state (MC LocalPlayer's
        //    baseTick). Velocity is in blocks per second here.
        {
            static PlayerParticleState s_local;
            const Game::PlayerPhysics& ph = local.physics;
            const glm::dvec3 motion = glm::dvec3(ph.velocity) / 20.0;
            const bool alive = local.health > 0;
            // Engine rule: invisible players leave no dust or splash.
            const bool spectator = local.IsSpectator() || s_localPlayerInvisible ||
                                   local.HasEffect(Game::MobEffectId::Invisibility);
            if (ph.isSprinting && !ph.isInWater && !ph.isInLava && !ph.isSneaking && !spectator && alive &&
                !local.IsSleeping() && local.vehicleId == 0) {
                SprintParticle(ph.position, motion, ph.GetWidth(), blocks, sink);
            }
            if (ph.isInWater && !s_local.wasInWater && s_local.primed && !spectator) {
                WaterSplashParticles(ph.position, motion, ph.GetWidth(), sink);
            }
            s_local.wasInWater = ph.isInWater;
            s_local.primed = true;
        }

        // ── The remote players (their Entity.baseTick on this client).
        if (!g_remotePlayerManager) return;
        static std::unordered_map<uint32_t, PlayerParticleState> s_remote;
        for (auto& [id, st] : s_remote) st.seen = false;
        for (const auto& [id, rp] : g_remotePlayerManager->GetPlayers()) {
            if (!rp.positionInitialized || rp.IsSpectator() || rp.invisible || rp.effects.Invisible()) continue;
            PlayerParticleState& st = s_remote[id];
            st.seen = true;
            const glm::dvec3 pos = rp.position;
            const glm::dvec3 motion = st.primed ? pos - st.lastPos : glm::dvec3(0.0);
            // A jump across the map (teleport, portal) is not motion.
            const bool plausible = glm::dot(motion, motion) < 16.0;
            const bool inWater = CellHasWater(blocks, pos, 0.0) || CellHasWater(blocks, pos, 0.5);
            const bool inLava = Game::FluidStateOf(blocks.GetBlockState(static_cast<int>(std::floor(pos.x)),
                                                                        static_cast<int>(std::floor(pos.y)),
                                                                        static_cast<int>(std::floor(pos.z))))
                                    .Is(Game::FluidType::Lava);
            if (rp.sprinting && !inWater && !inLava && !rp.isCrouching && rp.deathTime == 0 &&
                !rp.sleepingPos.has_value() && rp.vehicleId == 0 && plausible) {
                SprintParticle(pos, motion, kPlayerWidth, blocks, sink);
            }
            if (inWater && !st.wasInWater && st.primed && plausible) {
                WaterSplashParticles(pos, motion, kPlayerWidth, sink);
            }
            st.wasInWater = inWater;
            st.lastPos = pos;
            st.primed = true;
        }
        for (auto it = s_remote.begin(); it != s_remote.end();) {
            it = it->second.seen ? std::next(it) : s_remote.erase(it);
        }
    }

} // namespace Client::ParticleTicks

// File: src/server/level/LiveFeatureLevel.cpp
//
// See LiveFeatureLevel.hpp.
#include "server/level/LiveFeatureLevel.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/block/Direction.hpp"
#include "common/world/block/TreeGrower.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/World.hpp"
#include "common/world/math/WorldMath.hpp"
#include "common/world/ticks/ScheduledTickAccess.hpp"
#include "server/IntegratedServer.hpp"
#include "server/world/ChunkProvider.hpp"
#include "server/world/MyTerrainGenerator.hpp"
#include "server/world/storage/SectionDataUnpacker.hpp"
#include "server/world/tracking/SectionChangeAccumulator.hpp"

#include "data/worldgen/BiomeFeatureRegistry.h"
#include "data/worldgen/features/AetherFeatures.h"
#include "data/worldgen/features/AquaticFeatures.h"
#include "data/worldgen/features/CaveFeatures.h"
#include "data/worldgen/features/EndFeatures.h"
#include "data/worldgen/features/HushFeatures.h"
#include "data/worldgen/features/MiscOverworldFeatures.h"
#include "data/worldgen/features/NetherFeatures.h"
#include "data/worldgen/features/OreFeatures.h"
#include "data/worldgen/features/TreeFeatures.h"
#include "data/worldgen/features/TwilightFeatureRegistry.h"
#include "data/worldgen/features/VegetationFeatures.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/WorldgenRandom.h"
#include "levelgen/feature/Feature.h"
#include "random/LegacyRandomSource.h"
#include "random/XoroshiroRandomSource.h"
#include "world/biome/Biomes.h"
#include "world/level/block/Blocks.h"

#include <exception>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Game {

    namespace {

        namespace mc  = ::minecraft;
        namespace mlg = ::minecraft::levelgen;
        namespace mwf = ::minecraft::data::worldgen::features;
        using LibState = ::minecraft::world::level::block::state::BlockState;
        using LibBlocks = ::minecraft::world::level::block::Blocks;
        using LibPos = ::minecraft::core::BlockPos;

        // ── Library bootstrap ───────────────────────────────────────────────
        //
        // A world with a terrain generator has bootstrapped all of this before
        // its first chunk; a world without one (a read-only Minecraft save)
        // does it here, once. Both calls are once-only and thread-safe.
        void EnsureLibraryBootstrapped() {
            static std::once_flag once;
            std::call_once(once, [] {
                LibBlocks::bootstrap();
                mc::data::worldgen::BiomeFeatureRegistry::bootstrap();
            });
        }

        LibState* LibAir() { return LibBlocks::AIR->defaultBlockState(); }

        // ── Block states across the boundary ────────────────────────────────
        //
        // Both directions are cached per thread (the server thread is the
        // only caller today). The caches are keyed on library pointers, which
        // live as long as the library's block table; LibAir() doubles as the
        // table's identity, so a re-bootstrapped table drops them.

        struct StateCaches {
            LibState*                                     airSentinel = nullptr;
            std::vector<LibState*>                        toLib;     // game raw id -> library state
            std::unordered_map<const LibState*, uint32_t> toGame;    // library state -> game raw id
        };

        StateCaches& Caches() {
            thread_local StateCaches caches;
            LibState* air = LibAir();
            if (caches.airSentinel != air) {
                caches.airSentinel = air;
                caches.toLib.assign(BlockStates::Total(), nullptr);
                caches.toGame.clear();
            }
            return caches;
        }

        // Game -> library: the registry name ("minecraft:" + slug — the
        // library registers the engine's mod blocks under the same names) and
        // every property value, resolved by the library the way MC decodes a
        // saved state.
        LibState* ToLib(BlockState state) {
            StateCaches& c = Caches();
            const uint32_t raw = state.RawId();
            if (raw < c.toLib.size() && c.toLib[raw]) return c.toLib[raw];

            LibState* resolved = nullptr;
            const BlockID id = state.Block();
            if (id == BlockID::Air) {
                resolved = LibAir();
            } else {
                const std::string& slug = BlockRegistry::Get(id).registrySlug;
                std::map<std::string, std::string> properties;
                const uint16_t count = BlockStates::PropertyCount(id);
                for (uint16_t slot = 0; slot < count; ++slot) {
                    const PropertyId prop = BlockStates::PropertyAt(id, slot);
                    properties.emplace(std::string(BlockStates::PropertyName(prop)),
                                       std::string(state.GetName(prop)));
                }
                if (!slug.empty()) resolved = LibBlocks::resolveState("minecraft:" + slug, properties);
                if (!resolved) {
                    // A block the library does not know stands in as stone:
                    // solid, not replaceable, not a log — a tree grows round it
                    // rather than through it.
                    static std::unordered_set<uint16_t> s_warned;
                    if (s_warned.insert(static_cast<uint16_t>(id)).second) {
                        Log::Warning("[LiveFeatures] block '%s' has no terrain-library state; "
                                     "features treat it as stone", slug.c_str());
                    }
                    resolved = LibBlocks::STONE->defaultBlockState();
                }
            }
            if (raw < c.toLib.size()) c.toLib[raw] = resolved;
            return resolved;
        }

        // Library -> game: exactly the chunk hand-off's resolution
        // (MyTerrainGenerator::MapBlockType), so a grown tree is made of the
        // states a generated one is.
        BlockState ToGame(LibState* state) {
            if (!state) return BlockState{};
            StateCaches& c = Caches();
            auto it = c.toGame.find(state);
            if (it != c.toGame.end()) return BlockState::FromRawId(it->second);
            BlockStateRegistry::Initialize();
            const NbtBlockState nbt =
                BlockStateRegistry::CreateBlockState(state->getIdentifier(), state->getProperties());
            const BlockState resolved = BlockStates::FromIndex(nbt.resolvedId, nbt.resolvedState);
            c.toGame.emplace(state, resolved.RawId());
            return resolved;
        }

        // ── Biomes ──────────────────────────────────────────────────────────

        const mc::world::biome::Biome* LibBiome(uint16_t biomeId) {
            thread_local std::unordered_map<uint16_t, const mc::world::biome::Biome*> cache;
            auto it = cache.find(biomeId);
            if (it != cache.end()) return it->second;
            const std::string_view name = BiomeRegistry::Get(biomeId).name;
            std::string key(name);
            if (key.find(':') == std::string::npos) key = "minecraft:" + key;
            const mc::world::biome::Biome* biome = mc::world::biome::Biomes::get(key);
            cache.emplace(biomeId, biome);
            return biome;
        }

        HeightmapType ToGameHeightmap(mlg::Heightmap::Types type) {
            switch (type) {
                case mlg::Heightmap::Types::WORLD_SURFACE_WG:
                case mlg::Heightmap::Types::WORLD_SURFACE:             return HeightmapType::WorldSurface;
                case mlg::Heightmap::Types::OCEAN_FLOOR_WG:
                case mlg::Heightmap::Types::OCEAN_FLOOR:               return HeightmapType::OceanFloor;
                case mlg::Heightmap::Types::MOTION_BLOCKING:           return HeightmapType::MotionBlocking;
                case mlg::Heightmap::Types::MOTION_BLOCKING_NO_LEAVES: return HeightmapType::MotionBlockingNoLeaves;
            }
            return HeightmapType::WorldSurface;
        }

        Direction DirectionOfStep(int sx, int sy, int sz) {
            if (sy < 0) return Direction::Down;
            if (sy > 0) return Direction::Up;
            if (sz < 0) return Direction::North;
            if (sz > 0) return Direction::South;
            if (sx < 0) return Direction::West;
            return Direction::East;
        }

        glm::ivec3 ToVec(const LibPos& p) { return {p.getX(), p.getY(), p.getZ()}; }

        // ── The adapter ─────────────────────────────────────────────────────

        class LiveFeatureLevel final : public mlg::WorldGenLevel {
        public:
            LiveFeatureLevel(World& world, int64_t randomSeed)
                : m_world(world), m_random(randomSeed) {}

            // LevelReader.getBlockState: air outside the build height (MC's
            // VOID_AIR is air too) and in any chunk that is not resident —
            // the live world is never asked to load one for a feature.
            LibState* getBlockState(const LibPos& pos) const override {
                if (!Readable(pos)) return LibAir();
                return ToLib(m_world.GetBlockState(pos.getX(), pos.getY(), pos.getZ()));
            }

            // Level.setBlock(pos, state, flags) — the feature's own MC flags.
            bool setBlock(const LibPos& pos, LibState* state, int flags) override {
                if (!state || !Readable(pos)) return false;
                return m_world.SetBlock(ToVec(pos), ToGame(state), static_cast<uint32_t>(flags),
                                        World::kUpdateLimit);
            }

            bool isStateAtPosition(const LibPos& pos,
                                   std::function<bool(LibState*)> predicate) const override {
                return predicate(getBlockState(pos));
            }

            bool isFluidAtPosition(const LibPos& pos,
                                   std::function<bool(LibState*)> predicate) const override {
                LibState* state = getBlockState(pos);
                return state && state->hasAnyFluid() && predicate(state);
            }

            // The live world has no library chunk to hand out; every feature
            // that asks for one only uses it as a "loaded here?" test, which
            // hasChunkAt answers.
            ::world::IChunk* getChunk(int /*chunkX*/, int /*chunkZ*/) override { return nullptr; }

            bool hasChunkAt(const LibPos& pos) override {
                return m_world.IsChunkLoaded(pos.getX() >> 4, pos.getZ() >> 4);
            }

            // WorldGenRegion.getHeight convention: the first free Y above the
            // column's top matching block (MC Heightmap.getFirstAvailable).
            int getHeight(mlg::Heightmap::Types type, int x, int z) const override {
                if (!m_world.IsChunkLoaded(x >> 4, z >> 4)) return getMinY();
                return m_world.GetSurfaceHeight(x, z, ToGameHeightmap(type)) + 1;
            }

            // The library's convention: getMaxY is exclusive (min + height).
            int getMinY() const override { return World::MIN_Y; }
            int getMaxY() const override { return World::MAX_Y + 1; }

            bool hasSkyLight() const override { return DimensionHasSkyLight(m_world.GetDimension()); }

            const mc::world::biome::Biome* getBiome(const LibPos& pos) const override {
                return LibBiome(m_world.GetBiome(pos.getX(), pos.getY(), pos.getZ()));
            }

            int64_t getSeed() const override { return m_world.GetGenerationSeed(); }

            mc::XoroshiroRandomSource& getRandom() override { return m_random; }

            // Every resident chunk is writable; nothing else is.
            bool ensureCanWrite(const LibPos& pos) const override { return Readable(pos); }

            // LevelAccessor.scheduleTick(pos, block, delay) — fluids included:
            // their ticks ride the block-tick queue keyed on Water / Lava.
            void scheduleTick(const LibPos& pos, const std::string& blockName, int delay) override {
                ScheduledTickAccess* ticks = m_world.Ticks();
                if (!ticks || !Readable(pos)) return;
                BlockStateRegistry::Initialize();
                const BlockID block = BlockStateRegistry::CreateBlockState(blockName).resolvedId;
                if (block == BlockID::Air) return;
                ticks->ScheduleTick(ToVec(pos), block, delay);
            }

            // Level.destroyBlock(pos, dropResources): loot, then the cell's
            // fluid or air with flag 3.
            bool destroyBlock(const LibPos& pos, bool dropResources) override {
                if (!Readable(pos)) return false;
                return m_world.DestroyBlock(ToVec(pos), dropResources);
            }

            // StructureTemplate.updateShapeAtEdge, one face:
            //   state.updateShape(... direction, neighborPos, neighborState)
            //   → setBlock(pos, newState, flags & -2) when it changed;
            //   neighborState.updateShape(... opposite, pos, newState)
            //   → setBlock(neighborPos, …, flags & -2) when it changed.
            // Both states are read before either write, as vanilla does.
            bool updateShapeAtEdge(const LibPos& libPos, int sx, int sy, int sz, int flags) override {
                const glm::ivec3 pos = ToVec(libPos);
                const glm::ivec3 neighborPos = pos + glm::ivec3(sx, sy, sz);
                const Direction direction = DirectionOfStep(sx, sy, sz);
                const uint32_t writeFlags =
                    static_cast<uint32_t>(flags) & ~static_cast<uint32_t>(World::UpdateFlags::NotifyNeighbors);

                const bool posLive = Readable(libPos);
                const bool neighborLive = Readable(LibPos(neighborPos.x, neighborPos.y, neighborPos.z));
                const BlockState state = posLive ? m_world.GetBlockState(pos.x, pos.y, pos.z) : BlockState{};
                const BlockState neighborState =
                    neighborLive ? m_world.GetBlockState(neighborPos.x, neighborPos.y, neighborPos.z)
                                 : BlockState{};

                BlockState newState = state;
                if (posLive) {
                    newState = m_world.UpdateShape(state, direction, pos, neighborState);
                    if (newState != state) {
                        m_world.SetBlock(pos, newState, writeFlags, World::kUpdateLimit);
                    }
                }
                if (neighborLive) {
                    const BlockState newNeighborState =
                        m_world.UpdateShape(neighborState, Opposite(direction), neighborPos, newState);
                    if (newNeighborState != neighborState) {
                        m_world.SetBlock(neighborPos, newNeighborState, writeFlags, World::kUpdateLimit);
                    }
                }
                return true;
            }

        private:
            bool Readable(const LibPos& pos) const {
                return pos.getY() >= World::MIN_Y && pos.getY() <= World::MAX_Y &&
                       m_world.IsChunkLoaded(pos.getX() >> 4, pos.getZ() >> 4);
            }

            World&                    m_world;
            mc::XoroshiroRandomSource m_random;
        };

        // ── Feature registry (MC Registries.FEATURE, the keys growers name) ─

        struct NamedFeature {
            const char*              id;
            mlg::ConfiguredFeature** feature;
        };

        // Every configured feature the library carries (common/world/level/
        // ConfiguredFeatureIds.inc — the same list /place feature completes
        // from). Pointers-to-pointers: the statics are filled by the bootstrap.
        const NamedFeature kNamedFeatures[] = {
#define CONFIGURED_FEATURE(id, cls, name) {id, &mwf::cls::name},
#include "common/world/level/ConfiguredFeatureIds.inc"
#undef CONFIGURED_FEATURE
        };

        mlg::ConfiguredFeature* ResolveFeature(std::string_view id) {
            EnsureLibraryBootstrapped();
            for (const NamedFeature& f : kNamedFeatures) {
                if (id == f.id) return *f.feature;
            }
            // The Twilight Forest registers every configured feature under
            // its JSON id ("twilightforest:tree/canopy_tree").
            if (id.rfind("twilightforest:", 0) == 0) {
                return mwf::twilight::findConfigured(std::string(id));
            }
            return nullptr;
        }

        // The dimension's library chunk generator, when it has one — handed
        // to Feature.place as vanilla hands ServerChunkCache.getGenerator().
        mlg::ChunkGenerator* LibGeneratorOf(World& world) {
            ChunkProvider* provider = world.GetChunkProvider();
            if (!provider) return nullptr;
            auto* generator = dynamic_cast<MyTerrainGenerator*>(provider->GetGenerator());
            return generator ? generator->GetLibGenerator() : nullptr;
        }

        // A feature may reach this far from its origin. The widest growable
        // trees (mega jungle, the Twilight Forest's mega canopy) stay within
        // one chunk of the sapling; two leaves a margin, and a chunk this near
        // an active player is always resident.
        constexpr int kLoadedChunkRadius = 2;

        bool AreaLoaded(const World& world, const glm::ivec3& origin) {
            const int cx = origin.x >> 4;
            const int cz = origin.z >> 4;
            for (int dx = -kLoadedChunkRadius; dx <= kLoadedChunkRadius; ++dx) {
                for (int dz = -kLoadedChunkRadius; dz <= kLoadedChunkRadius; ++dz) {
                    if (!world.IsChunkLoaded(cx + dx, cz + dz)) return false;
                }
            }
            return true;
        }

        // ── LiveFeatures hooks ──────────────────────────────────────────────

        bool HookHasFeature(std::string_view featureId) { return HasLiveFeature(featureId); }

        bool HookPlace(ILevelWrite& level, std::string_view featureId, const glm::ivec3& origin,
                       JavaRandom& random) {
            World* world = dynamic_cast<World*>(&level);
            return world && PlaceLiveFeature(*world, featureId, origin, random);
        }

        // ServerLevel.sendBlockUpdated: queue the cell's current state for
        // this tick's broadcast, as World::SetBlock does under UPDATE_CLIENTS.
        void HookSendBlockUpdated(ILevelWrite& level, const glm::ivec3& pos) {
            World* world = dynamic_cast<World*>(&level);
            if (!world || !Server::g_integratedServer) return;
            if (!world->IsValidPosition(pos.x, pos.y, pos.z)) return;
            auto* accumulator = Server::g_integratedServer->GetChangeAccumulatorFor(*world);
            if (!accumulator) return;
            const Math::SectionPos sp = Math::SectionPos::fromWorldPos(pos.x, pos.y, pos.z);
            accumulator->accumulate(sp, static_cast<uint8_t>(pos.x & 0xF), static_cast<uint8_t>(pos.y & 0xF),
                                    static_cast<uint8_t>(pos.z & 0xF),
                                    world->GetBlockState(pos.x, pos.y, pos.z));
        }

    } // namespace

    bool HasLiveFeature(std::string_view featureId) {
        return ResolveFeature(featureId) != nullptr;
    }

    bool PlaceLiveFeature(World& world, std::string_view featureId, const glm::ivec3& origin,
                          JavaRandom& random) {
        mlg::ConfiguredFeature* feature = ResolveFeature(featureId);
        if (!feature) return false;
        if (!AreaLoaded(world, origin)) return false;

        // Feature.place(level, generator, random, origin). The level random
        // (LevelAccessor.getRandom) and the placement random are both drawn
        // from the caller's level random; MC shares one stream, which no
        // player can tell apart from two derived ones.
        LiveFeatureLevel level(world, random.NextLong());
        mlg::ChunkGenerator* generator = LibGeneratorOf(world);
        if (generator) level.setSeaLevel(generator->getSeaLevel());
        mlg::WorldgenRandom placeRandom(mc::LegacyRandomSource(random.NextLong()));

        try {
            return feature->place(&level, generator, placeRandom,
                                  LibPos(origin.x, origin.y, origin.z));
        } catch (const std::exception& e) {
            Log::Error("[LiveFeatures] %.*s at (%d, %d, %d) failed: %s",
                       static_cast<int>(featureId.size()), featureId.data(),
                       origin.x, origin.y, origin.z, e.what());
            return false;
        }
    }

    void InstallLiveFeaturePlacement() {
        LiveFeatures::Hooks hooks;
        hooks.hasFeature       = &HookHasFeature;
        hooks.place            = &HookPlace;
        hooks.sendBlockUpdated = &HookSendBlockUpdated;
        LiveFeatures::SetHooks(hooks);
    }

} // namespace Game

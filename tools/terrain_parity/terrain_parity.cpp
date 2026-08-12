// Terrain parity check.
//
// Generates a fixed grid of chunks from a fixed seed and prints a hash of every
// block state produced. Two runs that print the same hashes generated bit-identical
// terrain.
//
// This exists to make optimisations to the vendored terrain library falsifiable.
// A change like short-circuiting DensityFunction::mapAll is supposed to be purely
// an allocation optimisation — same graph, same values — but a mistake there does
// not crash, it silently produces different terrain. This turns "I believe it is
// equivalent" into "the hashes match".
//
// Usage:
//     terrain_parity [--seed N] [--radius N] [--quiet]
//
// Workflow:
//     terrain_parity > /tmp/before.txt     # BEFORE the change
//     ...apply change, rebuild...
//     terrain_parity > /tmp/after.txt
//     diff /tmp/before.txt /tmp/after.txt  # must be empty
//
// Deliberately single-threaded: both executors run tasks inline, so the whole
// pipeline is deterministic and there is no scheduling nondeterminism to chase if
// hashes ever disagree. It is also why this links terrain_library alone and does
// not touch game code.

#include "world/level/block/Blocks.h"
#include "world/level/block/state/BlockState.h"
#include "world/LevelChunkSection.h"   // world::BlockRegistry lives here
#include "world/IChunk.h"
#include "world/ChunkPos.h"
#include "world/chunk/status/ChunkStatus.h"
#include "world/biome/MultiNoiseBiomeSource.h"
#include "world/biome/OverworldBiomeBuilder.h"
#include "levelgen/NoiseRegistry.h"
#include "levelgen/DensityFunctionRegistry.h"
#include "levelgen/NoiseSettings.h"
#include "levelgen/NoiseGeneratorSettings.h"
#include "levelgen/NoiseRouterData.h"
#include "levelgen/RandomState.h"
#include "levelgen/SurfaceRuleData.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/FluidPicker.h"
#include "server/level/ServerChunkCache.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace mcl = minecraft::levelgen;
namespace mcw = minecraft::world;

// Must match MyTerrainGenerator.cpp — a different world height would generate
// different terrain and make the comparison meaningless.
static constexpr int MIN_Y = -64;
static constexpr int HEIGHT = 384;

static uint64_t fnv1a(const char* data, size_t len, uint64_t h = 1469598103934665603ULL) {
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<unsigned char>(data[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

int main(int argc, char** argv) {
    int64_t seed = 12345;
    int radius = 2;          // (2*radius+1)^2 chunks
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc)        seed = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--radius") && i + 1 < argc) radius = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--quiet"))                  quiet = true;
        else {
            fprintf(stderr, "usage: %s [--seed N] [--radius N] [--quiet]\n", argv[0]);
            return 2;
        }
    }

    // The library reads block tags and structure NBT relative to MC_DATA_ROOT;
    // without it those loads fail and generation silently differs. Same variable
    // PlatformMain sets from the app bundle.
    if (!getenv("MC_DATA_ROOT")) setenv("MC_DATA_ROOT", "data", 0);

    printf("# terrain parity  seed=%lld radius=%d  (%d chunks)\n",
           (long long)seed, radius, (2 * radius + 1) * (2 * radius + 1));

    // ---- setup: mirrors MyTerrainGenerator::Initialize ----
    mcw::level::block::Blocks::bootstrap();
    mcl::NoiseRegistry::bootstrap();
    mcl::DensityFunctionRegistry::bootstrap(seed);
    mcl::SurfaceRuleData::initialize();

    using mcw::level::block::Blocks;
    auto* airBlock   = Blocks::AIR->defaultBlockState();
    auto* stoneBlock = Blocks::STONE->defaultBlockState();

    auto* blockRegistry = new mcw::BlockRegistry();
    for (auto* s : { airBlock, stoneBlock,
                     Blocks::WATER->defaultBlockState(),
                     Blocks::LAVA->defaultBlockState(),
                     Blocks::DEEPSLATE->defaultBlockState(),
                     Blocks::BEDROCK->defaultBlockState(),
                     Blocks::GRASS_BLOCK->defaultBlockState(),
                     Blocks::DIRT->defaultBlockState(),
                     Blocks::SAND->defaultBlockState(),
                     Blocks::GRAVEL->defaultBlockState(),
                     Blocks::TUFF->defaultBlockState() }) {
        blockRegistry->registerBlock(s);
    }

    auto* router = mcl::NoiseRouterData::overworld(false, false);
    auto noiseSettings = mcl::NoiseSettings::OVERWORLD_NOISE_SETTINGS;

    auto spawnTargetStorage = mcw::biome::OverworldBiomeBuilder().spawnTarget();
    std::vector<mcl::ClimateParameterPoint*> spawnTargetPtrs;
    spawnTargetPtrs.reserve(spawnTargetStorage.size());
    for (auto& point : spawnTargetStorage) {
        spawnTargetPtrs.push_back(reinterpret_cast<mcl::ClimateParameterPoint*>(&point));
    }

    auto* settings = new mcl::NoiseGeneratorSettings(
        noiseSettings, stoneBlock, Blocks::WATER->defaultBlockState(),
        *router, nullptr, spawnTargetPtrs, 63, false, true, true, false);

    auto* randomState  = mcl::RandomState::create(settings, seed);
    auto* surfaceRules = mcl::SurfaceRuleData::overworld();
    auto* fluidPicker  = new mcl::OverworldFluidPicker(
        63, -54, Blocks::WATER->defaultBlockState(), Blocks::LAVA->defaultBlockState());

    auto biomeSource = mcw::biome::MultiNoiseBiomeSource::createOverworld();

    auto* generator = new mcl::NoiseBasedChunkGenerator(
        settings, randomState->surfaceSystem(), surfaceRules,
        stoneBlock, airBlock, fluidPicker, nullptr);
    generator->setBiomeSource(biomeSource.get());

    // Inline executors: run every task on the calling thread, immediately. Keeps
    // the run deterministic and removes the sleep-poll path in getChunk.
    auto inlineExec = [](std::function<void()> task) { task(); };

    minecraft::server::level::ServerChunkCache cache(
        generator, randomState, seed,
        inlineExec, inlineExec,
        blockRegistry, airBlock, stoneBlock,
        MIN_Y, HEIGHT);

    const auto& target = minecraft::world::chunk::status::ChunkStatus::FULL;

    // BlockState* is not stable across runs, so identity comes from the full
    // state string (block name + properties) hashed once per distinct state.
    std::unordered_map<void*, uint64_t> stateIds;
    auto stateId = [&](minecraft::world::level::block::state::BlockState* s) -> uint64_t {
        if (!s) return 0;
        auto it = stateIds.find(s);
        if (it != stateIds.end()) return it->second;
        const std::string str = s->toStateString();
        const uint64_t id = fnv1a(str.data(), str.size());
        stateIds.emplace(s, id);
        return id;
    };

    uint64_t overall = 1469598103934665603ULL;
    int generated = 0, failed = 0;

    for (int cz = -radius; cz <= radius; ++cz) {
        for (int cx = -radius; cx <= radius; ++cx) {
            auto* chunk = cache.getChunk(cx, cz, target, true);
            if (!chunk) {
                printf("chunk %4d %4d  FAILED\n", cx, cz);
                ++failed;
                continue;
            }

            uint64_t h = 1469598103934665603ULL;
            const int sections = chunk->getSectionsCount();
            for (int si = 0; si < sections; ++si) {
                auto& sec = chunk->getSection(si);
                for (int y = 0; y < 16; ++y)
                    for (int z = 0; z < 16; ++z)
                        for (int x = 0; x < 16; ++x)
                            h = mix(h, stateId(sec.getBlockState(x, y, z)));
            }

            if (!quiet) printf("chunk %4d %4d  %016llx\n", cx, cz, (unsigned long long)h);
            overall = mix(overall, h);
            ++generated;
        }
    }

    printf("# chunks=%d failed=%d distinct_states=%zu\n",
           generated, failed, stateIds.size());
    printf("OVERALL %016llx\n", (unsigned long long)overall);

    // Non-zero on failure so this can gate a script.
    return failed ? 1 : 0;
}

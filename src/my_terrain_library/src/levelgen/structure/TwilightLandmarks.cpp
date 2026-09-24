#include "levelgen/structure/TwilightLandmarks.h"

#include "random/LegacyRandomSource.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

// Twilight Forest 4.9 — util/landmarks/LegacyLandmarkPlacements.java, line
// for line. Java long arithmetic wraps; it is done in uint64_t here and
// reinterpreted, which is the same two's-complement result.

namespace minecraft {
namespace levelgen {
namespace structure {
namespace twilight_landmarks {

namespace {

int64_t wrapMul(int64_t a, int64_t b) {
    return static_cast<int64_t>(static_cast<uint64_t>(a) * static_cast<uint64_t>(b));
}
int64_t wrapAdd(int64_t a, int64_t b) {
    return static_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b));
}

// VARIETY_LANDMARKS (WeightedList, total 16): small hill 6, medium hill 3,
// large hill 1, hedge maze 2, naga courtyard 2, lich tower 2 — in builder
// order, which is the order WeightedList.getRandom walks.
struct WeightedLandmark {
    const char* id;
    int32_t weight;
};
constexpr WeightedLandmark kVarietyLandmarks[] = {
    {"twilightforest:small_hollow_hill", 6},
    {"twilightforest:medium_hollow_hill", 3},
    {"twilightforest:large_hollow_hill", 1},
    {"twilightforest:hedge_maze", 2},
    {"twilightforest:naga_courtyard", 2},
    {"twilightforest:lich_tower", 2},
};
constexpr int32_t kVarietyTotalWeight = 16;

} // namespace

int32_t javaRoundFloat(float value) {
    return static_cast<int32_t>(std::floor(value + 0.5f));
}

core::BlockPos getNearestCenterXZ(int32_t chunkX, int32_t chunkZ, int32_t height) {
    // generate random number for the whole biome area
    const int32_t regionX = (chunkX + 8) >> 4;
    const int32_t regionZ = (chunkZ + 8) >> 4;

    int64_t seed = wrapMul(regionX, 3129871LL) ^ wrapMul(regionZ, 116129781LL);
    seed = wrapAdd(wrapMul(wrapMul(seed, seed), 42317861LL), wrapMul(seed, 7LL));

    const int32_t num0 = static_cast<int32_t>((seed >> 12) & 3LL);
    const int32_t num1 = static_cast<int32_t>((seed >> 15) & 3LL);
    const int32_t num2 = static_cast<int32_t>((seed >> 18) & 3LL);
    const int32_t num3 = static_cast<int32_t>((seed >> 21) & 3LL);

    // slightly randomize center of biome (+/- 3)
    const int32_t centerX = 8 + num0 - num1;
    const int32_t centerZ = 8 + num2 - num3;

    // centers are offset strangely depending on +/-
    int32_t ccz;
    if (regionZ >= 0) {
        ccz = (regionZ * 16 + centerZ - 8) * 16 + 8;
    } else {
        ccz = (regionZ * 16 + (16 - centerZ) - 8) * 16 + 9;
    }
    int32_t ccx;
    if (regionX >= 0) {
        ccx = (regionX * 16 + centerX - 8) * 16 + 8;
    } else {
        ccx = (regionX * 16 + (16 - centerX) - 8) * 16 + 9;
    }
    return core::BlockPos(ccx, height, ccz);
}

bool chunkHasLandmarkCenter(int32_t chunkX, int32_t chunkZ) {
    const core::BlockPos nearestCenter = getNearestCenterXZ(chunkX, chunkZ);
    return chunkX == (nearestCenter.getX() >> 4) && chunkZ == (nearestCenter.getZ() >> 4);
}

bool blockIsInLandmarkCenter(int32_t blockX, int32_t blockZ) {
    return chunkHasLandmarkCenter(blockX >> 4, blockZ >> 4);
}

bool blockNearLandmarkCenter(int32_t blockX, int32_t blockZ, int32_t range) {
    for (int32_t x = -range; x <= range; ++x) {
        for (int32_t z = -range; z <= range; ++z) {
            // Java: blockX >> 4 + x == blockX >> (4 + x). Java masks an int
            // shift count to 5 bits; do the same so negative x stays defined.
            const int32_t sx = (4 + x) & 31;
            const int32_t sz = (4 + z) & 31;
            if (chunkHasLandmarkCenter(blockX >> sx, blockZ >> sz)) return true;
        }
    }
    return false;
}

int32_t manhattanDistanceFromLandmarkCenter(int32_t chunkX, int32_t chunkZ) {
    const core::BlockPos nearestCenter = getNearestCenterXZ(chunkX, chunkZ);
    const int32_t deltaChunkX = std::abs(chunkX - (nearestCenter.getX() >> 4));
    const int32_t deltaChunkZ = std::abs(chunkZ - (nearestCenter.getZ() >> 4));
    return deltaChunkX + deltaChunkZ;
}

std::string pickVarietyLandmark(int32_t chunkX, int32_t chunkZ, int64_t worldSeed) {
    // set the chunkX and chunkZ to the center of the biome in case they arent already
    chunkX = javaRoundFloat(static_cast<float>(chunkX) / 16.0f) * 16;
    chunkZ = javaRoundFloat(static_cast<float>(chunkZ) / 16.0f) * 16;

    // Java: Math.abs((chunkX + 64 >> 4) % 8) — shift binds looser than +,
    // % keeps the dividend's sign.
    const int32_t regionOffsetX = std::abs(((chunkX + 64) >> 4) % 8);
    const int32_t regionOffsetZ = std::abs(((chunkZ + 64) >> 4) % 8);

    // plant two lich towers near the center of each 2048x2048 map area
    if ((regionOffsetX == 4 && regionOffsetZ == 5) || (regionOffsetX == 4 && regionOffsetZ == 3)) {
        return "twilightforest:lich_tower";
    }
    // also two nagas
    if ((regionOffsetX == 5 && regionOffsetZ == 4) || (regionOffsetX == 3 && regionOffsetZ == 4)) {
        return "twilightforest:naga_courtyard";
    }

    // VARIETY_LANDMARKS.getRandom(new LegacyRandomSource(seed + chunkX *
    // 25117L + chunkZ * 151121L)): one nextInt(totalWeight), then the
    // cumulative-weight walk (WeightedList Flat/Compact select the same).
    const int64_t seed = wrapAdd(wrapAdd(worldSeed, wrapMul(chunkX, 25117LL)), wrapMul(chunkZ, 151121LL));
    LegacyRandomSource random(seed);
    int32_t selection = random.nextInt(kVarietyTotalWeight);
    for (const WeightedLandmark& entry : kVarietyLandmarks) {
        selection -= entry.weight;
        if (selection < 0) return entry.id;
    }
    return "twilightforest:small_hollow_hill";
}

std::string pickLandmarkForChunk(int32_t chunkX, int32_t chunkZ, int64_t worldSeed,
                                 const std::function<std::string(int32_t, int32_t, int32_t)>& biomeAt) {
    // set the chunkX and chunkZ to the center of the biome
    chunkX = javaRoundFloat(static_cast<float>(chunkX) / 16.0f) * 16;
    chunkZ = javaRoundFloat(static_cast<float>(chunkZ) / 16.0f) * 16;

    if (biomeAt) {
        const std::string biome = biomeAt((chunkX << 4) + 8, 0, (chunkZ << 4) + 8);
        // BIOME_2_STRUCTURES (quest_island is commented out of the mod's
        // structure sets; it still wins the lake here, as in the mod).
        struct Pair { const char* biome; const char* structure; };
        static constexpr Pair kBiomeStructures[] = {
            {"twilightforest:enchanted_forest", "twilightforest:quest_grove"},
            {"twilightforest:lake", "twilightforest:quest_island"},
            {"twilightforest:swamp", "twilightforest:labyrinth"},
            {"twilightforest:fire_swamp", "twilightforest:hydra_lair"},
            {"twilightforest:dark_forest", "twilightforest:knight_stronghold"},
            {"twilightforest:dark_forest_center", "twilightforest:dark_tower"},
            {"twilightforest:snowy_forest", "twilightforest:yeti_cave"},
            {"twilightforest:glacier", "twilightforest:aurora_palace"},
            {"twilightforest:highlands", "twilightforest:troll_cave"},
            {"twilightforest:final_plateau", "twilightforest:final_castle"},
        };
        for (const Pair& pair : kBiomeStructures) {
            if (biome == pair.biome) return pair.structure;
        }
    }
    return pickVarietyLandmark(chunkX, chunkZ, worldSeed);
}

} // namespace twilight_landmarks
} // namespace structure
} // namespace levelgen
} // namespace minecraft

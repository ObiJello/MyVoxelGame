#include "world/biome/BiomeManager.h"
#include "util/LinearCongruentialGenerator.h"
#include "util/SHA256.h"
#include "math/Mth.h"
#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <iostream>

namespace minecraft {
namespace world {
namespace biome {

// Reference: BiomeManager.java lines 18-21
BiomeManager::BiomeManager(const NoiseBiomeSource* noiseBiomeSource, int64_t seed)
    : m_noiseBiomeSource(noiseBiomeSource)
    , m_biomeZoomSeed(seed)
{
}

// Reference: BiomeManager.java lines 23-25
int64_t BiomeManager::obfuscateSeed(int64_t seed) {
    // Reference: Hashing.sha256().hashLong(seed).asLong()
    // Uses Google Guava's SHA-256 hashing
    return util::SHA256::hashLong(seed);
}

// Reference: BiomeManager.java lines 27-29
BiomeManager BiomeManager::withDifferentSource(const NoiseBiomeSource* biomeSource) const {
    return BiomeManager(biomeSource, m_biomeZoomSeed);
}

namespace {

// Direct-mapped, per worker thread. The surface rules ask for the biome of
// nearly every block of a column; walking 16x16 columns top to bottom touches
// ~6x6x100 corners per chunk, each wanted by up to 512 blocks.
constexpr int kFiddleCacheBits = 11;

struct FiddleCache {
    int64_t seed;
    bool primed;
    uint8_t valid[1 << kFiddleCacheBits];
    BiomeManager::CornerFiddle entries[1 << kFiddleCacheBits];
    // The eight corners of the last cell asked for, in getBiome's corner
    // order. A column walk asks for the same cell 3 times in 4, and every
    // block of a 4x4x4 cell reads the same corners.
    bool cellValid;
    int32_t cellX, cellY, cellZ;
    BiomeManager::CornerFiddle cellCorners[8];
};

thread_local FiddleCache t_fiddleCache;

// The calling thread's cache for `seed`, emptied when the seed changes.
// Fetched once per getBiome: every thread_local access is a call on Darwin
// (_tlv_get_addr), which cost as much as the LCG it saved when made per corner.
FiddleCache& fiddleCacheFor(int64_t seed) {
    FiddleCache& cache = t_fiddleCache;
    if (!cache.primed || cache.seed != seed) {
        std::fill(std::begin(cache.valid), std::end(cache.valid), uint8_t{0});
        cache.cellValid = false;
        cache.seed = seed;
        cache.primed = true;
    }
    return cache;
}

BiomeManager::CornerFiddle cornerFiddle(FiddleCache& cache, int32_t x, int32_t y, int32_t z) {
    const uint32_t hash = static_cast<uint32_t>(x) * 0x9E3779B1u
                        ^ static_cast<uint32_t>(y) * 0x85EBCA77u
                        ^ static_cast<uint32_t>(z) * 0xC2B2AE3Du;
    const uint32_t slot = hash >> (32 - kFiddleCacheBits);
    BiomeManager::CornerFiddle& entry = cache.entries[slot];
    if (!cache.valid[slot] || entry.x != x || entry.y != y || entry.z != z) {
        entry = BiomeManager::computeCornerFiddle(cache.seed, x, y, z);
        cache.valid[slot] = 1;
    }
    return entry;
}

} // namespace

// Reference: BiomeManager.java lines 31-65
BiomeHolder BiomeManager::getBiome(const core::BlockPos& pos) const {
    // Reference: BiomeManager.java lines 32-34
    int32_t absX = pos.getX() - 2;
    int32_t absY = pos.getY() - 2;
    int32_t absZ = pos.getZ() - 2;

    // Reference: BiomeManager.java lines 35-37
    int32_t parentX = absX >> 2;  // Divide by 4
    int32_t parentY = absY >> 2;
    int32_t parentZ = absZ >> 2;

    // Reference: BiomeManager.java lines 38-40
    // CRITICAL: Java casts to (double)4.0F - must match exactly
    double fractX = static_cast<double>(absX & 3) / static_cast<double>(4.0F);
    double fractY = static_cast<double>(absY & 3) / static_cast<double>(4.0F);
    double fractZ = static_cast<double>(absZ & 3) / static_cast<double>(4.0F);

    // Reference: BiomeManager.java lines 41-42
    int32_t minI = 0;
    double minFiddledDistance = std::numeric_limits<double>::infinity();

    // Reference: BiomeManager.java lines 44-59
    // Loop through 8 corners of the cube. A corner's fiddle offsets depend on
    // the seed and the corner alone, and every block of the 4x4x4 cell (and
    // of the seven cells sharing the corner) reads the same ones: they come
    // from a per-thread corner cache. The distance sum below is Java's.
    // Copied out of the corner cache: two corners of one cell may share a slot.
    FiddleCache& cache = fiddleCacheFor(m_biomeZoomSeed);
    if (!cache.cellValid || cache.cellX != parentX || cache.cellY != parentY || cache.cellZ != parentZ) {
        for (int32_t i = 0; i < 8; ++i) {
            cache.cellCorners[i] = cornerFiddle(cache,
                                                (i & 4) == 0 ? parentX : parentX + 1,
                                                (i & 2) == 0 ? parentY : parentY + 1,
                                                (i & 1) == 0 ? parentZ : parentZ + 1);
        }
        cache.cellX = parentX;
        cache.cellY = parentY;
        cache.cellZ = parentZ;
        cache.cellValid = true;
    }
    const CornerFiddle* corners = cache.cellCorners;
    for (int32_t i = 0; i < 8; ++i) {
        // Reference: BiomeManager.java lines 45-47
        bool xEven = (i & 4) == 0;
        bool yEven = (i & 2) == 0;
        bool zEven = (i & 1) == 0;

        // Reference: BiomeManager.java lines 51-53
        // CRITICAL: Java uses (double)1.0F
        double distanceX = xEven ? fractX : fractX - static_cast<double>(1.0F);
        double distanceY = yEven ? fractY : fractY - static_cast<double>(1.0F);
        double distanceZ = zEven ? fractZ : fractZ - static_cast<double>(1.0F);

        // Reference: BiomeManager.java line 54 (getFiddledDistance line 97)
        const CornerFiddle& c = corners[i];
        double next = Mth::square(distanceZ + c.fiddleZ) +
                      Mth::square(distanceY + c.fiddleY) +
                      Mth::square(distanceX + c.fiddleX);

        // Reference: BiomeManager.java lines 55-58
        if (minFiddledDistance > next) {
            minI = i;
            minFiddledDistance = next;
        }
    }

    // Reference: BiomeManager.java lines 61-63
    int32_t biomeX = (minI & 4) == 0 ? parentX : parentX + 1;
    int32_t biomeY = (minI & 2) == 0 ? parentY : parentY + 1;
    int32_t biomeZ = (minI & 1) == 0 ? parentZ : parentZ + 1;

    // Reference: BiomeManager.java line 64
    return m_noiseBiomeSource->getNoiseBiome(biomeX, biomeY, biomeZ);
}

// Reference: BiomeManager.java lines 67-72
BiomeHolder BiomeManager::getNoiseBiomeAtPosition(double x, double y, double z) const {
    int32_t quartX = Mth::quartFromBlock(Mth::floor(x));
    int32_t quartY = Mth::quartFromBlock(Mth::floor(y));
    int32_t quartZ = Mth::quartFromBlock(Mth::floor(z));
    return getNoiseBiomeAtQuart(quartX, quartY, quartZ);
}

// Reference: BiomeManager.java lines 74-79
BiomeHolder BiomeManager::getNoiseBiomeAtPosition(const core::BlockPos& blockPos) const {
    int32_t quartX = Mth::quartFromBlock(blockPos.getX());
    int32_t quartY = Mth::quartFromBlock(blockPos.getY());
    int32_t quartZ = Mth::quartFromBlock(blockPos.getZ());
    return getNoiseBiomeAtQuart(quartX, quartY, quartZ);
}

// Reference: BiomeManager.java lines 81-83
BiomeHolder BiomeManager::getNoiseBiomeAtQuart(int32_t quartX, int32_t quartY, int32_t quartZ) const {
    return m_noiseBiomeSource->getNoiseBiome(quartX, quartY, quartZ);
}

// Reference: BiomeManager.java lines 85-96 (getFiddledDistance up to the
// distance sum, which getBiome applies)
BiomeManager::CornerFiddle BiomeManager::computeCornerFiddle(int64_t seed, int32_t xRandom, int32_t yRandom,
                                                             int32_t zRandom) {
    // Reference: BiomeManager.java lines 86-91
    // Mix coordinates into seed using LCG
    int64_t rval = util::LinearCongruentialGenerator::next(seed, static_cast<int64_t>(xRandom));
    rval = util::LinearCongruentialGenerator::next(rval, static_cast<int64_t>(yRandom));
    rval = util::LinearCongruentialGenerator::next(rval, static_cast<int64_t>(zRandom));
    rval = util::LinearCongruentialGenerator::next(rval, static_cast<int64_t>(xRandom));
    rval = util::LinearCongruentialGenerator::next(rval, static_cast<int64_t>(yRandom));
    rval = util::LinearCongruentialGenerator::next(rval, static_cast<int64_t>(zRandom));

    // Reference: BiomeManager.java lines 92-96
    // Generate random offsets
    CornerFiddle corner;
    corner.x = xRandom;
    corner.y = yRandom;
    corner.z = zRandom;
    corner.fiddleX = getFiddle(rval);
    rval = util::LinearCongruentialGenerator::next(rval, seed);
    corner.fiddleY = getFiddle(rval);
    rval = util::LinearCongruentialGenerator::next(rval, seed);
    corner.fiddleZ = getFiddle(rval);
    return corner;
}

// Reference: BiomeManager.java lines 100-103
double BiomeManager::getFiddle(int64_t rval) {
    // Reference: BiomeManager.java line 101
    // CRITICAL: Java uses (double)1024.0F
    double uniform = static_cast<double>(((rval >> 24) % 1024 + 1024) % 1024) / static_cast<double>(1024.0F);

    // Reference: BiomeManager.java line 102
    // CRITICAL: Java uses (double)0.5F
    return (uniform - static_cast<double>(0.5F)) * 0.9;
}

} // namespace biome
} // namespace world
} // namespace minecraft

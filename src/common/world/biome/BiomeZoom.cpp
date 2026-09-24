// File: src/common/world/biome/BiomeZoom.cpp
//
// Transcribed from MC BiomeManager.java (getBiome, getFiddledDistance,
// getFiddle) and LinearCongruentialGenerator.java. Java's long arithmetic
// wraps; it is done in uint64_t here so the wrap is defined behaviour, and
// converted back to int64_t wherever Java's sign matters (the >> in getFiddle
// is an arithmetic shift).
#include "common/world/biome/BiomeZoom.hpp"

#include "util/SHA256.h"   // terrain library: Guava's sha256().hashLong

#include <limits>

namespace Game::BiomeZoom {

    namespace {

        // MC LinearCongruentialGenerator.next.
        int64_t LcgNext(int64_t rval, int64_t c) {
            constexpr uint64_t kMultiplier = 6364136223846793005ULL;
            constexpr uint64_t kIncrement  = 1442695040888963407ULL;
            uint64_t r = static_cast<uint64_t>(rval);
            r *= r * kMultiplier + kIncrement;
            r += static_cast<uint64_t>(c);
            return static_cast<int64_t>(r);
        }

        // MC BiomeManager.getFiddle: Math.floorMod(rval >> 24, 1024) / 1024.0,
        // centred and scaled to +-0.45. floorMod by a power of two is the low
        // bits of the two's-complement value, negative inputs included.
        double Fiddle(int64_t rval) {
            const double uniform = static_cast<double>((rval >> 24) & 1023) / 1024.0;
            return (uniform - 0.5) * 0.9;
        }

        // MC BiomeManager.getFiddledDistance.
        double FiddledDistance(int64_t seed, int xRandom, int yRandom, int zRandom,
                               double distanceX, double distanceY, double distanceZ) {
            int64_t rval = seed;
            rval = LcgNext(rval, xRandom);
            rval = LcgNext(rval, yRandom);
            rval = LcgNext(rval, zRandom);
            rval = LcgNext(rval, xRandom);
            rval = LcgNext(rval, yRandom);
            rval = LcgNext(rval, zRandom);
            const double fiddleX = Fiddle(rval);
            rval = LcgNext(rval, seed);
            const double fiddleY = Fiddle(rval);
            rval = LcgNext(rval, seed);
            const double fiddleZ = Fiddle(rval);
            const double dz = distanceZ + fiddleZ;
            const double dy = distanceY + fiddleY;
            const double dx = distanceX + fiddleX;
            return dz * dz + dy * dy + dx * dx;
        }

    } // namespace

    int64_t ObfuscateSeed(int64_t worldSeed) {
        return minecraft::util::SHA256::hashLong(worldSeed);
    }

    glm::ivec3 NoiseQuartAt(int64_t zoomSeed, int x, int y, int z) {
        const int absX = x - 2;
        const int absY = y - 2;
        const int absZ = z - 2;
        const int parentX = absX >> 2;
        const int parentY = absY >> 2;
        const int parentZ = absZ >> 2;
        const double fractX = static_cast<double>(absX & 3) / 4.0;
        const double fractY = static_cast<double>(absY & 3) / 4.0;
        const double fractZ = static_cast<double>(absZ & 3) / 4.0;

        int minI = 0;
        double minFiddledDistance = std::numeric_limits<double>::infinity();
        for (int i = 0; i < 8; ++i) {
            const bool xEven = (i & 4) == 0;
            const bool yEven = (i & 2) == 0;
            const bool zEven = (i & 1) == 0;
            const int cornerX = xEven ? parentX : parentX + 1;
            const int cornerY = yEven ? parentY : parentY + 1;
            const int cornerZ = zEven ? parentZ : parentZ + 1;
            const double distanceX = xEven ? fractX : fractX - 1.0;
            const double distanceY = yEven ? fractY : fractY - 1.0;
            const double distanceZ = zEven ? fractZ : fractZ - 1.0;
            const double next = FiddledDistance(zoomSeed, cornerX, cornerY, cornerZ,
                                                distanceX, distanceY, distanceZ);
            if (minFiddledDistance > next) {
                minI = i;
                minFiddledDistance = next;
            }
        }

        return glm::ivec3((minI & 4) == 0 ? parentX : parentX + 1,
                          (minI & 2) == 0 ? parentY : parentY + 1,
                          (minI & 1) == 0 ? parentZ : parentZ + 1);
    }

} // namespace Game::BiomeZoom

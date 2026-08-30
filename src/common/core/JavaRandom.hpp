// File: src/common/core/JavaRandom.hpp
// Bit-exact port of java.util.Random's LCG — MC's RandomSource in every place
// that matters for parity. Originally written client-side for the star field
// (SkyRenderer.buildStars consumes the RNG even for rejected stars, so any
// deviation desynchronizes every star after it); promoted here when the loot
// system needed a seeded RNG on the server too.
//
// Bit-exactness is the point: nextInt's rejection loop and nextFloat's 24-bit
// truncation are what make a given seed produce MC's exact sequence. Do not
// "simplify" them into a modulo or a division by RAND_MAX.
#pragma once

#include <cmath>
#include <cstdint>

namespace Game {

    class JavaRandom {
    public:
        explicit JavaRandom(int64_t seed) { SetSeed(seed); }

        void SetSeed(int64_t seed) {
            m_seed = (static_cast<uint64_t>(seed) ^ kMultiplier) & kMask48;
            // java.util.Random.setSeed also clears the cached spare gaussian.
            m_haveNextGaussian = false;
        }

        // java.util.Random.next(bits): 48-bit LCG step, top bits returned.
        int32_t Next(int bits) {
            m_seed = (m_seed * kMultiplier + kAddend) & kMask48;
            return static_cast<int32_t>(static_cast<int64_t>(m_seed) >> (48 - bits));
        }

        // java.util.Random.nextInt(bound): power-of-two fast path plus the
        // rejection loop that keeps the distribution uniform.
        int32_t NextInt(int32_t bound) {
            if (bound <= 0) return 0;
            if ((bound & -bound) == bound) {   // power of two
                return static_cast<int32_t>((static_cast<int64_t>(bound) * Next(31)) >> 31);
            }
            int32_t bits, value;
            do {
                bits  = Next(31);
                value = bits % bound;
            } while (bits - value + (bound - 1) < 0);
            return value;
        }

        // MC RandomSource.nextInt(min, max) — inclusive on both ends
        // (RandomSource.java: nextInt(max - min + 1) + min).
        int32_t NextInt(int32_t minInclusive, int32_t maxInclusive) {
            if (maxInclusive <= minInclusive) return minInclusive;
            return NextInt(maxInclusive - minInclusive + 1) + minInclusive;
        }

        float NextFloat() {
            return Next(24) / static_cast<float>(1 << 24);
        }

        double NextDouble() {
            return ((static_cast<int64_t>(Next(26)) << 27) + Next(27))
                   / static_cast<double>(1LL << 53);
        }

        bool NextBool() { return Next(1) != 0; }

        // MC RandomSource.triangle(mode, deviation) — the sum of two uniform
        // draws gives a triangular distribution centred on `mode`. Draw order
        // (two NextDoubles) is part of the stream, so this is a method rather
        // than an ad-hoc expression at call sites.
        double Triangle(double mode, double deviation) {
            return mode + deviation * (NextDouble() - NextDouble());
        }

        // java.util.Random.nextGaussian — Marsaglia polar method with the
        // cached-spare optimisation, exactly Java's (MC's LegacyRandomSource
        // delegates to the same MarsagliaPolarGaussian). The draw ORDER (two
        // NextDoubles per rejection round, spare consumed first) is part of
        // the stream, so this must stay byte-for-byte Java's algorithm.
        double NextGaussian() {
            if (m_haveNextGaussian) {
                m_haveNextGaussian = false;
                return m_nextGaussian;
            }
            double v1, v2, s;
            do {
                v1 = 2.0 * NextDouble() - 1.0;
                v2 = 2.0 * NextDouble() - 1.0;
                s = v1 * v1 + v2 * v2;
            } while (s >= 1.0 || s == 0.0);
            const double multiplier = std::sqrt(-2.0 * std::log(s) / s);
            m_nextGaussian = v2 * multiplier;
            m_haveNextGaussian = true;
            return v1 * multiplier;
        }

    private:
        static constexpr uint64_t kMultiplier = 0x5DEECE66DULL;
        static constexpr uint64_t kAddend = 0xBULL;
        static constexpr uint64_t kMask48 = (1ULL << 48) - 1;

        uint64_t m_seed = 0;
        // nextGaussian's cached spare (java.util.Random.haveNextNextGaussian).
        double m_nextGaussian = 0.0;
        bool   m_haveNextGaussian = false;
    };

} // namespace Game

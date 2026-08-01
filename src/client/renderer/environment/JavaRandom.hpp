// File: src/client/renderer/environment/JavaRandom.hpp
// Bit-exact port of java.util.Random's LCG. Needed so the star field built
// from RandomSource.create(10842L) matches Minecraft's sky star-for-star
// (SkyRenderer.buildStars consumes the RNG even for rejected stars, so any
// deviation desynchronizes every star after it).
#pragma once

#include <cstdint>

namespace Render {

    class JavaRandom {
    public:
        explicit JavaRandom(int64_t seed) { SetSeed(seed); }

        void SetSeed(int64_t seed) {
            m_seed = (static_cast<uint64_t>(seed) ^ kMultiplier) & kMask48;
        }

        // java.util.Random.next(bits): 48-bit LCG step, top bits returned.
        int32_t Next(int bits) {
            m_seed = (m_seed * kMultiplier + kAddend) & kMask48;
            return static_cast<int32_t>(static_cast<int64_t>(m_seed) >> (48 - bits));
        }

        float NextFloat() {
            return Next(24) / static_cast<float>(1 << 24);
        }

        double NextDouble() {
            return ((static_cast<int64_t>(Next(26)) << 27) + Next(27))
                   / static_cast<double>(1LL << 53);
        }

    private:
        static constexpr uint64_t kMultiplier = 0x5DEECE66DULL;
        static constexpr uint64_t kAddend = 0xBULL;
        static constexpr uint64_t kMask48 = (1ULL << 48) - 1;

        uint64_t m_seed = 0;
    };

} // namespace Render

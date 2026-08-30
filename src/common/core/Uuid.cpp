// File: src/common/core/Uuid.cpp
#include "common/core/Uuid.hpp"

#include <random>

namespace Game {

    Uuid RandomUuid() {
        // Seeded once per thread from the platform entropy source. The engine
        // already uses this shape for non-deterministic randomness elsewhere;
        // the world's JavaRandom streams are deliberately not touched, because
        // drawing identity from them would perturb worldgen determinism.
        static thread_local std::mt19937_64 rng{std::random_device{}()};

        Uuid u{};
        const uint64_t hi = rng();
        const uint64_t lo = rng();
        for (int i = 0; i < 8; ++i) {
            u[i]     = static_cast<uint8_t>(hi >> (56 - i * 8));
            u[8 + i] = static_cast<uint8_t>(lo >> (56 - i * 8));
        }

        // RFC 4122: version 4 in the high nibble of byte 6, IETF variant in
        // the top bits of byte 8. Without these a reader can still use the
        // value, but it is not a well-formed UUID and tools will say so.
        u[6] = static_cast<uint8_t>((u[6] & 0x0F) | 0x40);
        u[8] = static_cast<uint8_t>((u[8] & 0x3F) | 0x80);
        return u;
    }

} // namespace Game

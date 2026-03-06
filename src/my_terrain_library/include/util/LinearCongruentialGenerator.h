#pragma once

#include <cstdint>

namespace minecraft {
namespace util {

/**
 * LinearCongruentialGenerator - Simple LCG for pseudorandom number generation
 *
 * Reference: net/minecraft/util/LinearCongruentialGenerator.java
 */
class LinearCongruentialGenerator {
private:
    // Reference: LinearCongruentialGenerator.java lines 4-5
    static constexpr int64_t MULTIPLIER = 6364136223846793005LL;
    static constexpr int64_t INCREMENT = 1442695040888963407LL;

public:
    /**
     * Advance the LCG state
     * Reference: LinearCongruentialGenerator.java lines 7-11
     *
     * @param rval - Current state
     * @param c - Additional value to mix in
     * @return Next state
     */
    static int64_t next(int64_t rval, int64_t c) {
        // Reference: LinearCongruentialGenerator.java line 8
        rval *= rval * MULTIPLIER + INCREMENT;
        // Reference: LinearCongruentialGenerator.java line 9
        rval += c;
        return rval;
    }
};

} // namespace util
} // namespace minecraft

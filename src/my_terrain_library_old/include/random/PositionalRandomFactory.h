#pragma once

#include <string>
#include <cstdint>

// Forward declaration
namespace minecraft {
    class XoroshiroRandomSource;
    class XoroshiroPositionalRandomFactory;
}

// Reference: net/minecraft/world/level/levelgen/PositionalRandomFactory.java

namespace minecraft {
namespace random {

/**
 * PositionalRandomFactory - Base type for creating position-based random sources
 *
 * This is an interface in Java, implemented by XoroshiroPositionalRandomFactory.
 * In C++, we use XoroshiroPositionalRandomFactory directly, but define this typedef
 * for consistency with the Java code structure.
 *
 * Reference: PositionalRandomFactory.java
 */
typedef XoroshiroPositionalRandomFactory PositionalRandomFactory;

} // namespace random
} // namespace minecraft

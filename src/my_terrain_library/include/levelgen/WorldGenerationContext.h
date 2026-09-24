#pragma once

#include <cstdint>

namespace minecraft {
namespace levelgen {

/**
 * WorldGenerationContext - Height bounds for world generation
 * Reference: net/minecraft/world/level/levelgen/WorldGenerationContext.java
 * (minGenY = max(level minY, generator minY), genDepth = min(level height,
 * generator depth)).
 */
class WorldGenerationContext {
private:
    int32_t m_minY;
    int32_t m_height;

public:
    WorldGenerationContext(int32_t minY, int32_t height)
        : m_minY(minY), m_height(height) {}

    int32_t getMinGenY() const { return m_minY; }
    int32_t getGenDepth() const { return m_height; }
};

} // namespace levelgen
} // namespace minecraft

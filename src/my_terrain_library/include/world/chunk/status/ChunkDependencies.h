#pragma once

#include "world/chunk/status/ChunkStatus.h"
#include <vector>
#include <cstdint>
#include <stdexcept>

// Reference: net/minecraft/world/level/chunk/status/ChunkDependencies.java

namespace minecraft {
namespace world {
namespace chunk {
namespace status {

/**
 * ChunkDependencies - Tracks status dependencies at different radii
 *
 * For each generation step, neighboring chunks may need to be at certain
 * statuses. This class tracks those requirements.
 *
 * Reference: ChunkDependencies.java
 */
class ChunkDependencies {
private:
    std::vector<const ChunkStatus*> m_dependencyByRadius;  // Status required at each radius
    std::vector<int32_t> m_radiusByDependency;             // Radius required for each status

public:
    /**
     * Constructor from dependency list
     * Reference: ChunkDependencies.java lines 11-24
     *
     * @param dependencyByRadius - List of ChunkStatus required at each radius index
     */
    explicit ChunkDependencies(std::vector<const ChunkStatus*> dependencyByRadius)
        : m_dependencyByRadius(std::move(dependencyByRadius))
    {
        // Build reverse lookup: for each status index, what's the required radius?
        // Reference: ChunkDependencies.java lines 13-23
        int32_t size = m_dependencyByRadius.empty() ? 0
            : m_dependencyByRadius[0]->getIndex() + 1;

        m_radiusByDependency.resize(size, 0);

        for (int32_t radius = 0; radius < static_cast<int32_t>(m_dependencyByRadius.size()); ++radius) {
            const ChunkStatus* dependency = m_dependencyByRadius[radius];
            int32_t index = dependency->getIndex();

            // For this dependency and all earlier statuses, record the radius
            for (int32_t statusIndex = 0; statusIndex <= index; ++statusIndex) {
                m_radiusByDependency[statusIndex] = radius;
            }
        }
    }

    /**
     * Default constructor for empty dependencies
     */
    ChunkDependencies() = default;

    /**
     * Get the dependency list
     * Reference: ChunkDependencies.java lines 27-29
     */
    const std::vector<const ChunkStatus*>& asList() const {
        return m_dependencyByRadius;
    }

    /**
     * Get the number of radius levels
     * Reference: ChunkDependencies.java lines 31-33
     */
    int32_t size() const {
        return static_cast<int32_t>(m_dependencyByRadius.size());
    }

    /**
     * Get the radius at which a status is required
     * Reference: ChunkDependencies.java lines 35-42
     *
     * @param status - The status to look up
     * @return The minimum radius at which this status is needed
     * @throws If status is outside dependency range
     */
    int32_t getRadiusOf(const ChunkStatus& status) const {
        int32_t index = status.getIndex();
        if (index >= static_cast<int32_t>(m_radiusByDependency.size())) {
            throw std::invalid_argument(
                "Requesting ChunkStatus outside of dependency range"
            );
        }
        return m_radiusByDependency[index];
    }

    /**
     * Get the maximum radius (size - 1, minimum 0)
     * Reference: ChunkDependencies.java lines 44-46
     */
    int32_t getRadius() const {
        return std::max(0, static_cast<int32_t>(m_dependencyByRadius.size()) - 1);
    }

    /**
     * Get the status required at a specific distance
     * Reference: ChunkDependencies.java lines 48-50
     *
     * @param distance - The radius distance
     * @return The ChunkStatus required at that distance
     */
    const ChunkStatus& get(int32_t distance) const {
        return *m_dependencyByRadius[distance];
    }
};

} // namespace status
} // namespace chunk
} // namespace world
} // namespace minecraft

#pragma once

#include <vector>
#include <functional>
#include <stdexcept>
#include <string>
#include <sstream>

// Reference: net/minecraft/util/StaticCache2D.java

namespace minecraft {
namespace util {

/**
 * StaticCache2D - 2D spatial cache for holding values indexed by (x, z) coordinates
 * Reference: StaticCache2D.java
 *
 * @tparam T The type of values stored in the cache
 */
template<typename T>
class StaticCache2D {
public:
    /**
     * Initializer function type
     * Reference: StaticCache2D.java lines 66-68
     */
    using Initializer = std::function<T(int x, int z)>;

    /**
     * Create a StaticCache2D centered at (centerX, centerZ) with the given range
     * Reference: StaticCache2D.java lines 13-18
     *
     * @param centerX Center X coordinate
     * @param centerZ Center Z coordinate
     * @param range Range from center (creates a (2*range+1) x (2*range+1) cache)
     * @param initializer Function to initialize each cell
     * @return The created cache
     */
    static StaticCache2D<T> create(int centerX, int centerZ, int range, Initializer initializer) {
        int minX = centerX - range;
        int minZ = centerZ - range;
        int size = 2 * range + 1;
        return StaticCache2D<T>(minX, minZ, size, size, initializer);
    }

    /**
     * Iterate over all values in the cache
     * Reference: StaticCache2D.java lines 35-40
     */
    void forEach(std::function<void(T&)> consumer) {
        for (auto& item : m_cache) {
            consumer(item);
        }
    }

    /**
     * Iterate over all values in the cache (const version)
     */
    void forEach(std::function<void(const T&)> consumer) const {
        for (const auto& item : m_cache) {
            consumer(item);
        }
    }

    /**
     * Get the value at (x, z)
     * Reference: StaticCache2D.java lines 42-48
     *
     * @throws std::invalid_argument if coordinates are out of range
     */
    T& get(int x, int z) {
        if (!contains(x, z)) {
            std::ostringstream ss;
            ss << "Requested out of range value (" << x << "," << z << ") from " << toString();
            throw std::invalid_argument(ss.str());
        }
        return m_cache[getIndex(x, z)];
    }

    /**
     * Get the value at (x, z) (const version)
     * Reference: StaticCache2D.java lines 42-48
     */
    const T& get(int x, int z) const {
        if (!contains(x, z)) {
            std::ostringstream ss;
            ss << "Requested out of range value (" << x << "," << z << ") from " << toString();
            throw std::invalid_argument(ss.str());
        }
        return m_cache[getIndex(x, z)];
    }

    /**
     * Check if (x, z) is within the cache bounds
     * Reference: StaticCache2D.java lines 50-54
     */
    bool contains(int x, int z) const {
        int deltaX = x - m_minX;
        int deltaZ = z - m_minZ;
        return deltaX >= 0 && deltaX < m_sizeX && deltaZ >= 0 && deltaZ < m_sizeZ;
    }

    /**
     * Get string representation
     * Reference: StaticCache2D.java lines 56-58
     */
    std::string toString() const {
        std::ostringstream ss;
        ss << "StaticCache2D[" << m_minX << ", " << m_minZ << ", "
           << (m_minX + m_sizeX) << ", " << (m_minZ + m_sizeZ) << "]";
        return ss.str();
    }

    // Accessors
    int getMinX() const { return m_minX; }
    int getMinZ() const { return m_minZ; }
    int getSizeX() const { return m_sizeX; }
    int getSizeZ() const { return m_sizeZ; }

private:
    /**
     * Private constructor
     * Reference: StaticCache2D.java lines 20-33
     */
    StaticCache2D(int minX, int minZ, int sizeX, int sizeZ, Initializer initializer)
        : m_minX(minX)
        , m_minZ(minZ)
        , m_sizeX(sizeX)
        , m_sizeZ(sizeZ)
        , m_cache(sizeX * sizeZ)
    {
        for (int x = minX; x < minX + sizeX; ++x) {
            for (int z = minZ; z < minZ + sizeZ; ++z) {
                m_cache[getIndex(x, z)] = initializer(x, z);
            }
        }
    }

    /**
     * Calculate array index for (x, z)
     * Reference: StaticCache2D.java lines 60-64
     */
    int getIndex(int x, int z) const {
        int deltaX = x - m_minX;
        int deltaZ = z - m_minZ;
        return deltaX * m_sizeZ + deltaZ;
    }

    int m_minX;
    int m_minZ;
    int m_sizeX;
    int m_sizeZ;
    std::vector<T> m_cache;
};

} // namespace util
} // namespace minecraft

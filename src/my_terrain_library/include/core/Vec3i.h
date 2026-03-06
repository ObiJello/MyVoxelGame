#pragma once

#include <cstdint>

namespace minecraft {
namespace core {

/**
 * Immutable 3D integer vector
 * Reference: Vec3i.java
 */
class Vec3i {
protected:
    int32_t m_x;
    int32_t m_y;
    int32_t m_z;

public:
    // Setters for mutable subclasses (Vec3i.java lines 69-82)
    Vec3i& setX(int32_t x) { m_x = x; return *this; }
    Vec3i& setY(int32_t y) { m_y = y; return *this; }
    Vec3i& setZ(int32_t z) { m_z = z; return *this; }
    // Constructors (Vec3i.java line 28-32)
    Vec3i(int32_t x, int32_t y, int32_t z) : m_x(x), m_y(y), m_z(z) {}

    // Default constructor
    Vec3i() : m_x(0), m_y(0), m_z(0) {}

    // Getters (Vec3i.java lines 57-67)
    int32_t getX() const { return m_x; }
    int32_t getY() const { return m_y; }
    int32_t getZ() const { return m_z; }

    // Equality (Vec3i.java lines 34-43)
    bool operator==(const Vec3i& other) const {
        return m_x == other.m_x && m_y == other.m_y && m_z == other.m_z;
    }

    bool operator!=(const Vec3i& other) const {
        return !(*this == other);
    }

    // Comparison for use in sorted containers (std::set, std::map)
    bool operator<(const Vec3i& other) const {
        if (m_x != other.m_x) return m_x < other.m_x;
        if (m_y != other.m_y) return m_y < other.m_y;
        return m_z < other.m_z;
    }

    // Hash code (Vec3i.java lines 45-47)
    int32_t hashCode() const {
        return (m_y + m_z * 31) * 31 + m_x;
    }

    // Distance squared (Vec3i.java lines 104-107)
    double distSqr(const Vec3i& other) const {
        double dx = static_cast<double>(other.m_x - m_x);
        double dy = static_cast<double>(other.m_y - m_y);
        double dz = static_cast<double>(other.m_z - m_z);
        return dx * dx + dy * dy + dz * dz;
    }

    double distSqr(int32_t x, int32_t y, int32_t z) const {
        double dx = static_cast<double>(x - m_x);
        double dy = static_cast<double>(y - m_y);
        double dz = static_cast<double>(z - m_z);
        return dx * dx + dy * dy + dz * dz;
    }

    // Static ZERO constant (Vec3i.java line 19)
    static const Vec3i& ZERO();
};

} // namespace core
} // namespace minecraft

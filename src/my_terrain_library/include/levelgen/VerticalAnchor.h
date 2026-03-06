#pragma once

#include "levelgen/SurfaceRules.h"
#include <cstdint>
#include <variant>

// Reference: net/minecraft/world/level/levelgen/VerticalAnchor.java

namespace minecraft {
namespace levelgen {

/**
 * VerticalAnchor - Represents a Y-coordinate that can be absolute or relative
 * Reference: VerticalAnchor.java
 */
class VerticalAnchor {
public:
    enum class Type {
        ABSOLUTE,      // Fixed Y value
        ABOVE_BOTTOM,  // Offset above world bottom
        BELOW_TOP      // Offset below world top
    };

private:
    Type m_type;
    int32_t m_value;

    VerticalAnchor(Type type, int32_t value) : m_type(type), m_value(value) {}

public:
    /**
     * Create an absolute anchor at fixed Y coordinate
     * Reference: VerticalAnchor.java lines 13-15
     */
    static VerticalAnchor absolute(int32_t y) {
        return VerticalAnchor(Type::ABSOLUTE, y);
    }

    /**
     * Create an anchor relative to world bottom
     * Reference: VerticalAnchor.java lines 17-19
     */
    static VerticalAnchor aboveBottom(int32_t offset) {
        return VerticalAnchor(Type::ABOVE_BOTTOM, offset);
    }

    /**
     * Create an anchor relative to world top
     * Reference: VerticalAnchor.java lines 21-23
     */
    static VerticalAnchor belowTop(int32_t offset) {
        return VerticalAnchor(Type::BELOW_TOP, offset);
    }

    /**
     * Convenience method for bottom of world
     * Reference: VerticalAnchor.java line 10
     */
    static VerticalAnchor bottom() {
        return aboveBottom(0);
    }

    /**
     * Convenience method for top of world
     * Reference: VerticalAnchor.java line 11
     */
    static VerticalAnchor top() {
        return belowTop(0);
    }

    /**
     * Resolve the actual Y coordinate given a world context
     * Reference: VerticalAnchor.java line 41
     */
    int32_t resolveY(const WorldGenerationContext& context) const {
        switch (m_type) {
            case Type::ABSOLUTE:
                // Reference: Absolute.resolveY() - VerticalAnchor.java lines 46-48
                return m_value;

            case Type::ABOVE_BOTTOM:
                // Reference: AboveBottom.resolveY() - VerticalAnchor.java lines 62-64
                return context.getMinGenY() + m_value;

            case Type::BELOW_TOP:
                // Reference: BelowTop.resolveY() - VerticalAnchor.java lines 78-80
                return context.getGenDepth() - 1 + context.getMinGenY() - m_value;
        }
        return m_value;
    }

    Type getType() const { return m_type; }
    int32_t getValue() const { return m_value; }
};

} // namespace levelgen
} // namespace minecraft

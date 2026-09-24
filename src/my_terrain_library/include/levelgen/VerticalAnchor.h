#pragma once

#include "levelgen/WorldGenerationContext.h"

#include <cstdint>

namespace minecraft {
namespace levelgen {

/**
 * VerticalAnchor - Resolves Y coordinates relative to world bounds
 * Reference: net/minecraft/world/level/levelgen/VerticalAnchor.java
 */
class VerticalAnchor {
public:
    enum class Type {
        ABSOLUTE,
        ABOVE_BOTTOM,
        BELOW_TOP
    };

private:
    Type m_type;
    int32_t m_value;

public:
    VerticalAnchor(Type type, int32_t value) : m_type(type), m_value(value) {}

    // Factory methods matching Java
    static VerticalAnchor absolute(int32_t value) {
        return VerticalAnchor(Type::ABSOLUTE, value);
    }

    static VerticalAnchor aboveBottom(int32_t offset) {
        return VerticalAnchor(Type::ABOVE_BOTTOM, offset);
    }

    static VerticalAnchor belowTop(int32_t offset) {
        return VerticalAnchor(Type::BELOW_TOP, offset);
    }

    static VerticalAnchor bottom() {
        return aboveBottom(0);
    }

    static VerticalAnchor top() {
        return belowTop(0);
    }

    // Reference: VerticalAnchor.java resolveY methods
    int32_t resolveY(const WorldGenerationContext& context) const {
        switch (m_type) {
            case Type::ABSOLUTE:
                return m_value;
            case Type::ABOVE_BOTTOM:
                return context.getMinGenY() + m_value;
            case Type::BELOW_TOP:
                return context.getGenDepth() - 1 + context.getMinGenY() - m_value;
        }
        return m_value;
    }

    Type getType() const { return m_type; }
    int32_t getValue() const { return m_value; }
};

} // namespace levelgen
} // namespace minecraft

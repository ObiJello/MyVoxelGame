#pragma once

#include <cstdint>
#include <string>

// Reference: levelgen.VerticalAnchor and levelgen.WorldGenerationContext
// (26.3).
//
// The 26.3 anchor has four forms - absolute, above_bottom, below_top and
// relative_to_sea_level - and resolves against the generation context's
// (minY, height, seaLevel). The engine's levelgen::WorldGenerationContext
// (SurfaceRules.h) predates the sea-level field, so the material system
// resolves anchors against this GenerationContext instead: minGenY/genDepth
// come from the engine's context, seaLevel from the generator (Java's
// WorldGenerationContext takes it from generator.getSeaLevel(), the same
// noise-settings sea level the MaterialSystem is built with).

namespace minecraft {
namespace levelgen {
namespace material {

// The fields of levelgen.WorldGenerationContext an anchor reads.
struct GenerationContext {
    int32_t minGenY = 0;
    int32_t genDepth = 0;
    int32_t seaLevel = 0;

    int32_t getMinGenY() const { return minGenY; }
    int32_t getGenDepth() const { return genDepth; }
};

class VerticalAnchor {
public:
    enum class Kind { ABSOLUTE, ABOVE_BOTTOM, BELOW_TOP, RELATIVE_TO_SEA_LEVEL };

    // DimensionType.MIN_Y / MAX_Y: the range every anchor codec accepts.
    static constexpr int32_t MIN_VALUE = -2032;
    static constexpr int32_t MAX_VALUE = 2031;

    constexpr VerticalAnchor() = default;
    constexpr VerticalAnchor(Kind kind, int32_t value) : m_kind(kind), m_value(value) {}

    static constexpr VerticalAnchor absolute(int32_t y) { return {Kind::ABSOLUTE, y}; }
    static constexpr VerticalAnchor aboveBottom(int32_t offset) { return {Kind::ABOVE_BOTTOM, offset}; }
    static constexpr VerticalAnchor belowTop(int32_t offset) { return {Kind::BELOW_TOP, offset}; }
    static constexpr VerticalAnchor relativeToSeaLevel(int32_t offset) { return {Kind::RELATIVE_TO_SEA_LEVEL, offset}; }
    static constexpr VerticalAnchor bottom() { return aboveBottom(0); }
    static constexpr VerticalAnchor top() { return belowTop(0); }
    static constexpr VerticalAnchor seaLevel() { return relativeToSeaLevel(0); }

    // VerticalAnchor.resolveY, each record's expression as written.
    int32_t resolveY(const GenerationContext& context) const {
        switch (m_kind) {
            case Kind::ABSOLUTE:
                return m_value;
            case Kind::ABOVE_BOTTOM:
                return context.getMinGenY() + m_value;
            case Kind::BELOW_TOP:
                return context.getGenDepth() - 1 + context.getMinGenY() - m_value;
            case Kind::RELATIVE_TO_SEA_LEVEL:
                return context.seaLevel + m_value;
        }
        return m_value;
    }

    Kind kind() const { return m_kind; }
    int32_t value() const { return m_value; }

    bool operator==(const VerticalAnchor& other) const { return m_kind == other.m_kind && m_value == other.m_value; }
    bool operator!=(const VerticalAnchor& other) const { return !(*this == other); }

    // The record's toString ("5 above bottom").
    std::string toString() const;

private:
    Kind m_kind = Kind::ABSOLUTE;
    int32_t m_value = 0;
};

} // namespace material
} // namespace levelgen
} // namespace minecraft

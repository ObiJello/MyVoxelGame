#pragma once

#include "util/StringRepresentable.h"
#include <string>
#include <vector>

namespace minecraft {
namespace world {
namespace level {
namespace block {
namespace state {
namespace properties {

/**
 * StairsShape - Shape of stairs when connecting to other stairs
 * Reference: net/minecraft/world/level/block/state/properties/StairsShape.java
 */
class StairsShape {
public:
    enum Value {
        STRAIGHT,
        INNER_LEFT,
        INNER_RIGHT,
        OUTER_LEFT,
        OUTER_RIGHT
    };

private:
    Value m_value;
    std::string m_name;

public:
    StairsShape() : m_value(STRAIGHT), m_name("straight") {}

    StairsShape(Value value) : m_value(value) {
        switch (value) {
            case STRAIGHT:    m_name = "straight"; break;
            case INNER_LEFT:  m_name = "inner_left"; break;
            case INNER_RIGHT: m_name = "inner_right"; break;
            case OUTER_LEFT:  m_name = "outer_left"; break;
            case OUTER_RIGHT: m_name = "outer_right"; break;
        }
    }

    std::string getSerializedName() const { return m_name; }

    Value getValue() const { return m_value; }

    bool operator==(const StairsShape& other) const { return m_value == other.m_value; }
    bool operator!=(const StairsShape& other) const { return m_value != other.m_value; }

    // Pre-defined instances
    static StairsShape straight() { return StairsShape(STRAIGHT); }
    static StairsShape innerLeft() { return StairsShape(INNER_LEFT); }
    static StairsShape innerRight() { return StairsShape(INNER_RIGHT); }
    static StairsShape outerLeft() { return StairsShape(OUTER_LEFT); }
    static StairsShape outerRight() { return StairsShape(OUTER_RIGHT); }

    // All values for EnumProperty
    static std::vector<StairsShape> values() {
        return {
            StairsShape(STRAIGHT),
            StairsShape(INNER_LEFT),
            StairsShape(INNER_RIGHT),
            StairsShape(OUTER_LEFT),
            StairsShape(OUTER_RIGHT)
        };
    }
};

} // namespace properties
} // namespace state
} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft

// Hash support for unordered_map
namespace std {
template<>
struct hash<minecraft::world::level::block::state::properties::StairsShape> {
    size_t operator()(const minecraft::world::level::block::state::properties::StairsShape& s) const {
        return std::hash<int>()(static_cast<int>(s.getValue()));
    }
};
}

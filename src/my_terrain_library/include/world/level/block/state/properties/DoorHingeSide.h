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
 * DoorHingeSide - Left or right hinge of a door
 * Reference: net/minecraft/world/level/block/state/properties/DoorHingeSide.java
 */
class DoorHingeSide {
public:
    enum Value {
        LEFT,
        RIGHT
    };

private:
    Value m_value;

public:
    DoorHingeSide() : m_value(LEFT) {}

    DoorHingeSide(Value value) : m_value(value) {}

    std::string getSerializedName() const {
        return m_value == LEFT ? "left" : "right";
    }

    std::string toString() const { return getSerializedName(); }
    Value getValue() const { return m_value; }

    bool operator==(const DoorHingeSide& other) const { return m_value == other.m_value; }
    bool operator!=(const DoorHingeSide& other) const { return m_value != other.m_value; }

    static DoorHingeSide left() { return DoorHingeSide(LEFT); }
    static DoorHingeSide right() { return DoorHingeSide(RIGHT); }

    static std::vector<DoorHingeSide> values() {
        return { DoorHingeSide(LEFT), DoorHingeSide(RIGHT) };
    }
};

} // namespace properties
} // namespace state
} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft

namespace std {
template<>
struct hash<minecraft::world::level::block::state::properties::DoorHingeSide> {
    size_t operator()(const minecraft::world::level::block::state::properties::DoorHingeSide& d) const {
        return std::hash<int>()(static_cast<int>(d.getValue()));
    }
};
}

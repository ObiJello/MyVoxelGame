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
 * RailShape - Shape of a rail block
 * Reference: net/minecraft/world/level/block/state/properties/RailShape.java
 */
class RailShape {
public:
    enum Value {
        NORTH_SOUTH,
        EAST_WEST,
        ASCENDING_EAST,
        ASCENDING_WEST,
        ASCENDING_NORTH,
        ASCENDING_SOUTH,
        SOUTH_EAST,
        SOUTH_WEST,
        NORTH_WEST,
        NORTH_EAST
    };

private:
    Value m_value;
    std::string m_name;

public:
    RailShape() : m_value(NORTH_SOUTH), m_name("north_south") {}

    RailShape(Value value) : m_value(value) {
        switch (value) {
            case NORTH_SOUTH:     m_name = "north_south"; break;
            case EAST_WEST:       m_name = "east_west"; break;
            case ASCENDING_EAST:  m_name = "ascending_east"; break;
            case ASCENDING_WEST:  m_name = "ascending_west"; break;
            case ASCENDING_NORTH: m_name = "ascending_north"; break;
            case ASCENDING_SOUTH: m_name = "ascending_south"; break;
            case SOUTH_EAST:      m_name = "south_east"; break;
            case SOUTH_WEST:      m_name = "south_west"; break;
            case NORTH_WEST:      m_name = "north_west"; break;
            case NORTH_EAST:      m_name = "north_east"; break;
        }
    }

    std::string getName() const { return m_name; }
    std::string getSerializedName() const { return m_name; }
    std::string toString() const { return m_name; }
    Value getValue() const { return m_value; }

    bool isSlope() const {
        return m_value == ASCENDING_NORTH || m_value == ASCENDING_EAST ||
               m_value == ASCENDING_SOUTH || m_value == ASCENDING_WEST;
    }

    bool operator==(const RailShape& other) const { return m_value == other.m_value; }
    bool operator!=(const RailShape& other) const { return m_value != other.m_value; }

    static RailShape northSouth() { return RailShape(NORTH_SOUTH); }
    static RailShape eastWest() { return RailShape(EAST_WEST); }

    static std::vector<RailShape> values() {
        return {
            RailShape(NORTH_SOUTH), RailShape(EAST_WEST),
            RailShape(ASCENDING_EAST), RailShape(ASCENDING_WEST),
            RailShape(ASCENDING_NORTH), RailShape(ASCENDING_SOUTH),
            RailShape(SOUTH_EAST), RailShape(SOUTH_WEST),
            RailShape(NORTH_WEST), RailShape(NORTH_EAST)
        };
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
struct hash<minecraft::world::level::block::state::properties::RailShape> {
    size_t operator()(const minecraft::world::level::block::state::properties::RailShape& r) const {
        return std::hash<int>()(static_cast<int>(r.getValue()));
    }
};
}

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
 * BellAttachType - How a bell is attached
 * Reference: net/minecraft/world/level/block/state/properties/BellAttachType.java
 */
class BellAttachType {
public:
    enum Value {
        FLOOR,
        CEILING,
        SINGLE_WALL,
        DOUBLE_WALL
    };

private:
    Value m_value;
    std::string m_name;

public:
    BellAttachType() : m_value(FLOOR), m_name("floor") {}

    BellAttachType(Value value) : m_value(value) {
        switch (value) {
            case FLOOR:       m_name = "floor"; break;
            case CEILING:     m_name = "ceiling"; break;
            case SINGLE_WALL: m_name = "single_wall"; break;
            case DOUBLE_WALL: m_name = "double_wall"; break;
        }
    }

    std::string getSerializedName() const { return m_name; }
    Value getValue() const { return m_value; }

    bool operator==(const BellAttachType& other) const { return m_value == other.m_value; }
    bool operator!=(const BellAttachType& other) const { return m_value != other.m_value; }

    static BellAttachType floor() { return BellAttachType(FLOOR); }
    static BellAttachType ceiling() { return BellAttachType(CEILING); }
    static BellAttachType singleWall() { return BellAttachType(SINGLE_WALL); }
    static BellAttachType doubleWall() { return BellAttachType(DOUBLE_WALL); }

    static std::vector<BellAttachType> values() {
        return { BellAttachType(FLOOR), BellAttachType(CEILING),
                 BellAttachType(SINGLE_WALL), BellAttachType(DOUBLE_WALL) };
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
struct hash<minecraft::world::level::block::state::properties::BellAttachType> {
    size_t operator()(const minecraft::world::level::block::state::properties::BellAttachType& b) const {
        return std::hash<int>()(static_cast<int>(b.getValue()));
    }
};
}

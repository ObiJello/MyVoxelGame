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
 * AttachFace - Face a block is attached to
 * Reference: net/minecraft/world/level/block/state/properties/AttachFace.java
 */
class AttachFace {
public:
    enum Value {
        FLOOR,
        WALL,
        CEILING
    };

private:
    Value m_value;
    std::string m_name;

public:
    AttachFace() : m_value(FLOOR), m_name("floor") {}

    AttachFace(Value value) : m_value(value) {
        switch (value) {
            case FLOOR:   m_name = "floor"; break;
            case WALL:    m_name = "wall"; break;
            case CEILING: m_name = "ceiling"; break;
        }
    }

    std::string getSerializedName() const { return m_name; }
    Value getValue() const { return m_value; }

    bool operator==(const AttachFace& other) const { return m_value == other.m_value; }
    bool operator!=(const AttachFace& other) const { return m_value != other.m_value; }

    static AttachFace floor() { return AttachFace(FLOOR); }
    static AttachFace wall() { return AttachFace(WALL); }
    static AttachFace ceiling() { return AttachFace(CEILING); }

    static std::vector<AttachFace> values() {
        return { AttachFace(FLOOR), AttachFace(WALL), AttachFace(CEILING) };
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
struct hash<minecraft::world::level::block::state::properties::AttachFace> {
    size_t operator()(const minecraft::world::level::block::state::properties::AttachFace& a) const {
        return std::hash<int>()(static_cast<int>(a.getValue()));
    }
};
}

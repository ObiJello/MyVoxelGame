#pragma once

#include "util/StringRepresentable.h"
#include <string>

namespace minecraft {
namespace world {
namespace level {
namespace block {
namespace state {
namespace properties {

/**
 * Half - Top or bottom half of a block
 * Reference: net/minecraft/world/level/block/state/properties/Half.java
 */
class Half {
public:
    enum Value {
        TOP,
        BOTTOM
    };

private:
    Value m_value;
    std::string m_name;

public:
    Half() : m_value(BOTTOM), m_name("bottom") {}

    Half(Value value) : m_value(value) {
        switch (value) {
            case TOP:    m_name = "top"; break;
            case BOTTOM: m_name = "bottom"; break;
        }
    }

    std::string getSerializedName() const { return m_name; }

    Value getValue() const { return m_value; }

    bool operator==(const Half& other) const { return m_value == other.m_value; }
    bool operator!=(const Half& other) const { return m_value != other.m_value; }

    // Pre-defined instances
    static Half top() { return Half(TOP); }
    static Half bottom() { return Half(BOTTOM); }

    // All values for EnumProperty
    static std::vector<Half> values() {
        return { Half(TOP), Half(BOTTOM) };
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
struct hash<minecraft::world::level::block::state::properties::Half> {
    size_t operator()(const minecraft::world::level::block::state::properties::Half& h) const {
        return std::hash<int>()(static_cast<int>(h.getValue()));
    }
};
}

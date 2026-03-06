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
 * SlabType - Type of slab (top, bottom, or double)
 * Reference: net/minecraft/world/level/block/state/properties/SlabType.java
 */
class SlabType {
public:
    enum Value {
        TOP,
        BOTTOM,
        DOUBLE
    };

private:
    Value m_value;
    std::string m_name;

public:
    SlabType() : m_value(BOTTOM), m_name("bottom") {}

    SlabType(Value value) : m_value(value) {
        switch (value) {
            case TOP:    m_name = "top"; break;
            case BOTTOM: m_name = "bottom"; break;
            case DOUBLE: m_name = "double"; break;
        }
    }

    std::string getSerializedName() const { return m_name; }

    Value getValue() const { return m_value; }

    bool operator==(const SlabType& other) const { return m_value == other.m_value; }
    bool operator!=(const SlabType& other) const { return m_value != other.m_value; }

    // Pre-defined instances
    static SlabType top() { return SlabType(TOP); }
    static SlabType bottom() { return SlabType(BOTTOM); }
    static SlabType full() { return SlabType(DOUBLE); }

    // All values for EnumProperty
    static std::vector<SlabType> values() {
        return { SlabType(TOP), SlabType(BOTTOM), SlabType(DOUBLE) };
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
struct hash<minecraft::world::level::block::state::properties::SlabType> {
    size_t operator()(const minecraft::world::level::block::state::properties::SlabType& s) const {
        return std::hash<int>()(static_cast<int>(s.getValue()));
    }
};
}

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
 * BambooLeaves - Leaf size on bamboo
 * Reference: net/minecraft/world/level/block/state/properties/BambooLeaves.java
 */
class BambooLeaves {
public:
    enum Value {
        NONE,
        SMALL,
        LARGE
    };

private:
    Value m_value;
    std::string m_name;

public:
    BambooLeaves() : m_value(NONE), m_name("none") {}

    BambooLeaves(Value value) : m_value(value) {
        switch (value) {
            case NONE:  m_name = "none"; break;
            case SMALL: m_name = "small"; break;
            case LARGE: m_name = "large"; break;
        }
    }

    std::string getSerializedName() const { return m_name; }
    Value getValue() const { return m_value; }

    bool operator==(const BambooLeaves& other) const { return m_value == other.m_value; }
    bool operator!=(const BambooLeaves& other) const { return m_value != other.m_value; }

    static BambooLeaves none() { return BambooLeaves(NONE); }
    static BambooLeaves small() { return BambooLeaves(SMALL); }
    static BambooLeaves large() { return BambooLeaves(LARGE); }

    static std::vector<BambooLeaves> values() {
        return { BambooLeaves(NONE), BambooLeaves(SMALL), BambooLeaves(LARGE) };
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
struct hash<minecraft::world::level::block::state::properties::BambooLeaves> {
    size_t operator()(const minecraft::world::level::block::state::properties::BambooLeaves& b) const {
        return std::hash<int>()(static_cast<int>(b.getValue()));
    }
};
}

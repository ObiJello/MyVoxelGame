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
 * RedstoneSide - Connection state for redstone wire
 * Reference: net/minecraft/world/level/block/state/properties/RedstoneSide.java
 */
class RedstoneSide {
public:
    enum Value {
        UP,
        SIDE,
        NONE
    };

private:
    Value m_value;
    std::string m_name;

public:
    RedstoneSide() : m_value(NONE), m_name("none") {}

    RedstoneSide(Value value) : m_value(value) {
        switch (value) {
            case UP:   m_name = "up"; break;
            case SIDE: m_name = "side"; break;
            case NONE: m_name = "none"; break;
        }
    }

    std::string getSerializedName() const { return m_name; }
    std::string toString() const { return getSerializedName(); }
    Value getValue() const { return m_value; }

    bool isConnected() const { return m_value != NONE; }

    bool operator==(const RedstoneSide& other) const { return m_value == other.m_value; }
    bool operator!=(const RedstoneSide& other) const { return m_value != other.m_value; }

    static RedstoneSide up() { return RedstoneSide(UP); }
    static RedstoneSide side() { return RedstoneSide(SIDE); }
    static RedstoneSide none() { return RedstoneSide(NONE); }

    static std::vector<RedstoneSide> values() {
        return { RedstoneSide(UP), RedstoneSide(SIDE), RedstoneSide(NONE) };
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
struct hash<minecraft::world::level::block::state::properties::RedstoneSide> {
    size_t operator()(const minecraft::world::level::block::state::properties::RedstoneSide& r) const {
        return std::hash<int>()(static_cast<int>(r.getValue()));
    }
};
}

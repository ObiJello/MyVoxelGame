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
 * WallSide - Height of a wall connection
 * Reference: net/minecraft/world/level/block/state/properties/WallSide.java
 */
class WallSide {
public:
    enum Value {
        NONE,
        LOW,
        TALL
    };

private:
    Value m_value;
    std::string m_name;

public:
    WallSide() : m_value(NONE), m_name("none") {}

    WallSide(Value value) : m_value(value) {
        switch (value) {
            case NONE: m_name = "none"; break;
            case LOW:  m_name = "low"; break;
            case TALL: m_name = "tall"; break;
        }
    }

    std::string getSerializedName() const { return m_name; }
    std::string toString() const { return getSerializedName(); }
    Value getValue() const { return m_value; }

    bool operator==(const WallSide& other) const { return m_value == other.m_value; }
    bool operator!=(const WallSide& other) const { return m_value != other.m_value; }

    static WallSide none() { return WallSide(NONE); }
    static WallSide low() { return WallSide(LOW); }
    static WallSide tall() { return WallSide(TALL); }

    static std::vector<WallSide> values() {
        return { WallSide(NONE), WallSide(LOW), WallSide(TALL) };
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
struct hash<minecraft::world::level::block::state::properties::WallSide> {
    size_t operator()(const minecraft::world::level::block::state::properties::WallSide& w) const {
        return std::hash<int>()(static_cast<int>(w.getValue()));
    }
};
}

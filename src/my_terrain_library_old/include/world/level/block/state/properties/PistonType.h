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
 * PistonType - Normal or sticky piston
 * Reference: net/minecraft/world/level/block/state/properties/PistonType.java
 */
class PistonType {
public:
    enum Value {
        DEFAULT,
        STICKY
    };

private:
    Value m_value;
    std::string m_name;

public:
    PistonType() : m_value(DEFAULT), m_name("normal") {}

    PistonType(Value value) : m_value(value) {
        switch (value) {
            case DEFAULT: m_name = "normal"; break;
            case STICKY:  m_name = "sticky"; break;
        }
    }

    std::string getSerializedName() const { return m_name; }
    Value getValue() const { return m_value; }

    bool operator==(const PistonType& other) const { return m_value == other.m_value; }
    bool operator!=(const PistonType& other) const { return m_value != other.m_value; }

    static PistonType normal() { return PistonType(DEFAULT); }
    static PistonType sticky() { return PistonType(STICKY); }

    static std::vector<PistonType> values() {
        return { PistonType(DEFAULT), PistonType(STICKY) };
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
struct hash<minecraft::world::level::block::state::properties::PistonType> {
    size_t operator()(const minecraft::world::level::block::state::properties::PistonType& p) const {
        return std::hash<int>()(static_cast<int>(p.getValue()));
    }
};
}

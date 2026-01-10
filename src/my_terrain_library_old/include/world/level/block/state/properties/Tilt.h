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
 * Tilt - Tilt state for big dripleaf
 * Reference: net/minecraft/world/level/block/state/properties/Tilt.java
 */
class Tilt {
public:
    enum Value {
        NONE,
        UNSTABLE,
        PARTIAL,
        FULL
    };

private:
    Value m_value;
    std::string m_name;
    bool m_causesVibration;

public:
    Tilt() : m_value(NONE), m_name("none"), m_causesVibration(false) {}

    Tilt(Value value) : m_value(value), m_causesVibration(value != NONE) {
        switch (value) {
            case NONE:     m_name = "none"; break;
            case UNSTABLE: m_name = "unstable"; break;
            case PARTIAL:  m_name = "partial"; break;
            case FULL:     m_name = "full"; break;
        }
    }

    std::string getSerializedName() const { return m_name; }
    Value getValue() const { return m_value; }
    bool causesVibration() const { return m_causesVibration; }

    bool operator==(const Tilt& other) const { return m_value == other.m_value; }
    bool operator!=(const Tilt& other) const { return m_value != other.m_value; }

    static Tilt none() { return Tilt(NONE); }
    static Tilt unstable() { return Tilt(UNSTABLE); }
    static Tilt partial() { return Tilt(PARTIAL); }
    static Tilt full() { return Tilt(FULL); }

    static std::vector<Tilt> values() {
        return { Tilt(NONE), Tilt(UNSTABLE), Tilt(PARTIAL), Tilt(FULL) };
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
struct hash<minecraft::world::level::block::state::properties::Tilt> {
    size_t operator()(const minecraft::world::level::block::state::properties::Tilt& t) const {
        return std::hash<int>()(static_cast<int>(t.getValue()));
    }
};
}

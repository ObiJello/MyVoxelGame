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
 * SculkSensorPhase - Phase of a sculk sensor
 * Reference: net/minecraft/world/level/block/state/properties/SculkSensorPhase.java
 */
class SculkSensorPhase {
public:
    enum Value {
        INACTIVE,
        ACTIVE,
        COOLDOWN
    };

private:
    Value m_value;
    std::string m_name;

public:
    SculkSensorPhase() : m_value(INACTIVE), m_name("inactive") {}

    SculkSensorPhase(Value value) : m_value(value) {
        switch (value) {
            case INACTIVE: m_name = "inactive"; break;
            case ACTIVE:   m_name = "active"; break;
            case COOLDOWN: m_name = "cooldown"; break;
        }
    }

    std::string getSerializedName() const { return m_name; }
    Value getValue() const { return m_value; }

    bool operator==(const SculkSensorPhase& other) const { return m_value == other.m_value; }
    bool operator!=(const SculkSensorPhase& other) const { return m_value != other.m_value; }

    static SculkSensorPhase inactive() { return SculkSensorPhase(INACTIVE); }
    static SculkSensorPhase active() { return SculkSensorPhase(ACTIVE); }
    static SculkSensorPhase cooldown() { return SculkSensorPhase(COOLDOWN); }

    static std::vector<SculkSensorPhase> values() {
        return { SculkSensorPhase(INACTIVE), SculkSensorPhase(ACTIVE), SculkSensorPhase(COOLDOWN) };
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
struct hash<minecraft::world::level::block::state::properties::SculkSensorPhase> {
    size_t operator()(const minecraft::world::level::block::state::properties::SculkSensorPhase& s) const {
        return std::hash<int>()(static_cast<int>(s.getValue()));
    }
};
}

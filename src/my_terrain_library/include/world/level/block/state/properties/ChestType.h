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
 * ChestType - Single, left, or right part of a chest
 * Reference: net/minecraft/world/level/block/state/properties/ChestType.java
 */
class ChestType {
public:
    enum Value {
        SINGLE,
        LEFT,
        RIGHT
    };

private:
    Value m_value;
    std::string m_name;

public:
    ChestType() : m_value(SINGLE), m_name("single") {}

    ChestType(Value value) : m_value(value) {
        switch (value) {
            case SINGLE: m_name = "single"; break;
            case LEFT:   m_name = "left"; break;
            case RIGHT:  m_name = "right"; break;
        }
    }

    std::string getSerializedName() const { return m_name; }
    Value getValue() const { return m_value; }

    ChestType getOpposite() const {
        switch (m_value) {
            case SINGLE: return ChestType(SINGLE);
            case LEFT:   return ChestType(RIGHT);
            case RIGHT:  return ChestType(LEFT);
            default:     return ChestType(SINGLE);
        }
    }

    bool operator==(const ChestType& other) const { return m_value == other.m_value; }
    bool operator!=(const ChestType& other) const { return m_value != other.m_value; }

    static ChestType single() { return ChestType(SINGLE); }
    static ChestType left() { return ChestType(LEFT); }
    static ChestType right() { return ChestType(RIGHT); }

    static std::vector<ChestType> values() {
        return { ChestType(SINGLE), ChestType(LEFT), ChestType(RIGHT) };
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
struct hash<minecraft::world::level::block::state::properties::ChestType> {
    size_t operator()(const minecraft::world::level::block::state::properties::ChestType& c) const {
        return std::hash<int>()(static_cast<int>(c.getValue()));
    }
};
}

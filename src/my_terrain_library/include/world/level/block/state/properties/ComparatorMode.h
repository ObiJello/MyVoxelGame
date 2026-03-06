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
 * ComparatorMode - Compare or subtract mode
 * Reference: net/minecraft/world/level/block/state/properties/ComparatorMode.java
 */
class ComparatorMode {
public:
    enum Value {
        COMPARE,
        SUBTRACT
    };

private:
    Value m_value;
    std::string m_name;

public:
    ComparatorMode() : m_value(COMPARE), m_name("compare") {}

    ComparatorMode(Value value) : m_value(value) {
        switch (value) {
            case COMPARE:  m_name = "compare"; break;
            case SUBTRACT: m_name = "subtract"; break;
        }
    }

    std::string getSerializedName() const { return m_name; }
    std::string toString() const { return m_name; }
    Value getValue() const { return m_value; }

    bool operator==(const ComparatorMode& other) const { return m_value == other.m_value; }
    bool operator!=(const ComparatorMode& other) const { return m_value != other.m_value; }

    static ComparatorMode compare() { return ComparatorMode(COMPARE); }
    static ComparatorMode subtract() { return ComparatorMode(SUBTRACT); }

    static std::vector<ComparatorMode> values() {
        return { ComparatorMode(COMPARE), ComparatorMode(SUBTRACT) };
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
struct hash<minecraft::world::level::block::state::properties::ComparatorMode> {
    size_t operator()(const minecraft::world::level::block::state::properties::ComparatorMode& c) const {
        return std::hash<int>()(static_cast<int>(c.getValue()));
    }
};
}

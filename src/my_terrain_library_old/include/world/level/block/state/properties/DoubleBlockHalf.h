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
 * DoubleBlockHalf - Upper or lower half of a double-height block
 * Reference: net/minecraft/world/level/block/state/properties/DoubleBlockHalf.java
 */
class DoubleBlockHalf {
public:
    enum Value {
        UPPER,
        LOWER
    };

private:
    Value m_value;

public:
    DoubleBlockHalf() : m_value(LOWER) {}

    DoubleBlockHalf(Value value) : m_value(value) {}

    std::string getSerializedName() const {
        return m_value == UPPER ? "upper" : "lower";
    }

    std::string toString() const { return getSerializedName(); }
    Value getValue() const { return m_value; }

    DoubleBlockHalf getOtherHalf() const {
        return DoubleBlockHalf(m_value == UPPER ? LOWER : UPPER);
    }

    bool operator==(const DoubleBlockHalf& other) const { return m_value == other.m_value; }
    bool operator!=(const DoubleBlockHalf& other) const { return m_value != other.m_value; }

    static DoubleBlockHalf upper() { return DoubleBlockHalf(UPPER); }
    static DoubleBlockHalf lower() { return DoubleBlockHalf(LOWER); }

    static std::vector<DoubleBlockHalf> values() {
        return { DoubleBlockHalf(UPPER), DoubleBlockHalf(LOWER) };
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
struct hash<minecraft::world::level::block::state::properties::DoubleBlockHalf> {
    size_t operator()(const minecraft::world::level::block::state::properties::DoubleBlockHalf& d) const {
        return std::hash<int>()(static_cast<int>(d.getValue()));
    }
};
}

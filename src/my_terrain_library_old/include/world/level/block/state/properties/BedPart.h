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
 * BedPart - Head or foot of a bed
 * Reference: net/minecraft/world/level/block/state/properties/BedPart.java
 */
class BedPart {
public:
    enum Value {
        HEAD,
        FOOT
    };

private:
    Value m_value;
    std::string m_name;

public:
    BedPart() : m_value(FOOT), m_name("foot") {}

    BedPart(Value value) : m_value(value) {
        switch (value) {
            case HEAD: m_name = "head"; break;
            case FOOT: m_name = "foot"; break;
        }
    }

    std::string getSerializedName() const { return m_name; }
    std::string toString() const { return m_name; }
    Value getValue() const { return m_value; }

    bool operator==(const BedPart& other) const { return m_value == other.m_value; }
    bool operator!=(const BedPart& other) const { return m_value != other.m_value; }

    static BedPart head() { return BedPart(HEAD); }
    static BedPart foot() { return BedPart(FOOT); }

    static std::vector<BedPart> values() {
        return { BedPart(HEAD), BedPart(FOOT) };
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
struct hash<minecraft::world::level::block::state::properties::BedPart> {
    size_t operator()(const minecraft::world::level::block::state::properties::BedPart& b) const {
        return std::hash<int>()(static_cast<int>(b.getValue()));
    }
};
}

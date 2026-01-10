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
 * DripstoneThickness - Thickness of pointed dripstone
 * Reference: net/minecraft/world/level/block/state/properties/DripstoneThickness.java
 */
class DripstoneThickness {
public:
    enum Value {
        TIP_MERGE,
        TIP,
        FRUSTUM,
        MIDDLE,
        BASE
    };

private:
    Value m_value;
    std::string m_name;

public:
    DripstoneThickness() : m_value(TIP), m_name("tip") {}

    DripstoneThickness(Value value) : m_value(value) {
        switch (value) {
            case TIP_MERGE: m_name = "tip_merge"; break;
            case TIP:       m_name = "tip"; break;
            case FRUSTUM:   m_name = "frustum"; break;
            case MIDDLE:    m_name = "middle"; break;
            case BASE:      m_name = "base"; break;
        }
    }

    std::string getSerializedName() const { return m_name; }
    Value getValue() const { return m_value; }

    bool operator==(const DripstoneThickness& other) const { return m_value == other.m_value; }
    bool operator!=(const DripstoneThickness& other) const { return m_value != other.m_value; }

    static DripstoneThickness tipMerge() { return DripstoneThickness(TIP_MERGE); }
    static DripstoneThickness tip() { return DripstoneThickness(TIP); }
    static DripstoneThickness frustum() { return DripstoneThickness(FRUSTUM); }
    static DripstoneThickness middle() { return DripstoneThickness(MIDDLE); }
    static DripstoneThickness base() { return DripstoneThickness(BASE); }

    static std::vector<DripstoneThickness> values() {
        return { DripstoneThickness(TIP_MERGE), DripstoneThickness(TIP),
                 DripstoneThickness(FRUSTUM), DripstoneThickness(MIDDLE),
                 DripstoneThickness(BASE) };
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
struct hash<minecraft::world::level::block::state::properties::DripstoneThickness> {
    size_t operator()(const minecraft::world::level::block::state::properties::DripstoneThickness& d) const {
        return std::hash<int>()(static_cast<int>(d.getValue()));
    }
};
}

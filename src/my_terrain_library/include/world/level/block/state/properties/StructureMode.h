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
 * StructureMode - Mode of a structure block
 * Reference: net/minecraft/world/level/block/state/properties/StructureMode.java
 */
class StructureMode {
public:
    enum Value {
        SAVE,
        LOAD,
        CORNER,
        DATA
    };

private:
    Value m_value;
    std::string m_name;

public:
    StructureMode() : m_value(SAVE), m_name("save") {}

    StructureMode(Value value) : m_value(value) {
        switch (value) {
            case SAVE:   m_name = "save"; break;
            case LOAD:   m_name = "load"; break;
            case CORNER: m_name = "corner"; break;
            case DATA:   m_name = "data"; break;
        }
    }

    std::string getSerializedName() const { return m_name; }
    Value getValue() const { return m_value; }

    bool operator==(const StructureMode& other) const { return m_value == other.m_value; }
    bool operator!=(const StructureMode& other) const { return m_value != other.m_value; }

    static StructureMode save() { return StructureMode(SAVE); }
    static StructureMode load() { return StructureMode(LOAD); }
    static StructureMode corner() { return StructureMode(CORNER); }
    static StructureMode data() { return StructureMode(DATA); }

    static std::vector<StructureMode> values() {
        return { StructureMode(SAVE), StructureMode(LOAD),
                 StructureMode(CORNER), StructureMode(DATA) };
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
struct hash<minecraft::world::level::block::state::properties::StructureMode> {
    size_t operator()(const minecraft::world::level::block::state::properties::StructureMode& s) const {
        return std::hash<int>()(static_cast<int>(s.getValue()));
    }
};
}

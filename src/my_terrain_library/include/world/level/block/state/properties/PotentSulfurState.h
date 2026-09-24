#pragma once

#include <string>
#include <vector>

namespace minecraft {
namespace world {
namespace level {
namespace block {
namespace state {
namespace properties {

/**
 * PotentSulfurState - state of a potent sulfur block (26.3)
 * Reference: net/minecraft/world/level/block/state/properties/PotentSulfurState.java
 */
class PotentSulfurState {
public:
    enum Value {
        DRY,
        WET,
        DORMANT,
        ERUPTING,
        CONTINUOUS
    };

private:
    Value m_value;

public:
    PotentSulfurState() : m_value(DRY) {}
    PotentSulfurState(Value value) : m_value(value) {}

    std::string getSerializedName() const {
        switch (m_value) {
            case DRY:        return "dry";
            case WET:        return "wet";
            case DORMANT:    return "dormant";
            case ERUPTING:   return "erupting";
            case CONTINUOUS: return "continuous";
        }
        return "dry";
    }
    Value getValue() const { return m_value; }

    bool operator==(const PotentSulfurState& other) const { return m_value == other.m_value; }
    bool operator!=(const PotentSulfurState& other) const { return m_value != other.m_value; }
    bool operator<(const PotentSulfurState& other) const { return m_value < other.m_value; }

    static std::vector<PotentSulfurState> values() {
        return {PotentSulfurState(DRY), PotentSulfurState(WET), PotentSulfurState(DORMANT),
                PotentSulfurState(ERUPTING), PotentSulfurState(CONTINUOUS)};
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
struct hash<minecraft::world::level::block::state::properties::PotentSulfurState> {
    size_t operator()(const minecraft::world::level::block::state::properties::PotentSulfurState& s) const {
        return std::hash<int>()(static_cast<int>(s.getValue()));
    }
};
}

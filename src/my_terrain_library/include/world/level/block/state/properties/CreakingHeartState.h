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
 * CreakingHeartState - State of a creaking heart block
 * Reference: net/minecraft/world/level/block/state/properties/CreakingHeartState.java
 */
class CreakingHeartState {
public:
    enum Value {
        UPROOTED,
        DORMANT,
        AWAKE
    };

private:
    Value m_value;
    std::string m_name;

public:
    CreakingHeartState() : m_value(UPROOTED), m_name("uprooted") {}

    CreakingHeartState(Value value) : m_value(value) {
        switch (value) {
            case UPROOTED: m_name = "uprooted"; break;
            case DORMANT:  m_name = "dormant"; break;
            case AWAKE:    m_name = "awake"; break;
        }
    }

    std::string getSerializedName() const { return m_name; }
    Value getValue() const { return m_value; }

    bool operator==(const CreakingHeartState& other) const { return m_value == other.m_value; }
    bool operator!=(const CreakingHeartState& other) const { return m_value != other.m_value; }
    bool operator<(const CreakingHeartState& other) const { return m_value < other.m_value; }

    static std::vector<CreakingHeartState> values() {
        return {CreakingHeartState(UPROOTED), CreakingHeartState(DORMANT), CreakingHeartState(AWAKE)};
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
struct hash<minecraft::world::level::block::state::properties::CreakingHeartState> {
    size_t operator()(const minecraft::world::level::block::state::properties::CreakingHeartState& s) const {
        return std::hash<int>()(static_cast<int>(s.getValue()));
    }
};
}

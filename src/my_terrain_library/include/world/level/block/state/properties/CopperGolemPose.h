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
 * CopperGolemPose - the pose of a copper golem statue (26.3)
 * Reference: net/minecraft/world/level/block/CopperGolemStatueBlock.java Pose
 */
class CopperGolemPose {
public:
    enum Value {
        STANDING,
        SITTING,
        RUNNING,
        STAR
    };

private:
    Value m_value;

public:
    CopperGolemPose() : m_value(STANDING) {}
    CopperGolemPose(Value value) : m_value(value) {}

    std::string getSerializedName() const {
        switch (m_value) {
            case STANDING: return "standing";
            case SITTING:  return "sitting";
            case RUNNING:  return "running";
            case STAR:     return "star";
        }
        return "standing";
    }
    Value getValue() const { return m_value; }

    bool operator==(const CopperGolemPose& other) const { return m_value == other.m_value; }
    bool operator!=(const CopperGolemPose& other) const { return m_value != other.m_value; }
    bool operator<(const CopperGolemPose& other) const { return m_value < other.m_value; }

    static std::vector<CopperGolemPose> values() {
        return {CopperGolemPose(STANDING), CopperGolemPose(SITTING), CopperGolemPose(RUNNING),
                CopperGolemPose(STAR)};
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
struct hash<minecraft::world::level::block::state::properties::CopperGolemPose> {
    size_t operator()(const minecraft::world::level::block::state::properties::CopperGolemPose& p) const {
        return std::hash<int>()(static_cast<int>(p.getValue()));
    }
};
}

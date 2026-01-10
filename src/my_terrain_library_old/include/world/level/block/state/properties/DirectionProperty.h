#pragma once

#include "Property.h"
#include "core/Direction.h"
#include <vector>
#include <optional>
#include <memory>
#include <unordered_map>

namespace minecraft {
namespace world {
namespace level {
namespace block {
namespace state {
namespace properties {

using core::Direction;

/**
 * DirectionProperty - Property for Direction values
 * Reference: net/minecraft/world/level/block/state/properties/DirectionProperty.java
 *
 * Specialized property for handling Direction enum values.
 */
class DirectionProperty : public Property<Direction> {
private:
    std::vector<Direction> m_values;
    std::unordered_map<std::string, Direction> m_nameToValue;
    std::unordered_map<Direction, int> m_valueToIndex;

    /**
     * Get the serialized name for a direction
     */
    static std::string getDirectionName(Direction dir) {
        switch (dir) {
            case Direction::DOWN:  return "down";
            case Direction::UP:    return "up";
            case Direction::NORTH: return "north";
            case Direction::SOUTH: return "south";
            case Direction::WEST:  return "west";
            case Direction::EAST:  return "east";
            default: return "unknown";
        }
    }

    /**
     * Private constructor - use create()
     */
    DirectionProperty(const std::string& name, const std::vector<Direction>& values)
        : Property<Direction>(name)
        , m_values(values)
    {
        for (size_t i = 0; i < values.size(); ++i) {
            std::string dirName = getDirectionName(values[i]);
            m_nameToValue[dirName] = values[i];
            m_valueToIndex[values[i]] = static_cast<int>(i);
        }
    }

public:
    /**
     * Create with all 6 directions
     * Reference: DirectionProperty.java create(String)
     */
    static std::unique_ptr<DirectionProperty> create(const std::string& name) {
        return std::unique_ptr<DirectionProperty>(new DirectionProperty(name, {
            Direction::NORTH, Direction::EAST, Direction::SOUTH, Direction::WEST,
            Direction::UP, Direction::DOWN
        }));
    }

    /**
     * Create with specific directions
     * Reference: DirectionProperty.java create(String, Direction...)
     */
    static std::unique_ptr<DirectionProperty> create(const std::string& name,
                                                      std::initializer_list<Direction> values) {
        return std::unique_ptr<DirectionProperty>(new DirectionProperty(name, std::vector<Direction>(values)));
    }

    /**
     * Create with horizontal directions only (N, E, S, W)
     */
    static std::unique_ptr<DirectionProperty> createHorizontal(const std::string& name) {
        return std::unique_ptr<DirectionProperty>(new DirectionProperty(name, {
            Direction::NORTH, Direction::EAST, Direction::SOUTH, Direction::WEST
        }));
    }

    const std::vector<Direction>& getPossibleValues() const override {
        return m_values;
    }

    std::string getName(const Direction& value) const override {
        return getDirectionName(value);
    }

    std::optional<Direction> getValue(const std::string& name) const override {
        auto it = m_nameToValue.find(name);
        if (it != m_nameToValue.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    int getInternalIndex(const Direction& value) const override {
        auto it = m_valueToIndex.find(value);
        if (it != m_valueToIndex.end()) {
            return it->second;
        }
        return -1;
    }
};

} // namespace properties
} // namespace state
} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft

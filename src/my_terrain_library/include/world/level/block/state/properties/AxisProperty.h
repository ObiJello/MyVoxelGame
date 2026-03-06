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

using core::Axis;

/**
 * AxisProperty - Property for Axis values (X, Y, Z)
 * Reference: EnumProperty<Direction.Axis>
 *
 * Specialized property for handling Axis enum values.
 */
class AxisProperty : public Property<Axis> {
private:
    std::vector<Axis> m_values;
    std::unordered_map<std::string, Axis> m_nameToValue;
    std::unordered_map<Axis, int> m_valueToIndex;

    /**
     * Get the serialized name for an axis
     */
    static std::string getAxisName(Axis axis) {
        switch (axis) {
            case Axis::X: return "x";
            case Axis::Y: return "y";
            case Axis::Z: return "z";
            default: return "unknown";
        }
    }

    /**
     * Private constructor - use create()
     */
    AxisProperty(const std::string& name, const std::vector<Axis>& values)
        : Property<Axis>(name)
        , m_values(values)
    {
        for (size_t i = 0; i < values.size(); ++i) {
            std::string axisName = getAxisName(values[i]);
            m_nameToValue[axisName] = values[i];
            m_valueToIndex[values[i]] = static_cast<int>(i);
        }
    }

public:
    /**
     * Create with all 3 axes
     * Reference: EnumProperty.create("axis", Direction.Axis.class)
     */
    static std::unique_ptr<AxisProperty> create(const std::string& name) {
        return std::unique_ptr<AxisProperty>(new AxisProperty(name, {
            Axis::X, Axis::Y, Axis::Z
        }));
    }

    /**
     * Create with horizontal axes only (X, Z)
     * Reference: HORIZONTAL_AXIS in BlockStateProperties
     */
    static std::unique_ptr<AxisProperty> createHorizontal(const std::string& name) {
        return std::unique_ptr<AxisProperty>(new AxisProperty(name, {
            Axis::X, Axis::Z
        }));
    }

    /**
     * Create with specific axes
     */
    static std::unique_ptr<AxisProperty> create(const std::string& name,
                                                 std::initializer_list<Axis> values) {
        return std::unique_ptr<AxisProperty>(new AxisProperty(name, std::vector<Axis>(values)));
    }

    const std::vector<Axis>& getPossibleValues() const override {
        return m_values;
    }

    std::string getName(const Axis& value) const override {
        return getAxisName(value);
    }

    std::optional<Axis> getValue(const std::string& name) const override {
        auto it = m_nameToValue.find(name);
        if (it != m_nameToValue.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    int getInternalIndex(const Axis& value) const override {
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

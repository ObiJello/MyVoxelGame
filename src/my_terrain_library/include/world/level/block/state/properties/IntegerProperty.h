#pragma once

#include "Property.h"
#include <vector>
#include <optional>
#include <memory>
#include <stdexcept>
#include <sstream>

namespace minecraft {
namespace world {
namespace level {
namespace block {
namespace state {
namespace properties {

/**
 * IntegerProperty - Property with a range of integer values
 * Reference: net/minecraft/world/level/block/state/properties/IntegerProperty.java
 *
 * Creates a property with values from min to max (inclusive).
 * Constraints: min >= 0, max > min
 */
class IntegerProperty : public Property<int> {
private:
    int m_min;
    int m_max;
    std::vector<int> m_values;

    /**
     * Private constructor - use create()
     * Reference: IntegerProperty.java IntegerProperty(String, int, int)
     */
    IntegerProperty(const std::string& name, int min, int max)
        : Property<int>(name)
        , m_min(min)
        , m_max(max)
    {
        // Reference: IntegerProperty.java lines 16-22 validation
        if (min < 0) {
            throw std::invalid_argument("Min value of " + name + " must be 0 or greater");
        }
        if (max <= min) {
            throw std::invalid_argument("Max value of " + name + " must be greater than min (" +
                                        std::to_string(min) + ")");
        }

        // Reference: IntegerProperty.java line 24 - create values list
        m_values.reserve(max - min + 1);
        for (int i = min; i <= max; ++i) {
            m_values.push_back(i);
        }
    }

public:
    /**
     * Factory method to create an IntegerProperty
     * Reference: IntegerProperty.java create(String, int, int)
     */
    static std::unique_ptr<IntegerProperty> create(const std::string& name, int min, int max) {
        return std::unique_ptr<IntegerProperty>(new IntegerProperty(name, min, max));
    }

    /**
     * Get all possible values (min to max inclusive)
     * Reference: IntegerProperty.java getPossibleValues()
     */
    const std::vector<int>& getPossibleValues() const override {
        return m_values;
    }

    /**
     * Get string representation of value
     * Reference: IntegerProperty.java getName(Integer)
     */
    std::string getName(const int& value) const override {
        return std::to_string(value);
    }

    /**
     * Parse string to integer value
     * Reference: IntegerProperty.java getValue(String)
     */
    std::optional<int> getValue(const std::string& name) const override {
        try {
            int value = std::stoi(name);
            // Validate it's in range
            if (value >= m_min && value <= m_max) {
                return value;
            }
        } catch (...) {
            // Parse error - return nullopt
        }
        return std::nullopt;
    }

    /**
     * Get internal index for neighbour lookup
     * Reference: IntegerProperty.java getInternalIndex(Integer)
     *
     * Returns value - min, or -1 if out of range.
     */
    int getInternalIndex(const int& value) const override {
        if (value >= m_min && value <= m_max) {
            return value - m_min;
        }
        return -1;
    }

    /**
     * Get minimum value
     */
    int getMin() const { return m_min; }

    /**
     * Get maximum value
     */
    int getMax() const { return m_max; }
};

} // namespace properties
} // namespace state
} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft

#pragma once

#include "Property.h"
#include <vector>
#include <optional>
#include <memory>

namespace minecraft {
namespace world {
namespace level {
namespace block {
namespace state {
namespace properties {

/**
 * BooleanProperty - Property with true/false values
 * Reference: net/minecraft/world/level/block/state/properties/BooleanProperty.java
 *
 * Always has exactly 2 values: true and false.
 * Internal indices: true=0, false=1
 */
class BooleanProperty : public Property<bool> {
private:
    static const std::vector<bool> VALUES;
    static constexpr int TRUE_INDEX = 0;
    static constexpr int FALSE_INDEX = 1;

    /**
     * Private constructor - use create()
     * Reference: BooleanProperty.java BooleanProperty(String)
     */
    BooleanProperty(const std::string& name) : Property<bool>(name) {}

public:
    /**
     * Factory method to create a BooleanProperty
     * Reference: BooleanProperty.java create(String)
     */
    static std::unique_ptr<BooleanProperty> create(const std::string& name) {
        return std::unique_ptr<BooleanProperty>(new BooleanProperty(name));
    }

    /**
     * Get all possible values (always [true, false])
     * Reference: BooleanProperty.java getPossibleValues()
     */
    const std::vector<bool>& getPossibleValues() const override {
        return VALUES;
    }

    /**
     * Get string representation of value
     * Reference: BooleanProperty.java getName(Boolean)
     */
    std::string getName(const bool& value) const override {
        return value ? "true" : "false";
    }

    /**
     * Parse string to boolean value
     * Reference: BooleanProperty.java getValue(String)
     */
    std::optional<bool> getValue(const std::string& name) const override {
        if (name == "true") {
            return true;
        } else if (name == "false") {
            return false;
        }
        return std::nullopt;
    }

    /**
     * Get internal index for neighbour lookup
     * Reference: BooleanProperty.java getInternalIndex(Boolean)
     *
     * true -> 0, false -> 1
     */
    int getInternalIndex(const bool& value) const override {
        return value ? TRUE_INDEX : FALSE_INDEX;
    }
};

// Static initialization
inline const std::vector<bool> BooleanProperty::VALUES = {true, false};

} // namespace properties
} // namespace state
} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft

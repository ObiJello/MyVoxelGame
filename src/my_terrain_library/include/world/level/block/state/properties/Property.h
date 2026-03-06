#pragma once

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <memory>
#include <any>

namespace minecraft {
namespace world {
namespace level {
namespace block {
namespace state {
namespace properties {

/**
 * PropertyBase - Non-templated base for type erasure
 * Allows storing different Property<T> types in containers
 * Reference: net/minecraft/world/level/block/state/properties/Property.java
 */
class PropertyBase {
public:
    virtual ~PropertyBase() = default;

    /**
     * Get the property name
     * Reference: Property.java getName()
     */
    virtual const std::string& getName() const = 0;

    /**
     * Get number of possible values
     * Reference: Property.java getPossibleValues().size()
     */
    virtual size_t getValueCount() const = 0;

    /**
     * Get all possible value names (for serialization)
     */
    virtual std::vector<std::string> getValueNames() const = 0;

    /**
     * Get value name by index
     * Used for serialization when we have the index stored in ValueMap
     */
    virtual std::string getValueNameByIndex(size_t index) const = 0;

    /**
     * Get value at index as type-erased std::any
     * Reference: Property.java getPossibleValues().get(index)
     * Used by StateDefinition to store actual values in ValueMap
     */
    virtual std::any getValueAtIndex(size_t index) const = 0;

    /**
     * Get string representation from type-erased value
     * Reference: Property.java getName(T value)
     * Used by getProperties() to serialize values to strings
     */
    virtual std::string getNameFromAny(const std::any& value) const = 0;

    /**
     * Get internal index from type-erased value
     * Reference: Property.java getInternalIndex(T value)
     * Used by setValueInternal for neighbour lookup
     */
    virtual int getInternalIndexFromAny(const std::any& value) const = 0;

    /**
     * Check equality by name (properties with same name are considered equal)
     */
    bool operator==(const PropertyBase& other) const {
        return getName() == other.getName();
    }
};

/**
 * Property<T> - Abstract base for block state properties
 * Reference: net/minecraft/world/level/block/state/properties/Property.java
 *
 * Properties define what values a block state can have.
 * Each property has a name and a set of possible values.
 *
 * @tparam T - The value type (bool, int, enum, etc.)
 */
template<typename T>
class Property : public PropertyBase {
protected:
    std::string m_name;

    /**
     * Protected constructor
     * Reference: Property.java Property(String, Class)
     */
    Property(const std::string& name) : m_name(name) {}

public:
    virtual ~Property() = default;

    /**
     * Get the property name
     * Reference: Property.java getName()
     */
    const std::string& getName() const override {
        return m_name;
    }

    /**
     * Get all possible values for this property
     * Reference: Property.java getPossibleValues()
     */
    virtual const std::vector<T>& getPossibleValues() const = 0;

    /**
     * Get number of possible values
     */
    size_t getValueCount() const override {
        return getPossibleValues().size();
    }

    /**
     * Get the string representation of a value
     * Reference: Property.java getName(T)
     */
    virtual std::string getName(const T& value) const = 0;

    /**
     * Parse a string to get the value
     * Reference: Property.java getValue(String)
     */
    virtual std::optional<T> getValue(const std::string& name) const = 0;

    /**
     * Get the internal index of a value for neighbour array lookup
     * Reference: Property.java getInternalIndex(T)
     *
     * Returns -1 if the value is not valid for this property.
     */
    virtual int getInternalIndex(const T& value) const = 0;

    /**
     * Get all possible value names
     */
    std::vector<std::string> getValueNames() const override {
        std::vector<std::string> names;
        for (const auto& value : getPossibleValues()) {
            names.push_back(getName(value));
        }
        return names;
    }

    /**
     * Get value at index
     */
    const T& getValueByIndex(int index) const {
        return getPossibleValues()[index];
    }

    /**
     * Get value name by index
     */
    std::string getValueNameByIndex(size_t index) const override {
        if (index < getPossibleValues().size()) {
            return getName(getPossibleValues()[index]);
        }
        return "";
    }

    /**
     * Get value at index as type-erased std::any
     * Reference: Property.java getPossibleValues().get(index)
     */
    std::any getValueAtIndex(size_t index) const override {
        if (index < getPossibleValues().size()) {
            return std::any(getPossibleValues()[index]);
        }
        return std::any();
    }

    /**
     * Get string representation from type-erased value
     * Reference: Property.java getName(T value)
     */
    std::string getNameFromAny(const std::any& value) const override {
        try {
            return getName(std::any_cast<T>(value));
        } catch (const std::bad_any_cast&) {
            return "";
        }
    }

    /**
     * Get internal index from type-erased value
     * Reference: Property.java getInternalIndex(T value)
     */
    int getInternalIndexFromAny(const std::any& value) const override {
        try {
            return getInternalIndex(std::any_cast<T>(value));
        } catch (const std::bad_any_cast&) {
            return -1;
        }
    }
};

} // namespace properties
} // namespace state
} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft

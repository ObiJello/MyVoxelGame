#pragma once

#include "world/level/block/state/properties/Property.h"
#include <unordered_map>
#include <map>
#include <any>
#include <vector>
#include <memory>
#include <stdexcept>
#include <functional>

namespace minecraft {
namespace world {
namespace level {
namespace block {
namespace state {

// Forward declarations
template<typename O, typename S> class StateDefinition;

using properties::Property;
using properties::PropertyBase;

/**
 * ValueMap - Maps properties to their values
 * Using std::map for consistent iteration order
 */
using ValueMap = std::map<const PropertyBase*, std::any>;

/**
 * StateHolder<O, S> - Base class for objects that hold property values
 * Reference: net/minecraft/world/level/block/state/StateHolder.java
 *
 * States are immutable - setValue() returns a new state, doesn't modify this one.
 * Uses pre-computed neighbour arrays for O(1) state transitions.
 *
 * @tparam O - Owner type (e.g., Block)
 * @tparam S - State type (e.g., BlockState)
 */
template<typename O, typename S>
class StateHolder {
public:
    // Reference: StateHolder.java NAME_TAG, PROPERTIES_TAG
    static constexpr const char* NAME_TAG = "Name";
    static constexpr const char* PROPERTIES_TAG = "Properties";

protected:
    O* m_owner;
    ValueMap m_values;
    std::unordered_map<const PropertyBase*, std::vector<S*>> m_neighbours;

    /**
     * Constructor
     * Reference: StateHolder.java StateHolder(O, Map, MapCodec)
     */
    StateHolder(O* owner, const ValueMap& values)
        : m_owner(owner)
        , m_values(values)
    {}

public:
    virtual ~StateHolder() = default;

    /**
     * Get the owner (e.g., the Block for BlockState)
     */
    O* getOwner() const { return m_owner; }

    /**
     * Get value of a property
     * Reference: StateHolder.java getValue(Property) lines 84-92
     *
     * @throws std::invalid_argument if property doesn't exist
     */
    template<typename T>
    T getValue(const Property<T>& property) const {
        auto it = m_values.find(&property);
        if (it == m_values.end()) {
            throw std::invalid_argument("Cannot get property " + property.getName() +
                                        " as it does not exist in this state");
        }
        return std::any_cast<T>(it->second);
    }

    /**
     * Get optional value of a property
     * Reference: StateHolder.java getOptionalValue(Property) lines 94-96
     */
    template<typename T>
    std::optional<T> getOptionalValue(const Property<T>& property) const {
        auto it = m_values.find(&property);
        if (it == m_values.end()) {
            return std::nullopt;
        }
        try {
            return std::any_cast<T>(it->second);
        } catch (...) {
            return std::nullopt;
        }
    }

    /**
     * Get value or default
     * Reference: StateHolder.java getValueOrElse(Property, T) lines 98-100
     */
    template<typename T>
    T getValueOrElse(const Property<T>& property, const T& defaultValue) const {
        auto opt = getOptionalValue(property);
        return opt.has_value() ? opt.value() : defaultValue;
    }

    /**
     * Set value of a property, returning new state
     * Reference: StateHolder.java setValue(Property, V) lines 107-115
     *
     * Does NOT modify this state - returns a new state with the changed value.
     *
     * @throws std::invalid_argument if property doesn't exist or value is invalid
     */
    template<typename T>
    S* setValue(const Property<T>& property, const T& value) {
        auto it = m_values.find(&property);
        if (it == m_values.end()) {
            throw std::invalid_argument("Cannot set property " + property.getName() +
                                        " as it does not exist in this state");
        }

        T oldValue = std::any_cast<T>(it->second);
        return setValueInternal(property, value, oldValue);
    }

    /**
     * Try to set value, returning this state if property doesn't exist
     * Reference: StateHolder.java trySetValue(Property, V) lines 117-120
     */
    template<typename T>
    S* trySetValue(const Property<T>& property, const T& value) {
        auto it = m_values.find(&property);
        if (it == m_values.end()) {
            return static_cast<S*>(this);
        }

        T oldValue = std::any_cast<T>(it->second);
        return setValueInternal(property, value, oldValue);
    }

    /**
     * Cycle through property values
     * Reference: StateHolder.java cycle(Property) lines 47-49
     */
    template<typename T>
    S* cycle(const Property<T>& property) {
        T current = getValue(property);
        const auto& values = property.getPossibleValues();
        int currentIndex = property.getInternalIndex(current);
        int nextIndex = (currentIndex + 1) % values.size();
        return setValue(property, values[nextIndex]);
    }

    /**
     * Check if this state has a property
     * Reference: StateHolder.java hasProperty(Property) lines 80-82
     */
    bool hasProperty(const PropertyBase* property) const {
        return m_values.find(property) != m_values.end();
    }

    /**
     * Get all property values
     * Reference: StateHolder.java getValues() line 159
     */
    const ValueMap& getValues() const {
        return m_values;
    }

    /**
     * Get all properties
     * Reference: StateHolder.java getProperties() lines 76-78
     */
    std::vector<const PropertyBase*> getProperties() const {
        std::vector<const PropertyBase*> props;
        for (const auto& [prop, value] : m_values) {
            props.push_back(prop);
        }
        return props;
    }

    /**
     * Populate neighbour arrays for O(1) state transitions
     * Reference: StateHolder.java populateNeighbours(Map) lines 136-151
     *
     * Called by StateDefinition after all states are created.
     *
     * @param statesByKey Map of string keys to states
     * @param keyFunc Function to compute string key from ValueMap
     */
    void populateNeighbours(const std::map<std::string, S*>& statesByKey,
                            std::function<std::string(const ValueMap&)> keyFunc) {
        if (!m_neighbours.empty()) {
            throw std::logic_error("Neighbours already populated");
        }

        for (const auto& [prop, value] : m_values) {
            std::vector<S*> neighbours;

            // Get the property's possible values
            auto names = prop->getValueNames();
            neighbours.reserve(names.size());

            // For each possible value, find the corresponding state
            for (size_t i = 0; i < prop->getValueCount(); ++i) {
                ValueMap neighbourValues = makeNeighbourValues(prop, i);
                std::string key = keyFunc(neighbourValues);
                auto it = statesByKey.find(key);
                if (it != statesByKey.end()) {
                    neighbours.push_back(it->second);
                } else {
                    neighbours.push_back(nullptr);
                }
            }

            m_neighbours[prop] = std::move(neighbours);
        }
    }

protected:
    /**
     * Internal method to set value using pre-computed neighbours
     * Reference: StateHolder.java setValueInternal(Property, V, Comparable) lines 122-134
     */
    template<typename T>
    S* setValueInternal(const Property<T>& property, const T& value, const T& oldValue) {
        // If same value, return this
        if (oldValue == value) {
            return static_cast<S*>(this);
        }

        int internalIndex = property.getInternalIndex(value);
        if (internalIndex < 0) {
            throw std::invalid_argument("Cannot set property " + property.getName() +
                                        " to invalid value");
        }

        // Look up in neighbour array
        auto it = m_neighbours.find(&property);
        if (it == m_neighbours.end() || internalIndex >= static_cast<int>(it->second.size())) {
            throw std::logic_error("Neighbour array not properly initialized");
        }

        return it->second[internalIndex];
    }

    /**
     * Create values map for a neighbour state
     * Reference: StateHolder.java makeNeighbourValues(Property, Comparable) lines 153-157
     */
    ValueMap makeNeighbourValues(const PropertyBase* property, size_t valueIndex) const {
        ValueMap neighbour = m_values;

        // Store actual value at the given index (like Java stores Comparable<?>)
        // Reference: StateHolder.java line 154-156
        if (valueIndex < property->getValueCount()) {
            neighbour[property] = property->getValueAtIndex(valueIndex);
        }

        return neighbour;
    }

    /**
     * For StateDefinition to set values directly during construction
     */
    template<typename T>
    void setValueDirect(const Property<T>& property, const T& value) {
        m_values[&property] = value;
    }

    friend class StateDefinition<O, S>;
};

} // namespace state
} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft

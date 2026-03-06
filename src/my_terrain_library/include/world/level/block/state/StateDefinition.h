#pragma once

#include "StateHolder.h"
#include "world/level/block/state/properties/Property.h"
#include <map>
#include <vector>
#include <memory>
#include <functional>
#include <regex>
#include <stdexcept>

namespace minecraft {
namespace world {
namespace level {
namespace block {
namespace state {

using properties::Property;
using properties::PropertyBase;

/**
 * StateDefinition<O, S> - Factory that creates all possible state combinations
 * Reference: net/minecraft/world/level/block/state/StateDefinition.java
 *
 * Given a set of properties, generates the Cartesian product of all possible
 * value combinations as immutable state objects.
 *
 * @tparam O - Owner type (e.g., Block)
 * @tparam S - State type (e.g., BlockState)
 */
template<typename O, typename S>
class StateDefinition {
public:
    /**
     * Factory function type for creating states
     */
    using StateFactory = std::function<S*(O*, const ValueMap&)>;

private:
    static const std::regex NAME_PATTERN;

    O* m_owner;
    std::map<std::string, const PropertyBase*> m_propertiesByName;  // Sorted by name
    std::vector<std::unique_ptr<S>> m_states;
    std::vector<S*> m_statePointers;

public:
    /**
     * Builder for constructing StateDefinition
     * Reference: StateDefinition.java Builder inner class
     */
    class Builder {
    private:
        O* m_owner;
        std::map<std::string, const PropertyBase*> m_properties;

    public:
        /**
         * Constructor
         * Reference: StateDefinition.Builder(O)
         */
        Builder(O* owner) : m_owner(owner) {}

        /**
         * Add a property
         * Reference: StateDefinition.Builder.add(Property...) lines 116-122
         */
        Builder& add(const PropertyBase* property) {
            validateProperty(property);
            m_properties[property->getName()] = property;
            return *this;
        }

        /**
         * Add multiple properties
         */
        template<typename... Props>
        Builder& add(const PropertyBase* first, Props*... rest) {
            add(first);
            if constexpr (sizeof...(rest) > 0) {
                add(rest...);
            }
            return *this;
        }

        /**
         * Create the StateDefinition
         * Reference: StateDefinition.Builder.create(Function, Factory) lines 148-151
         */
        StateDefinition<O, S> create(std::function<S*(O*)> defaultState,
                                      StateFactory factory) {
            return StateDefinition<O, S>(m_owner, m_properties, defaultState, factory);
        }

    private:
        /**
         * Validate property name and values
         * Reference: StateDefinition.Builder.validateProperty(Property) lines 124-146
         */
        void validateProperty(const PropertyBase* property) {
            const std::string& name = property->getName();

            // Check name format
            // Reference: StateDefinition.java NAME_PATTERN = ^[a-z0-9_]+$
            std::regex pattern("^[a-z0-9_]+$");
            if (!std::regex_match(name, pattern)) {
                throw std::invalid_argument("Property '" + name +
                    "' has invalid name (must be lowercase alphanumeric with underscores)");
            }

            // Check not already added
            if (m_properties.find(name) != m_properties.end()) {
                throw std::invalid_argument("Property '" + name + "' already added");
            }

            // Check has at least 2 values
            if (property->getValueCount() < 2) {
                throw std::invalid_argument("Property '" + name +
                    "' must have at least 2 possible values");
            }

            // Check value names
            for (const auto& valueName : property->getValueNames()) {
                if (!std::regex_match(valueName, pattern)) {
                    throw std::invalid_argument("Property '" + name +
                        "' has invalid value name '" + valueName + "'");
                }
            }
        }
    };

private:
    /**
     * Private constructor - use Builder
     * Reference: StateDefinition.StateDefinition(Function, O, Factory, Map) lines 33-74
     */
    StateDefinition(O* owner,
                    const std::map<std::string, const PropertyBase*>& properties,
                    std::function<S*(O*)> /*defaultState*/,
                    StateFactory factory)
        : m_owner(owner)
        , m_propertiesByName(properties)
    {
        // Generate all combinations of property values
        // Reference: StateDefinition.java lines 44-67

        // Start with empty combination
        std::vector<ValueMap> combinations;
        combinations.push_back(ValueMap{});

        // For each property, expand combinations with all possible values
        // Reference: StateDefinition.java lines 49-55
        for (const auto& [name, property] : m_propertiesByName) {
            std::vector<ValueMap> newCombinations;

            for (const auto& combo : combinations) {
                // Add each possible value
                // Reference: StateDefinition.java line 50-52 - stores actual Comparable<?> values
                for (size_t i = 0; i < property->getValueCount(); ++i) {
                    ValueMap newCombo = combo;
                    // Store actual value wrapped in std::any (like Java stores Comparable<?>)
                    newCombo[property] = property->getValueAtIndex(i);
                    newCombinations.push_back(std::move(newCombo));
                }
            }

            combinations = std::move(newCombinations);
        }

        // Create state objects for each combination
        // Use string key for lookup since ValueMap contains std::any which doesn't support <
        std::unordered_map<std::string, S*> statesByKey;
        std::unordered_map<std::string, ValueMap> keyToValues;
        m_states.reserve(combinations.size());
        m_statePointers.reserve(combinations.size());

        for (const auto& combo : combinations) {
            // Create unique key from property indices
            std::string key = computeValueMapKey(combo);

            // Create state with the factory
            auto state = std::unique_ptr<S>(factory(owner, combo));
            S* statePtr = state.get();
            statesByKey[key] = statePtr;
            keyToValues[key] = combo;
            m_statePointers.push_back(statePtr);
            m_states.push_back(std::move(state));
        }

        // Build ValueMap to state lookup for neighbour population
        std::map<std::string, S*> statesByValueKey;
        for (const auto& [key, state] : statesByKey) {
            statesByValueKey[key] = state;
        }

        // Populate neighbour arrays for O(1) transitions
        // Reference: StateDefinition.java lines 69-71
        for (auto& state : m_states) {
            state->populateNeighbours(statesByValueKey, [this](const ValueMap& v) {
                return computeValueMapKey(v);
            });
        }
    }

    /**
     * Compute a unique string key for a ValueMap
     * Uses property indices which are comparable
     */
    static std::string computeValueMapKey(const ValueMap& values) {
        std::string key;
        for (const auto& [prop, value] : values) {
            int index = prop->getInternalIndexFromAny(value);
            key += prop->getName() + ":" + std::to_string(index) + ";";
        }
        return key;
    }

public:
    /**
     * Get all possible states
     * Reference: StateDefinition.java getPossibleStates() lines 81-83
     */
    const std::vector<S*>& getPossibleStates() const {
        return m_statePointers;
    }

    /**
     * Get any state (first one)
     * Reference: StateDefinition.java any() lines 85-87
     */
    S* any() const {
        return m_statePointers.empty() ? nullptr : m_statePointers[0];
    }

    /**
     * Get owner
     * Reference: StateDefinition.java getOwner() lines 89-91
     */
    O* getOwner() const {
        return m_owner;
    }

    /**
     * Get property by name
     * Reference: StateDefinition.java getProperty(String) lines 101-103
     */
    const PropertyBase* getProperty(const std::string& name) const {
        auto it = m_propertiesByName.find(name);
        return it != m_propertiesByName.end() ? it->second : nullptr;
    }

    /**
     * Get all properties
     * Reference: StateDefinition.java getProperties() lines 93-95
     */
    const std::map<std::string, const PropertyBase*>& getProperties() const {
        return m_propertiesByName;
    }

    /**
     * Get number of states
     */
    size_t getStateCount() const {
        return m_states.size();
    }
};

} // namespace state
} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft

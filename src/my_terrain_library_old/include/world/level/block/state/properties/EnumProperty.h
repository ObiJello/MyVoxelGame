#pragma once

#include "Property.h"
#include "util/StringRepresentable.h"
#include <vector>
#include <optional>
#include <memory>
#include <unordered_map>
#include <stdexcept>
#include <type_traits>
#include <algorithm>

namespace minecraft {
namespace world {
namespace level {
namespace block {
namespace state {
namespace properties {

/**
 * EnumProperty<T> - Property with enum values
 * Reference: net/minecraft/world/level/block/state/properties/EnumProperty.java
 *
 * The enum type T must provide a getSerializedName() method (like StringRepresentable).
 * Values are looked up by their serialized names.
 *
 * @tparam T - Enum type that provides getSerializedName()
 */
template<typename T>
class EnumProperty : public Property<T> {
private:
    std::vector<T> m_values;
    std::unordered_map<std::string, T> m_names;
    std::unordered_map<T, int> m_valueToIndex;

    /**
     * Private constructor - use create()
     * Reference: EnumProperty.java EnumProperty(String, Class, List)
     */
    EnumProperty(const std::string& name, const std::vector<T>& values)
        : Property<T>(name)
        , m_values(values)
    {
        // Reference: EnumProperty.java lines 20-22 validation
        if (values.empty()) {
            throw std::invalid_argument("Trying to make empty EnumProperty '" + name + "'");
        }

        // Reference: EnumProperty.java lines 29-33 - build name lookup map
        for (size_t i = 0; i < values.size(); ++i) {
            const T& value = values[i];
            std::string serializedName = getSerializedName(value);
            m_names[serializedName] = value;
            m_valueToIndex[value] = static_cast<int>(i);
        }
    }

    /**
     * Helper to get serialized name from value
     * Works with types that have getSerializedName() method
     */
    template<typename U = T>
    static auto getSerializedName(const U& value)
        -> decltype(value.getSerializedName(), std::string()) {
        return value.getSerializedName();
    }

public:
    /**
     * Factory method to create an EnumProperty with all enum values
     * Reference: EnumProperty.java create(String, Class)
     *
     * Note: Caller must provide the values since C++ doesn't have enum reflection
     */
    static std::unique_ptr<EnumProperty<T>> create(const std::string& name,
                                                    const std::vector<T>& values) {
        return std::unique_ptr<EnumProperty<T>>(new EnumProperty<T>(name, values));
    }

    /**
     * Factory method with predicate filter
     * Reference: EnumProperty.java create(String, Class, Predicate)
     */
    template<typename Predicate>
    static std::unique_ptr<EnumProperty<T>> create(const std::string& name,
                                                    const std::vector<T>& allValues,
                                                    Predicate filter) {
        std::vector<T> filtered;
        for (const auto& value : allValues) {
            if (filter(value)) {
                filtered.push_back(value);
            }
        }
        return std::unique_ptr<EnumProperty<T>>(new EnumProperty<T>(name, filtered));
    }

    /**
     * Factory method with initializer list
     */
    static std::unique_ptr<EnumProperty<T>> create(const std::string& name,
                                                    std::initializer_list<T> values) {
        return std::unique_ptr<EnumProperty<T>>(new EnumProperty<T>(name, std::vector<T>(values)));
    }

    /**
     * Get all possible values
     * Reference: EnumProperty.java getPossibleValues()
     */
    const std::vector<T>& getPossibleValues() const override {
        return m_values;
    }

    /**
     * Get string representation of value
     * Reference: EnumProperty.java getName(T)
     */
    std::string getName(const T& value) const override {
        return getSerializedName(value);
    }

    /**
     * Parse string to enum value
     * Reference: EnumProperty.java getValue(String)
     */
    std::optional<T> getValue(const std::string& name) const override {
        auto it = m_names.find(name);
        if (it != m_names.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    /**
     * Get internal index for neighbour lookup
     * Reference: EnumProperty.java getInternalIndex(T)
     */
    int getInternalIndex(const T& value) const override {
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

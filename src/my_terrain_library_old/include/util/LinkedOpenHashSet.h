#pragma once

#include <unordered_set>
#include <unordered_map>
#include <list>
#include <stdexcept>

// C++ equivalent of Java's LongLinkedOpenHashSet
// Maintains insertion order + O(1) operations for add/remove/contains

namespace minecraft {
namespace util {

/**
 * LinkedOpenHashSet - A hash set that maintains insertion order
 *
 * This is equivalent to Java's fastutil LongLinkedOpenHashSet.
 * It provides:
 * - O(1) add, remove, contains
 * - O(1) removeFirst (removes the oldest inserted element)
 * - Iteration in insertion order
 */
template<typename T>
class LinkedOpenHashSet {
public:
    LinkedOpenHashSet() = default;

    explicit LinkedOpenHashSet(size_t initialCapacity, float loadFactor = 0.5f) {
        m_set.reserve(initialCapacity);
    }

    /**
     * Add a value to the set
     * @return true if the value was added, false if it already existed
     */
    bool add(const T& value) {
        if (m_set.find(value) != m_set.end()) {
            return false;
        }
        m_order.push_back(value);
        auto it = std::prev(m_order.end());
        m_iterators[value] = it;
        m_set.insert(value);
        return true;
    }

    /**
     * Remove a value from the set
     * @return true if the value was removed, false if it didn't exist
     */
    bool remove(const T& value) {
        auto setIt = m_set.find(value);
        if (setIt == m_set.end()) {
            return false;
        }
        auto mapIt = m_iterators.find(value);
        if (mapIt != m_iterators.end()) {
            m_order.erase(mapIt->second);
            m_iterators.erase(mapIt);
        }
        m_set.erase(setIt);
        return true;
    }

    /**
     * Remove and return the first (oldest) inserted element
     * @return The first element
     * @throws std::out_of_range if the set is empty
     */
    T removeFirstLong() {
        if (m_order.empty()) {
            throw std::out_of_range("LinkedOpenHashSet is empty");
        }
        T value = m_order.front();
        m_order.pop_front();
        m_iterators.erase(value);
        m_set.erase(value);
        return value;
    }

    /**
     * Check if the set contains a value
     */
    bool contains(const T& value) const {
        return m_set.find(value) != m_set.end();
    }

    /**
     * Check if the set is empty
     */
    bool isEmpty() const {
        return m_set.empty();
    }

    /**
     * Get the number of elements
     */
    size_t size() const {
        return m_set.size();
    }

    /**
     * Clear all elements
     */
    void clear() {
        m_set.clear();
        m_order.clear();
        m_iterators.clear();
    }

    // Iterator support for range-based for loops
    typename std::list<T>::const_iterator begin() const {
        return m_order.begin();
    }

    typename std::list<T>::const_iterator end() const {
        return m_order.end();
    }

    typename std::list<T>::iterator begin() {
        return m_order.begin();
    }

    typename std::list<T>::iterator end() {
        return m_order.end();
    }

private:
    std::unordered_set<T> m_set;
    std::list<T> m_order;
    std::unordered_map<T, typename std::list<T>::iterator> m_iterators;
};

// Type alias for common use case
using LongLinkedOpenHashSet = LinkedOpenHashSet<int64_t>;

} // namespace util
} // namespace minecraft

#pragma once

#include <vector>
#include <unordered_map>
#include <iterator>

namespace minecraft {
namespace core {

/**
 * IdMapper<T> - Maps objects to integer IDs
 * Reference: net/minecraft/core/IdMapper.java
 *
 * Provides bidirectional mapping between objects and integer IDs.
 * Used for block state registry and other ID-based lookups.
 */
template<typename T>
class IdMapper {
private:
    int m_nextId = 0;
    std::unordered_map<T, int> m_tToId;
    std::vector<T> m_idToT;

public:
    /**
     * Default constructor
     * Reference: IdMapper.java IdMapper()
     */
    IdMapper() : IdMapper(512) {}

    /**
     * Constructor with expected size
     * Reference: IdMapper.java IdMapper(int)
     */
    explicit IdMapper(int expectedSize) {
        m_idToT.reserve(expectedSize);
    }

    /**
     * Add mapping with specific ID
     * Reference: IdMapper.java addMapping(T, int) lines 27-38
     */
    void addMapping(T thing, int id) {
        m_tToId[thing] = id;

        // Grow vector if needed
        while (static_cast<int>(m_idToT.size()) <= id) {
            m_idToT.push_back(T{});
        }

        m_idToT[id] = thing;
        if (m_nextId <= id) {
            m_nextId = id + 1;
        }
    }

    /**
     * Add with auto-assigned ID
     * Reference: IdMapper.java add(T) lines 41-43
     */
    void add(T thing) {
        addMapping(thing, m_nextId);
    }

    /**
     * Get ID for object
     * Reference: IdMapper.java getId(T) lines 45-47
     *
     * @return ID or -1 if not found
     */
    int getId(T thing) const {
        auto it = m_tToId.find(thing);
        return it != m_tToId.end() ? it->second : -1;
    }

    /**
     * Get object by ID
     * Reference: IdMapper.java byId(int) lines 49-51
     *
     * @return Object or default T{} if not found
     */
    T byId(int id) const {
        if (id >= 0 && id < static_cast<int>(m_idToT.size())) {
            return m_idToT[id];
        }
        return T{};
    }

    /**
     * Check if ID exists
     * Reference: IdMapper.java contains(int) lines 57-59
     */
    bool contains(int id) const {
        return byId(id) != T{};
    }

    /**
     * Get number of mappings
     * Reference: IdMapper.java size() lines 61-63
     */
    int size() const {
        return static_cast<int>(m_tToId.size());
    }

    /**
     * Get next available ID
     */
    int getNextId() const {
        return m_nextId;
    }

    /**
     * Iterator support
     */
    typename std::vector<T>::const_iterator begin() const {
        return m_idToT.begin();
    }

    typename std::vector<T>::const_iterator end() const {
        return m_idToT.end();
    }
};

} // namespace core
} // namespace minecraft

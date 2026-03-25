#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace minecraft {
namespace util {

/**
 * JavaHashSet - Minimal HashSet implementation with Java-style iteration order.
 *
 * This mirrors java.util.HashSet backed by HashMap closely enough for parity-
 * sensitive worldgen code:
 * - default capacity 16
 * - load factor 0.75
 * - hash spreading via h ^ (h >>> 16)
 * - iteration by bucket order, then insertion order within a bucket
 */
template <typename T>
class JavaHashSet {
private:
    struct Node {
        T value;
        int32_t hash;
        Node* next = nullptr;

        Node(const T& v, int32_t h) : value(v), hash(h) {}
    };

    static constexpr size_t DEFAULT_INITIAL_CAPACITY = 16;
    static constexpr float LOAD_FACTOR = 0.75f;

    std::vector<Node*> m_table;
    std::vector<std::unique_ptr<Node>> m_nodes;
    size_t m_size = 0;
    size_t m_threshold = 0;

    static int32_t spread(int32_t hash) {
        uint32_t h = static_cast<uint32_t>(hash);
        return static_cast<int32_t>(h ^ (h >> 16));
    }

    static size_t bucketIndex(int32_t hash, size_t capacity) {
        return static_cast<size_t>(static_cast<uint32_t>(hash)) & (capacity - 1);
    }

    void initialize(size_t capacity = DEFAULT_INITIAL_CAPACITY) {
        size_t powerOfTwo = DEFAULT_INITIAL_CAPACITY;
        while (powerOfTwo < capacity) {
            powerOfTwo <<= 1;
        }
        m_table.assign(powerOfTwo, nullptr);
        m_threshold = std::max<size_t>(1, static_cast<size_t>(static_cast<float>(powerOfTwo) * LOAD_FACTOR));
    }

    void rehash(size_t newCapacity) {
        // Match java.util.HashMap.resize(): each old bucket is split into low/high
        // lists based on (hash & oldCap), preserving relative order within each list.
        std::vector<Node*> newTable(newCapacity, nullptr);
        size_t oldCapacity = m_table.size();

        for (size_t i = 0; i < oldCapacity; ++i) {
            Node* bucket = m_table[i];
            if (!bucket) {
                continue;
            }

            Node* loHead = nullptr;
            Node* loTail = nullptr;
            Node* hiHead = nullptr;
            Node* hiTail = nullptr;

            while (bucket) {
                Node* next = bucket->next;
                bucket->next = nullptr;

                if ((static_cast<size_t>(static_cast<uint32_t>(bucket->hash)) & oldCapacity) == 0) {
                    if (!loHead) {
                        loHead = bucket;
                    } else {
                        loTail->next = bucket;
                    }
                    loTail = bucket;
                } else {
                    if (!hiHead) {
                        hiHead = bucket;
                    } else {
                        hiTail->next = bucket;
                    }
                    hiTail = bucket;
                }

                bucket = next;
            }

            newTable[i] = loHead;
            newTable[i + oldCapacity] = hiHead;
        }

        m_table = std::move(newTable);
        m_threshold = std::max<size_t>(1, static_cast<size_t>(static_cast<float>(newCapacity) * LOAD_FACTOR));
    }

    void ensureInitialized() {
        if (m_table.empty()) {
            initialize();
        }
    }

public:
    JavaHashSet() {
        initialize();
    }

    JavaHashSet(JavaHashSet&&) noexcept = default;
    JavaHashSet& operator=(JavaHashSet&&) noexcept = default;

    JavaHashSet(const JavaHashSet&) = delete;
    JavaHashSet& operator=(const JavaHashSet&) = delete;

    class const_iterator {
    private:
        const JavaHashSet* m_owner = nullptr;
        size_t m_bucketIndex = 0;
        Node* m_node = nullptr;

        void advanceToNextBucket() {
            while (!m_node && m_owner && m_bucketIndex < m_owner->m_table.size()) {
                m_node = m_owner->m_table[m_bucketIndex];
                if (m_node) {
                    break;
                }
                ++m_bucketIndex;
            }

            if (!m_node && m_owner) {
                m_bucketIndex = m_owner->m_table.size();
            }
        }

    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;

        const_iterator() = default;

        const_iterator(const JavaHashSet* owner, size_t bucketIndex, Node* node)
            : m_owner(owner)
            , m_bucketIndex(bucketIndex)
            , m_node(node) {
            advanceToNextBucket();
        }

        reference operator*() const { return m_node->value; }
        pointer operator->() const { return &m_node->value; }

        const_iterator& operator++() {
            if (!m_owner || !m_node) {
                return *this;
            }

            if (m_node->next) {
                m_node = m_node->next;
                return *this;
            }

            ++m_bucketIndex;
            m_node = nullptr;
            advanceToNextBucket();
            return *this;
        }

        bool operator==(const const_iterator& other) const {
            return m_owner == other.m_owner &&
                   m_bucketIndex == other.m_bucketIndex &&
                   m_node == other.m_node;
        }

        bool operator!=(const const_iterator& other) const {
            return !(*this == other);
        }
    };

    bool add(const T& value) {
        ensureInitialized();

        int32_t hash = spread(value.hashCode());
        size_t idx = bucketIndex(hash, m_table.size());
        Node*& bucketHead = m_table[idx];

        if (!bucketHead) {
            auto node = std::make_unique<Node>(value, hash);
            bucketHead = node.get();
            m_nodes.push_back(std::move(node));
            ++m_size;
            if (m_size > m_threshold) {
                rehash(m_table.size() << 1);
            }
            return true;
        }

        Node* current = bucketHead;
        while (true) {
            if (current->hash == hash && current->value == value) {
                return false;
            }

            if (!current->next) {
                break;
            }
            current = current->next;
        }

        auto node = std::make_unique<Node>(value, hash);
        current->next = node.get();
        m_nodes.push_back(std::move(node));
        ++m_size;
        if (m_size > m_threshold) {
            rehash(m_table.size() << 1);
        }
        return true;
    }

    bool empty() const {
        return m_size == 0;
    }

    size_t size() const {
        return m_size;
    }

    const_iterator begin() const {
        return const_iterator(this, 0, nullptr);
    }

    const_iterator end() const {
        return const_iterator(this, m_table.size(), nullptr);
    }
};

} // namespace util
} // namespace minecraft

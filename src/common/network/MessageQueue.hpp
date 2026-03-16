// File: src/common/network/MessageQueue.hpp
// Lock-free MPSC queue for network packet communication.
// Based on Dmitry Vyukov's intrusive MPSC queue — the same pattern
// underlying Java's ConcurrentLinkedQueue (used by Minecraft's PacketProcessor).
//
// Single consumer (main/game thread) pops. One or more producers (I/O thread) push.
// Push is lock-free (CAS on head). Pop is wait-free (single consumer, no contention).
#pragma once

#include <atomic>
#include <vector>
#include <chrono>
#include <thread>
#include <cstddef>

namespace Network {

    template<typename T>
    class MessageQueue {
    public:
        static constexpr size_t DEFAULT_MAX_SIZE = 2048;

        explicit MessageQueue(size_t maxSize = DEFAULT_MAX_SIZE)
            : m_maxSize(maxSize) {
            // Sentinel node — simplifies push/pop logic
            Node* sentinel = new Node{};
            m_head.store(sentinel, std::memory_order_relaxed);
            m_tail = sentinel;
        }

        ~MessageQueue() {
            // Drain remaining nodes
            T dummy;
            while (try_pop(dummy)) {}
            // Delete sentinel
            delete m_tail;
        }

        // Non-copyable, non-movable
        MessageQueue(const MessageQueue&) = delete;
        MessageQueue& operator=(const MessageQueue&) = delete;
        MessageQueue(MessageQueue&&) = delete;
        MessageQueue& operator=(MessageQueue&&) = delete;

        // Push (producer side, lock-free via CAS)
        bool try_push(T&& message) {
            size_t currentSize = m_size.load(std::memory_order_relaxed);
            if (currentSize >= m_maxSize) {
                m_droppedCount.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            Node* node = new Node{std::move(message)};
            Node* prev = m_head.exchange(node, std::memory_order_acq_rel);
            prev->next.store(node, std::memory_order_release);
            m_size.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        bool try_push(const T& message) {
            size_t currentSize = m_size.load(std::memory_order_relaxed);
            if (currentSize >= m_maxSize) {
                m_droppedCount.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            Node* node = new Node{message};
            Node* prev = m_head.exchange(node, std::memory_order_acq_rel);
            prev->next.store(node, std::memory_order_release);
            m_size.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        // Pop (consumer side, single-threaded — no CAS needed)
        bool try_pop(T& message) {
            Node* tail = m_tail;
            Node* next = tail->next.load(std::memory_order_acquire);
            if (!next) return false;

            message = std::move(next->data);
            m_tail = next;
            delete tail;
            m_size.fetch_sub(1, std::memory_order_relaxed);
            return true;
        }

        // Check if queue has messages (approximate)
        bool has_message() const {
            return m_tail->next.load(std::memory_order_acquire) != nullptr;
        }

        // Peek at front (for inspection without consuming)
        template<typename Func>
        bool peek_front(Func&& func) const {
            Node* next = m_tail->next.load(std::memory_order_acquire);
            if (!next) return false;
            func(next->data);
            return true;
        }

        // Blocking dequeue with timeout (for compatibility — uses spin+yield)
        bool DequeueWait(T& message, std::chrono::milliseconds timeout = std::chrono::milliseconds(100)) {
            auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline) {
                if (try_pop(message)) return true;
                std::this_thread::yield();
            }
            return false;
        }

        // Approximate size
        size_t Size() const {
            return m_size.load(std::memory_order_relaxed);
        }

        bool Empty() const {
            return !has_message();
        }

        // Drain all into vector (consumer only)
        std::vector<T> DrainAll() {
            std::vector<T> messages;
            T item;
            while (try_pop(item)) {
                messages.push_back(std::move(item));
            }
            return messages;
        }

        // Clear (consumer only)
        void Clear() {
            T dummy;
            while (try_pop(dummy)) {}
        }

        size_t GetDroppedCount() const {
            return m_droppedCount.load(std::memory_order_relaxed);
        }

        size_t GetMaxSize() const { return m_maxSize; }

    private:
        struct Node {
            T data{};
            std::atomic<Node*> next{nullptr};
        };

        // Head: producers push here (CAS). Cache-line separated from tail.
        alignas(64) std::atomic<Node*> m_head;

        // Tail: consumer pops here (single-threaded, no CAS).
        alignas(64) Node* m_tail;

        // Approximate size for backpressure
        std::atomic<size_t> m_size{0};
        size_t m_maxSize;
        std::atomic<size_t> m_droppedCount{0};
    };

    // Aliases for clarity
    template<typename PacketType>
    using ServerToClientQueue = MessageQueue<PacketType>;

    template<typename PacketType>
    using ClientToServerQueue = MessageQueue<PacketType>;

    // Specialized result queues for worker thread communication
    template<typename ResultType>
    class ResultQueue : public MessageQueue<ResultType> {
    public:
        void IncrementProcessed() {
            m_processedCount.fetch_add(1, std::memory_order_relaxed);
        }

        size_t GetProcessedCount() const {
            return m_processedCount.load(std::memory_order_relaxed);
        }

        void ResetProcessedCount() {
            m_processedCount.store(0, std::memory_order_relaxed);
        }

    private:
        std::atomic<size_t> m_processedCount{0};
    };

    template<typename T>
    using ChunkGenResultQueue = ResultQueue<T>;

    template<typename T>
    using MeshResultQueue = ResultQueue<T>;

} // namespace Network

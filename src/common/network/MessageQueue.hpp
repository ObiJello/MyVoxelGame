// File: src/common/network/MessageQueue.hpp
#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace Network {

    // Thread-safe message queue template for packet communication
    template<typename T>
    class MessageQueue {
    public:
        static constexpr size_t DEFAULT_MAX_SIZE = 1024;
        
        explicit MessageQueue(size_t maxSize = DEFAULT_MAX_SIZE) 
            : m_maxSize(maxSize) {}
        ~MessageQueue() = default;

        // Non-copyable, non-movable for safety
        MessageQueue(const MessageQueue&) = delete;
        MessageQueue& operator=(const MessageQueue&) = delete;
        MessageQueue(MessageQueue&&) = delete;
        MessageQueue& operator=(MessageQueue&&) = delete;

        // Try to push a message (returns false if queue is full)
        bool try_push(T&& message) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_queue.size() >= m_maxSize) {
                m_droppedCount++;
                return false;
            }
            m_queue.push(std::move(message));
            m_condition.notify_one();
            return true;
        }

        bool try_push(const T& message) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_queue.size() >= m_maxSize) {
                m_droppedCount++;
                return false;
            }
            m_queue.push(message);
            m_condition.notify_one();
            return true;
        }

        // Try to pop a message (non-blocking, returns false if empty)
        bool try_pop(T& message) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_queue.empty()) {
                return false;
            }
            message = std::move(m_queue.front());
            m_queue.pop();
            return true;
        }

        // Legacy methods for backward compatibility
        void Enqueue(T&& message) {
            try_push(std::move(message));
        }

        void Enqueue(const T& message) {
            try_push(message);
        }

        bool TryDequeue(T& message) {
            return try_pop(message);
        }

        // Dequeue a message (blocking with timeout)
        bool DequeueWait(T& message, std::chrono::milliseconds timeout = std::chrono::milliseconds(100)) {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_condition.wait_for(lock, timeout, [this] { return !m_queue.empty(); })) {
                message = std::move(m_queue.front());
                m_queue.pop();
                return true;
            }
            return false;
        }

        // Get queue size (approximate, for debugging)
        size_t Size() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_queue.size();
        }

        // Check if queue is empty
        bool Empty() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_queue.empty();
        }

        // Clear all messages
        void Clear() {
            std::lock_guard<std::mutex> lock(m_mutex);
            std::queue<T> empty;
            std::swap(m_queue, empty);
        }

        // Drain all messages into a vector for batch processing
        std::vector<T> DrainAll() {
            std::vector<T> messages;
            std::lock_guard<std::mutex> lock(m_mutex);
            
            messages.reserve(m_queue.size());
            while (!m_queue.empty()) {
                messages.push_back(std::move(m_queue.front()));
                m_queue.pop();
            }
            
            return messages;
        }

        // Get dropped packet count
        size_t GetDroppedCount() const {
            return m_droppedCount.load(std::memory_order_relaxed);
        }
        
        // Get max queue size
        size_t GetMaxSize() const { return m_maxSize; }

    private:
        mutable std::mutex m_mutex;
        std::condition_variable m_condition;
        std::queue<T> m_queue;
        size_t m_maxSize;
        std::atomic<size_t> m_droppedCount{0};
    };

    // Server → Client packet queue
    template<typename PacketType>
    using ServerToClientQueue = MessageQueue<PacketType>;

    // Client → Server packet queue  
    template<typename PacketType>
    using ClientToServerQueue = MessageQueue<PacketType>;

    // Specialized result queues for worker thread communication
    template<typename ResultType>
    class ResultQueue : public MessageQueue<ResultType> {
    public:
        // Add result processing statistics
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

    // Server worker threads → Server thread
    template<typename T>
    using ChunkGenResultQueue = ResultQueue<T>;

    // Client worker threads → Client render thread
    template<typename T>
    using MeshResultQueue = ResultQueue<T>;

} // namespace Network
#pragma once

#include <memory>
#include <future>
#include <functional>
#include <mutex>
#include <vector>
#include <atomic>
#include <optional>
#include <exception>
#include <type_traits>
#include <chrono>

// Reference: java.util.concurrent.CompletableFuture
// C++ implementation providing similar async completion semantics

namespace minecraft {
namespace util {

// Forward declaration
template<typename T>
class CompletableFuture;

/**
 * CompletableFuture - A future that can be explicitly completed
 *
 * This is a C++ implementation of Java's CompletableFuture, providing:
 * - Explicit completion via complete() or completeExceptionally()
 * - Chaining via thenApply(), thenAccept(), handle()
 * - Non-blocking query via isDone(), getNow()
 * - Blocking wait via join()
 *
 * Thread-safe for all operations.
 */
template<typename T>
class CompletableFuture : public std::enable_shared_from_this<CompletableFuture<T>> {
public:
    /**
     * Create an incomplete CompletableFuture
     */
    CompletableFuture()
        : m_completed(false)
        , m_hasValue(false)
        , m_value()
        , m_exception(nullptr)
    {}

    /**
     * Create an already-completed future with the given value
     * Reference: CompletableFuture.completedFuture()
     */
    static std::shared_ptr<CompletableFuture<T>> completedFuture(T value) {
        auto future = std::make_shared<CompletableFuture<T>>();
        future->complete(std::move(value));
        return future;
    }

    /**
     * Convenience alias for completedFuture
     * Reference: CompletableFuture.completedFuture()
     */
    static std::shared_ptr<CompletableFuture<T>> completed(T value) {
        return completedFuture(std::move(value));
    }

    /**
     * Create an already-failed future with the given exception
     */
    static std::shared_ptr<CompletableFuture<T>> failedFuture(std::exception_ptr ex) {
        auto future = std::make_shared<CompletableFuture<T>>();
        future->completeExceptionally(ex);
        return future;
    }

    /**
     * Execute a supplier function asynchronously and return a future for the result
     * Reference: CompletableFuture.supplyAsync(Supplier, Executor)
     *
     * @param supplier Function that produces a value
     * @param executor Function that takes a task and schedules it for execution
     * @return A future that will complete with the supplier's result
     */
    static std::shared_ptr<CompletableFuture<T>> supplyAsync(
        std::function<T()> supplier,
        std::function<void(std::function<void()>)> executor
    ) {
        auto future = std::make_shared<CompletableFuture<T>>();

        // Schedule the supplier to run on the executor
        executor([future, supplier = std::move(supplier)]() {
            try {
                T result = supplier();
                future->complete(std::move(result));
            } catch (...) {
                future->completeExceptionally(std::current_exception());
            }
        });

        return future;
    }

    /**
     * Complete the future with a value
     * Returns true if this invocation caused completion, false if already completed
     * Reference: CompletableFuture.complete()
     */
    bool complete(T value) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_completed) {
            return false;
        }

        m_value = std::move(value);
        m_hasValue = true;
        m_completed = true;

        // Copy continuations to invoke outside lock
        auto continuations = std::move(m_continuations);
        m_continuations.clear();

        // Notify any threads waiting in join()
        // Reference: Java's CompletableFuture notifies waiters on completion
        m_cv.notify_all();

        lock.unlock();

        // Invoke continuations
        for (auto& cont : continuations) {
            cont();
        }

        return true;
    }

    /**
     * Complete the future with an exception
     * Returns true if this invocation caused completion, false if already completed
     * Reference: CompletableFuture.completeExceptionally()
     */
    bool completeExceptionally(std::exception_ptr ex) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_completed) {
            return false;
        }

        m_exception = ex;
        m_hasValue = false;
        m_completed = true;

        // Copy continuations to invoke outside lock
        auto continuations = std::move(m_continuations);
        m_continuations.clear();

        // Notify any threads waiting in join()
        // Reference: Java's CompletableFuture notifies waiters on completion
        m_cv.notify_all();

        lock.unlock();

        // Invoke continuations
        for (auto& cont : continuations) {
            cont();
        }

        return true;
    }

    /**
     * Check if the future is completed (successfully or exceptionally)
     * Reference: CompletableFuture.isDone()
     */
    bool isDone() const {
        return m_completed.load(std::memory_order_acquire);
    }

    /**
     * Get the value if completed, or return the default value
     * Does not block.
     * Reference: CompletableFuture.getNow()
     */
    T getNow(T defaultValue) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_completed && m_hasValue) {
            return m_value;
        }
        return defaultValue;
    }

    /**
     * Block until completed and return the value
     * Throws if completed exceptionally
     * Reference: CompletableFuture.join()
     */
    T join() {
        // Wait for completion
        std::unique_lock<std::mutex> lock(m_mutex);
        while (!m_completed) {
            m_cv.wait(lock);
        }

        if (m_exception) {
            std::rethrow_exception(m_exception);
        }

        return m_value;
    }

    /**
     * Get the value, throwing if exceptionally completed
     * Same as join() but matches get() semantics
     */
    T get() {
        return join();
    }

    /**
     * Apply a function to the result when completed
     * Returns a new CompletableFuture with the transformed result
     * Reference: CompletableFuture.thenApply()
     *
     * Lambda-friendly version that deduces return type automatically
     */
    template<typename Func, typename R = std::invoke_result_t<Func, T>>
    std::shared_ptr<CompletableFuture<R>> thenApply(Func&& fn) {
        auto result = std::make_shared<CompletableFuture<R>>();
        auto func = std::forward<Func>(fn);

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_completed) {
            // Already done, run immediately (still under lock for consistency)
            if (m_exception) {
                result->completeExceptionally(m_exception);
            } else {
                try {
                    result->complete(func(m_value));
                } catch (...) {
                    result->completeExceptionally(std::current_exception());
                }
            }
        } else {
            // Schedule for when completed
            // OPTIMIZATION: No lock needed in continuation - by the time it runs,
            // complete() has finished and m_value/m_exception are immutable.
            // The acquire semantics from isDone() check ensures visibility.
            m_continuations.push_back([this, result, func]() mutable {
                // m_value and m_exception are immutable after completion - no lock needed
                if (m_exception) {
                    result->completeExceptionally(m_exception);
                } else {
                    try {
                        result->complete(func(m_value));
                    } catch (...) {
                        result->completeExceptionally(std::current_exception());
                    }
                }
            });
        }

        return result;
    }

    /**
     * Apply a function asynchronously when completed
     * Reference: CompletableFuture.thenApplyAsync()
     */
    template<typename Func, typename R = std::invoke_result_t<Func, T>>
    std::shared_ptr<CompletableFuture<R>> thenApplyAsync(Func&& fn) {
        // For now, just call thenApply synchronously
        // In a full implementation, this would use an executor
        return thenApply(std::forward<Func>(fn));
    }

    /**
     * Consume the result when completed (no return value)
     * Reference: CompletableFuture.thenAccept()
     * Uses const T& to support non-copyable types like unique_ptr
     */
    std::shared_ptr<CompletableFuture<void>> thenAccept(std::function<void(const T&)> consumer);

    /**
     * Run an action when completed (ignores result)
     * Reference: CompletableFuture.thenRun()
     */
    std::shared_ptr<CompletableFuture<void>> thenRun(std::function<void()> action);

    /**
     * Handle both success and exception cases
     * Reference: CompletableFuture.handle()
     *
     * @param handler Function taking (result, exception) and returning new value
     *                If completed normally: result is valid, exception is nullptr
     *                If completed exceptionally: exception is valid
     */
    template<typename R>
    std::shared_ptr<CompletableFuture<R>> handle(std::function<R(T, std::exception_ptr)> handler) {
        auto result = std::make_shared<CompletableFuture<R>>();

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_completed) {
            // Already done, run immediately
            try {
                result->complete(handler(m_hasValue ? m_value : T{}, m_exception));
            } catch (...) {
                result->completeExceptionally(std::current_exception());
            }
        } else {
            // Schedule for when completed
            m_continuations.push_back([this, result, handler]() {
                std::lock_guard<std::mutex> lock(m_mutex);
                try {
                    result->complete(handler(m_hasValue ? m_value : T{}, m_exception));
                } catch (...) {
                    result->completeExceptionally(std::current_exception());
                }
            });
        }

        return result;
    }

    /**
     * Add a callback to be invoked when completed
     */
    void whenComplete(std::function<void(T, std::exception_ptr)> callback) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_completed) {
            callback(m_hasValue ? m_value : T{}, m_exception);
        } else {
            m_continuations.push_back([this, callback]() {
                std::lock_guard<std::mutex> lock(m_mutex);
                callback(m_hasValue ? m_value : T{}, m_exception);
            });
        }
    }

    /**
     * Check if completed exceptionally
     */
    bool isCompletedExceptionally() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_completed && m_exception != nullptr;
    }

    /**
     * Create a future that completes when all futures complete
     * Reference: CompletableFuture.allOf()
     */
    static std::shared_ptr<CompletableFuture<std::vector<T>>> allOf(
        std::vector<std::shared_ptr<CompletableFuture<T>>> futures
    ) {
        if (futures.empty()) {
            return completedFuture(std::vector<T>{});
        }

        auto result = std::make_shared<CompletableFuture<std::vector<T>>>();
        auto counter = std::make_shared<std::atomic<size_t>>(futures.size());
        auto results = std::make_shared<std::vector<T>>(futures.size());
        auto failed = std::make_shared<std::atomic<bool>>(false);
        auto failException = std::make_shared<std::exception_ptr>();

        for (size_t i = 0; i < futures.size(); ++i) {
            futures[i]->whenComplete([=](T value, std::exception_ptr ex) {
                if (ex) {
                    bool expected = false;
                    if (failed->compare_exchange_strong(expected, true)) {
                        *failException = ex;
                    }
                } else {
                    (*results)[i] = std::move(value);
                }

                if (counter->fetch_sub(1) == 1) {
                    // Last one done
                    if (*failed) {
                        result->completeExceptionally(*failException);
                    } else {
                        result->complete(std::move(*results));
                    }
                }
            });
        }

        return result;
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_completed;
    bool m_hasValue;
    T m_value;
    std::exception_ptr m_exception;
    std::vector<std::function<void()>> m_continuations;
};

/**
 * Specialization for void - a future that just signals completion
 */
template<>
class CompletableFuture<void> : public std::enable_shared_from_this<CompletableFuture<void>> {
public:
    CompletableFuture()
        : m_completed(false)
        , m_exception(nullptr)
    {}

    static std::shared_ptr<CompletableFuture<void>> completedFuture() {
        auto future = std::make_shared<CompletableFuture<void>>();
        future->complete();
        return future;
    }

    static std::shared_ptr<CompletableFuture<void>> failedFuture(std::exception_ptr ex) {
        auto future = std::make_shared<CompletableFuture<void>>();
        future->completeExceptionally(ex);
        return future;
    }

    bool complete() {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_completed) {
            return false;
        }

        m_completed = true;
        auto continuations = std::move(m_continuations);
        m_continuations.clear();

        // Notify any threads waiting in join()
        // Reference: Java's CompletableFuture notifies waiters on completion
        m_cv.notify_all();

        lock.unlock();

        for (auto& cont : continuations) {
            cont();
        }

        return true;
    }

    bool completeExceptionally(std::exception_ptr ex) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_completed) {
            return false;
        }

        m_exception = ex;
        m_completed = true;

        auto continuations = std::move(m_continuations);
        m_continuations.clear();

        // Notify any threads waiting in join()
        // Reference: Java's CompletableFuture notifies waiters on completion
        m_cv.notify_all();

        lock.unlock();

        for (auto& cont : continuations) {
            cont();
        }

        return true;
    }

    bool isDone() const {
        return m_completed.load(std::memory_order_acquire);
    }

    void join() {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (!m_completed) {
            m_cv.wait(lock);
        }

        if (m_exception) {
            std::rethrow_exception(m_exception);
        }
    }

    std::shared_ptr<CompletableFuture<void>> thenRun(std::function<void()> action) {
        auto result = std::make_shared<CompletableFuture<void>>();

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_completed) {
            if (m_exception) {
                result->completeExceptionally(m_exception);
            } else {
                try {
                    action();
                    result->complete();
                } catch (...) {
                    result->completeExceptionally(std::current_exception());
                }
            }
        } else {
            // OPTIMIZATION: No lock needed in continuation - m_exception is immutable after completion
            m_continuations.push_back([this, result, action]() {
                if (m_exception) {
                    result->completeExceptionally(m_exception);
                } else {
                    try {
                        action();
                        result->complete();
                    } catch (...) {
                        result->completeExceptionally(std::current_exception());
                    }
                }
            });
        }

        return result;
    }

    void whenComplete(std::function<void(std::exception_ptr)> callback) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_completed) {
            callback(m_exception);
        } else {
            // OPTIMIZATION: No lock needed in continuation - m_exception is immutable after completion
            m_continuations.push_back([this, callback]() {
                callback(m_exception);
            });
        }
    }

    bool isCompletedExceptionally() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_completed && m_exception != nullptr;
    }

    static std::shared_ptr<CompletableFuture<void>> allOf(
        std::vector<std::shared_ptr<CompletableFuture<void>>> futures
    ) {
        if (futures.empty()) {
            return completedFuture();
        }

        auto result = std::make_shared<CompletableFuture<void>>();
        auto counter = std::make_shared<std::atomic<size_t>>(futures.size());
        auto failed = std::make_shared<std::atomic<bool>>(false);
        auto failException = std::make_shared<std::exception_ptr>();

        for (auto& future : futures) {
            future->whenComplete([=](std::exception_ptr ex) {
                if (ex) {
                    bool expected = false;
                    if (failed->compare_exchange_strong(expected, true)) {
                        *failException = ex;
                    }
                }

                if (counter->fetch_sub(1) == 1) {
                    if (*failed) {
                        result->completeExceptionally(*failException);
                    } else {
                        result->complete();
                    }
                }
            });
        }

        return result;
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_completed;
    std::exception_ptr m_exception;
    std::vector<std::function<void()>> m_continuations;
};

// Implementation of thenAccept and thenRun for non-void
// Uses const T& to avoid copying non-copyable types like unique_ptr
template<typename T>
std::shared_ptr<CompletableFuture<void>> CompletableFuture<T>::thenAccept(std::function<void(const T&)> consumer) {
    auto result = std::make_shared<CompletableFuture<void>>();

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_completed) {
        if (m_exception) {
            result->completeExceptionally(m_exception);
        } else {
            try {
                consumer(m_value);
                result->complete();
            } catch (...) {
                result->completeExceptionally(std::current_exception());
            }
        }
    } else {
        // OPTIMIZATION: No lock needed in continuation - m_value/m_exception are immutable after completion
        m_continuations.push_back([this, result, consumer]() {
            if (m_exception) {
                result->completeExceptionally(m_exception);
            } else {
                try {
                    consumer(m_value);
                    result->complete();
                } catch (...) {
                    result->completeExceptionally(std::current_exception());
                }
            }
        });
    }

    return result;
}

template<typename T>
std::shared_ptr<CompletableFuture<void>> CompletableFuture<T>::thenRun(std::function<void()> action) {
    auto result = std::make_shared<CompletableFuture<void>>();

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_completed) {
        if (m_exception) {
            result->completeExceptionally(m_exception);
        } else {
            try {
                action();
                result->complete();
            } catch (...) {
                result->completeExceptionally(std::current_exception());
            }
        }
    } else {
        // OPTIMIZATION: No lock needed in continuation - m_exception is immutable after completion
        m_continuations.push_back([this, result, action]() {
            if (m_exception) {
                result->completeExceptionally(m_exception);
            } else {
                try {
                    action();
                    result->complete();
                } catch (...) {
                    result->completeExceptionally(std::current_exception());
                }
            }
        });
    }

    return result;
}

} // namespace util
} // namespace minecraft

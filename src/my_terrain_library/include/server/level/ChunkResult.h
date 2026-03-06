#pragma once

#include <functional>
#include <string>
#include <memory>
#include <variant>
#include <stdexcept>

// Reference: net/minecraft/server/level/ChunkResult.java

namespace minecraft {
namespace server {
namespace level {

// Forward declarations
template<typename T>
class ChunkResult;

template<typename T>
class ChunkResultSuccess;

template<typename T>
class ChunkResultFail;

/**
 * ChunkResult - Result wrapper for chunk operations (Success or Fail)
 * Reference: ChunkResult.java
 *
 * @tparam T The type of the success value
 */
template<typename T>
class ChunkResult {
public:
    virtual ~ChunkResult() = default;

    /**
     * Create a success result
     * Reference: ChunkResult.java lines 9-11
     */
    static std::shared_ptr<ChunkResult<T>> of(T value);

    /**
     * Create a failure result with error message
     * Reference: ChunkResult.java lines 13-15
     */
    static std::shared_ptr<ChunkResult<T>> error(const std::string& errorMsg);

    /**
     * Create a failure result with error supplier
     * Reference: ChunkResult.java lines 17-19
     */
    static std::shared_ptr<ChunkResult<T>> error(std::function<std::string()> errorSupplier);

    /**
     * Check if this is a success result
     * Reference: ChunkResult.java line 21
     */
    virtual bool isSuccess() const = 0;

    /**
     * Get the value or return the default
     * Reference: ChunkResult.java line 23
     */
    virtual T orElse(T orElseValue) const = 0;

    /**
     * Get the error message (nullptr if success)
     * Reference: ChunkResult.java line 30
     */
    virtual const std::string* getError() const = 0;

    /**
     * Execute consumer if success
     * Reference: ChunkResult.java line 32
     */
    virtual std::shared_ptr<ChunkResult<T>> ifSuccess(std::function<void(T)> consumer) = 0;

    /**
     * Map the value if success
     * Reference: ChunkResult.java line 34
     */
    template<typename R>
    std::shared_ptr<ChunkResult<R>> map(std::function<R(T)> mapper) const;

    /**
     * Get value or throw exception
     * Reference: ChunkResult.java line 36
     */
    virtual T orElseThrow(std::function<std::runtime_error()> exceptionSupplier) const = 0;

    /**
     * Static helper to get value or default from a result
     * Reference: ChunkResult.java lines 25-28
     */
    static T orElse(const std::shared_ptr<ChunkResult<T>>& chunkResult, T orElseValue) {
        T result = chunkResult->orElse(T{});
        if (result != T{}) {
            return result;
        }
        return orElseValue;
    }

protected:
    // Protected to prevent direct instantiation
    ChunkResult() = default;
};

/**
 * Success implementation of ChunkResult
 * Reference: ChunkResult.java lines 38-63
 */
template<typename T>
class ChunkResultSuccess : public ChunkResult<T> {
public:
    explicit ChunkResultSuccess(T value) : m_value(std::move(value)) {}

    bool isSuccess() const override {
        return true;
    }

    T orElse(T orElseValue) const override {
        return m_value;
    }

    const std::string* getError() const override {
        return nullptr;
    }

    std::shared_ptr<ChunkResult<T>> ifSuccess(std::function<void(T)> consumer) override {
        consumer(m_value);
        return std::make_shared<ChunkResultSuccess<T>>(m_value);
    }

    T orElseThrow(std::function<std::runtime_error()> exceptionSupplier) const override {
        return m_value;
    }

    T value() const { return m_value; }

private:
    T m_value;
};

/**
 * Fail implementation of ChunkResult
 * Reference: ChunkResult.java lines 65-89
 */
template<typename T>
class ChunkResultFail : public ChunkResult<T> {
public:
    explicit ChunkResultFail(std::function<std::string()> errorSupplier)
        : m_errorSupplier(std::move(errorSupplier))
        , m_cachedError()
    {}

    bool isSuccess() const override {
        return false;
    }

    T orElse(T orElseValue) const override {
        return orElseValue;
    }

    const std::string* getError() const override {
        if (m_cachedError.empty()) {
            m_cachedError = m_errorSupplier();
        }
        return &m_cachedError;
    }

    std::shared_ptr<ChunkResult<T>> ifSuccess(std::function<void(T)> consumer) override {
        return std::make_shared<ChunkResultFail<T>>(m_errorSupplier);
    }

    T orElseThrow(std::function<std::runtime_error()> exceptionSupplier) const override {
        throw exceptionSupplier();
    }

    const std::function<std::string()>& errorSupplier() const { return m_errorSupplier; }

private:
    std::function<std::string()> m_errorSupplier;
    mutable std::string m_cachedError;
};

// Template implementations

template<typename T>
std::shared_ptr<ChunkResult<T>> ChunkResult<T>::of(T value) {
    return std::make_shared<ChunkResultSuccess<T>>(std::move(value));
}

template<typename T>
std::shared_ptr<ChunkResult<T>> ChunkResult<T>::error(const std::string& errorMsg) {
    return error([errorMsg]() { return errorMsg; });
}

template<typename T>
std::shared_ptr<ChunkResult<T>> ChunkResult<T>::error(std::function<std::string()> errorSupplier) {
    return std::make_shared<ChunkResultFail<T>>(std::move(errorSupplier));
}

template<typename T>
template<typename R>
std::shared_ptr<ChunkResult<R>> ChunkResult<T>::map(std::function<R(T)> mapper) const {
    if (isSuccess()) {
        auto* success = dynamic_cast<const ChunkResultSuccess<T>*>(this);
        return std::make_shared<ChunkResultSuccess<R>>(mapper(success->value()));
    } else {
        auto* fail = dynamic_cast<const ChunkResultFail<T>*>(this);
        return std::make_shared<ChunkResultFail<R>>(fail->errorSupplier());
    }
}

} // namespace level
} // namespace server
} // namespace minecraft

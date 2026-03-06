#pragma once

#include "util/thread/AbstractConsecutiveExecutor.h"
#include "util/thread/StrictQueue.h"
#include <memory>
#include <functional>
#include <string>

// Reference: net/minecraft/util/thread/ConsecutiveExecutor.java

namespace minecraft {
namespace util {
namespace thread {

/**
 * ConsecutiveExecutor - Simple consecutive executor using a standard queue
 * Reference: ConsecutiveExecutor.java
 */
class ConsecutiveExecutor : public AbstractConsecutiveExecutor<std::function<void()>> {
public:
    using Executor = std::function<void(std::function<void()>)>;

    /**
     * Constructor
     * Reference: ConsecutiveExecutor.java lines 7-9
     */
    ConsecutiveExecutor(Executor dispatcher, const std::string& name)
        : AbstractConsecutiveExecutor<std::function<void()>>(
            std::make_unique<QueueStrictQueue>(),
            std::move(dispatcher),
            name
        )
    {}

    /**
     * Wrap a runnable (identity operation for simple executor)
     * Reference: ConsecutiveExecutor.java lines 11-13
     */
    std::function<void()> wrapRunnable(std::function<void()> runnable) override {
        return runnable;
    }
};

} // namespace thread
} // namespace util
} // namespace minecraft

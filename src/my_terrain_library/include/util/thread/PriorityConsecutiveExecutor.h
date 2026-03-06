#pragma once

#include "util/thread/AbstractConsecutiveExecutor.h"
#include "util/thread/StrictQueue.h"
#include "util/CompletableFuture.h"
#include <memory>
#include <functional>
#include <string>

// Reference: net/minecraft/util/thread/PriorityConsecutiveExecutor.java

namespace minecraft {
namespace util {
namespace thread {

/**
 * PriorityConsecutiveExecutor - Consecutive executor with priority levels
 * Reference: PriorityConsecutiveExecutor.java
 *
 * Lower priority number = higher priority (processed first)
 */
class PriorityConsecutiveExecutor : public AbstractConsecutiveExecutor<RunnableWithPriority> {
public:
    using Executor = std::function<void(std::function<void()>)>;

    /**
     * Constructor
     * Reference: PriorityConsecutiveExecutor.java lines 9-12
     *
     * @param priorityCount Number of priority levels
     * @param executor The underlying executor to dispatch to
     * @param name Name of this executor
     */
    PriorityConsecutiveExecutor(int priorityCount, Executor executor, const std::string& name)
        : AbstractConsecutiveExecutor<RunnableWithPriority>(
            std::make_unique<FixedPriorityQueue>(priorityCount),
            std::move(executor),
            name
        )
    {}

    /**
     * Wrap a runnable with default priority (0 = highest)
     * Reference: PriorityConsecutiveExecutor.java lines 14-16
     */
    RunnableWithPriority wrapRunnable(std::function<void()> runnable) override {
        return RunnableWithPriority(0, std::move(runnable));
    }

    /**
     * Schedule a task with specific priority
     */
    void schedule(int priority, std::function<void()> task) {
        AbstractConsecutiveExecutor<RunnableWithPriority>::schedule(
            RunnableWithPriority(priority, std::move(task))
        );
    }

    /**
     * Schedule a task that produces a result, with specific priority
     * Reference: PriorityConsecutiveExecutor.java lines 18-22
     *
     * @tparam Source The type of result
     * @param priority The priority level
     * @param futureConsumer A consumer that receives a future to complete with the result
     * @return A future that will contain the result
     */
    template<typename Source>
    std::shared_ptr<CompletableFuture<Source>> scheduleWithResult(
        int priority,
        std::function<void(std::shared_ptr<CompletableFuture<Source>>)> futureConsumer
    ) {
        auto future = std::make_shared<CompletableFuture<Source>>();
        schedule(priority, [future, futureConsumer]() {
            futureConsumer(future);
        });
        return future;
    }
};

} // namespace thread
} // namespace util
} // namespace minecraft

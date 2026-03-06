#pragma once

#include "util/CompletableFuture.h"
#include <functional>
#include <string>
#include <memory>

// Reference: net/minecraft/util/thread/TaskScheduler.java

namespace minecraft {
namespace util {
namespace thread {

/**
 * TaskScheduler - Interface for scheduling and executing tasks
 * Reference: TaskScheduler.java
 *
 * @tparam R The type of runnable this scheduler accepts
 */
template<typename R>
class TaskScheduler {
public:
    virtual ~TaskScheduler() = default;

    /**
     * Get the name of this scheduler
     * Reference: TaskScheduler.java line 8
     */
    virtual std::string name() const = 0;

    /**
     * Schedule a task for execution
     * Reference: TaskScheduler.java line 10
     */
    virtual void schedule(R task) = 0;

    /**
     * Close the scheduler (default: no-op)
     * Reference: TaskScheduler.java lines 12-13
     */
    virtual void close() {}

    /**
     * Wrap a generic runnable into the scheduler's specific type
     * Reference: TaskScheduler.java line 15
     */
    virtual R wrapRunnable(std::function<void()> runnable) = 0;

    /**
     * Schedule a task that produces a result
     * Reference: TaskScheduler.java lines 17-21
     *
     * @tparam Source The type of result
     * @param futureConsumer A consumer that receives a future to complete with the result
     * @return A future that will contain the result
     */
    template<typename Source>
    std::shared_ptr<CompletableFuture<Source>> scheduleWithResult(
        std::function<void(std::shared_ptr<CompletableFuture<Source>>)> futureConsumer
    ) {
        auto future = std::make_shared<CompletableFuture<Source>>();
        this->schedule(this->wrapRunnable([future, futureConsumer]() {
            futureConsumer(future);
        }));
        return future;
    }
};

/**
 * ExecutorTaskScheduler - TaskScheduler wrapping an executor function
 * Reference: TaskScheduler.java lines 23-41
 */
class ExecutorTaskScheduler : public TaskScheduler<std::function<void()>> {
public:
    using Executor = std::function<void(std::function<void()>)>;

    /**
     * Create a TaskScheduler wrapping an executor
     * Reference: TaskScheduler.java lines 23-41
     */
    static std::shared_ptr<ExecutorTaskScheduler> wrapExecutor(
        const std::string& name,
        Executor executor
    ) {
        return std::make_shared<ExecutorTaskScheduler>(name, std::move(executor));
    }

    ExecutorTaskScheduler(const std::string& name, Executor executor)
        : m_name(name)
        , m_executor(std::move(executor))
    {}

    std::string name() const override {
        return m_name;
    }

    void schedule(std::function<void()> task) override {
        m_executor(std::move(task));
    }

    std::function<void()> wrapRunnable(std::function<void()> runnable) override {
        return runnable;
    }

private:
    std::string m_name;
    Executor m_executor;
};

} // namespace thread
} // namespace util
} // namespace minecraft

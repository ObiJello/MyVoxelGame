#include "JobSystem.hpp"

namespace JobSystem {
    // Define the global ThreadPool with default worker count.
    ThreadPool g_ThreadPool;

    ThreadPool::ThreadPool(size_t workerCount) {
        if (workerCount == 0) {
            workerCount = 1; // at least one thread
        }
        for (size_t i = 0; i < workerCount; ++i) {
            workers.emplace_back([this] { WorkerLoop(); });
        }
    }

    ThreadPool::~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            stop = true;
        }
        condition.notify_all();
        for (auto &thread : workers) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    void ThreadPool::Enqueue(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            jobQueue.push(std::move(job));
        }
        condition.notify_one();
    }

    void ThreadPool::WorkerLoop() {
        while (true) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                condition.wait(lock, [this] { return stop || !jobQueue.empty(); });

                if (stop && jobQueue.empty()) {
                    return; // exit thread
                }

                job = std::move(jobQueue.front());
                jobQueue.pop();
            }
            // Execute the job outside the lock
            job();
        }
    }
}

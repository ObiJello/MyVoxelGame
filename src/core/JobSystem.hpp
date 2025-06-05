#pragma once

#include <functional>
#include <queue>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>

namespace JobSystem {

    // A simple thread pool.
    class ThreadPool {
    public:
        // Constructor: spawns (hardware_concurrency - 1) worker threads by default.
        explicit ThreadPool(size_t workerCount = std::thread::hardware_concurrency() > 1
                                           ? std::thread::hardware_concurrency() - 1
                                           : 1);

        // Destructor: signals all threads to quit and joins them.
        ~ThreadPool();

        // Enqueue a job (callable) into the pool.
        void Enqueue(std::function<void()> job);

        // Disable copy/move
        ThreadPool(const ThreadPool&) = delete;
        ThreadPool& operator=(const ThreadPool&) = delete;

    private:
        // Worker entry: each thread runs this function, pulling jobs from the queue.
        void WorkerLoop();

        std::vector<std::thread>             workers;
        std::queue<std::function<void()>>    jobQueue;
        std::mutex                           queueMutex;
        std::condition_variable              condition;
        bool                                 stop{false};
    };

    // A global ThreadPool instance you can use throughout the game.
    extern ThreadPool g_ThreadPool;
}

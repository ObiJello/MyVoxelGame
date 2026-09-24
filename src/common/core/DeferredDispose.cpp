// File: src/common/core/DeferredDispose.cpp
#include "common/core/DeferredDispose.hpp"

#include "common/core/ThreadPriority.hpp"
#include "common/core/Profiling_Tracy.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace Core::DeferredDispose {

    namespace {

        struct State {
            std::mutex                        mutex;
            std::condition_variable           wake;      // work queued / stop requested
            std::condition_variable           idle;      // queue empty and nothing running
            std::deque<std::function<void()>> queue;
            std::thread                       thread;
            bool                              running = false;   // a disposal is executing
            bool                              stop    = false;
            bool                              down    = false;   // Shutdown ran: inline from now on
        };

        State& Get() {
            static State s;
            return s;
        }

        void ThreadMain() {
            SetCurrentThreadPriority(ThreadPriorityClass::Throughput);
            PROFILE_THREAD("DeferredDispose");
            State& s = Get();
            std::unique_lock<std::mutex> lock(s.mutex);
            for (;;) {
                s.wake.wait(lock, [&] { return s.stop || !s.queue.empty(); });
                if (s.queue.empty()) return;   // stop with nothing left
                std::function<void()> fn = std::move(s.queue.front());
                s.queue.pop_front();
                s.running = true;
                lock.unlock();
                { PROFILE_ZONE_N("Dispose.Run"); fn(); }
                fn = nullptr;   // the captured containers die here, outside the lock
                lock.lock();
                s.running = false;
                if (s.queue.empty()) s.idle.notify_all();
            }
        }

    } // namespace

    void Post(std::function<void()> fn) {
        if (!fn) return;
        State& s = Get();
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            if (!s.down) {
                if (!s.thread.joinable()) s.thread = std::thread(ThreadMain);
                s.queue.push_back(std::move(fn));
                s.wake.notify_one();
                return;
            }
        }
        fn();
    }

    void Drain() {
        State& s = Get();
        std::unique_lock<std::mutex> lock(s.mutex);
        if (!s.thread.joinable()) return;
        s.idle.wait(lock, [&] { return s.queue.empty() && !s.running; });
    }

    void Shutdown() {
        Drain();
        State& s = Get();
        std::thread thread;
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.down = true;
            s.stop = true;
            thread = std::move(s.thread);
            s.wake.notify_all();
        }
        if (thread.joinable()) thread.join();
    }

    size_t Pending() {
        State& s = Get();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.queue.size() + (s.running ? 1u : 0u);
    }

} // namespace Core::DeferredDispose

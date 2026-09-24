// File: src/client/sound/SoundEngineExecutor.cpp
#include "client/sound/SoundEngineExecutor.hpp"

#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"

#include <exception>
#include <utility>

namespace Client {

    void SoundEngineExecutor::Execute(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_shutdown) return;
            m_tasks.push_back(std::move(task));
        }
        m_cv.notify_one();
    }

    void SoundEngineExecutor::ShutDown() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_shutdown && !m_thread.joinable()) return;
            m_shutdown = true;
            m_tasks.clear();          // MC dropAllTasks
        }
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();
        m_threadId = std::thread::id();
    }

    void SoundEngineExecutor::StartUp() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_shutdown) return;
            m_shutdown = false;
        }
        m_thread = std::thread([this] { Run(); });
        m_threadId = m_thread.get_id();
    }

    void SoundEngineExecutor::Run() {
        PROFILE_THREAD("Sound engine");
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return m_shutdown || !m_tasks.empty(); });
                if (m_shutdown) return;
                task = std::move(m_tasks.front());
                m_tasks.pop_front();
            }
            // A failing task must not take the audio thread down with it (MC
            // reports it as a delayed crash; a lost sound is the lesser harm
            // for a thread nothing else can restart mid-session).
            try {
                task();
            } catch (const std::exception& e) {
                Log::Error("[Sound] Uncaught exception on the sound engine thread: %s", e.what());
            }
        }
    }

} // namespace Client

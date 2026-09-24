// File: src/client/sound/SoundEngineExecutor.hpp
//
// MC net.minecraft.client.sounds.SoundEngineExecutor: the "Sound engine"
// thread every OpenAL source call runs on. SoundEngine (main thread) decides
// WHAT plays; the channel work — creating sources, attaching buffers, setting
// gain / pitch / position, pumping streams — is posted here as tasks, so the
// render thread never waits on the audio driver.
//
// ShutDown drops whatever is queued and joins; StartUp starts a fresh thread
// (MC SoundEngine.stopAll bounces it that way).
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace Client {

    class SoundEngineExecutor {
    public:
        SoundEngineExecutor() { StartUp(); }
        ~SoundEngineExecutor() { ShutDown(); }

        SoundEngineExecutor(const SoundEngineExecutor&) = delete;
        SoundEngineExecutor& operator=(const SoundEngineExecutor&) = delete;

        // Any thread. Dropped silently while shut down (MC schedule()).
        void Execute(std::function<void()> task);

        void ShutDown();
        void StartUp();

        bool IsExecutorThread() const { return std::this_thread::get_id() == m_threadId; }

    private:
        void Run();

        std::mutex                        m_mutex;
        std::condition_variable           m_cv;
        std::deque<std::function<void()>> m_tasks;
        bool                              m_shutdown = true;
        std::thread                       m_thread;
        std::thread::id                   m_threadId;
    };

} // namespace Client

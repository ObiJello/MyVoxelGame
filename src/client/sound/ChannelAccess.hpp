// File: src/client/sound/ChannelAccess.hpp
//
// MC net.minecraft.client.sounds.ChannelAccess: the main thread's handles onto
// channels that live on the sound executor.
//
// A ChannelHandle is created on the main thread the moment SoundEngine
// decides to play (after the pool accepted it — Library::TryReserve), and its
// AL source is created by the first task posted for it. Every later
// Execute(action) runs on the executor, in order, against that channel; a
// handle whose source could not be made runs nothing and reads as stopped.
// ScheduleTick (once per sound tick) pumps the streams and releases the
// channels that finished, which is what flips IsStopped for SoundEngine.
#pragma once

#include "client/sound/SoundEngineExecutor.hpp"
#include "client/sound/audio/Library.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace Client {

    class ChannelAccess;

    class ChannelHandle : public std::enable_shared_from_this<ChannelHandle> {
    public:
        ChannelHandle(ChannelAccess& owner, Audio::Library::Pool pool) : m_owner(owner), m_pool(pool) {}

        // Main thread: has the channel finished and been released?
        bool IsStopped() const { return m_stopped.load(std::memory_order_acquire); }

        // Any thread: run `action` on the executor against this channel.
        void Execute(std::function<void(Audio::Channel&)> action);

    private:
        friend class ChannelAccess;
        // Executor thread (or the main thread once the executor is stopped).
        void Release();

        ChannelAccess&                  m_owner;
        Audio::Library::Pool            m_pool;
        std::unique_ptr<Audio::Channel> m_channel;   // executor-owned
        std::atomic<bool>               m_stopped{false};
    };

    class ChannelAccess {
    public:
        ChannelAccess(Audio::Library& library, SoundEngineExecutor& executor)
            : m_library(library), m_executor(executor) {}

        // Main thread. Null when the pool is full (MC: the executor's
        // acquireChannel returned null) — the sound does not play.
        std::shared_ptr<ChannelHandle> CreateHandle(Audio::Library::Pool pool);

        // Executor: act on every live channel (MC executeOnChannels —
        // pause-all / resume-all).
        void ExecuteOnChannels(std::function<void(Audio::Channel&)> action);

        // Main thread, every sound tick: pump streams, release the finished.
        void ScheduleTick();

        // Main thread, with the executor shut down: release everything.
        void Clear();

        SoundEngineExecutor& Executor() { return m_executor; }
        Audio::Library&      Library() { return m_library; }

    private:
        Audio::Library&      m_library;
        SoundEngineExecutor& m_executor;
        // Executor-owned, except in Clear (executor stopped).
        std::vector<std::shared_ptr<ChannelHandle>> m_channels;
    };

} // namespace Client

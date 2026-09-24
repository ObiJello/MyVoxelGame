// File: src/client/sound/SoundBufferLibrary.hpp
//
// MC net.minecraft.client.sounds.SoundBufferLibrary: decoded sounds, cached
// by file, decoded OFF the main thread.
//
//   GetCompleteBuffer  a static sound: the whole file decoded once into a
//                      SoundBuffer and kept for every later play (MC's
//                      CompletableFuture cache). The callback fires when the
//                      decode is done — at once if it already is — on
//                      whichever thread finished it; callers only use it to
//                      post channel work to the sound executor.
//   GetStream          a `stream: true` sound (music, records, long loops):
//                      a fresh decoder per play, opened off-thread, fed to the
//                      channel a second at a time.
//   Preload            `preload: true` sounds decoded at load (MC preload).
//
// MC runs these on Util.nonCriticalIoPool; here a two-thread decode pool owned
// by the library. A failed decode hands the callback null.
#pragma once

#include "client/sound/audio/OggAudioStream.hpp"
#include "client/sound/audio/SoundBuffer.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Client {

    class SoundBufferLibrary {
    public:
        using BufferCallback = std::function<void(std::shared_ptr<Audio::SoundBuffer>)>;
        using StreamCallback = std::function<void(std::shared_ptr<Audio::AudioStream>)>;

        SoundBufferLibrary();
        ~SoundBufferLibrary();

        SoundBufferLibrary(const SoundBufferLibrary&) = delete;
        SoundBufferLibrary& operator=(const SoundBufferLibrary&) = delete;

        // Main thread.
        void GetCompleteBuffer(const std::string& path, BufferCallback callback);
        void GetStream(const std::string& path, bool looping, StreamCallback callback);
        void Preload(const std::vector<std::string>& paths);

        // Main thread, with the sound executor idle (MC destroy path): drop
        // every cached buffer, deleting its AL buffer.
        void Clear();

        // MC enumerate → DebugOutput.Counter: decoded buffers and their bytes.
        struct Stats { int count = 0; int64_t bytes = 0; };
        Stats GetStats() const;

    private:
        struct Entry {
            std::mutex                           mutex;
            bool                                 done = false;
            std::shared_ptr<Audio::SoundBuffer>  buffer;
            std::vector<BufferCallback>          waiters;
        };

        void Submit(std::function<void()> job);
        void WorkerLoop();

        std::unordered_map<std::string, std::shared_ptr<Entry>> m_cache;   // main thread

        std::mutex                        m_jobMutex;
        std::condition_variable           m_jobCv;
        std::deque<std::function<void()>> m_jobs;
        bool                              m_stopping = false;
        std::vector<std::thread>          m_workers;
    };

} // namespace Client

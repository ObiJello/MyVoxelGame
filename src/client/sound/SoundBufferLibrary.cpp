// File: src/client/sound/SoundBufferLibrary.cpp
#include "client/sound/SoundBufferLibrary.hpp"

#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"

#include <utility>

namespace Client {

    namespace {
        constexpr int kDecodeThreads = 2;
    }

    SoundBufferLibrary::SoundBufferLibrary() {
        for (int i = 0; i < kDecodeThreads; ++i) {
            m_workers.emplace_back([this] { WorkerLoop(); });
        }
    }

    SoundBufferLibrary::~SoundBufferLibrary() {
        {
            std::lock_guard<std::mutex> lock(m_jobMutex);
            m_stopping = true;
            m_jobs.clear();
        }
        m_jobCv.notify_all();
        for (auto& t : m_workers) {
            if (t.joinable()) t.join();
        }
    }

    void SoundBufferLibrary::Submit(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(m_jobMutex);
            if (m_stopping) return;
            m_jobs.push_back(std::move(job));
        }
        m_jobCv.notify_one();
    }

    void SoundBufferLibrary::WorkerLoop() {
        PROFILE_THREAD("Sound decode");
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(m_jobMutex);
                m_jobCv.wait(lock, [this] { return m_stopping || !m_jobs.empty(); });
                if (m_stopping) return;
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
            }
            job();
        }
    }

    void SoundBufferLibrary::GetCompleteBuffer(const std::string& path, BufferCallback callback) {
        std::shared_ptr<Entry>& slot = m_cache[path];
        if (slot) {
            std::shared_ptr<Audio::SoundBuffer> ready;
            {
                std::lock_guard<std::mutex> lock(slot->mutex);
                if (!slot->done) {
                    if (callback) slot->waiters.push_back(std::move(callback));
                    return;
                }
                ready = slot->buffer;
            }
            if (callback) callback(std::move(ready));
            return;
        }

        auto entry = std::make_shared<Entry>();
        if (callback) entry->waiters.push_back(std::move(callback));
        slot = entry;
        Submit([entry, path] {
            PROFILE_ZONE_N("Sound.Decode");
            std::vector<int16_t> pcm;
            Audio::AudioFormat format;
            std::string error;
            std::shared_ptr<Audio::SoundBuffer> buffer;
            if (Audio::DecodeOggFile(path, pcm, format, error)) {
                buffer = std::make_shared<Audio::SoundBuffer>(std::move(pcm), format);
            } else {
                Log::Warning("[Sound] Failed to load sound %s", error.c_str());
            }
            std::vector<BufferCallback> waiters;
            {
                std::lock_guard<std::mutex> lock(entry->mutex);
                entry->done = true;
                entry->buffer = buffer;
                waiters.swap(entry->waiters);
            }
            for (auto& w : waiters) w(buffer);
        });
    }

    void SoundBufferLibrary::GetStream(const std::string& path, bool looping, StreamCallback callback) {
        Submit([path, looping, callback = std::move(callback)] {
            std::string error;
            std::shared_ptr<Audio::AudioStream> stream = Audio::OggAudioStream::Open(path, looping, error);
            if (!stream) Log::Warning("[Sound] Failed to open stream %s", error.c_str());
            if (callback) callback(std::move(stream));
        });
    }

    void SoundBufferLibrary::Preload(const std::vector<std::string>& paths) {
        for (const std::string& path : paths) GetCompleteBuffer(path, nullptr);
    }

    void SoundBufferLibrary::Clear() {
        for (auto& [path, entry] : m_cache) {
            (void)path;
            std::lock_guard<std::mutex> lock(entry->mutex);
            // A decode still running has no AL buffer yet — uploads happen
            // only on attach — so there is nothing to delete for it, and the
            // buffer it produces dies with the entry.
            if (entry->done && entry->buffer) entry->buffer->DiscardAlBuffer();
            entry->waiters.clear();
        }
        m_cache.clear();
    }

    SoundBufferLibrary::Stats SoundBufferLibrary::GetStats() const {
        Stats s;
        for (const auto& [path, entry] : m_cache) {
            (void)path;
            std::lock_guard<std::mutex> lock(entry->mutex);
            if (entry->done && entry->buffer && entry->buffer->IsValid()) {
                ++s.count;
                s.bytes += entry->buffer->Size();
            }
        }
        return s;
    }

} // namespace Client

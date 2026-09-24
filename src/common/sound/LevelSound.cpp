// File: src/common/sound/LevelSound.cpp
#include "common/sound/LevelSound.hpp"

#include <atomic>
#include <chrono>

namespace Game::Sound {

    namespace {
        std::atomic<ServerSoundSink*> g_serverSink{nullptr};

        // SplitMix64 over an atomic counter: lock-free, well mixed, and a
        // fresh value per call from any thread — the property MC's
        // RandomSource-per-level gives the one server thread.
        std::atomic<uint64_t> g_seedState{
            static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
    }

    int64_t NextSeed() {
        uint64_t z = g_seedState.fetch_add(0x9E3779B97F4A7C15ull, std::memory_order_relaxed)
                   + 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return static_cast<int64_t>(z ^ (z >> 31));
    }

    void SetServerSink(ServerSoundSink* sink) { g_serverSink.store(sink, std::memory_order_release); }
    ServerSoundSink* GetServerSink() { return g_serverSink.load(std::memory_order_acquire); }

} // namespace Game::Sound

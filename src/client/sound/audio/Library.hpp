// File: src/client/sound/audio/Library.hpp
//
// MC com.mojang.blaze3d.audio.Library + DeviceList + PollingDeviceTracker: the
// OpenAL device and context, and the two channel pools.
//
// Pools (MC Library.init): the device's mono-source count (30 when it will not
// say) is split into sqrt(n) clamped 2..8 STREAMING channels and the rest,
// clamped 8..255, STATIC ones. A sound that finds its pool full does not play
// (SoundEngine.play → NOT_STARTED), which is how MC sheds sound under load.
//
// One deviation, for the render thread: MC's SoundEngine.play blocks on a
// future while the executor creates the AL source. Here the pool's LIMIT test
// (TryReserve) is an atomic counter the main thread checks directly, and the
// AL source is created on the executor afterwards — same accept/refuse answer,
// no wait.
//
// Thread ownership: Init / Cleanup on the main thread with the executor idle;
// CreateReserved / ReleaseChannel on the executor; TryReserve and the debug
// string from anywhere.
#pragma once

#include "client/sound/audio/Channel.hpp"
#include "client/sound/audio/Listener.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Client::Audio {

    // MC DeviceList.
    struct DeviceList {
        std::string              defaultDevice;
        std::vector<std::string> allDevices;

        bool operator==(const DeviceList& o) const {
            return defaultDevice == o.defaultDevice && allDevices == o.allDevices;
        }
        bool operator!=(const DeviceList& o) const { return !(*this == o); }
        bool Contains(const std::string& name) const;

        // MC DeviceList.query: ALC_ENUMERATE_ALL_EXT, or empty.
        static DeviceList Query();
    };

    // MC PollingDeviceTracker (AbstractDeviceTracker): the device list,
    // re-queried at most once a second.
    class DeviceTracker {
    public:
        explicit DeviceTracker(DeviceList initial) : m_list(std::move(initial)) {}
        DeviceList CurrentDevices() const;
        void ForceRefresh();
        // Main thread, every sound tick. The re-query itself is handed to
        // `runOffThread` (the sound executor), as MC hands it to its IO pool:
        // enumerating devices asks the OS audio stack and has no business on
        // the render thread.
        void Tick(const std::function<void(std::function<void()>)>& runOffThread);
        // The executor dropped its queue (SoundEngine.stopAll): a query that
        // was waiting in it will never run.
        void ResetPending() { m_updatePending.store(false); }

    private:
        mutable std::mutex m_mutex;
        DeviceList         m_list;
        std::chrono::steady_clock::time_point m_lastCheck{};
        std::atomic<bool>  m_updatePending{false};
    };

    class Library {
    public:
        enum class Pool : uint8_t { Static = 0, Streaming = 1 };

        Library() = default;
        ~Library();

        // MC Library.init. `preferredDevice` empty = the system default.
        // False (logged) when no device or context could be had — the engine
        // then stays silent, as MC turns sounds off.
        bool Init(const std::string& preferredDevice, const DeviceList& currentDevices, bool useHrtf);
        void Cleanup();

        Listener& GetListener() { return m_listener; }

        // See the header note. TryReserve takes a slot or refuses; a taken
        // slot is filled by CreateReserved (executor) or handed back by
        // CancelReservation.
        bool TryReserve(Pool pool);
        void CancelReservation(Pool pool);
        std::unique_ptr<Channel> CreateReserved(Pool pool);
        void ReleaseChannel(Pool pool, std::unique_ptr<Channel> channel);
        // Every channel released and the executor stopped: nothing is held.
        void ResetReservations() { m_used[0].store(0); m_used[1].store(0); }

        std::string CurrentDeviceName() const;
        bool IsCurrentDeviceDisconnected() const;
        // MC getChannelDebugString: "Sounds: used/max + used/max".
        std::string GetChannelDebugString() const;

    private:
        int GetChannelCount() const;

        ALCdevice*  m_device = nullptr;
        ALCcontext* m_context = nullptr;
        std::string m_deviceName = "(None)";
        bool        m_supportsDisconnections = false;

        std::array<int, 2>              m_limit{0, 0};
        std::array<std::atomic<int>, 2> m_used{};
        Listener                        m_listener;
    };

} // namespace Client::Audio

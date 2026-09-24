// File: src/client/sound/audio/Library.cpp
#include "client/sound/audio/Library.hpp"

#include "common/core/Log.hpp"

#include <AL/alext.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Client::Audio {

    // ── DeviceList ────────────────────────────────────────────────────────

    bool DeviceList::Contains(const std::string& name) const {
        return std::find(allDevices.begin(), allDevices.end(), name) != allDevices.end();
    }

    DeviceList DeviceList::Query() {
        DeviceList list;
        if (!alcIsExtensionPresent(nullptr, "ALC_ENUMERATE_ALL_EXT")) return list;
        // ALUtil.getStringList: NUL-separated, double-NUL terminated.
        if (const ALCchar* names = alcGetString(nullptr, ALC_ALL_DEVICES_SPECIFIER)) {
            for (const ALCchar* p = names; *p; p += std::char_traits<char>::length(p) + 1) {
                list.allDevices.emplace_back(p);
            }
        }
        if (const ALCchar* def = alcGetString(nullptr, ALC_DEFAULT_ALL_DEVICES_SPECIFIER)) {
            list.defaultDevice = def;
        }
        return list;
    }

    // ── DeviceTracker ─────────────────────────────────────────────────────

    DeviceList DeviceTracker::CurrentDevices() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_list;
    }

    void DeviceTracker::ForceRefresh() {
        m_lastCheck = std::chrono::steady_clock::now();
        DeviceList fresh = DeviceList::Query();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_list = std::move(fresh);
    }

    void DeviceTracker::Tick(const std::function<void(std::function<void()>)>& runOffThread) {
        // MC PollingDeviceTracker: at most once a second, and one query in
        // flight at a time (AbstractDeviceTracker.updatePending).
        const auto now = std::chrono::steady_clock::now();
        if (now - m_lastCheck < std::chrono::seconds(1)) return;
        m_lastCheck = now;
        bool expected = false;
        if (!m_updatePending.compare_exchange_strong(expected, true)) return;
        runOffThread([this] {
            DeviceList fresh = DeviceList::Query();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_list = std::move(fresh);
            }
            m_updatePending.store(false);
        });
    }

    // ── Library ───────────────────────────────────────────────────────────

    namespace {
        std::string QueryDeviceName(ALCdevice* device) {
            const ALCchar* name = alcGetString(device, ALC_ALL_DEVICES_SPECIFIER);
            if (!name) name = alcGetString(device, ALC_DEVICE_SPECIFIER);
            if (name) return name;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Unknown (%p)", static_cast<void*>(device));
            return buf;
        }

        ALCdevice* TryOpenDevice(const char* name) {
            ALCdevice* device = alcOpenDevice(name);
            if (device && !CheckALCError(device, "Open device")) return device;
            if (device) alcCloseDevice(device);
            return nullptr;
        }

        // MC openDeviceOrFallback: the chosen device, else the system
        // default, else whatever OpenAL opens for "no name".
        ALCdevice* OpenDeviceOrFallback(const std::string& preferred, const std::string& systemDefault) {
            ALCdevice* device = nullptr;
            if (!preferred.empty()) device = TryOpenDevice(preferred.c_str());
            if (!device && !systemDefault.empty()) device = TryOpenDevice(systemDefault.c_str());
            if (!device) device = TryOpenDevice(nullptr);
            return device;
        }
    } // namespace

    Library::~Library() { Cleanup(); }

    bool Library::Init(const std::string& preferredDevice, const DeviceList& currentDevices, bool useHrtf) {
        m_deviceName = "(None)";
        m_device = OpenDeviceOrFallback(preferredDevice, currentDevices.defaultDevice);
        if (!m_device) {
            Log::Error("[Sound] Failed to open OpenAL device");
            return false;
        }
        m_deviceName = QueryDeviceName(m_device);
        m_supportsDisconnections = false;

        // MC createAttributes: HRTF (directional audio) when the device has
        // any, and the output limiter on.
        std::vector<ALCint> attributes;
        ALCint numHrtf = 0;
        if (alcIsExtensionPresent(m_device, "ALC_SOFT_HRTF")) {
            alcGetIntegerv(m_device, ALC_NUM_HRTF_SPECIFIERS_SOFT, 1, &numHrtf);
        }
        if (numHrtf > 0) {
            attributes.push_back(ALC_HRTF_SOFT);
            attributes.push_back(useHrtf ? ALC_TRUE : ALC_FALSE);
            attributes.push_back(ALC_HRTF_ID_SOFT);
            attributes.push_back(0);
        }
        attributes.push_back(ALC_OUTPUT_LIMITER_SOFT);
        attributes.push_back(ALC_TRUE);
        attributes.push_back(0);

        m_context = alcCreateContext(m_device, attributes.data());
        if (CheckALCError(m_device, "Create context") || !m_context) {
            Log::Error("[Sound] Unable to create OpenAL context");
            Cleanup();
            return false;
        }
        alcMakeContextCurrent(m_context);

        const int total = GetChannelCount();
        const int streaming = std::clamp(static_cast<int>(std::sqrt(static_cast<float>(total))), 2, 8);
        const int statics = std::clamp(total - streaming, 8, 255);
        m_limit[static_cast<size_t>(Pool::Static)]    = statics;
        m_limit[static_cast<size_t>(Pool::Streaming)] = streaming;
        m_used[0].store(0);
        m_used[1].store(0);

        CheckALError("Initialization");
        if (!alIsExtensionPresent("AL_EXT_source_distance_model")) {
            Log::Error("[Sound] AL_EXT_source_distance_model is not supported");
            Cleanup();
            return false;
        }
        alEnable(AL_SOURCE_DISTANCE_MODEL);
        if (!alIsExtensionPresent("AL_EXT_LINEAR_DISTANCE")) {
            Log::Error("[Sound] AL_EXT_LINEAR_DISTANCE is not supported");
            Cleanup();
            return false;
        }
        CheckALError("Enable per-source distance models");
        Log::Info("[Sound] OpenAL initialized on device %s (%d static + %d streaming channels)",
                  m_deviceName.c_str(), statics, streaming);
        m_supportsDisconnections = alcIsExtensionPresent(m_device, "ALC_EXT_disconnect") == ALC_TRUE;
        return true;
    }

    int Library::GetChannelCount() const {
        ALCint size = 0;
        alcGetIntegerv(m_device, ALC_ATTRIBUTES_SIZE, 1, &size);
        if (CheckALCError(m_device, "Get attributes size") || size <= 0) return 30;
        std::vector<ALCint> attributes(static_cast<size_t>(size));
        alcGetIntegerv(m_device, ALC_ALL_ATTRIBUTES, size, attributes.data());
        if (CheckALCError(m_device, "Get attributes")) return 30;
        for (size_t pos = 0; pos + 1 < attributes.size(); pos += 2) {
            const ALCint attribute = attributes[pos];
            if (attribute == 0) break;
            if (attribute == ALC_MONO_SOURCES) return attributes[pos + 1];
        }
        return 30;
    }

    void Library::Cleanup() {
        if (m_context) {
            alcMakeContextCurrent(nullptr);
            alcDestroyContext(m_context);
            m_context = nullptr;
        }
        if (m_device) {
            alcCloseDevice(m_device);
            m_device = nullptr;
        }
        m_limit = {0, 0};
        m_used[0].store(0);
        m_used[1].store(0);
    }

    bool Library::TryReserve(Pool pool) {
        const size_t i = static_cast<size_t>(pool);
        const int limit = m_limit[i];
        int used = m_used[i].load(std::memory_order_relaxed);
        while (used < limit) {
            if (m_used[i].compare_exchange_weak(used, used + 1, std::memory_order_acq_rel)) return true;
        }
        return false;   // MC CountingChannelPool: "Maximum sound pool size reached"
    }

    void Library::CancelReservation(Pool pool) {
        m_used[static_cast<size_t>(pool)].fetch_sub(1, std::memory_order_acq_rel);
    }

    std::unique_ptr<Channel> Library::CreateReserved(Pool pool) {
        if (!m_context) {
            CancelReservation(pool);
            return nullptr;
        }
        std::unique_ptr<Channel> channel = Channel::Create();
        if (!channel) CancelReservation(pool);
        return channel;
    }

    void Library::ReleaseChannel(Pool pool, std::unique_ptr<Channel> channel) {
        if (!channel) return;
        channel->Destroy();
        CancelReservation(pool);
    }

    std::string Library::CurrentDeviceName() const { return m_deviceName; }

    bool Library::IsCurrentDeviceDisconnected() const {
        if (!m_supportsDisconnections || !m_device) return false;
        ALCint connected = ALC_TRUE;
        alcGetIntegerv(m_device, ALC_CONNECTED, 1, &connected);
        return connected == 0;
    }

    std::string Library::GetChannelDebugString() const {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Sounds: %d/%d + %d/%d",
                      m_used[0].load(), m_limit[0], m_used[1].load(), m_limit[1]);
        return buf;
    }

} // namespace Client::Audio

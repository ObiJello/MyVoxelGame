// File: src/common/core/HardwareProfile.cpp
#include "HardwareProfile.hpp"
#include "Log.hpp"

#include <algorithm>
#include <sstream>
#include <thread>

#if defined(__APPLE__)
    #include <sys/sysctl.h>
#elif defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <unistd.h>
#endif

namespace Core {

    namespace {

#if defined(__APPLE__)
        bool SysctlU32(const char* name, uint32_t& out) {
            uint32_t value = 0;
            size_t size = sizeof(value);
            if (sysctlbyname(name, &value, &size, nullptr, 0) != 0) return false;
            out = value;
            return true;
        }
        bool SysctlU64(const char* name, uint64_t& out) {
            uint64_t value = 0;
            size_t size = sizeof(value);
            if (sysctlbyname(name, &value, &size, nullptr, 0) != 0) return false;
            out = value;
            return true;
        }
#endif

        uint64_t DetectPhysicalMemory() {
#if defined(__APPLE__)
            uint64_t bytes = 0;
            if (SysctlU64("hw.memsize", bytes)) return bytes;
            return 0;
#elif defined(_WIN32)
            MEMORYSTATUSEX status{};
            status.dwLength = sizeof(status);
            if (GlobalMemoryStatusEx(&status)) return static_cast<uint64_t>(status.ullTotalPhys);
            return 0;
#else
            const long pages = sysconf(_SC_PHYS_PAGES);
            const long pageSize = sysconf(_SC_PAGE_SIZE);
            if (pages > 0 && pageSize > 0) {
                return static_cast<uint64_t>(pages) * static_cast<uint64_t>(pageSize);
            }
            return 0;
#endif
        }

    } // namespace

    const HardwareProfile& HardwareProfile::Get() {
        static const HardwareProfile profile = [] {
            HardwareProfile p = Detect();
            Log::Info("Hardware profile: %s", p.ToString().c_str());
            return p;
        }();
        return profile;
    }

    HardwareProfile HardwareProfile::Detect() {
        HardwareProfile p;

        const unsigned hw = std::thread::hardware_concurrency();
        p.logicalCores = hw > 0 ? static_cast<size_t>(hw) : 4;
        p.performanceCores = p.logicalCores;
        p.physicalMemoryBytes = DetectPhysicalMemory();

#if defined(__APPLE__)
        // perflevel0 is always the FASTEST level on Apple's scheme. Intel
        // Macs have no perflevel keys at all, so a failed lookup correctly
        // leaves the homogeneous answer in place.
        uint32_t perfCores = 0;
        if (SysctlU32("hw.perflevel0.logicalcpu", perfCores) && perfCores > 0) {
            p.performanceCores = std::min<size_t>(perfCores, p.logicalCores);
        }

        // hw.optional.arm64 reports the MACHINE, so it is 1 for an x86_64
        // slice running under Rosetta as well. proc_translated separates the
        // two; an Intel Mac has neither key and reads as false/false.
        uint32_t arm64 = 0;
        p.appleSilicon = SysctlU32("hw.optional.arm64", arm64) && arm64 != 0;
        uint32_t translated = 0;
        p.rosetta = SysctlU32("sysctl.proc_translated", translated) && translated != 0;
        p.intelMac = !p.appleSilicon;
#endif

        // The tier is deliberately conservative in ONE direction: it only
        // ever picks the lighter preset. A machine it misjudges as High runs
        // exactly what it always did, and the player can still choose Fast.
        //
        //   Intel Mac — every one is on Apple's legacy GL 4.1 driver, most on
        //   an integrated GPU whose "VRAM" is this same RAM.
        //   <= 4 logical cores — a 2-core/4-thread laptop chip.
        //   < 7.5 GB — the 4 GB and 6 GB machines; 8 GB is not low-end on its
        //   own (an M1 Air with 8 GB runs the High settings fine).
        constexpr uint64_t kLowMemoryBytes = uint64_t(7.5 * 1024.0 * 1024.0 * 1024.0);
        const bool lowMemory = p.physicalMemoryBytes > 0 && p.physicalMemoryBytes < kLowMemoryBytes;
        p.tier = (p.intelMac || p.logicalCores <= 4 || lowMemory) ? Tier::Low : Tier::High;

        return p;
    }

    std::string HardwareProfile::ToString() const {
        std::ostringstream ss;
        ss << "cores=" << logicalCores << " (performance=" << performanceCores << ")"
           << ", ram=" << (physicalMemoryBytes >> 20) << " MB"
#if defined(__APPLE__)
           << ", mac=" << (appleSilicon ? (rosetta ? "apple-silicon (rosetta)" : "apple-silicon")
                                        : "intel")
#endif
           << ", tier=" << (tier == Tier::Low ? "low" : "high");
        return ss.str();
    }

} // namespace Core

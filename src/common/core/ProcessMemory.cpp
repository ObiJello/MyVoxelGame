// File: src/common/core/ProcessMemory.cpp
#include "common/core/ProcessMemory.hpp"

#if defined(__APPLE__)
    #include <mach/mach.h>
#elif defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <psapi.h>
#else
    #include <cstdlib>
    #include <fstream>
    #include <string>
#endif

namespace Core {

    ProcessMemory QueryProcessMemory() {
        ProcessMemory m;
#if defined(__APPLE__)
        task_vm_info_data_t info{};
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) ==
            KERN_SUCCESS) {
            m.footprintBytes = info.phys_footprint;
            // The peak field arrived in a later revision of the struct; an
            // older kernel fills fewer words and leaves it zero.
            if (count >= TASK_VM_INFO_REV3_COUNT) {
                m.peakBytes = static_cast<uint64_t>(info.ledger_phys_footprint_peak);
            }
            if (m.peakBytes < m.footprintBytes) m.peakBytes = m.footprintBytes;
        }
#elif defined(_WIN32)
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                                 sizeof(pmc))) {
            m.footprintBytes = pmc.PrivateUsage;
            m.peakBytes = pmc.PeakPagefileUsage;
        }
#else
        if (std::ifstream st("/proc/self/status"); st) {
            std::string line;
            while (std::getline(st, line)) {
                if (line.rfind("VmRSS:", 0) == 0) {
                    m.footprintBytes = std::strtoull(line.c_str() + 6, nullptr, 10) * 1024;
                } else if (line.rfind("VmHWM:", 0) == 0) {
                    m.peakBytes = std::strtoull(line.c_str() + 6, nullptr, 10) * 1024;
                }
            }
        }
#endif
        return m;
    }

} // namespace Core

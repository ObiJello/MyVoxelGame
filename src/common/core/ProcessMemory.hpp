// File: src/common/core/ProcessMemory.hpp
//
// How much memory this process is charged for, for the once-a-second load
// reports (ServerStressStats) — the number that decides whether the machine
// starts paging.
//
//   macOS    phys_footprint (task_vm_info): what Activity Monitor's "Memory"
//            column shows. NOT resident_size, which stops counting a page the
//            moment the compressor takes it — under memory pressure resident
//            FALLS while the process keeps growing.
//   Windows  PrivateUsage (commit charge) / PeakPagefileUsage.
//   Linux    VmRSS / VmHWM.
#pragma once

#include <cstdint>

namespace Core {

    struct ProcessMemory {
        uint64_t footprintBytes = 0;   // 0 when the platform query failed
        uint64_t peakBytes = 0;        // lifetime peak of the same measure
    };

    ProcessMemory QueryProcessMemory();

} // namespace Core

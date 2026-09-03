// File: src/common/core/HardwareProfile.hpp
//
// One place that answers "what machine is this?" for every budget the
// engine sizes from hardware: worker-thread counts, mesh upload permits,
// the client chunk retention cache, the HiDPI framebuffer default and the
// first-run graphics preset.
//
// MC does the same reading in scattered places — Util.maxAllowedExecutorThreads
// (availableProcessors - 1), Options' `largeDistances = maxMemory >= 1 GB`
// gate on the render-distance slider, GraphicsWorkarounds' vendor sniffing,
// SectionBufferBuilderPool's maxMemory * 0.3 cap — this collects ours so a
// weak machine is recognised ONCE and every consumer scales from the same
// facts.
//
// Detection is cheap and runs on first use; the result never changes for
// the life of the process.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace Core {

    struct HardwareProfile {
        // std::thread::hardware_concurrency() — counts hyperthreads, so a
        // 2-core/4-thread Intel i5 reports 4.
        size_t   logicalCores = 4;
        // Fast cores only. Equals logicalCores on a homogeneous CPU; on Apple
        // Silicon it is the performance-core count (hw.perflevel0).
        size_t   performanceCores = 4;
        // Physical RAM. On every Mac and on integrated-GPU PCs this is ALSO
        // the pool the GPU draws from, which is why GPU-side budgets read it.
        uint64_t physicalMemoryBytes = 0;

        // macOS specifics. `appleSilicon` is the MACHINE (true under Rosetta
        // too); `rosetta` means this is the x86_64 slice translated on an
        // Apple Silicon Mac; `intelMac` is a real Intel Mac.
        bool appleSilicon = false;
        bool rosetta      = false;
        bool intelMac     = false;

        // The one coarse decision the profile makes itself. LOW picks the
        // Fast graphics preset on first run and is what the video-settings
        // tooltip means by "recommended for this machine". Everything else
        // scales continuously from the numbers above rather than from the
        // tier, so a machine just over the line degrades gracefully.
        enum class Tier { Low, High };
        Tier tier = Tier::High;
        bool IsLowEnd() const { return tier == Tier::Low; }

        // Detected once, cached for the process.
        static const HardwareProfile& Get();

        std::string ToString() const;

    private:
        static HardwareProfile Detect();
    };

} // namespace Core

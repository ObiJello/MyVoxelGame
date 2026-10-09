// File: src/client/renderer/backend/metal/MetalInstrumentation.mm
//
// What the Metal backend tells the tools and the engine about itself:
// per-encoder GPU timestamps (counter sample buffers at stage boundaries),
// the GPU timer API over them, Tracy GPU zones, Tracy plots, command-buffer
// error reporting, GPU frame capture, debug labels and the device's memory
// figures. See MetalBackend.hpp (m_gpu) for the design and the gates.
#ifdef HAS_METAL

#import "MetalBackend.hpp"
#include "MetalSignposts.hpp"

#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "platform/GameDirectory.hpp"

#ifdef TRACY_ENABLE
#include <client/TracyProfiler.hpp>
#include <common/TracyQueue.hpp>
#endif

#include <algorithm>
#include <mach/mach_time.h>
#include <QuartzCore/QuartzCore.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

namespace Render {

    // ========================================================================
    // COMMAND BUFFERS: errors
    // ========================================================================

    id<MTLCommandBuffer> MetalBackend::MakeCommandBuffer(NSString* label, bool watchErrors) {
        id<MTLCommandBuffer> cmd = nil;
        if (m_errorOptions) {
            // Per-encoder execution status on a failure (which encoder
            // faulted, which were affected): costs a little per encoder, so
            // only behind OBEY_MTL_ERRORS=1.
            MTLCommandBufferDescriptor* d = [MTLCommandBufferDescriptor new];
            d.errorOptions = MTLCommandBufferErrorOptionEncoderExecutionStatus;
            d.retainedReferences = YES;
            cmd = [m_queue commandBufferWithDescriptor:d];
        } else {
            cmd = [m_queue commandBuffer];
        }
        cmd.label = label;
        if (watchErrors) {
            // The frame's own handler (BeginFrame) reports as well; this is
            // for the upload buffer, which had none.
            const char* what = label.UTF8String;
            [cmd addCompletedHandler:^(id<MTLCommandBuffer> done) {
                if (done.status == MTLCommandBufferStatusError) {
                    Log::Error("MetalBackend: '%s' command buffer failed: %s", what,
                               done.error ? done.error.localizedDescription.UTF8String : "unknown error");
                }
            }];
        }
        return cmd;
    }

    void MetalBackend::ReportCommandBufferError(id<MTLCommandBuffer> cmd, const char* what) {
        // Called from the completed handler (Metal's thread): Log is
        // mutex-protected; nothing of the backend is touched beyond the
        // atomic counter.
        if (cmd.status != MTLCommandBufferStatusError) return;
        NSError* e = cmd.error;
        Log::Error("MetalBackend: %s failed: %s (%ld)", what,
                   e ? e.localizedDescription.UTF8String : "unknown error", e ? static_cast<long>(e.code) : 0L);
        if (e) {
            for (id<MTLCommandBufferEncoderInfo> info in e.userInfo[MTLCommandBufferEncoderInfoErrorKey]) {
                const char* state = info.errorState == MTLCommandEncoderErrorStateFaulted ? "FAULTED"
                                  : info.errorState == MTLCommandEncoderErrorStateAffected ? "affected"
                                  : info.errorState == MTLCommandEncoderErrorStatePending ? "pending"
                                  : info.errorState == MTLCommandEncoderErrorStateCompleted ? "completed" : "unknown";
                Log::Error("  encoder '%s': %s", info.label ? info.label.UTF8String : "?", state);
            }
        }
        m_cmdErrors.fetch_add(1, std::memory_order_relaxed);
    }

    // ========================================================================
    // GPU TIMING: counter sample buffers
    // ========================================================================

    bool MetalBackend::ProbeGpuTiming() {
        m_gpu.probed = true;
        m_gpu.supported = false;
        if (m_metal4) {
            // Metal 4: a timestamp counter heap per slot, the same sample
            // layout as the sample buffers (4 per render encoder, 2 per copy).
            if (@available(macOS 26.0, *)) {
                @autoreleasepool {
                    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
                        MTL4CounterHeapDescriptor* d = [MTL4CounterHeapDescriptor new];
                        d.type = MTL4CounterHeapTypeTimestamp;
                        d.count = GpuTiming::kSamplesPerSlot;
                        NSError* error = nil;
                        id<MTL4CounterHeap> heap = [m_device newCounterHeapWithDescriptor:d error:&error];
                        if (!heap) {
                            Log::Warning("Metal 4: no timestamp counter heap (%s) - GPU timers off",
                                         error ? error.localizedDescription.UTF8String : "unknown");
                            return false;
                        }
                        heap.label = [NSString stringWithFormat:@"GPU timestamps [%u]", i];
                        m_gpu.slots[i].heap = heap;
                        m_gpu.slots[i].encoders.reserve(32);
                        m_gpu.slots[i].durations.reserve(32);
                    }
                }
                m_gpu.supported = true;
                Log::Info("MetalBackend: GPU timers on (Metal 4 counter heap, %u samples per frame slot)", GpuTiming::kSamplesPerSlot);
                return true;
            }
            return false;
        }
        for (id<MTLCounterSet> cs in m_device.counterSets) {
            if ([cs.name isEqualToString:MTLCommonCounterSetTimestamp]) m_gpu.counterSet = cs;
        }
        const bool stage = [m_device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary];
        if (!m_gpu.counterSet || !stage) {
            Log::Warning("MetalBackend: GPU timers unsupported on this device (timestamp counters %s, stage-boundary "
                         "sampling %s) - per-encoder GPU times stay off", m_gpu.counterSet ? "yes" : "no",
                         stage ? "yes" : "no");
            return false;
        }
        @autoreleasepool {
            for (uint32_t s = 0; s < kFramesInFlight; ++s) {
                MTLCounterSampleBufferDescriptor* d = [MTLCounterSampleBufferDescriptor new];
                d.counterSet = m_gpu.counterSet;
                d.sampleCount = GpuTiming::kSamplesPerSlot;
                d.storageMode = MTLStorageModeShared;
                d.label = [NSString stringWithFormat:@"GPU timestamps [slot %u]", s];
                NSError* error = nil;
                m_gpu.slots[s].buffer = [m_device newCounterSampleBufferWithDescriptor:d error:&error];
                if (!m_gpu.slots[s].buffer) {
                    Log::Warning("MetalBackend: counter sample buffer failed: %s - GPU timers stay off",
                                 error ? error.localizedDescription.UTF8String : "unknown");
                    for (GpuTiming::Slot& slot : m_gpu.slots) slot.buffer = nil;
                    return false;
                }
                m_gpu.slots[s].encoders.reserve(32);
                m_gpu.slots[s].durations.reserve(32);
            }
        }
        [m_device sampleTimestamps:&m_gpu.cpuNow gpuTimestamp:&m_gpu.gpuNow];
        m_gpu.supported = true;
        Log::Info("MetalBackend: GPU timers on (stage-boundary timestamps, %u samples per frame slot; draw-boundary "
                  "sampling %s) - the Metal HUD's encoder timing is blank while they run",
                  GpuTiming::kSamplesPerSlot,
                  [m_device supportsCounterSampling:MTLCounterSamplingPointAtDrawBoundary] ? "available" : "unavailable");
        return true;
    }

    const char* MetalBackend::InternLabel(NSString* label) {
        if (!label) return "?";
        std::string key = label.UTF8String;
        auto it = m_internedLabels.find(key);
        if (it != m_internedLabels.end()) return it->second->c_str();
        auto owned = std::make_unique<std::string>(key);
        const char* c = owned->c_str();
        m_internedLabels.emplace(std::move(key), std::move(owned));
        return c;
    }

    uint32_t MetalBackend::AttachRenderSamples(MTLRenderPassDescriptor* desc, uint8_t kind, bool shared, const char* label) {
        if (!m_gpu.active || !desc) return UINT32_MAX;
        GpuTiming::Slot& slot = m_gpu.slots[m_currentFrame];
        if (slot.used + 4 > GpuTiming::kSamplesPerSlot) {
            ++m_gpu.dropped;
            return UINT32_MAX;
        }
        if (!m_metal4) {
            // Metal 4 writes the heap's samples itself (M4MakeRenderEncoder
            // the stage starts, M4WriteEncoderEnd the stage ends); the entry
            // below is the same.
            MTLRenderPassSampleBufferAttachmentDescriptor* a = desc.sampleBufferAttachments[0];
            a.sampleBuffer = slot.buffer;
            a.startOfVertexSampleIndex = slot.used;
            a.endOfVertexSampleIndex = slot.used + 1;
            a.startOfFragmentSampleIndex = slot.used + 2;
            a.endOfFragmentSampleIndex = slot.used + 3;
        }
        const uint32_t first = slot.used;
        slot.encoders.push_back({first, kind, false, shared, label,
                                 m_debugGroups.empty() ? nullptr : m_debugGroups.back()});
        slot.used += 4;
        return first;
    }

    id MetalBackend::MakeBlitEncoder(id<MTLCommandBuffer> cmd, NSString* label, bool afterFragment) {
        ++m_counters.blits;
        ++m_counters.encoders;
        if (m_metal4) {
            // The frame's copies (texture flush, readback, framebuffer copy):
            // a compute encoder on the frame's Metal 4 command buffer, after
            // the fragment work it reads.
            (void)cmd;
            return M4MakeCopyEncoder(m_cmd4, label, afterFragment);
        }
        // The frame's and the upload buffer's blits can be timed (the
        // uploads commit first on the same queue, so their samples are
        // complete when the frame's are); blits recorded before any frame
        // (loading) are only labelled.
        const bool timed = m_gpu.active && m_frameActive && (cmd == m_cmd || cmd == m_uploadCmd);
        GpuTiming::Slot& slot = m_gpu.slots[m_currentFrame];
        id<MTLBlitCommandEncoder> blit = nil;
        if (timed && slot.used + 2 <= GpuTiming::kSamplesPerSlot) {
            MTLBlitPassDescriptor* d = [MTLBlitPassDescriptor blitPassDescriptor];
            MTLBlitPassSampleBufferAttachmentDescriptor* a = d.sampleBufferAttachments[0];
            a.sampleBuffer = slot.buffer;
            a.startOfEncoderSampleIndex = slot.used;
            a.endOfEncoderSampleIndex = slot.used + 1;
            const char* name = InternLabel(label);
            const uint32_t first = slot.used;
            slot.encoders.push_back({first, 4, true, false, name,
                                     m_debugGroups.empty() ? nullptr : m_debugGroups.back()});
            slot.used += 2;
            TracyGpuZone(m_currentFrame, first, first + 1, TracySrcloc(name, nullptr, 0x3C78D8));
            blit = [cmd blitCommandEncoderWithDescriptor:d];
        } else {
            if (timed) ++m_gpu.dropped;
            blit = [cmd blitCommandEncoder];
        }
        blit.label = label;
        return blit;
    }

    void MetalBackend::EndBlitEncoder(id blit) {
        if (!blit) return;
        if (m_metal4) M4WriteCopyEnd(blit);
        [(id<MTLCommandEncoder>)blit endEncoding];
    }

    void MetalBackend::ResolveGpuTiming(uint32_t slotIndex) {
        // Called from BeginFrame right after the frame-slot wait: the slot's
        // last command buffer has completed (the semaphore says so), so its
        // samples are final and its GPUStartTime / GPUEndTime are set.
        GpuTiming::Slot& slot = m_gpu.slots[slotIndex];
        if (m_metal4) {
            // The commit feedback's span (M4CommitFrame).
            const double start = m_m4.gpuStart[slotIndex], end = m_m4.gpuEnd[slotIndex];
            if (end > start) {
                m_gpu.lastFrameGpuMs = static_cast<float>((end - start) * 1000.0);
                PROFILE_PLOT("Mtl/GpuFrameUs", static_cast<int64_t>(m_gpu.lastFrameGpuMs * 1000.0f));
                if (slot.commitHostTime > 0.0) {
                    PROFILE_PLOT("Mtl/GpuLatencyUs", static_cast<int64_t>(std::max(0.0, start - slot.commitHostTime) * 1e6));
                }
            }
            // The encoder timestamps follow below, from the counter heap.
        } else {
        id<MTLCommandBuffer> cmd = m_slotCommandBuffers[slotIndex];
        if (cmd && cmd.status == MTLCommandBufferStatusCompleted) {
            m_gpu.lastFrameGpuMs = static_cast<float>(std::max(0.0, (cmd.GPUEndTime - cmd.GPUStartTime) * 1000.0));
            PROFILE_PLOT("Mtl/GpuFrameUs", static_cast<int64_t>(m_gpu.lastFrameGpuMs * 1000.0f));
            if (slot.commitHostTime > 0.0) {
                PROFILE_PLOT("Mtl/GpuLatencyUs",
                             static_cast<int64_t>(std::max(0.0, cmd.GPUStartTime - slot.commitHostTime) * 1e6));
                PROFILE_PLOT("Mtl/GpuSchedUs",
                             static_cast<int64_t>(std::max(0.0, cmd.kernelEndTime - cmd.kernelStartTime) * 1e6));
            }
        }
        }
        slot.durations.clear();
        if (slot.encoders.empty() || (!slot.buffer && !slot.heap)) return;
        @autoreleasepool {
            // The samples, from the Metal 3 sample buffer or the Metal 4
            // counter heap (the same layout and timebase).
            struct Stamp { uint64_t timestamp; };
            std::vector<Stamp> stamps;
            if (slot.buffer) {
                NSData* data = [slot.buffer resolveCounterRange:NSMakeRange(0, slot.used)];
                const auto* src = static_cast<const MTLCounterResultTimestamp*>(data.bytes);
                const NSUInteger count = data.length / sizeof(MTLCounterResultTimestamp);
                for (NSUInteger i = 0; i < count; ++i) stamps.push_back({src[i].timestamp});
            } else if (@available(macOS 26.0, *)) {
                id<MTL4CounterHeap> heap = slot.heap;
                NSData* data = [heap resolveCounterRange:NSMakeRange(0, slot.used)];
                const auto* src = static_cast<const MTL4TimestampHeapEntry*>(data.bytes);
                const NSUInteger count = data.length / sizeof(MTL4TimestampHeapEntry);
                for (NSUInteger i = 0; i < count; ++i) stamps.push_back({src[i].timestamp});
                [heap invalidateCounterRange:NSMakeRange(0, slot.used)];
                M4CalibrateHeap(stamps.empty() ? 0 : stamps[0].timestamp, m_m4.gpuStart[slotIndex]);
            }
            const Stamp* ts = stamps.data();
            const NSUInteger n = stamps.size();
            m_gpu.results.clear();
            float frameUs = 0, targetUs = 0, oitUs = 0, blitUs = 0, upscaleUs = 0;
            auto valid = [&](uint32_t i) {
                return i < n && ts[i].timestamp != 0 && ts[i].timestamp != MTLCounterErrorValue;
            };
            auto span = [&](uint32_t a, uint32_t b) -> float {   // ms between two samples, 0 when unsampled
                if (!valid(a) || !valid(b) || ts[b].timestamp < ts[a].timestamp) {
                    ++m_gpu.missing;
                    return 0.0f;
                }
                const double tickNs = m_metal4 ? m_m4.tickNs : m_gpu.ticksToNs;
                const double ms = static_cast<double>(ts[b].timestamp - ts[a].timestamp) * tickNs / 1e6;
                if (ms > 250.0) {   // not one encoder's span: a stale or mispaired entry
                    ++m_gpu.missing;
                    return 0.0f;
                }
                return static_cast<float>(ms);
            };
            for (const GpuTiming::Encoder& e : slot.encoders) {
                GpuEncoderTiming t;
                t.label = e.label;
                t.stage = e.stage;
                t.kind = e.kind;
                t.shared = e.shared;
                t.frame = slot.frame;
                const uint32_t last = e.blit ? e.first + 1 : e.first + 3;
                if (e.blit) {
                    t.totalMs = span(e.first, e.first + 1);
                } else {
                    t.vertexMs = span(e.first, e.first + 1);
                    t.fragmentMs = span(e.first + 2, e.first + 3);
                    t.totalMs = span(e.first, e.first + 3);
                }
                // The Tracy zone's GPU times (an unsampled encoder — Metal
                // drops an empty one with its samples — gets a 5-tick stub
                // at the latest known time, so its begin/end still pair).
                if (valid(e.first) && valid(last)) {
                    TracyGpuTime(slotIndex, e.first, ts[e.first].timestamp);
                    TracyGpuTime(slotIndex, last, ts[last].timestamp);
                } else {
                    TracyGpuTime(slotIndex, e.first, 0);
                    TracyGpuTime(slotIndex, last, 0);
                }
                slot.durations.push_back(t.totalMs);
                m_gpu.results.push_back(t);
                const float us = t.totalMs * 1000.0f;
                switch (e.kind) {
                    case 0: frameUs += us; break;
                    case 1: targetUs += us; break;
                    case 2: oitUs += us; break;
                    case 3: upscaleUs += us; break;
                    default: blitUs += us; break;
                }
                // A stage that owns its encoder (targets, OIT, blits, the
                // upscale) is exact per stage; the frame's pass is shared.
                if (e.stage && !e.shared) {
                    // Tracy keys plots by pointer: the plot name must be a
                    // persistent string, which the interned "Gpu/<stage>Us" is.
                    const char* plot = InternLabel([NSString stringWithFormat:@"Gpu/%sUs", e.stage]);
                    PROFILE_PLOT(plot, static_cast<int64_t>(us));
                }
            }
            m_gpu.resultsFrame = slot.frame;
            PROFILE_PLOT("Gpu/EncFrameUs", static_cast<int64_t>(frameUs));
            PROFILE_PLOT("Gpu/EncTargetsUs", static_cast<int64_t>(targetUs));
            PROFILE_PLOT("Gpu/EncOitUs", static_cast<int64_t>(oitUs));
            PROFILE_PLOT("Gpu/EncBlitsUs", static_cast<int64_t>(blitUs));
            PROFILE_PLOT("Gpu/EncUpscaleUs", static_cast<int64_t>(upscaleUs));
            PROFILE_PLOT("Mtl/SamplesMissing", static_cast<int64_t>(m_gpu.missing));
            PROFILE_PLOT("Mtl/EncodersDropped", static_cast<int64_t>(m_gpu.dropped));
        }
    }

    bool MetalBackend::GetGpuEncoderTimings(std::vector<GpuEncoderTiming>& out) const {
        if (m_gpu.results.empty()) return false;
        out = m_gpu.results;
        return true;
    }

    // ── The RenderBackend timer API over the encoder timings ─────────────

    GPUTimerHandle MetalBackend::BeginGPUTimer(const std::string& /*name*/) {
        if (!m_frameActive || !m_gpu.active) return INVALID_GPU_TIMER;
        const uint32_t handle = AllocHandle();
        GpuTimer t;
        t.frame = m_frameNumber;
        t.slot = m_currentFrame;
        t.beginOrdinal = static_cast<uint32_t>(m_gpu.slots[m_currentFrame].encoders.size());
        m_gpuTimers[handle] = t;
        return handle;
    }

    void MetalBackend::EndGPUTimer(GPUTimerHandle handle) {
        auto it = m_gpuTimers.find(handle);
        if (it == m_gpuTimers.end()) return;
        it->second.endOrdinal = static_cast<uint32_t>(m_gpu.slots[it->second.slot].encoders.size());
        it->second.ended = true;
    }

    float MetalBackend::GetGPUTimerResultMs(GPUTimerHandle handle) {
        auto it = m_gpuTimers.find(handle);
        if (it == m_gpuTimers.end()) return -1.0f;
        const GpuTimer& t = it->second;
        if (!t.ended) return -1.0f;
        const GpuTiming::Slot& slot = m_gpu.slots[t.slot];
        // Resolved when the slot's frame has been resolved and it is still
        // this timer's frame (the slot is reused two frames later; a timer
        // polled after that reads 0 rather than another frame's encoders).
        if (slot.frame != t.frame || m_gpu.resultsFrame != t.frame) {
            if (m_gpu.resultsFrame > t.frame || slot.frame > t.frame) {
                m_gpuTimers.erase(it);
                return 0.0f;
            }
            return -1.0f;   // non-blocking: poll again
        }
        float ms = 0.0f;
        for (uint32_t i = t.beginOrdinal; i < t.endOrdinal && i < slot.durations.size(); ++i) ms += slot.durations[i];
        m_gpuTimers.erase(it);
        return ms;
    }

    // ========================================================================
    // TRACY GPU ZONES
    // ========================================================================
    void MetalBackend::M4CalibrateHeap(uint64_t firstStamp, double gpuStart) {
        // Called once per resolved frame with the frame's first heap stamp
        // (the first encoder's vertex start, or 0 when nothing was sampled)
        // and the frame's GPU start from the commit feedback. The two clocks
        // run at a fixed ratio (the tick), but their offset jitters by the
        // encoder's start latency, so the tick is a slope over a long
        // baseline rather than a frame-to-frame ratio: 0.5 ms of jitter is
        // 20 % of one 2.5 ms frame and 0.02 % of a second.
        if (firstStamp == 0 || gpuStart <= 0.0) return;
        if (m_m4.calStamp == 0 || firstStamp <= m_m4.calStamp || gpuStart <= m_m4.calStart) {
            m_m4.calStamp = firstStamp;
            m_m4.calStart = gpuStart;
            return;
        }
        const double seconds = gpuStart - m_m4.calStart;
        if (seconds >= 0.25) {
            const double measured = seconds * 1e9 / static_cast<double>(firstStamp - m_m4.calStamp);
            // The heap counts mach_absolute_time ticks on Apple silicon (the
            // slope reads 42.2 ns after 2 s against a 41.667 ns timebase,
            // M4, 2026-10-08): when the slope agrees with the timebase within
            // 2 %, the timebase is the tick, exact from the first quarter
            // second; a GPU that disagrees keeps its measured slope.
            mach_timebase_info_data_t tb = {};
            mach_timebase_info(&tb);
            const double machNs = tb.denom ? static_cast<double>(tb.numer) / tb.denom : 0.0;
            const bool mach = machNs > 0.0 && std::fabs(measured - machNs) <= machNs * 0.02;
            m_m4.tickNs = mach ? machNs : measured;
            if (!m_m4.tickLogged && seconds >= 2.0) {
                m_m4.tickLogged = true;
                Log::Info("MetalBackend: Metal 4 counter heap tick = %.3f ns (%s; slope %.3f ns over %.1f s, mach timebase %.3f ns)",
                          m_m4.tickNs, mach ? "the mach timebase" : "measured", measured, seconds, machNs);
            }
        }
        if (m_m4.tickNs <= 0.0) return;
        // The anchor for Tracy: this stamp's place in the sampleTimestamps
        // GPU timebase — "now" in that timebase, less how long ago the
        // frame started on CACurrentMediaTime's clock (the feedback's), the
        // GPU timestamp advancing in nanoseconds on Apple GPUs (ticksToNs
        // 1.0). Refreshed every second; a sampleTimestamps call is not free.
        const double now = CACurrentMediaTime();
        if (now - m_m4.anchorAt >= 1.0 || m_m4.anchorStamp == 0) {
            MTLTimestamp cpu = 0, gpu = 0;
            [m_device sampleTimestamps:&cpu gpuTimestamp:&gpu];
            m_m4.anchorStamp = firstStamp;
            m_m4.anchorGpuNs = static_cast<double>(gpu) - (now - gpuStart) * 1e9 * m_gpu.ticksToNs;
            m_m4.anchorAt = now;
        }
    }

    // The same queue items TracyMetal.hmm would send, with the backend's own
    // sample indices as query ids: (slot, index) -> slot * kSamplesPerSlot +
    // index, unique while both frame slots are in flight. Timestamps go out
    // in Tracy's GPU timebase: the calibration pair taken at creation, and
    // ticks scaled by ticksToNs (1.0 on Apple GPUs).

    void MetalBackend::TracyGpuInit() {
#ifdef TRACY_ENABLE
        if (m_tracyGpu.context >= 0) return;
        MTLTimestamp cpu = 0, gpu = 0;
        [m_device sampleTimestamps:&cpu gpuTimestamp:&gpu];
        m_tracyGpu.context = tracy::NextGpuContextId();
        m_tracyGpu.gpu0 = static_cast<int64_t>(gpu);
        m_tracyGpu.mostRecent = m_tracyGpu.gpu0;
        {
            auto* item = tracy::Profiler::QueueSerial();
            tracy::MemWrite(&item->hdr.type, tracy::QueueType::GpuNewContext);
            tracy::MemWrite(&item->gpuNewContext.cpuTime, tracy::Profiler::GetTime());
            tracy::MemWrite(&item->gpuNewContext.gpuTime, m_tracyGpu.gpu0);
            tracy::MemWrite(&item->gpuNewContext.thread, uint32_t(0));
            tracy::MemWrite(&item->gpuNewContext.period, 1.0f);
            tracy::MemWrite(&item->gpuNewContext.context, static_cast<uint8_t>(m_tracyGpu.context));
            tracy::MemWrite(&item->gpuNewContext.flags, tracy::GpuContextFlags(0));
            tracy::MemWrite(&item->gpuNewContext.type, tracy::GpuContextType::Metal);
            tracy::Profiler::QueueSerialFinish();
        }
        {
            static const char kName[] = "Metal GPU";
            const uint16_t len = sizeof(kName) - 1;
            auto* ptr = static_cast<char*>(tracy::tracy_malloc(len));
            std::memcpy(ptr, kName, len);
            auto* item = tracy::Profiler::QueueSerial();
            tracy::MemWrite(&item->hdr.type, tracy::QueueType::GpuContextName);
            tracy::MemWrite(&item->gpuContextNameFat.context, static_cast<uint8_t>(m_tracyGpu.context));
            tracy::MemWrite(&item->gpuContextNameFat.ptr, reinterpret_cast<uint64_t>(ptr));
            tracy::MemWrite(&item->gpuContextNameFat.size, len);
            tracy::Profiler::QueueSerialFinish();
        }
#endif
    }

    void MetalBackend::TracyGpuZone(uint32_t slot, uint32_t beginIndex, uint32_t endIndex,
                                    const tracy::SourceLocationData* srcloc) {
#ifdef TRACY_ENABLE
        if (m_tracyGpu.context < 0 || !srcloc) return;
        const uint16_t base = static_cast<uint16_t>(slot * GpuTiming::kSamplesPerSlot);
        {
            auto* item = tracy::Profiler::QueueSerial();
            tracy::MemWrite(&item->hdr.type, tracy::QueueType::GpuZoneBeginSerial);
            tracy::MemWrite(&item->gpuZoneBegin.cpuTime, tracy::Profiler::GetTime());
            tracy::MemWrite(&item->gpuZoneBegin.srcloc, reinterpret_cast<uint64_t>(srcloc));
            tracy::MemWrite(&item->gpuZoneBegin.thread, tracy::GetThreadHandle());
            tracy::MemWrite(&item->gpuZoneBegin.queryId, static_cast<uint16_t>(base + beginIndex));
            tracy::MemWrite(&item->gpuZoneBegin.context, static_cast<uint8_t>(m_tracyGpu.context));
            tracy::Profiler::QueueSerialFinish();
        }
        {
            auto* item = tracy::Profiler::QueueSerial();
            tracy::MemWrite(&item->hdr.type, tracy::QueueType::GpuZoneEndSerial);
            tracy::MemWrite(&item->gpuZoneEnd.cpuTime, tracy::Profiler::GetTime());
            tracy::MemWrite(&item->gpuZoneEnd.thread, tracy::GetThreadHandle());
            tracy::MemWrite(&item->gpuZoneEnd.queryId, static_cast<uint16_t>(base + endIndex));
            tracy::MemWrite(&item->gpuZoneEnd.context, static_cast<uint8_t>(m_tracyGpu.context));
            tracy::Profiler::QueueSerialFinish();
        }
#else
        (void)slot; (void)beginIndex; (void)endIndex; (void)srcloc;
#endif
    }

    void MetalBackend::TracyGpuTime(uint32_t slot, uint32_t index, uint64_t ticks) {
#ifdef TRACY_ENABLE
        if (m_tracyGpu.context < 0) return;
        int64_t t;
        if (ticks == 0 || (m_metal4 && m_m4.anchorStamp == 0)) {
            t = m_tracyGpu.mostRecent + 5;   // an unsampled encoder: a stub, as TracyMetal does
        } else if (m_metal4) {
            // Heap ticks onto the context's timebase through the anchor
            // (M4CalibrateHeap); the stamps are before it or soon after it.
            t = static_cast<int64_t>(m_m4.anchorGpuNs +
                                     (static_cast<double>(ticks) - static_cast<double>(m_m4.anchorStamp)) * m_m4.tickNs);
        } else {
            t = m_tracyGpu.gpu0 + static_cast<int64_t>((static_cast<double>(ticks) - static_cast<double>(m_tracyGpu.gpu0)) *
                                                       m_gpu.ticksToNs);
        }
        m_tracyGpu.mostRecent = std::max(m_tracyGpu.mostRecent, t);
        auto* item = tracy::Profiler::QueueSerial();
        tracy::MemWrite(&item->hdr.type, tracy::QueueType::GpuTime);
        tracy::MemWrite(&item->gpuTime.gpuTime, t);
        tracy::MemWrite(&item->gpuTime.queryId, static_cast<uint16_t>(slot * GpuTiming::kSamplesPerSlot + index));
        tracy::MemWrite(&item->gpuTime.context, static_cast<uint8_t>(m_tracyGpu.context));
        tracy::Profiler::QueueSerialFinish();
#else
        (void)slot; (void)index; (void)ticks;
#endif
    }

    const tracy::SourceLocationData* MetalBackend::TracySrcloc(const char* encoderLabel, const char* stage,
                                                               uint32_t color) {
#ifdef TRACY_ENABLE
        std::string key = encoderLabel ? encoderLabel : "?";
        if (stage) {
            key += " \xE2\x96\xB8 ";   // ▸
            key += stage;
        }
        auto it = m_tracySrclocs.find(key);
        if (it != m_tracySrclocs.end()) return it->second;
        // Tracy keeps the pointer for the run and fetches the strings once:
        // both the record and its name string live until exit.
        char* name = static_cast<char*>(std::malloc(key.size() + 1));
        std::memcpy(name, key.c_str(), key.size() + 1);
        auto* loc = new tracy::SourceLocationData{name, "MetalBackend", "MetalInstrumentation.mm", 0, color};
        m_tracySrclocs[key] = loc;
        return loc;
#else
        (void)encoderLabel; (void)stage; (void)color;
        return nullptr;
#endif
    }

    // ========================================================================
    // PLOTS AND MEMORY
    // ========================================================================

    void MetalBackend::EmitFramePlots() {
        PROFILE_PLOT("Mtl/Draws", static_cast<int64_t>(m_counters.draws));
        PROFILE_PLOT("Mtl/Encoders", static_cast<int64_t>(m_counters.encoders));
        PROFILE_PLOT("Mtl/Blits", static_cast<int64_t>(m_counters.blits));
        PROFILE_PLOT("Mtl/PipelinesBuilt", static_cast<int64_t>(m_counters.pipelinesBuilt));
        PROFILE_PLOT("Mtl/PipelinesTotal", static_cast<int64_t>(m_pipelines.size()));
        PROFILE_PLOT("Mtl/TexUpdates", static_cast<int64_t>(m_counters.texUpdates));
        if (m_metal4) {
            PROFILE_PLOT("Mtl/M4Adds", static_cast<int64_t>(m_m4.adds));
            PROFILE_PLOT("Mtl/M4Removes", static_cast<int64_t>(m_m4.removes));
            PROFILE_PLOT("Mtl/M4Commits", static_cast<int64_t>(m_m4.commits));
            if (@available(macOS 26.0, *)) {
                PROFILE_PLOT("Mtl/M4Allocations",
                             static_cast<int64_t>([(id<MTLResidencySet>)m_m4.residency allocationCount]));
            }
            m_m4.adds = m_m4.removes = m_m4.commits = 0;
        }
        PROFILE_PLOT("Mtl/UploadKB", static_cast<int64_t>(m_counters.uploadBytes / 1024));
        PROFILE_PLOT("Mtl/StagingKB", static_cast<int64_t>(m_counters.stagingBytes / 1024));
        PROFILE_PLOT("Mtl/RingKB", static_cast<int64_t>(m_slots[m_currentFrame].ringUsed / 1024));
        PROFILE_PLOT("Mtl/PresentsPending",
                     static_cast<int64_t>(m_presentsIssued - std::min(m_shared->presentsDone.load(std::memory_order_relaxed),
                                                                      m_presentsIssued)));
        PROFILE_PLOT("Mtl/DrawableWaitUs", static_cast<int64_t>(m_counters.drawableWaitUs));
        PROFILE_PLOT("Mtl/FrameWaitUs", static_cast<int64_t>(m_counters.frameWaitUs));
        PROFILE_PLOT("Mtl/PaceWaitUs", static_cast<int64_t>(m_counters.paceWaitUs));
        PROFILE_PLOT("Mtl/CmdErrors", static_cast<int64_t>(m_cmdErrors.load(std::memory_order_relaxed)));
        PROFILE_PLOT("Mtl/DeviceAllocMB", static_cast<int64_t>(m_memStats.deviceAllocated >> 20));
        m_counters = FrameCounters{};
    }

    void MetalBackend::SampleDeviceMemory() {
        // The driver's figure (every MTLResource of this device, drawables
        // and the ring included) beside the backend's bookkeeping.
        m_memStats.deviceAllocated = m_device.currentAllocatedSize;
        m_memStats.deviceRecommendedMax = m_device.recommendedMaxWorkingSetSize;
    }

    std::string MetalBackend::DebugStateSummary() {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "Metal: device %zu / %zu MB; buffers %zu MB (%zu), textures %zu MB (%zu), ring %zu KB; "
                      "GPU timers %s%s",
                      m_memStats.deviceAllocated >> 20, m_memStats.deviceRecommendedMax >> 20,
                      m_memStats.bufferMemory >> 20, m_memStats.bufferCount, m_memStats.textureMemory >> 20,
                      m_memStats.textureCount, m_slots[m_currentFrame].ringUsed >> 10,
                      m_gpu.active ? "on" : "off", m_gpu.envMode == GpuTiming::Mode::Split ? " (split)" : "");
        return buf;
    }

    // ========================================================================
    // DEBUG LABELS
    // ========================================================================

    void MetalBackend::SetDebugLabel(DebugLabelKind kind, uint32_t handle, const char* label) {
        if (!label) return;
        @autoreleasepool {
            NSString* s = [NSString stringWithUTF8String:label];
            switch (kind) {
                case DebugLabelKind::Buffer: {
                    auto it = m_buffers.find(handle);
                    if (it == m_buffers.end()) return;
                    it->second.label = label;
                    it->second.buffer.label = s;
                    break;
                }
                case DebugLabelKind::Texture: {
                    auto it = m_textures.find(handle);
                    if (it == m_textures.end()) return;
                    TextureInfo& tex = it->second;
                    tex.label = label;
                    if (tex.texture) tex.texture.label = s;
                    for (size_t i = 0; i < tex.frameCopies.size(); ++i) {
                        tex.frameCopies[i].texture.label = [NSString stringWithFormat:@"%@ copy[%zu]", s, i];
                    }
                    break;
                }
                case DebugLabelKind::RenderTarget: {
                    auto it = m_targets.find(handle);
                    if (it == m_targets.end()) return;
                    it->second.label = label;
                    ApplyTargetLabels(it->second);
                    break;
                }
            }
        }
    }

    void MetalBackend::ApplyTargetLabels(TargetInfo& rt) {
        NSString* s = [NSString stringWithUTF8String:rt.label.c_str()];
        if (rt.color) rt.color.label = [NSString stringWithFormat:@"%@ colour", s];
        if (rt.depth) rt.depth.label = [NSString stringWithFormat:@"%@ depth", s];
        auto texIt = m_textures.find(rt.colorTexture);
        if (texIt != m_textures.end()) texIt->second.label = rt.label;
    }

    // ========================================================================
    // GPU FRAME CAPTURE (dev)
    // ========================================================================

    bool MetalBackend::RequestGpuCapture() {
        if (![[MTLCaptureManager sharedCaptureManager] supportsDestination:MTLCaptureDestinationGPUTraceDocument]) {
            return false;
        }
        m_captureRequested = true;
        return true;
    }

    void MetalBackend::BeginGpuCaptureIfDue() {
        if (m_capturing) return;
        bool due = m_captureRequested;
        if (m_captureAt >= 0.0) {
            const double since = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_initTime).count();
            if (since >= m_captureAt) {
                m_captureAt = -1.0;   // one timed capture per run
                due = true;
            }
        }
        if (!due) return;
        m_captureRequested = false;
        MTLCaptureManager* manager = [MTLCaptureManager sharedCaptureManager];
        if (![manager supportsDestination:MTLCaptureDestinationGPUTraceDocument]) {
            Log::Warning("MetalBackend: GPU trace capture needs MTL_CAPTURE_ENABLED=1 in the launch environment "
                         "(open --env MTL_CAPTURE_ENABLED=1 <app> --args ...)");
            return;
        }
        std::string dir = Platform::g_gameDirectory.GetGameDirectory();
        if (dir.empty()) dir = Platform::GameDirectory::GetDefaultGameDirectory();
        dir += "/captures";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        char name[64];
        const std::time_t now = std::time(nullptr);
        std::strftime(name, sizeof(name), "/metal-%H-%M-%S.gputrace", std::localtime(&now));
        const std::string path = dir + name;
        MTLCaptureDescriptor* d = [MTLCaptureDescriptor new];
        // The frame scope: the trace holds exactly this frame's command
        // buffers, as Xcode's own capture button would take.
        d.captureObject = m_captureScope ? static_cast<id>(m_captureScope) : static_cast<id>(m_device);
        d.destination = MTLCaptureDestinationGPUTraceDocument;
        d.outputURL = [NSURL fileURLWithPath:@(path.c_str())];
        NSError* error = nil;
        if (![manager startCaptureWithDescriptor:d error:&error]) {
            Log::Warning("MetalBackend: GPU capture failed to start: %s",
                         error ? error.localizedDescription.UTF8String : "unknown");
            return;
        }
        m_capturing = true;
        Log::Info("MetalBackend: capturing frame %llu to %s", static_cast<unsigned long long>(m_frameNumber + 1),
                  path.c_str());
    }

    void MetalBackend::EndGpuCapture() {
        [[MTLCaptureManager sharedCaptureManager] stopCapture];
        m_capturing = false;
        Log::Info("MetalBackend: GPU capture written");
    }

} // namespace Render

#endif // HAS_METAL

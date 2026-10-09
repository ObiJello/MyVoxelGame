// File: src/client/renderer/backend/metal/MetalFour.mm
//
// MetalBackend's Metal 4 command path (docs/metal4.md): the objects
// (M4Setup), command buffer lifecycle and commit feedback, render and copy
// encoders with their barriers, the argument-table bindings, residency and
// deferred release. Every Metal 4 symbol sits inside @available(macOS 26.0),
// so the binary still loads on 12.7; Metal4Backend turns the path on only
// where the device offers it.
#include "MetalBackend.hpp"
#include "MetalBindings.hpp"
#include "MetalSignposts.hpp"
#include <cstdlib>
#include "imgui.h"
#include "backends/imgui_impl_metal.h"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"
#import <QuartzCore/CAMetalLayer.h>

namespace Render {

    namespace {
        bool IsMemoryless(id object) {
            if ([object respondsToSelector:@selector(storageMode)]) {
                return [(id<MTLResource>)object storageMode] == MTLStorageModeMemoryless;
            }
            return false;
        }
    }

    bool MetalBackend::M4Setup() {
        if (@available(macOS 26.0, *)) {
            if (!m_device || ![m_device supportsFamily:MTLGPUFamilyMetal4]) return false;
            @autoreleasepool {
                NSError* error = nil;
                MTL4CommandQueueDescriptor* qd = [MTL4CommandQueueDescriptor new];
                qd.label = @"ObeyCraft (Metal 4)";
                id<MTL4CommandQueue> queue = [m_device newMTL4CommandQueueWithDescriptor:qd error:&error];
                if (!queue) {
                    Log::Warning("Metal 4: no command queue (%s)", error ? error.localizedDescription.UTF8String : "unknown");
                    return false;
                }
                MTL4CompilerDescriptor* cd = [MTL4CompilerDescriptor new];
                cd.label = @"Metal 4 pipeline compiler";
                id<MTL4Compiler> compiler = [m_device newCompilerWithDescriptor:cd error:&error];
                if (!compiler) {
                    Log::Warning("Metal 4: no compiler (%s)", error ? error.localizedDescription.UTF8String : "unknown");
                    return false;
                }
                MTLResidencySetDescriptor* rd = [MTLResidencySetDescriptor new];
                rd.label = @"ObeyCraft resources";
                rd.initialCapacity = 4096;
                id<MTLResidencySet> residency = [m_device newResidencySetWithDescriptor:rd error:&error];
                if (!residency) {
                    Log::Warning("Metal 4: no residency set (%s)", error ? error.localizedDescription.UTF8String : "unknown");
                    return false;
                }
                [queue addResidencySet:residency];
                MTLResidencySetDescriptor* td0 = [MTLResidencySetDescriptor new];
                td0.label = @"ObeyCraft staging";
                td0.initialCapacity = 256;
                id<MTLResidencySet> transient = [m_device newResidencySetWithDescriptor:td0 error:&error];
                if (!transient) {
                    Log::Warning("Metal 4: no staging residency set (%s)", error ? error.localizedDescription.UTF8String : "unknown");
                    return false;
                }
                [queue addResidencySet:transient];
                m_m4.residencyTransient = transient;
                m_m4.residencyQueue = dispatch_queue_create("obeycraft.metal4.residency", DISPATCH_QUEUE_SERIAL);
                m_m4.residencyGroup = dispatch_group_create();
                // The drawables: CoreAnimation's own set for the layer.
                if (m_layer.residencySet) [queue addResidencySet:m_layer.residencySet];

                MTL4ArgumentTableDescriptor* td = [MTL4ArgumentTableDescriptor new];
                td.maxBufferBindCount = MetalBindings::kVertexStream + 1;   // 0..30: uniforms, the streams
                td.maxTextureBindCount = MetalBindings::kTextureIndices;
                td.maxSamplerStateBindCount = MetalBindings::kTextureIndices;
                td.initializeBindings = YES;
                td.label = @"Vertex arguments";
                id<MTL4ArgumentTable> vertexArgs = [m_device newArgumentTableWithDescriptor:td error:&error];
                td.label = @"Fragment arguments";
                id<MTL4ArgumentTable> fragmentArgs = [m_device newArgumentTableWithDescriptor:td error:&error];
                if (!vertexArgs || !fragmentArgs) {
                    Log::Warning("Metal 4: no argument tables (%s)", error ? error.localizedDescription.UTF8String : "unknown");
                    return false;
                }
                for (int slot = 0; slot < kFramesInFlight; ++slot) {
                    MTL4CommandAllocatorDescriptor* ad = [MTL4CommandAllocatorDescriptor new];
                    ad.label = [NSString stringWithFormat:@"Frame commands [%d]", slot];
                    id<MTL4CommandAllocator> fa = [m_device newCommandAllocatorWithDescriptor:ad error:&error];
                    ad.label = [NSString stringWithFormat:@"Upload commands [%d]", slot];
                    id<MTL4CommandAllocator> ua = [m_device newCommandAllocatorWithDescriptor:ad error:&error];
                    id<MTL4CommandBuffer> fc = [m_device newCommandBuffer];
                    id<MTL4CommandBuffer> uc = [m_device newCommandBuffer];
                    if (!fa || !ua || !fc || !uc) {
                        Log::Warning("Metal 4: no command allocators / buffers (%s)",
                                     error ? error.localizedDescription.UTF8String : "unknown");
                        return false;
                    }
                    fc.label = [NSString stringWithFormat:@"Frame [%d]", slot];
                    uc.label = [NSString stringWithFormat:@"Uploads [%d]", slot];
                    m_m4.frameAllocators[slot] = fa;
                    m_m4.uploadAllocators[slot] = ua;
                    m_m4.frameCmds[slot] = fc;
                    m_m4.uploadCmds[slot] = uc;
                }
                id<MTLSharedEvent> pacing = [m_device newSharedEvent];
                pacing.label = @"Frame pacing";
                MTLSharedEventListener* listener = [[MTLSharedEventListener alloc]
                    initWithDispatchQueue:dispatch_queue_create("obeycraft.metal4.pacing", DISPATCH_QUEUE_SERIAL)];
                id<MTLEvent> overlay = [m_device newEvent];
                overlay.label = @"Overlay after frame";
                if (!pacing || !listener || !overlay) {
                    Log::Warning("Metal 4: no events for pacing / the overlay");
                    return false;
                }
                m_m4.pacingEvent = pacing;
                m_m4.pacingListener = listener;
                m_m4.overlayEvent = overlay;
                m_m4.queue = queue;
                m_m4.compiler = compiler;
                m_m4.residency = residency;
                m_m4.vertexArgs = vertexArgs;
                m_m4.fragmentArgs = fragmentArgs;
                m_shared->uploadsFree = dispatch_semaphore_create(kFramesInFlight);
                // The gate Metal 4 can offer is "previous frame completed"
                // (no scheduled handler): measured −6 % fps for equal lows
                // against the unpaced three-deep queue (plateau series,
                // 2026-10-08), so it is off here unless OBEY_MTL_PACING=1.
                if (const char* pacing = std::getenv("OBEY_MTL_PACING")) m_pacing = std::atoi(pacing) != 0;
                else m_pacing = false;
                m_metal4 = true;

                // Everything the Metal 3 initialisation made before the path
                // was chosen: window images, rings, the shaders' buffers and
                // textures. From here on every creation site registers itself.
                for (auto& [handle, info] : m_buffers) M4Resident(info.buffer);
                for (auto& [handle, info] : m_textures) {
                    if (!info.owned) continue;
                    M4Resident(info.texture);
                    for (const TextureInfo::FrameCopy& c : info.frameCopies) M4Resident(c.texture);
                }
                for (SlotImages& slot : m_slots) {
                    M4Resident(slot.depth);
                    M4Resident(slot.standIn);
                    M4Resident(slot.sceneColor);
                    M4Resident(slot.sceneDepth);
                    M4Resident(slot.ring);
                }
                for (StagingSlot& slot : m_staging) M4Resident(slot.buffer);
                [residency commit];
                [residency requestResidency];
                m_m4.residencyDirty = false;

                // The capture scope was the Metal 3 queue's; the frame runs
                // on the Metal 4 queue now.
                m_captureScope = [[MTLCaptureManager sharedCaptureManager] newCaptureScopeWithDevice:m_device];
                m_captureScope.label = @"Frame (Metal 4)";
                [MTLCaptureManager sharedCaptureManager].defaultCaptureScope = m_captureScope;
            }
            Log::Info("Metal 4 path on: %d frame slots, argument tables, residency set of %lu allocations, "
                      "%s, timestamps by counter heap, pipelines by MTL4Compiler",
                      kFramesInFlight, static_cast<unsigned long>([(id<MTLResidencySet>)m_m4.residency allocationCount]),
                      m_pacing ? "paced by shared event (OBEY_MTL_PACING=1)" : "unpaced (OBEY_MTL_PACING=1 gates)");
            return true;
        }
        return false;
    }

    void MetalBackend::M4Resident(id allocation) {
        if (!m_metal4 || !allocation || IsMemoryless(allocation)) return;
        // A heap-backed texture is resident through its heap.
        if ([allocation respondsToSelector:@selector(heap)] && [(id<MTLResource>)allocation heap] != nil) return;
        if (@available(macOS 26.0, *)) {
            id<MTLResidencySet> set = m_m4.residency;
            ++m_m4.adds;
            const NSUInteger bytes = [allocation respondsToSelector:@selector(allocatedSize)]
                ? [(id<MTLAllocation>)allocation allocatedSize] : 0;
            if (bytes >= Metal4::kBackgroundResidencyBytes && m_m4.residencyQueue) {
                // Large: mapped on the residency queue (see Metal4::residencyQueue).
                id<MTLAllocation> alloc = allocation;
                std::mutex* mutex = &m_m4.residencyMutex;
                dispatch_group_async(m_m4.residencyGroup, m_m4.residencyQueue, ^{
                    PROFILE_ZONE_N("Mtl.M4ResidencyBackground");
                    std::lock_guard<std::mutex> lock(*mutex);
                    [set addAllocation:alloc];
                    [set commit];
                });
                return;
            }
            std::lock_guard<std::mutex> lock(m_m4.residencyMutex);
            [set addAllocation:(id<MTLAllocation>)allocation];
            m_m4.residencyDirty = true;
        }
    }

    void MetalBackend::M4WaitResidency() {
        if (@available(macOS 26.0, *)) {
            if (!m_m4.residencyGroup) return;
            if (dispatch_group_wait(m_m4.residencyGroup, DISPATCH_TIME_NOW) == 0) return;
            PROFILE_ZONE_N("Mtl.M4ResidencyWait");
            dispatch_group_wait(m_m4.residencyGroup, DISPATCH_TIME_FOREVER);
        }
    }

    id MetalBackend::M4HeapTexture(MTLTextureDescriptor* desc) {
        // Private, sampled (not a render target, not memoryless): what the
        // atlases, the entity sheets and their per-frame copies are. A
        // texture over half a heap stays its own allocation.
        if (!m_metal4 || desc.storageMode != MTLStorageModePrivate || (desc.usage & MTLTextureUsageRenderTarget)) return nil;
        if (@available(macOS 26.0, *)) {
            static constexpr NSUInteger kHeapBytes = 64u << 20;
            const MTLSizeAndAlign sa = [m_device heapTextureSizeAndAlignWithDescriptor:desc];
            if (sa.size == 0 || sa.size > kHeapBytes / 2) return nil;
            for (id h : m_m4.textureHeaps) {
                id<MTLHeap> heap = h;
                if ([heap maxAvailableSizeWithAlignment:sa.align] < sa.size) continue;
                if (id<MTLTexture> t = [heap newTextureWithDescriptor:desc]) return t;
            }
            MTLHeapDescriptor* hd = [MTLHeapDescriptor new];
            hd.type = MTLHeapTypeAutomatic;
            hd.storageMode = MTLStorageModePrivate;
            hd.hazardTrackingMode = MTLHazardTrackingModeUntracked;   // Metal 4: no tracking anyway
            hd.size = kHeapBytes;
            id<MTLHeap> heap = [m_device newHeapWithDescriptor:hd];
            if (!heap) return nil;
            heap.label = [NSString stringWithFormat:@"Texture heap %zu", m_m4.textureHeaps.size()];
            m_m4.textureHeaps.push_back(heap);
            M4Resident(heap);
            return [heap newTextureWithDescriptor:desc];
        }
        return nil;
    }

    void MetalBackend::M4ResidentTransient(id allocation) {
        if (!m_metal4 || !allocation) return;
        if (@available(macOS 26.0, *)) {
            id<MTLResidencySet> set = m_m4.residencyTransient;
            std::lock_guard<std::mutex> lock(m_m4.residencyMutex);
            [set addAllocation:(id<MTLAllocation>)allocation];
            m_m4.transientDirty = true;
            ++m_m4.adds;
        }
    }

    void MetalBackend::M4CommitResidency() {
        if (@available(macOS 26.0, *)) {
            std::lock_guard<std::mutex> lock(m_m4.residencyMutex);
            if (m_m4.residencyDirty) {
                PROFILE_ZONE_N("Mtl.M4ResidencyCommit");
                [(id<MTLResidencySet>)m_m4.residency commit];
                m_m4.residencyDirty = false;
                ++m_m4.commits;
            }
            if (m_m4.transientDirty) {
                PROFILE_ZONE_N("Mtl.M4TransientCommit");
                [(id<MTLResidencySet>)m_m4.residencyTransient commit];
                m_m4.transientDirty = false;
                ++m_m4.commits;
            }
        }
    }

    void MetalBackend::M4Release(id object) {
        if (!m_metal4 || !object) return;
        // A frame being recorded may reference it; outside a frame, the
        // next one (uploads queued now run before it).
        m_m4.garbage.push_back({m_frameActive ? m_frameNumber : m_frameNumber + 1, object});
    }

    void MetalBackend::M4CollectGarbage() {
        if (!m_metal4) return;
        if (@available(macOS 26.0, *)) {
            const uint64_t completed = m_shared->completedFrame.load(std::memory_order_acquire);
            id<MTLResidencySet> set = m_m4.residency;
            id<MTLResidencySet> transient = m_m4.residencyTransient;
            std::unique_lock<std::mutex> lock(m_m4.residencyMutex);
            while (!m_m4.garbage.empty() && m_m4.garbage.front().frame <= completed) {
                id object = m_m4.garbage.front().object;
                if (!IsMemoryless(object)) {
                    if ([transient containsAllocation:(id<MTLAllocation>)object]) {
                        [transient removeAllocation:(id<MTLAllocation>)object];
                        m_m4.transientDirty = true;
                        ++m_m4.removes;
                    } else if ([set containsAllocation:(id<MTLAllocation>)object]) {
                        [set removeAllocation:(id<MTLAllocation>)object];
                        m_m4.residencyDirty = true;
                        ++m_m4.removes;
                    }
                }
                m_m4.garbage.pop_front();
            }
            lock.unlock();
            M4CommitResidency();
        }
    }

    void MetalBackend::M4BeginFrameCommandBuffer() {
        if (@available(macOS 26.0, *)) {
            id<MTL4CommandAllocator> allocator = m_m4.frameAllocators[m_currentFrame];
            id<MTL4CommandBuffer> cmd = m_m4.frameCmds[m_currentFrame];
            [allocator reset];
            [cmd beginCommandBufferWithAllocator:allocator];
            m_m4.encFirst = UINT32_MAX;   // no timestamp of the last frame's carries over
            m_m4.copyEncoder = nil;
            m_m4.copyEnd = UINT32_MAX;
            m_cmd4 = cmd;
        }
    }

    void MetalBackend::M4CommitFrame() {
        if (@available(macOS 26.0, *)) {
            @autoreleasepool {
                id<MTL4CommandBuffer> cmd = m_cmd4;
                id<MTL4CommandQueue> queue = m_m4.queue;
                [cmd endCommandBuffer];
                M4CommitResidency();
                M4WaitResidency();
                const bool present = m_presenting && m_drawable != nil;
                if (present) [queue waitForDrawable:m_drawable];

                MTL4CommitOptions* options = [MTL4CommitOptions new];
                std::shared_ptr<SharedState> shared = m_shared;
                const uint64_t frame = m_frameNumber;
                const uint32_t slot = m_currentFrame;
                const bool signposts = m_signpostsOn;
                MetalBackend* self = this;   // outlives every frame: M4Shutdown drains them
                [options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
                    if (signposts) os_signpost_interval_end(MtlSignpost::FrameLog(), MtlSignpost::FrameId(frame), "GPU");
                    if (feedback.error) {
                        Log::Error("Metal 4: frame %llu failed: %s", static_cast<unsigned long long>(frame),
                                   feedback.error.localizedDescription.UTF8String);
                    }
                    self->m_m4.gpuStart[slot] = feedback.GPUStartTime;
                    self->m_m4.gpuEnd[slot] = feedback.GPUEndTime;
                    uint64_t d = shared->completedFrame.load(std::memory_order_relaxed);
                    while (d < frame && !shared->completedFrame.compare_exchange_weak(d, frame)) {}
                    dispatch_semaphore_signal(shared->framesInFlight);
                }];
                if (signposts) os_signpost_interval_begin(MtlSignpost::FrameLog(), MtlSignpost::FrameId(frame), "GPU");
                if (m_pacing) {
                    // Signalled once everything committed before this frame
                    // completed: the gate for the frame after this one.
                    id<MTLSharedEvent> pacing = m_m4.pacingEvent;
                    [pacing notifyListener:(MTLSharedEventListener*)m_m4.pacingListener atValue:frame
                                     block:^(id<MTLSharedEvent>, uint64_t) { dispatch_semaphore_signal(shared->frameStarted); }];
                    [queue signalEvent:pacing value:frame];
                }
                m_gpu.slots[slot].commitHostTime = CACurrentMediaTime();
                id<MTL4CommandBuffer> cmds[1] = {cmd};
                [queue commit:cmds count:1 options:options];
                if (present) {
                    [queue signalDrawable:m_drawable];
                    if (m_m4.overlayData) {
                        [queue signalEvent:(id<MTLEvent>)m_m4.overlayEvent value:frame];
                        M4PresentWithOverlay();
                    } else {
                        [m_drawable present];
                    }
                }
                m_m4.overlayData = nullptr;
                m_cmd4 = nil;
            }
        }
    }

    id MetalBackend::M4UploadEncoder() {
        if (@available(macOS 26.0, *)) {
            if (!m_uploadCmd4) {
                // An upload command buffer per use, round-robin over the
                // slots' allocators; the allocator is reset only once the
                // GPU finished its last command buffer (uploadsFree, signalled
                // by that commit's feedback).
                const long waited = dispatch_semaphore_wait(m_shared->uploadsFree, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC));
                if (waited != 0) Log::Warning("Metal 4: upload command buffers never came back (1 s)");
                const uint32_t slot = static_cast<uint32_t>(m_m4.uploadsInFlight++ % kFramesInFlight);
                id<MTL4CommandAllocator> allocator = m_m4.uploadAllocators[slot];
                id<MTL4CommandBuffer> cmd = m_m4.uploadCmds[slot];
                [allocator reset];
                [cmd beginCommandBufferWithAllocator:allocator];
                m_uploadCmd4 = cmd;
            }
            if (!m_uploadBlit) {
                // New buffers and textures (nothing has read them): after
                // prior copies only, never the previous frame's fragment
                // work — that wait would cost the frames' overlap.
                m_uploadBlit = M4MakeCopyEncoder(m_uploadCmd4, @"Uploads", /*afterFragment=*/false);
            }
            return m_uploadBlit;
        }
        return nil;
    }

    void MetalBackend::M4EndUploadEncoder() {
        if (@available(macOS 26.0, *)) {
            if (m_uploadBlit) {
                id<MTL4ComputeCommandEncoder> enc = m_uploadBlit;
                [enc barrierAfterStages:MTLStageBlit
                      beforeQueueStages:(MTLStageVertex | MTLStageFragment | MTLStageBlit | MTLStageDispatch)
                      visibilityOptions:MTL4VisibilityOptionDevice];
                [enc endEncoding];
                m_uploadBlit = nil;
            }
        }
    }

    void MetalBackend::M4FlushUploads() {
        if (@available(macOS 26.0, *)) {
            M4EndUploadEncoder();
            if (m_uploadCmd4) {
                @autoreleasepool {
                    id<MTL4CommandBuffer> cmd = m_uploadCmd4;
                    [cmd endCommandBuffer];
                    M4CommitResidency();
                    M4WaitResidency();
                    MTL4CommitOptions* options = [MTL4CommitOptions new];
                    std::shared_ptr<SharedState> shared = m_shared;
                    [options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
                        if (feedback.error) {
                            Log::Error("Metal 4: an upload command buffer failed: %s",
                                       feedback.error.localizedDescription.UTF8String);
                        }
                        dispatch_semaphore_signal(shared->uploadsFree);
                    }];
                    id<MTL4CommandBuffer> cmds[1] = {cmd};
                    [(id<MTL4CommandQueue>)m_m4.queue commit:cmds count:1 options:options];
                    m_uploadCmd4 = nil;
                }
            }
        }
    }

    id MetalBackend::M4MakeRenderEncoder(id cmdArg, MTLRenderPassDescriptor* desc, uint32_t width, uint32_t height, bool resumed,
                                         uint32_t sampleFirst) {
        if (@available(macOS 26.0, *)) {
            @autoreleasepool {
                m_m4.encFirst = UINT32_MAX;
                MTL4RenderPassDescriptor* d = [MTL4RenderPassDescriptor new];
                for (NSUInteger i = 0; i < 8; ++i) {
                    MTLRenderPassColorAttachmentDescriptor* c = desc.colorAttachments[i];
                    if (!c.texture) break;
                    MTLRenderPassColorAttachmentDescriptor* o = d.colorAttachments[i];
                    o.texture = c.texture;
                    o.loadAction = c.loadAction;
                    o.storeAction = c.storeAction;
                    o.clearColor = c.clearColor;
                }
                if (desc.depthAttachment.texture) {
                    d.depthAttachment.texture = desc.depthAttachment.texture;
                    d.depthAttachment.loadAction = desc.depthAttachment.loadAction;
                    d.depthAttachment.storeAction = desc.depthAttachment.storeAction;
                    d.depthAttachment.clearDepth = desc.depthAttachment.clearDepth;
                }
                if (desc.stencilAttachment.texture) {
                    d.stencilAttachment.texture = desc.stencilAttachment.texture;
                    d.stencilAttachment.loadAction = desc.stencilAttachment.loadAction;
                    d.stencilAttachment.storeAction = desc.stencilAttachment.storeAction;
                    d.stencilAttachment.clearStencil = desc.stencilAttachment.clearStencil;
                }
                d.renderTargetWidth = width;
                d.renderTargetHeight = height;
                id<MTL4CommandBuffer> cmd = cmdArg;
                id<MTL4RenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:d];
                if (!enc) return nil;
                // No hazard tracking in Metal 4: the pass reads what copies
                // (uploads, the texture flush, in-frame copies) wrote, and a
                // resumed pass loads what the passes before it stored. The
                // first pass of a frame does not wait for the previous
                // frame's fragment work — that overlap is the TBDR's pipeline.
                MTLStages after = MTLStageBlit | MTLStageDispatch;
                if (resumed) after |= MTLStageFragment;
                [enc barrierAfterQueueStages:after
                                beforeStages:(MTLStageVertex | MTLStageFragment)
                           visibilityOptions:MTL4VisibilityOptionDevice];
                [enc setArgumentTable:(id<MTL4ArgumentTable>)m_m4.vertexArgs atStages:MTLRenderStageVertex];
                [enc setArgumentTable:(id<MTL4ArgumentTable>)m_m4.fragmentArgs atStages:MTLRenderStageFragment];
                if (sampleFirst != UINT32_MAX && cmdArg == m_cmd4 && m_gpu.active) {
                    // The stage starts, in the sample layout's slots +0 and
                    // +2: a Precise stage timestamp written before any draw
                    // "is written before the stage begins" (MTL4RenderCommandEncoder.h)
                    // — the heap's own clock, like the stage ends
                    // M4WriteEncoderEnd writes. A command-buffer timestamp
                    // here marked the command processor reaching the encoder,
                    // up to frames ahead of its execution, and the spans
                    // mixed the two (2026-10-08).
                    id<MTL4CounterHeap> heap = m_gpu.slots[m_currentFrame].heap;
                    [enc writeTimestampWithGranularity:MTL4TimestampGranularityPrecise afterStage:MTLRenderStageVertex
                                              intoHeap:heap atIndex:sampleFirst];
                    [enc writeTimestampWithGranularity:MTL4TimestampGranularityPrecise afterStage:MTLRenderStageFragment
                                              intoHeap:heap atIndex:sampleFirst + 2];
                    m_m4.encFirst = sampleFirst;
                }
                return enc;
            }
        }
        return nil;
    }

    id MetalBackend::M4MakeCopyEncoder(id cmd, NSString* label, bool afterFragment) {
        if (@available(macOS 26.0, *)) {
            id<MTL4CommandBuffer> c = cmd;
            id<MTL4ComputeCommandEncoder> enc = [c computeCommandEncoder];
            if (!enc) return nil;
            enc.label = label;
            MTLStages after = MTLStageBlit | MTLStageDispatch;
            if (afterFragment) after |= MTLStageVertex | MTLStageFragment;
            [enc barrierAfterQueueStages:after beforeStages:MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
            if (cmd == m_cmd4 && m_gpu.active && m_frameActive) {
                // Timed like a blit: a Precise timestamp inside the encoder
                // before its first copy (after the barrier: the copy's
                // start), one before it ends (EndBlitEncoder). Command-buffer
                // timestamps around the encoder marked the command processor,
                // ~0.1 ms apart whatever the copies cost (2026-10-08).
                GpuTiming::Slot& slot = m_gpu.slots[m_currentFrame];
                if (slot.used + 2 <= GpuTiming::kSamplesPerSlot) {
                    const char* name = InternLabel(label);
                    const uint32_t first = slot.used;
                    slot.encoders.push_back({first, 4, true, false, name,
                                             m_debugGroups.empty() ? nullptr : m_debugGroups.back()});
                    slot.used += 2;
                    [enc writeTimestampWithGranularity:MTL4TimestampGranularityPrecise
                                              intoHeap:(id<MTL4CounterHeap>)slot.heap atIndex:first];
                    m_m4.copyEncoder = enc;
                    m_m4.copyEnd = first + 1;
                } else {
                    ++m_gpu.dropped;
                }
            }
            return enc;
        }
        return nil;
    }

    void MetalBackend::M4WriteEncoderEnd() {
        if (@available(macOS 26.0, *)) {
            if (m_m4.encFirst == UINT32_MAX || !m_encoder) return;
            // Before the encoder ends: the vertex stage's end at +1, the
            // fragment stage's end at +3 — Precise, written when the stage
            // completes for everything encoded so far, i.e. the whole
            // encoder (the starts, +0 and +2, were written at its opening).
            // Relaxed, and command-buffer boundary timestamps, read ~0.1 ms
            // for a 6 ms encoder: they mark the command processor, not the
            // execution (2026-10-08).
            id<MTL4RenderCommandEncoder> enc = m_encoder;
            id<MTL4CounterHeap> heap = m_gpu.slots[m_currentFrame].heap;
            const uint32_t first = m_m4.encFirst;
            [enc writeTimestampWithGranularity:MTL4TimestampGranularityPrecise afterStage:MTLRenderStageVertex
                                      intoHeap:heap atIndex:first + 1];
            [enc writeTimestampWithGranularity:MTL4TimestampGranularityPrecise afterStage:MTLRenderStageFragment
                                      intoHeap:heap atIndex:first + 3];
            m_m4.encFirst = UINT32_MAX;
        }
    }

    void MetalBackend::M4WriteCopyEnd(id encoder) {
        if (@available(macOS 26.0, *)) {
            if (m_m4.copyEnd == UINT32_MAX || encoder != m_m4.copyEncoder) return;
            id<MTL4ComputeCommandEncoder> enc = encoder;
            [enc writeTimestampWithGranularity:MTL4TimestampGranularityPrecise
                                      intoHeap:(id<MTL4CounterHeap>)m_gpu.slots[m_currentFrame].heap atIndex:m_m4.copyEnd];
            m_m4.copyEnd = UINT32_MAX;
            m_m4.copyEncoder = nil;
        }
    }

    void MetalBackend::M4PresentWithOverlay() {
        // The frame's drawable, finished by the Metal 4 queue (the event),
        // takes the overlay from a Metal 3 command buffer, which presents.
        @autoreleasepool {
            id<MTLCommandBuffer> cb = [m_queue commandBuffer];
            cb.label = @"Overlay";
            [cb encodeWaitForEvent:(id<MTLEvent>)m_m4.overlayEvent value:m_frameNumber];
            MTLRenderPassDescriptor* d = [MTLRenderPassDescriptor renderPassDescriptor];
            d.colorAttachments[0].texture = m_drawable.texture;
            d.colorAttachments[0].loadAction = MTLLoadActionLoad;
            d.colorAttachments[0].storeAction = MTLStoreActionStore;
            // The imgui pipeline carries the frame's depth/stencil format;
            // the slot's image serves, never loaded nor kept.
            id<MTLTexture> depth = m_slots[m_currentFrame].depth;
            d.depthAttachment.texture = depth;
            d.depthAttachment.loadAction = MTLLoadActionDontCare;
            d.depthAttachment.storeAction = MTLStoreActionDontCare;
            d.stencilAttachment.texture = depth;
            d.stencilAttachment.loadAction = MTLLoadActionDontCare;
            d.stencilAttachment.storeAction = MTLStoreActionDontCare;
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:d];
            enc.label = @"ImGui overlay";
            ImGui_ImplMetal_RenderDrawData(static_cast<ImDrawData*>(m_m4.overlayData), cb, enc);
            [enc endEncoding];
            [cb presentDrawable:m_drawable];
            [cb commit];
        }
    }

    void MetalBackend::M4BindBuffer(bool fragment, uint32_t index, id<MTLBuffer> buffer, NSUInteger offset) {
        if (@available(macOS 26.0, *)) {
            id<MTL4ArgumentTable> table = fragment ? m_m4.fragmentArgs : m_m4.vertexArgs;
            [table setAddress:(buffer ? buffer.gpuAddress + offset : 0) atIndex:index];
        }
    }

    void MetalBackend::M4BindBytes(bool fragment, uint32_t index, const void* data, size_t bytes) {
        size_t offset = 0;
        id<MTLBuffer> ring = RingAllocate(bytes, offset, data);
        M4BindBuffer(fragment, index, ring, offset);
    }

    void MetalBackend::M4BindTexture(bool fragment, uint32_t index, id<MTLTexture> texture) {
        if (@available(macOS 26.0, *)) {
            id<MTL4ArgumentTable> table = fragment ? m_m4.fragmentArgs : m_m4.vertexArgs;
            MTLResourceID id = texture ? texture.gpuResourceID : MTLResourceID{0};
            [table setTexture:id atIndex:index];
        }
    }

    void MetalBackend::M4BindSampler(bool fragment, uint32_t index, id<MTLSamplerState> sampler) {
        if (@available(macOS 26.0, *)) {
            if (!sampler) return;
            id<MTL4ArgumentTable> table = fragment ? m_m4.fragmentArgs : m_m4.vertexArgs;
            [table setSamplerState:sampler.gpuResourceID atIndex:index];
        }
    }

    void MetalBackend::M4DrawIndexed(MTLPrimitiveType primitive, uint32_t indexCount, MTLIndexType type,
                                     id<MTLBuffer> indices, size_t byteOffset, uint32_t instances, int32_t baseVertex) {
        if (@available(macOS 26.0, *)) {
            id<MTL4RenderCommandEncoder> enc = m_encoder;
            [enc drawIndexedPrimitives:primitive
                            indexCount:indexCount
                             indexType:type
                           indexBuffer:indices.gpuAddress + byteOffset
                     indexBufferLength:indices.length - byteOffset
                         instanceCount:instances
                            baseVertex:baseVertex
                          baseInstance:0];
        }
    }

    void MetalBackend::M4Shutdown() {
        if (!m_metal4) return;
        if (@available(macOS 26.0, *)) {
            // Every frame and upload in flight completes (their feedback
            // gives the semaphores back) before anything is released. The
            // counts are put back afterwards: libdispatch aborts when a
            // semaphore is released below the count it was created with.
            for (int i = 0; i < kFramesInFlight; ++i) {
                dispatch_semaphore_wait(m_shared->framesInFlight, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
            }
            for (int i = 0; i < kFramesInFlight; ++i) {
                dispatch_semaphore_wait(m_shared->uploadsFree, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
            }
            for (int i = 0; i < kFramesInFlight; ++i) {
                dispatch_semaphore_signal(m_shared->framesInFlight);
                dispatch_semaphore_signal(m_shared->uploadsFree);
            }
            @autoreleasepool {
                id<MTL4CommandQueue> queue = m_m4.queue;
                if (m_m4.residency) [queue removeResidencySet:(id<MTLResidencySet>)m_m4.residency];
                if (m_m4.residencyTransient) [queue removeResidencySet:(id<MTLResidencySet>)m_m4.residencyTransient];
                if (m_layer.residencySet) [queue removeResidencySet:m_layer.residencySet];
            }
            M4WaitResidency();
            m_m4.garbage.clear();
            m_m4.textureHeaps.clear();
            m_m4.residencyQueue = nullptr;
            m_m4.residencyGroup = nullptr;
            for (int slot = 0; slot < kFramesInFlight; ++slot) {
                m_m4.frameAllocators[slot] = nil;
                m_m4.uploadAllocators[slot] = nil;
                m_m4.frameCmds[slot] = nil;
                m_m4.uploadCmds[slot] = nil;
            }
            m_m4.vertexArgs = m_m4.fragmentArgs = nil;
            m_m4.pacingEvent = m_m4.pacingListener = m_m4.overlayEvent = nil;
            m_m4.overlayData = nullptr;
            for (GpuTiming::Slot& slot : m_gpu.slots) slot.heap = nil;
            m_m4.residency = nil;
            m_m4.compiler = nil;
            m_m4.queue = nil;
            m_cmd4 = nil;
            m_uploadCmd4 = nil;
            m_metal4 = false;
        }
    }

} // namespace Render

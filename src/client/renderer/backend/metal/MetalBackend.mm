// File: src/client/renderer/backend/metal/MetalBackend.mm
//
// The Metal backend's device, frame, presentation and pass machinery.
// Resources live in MetalResources.mm, shaders / pipelines / draws in
// MetalPipelines.mm. See MetalBackend.hpp for the design.
#ifdef HAS_METAL

#import "MetalBackend.hpp"
#import "MetalBindings.hpp"
#include "MetalSignposts.hpp"

#import <AppKit/AppKit.h>
#import <QuartzCore/CABase.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

#include "client/renderer/core/DevRenderSkip.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/core/TickParallel.hpp"
#include "platform/GameDirectory.hpp"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_metal.h"

#include <algorithm>
#include <ctime>
#include <cstdlib>
#include <thread>
#include <cstring>
#include <filesystem>

namespace Render {

    std::unique_ptr<RenderBackend> CreateMetalBackend() {
        return std::make_unique<MetalBackend>();
    }

    MetalBackend::MetalBackend() = default;

    MetalBackend::~MetalBackend() {
        Shutdown();
    }

    // ========================================================================
    // LIFECYCLE
    // ========================================================================

    bool MetalBackend::Initialize(GLFWwindow* window) {
        @autoreleasepool {
            Log::Info("MetalBackend: Initializing Metal backend");
            m_window = window;
            m_initTime = std::chrono::steady_clock::now();
            // Metal reads MTL_CAPTURE_ENABLED when the framework loads, so it
            // must be in the LAUNCH environment (`open --env
            // MTL_CAPTURE_ENABLED=1 ...`), not set from here or via --env.
            if (const char* at = std::getenv("OBEY_MTL_CAPTURE_AT")) m_captureAt = std::atof(at);
            // Instrumentation switches (MetalInstrumentation.mm): per-encoder
            // GPU timestamps (off by default so the Metal HUD's encoder
            // timing works), per-encoder error status, the HUD itself.
            if (const char* timers = std::getenv("OBEY_MTL_GPU_TIMERS")) {
                m_gpu.envMode = std::strcmp(timers, "split") == 0 ? GpuTiming::Mode::Split
                              : std::strcmp(timers, "0") == 0 ? GpuTiming::Mode::Off : GpuTiming::Mode::Encoders;
            }
            if (const char* errors = std::getenv("OBEY_MTL_ERRORS")) m_errorOptions = std::strcmp(errors, "0") != 0;
            m_device = MTLCreateSystemDefaultDevice();
            if (!m_device) {
                Log::Error("MetalBackend: no Metal device");
                return false;
            }
            m_queue = [m_device newCommandQueue];
            if (!m_queue) {
                Log::Error("MetalBackend: could not create a command queue");
                return false;
            }
            m_queue.label = @"ObeyCraft";
            // The frame as a capture scope: Xcode's capture button and
            // RequestGpuCapture take exactly one frame.
            m_captureScope = [[MTLCaptureManager sharedCaptureManager] newCaptureScopeWithCommandQueue:m_queue];
            m_captureScope.label = @"Frame";
            [MTLCaptureManager sharedCaptureManager].defaultCaptureScope = m_captureScope;
            m_appleGpu = [m_device supportsFamily:MTLGPUFamilyApple1];
            m_managedBuffers = !m_device.hasUnifiedMemory;
            m_hostVisibleOptions = m_managedBuffers ? MTLResourceStorageModeManaged : MTLResourceStorageModeShared;

            if (const char* mailbox = std::getenv("OBEY_MTL_MAILBOX")) m_mailboxWanted = std::strcmp(mailbox, "0") != 0;
            if (const char* tearing = std::getenv("OBEY_MTL_TEARING")) m_tearing = std::strcmp(tearing, "0") != 0;
            if (const char* pending = std::getenv("OBEY_MTL_MAILBOX_PENDING")) {
                m_mailboxPending = std::clamp(std::atoi(pending), 1, 2);
            }

            m_shared = std::make_shared<SharedState>();
            m_shared->framesInFlight = dispatch_semaphore_create(kFramesInFlight);
            // The pacing gate starts open once: frame 1 records with nothing
            // ahead of it, every later frame consumes the signal its
            // predecessor's scheduled handler gives.
            m_shared->frameStarted = dispatch_semaphore_create(1);
            m_memorylessDepthOk = [m_device supportsFamily:MTLGPUFamilyApple2] &&
                                  std::getenv("OBEY_MTL_DEPTH_PRIVATE") == nullptr;
            if (const char* pacing = std::getenv("OBEY_MTL_PACING")) m_pacing = std::atoi(pacing) != 0;

            // The window's layer: a CAMetalLayer on GLFW's content view
            // (the window was created with GLFW_NO_API).
            NSWindow* nsWindow = glfwGetCocoaWindow(window);
            if (!nsWindow) {
                Log::Error("MetalBackend: no Cocoa window");
                return false;
            }
            m_layer = [CAMetalLayer layer];
            m_layer.device = m_device;
            m_layer.pixelFormat = kColorFormat;
            // Read-backs, the post chains' copy and the scaled scene's
            // resolve read or write the drawable outside a render pass.
            m_layer.framebufferOnly = NO;
            // Untagged: the colours go to the display as they are, the look
            // of Vulkan's PASS_THROUGH colour space (and of OpenGL). A tagged
            // sRGB layer is colour-managed and desaturated on P3 displays.
            m_layer.colorspace = nil;
            m_layer.maximumDrawableCount = 3;
            m_layer.displaySyncEnabled = YES;
            m_layer.allowsNextDrawableTimeout = YES;
            // OBEY_MTL_HUD=1: Apple's Metal Performance HUD on this layer, the
            // in-process form of MTL_HUD_ENABLED=1 (the dictionary takes only
            // Apple's documented keys; the per-encoder timing and the log
            // need the MTL_HUD_* launch environment — tools/play.sh --hud).
            if (const char* hud = std::getenv("OBEY_MTL_HUD"); hud && std::strcmp(hud, "0") != 0) {
                if (@available(macOS 13.0, *)) {
                    m_layer.developerHUDProperties = @{@"mode": @"default", @"logging": @"default"};
                }
            }
            nsWindow.contentView.layer = m_layer;
            nsWindow.contentView.wantsLayer = YES;

            // The mailbox's spacing: half a refresh (see MailboxDrawableFree).
            NSInteger refresh = 60;
            if (NSScreen* screen = nsWindow.screen ?: NSScreen.mainScreen) {
                if (@available(macOS 12.0, *)) refresh = std::max<NSInteger>(1, screen.maximumFramesPerSecond);
            }
            m_presentInterval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(1.0 / (2.0 * static_cast<double>(refresh))));

            SyncDrawableSize();
            if (m_drawableWidth == 0 || m_drawableHeight == 0) {
                Log::Error("MetalBackend: the window has no framebuffer");
                return false;
            }
            if (!CreateInternalPipelines()) return false;
            {
                NSError* error = nil;
                NSURL* url = [NSURL fileURLWithPath:@"shaders/metal/shaders.metallib"];
                if ([[NSFileManager defaultManager] fileExistsAtPath:url.path]) {
                    m_shaderLibrary = [m_device newLibraryWithURL:url error:&error];
                }
                if (m_shaderLibrary) {
                    Log::Info("MetalBackend: precompiled shaders.metallib (%lu functions)",
                              static_cast<unsigned long>(m_shaderLibrary.functionNames.count));
                } else {
                    Log::Info("MetalBackend: no shaders.metallib - compiling the .metal shaders at startup%s%s",
                              error ? ": " : "", error ? error.localizedDescription.UTF8String : "");
                }
            }

            // The upscale draw of Render Resolution (scene_upscale_vk.*).
            m_upscaleShader = CreateShaderFromFiles("shaders/scene_upscale.vert", "shaders/scene_upscale.frag");
            if (m_upscaleShader == INVALID_SHADER) {
                Log::Warning("MetalBackend: scene_upscale shaders missing - Render Resolution stays at 100%%");
            }

            const GpuDeviceInfo info = GetDeviceInfo();
            Log::Info("MetalBackend: %s (%s GPU, %s memory, %u frames in flight, mailbox %s, vsync off %s)",
                      info.name.c_str(), m_appleGpu ? "Apple-family" : "Mac-family",
                      m_managedBuffers ? "discrete" : "unified", kFramesInFlight,
                      m_mailboxWanted ? "on" : "off", m_tearing ? "tears (OBEY_MTL_TEARING)" : "tear-free");
            return true;
        }
    }

    void MetalBackend::Shutdown() {
        if (!m_device) return;
        @autoreleasepool {
            // Everything in flight finishes before the objects go.
            FlushUploads();
            M4Shutdown();
            for (id<MTLCommandBuffer> cmd : m_slotCommandBuffers) {
                if (cmd && cmd.status >= MTLCommandBufferStatusCommitted) [cmd waitUntilCompleted];
            }
            SaveManifest(/*synchronous=*/true);
            if (m_manifestWriter.joinable()) m_manifestWriter.join();
            for (GpuTiming::Slot& slot : m_gpu.slots) slot.buffer = nil;
            m_captureScope = nil;
            m_encoder = nil;
            m_cmd = nil;
            m_drawable = nil;
            m_slotCommandBuffers = {};
            m_readbacks.clear();
            m_readbackFree.clear();
            m_gpuTimers.clear();
            m_pipelines.clear();
            m_depthStencilStates.clear();
            m_clearPipelines.clear();
            m_samplers.clear();
            m_shaders.clear();
            m_libraries.clear();
            m_shaderLibrary = nil;
            m_meshes.clear();
            m_targets.clear();
            m_textures.clear();
            m_buffers.clear();
            m_slots = {};
            m_oitSlots = {};
            m_staging = {};
            m_internalLibrary = nil;
            m_layer = nil;
            m_queue = nil;
            m_device = nil;
            m_shared.reset();
        }
    }

    GpuDeviceInfo MetalBackend::GetDeviceInfo() const {
        GpuDeviceInfo info;
        // F3 says which path draws (the user's request, 2026-10-08).
        info.backendName = m_metal4 ? "Metal 4" : "Metal 3";
        if (!m_device) return info;
        info.name = m_device.name.UTF8String;
        const std::string& n = info.name;
        if (m_appleGpu || n.find("Apple") != std::string::npos) info.vendorName = "Apple";
        else if (n.find("AMD") != std::string::npos || n.find("Radeon") != std::string::npos) info.vendorName = "AMD";
        else if (n.find("Intel") != std::string::npos) info.vendorName = "Intel";
        else if (n.find("NVIDIA") != std::string::npos || n.find("GeForce") != std::string::npos) info.vendorName = "NVIDIA";
        else info.vendorName = "Unknown";
        info.type = m_device.hasUnifiedMemory ? GpuDeviceInfo::Type::Integrated : GpuDeviceInfo::Type::Discrete;
        const NSOperatingSystemVersion os = NSProcessInfo.processInfo.operatingSystemVersion;
        char driver[96];
        std::snprintf(driver, sizeof(driver), "macOS %ld.%ld.%ld, MSL 2.4%s",
                      static_cast<long>(os.majorVersion), static_cast<long>(os.minorVersion),
                      static_cast<long>(os.patchVersion), m_appleGpu ? ", Apple GPU family" : "");
        info.driverInfo = driver;
        return info;
    }

    void MetalBackend::SetVSync(bool enabled) {
        m_vsync = enabled;
        ApplyDisplaySync();
    }

    void MetalBackend::ApplyDisplaySync() {
        // Display sync stays on with vsync off (tear-free, see m_tearing):
        // the mailbox keeps the loop from ever waiting on a drawable, so
        // the only thing the sync changes is WHEN a presented frame flips.
        // OBEY_SKIP=tearing: the skip phases tear (the phase A/B).
        if (!m_layer) return;
        const BOOL sync = (m_vsync || !(m_tearing || DevSkip("tearing"))) ? YES : NO;
        if (m_layer.displaySyncEnabled != sync) m_layer.displaySyncEnabled = sync;
    }

    void MetalBackend::SyncDrawableSize() {
        int fbW = 0, fbH = 0;
        glfwGetFramebufferSize(m_window, &fbW, &fbH);
        if (fbW <= 0 || fbH <= 0) return;   // minimised: keep what there is
        if (static_cast<uint32_t>(fbW) == m_drawableWidth && static_cast<uint32_t>(fbH) == m_drawableHeight) return;
        int winW = 0, winH = 0;
        glfwGetWindowSize(m_window, &winW, &winH);
        // Retina or not (Video Settings → Retina Resolution): the layer's
        // pixels per point is whatever the framebuffer came out as.
        m_layer.contentsScale = winW > 0 ? static_cast<CGFloat>(fbW) / static_cast<CGFloat>(winW) : 1.0;
        m_layer.drawableSize = CGSizeMake(fbW, fbH);
        m_drawableWidth = static_cast<uint32_t>(fbW);
        m_drawableHeight = static_cast<uint32_t>(fbH);
        if (!CreateWindowImages()) Log::Error("MetalBackend: window-sized images could not be created");
        // Improved Transparency's targets follow the frame's size.
        if (m_oitCreated) OitDestroyTargets();
        Log::Info("MetalBackend: drawable %ux%u (scale %.2f)", m_drawableWidth, m_drawableHeight,
                  static_cast<double>(m_layer.contentsScale));
    }

    bool MetalBackend::CreateWindowImages() {
        @autoreleasepool {
            // Replacing an image a frame in flight still uses is safe: the
            // command buffer holds its own reference.
            MTLTextureDescriptor* color = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kColorFormat
                                                                                              width:m_drawableWidth
                                                                                             height:m_drawableHeight
                                                                                          mipmapped:NO];
            color.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            color.storageMode = MTLStorageModePrivate;
            for (SlotImages& slot : m_slots) {
                // Private to begin with; SettleDepthStorage makes it
                // memoryless at the slot's first frame when nothing preserves it.
                M4Release(slot.depth);     // a resize: the old images go once their frames are done
                M4Release(slot.standIn);
                slot.depth = MakeDepthTexture(m_drawableWidth, m_drawableHeight, /*memoryless=*/false, @"Frame depth");
                slot.depthMemoryless = false;
                slot.standIn = [m_device newTextureWithDescriptor:color];
                if (!slot.depth || !slot.standIn) return false;
                slot.standIn.label = @"Mailbox stand-in";
                M4Resident(slot.standIn);
            }
            ++m_frameDepthGeneration;   // every handed-off depth is stale now
            return true;
        }
    }

    // ========================================================================
    // FRAME
    // ========================================================================

    void MetalBackend::BeginFrame() {
        ApplyDisplaySync();
        PROFILE_ZONE_N("Mtl.BeginFrame");
        m_frameActive = false;
        m_packFrame = false;
        m_depthHandoff = false;   // asked for again by the frame that wants it
        @autoreleasepool {
            // The frame slot's previous use must be done on the GPU: with two
            // slots, frame N waits for frame N-2. A wide Mtl.FrameWait means
            // GPU-bound, not that anything here is slow.
            m_signpostsOn = MtlSignpost::Enabled();
            {
                PROFILE_ZONE_N("Mtl.FrameWait");
                const auto t0 = std::chrono::steady_clock::now();
                if (m_signpostsOn) {
                    os_signpost_interval_begin(MtlSignpost::FrameLog(), MtlSignpost::FrameId(m_frameNumber + 1), "FrameWait");
                }
                const long waited = dispatch_semaphore_wait(m_shared->framesInFlight,
                                                            dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC));
                if (m_signpostsOn) {
                    os_signpost_interval_end(MtlSignpost::FrameLog(), MtlSignpost::FrameId(m_frameNumber + 1), "FrameWait");
                }
                m_counters.frameWaitUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
                if (waited != 0) {
                    Log::Error("MetalBackend: frame wait timed out (1s) - possible GPU hang");
                    return;
                }
            }
            // Frame pacing (kFramesInFlight): the previous frame has begun
            // executing, so this one will be the only frame queued behind it.
            m_counters.paceWaitUs = 0.0;
            if (m_pacing) {   // Metal 4: the gate is the queue's pacing event (M4CommitFrame)
                PROFILE_ZONE_N("Mtl.PaceWait");
                const auto t0 = std::chrono::steady_clock::now();
                // OBEY_SKIP=pacing: the skip phases take the signal if it is
                // there and never wait (the gate's count stays at most 1).
                const bool gateOff = DevSkip("pacing");
                const long waited = dispatch_semaphore_wait(m_shared->frameStarted,
                                                            gateOff ? DISPATCH_TIME_NOW
                                                                    : dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC));
                m_counters.paceWaitUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
                if (waited != 0 && !gateOff) {
                    // Pacing is only an ordering hint: a frame that never
                    // reached the GPU (a dropped command buffer) must not
                    // stall every frame after it by a second. Once.
                    static bool s_warned = false;
                    if (!s_warned) {
                        s_warned = true;
                        Log::Warning("MetalBackend: frame pacing wait timed out (1s); pacing continues unpaced this frame");
                    }
                }
            }

            // The slot this frame reuses has completed: its timestamps and
            // GPU span are final (MetalInstrumentation.mm). Then this frame's
            // timing is decided and its sample buffer reset.
            ResolveGpuTiming(m_currentFrame);
            {
                const bool wanted = m_gpu.envMode != GpuTiming::Mode::Off || m_gpu.requested;
                m_gpu.active = wanted && (m_gpu.probed ? m_gpu.supported : ProbeGpuTiming());
                GpuTiming::Slot& tslot = m_gpu.slots[m_currentFrame];
                tslot.used = 0;
                tslot.encoders.clear();
                tslot.frame = m_frameNumber + 1;
                tslot.commitHostTime = 0.0;
                if (m_gpu.active) {
                    m_gpu.cpuPrev = m_gpu.cpuNow;
                    m_gpu.gpuPrev = m_gpu.gpuNow;
                    [m_device sampleTimestamps:&m_gpu.cpuNow gpuTimestamp:&m_gpu.gpuNow];
                    if (m_gpu.gpuNow > m_gpu.gpuPrev && m_gpu.cpuNow > m_gpu.cpuPrev) {
                        m_gpu.ticksToNs = static_cast<double>(m_gpu.cpuNow - m_gpu.cpuPrev) /
                                          static_cast<double>(m_gpu.gpuNow - m_gpu.gpuPrev);
                        if (!m_gpu.ratioLogged) {
                            m_gpu.ratioLogged = true;
                            Log::Info("MetalBackend: GPU timestamp tick = %.4f ns", m_gpu.ticksToNs);
                        }
                    }
                    TracyGpuInit();
                }
            }

            SyncDrawableSize();

            // Preserving the frame's depth is this frame's choice (any user's
            // wish): a store op, nothing to rebuild.
            m_frameDepthPreserved = false;
            for (bool wanted : m_frameDepthUsers) m_frameDepthPreserved = m_frameDepthPreserved || wanted;

            // Render Resolution: this frame's scene, if one was asked for.
            bool useScene = false;
            if (m_sceneRequestW > 0 && m_sceneRequestH > 0) {
                useScene = EnsureSceneTargets(m_sceneRequestW, m_sceneRequestH);
            } else if (m_sceneWidth != 0) {
                DestroySceneTargets();   // back at 100 %: free the slots
            }
            m_sceneRequestW = m_sceneRequestH = 0;
            { PROFILE_ZONE_N("Mtl.SettleDepth"); SettleDepthStorage(m_currentFrame); }

            BeginGpuCaptureIfDue();
            [m_captureScope beginScope];

            // Committed to this frame: queued texture updates from here on
            // belong to the next frame's flush (see m_staging).
            ++m_frameNumber;
            m_signpostOrdinal = 0;
            if (m_signpostsOn) {
                os_signpost_interval_begin(MtlSignpost::FrameLog(), MtlSignpost::FrameId(m_frameNumber), "Frame",
                                           "%{public}llu", static_cast<unsigned long long>(m_frameNumber));
            }
            if (m_metal4) {
                // Garbage of completed frames out, residency committed, the
                // slot's reusable command buffer begun; completion comes
                // back through the commit's feedback (M4CommitFrame).
                { PROFILE_ZONE_N("Mtl.M4Garbage"); M4CollectGarbage(); }
                { PROFILE_ZONE_N("Mtl.M4BeginCmd"); M4BeginFrameCommandBuffer(); }
            } else {
                m_cmd = MakeCommandBuffer(@"Frame", /*watchErrors=*/false);
                {
                    std::shared_ptr<SharedState> shared = m_shared;
                    const uint64_t frame = m_frameNumber;
                    const bool signposts = m_signpostsOn;
                    MetalBackend* self = this;   // outlives every command buffer: Shutdown waits for them
                    [m_cmd addScheduledHandler:^(id<MTLCommandBuffer>) {
                        if (signposts) os_signpost_interval_begin(MtlSignpost::FrameLog(), MtlSignpost::FrameId(frame), "GPU");
                        dispatch_semaphore_signal(shared->frameStarted);   // the pacing gate (BeginFrame)
                    }];
                    [m_cmd addCompletedHandler:^(id<MTLCommandBuffer> done) {
                        if (signposts) os_signpost_interval_end(MtlSignpost::FrameLog(), MtlSignpost::FrameId(frame), "GPU");
                        if (done.status == MTLCommandBufferStatusError) self->ReportCommandBufferError(done, "frame command buffer");
                        uint64_t d = shared->completedFrame.load(std::memory_order_relaxed);
                        while (d < frame && !shared->completedFrame.compare_exchange_weak(d, frame)) {}
                        dispatch_semaphore_signal(shared->framesInFlight);
                    }];
                }
            }
            m_slotCommandBuffers[m_currentFrame] = m_cmd;

            SlotImages& slot = m_slots[m_currentFrame];
            slot.ringUsed = 0;
            m_haveCommon = false;
            m_haveBones = false;

            {
                PROFILE_ZONE_N("Mtl.TexFlush");
                FlushPendingTextureUpdates();
            }

            // GPU timers nobody collected.
            for (auto it = m_gpuTimers.begin(); it != m_gpuTimers.end();) {
                if (it->second.frame + 120 < m_frameNumber) it = m_gpuTimers.erase(it);
                else ++it;
            }

            m_drawable = nil;
            m_drawableDecided = false;
            m_presenting = false;
            m_sceneActive = useScene;
            m_activeTarget = INVALID_RENDER_TARGET;
            m_frameOpened = false;
            m_frameClearColor = m_clearColor;
            BeginFramePass(/*resume=*/false);

            m_boundShader = INVALID_SHADER;
            m_boundShaderInfo = nullptr;
            m_debugGroups.clear();   // a frame's groups never outlive it
            m_encoderGroups = 0;
            m_boundTextures.fill(INVALID_TEXTURE);
            m_boundTextureInfo.fill(nullptr);
            m_depthRangeMin = 0.0f;
            m_depthRangeMax = 1.0f;
            m_frameActive = true;
            SetViewport(0, 0, static_cast<int>(FrameWidth()), static_cast<int>(FrameHeight()));
            ClearScissorRect();
        }
    }

    void MetalBackend::EndFrame(GLFWwindow* /*window*/) {
        PROFILE_ZONE_N("Mtl.EndFrame");
        if (!m_frameActive) return;
        @autoreleasepool {
            // A render target or an OIT pass left bound: back to the frame.
            if (m_oitPassOpen) OitEndPass();
            if (m_activeTarget != INVALID_RENDER_TARGET) BindRenderTarget(INVALID_RENDER_TARGET);
            // The depth this frame drew the world into — the scaled scene's,
            // if it had one — for the handoff below (the resolve ends the scene).
            const bool sceneFrame = m_sceneActive;
            const uint32_t worldW = FrameWidth(), worldH = FrameHeight();
            if (m_sceneActive) ResolveScaledScene();
            EnsureEncoder();   // nothing drew into the frame: its pass still clears it
            m_endingFrame = true;   // the frame's last segment (EndPass: its colour store)
            EndPass();
            m_endingFrame = false;
            m_frameActive = false;

            // Depth handoff: the next frame may read this frame's depth (the
            // preserved frame stored it) before its own pass opens.
            DepthStamp& stamp = m_depthStamps[m_currentFrame];
            stamp = DepthStamp{};
            if (m_depthHandoff && m_frameDepthPreserved) {
                stamp.frame = m_frameNumber;
                stamp.generation = m_frameDepthGeneration;
                stamp.scene = sceneFrame;
                stamp.width = worldW;
                stamp.height = worldH;
            }

            FlushUploads();   // committed first, so they execute before this frame
            if (m_presenting && m_drawable) {
                ++m_presentsIssued;
                m_lastPresent = std::chrono::steady_clock::now();
                std::shared_ptr<SharedState> shared = m_shared;
                const bool signposts = m_signpostsOn;
                const uint64_t frame = m_frameNumber;
                if (signposts) os_signpost_interval_begin(MtlSignpost::PresentLog(), MtlSignpost::FrameId(frame), "Present");
                [m_drawable addPresentedHandler:^(id<MTLDrawable>) {
                    if (signposts) os_signpost_interval_end(MtlSignpost::PresentLog(), MtlSignpost::FrameId(frame), "Present");
                    shared->presentsDone.fetch_add(1, std::memory_order_relaxed);
                }];
                if (!m_metal4) [m_cmd presentDrawable:m_drawable];   // Metal 4: the queue's wait/signal (M4CommitFrame)
            } else if (m_signpostsOn) {
                os_signpost_event_emit(MtlSignpost::PresentLog(), MtlSignpost::FrameId(m_frameNumber), "Unpresented");
            }
            SampleDeviceMemory();
            {
                PROFILE_ZONE_N("Mtl.Commit");
                if (m_metal4) {
                    M4CommitFrame();
                } else {
                    m_gpu.slots[m_currentFrame].commitHostTime = CACurrentMediaTime();
                    [m_cmd commit];
                }
            }
            if (m_signpostsOn) os_signpost_interval_end(MtlSignpost::FrameLog(), MtlSignpost::FrameId(m_frameNumber), "Frame");
            [m_captureScope endScope];
            EmitFramePlots();
            m_cmd = nil;
            m_drawable = nil;
            m_currentFrame = (m_currentFrame + 1) % kFramesInFlight;
            if (m_capturing) EndGpuCapture();
        }
        // New pipelines go to the manifest once the burst that made them has
        // settled (~5 s at 60 fps), as on Vulkan.
        if (m_pipelinesSinceSave > 0 && m_frameNumber - m_lastPipelineFrame > 300) SaveManifest(/*synchronous=*/false);
    }

    bool MetalBackend::MailboxDrawableFree() const {
        // Shown only when CoreAnimation can hand a drawable over without
        // waiting: no sooner than half a refresh after the last present, and
        // once at most m_mailboxPending - 1 presents are still on their way
        // to the screen. Every refresh still gets a new frame; presenting
        // faster only queues for drawables that are not released yet (a
        // composited window frees them at 2 x refresh). A present that never
        // reports back cannot freeze the picture: after 50 ms without one the
        // frame is shown regardless.
        const auto sinceLast = std::chrono::steady_clock::now() - m_lastPresent;
        if (m_presentsIssued == 0 || sinceLast >= std::chrono::milliseconds(50)) return true;
        if (sinceLast < m_presentInterval) return false;
        const uint64_t done = m_shared->presentsDone.load(std::memory_order_relaxed);
        return m_presentsIssued - std::min(done, m_presentsIssued) < static_cast<uint64_t>(m_mailboxPending);
    }

    id<MTLTexture> MetalBackend::FrameColorTexture() {
        if (m_sceneActive) return m_slots[m_currentFrame].sceneColor;
        if (!m_drawableDecided) {
            m_drawableDecided = true;
            // Vsync on: every frame is shown, nextDrawable paces the loop.
            // Vsync off: the mailbox decides (OBEY_SKIP=mailbox: the skip
            // phases show every frame, for A/B in one run).
            const bool mailbox = m_mailboxWanted && !m_vsync;
            const bool show = !mailbox || DevSkip("mailbox") || MailboxDrawableFree();
            if (show) {
                PROFILE_ZONE_N("Mtl.NextDrawable");
                const auto t0 = std::chrono::steady_clock::now();
                if (m_signpostsOn) {
                    os_signpost_interval_begin(MtlSignpost::PresentLog(), MtlSignpost::FrameId(m_frameNumber), "NextDrawable");
                }
                @autoreleasepool {
                    m_drawable = [m_layer nextDrawable];
                }
                if (m_signpostsOn) {
                    os_signpost_interval_end(MtlSignpost::PresentLog(), MtlSignpost::FrameId(m_frameNumber), "NextDrawable");
                }
                m_counters.drawableWaitUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
                m_presenting = m_drawable != nil &&
                               m_drawable.texture.width == m_drawableWidth &&
                               m_drawable.texture.height == m_drawableHeight;
                if (!m_presenting) m_drawable = nil;
            }
            PROFILE_PLOT("Mtl/Presented", static_cast<int64_t>(m_presenting ? 1 : 0));
        }
        return m_presenting ? m_drawable.texture : m_slots[m_currentFrame].standIn;
    }

    id<MTLBuffer> MetalBackend::RingAllocate(size_t bytes, size_t& outOffset, const void* data) {
        SlotImages& slot = m_slots[m_currentFrame];
        size_t offset = (slot.ringUsed + 255u) & ~static_cast<size_t>(255u);   // constant-buffer alignment
        if (!slot.ring || offset + bytes > slot.ring.length) {
            // Run dry: a bigger ring for the rest of the frame (and after).
            // The old one stays alive while this frame's commands use it.
            size_t capacity = std::max(kRingInitialBytes, slot.ring ? static_cast<size_t>(slot.ring.length) * 2 : 0);
            while (capacity < bytes) capacity *= 2;
            M4Release(slot.ring);
            slot.ring = [m_device newBufferWithLength:capacity options:MTLResourceStorageModeShared |
                                                                      MTLResourceCPUCacheModeWriteCombined];
            slot.ring.label = @"Uniform ring";
            M4Resident(slot.ring);
            offset = 0;
        }
        if (data) std::memcpy(static_cast<uint8_t*>(slot.ring.contents) + offset, data, bytes);   // nullptr: reserved, written by the caller
        slot.ringUsed = offset + bytes;
        outOffset = offset;
        return slot.ring;
    }

    // ========================================================================
    // PASSES
    // ========================================================================

    void MetalBackend::BeginFramePass(bool resume) {
        // The frame's colour is resolved when the pass opens (the drawable is
        // asked for as late as possible). Fresh (`resume` false): colour,
        // depth and stencil clear. Resumed after an interruption: colour
        // loaded; depth and stencil loaded if the frame's depth is preserved,
        // cleared otherwise (it was not stored — a defined, empty buffer).
        MTLRenderPassDescriptor* d = [MTLRenderPassDescriptor renderPassDescriptor];
        d.colorAttachments[0].loadAction = resume ? MTLLoadActionLoad : MTLLoadActionClear;
        d.colorAttachments[0].clearColor = MTLClearColorMake(m_frameClearColor[0], m_frameClearColor[1],
                                                             m_frameClearColor[2], m_frameClearColor[3]);
        // Decided when the encoder ends (EndPass): Store, or DontCare for
        // the last segment of an unpresented frame (m_endingFrame).
        d.colorAttachments[0].storeAction = MTLStoreActionUnknown;
        id<MTLTexture> depth = FrameDepthTex(m_currentFrame);
        const bool loadDepth = resume && m_frameDepthPreserved;
        const MTLStoreAction depthStore = m_frameDepthPreserved ? MTLStoreActionStore : MTLStoreActionDontCare;
        d.depthAttachment.texture = depth;
        d.depthAttachment.loadAction = loadDepth ? MTLLoadActionLoad : MTLLoadActionClear;
        d.depthAttachment.clearDepth = 1.0;
        d.depthAttachment.storeAction = depthStore;
        d.stencilAttachment.texture = depth;
        d.stencilAttachment.loadAction = loadDepth ? MTLLoadActionLoad : MTLLoadActionClear;
        d.stencilAttachment.clearStencil = 0;
        d.stencilAttachment.storeAction = depthStore;
        m_pass = Pass{};
        m_pass.kind = PassKind::Frame;
        m_pass.desc = d;
        m_pass.width = FrameWidth();
        m_pass.height = FrameHeight();
        m_pass.config = 0;
        m_pass.colorCount = 1;
    }

    bool MetalBackend::EnsureEncoder() {
        if (m_encoder) return true;
        if (!m_frameActive || m_pass.kind == PassKind::None || !m_pass.desc) return false;
        @autoreleasepool {
            if (m_pass.kind == PassKind::Frame) {
                id<MTLTexture> color = FrameColorTexture();
                if (!color) return false;
                m_pass.desc.colorAttachments[0].texture = color;
            }
            // Named for Instruments and the Metal debugger; the same name
            // keys the encoder's GPU timing and its Tracy GPU zone.
            NSString* label = nil;
            uint8_t kind = 0;
            if (m_nextEncoderLabel) {
                label = [NSString stringWithUTF8String:m_nextEncoderLabel];
                kind = 3;
                m_nextEncoderLabel = nullptr;
            } else {
                switch (m_pass.kind) {
                    case PassKind::Frame:
                        label = m_sceneActive ? @"Scene (scaled)" : (m_frameOpened ? @"Frame (resumed)" : @"Frame");
                        kind = 0;
                        break;
                    case PassKind::Target: {
                        auto rtIt = m_targets.find(m_pass.target);
                        const char* name = rtIt != m_targets.end() && !rtIt->second.label.empty()
                                           ? rtIt->second.label.c_str() : "Target";
                        label = [NSString stringWithFormat:@"%s (%ux%u)", name, m_pass.width, m_pass.height];
                        kind = 1;
                        break;
                    }
                    case PassKind::Oit:
                        label = [NSString stringWithFormat:@"OIT pass %d", static_cast<int>(m_oitOpenPass)];
                        kind = 2;
                        break;
                    default: break;
                }
            }
            const char* name = InternLabel(label);
            const bool shared = m_pass.kind == PassKind::Frame;
            const uint32_t first = AttachRenderSamples(m_pass.desc, kind, shared, name);
            if (first != UINT32_MAX) {
                static const uint32_t kColors[4] = {0x4C9A2A, 0xD8A03C, 0x8C5AD8, 0x3C8CD8};
                TracyGpuZone(m_currentFrame, first, first + 3,
                             TracySrcloc(name, m_debugGroups.empty() ? nullptr : m_debugGroups.back(), kColors[kind & 3]));
            }
            if (m_metal4) {
                // A pass that loads any attachment continues earlier passes'
                // output: its barrier waits for their fragment work.
                bool resumed = m_packFrame;   // a shader pack's passes sample each other's output
                MTLRenderPassDescriptor* d = m_pass.desc;
                for (NSUInteger i = 0; i < 8 && d.colorAttachments[i].texture; ++i) {
                    if (d.colorAttachments[i].loadAction == MTLLoadActionLoad) resumed = true;
                }
                if (d.depthAttachment.texture && d.depthAttachment.loadAction == MTLLoadActionLoad) resumed = true;
                m_encoder = M4MakeRenderEncoder(m_cmd4, m_pass.desc, m_pass.width, m_pass.height, resumed, first);
            } else {
                m_encoder = [m_cmd renderCommandEncoderWithDescriptor:m_pass.desc];
            }
            if (m_encoder) {
                [m_encoder setLabel:label];
                ++m_counters.encoders;
                if (m_signpostsOn) {
                    os_signpost_interval_begin(MtlSignpost::EncodeLog(),
                                               MtlSignpost::EncoderId(m_frameNumber, m_signpostOrdinal), "Encode",
                                               "%{public}s", name);
                }
            }
        }
        if (!m_encoder) {
            Log::Error("MetalBackend: could not open a render pass");
            return false;
        }
        if (m_pass.kind == PassKind::Frame) m_frameOpened = true;
        // A target bound for overwriting (colour loads DontCare) resumes,
        // if its pass is ever split, with what its first encoder drew.
        if (m_pass.kind == PassKind::Target) {
            MTLRenderPassDescriptor* d = m_pass.desc;
            for (NSUInteger i = 0; i < 8 && d.colorAttachments[i].texture; ++i) {
                if (d.colorAttachments[i].loadAction == MTLLoadActionDontCare) d.colorAttachments[i].loadAction = MTLLoadActionLoad;
            }
        }
        m_pass.clears = false;
        m_cache = EncoderCache{};
        for (const char* name : m_debugGroups) [m_encoder pushDebugGroup:DebugGroupName(name)];
        m_encoderGroups = m_debugGroups.size();
        ApplyViewport();
        ApplyScissor();
        return true;
    }

    void MetalBackend::EndPass() {
        // The frame pass's colour store (its descriptor says Unknown): the
        // last segment of an unpresented frame is never read — nothing
        // samples a stand-in, the next frame's pass clears its own — so it
        // stays in tile memory; everything else stores.
        static const bool s_storeStandIn = std::getenv("OBEY_STANDIN_STORE") != nullptr;
        auto settleFrameColorStore = [&] {
            if (m_pass.kind != PassKind::Frame) return;
            // OBEY_SKIP=standin: the skip phases store it (the phase A/B).
            const bool unread = m_endingFrame && !m_presenting && !s_storeStandIn && !DevSkip("standin");
            [m_encoder setColorStoreAction:(unread ? MTLStoreActionDontCare : MTLStoreActionStore) atIndex:0];
        };
        if (m_encoder) {
            for (; m_encoderGroups > 0; --m_encoderGroups) [m_encoder popDebugGroup];
            settleFrameColorStore();
            if (m_metal4) M4WriteEncoderEnd();
            [m_encoder endEncoding];
            m_encoder = nil;
            if (m_signpostsOn) {
                os_signpost_interval_end(MtlSignpost::EncodeLog(),
                                         MtlSignpost::EncoderId(m_frameNumber, m_signpostOrdinal), "Encode");
            }
            ++m_signpostOrdinal;
        } else if (m_pass.kind != PassKind::None && m_pass.clears) {
            // A pass that never drew but was cleared: the clear is its whole
            // content, so it runs (an empty encoder: load, store).
            if (EnsureEncoder()) {
                for (; m_encoderGroups > 0; --m_encoderGroups) [m_encoder popDebugGroup];
                settleFrameColorStore();
                if (m_metal4) M4WriteEncoderEnd();
                [m_encoder endEncoding];
                m_encoder = nil;
                if (m_signpostsOn) {
                    os_signpost_interval_end(MtlSignpost::EncodeLog(),
                                             MtlSignpost::EncoderId(m_frameNumber, m_signpostOrdinal), "Encode");
                }
                ++m_signpostOrdinal;
            }
        }
        m_pass = Pass{};
    }

    void MetalBackend::BindRenderTargetOverwriting(RenderTargetHandle handle) {
        m_bindDiscardColor = true;
        BindRenderTarget(handle);
        m_bindDiscardColor = false;
    }

    void MetalBackend::BindRenderTarget(RenderTargetHandle handle) {
        if (!m_frameActive || handle == m_activeTarget) return;
        if (m_oitPassOpen) return;   // an OIT pass is open: OitEndPass returns to the frame
        auto rtIt = handle != INVALID_RENDER_TARGET ? m_targets.find(handle) : m_targets.end();
        if (handle != INVALID_RENDER_TARGET && (rtIt == m_targets.end() || (!rtIt->second.color && !rtIt->second.depth))) return;

        // Leaving the frame before its pass opened (a target drawn ahead of
        // it): the frame stays unopened — its first use opens it, clearing.
        if (m_pass.kind == PassKind::Frame && !m_frameOpened && !m_encoder) {
            m_pass = Pass{};
        } else {
            EndPass();
        }

        if (handle == INVALID_RENDER_TARGET) {
            m_activeTarget = INVALID_RENDER_TARGET;
            BeginFramePass(/*resume=*/m_frameOpened);
        } else {
            TargetInfo& rt = rtIt->second;
            MTLRenderPassDescriptor* d = [MTLRenderPassDescriptor renderPassDescriptor];
            // A texture target draws every colour attachment it wraps (a
            // shader pack's DRAWBUFFERS); the engine's own targets one.
            const size_t colorCount = rt.ownsImages ? 1u : rt.colors.size();
            for (size_t i = 0; i < colorCount; ++i) {
                d.colorAttachments[i].texture = rt.ownsImages ? rt.color : rt.colors[i];
                // A pass that overwrites every pixel (BindRenderTargetOverwriting)
                // skips the tile load of what is there.
                d.colorAttachments[i].loadAction = m_bindDiscardColor ? MTLLoadActionDontCare : MTLLoadActionLoad;
                d.colorAttachments[i].storeAction = MTLStoreActionStore;
            }
            if (!rt.ownsImages && colorCount == 0) {
                // Depth-only (a depth snapshot's target): the pass takes its
                // size from the depth attachment.
                d.renderTargetWidth = static_cast<NSUInteger>(rt.width);
                d.renderTargetHeight = static_cast<NSUInteger>(rt.height);
            }
            if (rt.depth) {
                d.depthAttachment.texture = rt.depth;
                d.depthAttachment.loadAction = MTLLoadActionLoad;
                d.depthAttachment.storeAction = MTLStoreActionStore;
                d.stencilAttachment.texture = rt.depth;
                d.stencilAttachment.loadAction = MTLLoadActionLoad;
                d.stencilAttachment.storeAction = MTLStoreActionStore;
            }
            m_pass = Pass{};
            m_pass.kind = PassKind::Target;
            m_pass.target = handle;
            m_pass.desc = d;
            m_pass.width = static_cast<uint32_t>(rt.width);
            m_pass.height = static_cast<uint32_t>(rt.height);
            // No depth attachment: its own pipeline shape, and every draw's
            // depth / stencil test off (PrepareDraw). A texture target's
            // pipelines bake its formats (configs 5 / 6, keyed by the set).
            if (rt.ownsImages) {
                m_pass.config = rt.depth ? 0 : kConfigTargetNoDepth;
            } else {
                m_pass.config = rt.depth ? kConfigTextures : kConfigTexturesNoDepth;
                m_pass.formats = rt.formats;
                m_pass.attachmentsKey = rt.attachmentsKey;
                m_pass.colorCount = static_cast<uint8_t>(colorCount);
            }
            m_activeTarget = handle;
            if (auto texIt = m_textures.find(rt.colorTexture); texIt != m_textures.end()) {
                texIt->second.lastUsedFrame = m_frameNumber;
            }
        }
        // Default viewport and scissor for the new pass, as GL's
        // BindRenderTarget sets the viewport to the target's size.
        SetViewport(0, 0, static_cast<int>(ActiveWidth()), static_cast<int>(ActiveHeight()));
        ClearScissorRect();
    }

    // ========================================================================
    // CLEAR / VIEWPORT / SCISSOR
    // ========================================================================

    void MetalBackend::SetClearColor(float r, float g, float b, float a) {
        m_clearColor = {r, g, b, a};
    }

    void MetalBackend::Clear(bool color, bool depth, bool stencil) {
        if (!m_frameActive) return;
        if (!color && !depth && !stencil) return;
        if (!m_encoder) {
            // The pass has not opened: the clear is its load actions.
            if (m_pass.kind == PassKind::None || !m_pass.desc) return;
            MTLRenderPassDescriptor* d = m_pass.desc;
            if (color) {
                // Every attachment of a texture target (glClear's rule);
                // attachment 0 of the engine's passes (vkCmdClearAttachments').
                const uint32_t count = ConfigFromPass(m_pass.config) ? m_pass.colorCount : 1u;
                for (uint32_t i = 0; i < count; ++i) {
                    d.colorAttachments[i].loadAction = MTLLoadActionClear;
                    d.colorAttachments[i].clearColor = MTLClearColorMake(m_clearColor[0], m_clearColor[1],
                                                                         m_clearColor[2], m_clearColor[3]);
                }
                if (m_pass.kind == PassKind::Frame && !m_frameOpened) m_frameClearColor = m_clearColor;
            }
            if (depth && d.depthAttachment.texture) {
                d.depthAttachment.loadAction = MTLLoadActionClear;
                d.depthAttachment.clearDepth = 1.0;
            }
            if (stencil && d.stencilAttachment.texture) {
                d.stencilAttachment.loadAction = MTLLoadActionClear;
                d.stencilAttachment.clearStencil = 0;
            }
            m_pass.clears = true;
            return;
        }
        // Inside an open pass: a quad over the whole of it (the scissor does
        // not apply, as vkCmdClearAttachments's full rect did not).
        DrawClearQuad(color, depth, stencil, 1.0f, MTLScissorRect{0, 0, ActiveWidth(), ActiveHeight()});
    }

    void MetalBackend::ClearDepthRect(int x, int y, int width, int height, float depth) {
        if (!m_frameActive || !EnsureEncoder()) return;
        const int w = static_cast<int>(ActiveWidth()), h = static_cast<int>(ActiveHeight());
        const int x0 = std::clamp(x, 0, w), y0 = std::clamp(y, 0, h);
        const int x1 = std::clamp(x + width, 0, w), y1 = std::clamp(y + height, 0, h);
        if (x1 <= x0 || y1 <= y0) return;
        DrawClearQuad(false, true, false, depth,
                      MTLScissorRect{static_cast<NSUInteger>(x0), static_cast<NSUInteger>(y0),
                                     static_cast<NSUInteger>(x1 - x0), static_cast<NSUInteger>(y1 - y0)});
    }

    MTLViewport MetalBackend::ToMetalViewport(const VkStyleViewport& v, float zMin, float zMax) {
        // A Vulkan viewport (x, y, w, h) maps NDC y onto framebuffer rows as
        // y + (ndc + 1) / 2 * h; Metal's (x, oy, w, mh) as oy + (1 - ndc) / 2
        // * mh, its NDC being y-up. They agree for oy = y + h, mh = -h: the
        // flipped viewport every world draw uses on Vulkan is Metal's plain
        // one, and a plain Vulkan viewport (the upscale draw) a flipped one.
        MTLViewport m;
        m.originX = v.x;
        m.originY = v.y + v.height;
        m.width   = v.width;
        m.height  = -v.height;
        m.znear   = zMin;
        m.zfar    = zMax;
        return m;
    }

    void MetalBackend::SetViewport(int x, int y, int width, int height) {
        if (!m_frameActive) return;
        // GL's convention (x, y from the bottom-left edge), as Vulkan's
        // flipped viewport, about the height of the pass recording now.
        m_viewport = {static_cast<float>(x), static_cast<float>(static_cast<int>(ActiveHeight()) - y),
                      static_cast<float>(width), -static_cast<float>(height)};
        ApplyViewport();
    }

    void MetalBackend::SetDepthRange(float minDepth, float maxDepth) {
        if (m_depthRangeMin == minDepth && m_depthRangeMax == maxDepth) return;
        m_depthRangeMin = minDepth;
        m_depthRangeMax = maxDepth;
        ApplyViewport();
    }

    void MetalBackend::ApplyViewport() {
        if (!m_encoder) return;
        MTLViewport v = ToMetalViewport(m_viewport, m_depthRangeMin, m_depthRangeMax);
        // A shader pack's texture target stores its image the OpenGL way, row
        // 0 at the scene's bottom (its programs sample it with GL's v and
        // read gl_FragCoord bottom-up): the viewport runs the other way for
        // every draw into one, and the winding flips with it (PrepareDraw).
        // The final pass draws into the frame, upright.
        if (ConfigFromPass(m_pass.config)) {
            v.originY += v.height;
            v.height = -v.height;
        }
        const MTLViewport& c = m_cache.viewport;
        if (m_cache.viewportValid && c.originX == v.originX && c.originY == v.originY && c.width == v.width &&
            c.height == v.height && c.znear == v.znear && c.zfar == v.zfar) return;
        [m_encoder setViewport:v];
        m_cache.viewport = v;
        m_cache.viewportValid = true;
    }

    void MetalBackend::SetScissorRect(int x, int y, int w, int h) {
        if (!m_frameActive) return;
        // Top-left origin, as the caller's. Must stay inside the pass.
        const int maxW = static_cast<int>(ActiveWidth()), maxH = static_cast<int>(ActiveHeight());
        const int x0 = std::clamp(x, 0, maxW), y0 = std::clamp(y, 0, maxH);
        m_scissor.x = static_cast<NSUInteger>(x0);
        m_scissor.y = static_cast<NSUInteger>(y0);
        m_scissor.width = static_cast<NSUInteger>(std::clamp(w, 0, std::max(0, maxW - x0)));
        m_scissor.height = static_cast<NSUInteger>(std::clamp(h, 0, std::max(0, maxH - y0)));
        ApplyScissor();
    }

    void MetalBackend::ClearScissorRect() {
        if (!m_frameActive) return;
        m_scissor = MTLScissorRect{0, 0, ActiveWidth(), ActiveHeight()};
        ApplyScissor();
    }

    void MetalBackend::ApplyScissor() {
        if (!m_encoder) return;
        // A scissor set for a larger pass, clamped into this one.
        MTLScissorRect r = m_scissor;
        const NSUInteger w = ActiveWidth(), h = ActiveHeight();
        r.x = std::min(r.x, w);
        r.y = std::min(r.y, h);
        r.width = std::min(r.width, w - r.x);
        r.height = std::min(r.height, h - r.y);
        const MTLScissorRect& c = m_cache.scissor;
        if (m_cache.scissorValid && c.x == r.x && c.y == r.y && c.width == r.width && c.height == r.height) return;
        [m_encoder setScissorRect:r];
        m_cache.scissor = r;
        m_cache.scissorValid = true;
    }

    // ========================================================================
    // READ-BACK AND COPIES
    // ========================================================================

    bool MetalBackend::SuspendFrameForCopy() {
        // The frame (not a target or OIT pass) is what a copy suspends and
        // resumes; its pass opens first, so everything drawn so far is in the
        // image the copy reads.
        if (!m_frameActive || m_activeTarget != INVALID_RENDER_TARGET || m_oitPassOpen) return false;
        if (m_pass.kind != PassKind::Frame || !EnsureEncoder()) return false;
        EndPass();
        return true;
    }

    void MetalBackend::BlitRenderTargetDepth(RenderTargetHandle src, RenderTargetHandle dst) {
        if (!m_frameActive || m_oitPassOpen) return;
        auto s = m_targets.find(src);
        auto d = m_targets.find(dst);
        if (s == m_targets.end() || d == m_targets.end() || !s->second.depth || !d->second.depth) return;
        if (s->second.depth == d->second.depth) return;
        if (s->second.width != d->second.width || s->second.height != d->second.height) {
            Log::Warning("MetalBackend: BlitRenderTargetDepth: %dx%d into %dx%d (sizes must match)",
                         s->second.width, s->second.height, d->second.width, d->second.height);
            return;
        }
        CopyBetweenPasses(s->second.depth, d->second.depth, @"Depth copy");
    }

    bool MetalBackend::CopyTexture(TextureHandle src, TextureHandle dst) {
        if (!m_frameActive || m_oitPassOpen) return false;
        auto s = m_textures.find(src);
        auto d = m_textures.find(dst);
        if (s == m_textures.end() || d == m_textures.end() || !s->second.texture || !d->second.texture) return false;
        if (s->second.bufferTexture || d->second.bufferTexture || s->second.texture == d->second.texture) return false;
        if (s->second.width != d->second.width || s->second.height != d->second.height || s->second.format != d->second.format) {
            Log::Warning("MetalBackend: CopyTexture: %dx%d (format %d) into %dx%d (format %d): sizes and formats must match",
                         s->second.width, s->second.height, static_cast<int>(s->second.format),
                         d->second.width, d->second.height, static_cast<int>(d->second.format));
            return false;
        }
        CopyBetweenPasses(FrameTexture(s->second), FrameTexture(d->second), @"Texture copy");
        d->second.lastUsedFrame = s->second.lastUsedFrame = m_frameNumber;
        return true;
    }

    void MetalBackend::CopyBetweenPasses(id<MTLTexture> src, id<MTLTexture> dst, NSString* label) {
        @autoreleasepool {
            // Whatever pass is recording ends (the copy reads what it drew),
            // the image copies whole, and the pass resumes with its viewport
            // and scissor.
            const RenderTargetHandle active = m_activeTarget;
            const VkStyleViewport viewport = m_viewport;
            const MTLScissorRect scissor = m_scissor;
            if (m_pass.kind == PassKind::Frame && !m_frameOpened && !m_encoder && !m_pass.clears) m_pass = Pass{};
            else EndPass();
            id blit = MakeBlitEncoder(m_cmd, label);
            [blit copyFromTexture:src toTexture:dst];
            EndBlitEncoder(blit);
            if (active != INVALID_RENDER_TARGET) {
                m_activeTarget = INVALID_RENDER_TARGET;
                BindRenderTarget(active);
            } else {
                BeginFramePass(/*resume=*/m_frameOpened);
            }
            m_viewport = viewport;
            m_scissor = scissor;
            ApplyViewport();
            ApplyScissor();
        }
    }

    bool MetalBackend::RequestBackbufferReadback(int x, int y, int w, int h) {
        if (!m_frameActive || m_activeTarget != INVALID_RENDER_TARGET || m_oitPassOpen) return false;
        @autoreleasepool {
            // Several may be in flight (the capture asks for a row a frame and
            // takes each two frames later); past the limit the oldest is
            // dropped, which has to wait for its copy first.
            if (m_readbacks.size() >= kMaxReadbacksInFlight) {
                Readback& oldest = m_readbacks.front();
                if (oldest.cmd) [oldest.cmd waitUntilCompleted];
                oldest.cmd = nil;
                m_readbackFree.push_back(oldest);
                m_readbacks.pop_front();
            }
            // Bottom-left based (the SetViewport convention); rows run top-down.
            const int sw = static_cast<int>(FrameWidth()), sh = static_cast<int>(FrameHeight());
            x = std::clamp(x, 0, sw);
            y = std::clamp(y, 0, sh);
            w = std::clamp(w, 0, sw - x);
            h = std::clamp(h, 0, sh - y);
            if (w <= 0 || h <= 0) return false;
            const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;

            if (m_pass.kind != PassKind::Frame || !EnsureEncoder()) return false;
            id<MTLTexture> source = m_pass.desc.colorAttachments[0].texture;
            if (!SuspendFrameForCopy() || !source) return false;

            Readback rb;
            for (size_t i = 0; i < m_readbackFree.size(); ++i) {
                if (m_readbackFree[i].capacity >= bytes) {
                    rb = m_readbackFree[i];
                    m_readbackFree.erase(m_readbackFree.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            }
            if (!rb.buffer) {
                rb.buffer = [m_device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
                rb.buffer.label = [NSString stringWithFormat:@"Readback %zu KB", bytes / 1024];
                rb.capacity = bytes;
                M4Resident(rb.buffer);
            }
            id blit = MakeBlitEncoder(m_cmd, @"Readback");
            [blit copyFromTexture:source
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(static_cast<NSUInteger>(x), static_cast<NSUInteger>(sh - y - h), 0)
                       sourceSize:MTLSizeMake(static_cast<NSUInteger>(w), static_cast<NSUInteger>(h), 1)
                         toBuffer:rb.buffer
                destinationOffset:0
           destinationBytesPerRow:static_cast<NSUInteger>(w) * 4u
         destinationBytesPerImage:bytes];
            EndBlitEncoder(blit);

            // Resume the frame, keeping what it has drawn so far.
            BeginFramePass(/*resume=*/true);
            SetViewport(0, 0, sw, sh);
            ClearScissorRect();

            rb.width = w;
            rb.height = h;
            rb.frameNumber = m_frameNumber;
            rb.cmd = m_cmd;
            m_readbacks.push_back(rb);
            return true;
        }
    }

    bool MetalBackend::TakeBackbufferReadback(std::vector<uint8_t>& outRgba, int& outW, int& outH) {
        if (m_readbacks.empty()) return false;
        Readback rb = m_readbacks.front();
        m_readbacks.pop_front();
        // Taken on a later frame than requested: the frame's command buffer
        // was committed, and its completion is the proof the copy ran.
        if (rb.cmd && rb.cmd.status < MTLCommandBufferStatusCompleted) [rb.cmd waitUntilCompleted];
        rb.cmd = nil;
        if (m_metal4) {
            // No command buffer to wait on: the frame's completion is the proof.
            while (m_shared->completedFrame.load(std::memory_order_acquire) < rb.frameNumber) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
        const size_t bytes = static_cast<size_t>(rb.width) * static_cast<size_t>(rb.height) * 4u;
        outRgba.resize(bytes);
        // BGRA → RGBA with a solid alpha, a pixel a word, over the fork-join
        // pool (a whole panorama face is 38 MB).
        const size_t pixels = bytes / 4u;
        constexpr size_t kChunk = 256u * 1024u;
        const auto* src = static_cast<const uint32_t*>(rb.buffer.contents);
        uint8_t* dstBytes = outRgba.data();
        Core::ParallelFor((pixels + kChunk - 1) / kChunk, 1, [&](size_t c) {
            const size_t begin = c * kChunk, end = std::min(pixels, begin + kChunk);
            uint32_t* dst = reinterpret_cast<uint32_t*>(dstBytes) + begin;
            const uint32_t* in = src + begin;
            for (size_t i = 0, n = end - begin; i < n; ++i) {
                const uint32_t v = in[i];   // bytes B G R A, little-endian
                dst[i] = (v & 0x0000FF00u) | ((v & 0x000000FFu) << 16) | ((v >> 16) & 0x000000FFu) | 0xFF000000u;
            }
        });
        outW = rb.width;
        outH = rb.height;
        m_readbackFree.push_back(rb);
        return true;
    }

    bool MetalBackend::CopyFramebufferToRenderTarget(RenderTargetHandle dst) {
        if (!m_frameActive || m_activeTarget != INVALID_RENDER_TARGET || m_oitPassOpen) return false;
        auto it = m_targets.find(dst);
        if (it == m_targets.end() || !it->second.color) return false;
        TargetInfo& rt = it->second;
        if (static_cast<uint32_t>(rt.width) != FrameWidth() || static_cast<uint32_t>(rt.height) != FrameHeight()) {
            return false;
        }
        @autoreleasepool {
            if (m_pass.kind != PassKind::Frame || !EnsureEncoder()) return false;
            id<MTLTexture> source = m_pass.desc.colorAttachments[0].texture;
            if (!SuspendFrameForCopy() || !source) return false;
            id blit = MakeBlitEncoder(m_cmd, @"Copy framebuffer");
            [blit copyFromTexture:source
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(FrameWidth(), FrameHeight(), 1)
                        toTexture:rt.color
                 destinationSlice:0
                 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
            EndBlitEncoder(blit);
            BeginFramePass(/*resume=*/true);
            SetViewport(0, 0, static_cast<int>(FrameWidth()), static_cast<int>(FrameHeight()));
            ClearScissorRect();
            return true;
        }
    }

    // ========================================================================
    // SCALED SCENE (Render Resolution)
    // ========================================================================

    void MetalBackend::RequestScaledScene(int width, int height) {
        m_sceneRequestW = m_sceneRequestH = 0;
        if (width <= 0 || height <= 0 || !m_device || m_upscaleShader == INVALID_SHADER) return;
        // The window's own size is the 100 % path: no scene, no extra pass.
        int fbW = 0, fbH = 0;
        if (m_window) glfwGetFramebufferSize(m_window, &fbW, &fbH);
        if (width == fbW && height == fbH) return;
        constexpr int kMaxDim = 16384;   // every Mac GPU's 2D texture limit
        m_sceneRequestW = static_cast<uint32_t>(std::min(width, kMaxDim));
        m_sceneRequestH = static_cast<uint32_t>(std::min(height, kMaxDim));
    }

    bool MetalBackend::EnsureSceneTargets(uint32_t width, uint32_t height) {
        if (m_sceneWidth == width && m_sceneHeight == height && m_slots[0].sceneColor) return true;
        DestroySceneTargets();
        @autoreleasepool {
            MTLTextureDescriptor* color = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kColorFormat
                                                                                              width:width
                                                                                             height:height
                                                                                          mipmapped:NO];
            color.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            color.storageMode = MTLStorageModePrivate;
            for (SlotImages& slot : m_slots) {
                slot.sceneColor = [m_device newTextureWithDescriptor:color];
                M4Resident(slot.sceneColor);
                slot.sceneDepth = MakeDepthTexture(width, height, /*memoryless=*/false, @"Scaled scene depth");
                slot.sceneDepthMemoryless = false;
                if (!slot.sceneColor || !slot.sceneDepth) {
                    Log::Error("MetalBackend: scaled scene %ux%u could not be created - drawn at 100%%", width, height);
                    DestroySceneTargets();
                    return false;
                }
                slot.sceneColor.label = @"Scaled scene";
            }
        }
        m_sceneWidth = width;
        m_sceneHeight = height;
        ++m_frameDepthGeneration;   // the scene's depth images are new
        const size_t bytes = static_cast<size_t>(width) * height * 9u * kFramesInFlight;
        m_memStats.textureMemory += bytes;
        m_memStats.totalAllocated += bytes;
        Log::Info("MetalBackend: scaled scene %ux%u (window %ux%u)", width, height, m_drawableWidth, m_drawableHeight);
        return true;
    }

    id<MTLTexture> MetalBackend::MakeDepthTexture(uint32_t width, uint32_t height, bool memoryless, NSString* label) {
        @autoreleasepool {
            MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kDepthFormat
                                                                                          width:width
                                                                                         height:height
                                                                                      mipmapped:NO];
            // Memoryless: a render target and nothing else — it has no
            // system memory to sample from. Private: sampleable (the
            // half-res rain's reprojection, Improved Transparency).
            d.usage = memoryless ? MTLTextureUsageRenderTarget : (MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
            d.storageMode = memoryless ? MTLStorageModeMemoryless : MTLStorageModePrivate;
            id<MTLTexture> t = [m_device newTextureWithDescriptor:d];
            if (t) t.label = memoryless ? [label stringByAppendingString:@" (memoryless)"] : label;
            M4Resident(t);
            return t;
        }
    }

    void MetalBackend::SettleDepthStorage(uint32_t slotIndex) {
        SlotImages& slot = m_slots[slotIndex];
        // Called once per frame for the slot about to record, after its
        // previous frame completed (the frame wait) and before anything
        // references its images: replacing them is safe. Preservation is
        // this frame's choice (m_frameDepthPreserved), so the storage
        // follows it with no lag — a frame that wants its depth kept
        // always draws into a Private image.
        // OBEY_SKIP=memoryless: Private depth in the skip phases (the phase A/B).
        const bool wantMemoryless = m_memorylessDepthOk && !m_frameDepthPreserved && !DevSkip("memoryless");
        if (slot.depth && slot.depthMemoryless != wantMemoryless) {
            id<MTLTexture> t = MakeDepthTexture(m_drawableWidth, m_drawableHeight, wantMemoryless, @"Frame depth");
            if (t) {
                M4Release(slot.depth);
                slot.depth = t;
                slot.depthMemoryless = wantMemoryless;
                ++m_frameDepthGeneration;   // a handed-off depth of the old image is stale
            }
        }
        if (slot.sceneDepth && slot.sceneDepthMemoryless != wantMemoryless) {
            id<MTLTexture> t = MakeDepthTexture(m_sceneWidth, m_sceneHeight, wantMemoryless, @"Scaled scene depth");
            if (t) {
                M4Release(slot.sceneDepth);
                slot.sceneDepth = t;
                slot.sceneDepthMemoryless = wantMemoryless;
                ++m_frameDepthGeneration;
            }
        }
        // Improved Transparency samples the frame's depth through its own
        // wrapper (OitSlot::frameDepthTex), taken when its targets were
        // made: point it at whichever image the slot holds now. (The
        // FrameDepthTexture wrapper follows on its own, FrameDepthTextureFor.)
        if (m_oitCreated && slotIndex < m_oitSlots.size()) {
            const OitSlot& o = m_oitSlots[slotIndex];
            auto it = o.frameDepthTex != INVALID_TEXTURE ? m_textures.find(o.frameDepthTex) : m_textures.end();
            id<MTLTexture> current = m_oitOnScene ? slot.sceneDepth : slot.depth;
            if (it != m_textures.end() && current && it->second.texture != current) {
                it->second.texture = current;
                it->second.width = static_cast<int>(current.width);
                it->second.height = static_cast<int>(current.height);
            }
        }
    }

    void MetalBackend::DestroySceneTargets() {
        if (m_sceneWidth == 0 && !m_slots[0].sceneColor) return;
        const size_t bytes = static_cast<size_t>(m_sceneWidth) * m_sceneHeight * 9u * kFramesInFlight;
        m_memStats.textureMemory -= std::min(m_memStats.textureMemory, bytes);
        m_memStats.totalAllocated -= std::min(m_memStats.totalAllocated, bytes);
        for (SlotImages& slot : m_slots) {
            M4Release(slot.sceneColor);
            M4Release(slot.sceneDepth);
            slot.sceneColor = nil;
            slot.sceneDepth = nil;
            if (slot.sceneDepthTexture != INVALID_TEXTURE) {
                EraseTextureEntry(slot.sceneDepthTexture);
                slot.sceneDepthTexture = INVALID_TEXTURE;
            }
        }
        m_sceneWidth = m_sceneHeight = 0;
        ++m_frameDepthGeneration;
    }

    void MetalBackend::ResolveScaledScene() {
        if (!m_sceneActive) return;
        if (!m_frameActive) { m_sceneActive = false; return; }
        PROFILE_ZONE_N("Mtl.ResolveScaledScene");
        @autoreleasepool {
            // Back on the scene's own pass first: an OIT pass or a render
            // target may still be open.
            if (m_oitPassOpen) OitEndPass();
            if (m_activeTarget != INVALID_RENDER_TARGET) BindRenderTarget(INVALID_RENDER_TARGET);
            EnsureEncoder();
            EndPass();

            // The frame is the drawable again: its pass opens on nothing for
            // the upscale draw to fill (colour DONT_CARE), the slot's own depth
            // cleared, for the GUI.
            id<MTLTexture> scene = m_slots[m_currentFrame].sceneColor;
            m_sceneActive = false;
            BeginFramePass(/*resume=*/false);
            m_pass.desc.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            m_nextEncoderLabel = "Upscale";   // not "Frame": its own encoder, its own timing
            if (!EnsureEncoder()) return;

            // One triangle over the whole image, sampling the scene bilinear —
            // a smooth upscale below 100 %, a 2x2 box at 200 %. The shader is
            // Vulkan's (a plain viewport: y-down NDC), hence the flip.
            PipelineState s;
            s.depthTestEnabled = false;
            s.depthWriteEnabled = false;
            s.blendEnabled = false;
            s.cullMode = CullMode::None;
            id<MTLRenderPipelineState> pipeline = PipelineFor(s, m_upscaleShader, 0);
            if (pipeline) {
                const VkStyleViewport full{0.0f, 0.0f, static_cast<float>(m_drawableWidth),
                                           static_cast<float>(m_drawableHeight)};
                [m_encoder setViewport:ToMetalViewport(full, 0.0f, 1.0f)];
                [m_encoder setScissorRect:MTLScissorRect{0, 0, m_drawableWidth, m_drawableHeight}];
                [m_encoder setRenderPipelineState:pipeline];
                [m_encoder setDepthStencilState:DepthStencilFor(s)];
                [m_encoder setCullMode:MTLCullModeNone];
                id<MTLSamplerState> bilinear = SamplerFor(MTLSamplerMinMagFilterLinear, MTLSamplerMinMagFilterLinear,
                                                          MTLSamplerMipFilterNotMipmapped,
                                                          MTLSamplerAddressModeClampToEdge,
                                                          MTLSamplerAddressModeClampToEdge, 1.0f, 0.0f);
                if (m_metal4) {
                    M4BindTexture(true, 0, scene);
                    M4BindSampler(true, 0, bilinear);
                } else {
                    [m_encoder setFragmentTexture:scene atIndex:0];
                    [m_encoder setFragmentSamplerState:bilinear atIndex:0];
                }
                [m_encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            }
            m_cache = EncoderCache::Unknown();
            SetViewport(0, 0, static_cast<int>(m_drawableWidth), static_cast<int>(m_drawableHeight));
            ClearScissorRect();
        }
    }

    // ========================================================================
    // FRAME DEPTH: PRESERVED, HANDOFF
    // ========================================================================

    void MetalBackend::SetFrameDepthPreserved(bool preserved, FrameDepthUser user) {
        m_frameDepthUsers[static_cast<size_t>(user)] = preserved;   // applied by the next BeginFrame
    }

    TextureHandle MetalBackend::FrameDepthTextureFor(uint32_t slot, bool scene) {
        id<MTLTexture> texture = scene ? m_slots[slot].sceneDepth : m_slots[slot].depth;
        if (!texture) return INVALID_TEXTURE;
        TextureHandle& handle = scene ? m_slots[slot].sceneDepthTexture : m_slots[slot].depthTexture;
        auto it = handle != INVALID_TEXTURE ? m_textures.find(handle) : m_textures.end();
        if (it == m_textures.end()) {
            handle = WrapTexture(texture, MTLSamplerMinMagFilterNearest);
            return handle;
        }
        if (it->second.texture != texture) {
            it->second.texture = texture;
            it->second.width = static_cast<int>(texture.width);
            it->second.height = static_cast<int>(texture.height);
        }
        return handle;
    }

    TextureHandle MetalBackend::FrameDepthTexture() {
        // The frame's depth while a render target interrupts a preserved
        // frame that has drawn (VKBackend's m_frameDepthReadable).
        if (!m_frameActive || !m_frameDepthPreserved || !m_frameOpened ||
            m_activeTarget == INVALID_RENDER_TARGET) return INVALID_TEXTURE;
        return FrameDepthTextureFor(m_currentFrame, m_sceneActive);
    }

    TextureHandle MetalBackend::PreviousFrameDepthTexture() {
        // Only before this frame's own pass opens, and only the depth the
        // frame just before drew — the same images (no rebuild), the same
        // frame (the window's or the scaled scene's), the same size.
        if (!m_frameActive || m_frameOpened) return INVALID_TEXTURE;
        const uint32_t prev = (m_currentFrame + kFramesInFlight - 1) % kFramesInFlight;
        const DepthStamp& stamp = m_depthStamps[prev];
        if (stamp.frame == 0 || stamp.frame + 1 != m_frameNumber || stamp.generation != m_frameDepthGeneration ||
            stamp.scene != m_sceneActive || stamp.width != FrameWidth() || stamp.height != FrameHeight()) {
            return INVALID_TEXTURE;
        }
        return FrameDepthTextureFor(prev, stamp.scene);
    }

    // ========================================================================
    // GPU DEBUG GROUPS
    // ========================================================================

    NSString* MetalBackend::DebugGroupName(const char* name) {
        auto it = m_debugGroupNames.find(name);
        if (it != m_debugGroupNames.end()) return it->second;
        NSString* s = [NSString stringWithUTF8String:name];
        m_debugGroupNames[name] = s;
        return s;
    }

    void MetalBackend::PushDebugGroup(const char* name) {
        if (!m_frameActive || !name) return;
        m_debugGroups.push_back(name);
        if (m_encoder) {
            [m_encoder pushDebugGroup:DebugGroupName(name)];
            m_encoderGroups = m_debugGroups.size();
        }
    }

    void MetalBackend::PopDebugGroup() {
        if (!m_frameActive || m_debugGroups.empty()) return;
        if (m_encoder && m_encoderGroups == m_debugGroups.size()) {
            [m_encoder popDebugGroup];
            --m_encoderGroups;
        }
        m_debugGroups.pop_back();
    }

    // GPU frame capture and the GPU timers: MetalInstrumentation.mm.

    // ========================================================================
    // IMGUI
    // ========================================================================

    void MetalBackend::ImGuiInit(GLFWwindow* window) {
        ImGui_ImplGlfw_InitForOther(window, true);
        ImGui_ImplMetal_Init(m_device);
        m_imguiReady = true;
    }

    void MetalBackend::ImGuiNewFrame() {
        if (!m_imguiReady) return;
        @autoreleasepool {
            // The pipeline imgui builds follows the attachments' formats: the
            // frame's (a slot's stand-in and depth stand for the drawable).
            MTLRenderPassDescriptor* d = [MTLRenderPassDescriptor renderPassDescriptor];
            d.colorAttachments[0].texture = m_slots[0].standIn;
            d.depthAttachment.texture = m_slots[0].depth;
            d.stencilAttachment.texture = m_slots[0].depth;
            ImGui_ImplMetal_NewFrame(d);
        }
        ImGui_ImplGlfw_NewFrame();
    }

    void MetalBackend::ImGuiRender() {
        if (!m_imguiReady || !m_frameActive) return;
        ImDrawData* drawData = ImGui::GetDrawData();
        if (!drawData || drawData->CmdListsCount == 0) return;
        if (m_metal4) {
            // imgui_impl_metal records into a Metal 3 render encoder: a
            // command buffer of its own after the frame, over the drawable,
            // which then presents (M4PresentWithOverlay). Only on a frame
            // that is shown.
            if (m_presenting && m_drawable) m_m4.overlayData = drawData;
            return;
        }
        if (m_pass.kind != PassKind::Frame || !EnsureEncoder()) return;
        @autoreleasepool {
            ImGui_ImplMetal_RenderDrawData(drawData, m_cmd, m_encoder);
        }
        // imgui set its own pipeline, buffers, textures and viewport.
        m_cache = EncoderCache::Unknown();
        ApplyViewport();
        ApplyScissor();
    }

    void MetalBackend::ImGuiShutdown() {
        if (!m_imguiReady) return;
        ImGui_ImplMetal_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        m_imguiReady = false;
    }

} // namespace Render

#endif // HAS_METAL

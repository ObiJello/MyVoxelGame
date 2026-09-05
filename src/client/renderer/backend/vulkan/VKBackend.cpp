// File: src/client/renderer/backend/vulkan/VKBackend.cpp
#ifdef HAS_VULKAN

#include <chrono>
#include "VKBackend.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "platform/GameDirectory.hpp"
#include "common/core/Config.hpp"   // GAME_VERSION stamps the pipeline manifest

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>

#include <set>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>

#ifdef __APPLE__
#include <MoltenVK/mvk_private_api.h>
#include <MoltenVK/mvk_deprecated_api.h>
#include <sys/sysctl.h>
#endif

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

namespace Render {

    // Factory function called from GLBackend.cpp
    std::unique_ptr<RenderBackend> CreateVulkanBackend() {
        return std::make_unique<VKBackend>();
    }

    // Required device extensions
    static const std::vector<const char*> s_deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
#ifdef __APPLE__
        "VK_KHR_portability_subset",  // Required for MoltenVK
#endif
    };

    // Validation layers (debug only)
    static const std::vector<const char*> s_validationLayers = {
        "VK_LAYER_KHRONOS_validation"
    };

#ifndef NDEBUG
    static constexpr bool s_enableValidation = true;
#else
    static constexpr bool s_enableValidation = false;
#endif

    // ========================================================================
    // LIFECYCLE
    // ========================================================================

    VKBackend::VKBackend() = default;

    VKBackend::~VKBackend() {
        try {
            Shutdown();
        } catch (...) {
            // Swallow exceptions during static destruction — Vulkan loader
            // or MoltenVK may already be partially unloaded at this point.
        }
    }

    bool VKBackend::Initialize(GLFWwindow* window) {
        Log::Info("VKBackend: Initializing Vulkan backend");
        m_window = window;

#ifdef __APPLE__
        // On Intel Macs, disable Metal heaps and argument buffers via environment
        // variables BEFORE instance creation. MoltenVK reads these during vkCreateInstance()
        // and the deprecated vkSetMoltenVKConfigurationMVK() ignores the VkInstance param,
        // so it cannot update the config after instance creation.
        // Intel Mac Metal drivers (e.g. HD 6000 / Broadwell) crash on MTLHeap validation.
#if defined(__x86_64__) || defined(__i386__)
        {
            int isTranslated = 0;
            size_t sz = sizeof(isTranslated);
            bool isRosetta = (sysctlbyname("sysctl.proc_translated", &isTranslated, &sz, NULL, 0) == 0 && isTranslated);
            if (!isRosetta) {
                setenv("MVK_CONFIG_USE_MTLHEAP", "0", 0);
                setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "0", 0);
                Log::Info("VKBackend: Intel Mac detected, disabled Metal heaps for driver compatibility");
            }
        }
#endif
#endif

        if (!CreateInstance()) return false;
        if (s_enableValidation && !SetupDebugMessenger()) {
            Log::Warning("VKBackend: Debug messenger setup failed, continuing without validation");
        }
        if (!CreateSurface(window)) return false;
        if (!PickPhysicalDevice()) return false;
        if (!CreateLogicalDevice()) return false;
        if (m_multiDrawIndirect && !CreateIndirectRing()) m_multiDrawIndirect = false;
        if (!CreateSwapchain(window)) return false;
        if (!CreateImageViews()) return false;
        if (!CreateDepthResources()) return false;
        if (!CreateRenderPass()) return false;
        if (!CreateFramebuffers()) return false;
        if (!CreateCommandPool()) return false;
        if (!CreateCommandBuffers()) return false;
        if (!CreateSyncObjects()) return false;
        if (!CreateDescriptorPool()) return false;
        if (!CreateDescriptorSetLayout()) return false;
        if (!CreatePipelineLayout()) return false;
        // Portal-feature path: separate descriptor set layout + pipeline
        // layout that adds CommonUBO + BonesUBO descriptors. Non-fatal —
        // if creation fails we lose the portal shaders but block rendering
        // keeps working through the original layouts.
        if (!CreatePortalDescriptorLayout()) {
            Log::Warning("VKBackend: portal descriptor layout create failed — portal-feature shaders will not render");
        }
        if (m_portalDescriptorLayout != VK_NULL_HANDLE && !CreateUniformBlockLayout()) {
            Log::Warning("VKBackend: uniform block layout create failed — terrain will not render");
        }
        if (m_uniformBlockLayout != VK_NULL_HANDLE && !CreateTexelBufferLayout()) {
            Log::Warning("VKBackend: texel buffer layout create failed — merged terrain will not render");
        }
        if (m_portalDescriptorLayout != VK_NULL_HANDLE && !CreatePortalPipelineLayout()) {
            Log::Warning("VKBackend: portal pipeline layout create failed");
        }
        if (m_portalPipelineLayout != VK_NULL_HANDLE && !CreateFrameUBOs()) {
            Log::Warning("VKBackend: per-frame UBO setup failed");
        }
        if (!CreatePipelineCache()) return false;
        CreateTimestampPool();   // non-fatal: leaves pass timers reporting -1

        Log::Info("VKBackend: Vulkan initialization complete");
        return true;
    }

    void VKBackend::Shutdown() {
        if (m_device == VK_NULL_HANDLE) return;

        vkDeviceWaitIdle(m_device);
        // Persist the pipeline cache first: everything below is destruction
        // and none of it can add to the cache.
        SavePipelineCache();
        DestroyIndirectRing();

        // Flush all deferred deletion queues before destroying resources
        for (auto& queue : m_deletionQueues) {
            for (auto& del : queue) {
                if (del.buffer != INVALID_BUFFER) DestroyBuffer(del.buffer);
                if (del.mesh != INVALID_MESH) DestroyMesh(del.mesh);
            }
            queue.clear();
        }

        DestroyTexStaging();
        m_pendingTextureUpdates.clear();

        // Destroy all user resources
        for (auto& [h, mesh] : m_meshes) { /* No VK objects to destroy */ }
        m_meshes.clear();

        for (auto& [h, buf] : m_buffers) {
            if (buf.mapped) vkUnmapMemory(m_device, buf.memory);
            if (buf.buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, buf.buffer, nullptr);
            if (buf.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, buf.memory, nullptr);
        }
        m_buffers.clear();

        for (auto& [h, tex] : m_textures) {
            if (tex.sampler != VK_NULL_HANDLE) vkDestroySampler(m_device, tex.sampler, nullptr);
            if (tex.imageView != VK_NULL_HANDLE) vkDestroyImageView(m_device, tex.imageView, nullptr);
            if (tex.image != VK_NULL_HANDLE) vkDestroyImage(m_device, tex.image, nullptr);
            if (tex.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, tex.memory, nullptr);
        }
        m_textures.clear();

        for (auto& [h, shader] : m_shaders) {
            if (shader.vertModule != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, shader.vertModule, nullptr);
            if (shader.fragModule != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, shader.fragModule, nullptr);
        }
        m_shaders.clear();

        DestroyAllPipelines();

        // Destroy core resources
        if (m_timestampPool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(m_device, m_timestampPool, nullptr);
            m_timestampPool = VK_NULL_HANDLE;
        }
        m_gpuTimers.clear();
        if (m_pipelineCache != VK_NULL_HANDLE) vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);
        // Portal-feature UBO infrastructure (cleaned BEFORE descriptor
        // pool / layouts so descriptor sets referencing the layout are
        // released first).
        DestroyFrameUBOs();
        if (m_portalPipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(m_device, m_portalPipelineLayout, nullptr);
            m_portalPipelineLayout = VK_NULL_HANDLE;
        }
        if (m_portalDescriptorLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(m_device, m_portalDescriptorLayout, nullptr);
            m_portalDescriptorLayout = VK_NULL_HANDLE;
        }
        if (m_uniformBlockLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(m_device, m_uniformBlockLayout, nullptr);
            m_uniformBlockLayout = VK_NULL_HANDLE;
        }
        if (m_texelBufferLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(m_device, m_texelBufferLayout, nullptr);
            m_texelBufferLayout = VK_NULL_HANDLE;
        }
        if (m_pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        if (m_textureDescriptorLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(m_device, m_textureDescriptorLayout, nullptr);
        if (m_descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        if (m_imguiDescriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, m_imguiDescriptorPool, nullptr);

        CleanupSwapchain();
        if (m_renderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(m_device, m_renderPass, nullptr);
            m_renderPass = VK_NULL_HANDLE;
        }

        DestroyRenderFinishedSemaphores();
        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            vkDestroySemaphore(m_device, m_imageAvailableSemaphores[i], nullptr);
            vkDestroyFence(m_device, m_inFlightFences[i], nullptr);
        }

        if (m_commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        vkDestroyDevice(m_device, nullptr);

        if (s_enableValidation && m_debugMessenger != VK_NULL_HANDLE) {
            auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)
                vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT");
            if (func) func(m_instance, m_debugMessenger, nullptr);
        }

        if (m_surface != VK_NULL_HANDLE) vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        vkDestroyInstance(m_instance, nullptr);

        m_device = VK_NULL_HANDLE;
        m_instance = VK_NULL_HANDLE;
        Log::Info("VKBackend: Shutdown complete");
    }

    // ========================================================================
    // FRAME MANAGEMENT
    // ========================================================================

    void VKBackend::BeginFrame() {
        PROFILE_ZONE_N("Vk.BeginFrame");
        m_frameActive = false; // Only set true at end if everything succeeds

        // Wait for the previous frame with this index to finish (1 second timeout).
        //
        // This is where the CPU pays for being ahead of the GPU: with
        // MAX_FRAMES_IN_FLIGHT slots, frame N blocks until frame N-MAX has
        // fully executed. A wide Vk.FenceWait therefore means GPU-bound (or
        // too few frames in flight), NOT that anything here is slow. It is
        // called out as its own zone because it used to be invisible inside
        // the Render phase, which made a GPU-bound frame look like CPU work.
        { PROFILE_ZONE_N("Vk.FenceWait");
        VkResult fenceResult = vkWaitForFences(m_device, 1, &m_inFlightFences[m_currentFrame], VK_TRUE, 1'000'000'000);
        if (fenceResult == VK_TIMEOUT) {
            Log::Error("VKBackend: Fence wait timed out (1s) — possible GPU hang");
            // Reset the fence and try to continue rather than blocking forever
            vkResetFences(m_device, 1, &m_inFlightFences[m_currentFrame]);
        }
        }

        // Flush deferred deletions for this frame slot — GPU is done with these resources
        { PROFILE_ZONE_N("Vk.DeferredDelete");
        for (auto& del : m_deletionQueues[m_currentFrame]) {
            // Queued in dependency order by the caller (a buffer-texture
            // view before its buffer); processed in that order.
            if (del.texture != INVALID_TEXTURE) DestroyTexture(del.texture);
            if (del.buffer != INVALID_BUFFER) DestroyBuffer(del.buffer);
            if (del.mesh != INVALID_MESH) DestroyMesh(del.mesh);
        }
        m_deletionQueues[m_currentFrame].clear();
        }

        // Acquire swapchain image (1 second timeout).
        //
        // Separate zone from the fence wait because the two mean different
        // things: fence = the GPU is behind, acquire = the PRESENTATION ENGINE
        // has no image free (too few swapchain images, or the compositor is
        // holding them). On MoltenVK this is also where a CAMetalLayer
        // nextDrawable stall can surface. Distinguishing them decides whether
        // to cut GPU work or add swapchain images.
        VkResult result;
        { PROFILE_ZONE_N("Vk.Acquire");
        result = vkAcquireNextImageKHR(m_device, m_swapchain, 1'000'000'000,
            m_imageAvailableSemaphores[m_currentFrame], VK_NULL_HANDLE, &m_currentImageIndex);
        }

        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            RecreateSwapchain(m_window);
            return;
        }

        if (result == VK_TIMEOUT) {
            Log::Error("VKBackend: Swapchain image acquire timed out (1s)");
            return;
        }

        vkResetFences(m_device, 1, &m_inFlightFences[m_currentFrame]);
        // Committed to this frame: from here on, queued texture updates belong
        // to the NEXT frame's flush (see m_texStaging).
        ++m_frameNumber;

        // Reset and begin command buffer
        vkResetCommandBuffer(m_commandBuffers[m_currentFrame], 0);
        if (!m_indirectOffset.empty()) m_indirectOffset[static_cast<size_t>(m_currentFrame)] = 0;   // indirect ring: frame's fence passed

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(m_commandBuffers[m_currentFrame], &beginInfo);

        // Begin render pass
        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = m_renderPass;
        renderPassInfo.framebuffer = m_framebuffers[m_currentImageIndex];
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = m_swapchainExtent;

        std::array<VkClearValue, 2> clearValues{};
        clearValues[0].color = m_clearColor;
        clearValues[1].depthStencil = {1.0f, 0};
        renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
        renderPassInfo.pClearValues = clearValues.data();

        // Flush queued texture updates (animated textures, etc.) into the command
        // buffer BEFORE the render pass — transfer ops can't run inside a render pass.
        // This replaces per-update vkQueueWaitIdle with batched pipeline barriers.
        { PROFILE_ZONE_N("Vk.TexFlush");
        FlushPendingTextureUpdates(m_commandBuffers[m_currentFrame]);
        }

        // Reclaim this frame slot's timestamp queries. MUST be outside the
        // render pass (vkCmdResetQueryPool is forbidden inside one), and only
        // this slot's partition may be touched — the other slot's queries can
        // still be in flight, and resetting those would lose their results.
        // The fence wait above guarantees this partition's previous use is done.
        if (m_timestampPool != VK_NULL_HANDLE) {
            const uint32_t base = m_currentFrame * kTimersPerFrame * 2;

            // Every timer still holding a slot in THIS partition is from this
            // slot's previous use, and the fence wait above proves the GPU has
            // finished it — so its queries are readable right now. Resolve them
            // into resultMs BEFORE the reset destroys the queries.
            //
            // Losing a result is not merely a missing number: ChunkRenderer
            // keeps polling the same handle until it returns >= 0 and refuses
            // to start a new timer for that pass until it does, so one dropped
            // result would stop GPU timing for the rest of the session.
            for (auto it = m_gpuTimers.begin(); it != m_gpuTimers.end(); ) {
                if (it->second.frameSlot != m_currentFrame) { ++it; continue; }
                if (it->second.resolved) {
                    // Resolved a full cycle ago and still not collected — the
                    // owner is not polling it, so let it go.
                    it = m_gpuTimers.erase(it);
                    continue;
                }
                if (it->second.ended) {
                    uint64_t stamps[2] = {0, 0};
                    if (vkGetQueryPoolResults(m_device, m_timestampPool, it->second.begin, 2,
                                              sizeof(stamps), stamps, sizeof(uint64_t),
                                              VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                        const uint64_t d = (stamps[1] >= stamps[0]) ? (stamps[1] - stamps[0]) : 0;
                        it->second.resultMs = static_cast<float>(d) * m_timestampPeriodNs / 1.0e6f;
                        it->second.resolved = true;
                        ++it;
                        continue;
                    }
                }
                // Never ended, or unreadable — nothing will ever come of it.
                it = m_gpuTimers.erase(it);
            }

            vkCmdResetQueryPool(m_commandBuffers[m_currentFrame], m_timestampPool,
                                base, kTimersPerFrame * 2);
            m_timersUsedThisFrame = 0;
        }

        vkCmdBeginRenderPass(m_commandBuffers[m_currentFrame], &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        // Reset per-frame state (each command buffer starts with nothing bound)
        m_currentPipeline = VK_NULL_HANDLE;
        m_boundShader = INVALID_SHADER;
        m_boundShaderInfo = nullptr;
        m_boundLayout = VK_NULL_HANDLE;
        m_boundIsPortal = false;
        m_boundTexture = INVALID_TEXTURE;
        ResetRecordedBindings();

        // Reset the per-frame UBO ring write cursor — each frame starts
        // writing into slot 0 of its own ring buffer. (The previous
        // frame may still be reading its ring; that's fine, this frame
        // gets a separate ring entirely.)
        m_frameUBOs[m_currentFrame].commonWriteSlot = 0;
        m_frameUBOs[m_currentFrame].bonesWriteSlot  = 0;
        m_frameUBOs[m_currentFrame].haveCommonSlot  = false;
        m_frameUBOs[m_currentFrame].haveBonesSlot   = false;
        m_frameUBOs[m_currentFrame].exhaustWarned   = false;
        m_frameUBOs[m_currentFrame].bonesWriteSlot  = 0;

        // Set dynamic viewport and scissor
        // Negative height flips Y to match OpenGL convention without affecting winding order
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = static_cast<float>(m_swapchainExtent.height);
        viewport.width = static_cast<float>(m_swapchainExtent.width);
        viewport.height = -static_cast<float>(m_swapchainExtent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(m_commandBuffers[m_currentFrame], 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = m_swapchainExtent;
        vkCmdSetScissor(m_commandBuffers[m_currentFrame], 0, 1, &scissor);

        m_frameActive = true; // All setup succeeded — safe to call EndFrame
    }

    void VKBackend::EndFrame(GLFWwindow* window) {
        PROFILE_ZONE_N("Vk.EndFrame");
        if (!m_frameActive) return; // BeginFrame failed — skip submit/present
        m_frameActive = false;

        // End render pass
        { PROFILE_ZONE_N("Vk.EndCommandBuffer");
        vkCmdEndRenderPass(m_commandBuffers[m_currentFrame]);
        vkEndCommandBuffer(m_commandBuffers[m_currentFrame]);
        }

        // Submit
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

        VkSemaphore waitSemaphores[] = {m_imageAvailableSemaphores[m_currentFrame]};
        VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &m_commandBuffers[m_currentFrame];

        // Indexed by the acquired IMAGE: the present engine keeps waiting on
        // this semaphore until the image is actually shown, which can be after
        // the frame SLOT has been reused when there are more images than slots.
        VkSemaphore signalSemaphores[] = {m_renderFinishedSemaphores[m_currentImageIndex]};
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;

        // On MoltenVK this is where the recorded command buffer is translated
        // and committed to Metal, so it is NOT free — it scales with how many
        // commands the frame recorded, unlike a native Vulkan driver where
        // submit is nearly instant.
        VkResult submitResult;
        { PROFILE_ZONE_N("Vk.QueueSubmit");
        submitResult = vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_inFlightFences[m_currentFrame]);
        }

        // Present
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &m_swapchain;
        presentInfo.pImageIndices = &m_currentImageIndex;

        VkResult result;
        { PROFILE_ZONE_N("Vk.QueuePresent");
        result = vkQueuePresentKHR(m_presentQueue, &presentInfo);
        }
        // A frame that fails here is a picture that never changes while the
        // game keeps running — invisible in the log until now. Logged at
        // most once a second so a persistent failure does not flood it.
        if (submitResult != VK_SUCCESS ||
            (result != VK_SUCCESS && result != VK_ERROR_OUT_OF_DATE_KHR && result != VK_SUBOPTIMAL_KHR)) {
            static auto s_lastLog = std::chrono::steady_clock::now() - std::chrono::seconds(2);
            const auto now = std::chrono::steady_clock::now();
            if (now - s_lastLog >= std::chrono::seconds(1)) {
                s_lastLog = now;
                Log::Error("VKBackend: frame not presented (vkQueueSubmit=%d, vkQueuePresentKHR=%d)",
                           static_cast<int>(submitResult), static_cast<int>(result));
            }
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || m_framebufferResized) {
            m_framebufferResized = false;
            RecreateSwapchain(window);
        }

        m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;

        // Write new pipelines to disk once the burst that produced them has
        // settled (~5 s at 60 fps), so a crash or a kill -9 later in the
        // session does not throw away what this session compiled. Shutdown
        // saves whatever is left.
        if (m_pipelinesSinceSave > 0 && m_frameNumber - m_lastPipelineFrame > 300) {
            SavePipelineCache();
        }
    }

    void VKBackend::SetClearColor(float r, float g, float b, float a) {
        m_clearColor = {{r, g, b, a}};
    }

    void VKBackend::Clear(bool color, bool depth, bool stencil) {
        // The per-attachment clear values in BeginFrame handle the
        // start-of-frame clear. Mid-frame Clear() calls (e.g. the
        // portal renderer's `Clear(false, false, true)` between
        // see-through pass directions, to drop the previous portal's
        // stencil mark before the next portal's mark pass) need
        // vkCmdClearAttachments — a render-pass-local clear that
        // covers the current render area / scissor.
        if (!m_frameActive) return;
        if (!color && !depth && !stencil) return;

        VkClearAttachment clears[2]{};
        uint32_t clearCount = 0;
        if (color) {
            clears[clearCount].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            clears[clearCount].colorAttachment = 0;
            clears[clearCount].clearValue.color = m_clearColor;
            clearCount++;
        }
        if (depth || stencil) {
            VkImageAspectFlags aspect = 0;
            if (depth)   aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
            if (stencil) aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
            clears[clearCount].aspectMask = aspect;
            clears[clearCount].clearValue.depthStencil = {1.0f, 0};
            clearCount++;
        }
        VkClearRect rect{};
        rect.rect.offset = {0, 0};
        rect.rect.extent = m_swapchainExtent;
        rect.baseArrayLayer = 0;
        rect.layerCount = 1;
        vkCmdClearAttachments(m_commandBuffers[m_currentFrame],
                              clearCount, clears, 1, &rect);
    }

    void VKBackend::SetViewport(int x, int y, int width, int height) {
        // Dynamic viewport is set in BeginFrame; this could update for sub-viewports
    }

    void VKBackend::SetScissorRect(int x, int y, int w, int h) {
        if (!m_frameActive) return;
        // Vulkan's scissor origin is top-left, same as the caller's, so no flip.
        // Must stay inside the framebuffer or validation errors out.
        VkRect2D r{};
        r.offset.x = std::max(0, x);
        r.offset.y = std::max(0, y);
        const int maxW = static_cast<int>(m_swapchainExtent.width)  - r.offset.x;
        const int maxH = static_cast<int>(m_swapchainExtent.height) - r.offset.y;
        r.extent.width  = static_cast<uint32_t>(std::clamp(w, 0, std::max(0, maxW)));
        r.extent.height = static_cast<uint32_t>(std::clamp(h, 0, std::max(0, maxH)));
        vkCmdSetScissor(m_commandBuffers[m_currentFrame], 0, 1, &r);
    }

    void VKBackend::ClearScissorRect() {
        if (!m_frameActive) return;
        // Back to the whole framebuffer — matches what BeginFrame installs.
        VkRect2D r{};
        r.offset = {0, 0};
        r.extent = m_swapchainExtent;
        vkCmdSetScissor(m_commandBuffers[m_currentFrame], 0, 1, &r);
    }

    // ========================================================================
    // BUFFERS
    // ========================================================================

    BufferHandle VKBackend::CreateBuffer(BufferUsage usage, size_t size,
                                        const void* data, BufferAccess access) {
        VkBufferUsageFlags vkUsage = 0;
        switch (usage) {
            // Vertex buffers may also be viewed as texel buffers: the terrain
            // mega buffer's face map (CreateBufferTexture) reads the records
            // it stores behind each section's vertices.
            case BufferUsage::Vertex:  vkUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                                 VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT; break;
            case BufferUsage::Index:   vkUsage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT; break;
            case BufferUsage::Uniform: vkUsage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT; break;
            case BufferUsage::Staging: vkUsage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; break;
        }

        if (access == BufferAccess::Static && data != nullptr) {
            // Staging copy + queue drain. Fine at load; a mid-game caller
            // should be using Dynamic (this zone is how a capture finds it).
            PROFILE_ZONE_N("Vk.CreateBuffer.StaticUpload");
            // Use staging buffer for static data
            vkUsage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;

            // Create staging buffer
            VkBuffer stagingBuffer;
            VkDeviceMemory stagingMemory;
            CreateVkBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          stagingBuffer, stagingMemory);

            // Copy data to staging
            void* mapped;
            vkMapMemory(m_device, stagingMemory, 0, size, 0, &mapped);
            std::memcpy(mapped, data, size);
            vkUnmapMemory(m_device, stagingMemory);

            // Create device-local buffer
            VkBuffer deviceBuffer;
            VkDeviceMemory deviceMemory;
            CreateVkBuffer(size, vkUsage,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          deviceBuffer, deviceMemory);

            // Copy staging → device
            CopyBuffer(stagingBuffer, deviceBuffer, size);

            // Cleanup staging
            vkDestroyBuffer(m_device, stagingBuffer, nullptr);
            vkFreeMemory(m_device, stagingMemory, nullptr);

            uint32_t handle = AllocHandle();
            m_buffers[handle] = {deviceBuffer, deviceMemory, size, usage};
            m_memStats.bufferMemory += size;
            m_memStats.totalAllocated += size;
            m_memStats.bufferCount++;
            if (m_memStats.totalAllocated > m_memStats.peakUsage)
                m_memStats.peakUsage = m_memStats.totalAllocated;
            return handle;
        } else {
            // Host-visible buffer (dynamic/streaming), mapped for its whole
            // life. HOST_COHERENT means no vkFlushMappedMemoryRanges is needed
            // after a write; the ordering guarantee comes from the queue
            // submit that follows.
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            if (!CreateVkBuffer(size, vkUsage,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                buffer, memory)) {
                Log::Error("VKBackend: failed to create host-visible buffer (%zu bytes)", size);
                return INVALID_BUFFER;
            }

            void* mapped = nullptr;
            if (vkMapMemory(m_device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
                Log::Error("VKBackend: failed to map host-visible buffer (%zu bytes)", size);
                vkDestroyBuffer(m_device, buffer, nullptr);
                vkFreeMemory(m_device, memory, nullptr);
                return INVALID_BUFFER;
            }
            if (data != nullptr) std::memcpy(mapped, data, size);

            uint32_t handle = AllocHandle();
            m_buffers[handle] = {buffer, memory, size, usage, static_cast<uint8_t*>(mapped)};
            m_memStats.bufferMemory += size;
            m_memStats.totalAllocated += size;
            m_memStats.bufferCount++;
            if (m_memStats.totalAllocated > m_memStats.peakUsage)
                m_memStats.peakUsage = m_memStats.totalAllocated;
            return handle;
        }
    }

    void VKBackend::UpdateBuffer(BufferHandle handle, size_t offset, size_t size, const void* data) {
        auto it = m_buffers.find(handle);
        if (it == m_buffers.end() || size == 0 || data == nullptr) return;
        if (offset + size > it->second.size) {
            Log::Error("VKBackend: UpdateBuffer range [%zu, %zu) exceeds buffer size %zu",
                       offset, offset + size, it->second.size);
            return;
        }

        if (it->second.mapped) {
            // Persistently mapped, host-coherent: the write is the whole job.
            // Same synchronisation contract as before (none) — a caller that
            // rewrites a range a queued frame still reads must version it.
            std::memcpy(it->second.mapped + offset, data, size);
            return;
        }

        // Device-local (Static) buffer. These were never meant to be updated,
        // but the old code mapped them anyway and got away with it on unified
        // memory. Do it properly: stage + copy. This drains the queue, so it is
        // logged once per buffer — a caller hitting it should be using Dynamic.
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            Log::Warning("VKBackend: UpdateBuffer on a Static (device-local) buffer — "
                         "staging copy with queue drain; create it as Dynamic instead");
        }
        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        if (!CreateVkBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            stagingBuffer, stagingMemory)) return;
        void* mapped = nullptr;
        vkMapMemory(m_device, stagingMemory, 0, size, 0, &mapped);
        std::memcpy(mapped, data, size);
        vkUnmapMemory(m_device, stagingMemory);

        VkCommandBuffer cmd = BeginSingleTimeCommands();
        VkBufferCopy region{};
        region.srcOffset = 0;
        region.dstOffset = offset;
        region.size = size;
        vkCmdCopyBuffer(cmd, stagingBuffer, it->second.buffer, 1, &region);
        EndSingleTimeCommands(cmd);

        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
    }

    void VKBackend::UpdateBufferUnsynchronized(BufferHandle handle, size_t offset,
                                               size_t size, const void* data) {
        // On Vulkan there is no synchronised path to opt out of: UpdateBuffer
        // is already a bare memcpy into persistently mapped memory. The
        // override exists so the intent is explicit at the call site and so
        // the GL backend's map-unsynchronized semantics have a Vulkan twin.
        // VK callers must double-buffer by frame themselves (the GUI and the
        // translucency re-sort both do).
        UpdateBuffer(handle, offset, size, data);
    }

    void VKBackend::DestroyBuffer(BufferHandle handle) {
        auto it = m_buffers.find(handle);
        if (it == m_buffers.end()) return;
        if (m_boundUniformBuffer == handle) m_boundUniformBuffer = INVALID_BUFFER;

        // If this buffer is what the active command buffer last bound, forget
        // that: a new buffer could otherwise be handed the same VkBuffer value
        // and the redundancy check would skip a bind it must record.
        for (uint32_t i = 0; i < 2; ++i) {
            if (m_recorded.vertexBuffers[i] == it->second.buffer) m_recorded.vertexCount = 0;
        }
        if (m_recorded.indexBuffer == it->second.buffer) m_recorded.indexBuffer = VK_NULL_HANDLE;

        if (it->second.mapped) vkUnmapMemory(m_device, it->second.memory);
        vkDestroyBuffer(m_device, it->second.buffer, nullptr);
        vkFreeMemory(m_device, it->second.memory, nullptr);
        m_memStats.bufferMemory -= it->second.size;
        m_memStats.totalAllocated -= it->second.size;
        m_memStats.bufferCount--;
        m_buffers.erase(it);
    }

    void VKBackend::DeferredDestroyBuffer(BufferHandle handle) {
        if (handle == INVALID_BUFFER) return;
        m_deletionQueues[m_currentFrame].push_back({handle, INVALID_MESH});
    }

    void VKBackend::DeferredDestroyMesh(MeshHandle handle) {
        if (handle == INVALID_MESH) return;
        m_deletionQueues[m_currentFrame].push_back({INVALID_BUFFER, handle});
    }

    void VKBackend::DeferredDestroyTexture(TextureHandle handle) {
        if (handle == INVALID_TEXTURE) return;
        m_deletionQueues[m_currentFrame].push_back({INVALID_BUFFER, INVALID_MESH, handle});
    }

    // ========================================================================
    // TEXTURES
    // ========================================================================

    TextureHandle VKBackend::CreateTexture2D(int width, int height,
                                            TextureFormat format, const void* data) {
        // Mid-game callers exist (a mob type's texture the first time one
        // is seen, an item icon the first time it is in the hotbar), and
        // each is a GPU drain; the zone is how a capture names them.
        PROFILE_ZONE_N("Vk.CreateTexture2D");
        VkFormat vkFormat = VK_FORMAT_R8G8B8A8_UNORM;
        VkDeviceSize bytesPerPixel = 4;
        if (format == TextureFormat::SRGB8_A8) vkFormat = VK_FORMAT_R8G8B8A8_SRGB;
        // Float data textures (the atlas sprite table is RGBA32F).
        if (format == TextureFormat::RGBA16F) { vkFormat = VK_FORMAT_R16G16B16A16_SFLOAT; bytesPerPixel = 8; }
        if (format == TextureFormat::RGBA32F) { vkFormat = VK_FORMAT_R32G32B32A32_SFLOAT; bytesPerPixel = 16; }

        VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * height * bytesPerPixel;

        // Create staging buffer
        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;
        CreateVkBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      stagingBuffer, stagingMemory);

        if (data) {
            void* mapped;
            vkMapMemory(m_device, stagingMemory, 0, imageSize, 0, &mapped);
            std::memcpy(mapped, data, imageSize);
            vkUnmapMemory(m_device, stagingMemory);
        }

        // Create image
        VkImage image;
        VkDeviceMemory imageMemory;
        CreateVkImage(width, height, 1, vkFormat, VK_IMAGE_TILING_OPTIMAL,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, image, imageMemory);

        // Transition + copy
        // Transition + copy + transition in ONE submit: three separate
        // single-time submits were three full queue drains per texture.
        {
            VkCommandBuffer cmd = BeginSingleTimeCommands();
            RecordImageLayoutTransition(cmd, image, vkFormat, VK_IMAGE_LAYOUT_UNDEFINED,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1);
            RecordCopyBufferToImage(cmd, stagingBuffer, image, width, height);
            RecordImageLayoutTransition(cmd, image, vkFormat, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1);
            EndSingleTimeCommands(cmd);
        }

        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);

        // Create image view
        VkImageView imageView = CreateImageView(image, vkFormat, VK_IMAGE_ASPECT_COLOR_BIT, 1);

        // Create sampler
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_NEAREST;
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.anisotropyEnable = VK_FALSE;
        samplerInfo.maxAnisotropy = 1.0f;
        samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;
        samplerInfo.compareEnable = VK_FALSE;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

        VkSampler sampler;
        vkCreateSampler(m_device, &samplerInfo, nullptr, &sampler);

        // Create descriptor set for this texture
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_descriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_textureDescriptorLayout;

        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(m_device, &allocInfo, &descriptorSet) != VK_SUCCESS) {
            // Pool exhausted: the texture still exists; draws that bind it
            // bail on the null set instead of silently reusing a stale one.
            Log::Error("VKBackend: descriptor pool exhausted — texture created "
                       "without a descriptor set");
            descriptorSet = VK_NULL_HANDLE;
        }

        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = imageView;
        imageInfo.sampler = sampler;

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = descriptorSet;
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(m_device, 1, &descriptorWrite, 0, nullptr);

        uint32_t handle = AllocHandle();
        m_textures[handle] = {image, imageMemory, imageView, sampler, descriptorSet,
                             width, height, 1, static_cast<size_t>(imageSize)};
        // Remembered so ReserveTextureMipLevels can rebuild the image later —
        // Vulkan fixes an image's mip count at creation.
        m_textures[handle].format = vkFormat;

        m_memStats.textureMemory += imageSize;
        m_memStats.totalAllocated += imageSize;
        m_memStats.textureCount++;
        if (m_memStats.totalAllocated > m_memStats.peakUsage)
            m_memStats.peakUsage = m_memStats.totalAllocated;

        return handle;
    }

    void VKBackend::UpdateTexture2D(TextureHandle handle, int x, int y,
                                   int width, int height, const void* data) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end() || !data) return;
        QueueTextureUpdate(it->second.image, 0, x, y, width, height, data);
    }

    void VKBackend::QueueTextureUpdate(VkImage image, uint32_t mipLevel, int x, int y,
                                       int width, int height, const void* data) {
        if (width <= 0 || height <= 0) return;
        const size_t dataSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;

        // The pixels go straight into the staging slot the next BeginFrame
        // will copy from — the only CPU copy they make. See m_texStaging for
        // why writing here, before that frame's fence wait, is safe.
        size_t offset = 0;
        uint8_t* dst = ReserveTexStaging(dataSize, offset);
        if (!dst) return;
        std::memcpy(dst, data, dataSize);

        PendingTextureUpdate update;
        update.image         = image;
        update.x             = x;
        update.y             = y;
        update.width         = width;
        update.height        = height;
        update.mipLevel      = mipLevel;
        update.stagingOffset = offset;
        update.byteSize      = dataSize;
        m_pendingTextureUpdates.push_back(update);
    }

    uint8_t* VKBackend::ReserveTexStaging(size_t bytes, size_t& outOffset) {
        TexStagingSlot& slot = PendingTexStaging();
        const size_t required = slot.used + bytes;
        if (required > slot.capacity) {
            // Grow, preserving what this slot already holds for the upcoming
            // flush. The slot is not in flight (that is the whole point of the
            // ring), so destroying its old buffer here is safe.
            const size_t newCapacity = std::max(required, std::max(slot.capacity * 2, size_t(256 * 1024)));
            VkBuffer newBuffer = VK_NULL_HANDLE;
            VkDeviceMemory newMemory = VK_NULL_HANDLE;
            if (!CreateVkBuffer(newCapacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                newBuffer, newMemory)) {
                Log::Error("VKBackend: failed to grow texture staging to %zu bytes", newCapacity);
                return nullptr;
            }
            void* mapped = nullptr;
            if (vkMapMemory(m_device, newMemory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
                vkDestroyBuffer(m_device, newBuffer, nullptr);
                vkFreeMemory(m_device, newMemory, nullptr);
                return nullptr;
            }
            if (slot.mapped && slot.used > 0) std::memcpy(mapped, slot.mapped, slot.used);
            if (slot.buffer != VK_NULL_HANDLE) {
                vkUnmapMemory(m_device, slot.memory);
                vkDestroyBuffer(m_device, slot.buffer, nullptr);
                vkFreeMemory(m_device, slot.memory, nullptr);
            }
            slot.buffer   = newBuffer;
            slot.memory   = newMemory;
            slot.capacity = newCapacity;
            slot.mapped   = static_cast<uint8_t*>(mapped);
        }
        outOffset = slot.used;
        slot.used += bytes;
        return slot.mapped + outOffset;
    }

    void VKBackend::DestroyTexStaging() {
        for (TexStagingSlot& slot : m_texStaging) {
            if (slot.mapped) vkUnmapMemory(m_device, slot.memory);
            if (slot.buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, slot.buffer, nullptr);
            if (slot.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, slot.memory, nullptr);
            slot = TexStagingSlot{};
        }
    }

    void VKBackend::FlushPendingTextureUpdates(VkCommandBuffer cmd) {
        // BeginFrame advanced m_frameNumber before calling; this is the slot
        // writers were targeting as "pending" until that moment.
        TexStagingSlot& slot = m_texStaging[static_cast<size_t>(m_frameNumber % kTexStagingSlots)];
        if (m_pendingTextureUpdates.empty()) {
            slot.used = 0;
            return;
        }

        // Collect unique images for barrier deduplication (typically just the atlas)
        std::vector<VkImage> uniqueImages;
        for (const auto& update : m_pendingTextureUpdates) {
            bool found = false;
            for (VkImage img : uniqueImages) {
                if (img == update.image) { found = true; break; }
            }
            if (!found) uniqueImages.push_back(update.image);
        }

        // Transition all target images: SHADER_READ_ONLY → TRANSFER_DST (one barrier per image)
        std::vector<VkImageMemoryBarrier> barriers(uniqueImages.size());
        for (size_t i = 0; i < uniqueImages.size(); i++) {
            barriers[i] = {};
            barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barriers[i].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barriers[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[i].image = uniqueImages[i];
            // All levels, not just 0: an animated sprite now uploads its whole
            // mip chain, so every level of the atlas is a transfer target here.
            barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,
                                            0, VK_REMAINING_MIP_LEVELS, 0, 1};
            barriers[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barriers[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        }
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr,
            static_cast<uint32_t>(barriers.size()), barriers.data());

        // Record all buffer-to-image copies
        for (const auto& update : m_pendingTextureUpdates) {
            VkBufferImageCopy region{};
            region.bufferOffset = update.stagingOffset;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel   = update.mipLevel;
            region.imageSubresource.layerCount = 1;
            region.imageOffset = {update.x, update.y, 0};
            region.imageExtent = {static_cast<uint32_t>(update.width),
                                  static_cast<uint32_t>(update.height), 1};
            vkCmdCopyBufferToImage(cmd, slot.buffer, update.image,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }

        // Transition all images back: TRANSFER_DST → SHADER_READ_ONLY
        for (auto& barrier : barriers) {
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        }
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr,
            static_cast<uint32_t>(barriers.size()), barriers.data());

        m_pendingTextureUpdates.clear();
        // The slot's bytes are now owned by this frame's command buffer; the
        // next writer to land on this slot is frame + kTexStagingSlots, which
        // starts from an empty cursor.
        slot.used = 0;
    }

    // Recreate the sampler from the texture's cached state and rewrite the
    // descriptor. Used by SetTextureFilter/SetTextureWrap so each only updates
    // its own state without clobbering the other (calls are typically back-to-
    // back at texture init — see GuiGraphics::LoadGlintTexture).
    void VKBackend::RecreateSamplerFromCache(VkDevice device, VKTextureInfo& tex) {
        VkSamplerCreateInfo info{};
        info.sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter               = tex.magFilter;
        info.minFilter               = tex.minFilter;
        info.addressModeU            = tex.addressModeU;
        info.addressModeV            = tex.addressModeV;
        info.addressModeW            = tex.addressModeV; // mirror V for 2D textures
        info.anisotropyEnable        = VK_FALSE;
        info.maxAnisotropy           = 1.0f;
        info.borderColor             = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        info.unnormalizedCoordinates = VK_FALSE;
        info.compareEnable           = VK_FALSE;
        info.mipmapMode              = tex.mipmapMode;
        // maxLod caps which levels the sampler may reach. It stays 0 for a
        // single-level image, so a texture without a chain keeps sampling
        // level 0 exactly as before.
        info.minLod                  = 0.0f;
        info.maxLod                  = static_cast<float>(tex.mipLevels - 1);

        VkSampler newSampler = VK_NULL_HANDLE;
        if (vkCreateSampler(device, &info, nullptr, &newSampler) != VK_SUCCESS) return;

        VkSampler oldSampler = tex.sampler;
        tex.sampler          = newSampler;

        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView   = tex.imageView;
        imageInfo.sampler     = newSampler;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = tex.descriptorSet;
        write.dstBinding      = 0;
        write.dstArrayElement = 0;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo      = &imageInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

        if (oldSampler != VK_NULL_HANDLE) {
            vkDestroySampler(device, oldSampler, nullptr);
        }
    }

    void VKBackend::SetTextureFilter(TextureHandle handle, TextureFilter min, TextureFilter mag) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end()) return;
        vkDeviceWaitIdle(m_device);
        // Vulkan splits what GL packs into one enum: the *_MIPMAP_* modes carry
        // BOTH the in-level filter and the between-level one. Collapsing them
        // to a bare NEAREST — as this did before — silently dropped every
        // mipmap request, which is why the atlas never got a chain here.
        switch (min) {
            case TextureFilter::NearestMipmapNearest:
                it->second.minFilter  = VK_FILTER_NEAREST;
                it->second.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; break;
            case TextureFilter::NearestMipmapLinear:
                it->second.minFilter  = VK_FILTER_NEAREST;
                it->second.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;  break;
            case TextureFilter::LinearMipmapNearest:
                it->second.minFilter  = VK_FILTER_LINEAR;
                it->second.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; break;
            case TextureFilter::LinearMipmapLinear:
                it->second.minFilter  = VK_FILTER_LINEAR;
                it->second.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;  break;
            case TextureFilter::Linear:
                it->second.minFilter  = VK_FILTER_LINEAR;
                it->second.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; break;
            default:
                it->second.minFilter  = VK_FILTER_NEAREST;
                it->second.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; break;
        }
        it->second.magFilter = (mag == TextureFilter::Linear) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        RecreateSamplerFromCache(m_device, it->second);
    }

    void VKBackend::SetTextureWrap(TextureHandle handle, TextureWrap s, TextureWrap t) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end()) return;
        vkDeviceWaitIdle(m_device);
        it->second.addressModeU = ToVkWrap(s);
        it->second.addressModeV = ToVkWrap(t);
        RecreateSamplerFromCache(m_device, it->second);
    }

    void VKBackend::GenerateMipmaps(TextureHandle /*handle*/) {
        // Intentionally unimplemented. The block atlas — the only texture whose
        // mips affect terrain — authors its chain on the CPU with MC's
        // algorithm and pushes it through ReserveTextureMipLevels +
        // UploadTextureMipLevel, so a vkCmdBlitImage chain here would be both
        // unused and wrong for cutout sprites (see texture/MipmapGenerator.hpp).
    }

    void VKBackend::ReserveTextureMipLevels(TextureHandle handle, int maxLevel) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end()) return;

        VKTextureInfo& tex = it->second;
        const uint32_t wanted = static_cast<uint32_t>(std::max(0, maxLevel)) + 1u;
        if (wanted == tex.mipLevels) return;

        // An image's mip count is baked in at vkCreateImage, so the only way to
        // change it is to build a new image. Contents are discarded — the
        // interface documents that every level is uploaded after reserving,
        // which is what AtlasBuilder does.
        vkDeviceWaitIdle(m_device);

        VkImage        newImage  = VK_NULL_HANDLE;
        VkDeviceMemory newMemory = VK_NULL_HANDLE;
        if (!CreateVkImage(static_cast<uint32_t>(tex.width), static_cast<uint32_t>(tex.height),
                           wanted, tex.format, VK_IMAGE_TILING_OPTIMAL,
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, newImage, newMemory)) {
            Log::Error("Vulkan: failed to reallocate texture for %u mip levels", wanted);
            return;
        }

        // Park every level in SHADER_READ_ONLY straight away. Levels that have
        // not been uploaded yet hold undefined data, but sampling them is legal
        // and the caller overwrites them immediately; leaving them UNDEFINED
        // would make the layout wrong instead, which is not.
        TransitionImageLayout(newImage, tex.format, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, wanted);

        VkImageView newView = CreateImageView(newImage, tex.format,
                                              VK_IMAGE_ASPECT_COLOR_BIT, wanted);
        if (newView == VK_NULL_HANDLE) {
            vkDestroyImage(m_device, newImage, nullptr);
            vkFreeMemory(m_device, newMemory, nullptr);
            Log::Error("Vulkan: failed to create mipped image view");
            return;
        }

        // Anything queued against the old image would write into freed memory.
        m_pendingTextureUpdates.erase(
            std::remove_if(m_pendingTextureUpdates.begin(), m_pendingTextureUpdates.end(),
                           [&](const PendingTextureUpdate& u) { return u.image == tex.image; }),
            m_pendingTextureUpdates.end());

        if (tex.imageView != VK_NULL_HANDLE) vkDestroyImageView(m_device, tex.imageView, nullptr);
        if (tex.image != VK_NULL_HANDLE)     vkDestroyImage(m_device, tex.image, nullptr);
        if (tex.memory != VK_NULL_HANDLE)    vkFreeMemory(m_device, tex.memory, nullptr);

        tex.image     = newImage;
        tex.memory    = newMemory;
        tex.imageView = newView;
        tex.mipLevels = wanted;

        // Rebuilds the sampler with the new maxLod AND rewrites the descriptor
        // to point at the new view — both are required, the descriptor most of
        // all, since it still referenced the destroyed one.
        RecreateSamplerFromCache(m_device, tex);

        // Sum the levels actually allocated rather than assuming a full chain —
        // a partial chain is legal and the stats panel should not overstate it.
        size_t newSize = 0;
        for (uint32_t lvl = 0; lvl < wanted; ++lvl) {
            const size_t lw = std::max<size_t>(1u, static_cast<size_t>(tex.width)  >> lvl);
            const size_t lh = std::max<size_t>(1u, static_cast<size_t>(tex.height) >> lvl);
            newSize += lw * lh * 4u;
        }
        m_memStats.textureMemory  = m_memStats.textureMemory  - tex.memorySize + newSize;
        m_memStats.totalAllocated = m_memStats.totalAllocated - tex.memorySize + newSize;
        if (m_memStats.totalAllocated > m_memStats.peakUsage)
            m_memStats.peakUsage = m_memStats.totalAllocated;
        tex.memorySize = newSize;
    }

    void VKBackend::UploadTextureMipLevel(TextureHandle handle, int level,
                                          int width, int height, const void* data) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end() || !data) return;
        VKTextureInfo& tex = it->second;
        if (level < 0 || static_cast<uint32_t>(level) >= tex.mipLevels) return;
        if (width <= 0 || height <= 0) return;

        const VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * height * 4;

        VkBuffer       staging       = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        CreateVkBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       staging, stagingMemory);

        void* mapped = nullptr;
        vkMapMemory(m_device, stagingMemory, 0, imageSize, 0, &mapped);
        std::memcpy(mapped, data, static_cast<size_t>(imageSize));
        vkUnmapMemory(m_device, stagingMemory);

        // One submit for transition + copy + transition. This runs at load time
        // (and on the debug UI's mipmap toggle), never per frame, so a
        // single-time command buffer is the right tool.
        VkCommandBuffer cmd = BeginSingleTimeCommands();

        VkImageMemoryBarrier barrier{};
        barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image               = tex.image;
        barrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT,
                                        static_cast<uint32_t>(level), 1, 0, 1 };

        barrier.oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel   = static_cast<uint32_t>(level);
        region.imageSubresource.layerCount = 1;
        region.imageExtent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
        vkCmdCopyBufferToImage(cmd, staging, tex.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);

        EndSingleTimeCommands(cmd);

        vkDestroyBuffer(m_device, staging, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
    }

    void VKBackend::UpdateTexture2DLevel(TextureHandle handle, int level, int x, int y,
                                         int width, int height, const void* data) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end() || !data) return;
        if (level < 0 || static_cast<uint32_t>(level) >= it->second.mipLevels) return;

        // Same deferred path as UpdateTexture2D: animated sprites call this
        // mid-frame, so the copy has to ride the frame's command buffer rather
        // than stall the queue.
        QueueTextureUpdate(it->second.image, static_cast<uint32_t>(level), x, y, width, height, data);
    }

    void VKBackend::DestroyTexture(TextureHandle handle) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end()) return;

        vkDeviceWaitIdle(m_device);

        // Drop anything still queued against this image — the next flush would
        // otherwise record a copy into freed memory. Reachable via
        // RebuildAtlas, which destroys the atlas mid-session, and now more
        // easily so: an animated sprite queues one update per mip level.
        m_pendingTextureUpdates.erase(
            std::remove_if(m_pendingTextureUpdates.begin(), m_pendingTextureUpdates.end(),
                           [&](const PendingTextureUpdate& u) { return u.image == it->second.image; }),
            m_pendingTextureUpdates.end());
        if (it->second.bufferView != VK_NULL_HANDLE) {
            vkDestroyBufferView(m_device, it->second.bufferView, nullptr);
        }
        if (it->second.sampler != VK_NULL_HANDLE) vkDestroySampler(m_device, it->second.sampler, nullptr);
        if (it->second.imageView != VK_NULL_HANDLE) vkDestroyImageView(m_device, it->second.imageView, nullptr);
        if (it->second.image != VK_NULL_HANDLE) vkDestroyImage(m_device, it->second.image, nullptr);
        if (it->second.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, it->second.memory, nullptr);

        m_memStats.textureMemory -= it->second.memorySize;
        m_memStats.totalAllocated -= it->second.memorySize;
        m_memStats.textureCount--;
        m_textures.erase(it);
    }

    void VKBackend::BindTexture(TextureHandle handle, uint32_t slot) {
        if (slot < kMaxTextureSlots) {
            m_boundTextures[slot] = handle;
        }
        // Maintain the legacy single-texture alias for any code path that
        // still reads m_boundTexture directly (block draw paths bind the
        // texture's per-texture descriptor set using this).
        if (slot == 0) m_boundTexture = handle;
    }

    uintptr_t VKBackend::GetNativeTextureID(TextureHandle handle) const {
        auto it = m_textures.find(handle);
        if (it != m_textures.end() && it->second.descriptorSet != VK_NULL_HANDLE) {
            return reinterpret_cast<uintptr_t>(it->second.descriptorSet);
        }
        return 0;
    }

    // ========================================================================
    // SHADERS
    // ========================================================================

    ShaderHandle VKBackend::CreateShader(const std::string& vertexSource,
                                        const std::string& fragmentSource) {
        Log::Error("VKBackend: CreateShader from GLSL source not supported - use SPIR-V files");
        return INVALID_SHADER;
    }

    const void* VKBackend::DebugGetMappedBufferPtr(BufferHandle handle) const {
        auto it = m_buffers.find(handle);
        return it != m_buffers.end() ? it->second.mapped : nullptr;
    }

    ShaderHandle VKBackend::CreateShaderFromFiles(const std::string& vertexPath,
                                                  const std::string& fragmentPath) {
        // For Vulkan, expect .spv files; derive path from GLSL path
        std::string vertSpvPath = vertexPath;
        std::string fragSpvPath = fragmentPath;

        // If paths end with .vert/.frag, look for _vk.vert.spv/_vk.frag.spv
        if (vertSpvPath.find(".vert") != std::string::npos && vertSpvPath.find(".spv") == std::string::npos) {
            // Replace "block.vert" with "block_vk.vert.spv"
            auto pos = vertSpvPath.rfind(".vert");
            vertSpvPath = vertSpvPath.substr(0, pos) + "_vk.vert.spv";
        }
        if (fragSpvPath.find(".frag") != std::string::npos && fragSpvPath.find(".spv") == std::string::npos) {
            auto pos = fragSpvPath.rfind(".frag");
            fragSpvPath = fragSpvPath.substr(0, pos) + "_vk.frag.spv";
        }

        auto vertCode = ReadBinaryFile(vertSpvPath);
        auto fragCode = ReadBinaryFile(fragSpvPath);
        if (vertCode.empty() || fragCode.empty()) {
            Log::Error("VKBackend: Failed to load SPIR-V shaders: %s, %s",
                      vertSpvPath.c_str(), fragSpvPath.c_str());
            return INVALID_SHADER;
        }

        VkShaderModule vertModule = CreateShaderModule(vertCode);
        VkShaderModule fragModule = CreateShaderModule(fragCode);

        if (vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE) {
            if (vertModule != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, vertModule, nullptr);
            if (fragModule != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, fragModule, nullptr);
            return INVALID_SHADER;
        }

        uint32_t handle = AllocHandle();
        m_shaders[handle] = {vertModule, fragModule};
        m_shaders[handle].vertPath = vertexPath;
        m_shaders[handle].fragPath = fragmentPath;
        m_memStats.shaderCount++;
        Log::Info("VKBackend: Loaded SPIR-V shaders: %s + %s", vertSpvPath.c_str(), fragSpvPath.c_str());
        return handle;
    }

    void VKBackend::DestroyShader(ShaderHandle handle) {
        auto it = m_shaders.find(handle);
        if (it == m_shaders.end()) return;
        if (m_boundShader == handle) {
            m_boundShader     = INVALID_SHADER;
            m_boundShaderInfo = nullptr;   // about to dangle
            m_boundLayout     = VK_NULL_HANDLE;
            m_boundIsPortal   = false;
        }
        // Pipelines built from these modules can no longer be rebuilt after a
        // swapchain recreate, and they are dead weight now anyway.
        vkDeviceWaitIdle(m_device);
        for (auto pit = m_pipelines.begin(); pit != m_pipelines.end(); ) {
            if (pit->second.shader == handle) {
                vkDestroyPipeline(m_device, pit->second.pipeline, nullptr);
                if (m_currentPipeline == pit->second.pipeline) m_currentPipeline = VK_NULL_HANDLE;
                pit = m_pipelines.erase(pit);
            } else {
                ++pit;
            }
        }
        vkDestroyShaderModule(m_device, it->second.vertModule, nullptr);
        vkDestroyShaderModule(m_device, it->second.fragModule, nullptr);
        m_memStats.shaderCount--;
        m_shaders.erase(it);
    }

    void VKBackend::BindShader(ShaderHandle handle) {
        m_boundShader = handle;
        // Resolve the layout here, once, instead of in every draw path.
        auto it = m_shaders.find(handle);
        m_boundShaderInfo = (it != m_shaders.end()) ? &it->second : nullptr;
        m_boundIsPortal   = m_boundShaderInfo && m_boundShaderInfo->layoutType == 1 &&
                            m_portalPipelineLayout != VK_NULL_HANDLE;
        m_boundLayout     = m_boundIsPortal ? m_portalPipelineLayout : m_pipelineLayout;
    }

    // Uniform names are matched by string on every call, from every draw of
    // every subsystem. std::string == const char* costs a strlen plus a
    // memcmp per candidate, and SetUniformFloat walks up to twenty of them.
    // With the literal's length known at compile time, a candidate is
    // rejected on size, then on its LAST character (every name starts with
    // 'u', so the first is useless), before any memcmp runs.
    template <size_t N>
    static inline bool NameIs(const std::string& name, const char (&lit)[N]) {
        constexpr size_t len = N - 1;
        return name.size() == len && name[len - 1] == lit[len - 1] &&
               std::memcmp(name.data(), lit, len) == 0;
    }

    // ----------------------------------------------------------------
    // Uniform setters — write to BOTH push constants (for block-style
    // shaders) AND the CommonUBO / BonesUBO (for portal-feature
    // shaders). The push constants get sent fresh every draw, so the
    // double-write costs nothing meaningful and lets a single C++
    // setter feed either shader path transparently.
    // ----------------------------------------------------------------
    // GL→Vulkan depth-range conversion matrix. glm::perspective produces
    // a GL-style matrix where the near plane maps to NDC z = -1; Vulkan's
    // near plane is z = 0 and the rasterizer clips anything with z_ndc < 0.
    // Without this premultiplication, half the depth range falls into the
    // Vulkan clipped half-space. For normal scenes you barely notice
    // (geometry is mostly distant, sitting in z_ndc > 0), but the portal
    // renderer's oblique projection explicitly anchors geometry to the
    // GL near plane (z_ndc = -1) so that vertices on the destination
    // portal's clip plane land exactly on the near plane in OpenGL. In
    // Vulkan those vertices end up at z_ndc = -1 (clipped) instead of 0
    // (kept). Visible symptom: the see-through view goes blank at steep
    // angles / distance where more dst-world geometry sits near the
    // oblique clip plane. This matrix premultiplies every uMVP coming
    // through SetUniformMat4 to map z_ndc [-1, +1] → [0, +1]:
    //   z_clip_new = 0.5·z_clip_old + 0.5·w  →  z_ndc_new = 0.5·z_ndc_old + 0.5
    // — so z_ndc_old = -1 → 0 (Vk near), z_ndc_old = +1 → 1 (Vk far).
    // Applied only on Vulkan; OpenGL keeps its native GL-style matrix.
    static const glm::mat4 kVkZCorrect = glm::mat4(
        1.0f, 0.0f, 0.0f, 0.0f,   // col 0
        0.0f, 1.0f, 0.0f, 0.0f,   // col 1
        0.0f, 0.0f, 0.5f, 0.0f,   // col 2 — z_clip *= 0.5
        0.0f, 0.0f, 0.5f, 1.0f);  // col 3 — z_clip += 0.5 * w

    void VKBackend::SetUniformMat4(ShaderHandle handle, const std::string& name,
                                   const glm::mat4& value) {
        if (NameIs(name, "uMVP") || NameIs(name, "uViewProj")) {
            // "uViewProj" is the instanced block shader's name for the same
            // push-constant slot — its model matrix arrives per instance.
            const glm::mat4 vkMVP = kVkZCorrect * value;
            m_pushConstants.uMVP    = vkMVP;
            m_commonUBOData.uMVP    = vkMVP;
            m_commonUBODirty = true;
        } else if (NameIs(name, "uModel")) {
            m_commonUBOData.uModel  = value;
            m_commonUBODirty = true;
        } else if (name.rfind("uBones[", 0) == 0) {
            // "uBones[N]" — parse N, write into BonesUBO.
            const size_t lbracket = 7;            // length of "uBones["
            const size_t rbracket = name.find(']', lbracket);
            if (rbracket != std::string::npos) {
                int idx = std::atoi(name.c_str() + lbracket);
                if (idx >= 0 && idx < kMaxBones) {
                    m_bonesUBOData.bones[idx] = value;
                    m_bonesUBODirty = true;
                }
            }
        }
    }

    void VKBackend::SetUniformVec4(ShaderHandle, const std::string& name, const glm::vec4& value) {
        if (NameIs(name, "uTint") || NameIs(name, "uColor") || NameIs(name, "uClipPlane") ||
            NameIs(name, "uPortalClipPlane")) {
            // uClipPlane (PlayerRenderer's portal-ghost half-space cull)
            // AND uPortalClipPlane (block shaders' world-space portal
            // plane → gl_ClipDistance[0]) are aliased onto the same
            // push-constant slot as uColor — no shader currently needs
            // both a tint and a clip plane simultaneously, and packing
            // them here keeps the chunk + player shaders from needing
            // a UBO descriptor for one tiny vec4.
            m_pushConstants.uColor  = value;
            m_commonUBOData.uTint   = value;
            m_commonUBODirty = true;
        } else if (NameIs(name, "uEntityClipPlane")) {
            // The entity shaders' portal clip plane. Their uColor slot is the
            // hurt overlay, so the plane rides in the uUVRange slot, which
            // they do not otherwise use (entity_vk.vert).
            m_pushConstants.uUVRange = value;
        } else if (NameIs(name, "uFogColor")) {
            // Environment fog block (sky/clouds/chunk shaders) — dedicated
            // CommonUBO fields, NOT aliased onto uTint: block shaders need
            // uPortalClipPlane (which lives in the uColor/uTint slot) and
            // fog uniforms simultaneously.
            m_commonUBOData.uFogColor = value;
            m_commonUBODirty = true;
        } else if (NameIs(name, "uFogEnv")) {
            m_commonUBOData.uFogEnv = value;
            m_commonUBODirty = true;
        } else if (NameIs(name, "uOverlayColor")) {
            // MC's entity overlay — the primed-TNT white flash. Its own UBO
            // field rather than an alias onto uTint, because the block shaders
            // need uPortalClipPlane (which lives in the uColor/uTint slot) at
            // the same time.
            m_commonUBOData.uOverlayColor = value;
            m_commonUBODirty = true;
        }
    }
    void VKBackend::SetUniformVec3(ShaderHandle, const std::string& name, const glm::vec3& value) {
        if (NameIs(name, "uPortalColor")) {
            m_pushConstants.uColor          = glm::vec4(value, m_pushConstants.uColor.a);
            m_commonUBOData.uPortalColor    = glm::vec4(value, m_commonUBOData.uPortalColor.a);
            m_commonUBODirty = true;
        } else if (NameIs(name, "uColorDark")) {
            m_commonUBOData.uColorDark      = glm::vec4(value, m_commonUBOData.uColorDark.a);
            m_commonUBODirty = true;
        } else if (NameIs(name, "uColorHot")) {
            m_commonUBOData.uColorHot       = glm::vec4(value, m_commonUBOData.uColorHot.a);
            m_commonUBODirty = true;
        } else if (NameIs(name, "uKeyDir")) {
            m_commonUBOData.uKeyDir         = glm::vec4(value, m_commonUBOData.uKeyDir.a);
            m_commonUBODirty = true;
        } else if (NameIs(name, "uTint") || NameIs(name, "uColor")) {
            m_pushConstants.uColor          = glm::vec4(value, m_pushConstants.uColor.a);
            m_commonUBOData.uTint           = glm::vec4(value, m_commonUBOData.uTint.a);
            m_commonUBODirty = true;
        } else if (NameIs(name, "uCameraPos")) {
            m_commonUBOData.uCamPosBright   = glm::vec4(value, m_commonUBOData.uCamPosBright.w);
            m_commonUBODirty = true;
        } else if (NameIs(name, "uFogColor")) {
            m_commonUBOData.uFogColor       = glm::vec4(value, m_commonUBOData.uFogColor.a);
            m_commonUBODirty = true;
        }
    }
    void VKBackend::SetUniformVec2(ShaderHandle, const std::string& name, const glm::vec2& value) {
        if (NameIs(name, "uScreenSize")) {
            m_pushConstants.uScreenSize     = value;
            m_commonUBOData.uScreenSize     = value;
            m_commonUBODirty = true;
        } else if (NameIs(name, "uUVMin")) {
            m_pushConstants.uUVRange.x = value.x; m_pushConstants.uUVRange.y = value.y;
            m_commonUBOData.uUVRange.x = value.x; m_commonUBOData.uUVRange.y = value.y;
            m_commonUBODirty = true;
        } else if (NameIs(name, "uUVMax")) {
            m_pushConstants.uUVRange.z = value.x; m_pushConstants.uUVRange.w = value.y;
            m_commonUBOData.uUVRange.z = value.x; m_commonUBOData.uUVRange.w = value.y;
            m_commonUBODirty = true;
        }
    }
    void VKBackend::SetUniformFloat(ShaderHandle, const std::string& name, float value) {
        // Push-constant block-style aliases:
        if (NameIs(name, "uLineWidth"))           { m_pushConstants.uLineWidth = value; }
        else if (NameIs(name, "uAlphaTest"))      { m_pushConstants.uAlphaTest = value; }
        // Portal renderer + crosshair scalar packing:
        else if (NameIs(name, "uPulse"))          { m_pushConstants.uScalars.x = value; m_commonUBOData.uPortalColor.a = value; m_commonUBODirty = true; }
        else if (NameIs(name, "uOpenAmount"))     { m_pushConstants.uScalars.z = value; m_commonUBOData.uColorDark.a    = value; m_commonUBODirty = true; }
        else if (NameIs(name, "uOpenAmountVS"))   { m_commonUBOData.uColorHot.a    = value; m_commonUBODirty = true; }
        else if (NameIs(name, "uKeyIntensity"))   { m_commonUBOData.uKeyDir.a      = value; m_commonUBODirty = true; }
        else if (NameIs(name, "uTime"))           { m_pushConstants.uScalars.y = value; m_commonUBOData.uScalarsA.x = value; m_commonUBODirty = true; }
        else if (NameIs(name, "uTimeVS"))         { m_commonUBOData.uScalarsA.y = value; m_commonUBODirty = true; }
        else if (NameIs(name, "uStaticAmount"))   { m_commonUBOData.uScalarsA.z = value; m_commonUBODirty = true; }
        else if (NameIs(name, "uColorScale"))     { m_commonUBOData.uScalarsA.w = value; m_commonUBODirty = true; }
        else if (NameIs(name, "uPortalActive"))   { m_commonUBOData.uScalarsB.x = value; m_commonUBODirty = true; }
        else if (NameIs(name, "uForceFarDepth"))  { m_commonUBOData.uScalarsB.y = value; m_commonUBODirty = true; }
        else if (NameIs(name, "uOutlineMode"))    { m_commonUBOData.uScalarsB.z = value; m_commonUBODirty = true; }
        else if (NameIs(name, "uFlashIntensity")) { m_pushConstants.uScalars.w = value; m_commonUBOData.uScalarsB.w = value; m_commonUBODirty = true; }
        else if (NameIs(name, "uAmbient"))        { m_commonUBOData.uScalarsC.x = value; m_commonUBODirty = true; }
        else if (NameIs(name, "uAlphaCutoff"))    { m_commonUBOData.uScalarsC.y = value; m_commonUBODirty = true; }
        else if (NameIs(name, "uHasSprite"))      { m_commonUBOData.uScalarsD.x = value; m_commonUBODirty = true; }
        else if (NameIs(name, "uUseSkin"))        { m_commonUBOData.uScalarsD.y = value; m_commonUBODirty = true; }
        else if (NameIs(name, "uUseTextures"))    { m_commonUBOData.uScalarsD.z = value; m_commonUBODirty = true; }
        else if (NameIs(name, "uSkyBrightness"))  { m_commonUBOData.uCamPosBright.w = value; m_commonUBODirty = true; }
    }
    void VKBackend::SetUniformInt(ShaderHandle, const std::string& name, int value) {
        // Texture-sampler bindings come through as integers (legacy
        // GL pattern). Vulkan binds textures via descriptor sets, so
        // we ignore those names. Other ints fall through into the
        // float path's uUseTextures slot etc.
        if (NameIs(name, "uUseTextures")) {
            m_commonUBOData.uScalarsD.z = (float)value;
            m_commonUBODirty = true;
        } else if (NameIs(name, "uHasSprite")) {
            // PortalParticleSystem uses uHasSprite to select between the
            // Portal-extracted sprite texture path and the procedural
            // soft-disc fallback. Routed into BOTH the push-constant
            // uScalars.x (where portal_particle_vk reads it) and the
            // CommonUBO slot (so any portal-layout shader that wants
            // it also sees it).
            m_pushConstants.uScalars.x        = (float)value;
            m_commonUBOData.uScalarsD.x       = (float)value;
            m_commonUBODirty = true;
        }
        // "uSprite" and other sampler-name ints are no-ops on Vulkan —
        // textures bind via descriptor sets, not via uniform int slot.
    }

    // ========================================================================
    // MESHES
    // ========================================================================

    MeshHandle VKBackend::CreateInstancedMesh(BufferHandle vertexBuffer, BufferHandle indexBuffer,
                                              BufferHandle instanceBuffer,
                                              const VertexLayout& vertexLayout,
                                              const VertexLayout& instanceLayout) {
        if (vertexBuffer == INVALID_BUFFER || instanceBuffer == INVALID_BUFFER) return INVALID_MESH;
        uint32_t handle = AllocHandle();
        VKMeshInfo info;
        info.vertexBuffer   = vertexBuffer;
        info.indexBuffer    = indexBuffer;
        info.layout         = vertexLayout;
        info.instanceBuffer = instanceBuffer;
        info.instanceLayout = instanceLayout;
        m_meshes[handle] = std::move(info);
        m_memStats.meshCount++;
        return handle;
    }

    void VKBackend::DrawIndexedInstanced(MeshHandle mesh, uint32_t indexCount,
                                         uint32_t indexOffset, uint32_t instanceCount,
                                         uint32_t instanceByteOffset) {
        // DrawIndexed's body with two vertex bindings and an instance count.
        // The mass-detonation path lives here: on Vulkan the per-entity
        // fallback was tens of thousands of vkCmdDrawIndexed per frame, which
        // is what froze the render thread while the server kept running.
        if (mesh == INVALID_MESH || m_boundShader == INVALID_SHADER ||
            indexCount == 0 || instanceCount == 0) return;

        auto meshIt = m_meshes.find(mesh);
        if (meshIt == m_meshes.end()) return;

        auto vbIt   = m_buffers.find(meshIt->second.vertexBuffer);
        auto ibIt   = m_buffers.find(meshIt->second.indexBuffer);
        auto instIt = m_buffers.find(meshIt->second.instanceBuffer);
        if (vbIt == m_buffers.end() || ibIt == m_buffers.end() ||
            instIt == m_buffers.end()) return;

        VkCommandBuffer cmd = m_commandBuffers[m_currentFrame];
        if (!PrepareDraw(cmd)) return;

        VkBuffer     vertexBuffers[2] = { vbIt->second.buffer, instIt->second.buffer };
        VkDeviceSize offsets[2]       = { 0, instanceByteOffset };
        BindVertexBuffersCached(cmd, 2, vertexBuffers, offsets);
        BindIndexBufferCached(cmd, ibIt->second.buffer, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, indexCount, instanceCount, indexOffset, 0, 0);
    }

    MeshHandle VKBackend::CreateMesh(BufferHandle vertexBuffer, BufferHandle indexBuffer,
                                    const VertexLayout& layout) {
        uint32_t handle = AllocHandle();
        m_meshes[handle] = {vertexBuffer, indexBuffer, layout};
        m_memStats.meshCount++;
        return handle;
    }

    void VKBackend::DestroyMesh(MeshHandle handle) {
        m_meshes.erase(handle);
        m_memStats.meshCount--;
    }

    // ========================================================================
    // PIPELINE STATE
    // ========================================================================

    void VKBackend::SetStencilOverride(bool enabled,
                                       CompareOp compareOp,
                                       StencilOp passOp,
                                       uint32_t  reference,
                                       uint32_t  readMask,
                                       uint32_t  writeMask) {
        m_stencilOverride = {enabled, compareOp, passOp, reference, readMask, writeMask};
    }

    void VKBackend::SetPipelineState(const PipelineState& state_) {
        // Splice in the portal renderer's stencil override (if active)
        // — same logic the GL backend has. Without this, chunks
        // rendering during a portal see-through pass make pipelines
        // with stencilTestEnabled=false, so the silhouette mask we
        // wrote to stencil is ignored and the destination view paints
        // EVERYWHERE on the screen instead of only inside the portal.
        PipelineState state = state_;
        if (m_stencilOverride.enabled) {
            state.stencilTestEnabled = true;
            state.stencilCompareOp   = m_stencilOverride.compareOp;
            state.stencilFailOp      = StencilOp::Keep;
            state.stencilDepthFailOp = StencilOp::Keep;
            state.stencilPassOp      = m_stencilOverride.passOp;
            state.stencilReference   = m_stencilOverride.reference;
            state.stencilReadMask    = m_stencilOverride.readMask;
            state.stencilWriteMask   = m_stencilOverride.writeMask;
        }
        // A mirrored view flips every triangle's screen winding. Inverting
        // by flipping the FRONT-FACE RULE (not by swapping the cull mode)
        // culls the same triangles and, unlike the swap, keeps
        // gl_FrontFacing meaning the geometric front — the two-sided plant
        // quads (TerrainVertex::kTwoSidedFlag) mirror their texture on the
        // geometric back and must see the same answer in a mirror.
        if (m_cullInvert) {
            state.frontFace = (state.frontFace == FrontFace::CounterClockwise)
                                  ? FrontFace::Clockwise : FrontFace::CounterClockwise;
        }
        m_currentPipelineState = state;
    }

    void VKBackend::ApplyDynamicStencilState(VkCommandBuffer cmd) const {
        if (!m_currentPipelineState.stencilTestEnabled) return;
        // Both faces use the same values — matches the symmetric front/back
        // setup in CreateGraphicsPipeline.
        constexpr VkStencilFaceFlags faces = VK_STENCIL_FACE_FRONT_AND_BACK;
        vkCmdSetStencilReference (cmd, faces, m_currentPipelineState.stencilReference);
        vkCmdSetStencilCompareMask(cmd, faces, m_currentPipelineState.stencilReadMask);
        vkCmdSetStencilWriteMask  (cmd, faces, m_currentPipelineState.stencilWriteMask);
    }

    void VKBackend::InvalidateStateCache() {
        // Vulkan rebuilds pipeline state each draw, no cache to invalidate
    }

    // ========================================================================
    // DRAWING
    // ========================================================================

    void VKBackend::DrawIndexed(MeshHandle mesh, uint32_t indexCount, uint32_t indexOffset) {
        if (mesh == INVALID_MESH || m_boundShader == INVALID_SHADER || indexCount == 0) return;

        auto meshIt = m_meshes.find(mesh);
        if (meshIt == m_meshes.end()) return;

        auto vbIt = m_buffers.find(meshIt->second.vertexBuffer);
        auto ibIt = m_buffers.find(meshIt->second.indexBuffer);
        if (vbIt == m_buffers.end() || ibIt == m_buffers.end()) return;

        VkCommandBuffer cmd = m_commandBuffers[m_currentFrame];
        if (!PrepareDraw(cmd)) return;

        VkBuffer vertexBuffers[] = {vbIt->second.buffer};
        VkDeviceSize offsets[] = {0};
        BindVertexBuffersCached(cmd, 1, vertexBuffers, offsets);
        BindIndexBufferCached(cmd, ibIt->second.buffer, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, indexCount, 1, indexOffset, 0, 0);
    }

    void VKBackend::DrawArrays(MeshHandle mesh, uint32_t vertexCount, uint32_t firstVertex) {
        if (mesh == INVALID_MESH || m_boundShader == INVALID_SHADER || vertexCount == 0) return;

        auto meshIt = m_meshes.find(mesh);
        if (meshIt == m_meshes.end()) return;

        auto vbIt = m_buffers.find(meshIt->second.vertexBuffer);
        if (vbIt == m_buffers.end()) return;

        VkCommandBuffer cmd = m_commandBuffers[m_currentFrame];
        if (!PrepareDraw(cmd)) return;

        VkBuffer vertexBuffers[] = {vbIt->second.buffer};
        VkDeviceSize offsets[] = {0};
        BindVertexBuffersCached(cmd, 1, vertexBuffers, offsets);

        vkCmdDraw(cmd, vertexCount, 1, firstVertex, 0);
    }

    // ------------------------------------------------------------------
    // Shared draw front half + redundancy filters
    // ------------------------------------------------------------------

    void VKBackend::ResetRecordedBindings() {
        m_recorded = RecordedBindings{};
    }

    bool VKBackend::PrepareDraw(VkCommandBuffer cmd) {
        if (!m_boundShaderInfo) return false;

        VkPipeline pipeline = GetOrCreatePipeline(m_currentPipelineState, m_boundShader);
        if (pipeline == VK_NULL_HANDLE) return false;
        if (pipeline != m_currentPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            m_currentPipeline = pipeline;
        }
        // Per-draw stencil dynamic state. Sticky across pipeline binds within
        // a command buffer, so re-pushing every draw is mildly wasteful but
        // never wrong; the early-out on stencilTestEnabled keeps the overhead
        // at zero when stencil isn't in use.
        ApplyDynamicStencilState(cmd);

        // Push constants only when they changed. Push constants are bound per
        // pipeline LAYOUT, so a layout switch always re-pushes even if the
        // bytes match.
        const VkPipelineLayout pl = m_boundLayout;
        if (!m_recorded.pushValid || m_recorded.pushLayout != pl ||
            std::memcmp(&m_lastPushed, &m_pushConstants, sizeof(PushConstantBlock)) != 0) {
            vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(PushConstantBlock), &m_pushConstants);
            m_lastPushed          = m_pushConstants;
            m_recorded.pushLayout = pl;
            m_recorded.pushValid  = true;
        }

        if (m_boundIsPortal) {
            // Portal-feature shader: UBO + texture descriptor sets with per-
            // draw dynamic offsets (BindPortalDescriptorForDraw also uploads
            // any dirty UBO data). A false return means no valid binding —
            // recording the draw anyway would use the PREVIOUS draw's texture
            // and offsets. It rebinds set 0, so the block-path cache is stale.
            m_recorded.textureSet = VK_NULL_HANDLE;
            return BindPortalDescriptorForDraw(cmd, m_boundTexture);
        }

        // Block-style shader: texture-only descriptor at set 0. Skipped when
        // the same set is already bound with the same layout (the portal
        // path binds with a different layout, which invalidates set 0 for
        // this one — hence the reset above).
        auto texIt = m_textures.find(m_boundTexture);
        if (texIt == m_textures.end() || texIt->second.descriptorSet == VK_NULL_HANDLE) return false;
        if (m_recorded.textureSet != texIt->second.descriptorSet) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl,
                                    0, 1, &texIt->second.descriptorSet, 0, nullptr);
            m_recorded.textureSet = texIt->second.descriptorSet;
        }
        return true;
    }

    void VKBackend::BindVertexBuffersCached(VkCommandBuffer cmd, uint32_t count,
                                            const VkBuffer* buffers, const VkDeviceSize* offsets) {
        bool same = (m_recorded.vertexCount == count);
        for (uint32_t i = 0; same && i < count; ++i) {
            same = m_recorded.vertexBuffers[i] == buffers[i] && m_recorded.vertexOffsets[i] == offsets[i];
        }
        if (same) return;
        vkCmdBindVertexBuffers(cmd, 0, count, buffers, offsets);
        m_recorded.vertexCount = count;
        for (uint32_t i = 0; i < 2; ++i) {
            m_recorded.vertexBuffers[i] = (i < count) ? buffers[i] : VK_NULL_HANDLE;
            m_recorded.vertexOffsets[i] = (i < count) ? offsets[i] : 0;
        }
    }

    void VKBackend::BindIndexBufferCached(VkCommandBuffer cmd, VkBuffer buffer, VkIndexType type) {
        if (m_recorded.indexBuffer == buffer && m_recorded.indexType == type) return;
        vkCmdBindIndexBuffer(cmd, buffer, 0, type);
        m_recorded.indexBuffer = buffer;
        m_recorded.indexType   = type;
    }

    // ========================================================================
    // MEGA-BUFFER RENDERING
    // ========================================================================

    void VKBackend::BindVertexBuffer(BufferHandle vbo, uint32_t stride) {
        m_megaBoundVBO = vbo;
    }

    void VKBackend::BindIndexBuffer(BufferHandle ibo) {
        m_megaBoundIBO = ibo;
    }

    void VKBackend::DrawIndexedBaseVertex(uint32_t indexCount, size_t indexByteOffset, int32_t baseVertex,
                                          IndexType indexType) {
        if (m_boundShader == INVALID_SHADER || indexCount == 0) return;
        if (m_megaBoundVBO == INVALID_BUFFER || m_megaBoundIBO == INVALID_BUFFER) return;
        if (!m_frameActive) return;

        auto vbIt = m_buffers.find(m_megaBoundVBO);
        auto ibIt = m_buffers.find(m_megaBoundIBO);
        if (vbIt == m_buffers.end() || ibIt == m_buffers.end()) return;

        VkCommandBuffer cmd = m_commandBuffers[m_currentFrame];
        if (!PrepareDraw(cmd)) return;

        VkBuffer vertexBuffers[] = {vbIt->second.buffer};
        VkDeviceSize offsets[] = {0};
        BindVertexBuffersCached(cmd, 1, vertexBuffers, offsets);

        const bool u16 = (indexType == IndexType::Uint16);
        BindIndexBufferCached(cmd, ibIt->second.buffer,
                              u16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);

        uint32_t firstIndex = static_cast<uint32_t>(
            indexByteOffset / (u16 ? sizeof(uint16_t) : sizeof(uint32_t)));
        vkCmdDrawIndexed(cmd, indexCount, 1, firstIndex, baseVertex, 0);
    }

    void VKBackend::MultiDrawIndexedBaseVertex(const int32_t* indexCounts,
                                                const size_t* indexByteOffsets,
                                                const int32_t* baseVertices,
                                                uint32_t drawCount,
                                                IndexType indexType) {
        if (m_boundShader == INVALID_SHADER || drawCount == 0) return;
        if (m_megaBoundVBO == INVALID_BUFFER || m_megaBoundIBO == INVALID_BUFFER) return;
        if (!m_frameActive) return;

        auto vbIt = m_buffers.find(m_megaBoundVBO);
        auto ibIt = m_buffers.find(m_megaBoundIBO);
        if (vbIt == m_buffers.end() || ibIt == m_buffers.end()) return;

        VkCommandBuffer cmd = m_commandBuffers[m_currentFrame];
        if (!PrepareDraw(cmd)) return;

        // Bind VBO + IBO once for the entire batch
        VkBuffer vertexBuffers[] = {vbIt->second.buffer};
        VkDeviceSize vbOffsets[] = {0};
        BindVertexBuffersCached(cmd, 1, vertexBuffers, vbOffsets);

        const bool u16 = (indexType == IndexType::Uint16);
        const size_t indexSize = u16 ? sizeof(uint16_t) : sizeof(uint32_t);
        BindIndexBufferCached(cmd, ibIt->second.buffer,
                              u16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);

        // Issue one draw per section (cheap — just command buffer recording)
        if (m_multiDrawIndirect && !m_indirectBuffers.empty()) {
            const size_t frame = static_cast<size_t>(m_currentFrame);
            VkDeviceSize& off = m_indirectOffset[frame];
            const VkDeviceSize need = VkDeviceSize(drawCount) * sizeof(VkDrawIndexedIndirectCommand);
            if (off + need <= kIndirectRingBytes) {
                auto* cmds = reinterpret_cast<VkDrawIndexedIndirectCommand*>(
                    static_cast<char*>(m_indirectMapped[frame]) + off);
                uint32_t n = 0;
                for (uint32_t i = 0; i < drawCount; i++) {
                    if (indexCounts[i] <= 0) continue;
                    cmds[n].indexCount = static_cast<uint32_t>(indexCounts[i]);
                    cmds[n].instanceCount = 1;
                    cmds[n].firstIndex = static_cast<uint32_t>(indexByteOffsets[i] / indexSize);
                    cmds[n].vertexOffset = baseVertices[i];
                    cmds[n].firstInstance = 0;
                    ++n;
                }
                if (n > 0) {
                    vkCmdDrawIndexedIndirect(cmd, m_indirectBuffers[frame], off, n,
                                             sizeof(VkDrawIndexedIndirectCommand));
                }
                off += VkDeviceSize(n) * sizeof(VkDrawIndexedIndirectCommand);
                return;
            }
        }
        for (uint32_t i = 0; i < drawCount; i++) {
            if (indexCounts[i] <= 0) continue;
            uint32_t firstIndex = static_cast<uint32_t>(indexByteOffsets[i] / indexSize);
            vkCmdDrawIndexed(cmd, indexCounts[i], 1, firstIndex, baseVertices[i], 0);
        }
    }

    bool VKBackend::CreateIndirectRing() {
        m_indirectBuffers.assign(MAX_FRAMES_IN_FLIGHT, VK_NULL_HANDLE);
        m_indirectMemory.assign(MAX_FRAMES_IN_FLIGHT, VK_NULL_HANDLE);
        m_indirectMapped.assign(MAX_FRAMES_IN_FLIGHT, nullptr);
        m_indirectOffset.assign(MAX_FRAMES_IN_FLIGHT, 0);
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            if (!CreateVkBuffer(kIndirectRingBytes, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                m_indirectBuffers[i], m_indirectMemory[i])) {
                DestroyIndirectRing();
                return false;
            }
            if (vkMapMemory(m_device, m_indirectMemory[i], 0, kIndirectRingBytes, 0, &m_indirectMapped[i]) != VK_SUCCESS) {
                DestroyIndirectRing();
                return false;
            }
        }
        return true;
    }

    void VKBackend::DestroyIndirectRing() {
        for (size_t i = 0; i < m_indirectBuffers.size(); ++i) {
            if (m_indirectMapped[i]) vkUnmapMemory(m_device, m_indirectMemory[i]);
            if (m_indirectBuffers[i] != VK_NULL_HANDLE) vkDestroyBuffer(m_device, m_indirectBuffers[i], nullptr);
            if (m_indirectMemory[i] != VK_NULL_HANDLE) vkFreeMemory(m_device, m_indirectMemory[i], nullptr);
        }
        m_indirectBuffers.clear(); m_indirectMemory.clear(); m_indirectMapped.clear(); m_indirectOffset.clear();
    }

    // ========================================================================
    // GPU TIMERS (stub)
    // ========================================================================

    bool VKBackend::CreateTimestampPool() {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_physicalDevice, &props);

        // timestampPeriod == 0 means the device cannot do timestamps at all.
        // Also require the graphics queue family to have timestamp bits —
        // vkCmdWriteTimestamp is undefined on a family reporting 0.
        uint32_t famCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &famCount, nullptr);
        std::vector<VkQueueFamilyProperties> fams(famCount);
        vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &famCount, fams.data());

        const uint32_t gfx = m_queueFamilies.graphicsFamily.value();
        if (props.limits.timestampPeriod <= 0.0f ||
            gfx >= famCount || fams[gfx].timestampValidBits == 0) {
            Log::Info("VKBackend: GPU timestamps unavailable — pass timers disabled");
            m_timestampPeriodNs = 0.0f;
            return true;   // not fatal; timers just report -1 forever
        }

        VkQueryPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = kTimersPerFrame * 2 * MAX_FRAMES_IN_FLIGHT;
        if (vkCreateQueryPool(m_device, &info, nullptr, &m_timestampPool) != VK_SUCCESS) {
            Log::Warning("VKBackend: failed to create timestamp query pool");
            m_timestampPeriodNs = 0.0f;
            return true;
        }

        m_timestampPeriodNs = props.limits.timestampPeriod;
        Log::Info("VKBackend: GPU pass timers enabled (timestampPeriod %.3f ns)",
                  m_timestampPeriodNs);
        return true;
    }

    GPUTimerHandle VKBackend::BeginGPUTimer(const std::string& /*name*/) {
        if (m_timestampPool == VK_NULL_HANDLE || !m_frameActive) return INVALID_GPU_TIMER;
        if (m_timersUsedThisFrame >= kTimersPerFrame) return INVALID_GPU_TIMER;

        const uint32_t local = m_timersUsedThisFrame++;
        const uint32_t base  = m_currentFrame * kTimersPerFrame * 2 + local * 2;

        // BOTTOM_OF_PIPE: stamp once everything queued so far has finished, so
        // the pair brackets exactly the work issued between Begin and End.
        vkCmdWriteTimestamp(m_commandBuffers[m_currentFrame],
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            m_timestampPool, base);

        const uint32_t handle = AllocHandle();
        GPUTimer t;
        t.begin     = base;
        t.frameSlot = m_currentFrame;
        m_gpuTimers[handle] = t;
        return handle;
    }

    void VKBackend::EndGPUTimer(GPUTimerHandle handle) {
        auto it = m_gpuTimers.find(handle);
        if (it == m_gpuTimers.end() || it->second.ended) return;
        if (m_timestampPool == VK_NULL_HANDLE || !m_frameActive) return;

        vkCmdWriteTimestamp(m_commandBuffers[m_currentFrame],
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            m_timestampPool, it->second.begin + 1);
        it->second.ended = true;
    }

    float VKBackend::GetGPUTimerResultMs(GPUTimerHandle handle) {
        auto it = m_gpuTimers.find(handle);
        if (it == m_gpuTimers.end()) return -1.0f;

        // Already banked by BeginFrame's pre-reset sweep.
        if (it->second.resolved) {
            const float ms = it->second.resultMs;
            m_gpuTimers.erase(it);
            return ms;
        }
        if (!it->second.ended) return -1.0f;   // still recording this frame

        uint64_t stamps[2] = {0, 0};
        // No WAIT bit — the contract is non-blocking. VK_NOT_READY means the
        // GPU has not reached those commands yet; the caller polls again next
        // frame and the timer stays alive until then.
        const VkResult r = vkGetQueryPoolResults(
            m_device, m_timestampPool, it->second.begin, 2,
            sizeof(stamps), stamps, sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT);
        if (r != VK_SUCCESS) return -1.0f;

        const uint64_t elapsed = (stamps[1] >= stamps[0]) ? (stamps[1] - stamps[0]) : 0;
        m_gpuTimers.erase(it);
        return static_cast<float>(elapsed) * m_timestampPeriodNs / 1.0e6f;
    }

    // ========================================================================
    // MEMORY STATS
    // ========================================================================

    GPUMemoryStats VKBackend::GetMemoryStats() const { return m_memStats; }

    // ========================================================================
    // IMGUI
    // ========================================================================

    void VKBackend::ImGuiInit(GLFWwindow* window) {
        // Create dedicated descriptor pool for ImGui
        VkDescriptorPoolSize pool_sizes[] = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 100},
        };
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = 100;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = pool_sizes;
        vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_imguiDescriptorPool);

        ImGui_ImplGlfw_InitForVulkan(window, true);

        ImGui_ImplVulkan_InitInfo initInfo{};
        initInfo.Instance = m_instance;
        initInfo.PhysicalDevice = m_physicalDevice;
        initInfo.Device = m_device;
        initInfo.QueueFamily = m_queueFamilies.graphicsFamily.value();
        initInfo.Queue = m_graphicsQueue;
        initInfo.DescriptorPool = m_imguiDescriptorPool;
        initInfo.MinImageCount = static_cast<uint32_t>(m_swapchainImages.size());
        initInfo.ImageCount = static_cast<uint32_t>(m_swapchainImages.size());
        initInfo.RenderPass = m_renderPass;
        ImGui_ImplVulkan_Init(&initInfo);
        ImGui_ImplVulkan_CreateFontsTexture();
    }

    void VKBackend::ImGuiNewFrame() {
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
    }

    void VKBackend::ImGuiRender() {
        ImDrawData* drawData = ImGui::GetDrawData();
        if (drawData && drawData->CmdListsCount > 0) {
            ImGui_ImplVulkan_RenderDrawData(drawData, m_commandBuffers[m_currentFrame]);
            // ImGui bound its own pipeline, descriptor set, vertex/index
            // buffers and push constants into OUR command buffer; nothing the
            // redundancy filters remember is still true.
            m_currentPipeline = VK_NULL_HANDLE;
            ResetRecordedBindings();
        }
    }

    void VKBackend::ImGuiShutdown() {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
    }

    // ========================================================================
    // INITIALIZATION HELPERS
    // ========================================================================

    bool VKBackend::CreateInstance() {
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "MyVoxelGame";
        appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.pEngineName = "VoxelEngine";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_0;

        // Get required GLFW extensions
        uint32_t glfwExtCount = 0;
        const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
        std::vector<const char*> extensions(glfwExts, glfwExts + glfwExtCount);

        if (s_enableValidation) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

#ifdef __APPLE__
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif

        // VK_EXT_swapchain_colorspace, when the loader has it: it is what
        // lets the swapchain ask for PASS_THROUGH (no colour management),
        // which on macOS is the difference between our frame and Minecraft's
        // looking the same on a wide-gamut display. Optional — a driver
        // without it just keeps the sRGB-tagged surface.
        {
            uint32_t count = 0;
            vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
            std::vector<VkExtensionProperties> props(count);
            vkEnumerateInstanceExtensionProperties(nullptr, &count, props.data());
            for (const auto& e : props) {
                if (std::strcmp(e.extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) == 0) {
                    extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
                    m_hasSwapchainColorSpaceExt = true;
                    break;
                }
            }
        }

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();

#ifdef __APPLE__
        createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

        if (s_enableValidation) {
            // Check if validation layers are actually available
            uint32_t layerCount = 0;
            vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
            std::vector<VkLayerProperties> availableLayers(layerCount);
            vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

            bool layersAvailable = true;
            for (const char* layerName : s_validationLayers) {
                bool found = false;
                for (const auto& prop : availableLayers) {
                    if (std::strcmp(layerName, prop.layerName) == 0) { found = true; break; }
                }
                if (!found) { layersAvailable = false; break; }
            }

            if (layersAvailable) {
                createInfo.enabledLayerCount = static_cast<uint32_t>(s_validationLayers.size());
                createInfo.ppEnabledLayerNames = s_validationLayers.data();
            } else {
                Log::Warning("VKBackend: Validation layers not available, running without validation");
            }
        }

        VkResult result = vkCreateInstance(&createInfo, nullptr, &m_instance);
        if (result != VK_SUCCESS) {
            Log::Error("VKBackend: Failed to create Vulkan instance (error %d)", result);
            return false;
        }

        Log::Info("VKBackend: Vulkan instance created");
        return true;
    }

    bool VKBackend::SetupDebugMessenger() {
        VkDebugUtilsMessengerCreateInfoEXT createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        createInfo.pfnUserCallback = DebugCallback;

        auto func = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT");
        if (!func) return false;
        return func(m_instance, &createInfo, nullptr, &m_debugMessenger) == VK_SUCCESS;
    }

    VKAPI_ATTR VkBool32 VKAPI_CALL VKBackend::DebugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT type,
        const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
        void* userData) {
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
            Log::Error("[VK] %s", callbackData->pMessage);
        } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
            Log::Warning("[VK] %s", callbackData->pMessage);
        }
        return VK_FALSE;
    }

    bool VKBackend::CreateSurface(GLFWwindow* window) {
        // Check if GLFW supports Vulkan
        if (!glfwVulkanSupported()) {
            Log::Error("VKBackend: GLFW reports Vulkan is NOT supported on this system");
            Log::Error("VKBackend: This usually means the Vulkan loader (libvulkan) was not found at runtime");
            Log::Error("VKBackend: On macOS, ensure MoltenVK is installed (brew install molten-vk vulkan-loader)");
            return false;
        }
        Log::Info("VKBackend: GLFW confirms Vulkan is supported");

        VkResult result = glfwCreateWindowSurface(m_instance, window, nullptr, &m_surface);
        if (result != VK_SUCCESS) {
            Log::Error("VKBackend: Failed to create window surface (VkResult: %d)", static_cast<int>(result));
            return false;
        }
        Log::Info("VKBackend: Window surface created successfully");
        return true;
    }

    bool VKBackend::PickPhysicalDevice() {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
        if (deviceCount == 0) {
            Log::Error("VKBackend: No Vulkan-capable GPU found");
            return false;
        }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

        for (const auto& device : devices) {
            auto indices = FindQueueFamilies(device);
            if (indices.IsComplete() && CheckDeviceExtensionSupport(device)) {
                m_physicalDevice = device;
                m_queueFamilies = indices;
                break;
            }
        }

        if (m_physicalDevice == VK_NULL_HANDLE) {
            Log::Error("VKBackend: No suitable GPU found");
            return false;
        }

        vkGetPhysicalDeviceProperties(m_physicalDevice, &m_deviceProperties);
        vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &m_memProperties);
        Log::Info("VKBackend: Selected GPU: %s", m_deviceProperties.deviceName);
        return true;
    }

    bool VKBackend::CreateLogicalDevice() {
        std::set<uint32_t> uniqueQueueFamilies = {
            m_queueFamilies.graphicsFamily.value(),
            m_queueFamilies.presentFamily.value()
        };

        float queuePriority = 1.0f;
        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        for (uint32_t family : uniqueQueueFamilies) {
            VkDeviceQueueCreateInfo queueInfo{};
            queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo.queueFamilyIndex = family;
            queueInfo.queueCount = 1;
            queueInfo.pQueuePriorities = &queuePriority;
            queueCreateInfos.push_back(queueInfo);
        }

        VkPhysicalDeviceFeatures deviceFeatures{};
        deviceFeatures.fillModeNonSolid  = VK_TRUE; // For wireframe
        // Required so block_vk.vert can write gl_ClipDistance[0] for the
        // portal-plane half-space cull. Without this, the SPIR-V loader
        // accepts the shader but the rasterizer silently ignores the
        // gl_ClipDistance write — the portal clip fails and the chunk
        // shader has no way to clip at the dst plane.
        deviceFeatures.shaderClipDistance = VK_TRUE;
        {
            VkPhysicalDeviceFeatures supported{};
            vkGetPhysicalDeviceFeatures(m_physicalDevice, &supported);
            m_multiDrawIndirect = supported.multiDrawIndirect == VK_TRUE;
            // Depth clamp for the immersive portal surfaces (see
            // PipelineState::depthClampEnabled); optional, so request it
            // only where the device has it.
            m_depthClampSupported = supported.depthClamp == VK_TRUE;
            deviceFeatures.depthClamp = supported.depthClamp;
            deviceFeatures.multiDrawIndirect = supported.multiDrawIndirect;
            if (std::getenv("OBEY_VK_NO_INDIRECT")) m_multiDrawIndirect = false;   // A/B switch
            Log::Info("[VKBackend] multiDrawIndirect: %s", m_multiDrawIndirect ? "yes" : "no");
        }

        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
        createInfo.pQueueCreateInfos = queueCreateInfos.data();
        createInfo.pEnabledFeatures = &deviceFeatures;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(s_deviceExtensions.size());
        createInfo.ppEnabledExtensionNames = s_deviceExtensions.data();

        if (vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device) != VK_SUCCESS) {
            Log::Error("VKBackend: Failed to create logical device");
            return false;
        }

        vkGetDeviceQueue(m_device, m_queueFamilies.graphicsFamily.value(), 0, &m_graphicsQueue);
        vkGetDeviceQueue(m_device, m_queueFamilies.presentFamily.value(), 0, &m_presentQueue);
        return true;
    }

    bool VKBackend::CreateSwapchain(GLFWwindow* window) {
        VkSurfaceCapabilitiesKHR capabilities;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface, &capabilities);

        uint32_t formatCount;
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, formats.data());

        uint32_t presentModeCount;
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &presentModeCount, nullptr);
        std::vector<VkPresentModeKHR> presentModes(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &presentModeCount, presentModes.data());

        auto surfaceFormat = ChooseSwapSurfaceFormat(formats);
        Log::Info("VKBackend: surface format %d, colour space %s",
                  static_cast<int>(surfaceFormat.format),
                  surfaceFormat.colorSpace == VK_COLOR_SPACE_PASS_THROUGH_EXT   ? "pass-through (no colour management, like Minecraft's GL window)"
                : surfaceFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR ? "sRGB nonlinear (colour-managed by macOS)"
                : "other");
        auto presentMode = ChooseSwapPresentMode(presentModes);
        auto extent = ChooseSwapExtent(capabilities, window);

        // Log the present-mode decision — the vsync-off path silently falls
        // back to FIFO (vsync ON) when the driver doesn't advertise
        // MAILBOX/IMMEDIATE, and without this line that's indistinguishable
        // from the setting being ignored.
        {
            auto modeName = [](VkPresentModeKHR m) -> const char* {
                switch (m) {
                    case VK_PRESENT_MODE_IMMEDIATE_KHR:    return "IMMEDIATE";
                    case VK_PRESENT_MODE_MAILBOX_KHR:      return "MAILBOX";
                    case VK_PRESENT_MODE_FIFO_KHR:         return "FIFO";
                    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
                    default:                                return "OTHER";
                }
            };
            std::string available;
            for (auto m : presentModes) {
                if (!available.empty()) available += ", ";
                available += modeName(m);
            }
            Log::Info("VKBackend: swapchain present mode = %s (vsync setting: %s; available: %s)",
                      modeName(presentMode), m_vsyncEnabled ? "on" : "off", available.c_str());
        }

        uint32_t imageCount = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount)
            imageCount = capabilities.maxImageCount;

        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = m_surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        uint32_t queueFamilyIndices[] = {
            m_queueFamilies.graphicsFamily.value(),
            m_queueFamilies.presentFamily.value()
        };

        if (m_queueFamilies.graphicsFamily != m_queueFamilies.presentFamily) {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = queueFamilyIndices;
        } else {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }

        createInfo.preTransform = capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = VK_NULL_HANDLE;

        if (vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &m_swapchain) != VK_SUCCESS) {
            Log::Error("VKBackend: Failed to create swapchain");
            return false;
        }

        vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, nullptr);
        m_swapchainImages.resize(imageCount);
        vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, m_swapchainImages.data());

        m_swapchainFormat = surfaceFormat.format;
        m_swapchainExtent = extent;

        Log::Info("VKBackend: Swapchain created %dx%d, %d images", extent.width, extent.height, imageCount);
        return true;
    }

    bool VKBackend::CreateImageViews() {
        m_swapchainImageViews.resize(m_swapchainImages.size());
        for (size_t i = 0; i < m_swapchainImages.size(); i++) {
            m_swapchainImageViews[i] = CreateImageView(m_swapchainImages[i], m_swapchainFormat,
                                                       VK_IMAGE_ASPECT_COLOR_BIT, 1);
        }
        return true;
    }

    // Forward decl — defined further down next to FindDepthFormat. True iff
    // the format has a packed stencil component (D24S8, D32S8, D16S8).
    static bool DepthFormatHasStencil(VkFormat fmt);

    bool VKBackend::CreateDepthResources() {
        m_depthFormat = FindDepthFormat();
        if (!CreateVkImage(m_swapchainExtent.width, m_swapchainExtent.height, 1,
                     m_depthFormat, VK_IMAGE_TILING_OPTIMAL,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_depthImage, m_depthMemory)) {
            Log::Error("VKBackend: Failed to create depth image");
            return false;
        }
        // Aspect mask must include STENCIL_BIT when the format actually has a
        // stencil component, otherwise the validation layer warns and (on
        // some drivers) reading the stencil aspect via this view fails.
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (DepthFormatHasStencil(m_depthFormat)) aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        m_depthImageView = CreateImageView(m_depthImage, m_depthFormat, aspect, 1);
        return m_depthImageView != VK_NULL_HANDLE;
    }

    bool VKBackend::CreateRenderPass() {
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = m_swapchainFormat;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentDescription depthAttachment{};
        depthAttachment.format = m_depthFormat;
        depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        // Stencil load: clear at render-pass start so each frame begins with
        // stencil = 0. (Without this, the portal renderer's "stencil ==
        // recursion level" check would see leftover values from prior frames
        // and either fail to mark or mask the wrong region.) Don't bother
        // storing — Phase 7's recursive passes ALL run within one render
        // pass, so stencil never needs to survive the swap.
        depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
        subpass.pDepthStencilAttachment = &depthRef;

        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        std::array<VkAttachmentDescription, 2> attachments = {colorAttachment, depthAttachment};
        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        renderPassInfo.pAttachments = attachments.data();
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies = &dependency;

        return vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass) == VK_SUCCESS;
    }

    bool VKBackend::CreateFramebuffers() {
        m_framebuffers.resize(m_swapchainImageViews.size());
        for (size_t i = 0; i < m_swapchainImageViews.size(); i++) {
            std::array<VkImageView, 2> attachments = {m_swapchainImageViews[i], m_depthImageView};

            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass = m_renderPass;
            fbInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
            fbInfo.pAttachments = attachments.data();
            fbInfo.width = m_swapchainExtent.width;
            fbInfo.height = m_swapchainExtent.height;
            fbInfo.layers = 1;

            if (vkCreateFramebuffer(m_device, &fbInfo, nullptr, &m_framebuffers[i]) != VK_SUCCESS)
                return false;
        }
        return true;
    }

    bool VKBackend::CreateCommandPool() {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = m_queueFamilies.graphicsFamily.value();
        return vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) == VK_SUCCESS;
    }

    bool VKBackend::CreateCommandBuffers() {
        m_commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = static_cast<uint32_t>(m_commandBuffers.size());
        return vkAllocateCommandBuffers(m_device, &allocInfo, m_commandBuffers.data()) == VK_SUCCESS;
    }

    bool VKBackend::CreateSyncObjects() {
        m_imageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
        m_inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);

        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            if (vkCreateSemaphore(m_device, &semInfo, nullptr, &m_imageAvailableSemaphores[i]) != VK_SUCCESS ||
                vkCreateFence(m_device, &fenceInfo, nullptr, &m_inFlightFences[i]) != VK_SUCCESS)
                return false;
        }
        return CreateRenderFinishedSemaphores();
    }

    bool VKBackend::CreateRenderFinishedSemaphores() {
        DestroyRenderFinishedSemaphores();
        m_renderFinishedSemaphores.assign(m_swapchainImages.size(), VK_NULL_HANDLE);
        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (VkSemaphore& sem : m_renderFinishedSemaphores) {
            if (vkCreateSemaphore(m_device, &semInfo, nullptr, &sem) != VK_SUCCESS) return false;
        }
        return true;
    }

    void VKBackend::DestroyRenderFinishedSemaphores() {
        // Callers hold a vkDeviceWaitIdle (Shutdown / RecreateSwapchain), so
        // no present is still waiting on any of these.
        for (VkSemaphore sem : m_renderFinishedSemaphores) {
            if (sem != VK_NULL_HANDLE) vkDestroySemaphore(m_device, sem, nullptr);
        }
        m_renderFinishedSemaphores.clear();
    }

    bool VKBackend::CreateDescriptorPool() {
        // Pool sized for: many block textures (one descriptor set per
        // texture) + a handful of portal descriptor sets (one per
        // frame-in-flight × N portal pipeline layouts; just MAX_FRAMES
        // for now). UBO descriptors: 2 per portal set (Common + Bones).
        VkDescriptorPoolSize poolSizes[3]{};
        poolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[0].descriptorCount = 4096;
        // CommonUBO + BonesUBO are dynamic — pool must size that type. Plus
        // one per BufferUsage::Uniform buffer that gets bound as the user
        // uniform block (three terrain mega buffers today).
        poolSizes[1].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        poolSizes[1].descriptorCount = 64;   // 2 × MAX_FRAMES + uniform blocks + headroom
        // One per buffer texture (CreateBufferTexture): a terrain mega-buffer
        // slab each, 128 slabs per pool at most.
        poolSizes[2].type            = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        poolSizes[2].descriptorCount = 512;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 3;
        poolInfo.pPoolSizes    = poolSizes;
        poolInfo.maxSets       = 4096 + MAX_FRAMES_IN_FLIGHT + 32 + 512;
        return vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) == VK_SUCCESS;
    }

    bool VKBackend::CreateDescriptorSetLayout() {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;
        return vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_textureDescriptorLayout) == VK_SUCCESS;
    }

    bool VKBackend::CreatePipelineLayout() {
        VkPushConstantRange pushConstant{};
        pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstant.offset = 0;
        pushConstant.size = sizeof(PushConstantBlock);

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &m_textureDescriptorLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushConstant;
        return vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout) == VK_SUCCESS;
    }

    // ------------------------------------------------------------------
    // Portal-feature uniform infrastructure: a second descriptor set
    // layout + pipeline layout that adds a CommonUBO + BonesUBO on top
    // of the existing texture sampler. Portal renderer, viewmodel,
    // crosshair-with-tint, HDR tonemap, bloom, etc. all use this.
    // ------------------------------------------------------------------
    bool VKBackend::CreatePortalDescriptorLayout() {
        // PORTAL DESCRIPTOR LAYOUT lives at set=1. Texture(s) stay at
        // set=0 reusing the existing per-texture descriptor sets so we
        // don't have to rewrite descriptors mid-frame (which is undefined
        // behavior in Vulkan — and caused the "grey gun" symptom because
        // pending draws were reading from stomped descriptors).
        //
        // CommonUBO + BonesUBO use DYNAMIC type so each draw can address
        // a different slice of the per-frame ring buffer via the
        // pDynamicOffsets array passed to vkCmdBindDescriptorSets.
        // Without this, every draw in a frame stomps the same buffer
        // and the GPU reads only the LAST values for ALL draws.
        VkDescriptorSetLayoutBinding bindings[2]{};
        // binding=0 — CommonUBO (dynamic).
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        // binding=1 — BonesUBO (dynamic; viewmodel only, declared on all
        // portal shaders so the layout is shared; shaders that don't
        // sample it simply ignore the binding).
        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutCreateInfo info{};
        info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = 2;
        info.pBindings    = bindings;
        return vkCreateDescriptorSetLayout(m_device, &info, nullptr,
                                           &m_portalDescriptorLayout) == VK_SUCCESS;
    }

    bool VKBackend::CreateUniformBlockLayout() {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = 1;
        info.pBindings    = &binding;
        return vkCreateDescriptorSetLayout(m_device, &info, nullptr, &m_uniformBlockLayout) == VK_SUCCESS;
    }

    void VKBackend::BindUniformBuffer(BufferHandle handle, size_t offset, size_t size) {
        auto it = m_buffers.find(handle);
        if (it == m_buffers.end() || m_uniformBlockLayout == VK_NULL_HANDLE) {
            m_boundUniformBuffer = INVALID_BUFFER;
            return;
        }
        VKBufferInfo& info = it->second;
        if (info.uniformSet == VK_NULL_HANDLE) {
            // First bind: one descriptor set per buffer, written once with
            // the window size; the per-bind offset is the dynamic offset.
            // Freed with the pool (it has no FREE_DESCRIPTOR_SET flag), like
            // texture sets — these buffers live as long as the world does.
            VkDescriptorSetAllocateInfo allocInfo{};
            allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocInfo.descriptorPool     = m_descriptorPool;
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts        = &m_uniformBlockLayout;
            if (vkAllocateDescriptorSets(m_device, &allocInfo, &info.uniformSet) != VK_SUCCESS) {
                Log::Error("VKBackend: descriptor pool exhausted — uniform block not bound");
                info.uniformSet = VK_NULL_HANDLE;
                m_boundUniformBuffer = INVALID_BUFFER;
                return;
            }
            VkDescriptorBufferInfo bufInfo{info.buffer, 0, static_cast<VkDeviceSize>(size)};
            VkWriteDescriptorSet write{};
            write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet          = info.uniformSet;
            write.dstBinding      = 0;
            write.descriptorCount = 1;
            write.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            write.pBufferInfo     = &bufInfo;
            vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
            info.uniformRange = size;
        } else if (info.uniformRange != size) {
            static bool s_warned = false;
            if (!s_warned) {
                s_warned = true;
                Log::Warning("VKBackend: BindUniformBuffer window size changed (%zu -> %zu); "
                             "the descriptor keeps its first size", info.uniformRange, size);
            }
        }
        m_boundUniformBuffer = handle;
        m_boundUniformOffset = static_cast<uint32_t>(offset);
    }

    bool VKBackend::CreateTexelBufferLayout() {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = 1;
        info.pBindings    = &binding;
        return vkCreateDescriptorSetLayout(m_device, &info, nullptr, &m_texelBufferLayout) == VK_SUCCESS;
    }

    TextureHandle VKBackend::CreateBufferTexture(BufferHandle buffer, TextureFormat format) {
        auto bit = m_buffers.find(buffer);
        if (bit == m_buffers.end() || m_texelBufferLayout == VK_NULL_HANDLE) return INVALID_TEXTURE;
        if (bit->second.usage != BufferUsage::Vertex) {
            // Only vertex buffers are created with the texel-buffer usage bit.
            Log::Error("VKBackend::CreateBufferTexture: buffer was not created with texel-buffer usage");
            return INVALID_TEXTURE;
        }
        VkFormat vkFormat;
        size_t bytesPerTexel;
        switch (format) {
            case TextureFormat::RGBA8:   vkFormat = VK_FORMAT_R8G8B8A8_UNORM;      bytesPerTexel = 4;  break;
            case TextureFormat::RGBA16:  vkFormat = VK_FORMAT_R16G16B16A16_UNORM;  bytesPerTexel = 8;  break;
            case TextureFormat::RGBA16F: vkFormat = VK_FORMAT_R16G16B16A16_SFLOAT; bytesPerTexel = 8;  break;
            case TextureFormat::RGBA32F: vkFormat = VK_FORMAT_R32G32B32A32_SFLOAT; bytesPerTexel = 16; break;
            default:
                Log::Error("VKBackend::CreateBufferTexture: unsupported format");
                return INVALID_TEXTURE;
        }
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
        const size_t texels = bit->second.size / bytesPerTexel;
        if (texels > props.limits.maxTexelBufferElements) {
            Log::Error("VKBackend::CreateBufferTexture: %zu texels exceeds maxTexelBufferElements (%u)",
                       texels, props.limits.maxTexelBufferElements);
            return INVALID_TEXTURE;
        }

        VkBufferViewCreateInfo viewInfo{};
        viewInfo.sType  = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
        viewInfo.buffer = bit->second.buffer;
        viewInfo.format = vkFormat;
        viewInfo.offset = 0;
        viewInfo.range  = VK_WHOLE_SIZE;
        VkBufferView view = VK_NULL_HANDLE;
        if (vkCreateBufferView(m_device, &viewInfo, nullptr, &view) != VK_SUCCESS) {
            Log::Error("VKBackend::CreateBufferTexture: vkCreateBufferView failed");
            return INVALID_TEXTURE;
        }

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool     = m_descriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts        = &m_texelBufferLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(m_device, &allocInfo, &set) != VK_SUCCESS) {
            Log::Error("VKBackend::CreateBufferTexture: descriptor pool exhausted");
            vkDestroyBufferView(m_device, view, nullptr);
            return INVALID_TEXTURE;
        }
        VkWriteDescriptorSet write{};
        write.sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet           = set;
        write.dstBinding       = 0;
        write.descriptorCount  = 1;
        write.descriptorType   = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        write.pTexelBufferView = &view;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);

        uint32_t handle = AllocHandle();
        VKTextureInfo info{};
        info.descriptorSet = set;
        info.width         = static_cast<int>(texels);
        info.height        = 1;
        info.format        = vkFormat;
        info.bufferView    = view;
        m_textures[handle] = info;
        m_memStats.textureCount++;
        return handle;
    }

    bool VKBackend::CreatePortalPipelineLayout() {
        VkPushConstantRange pushConstant{};
        pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstant.offset = 0;
        pushConstant.size   = sizeof(PushConstantBlock);

        // set=0 = primary texture (existing per-texture descriptor layout)
        // set=1 = UBOs (CommonUBO + BonesUBO, per-frame, stable)
        // set=2 = secondary texture (existing per-texture descriptor layout —
        //         reused so portal renderer's noise + colour-ramp pair maps
        //         to (slot 0, slot 1). Pipelines using only one texture
        //         simply bind a dummy texture at set=2 to satisfy the layout.)
        // set=3 = the user uniform block (RenderBackend::BindUniformBuffer):
        //         the terrain vertex shader's SectionOrigins. Present in the
        //         layout for every portal-feature pipeline, bound only when a
        //         buffer is bound; shaders that do not declare set 3 ignore it.
        // set=4 = a uniform texel buffer (RenderBackend::CreateBufferTexture,
        //         bound through texture slot 2): the terrain face map.
        VkDescriptorSetLayout sets[5] = { m_textureDescriptorLayout,
                                          m_portalDescriptorLayout,
                                          m_textureDescriptorLayout,
                                          m_uniformBlockLayout,
                                          m_texelBufferLayout };

        VkPipelineLayoutCreateInfo info{};
        info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount         = (m_uniformBlockLayout == VK_NULL_HANDLE) ? 3
                                    : (m_texelBufferLayout == VK_NULL_HANDLE) ? 4 : 5;
        info.pSetLayouts            = sets;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges    = &pushConstant;
        return vkCreatePipelineLayout(m_device, &info, nullptr,
                                      &m_portalPipelineLayout) == VK_SUCCESS;
    }

    bool VKBackend::CreateFrameUBOs() {
        const VkMemoryPropertyFlags hostVisible =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        const VkBufferUsageFlags uboUsage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

        // Query device-required alignment for UBO offsets. Per-slot stride
        // must be a multiple of this AND >= the actual UBO struct size.
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
        m_uboAlignment = static_cast<uint32_t>(
            std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 4));

        auto AlignUp = [](uint32_t v, uint32_t a) {
            return (v + a - 1) & ~(a - 1);
        };
        m_commonSlotStride = AlignUp(static_cast<uint32_t>(sizeof(CommonUBO)), m_uboAlignment);
        m_bonesSlotStride  = AlignUp(static_cast<uint32_t>(sizeof(BonesUBO)),  m_uboAlignment);

        const VkDeviceSize commonRingSize = VkDeviceSize(m_commonSlotStride) * kCommonSlotCount;
        const VkDeviceSize bonesRingSize  = VkDeviceSize(m_bonesSlotStride)  * kBonesSlotCount;

        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            FrameUBOs& fb = m_frameUBOs[i];
            fb.commonWriteSlot = 0;
            fb.bonesWriteSlot  = 0;
            fb.haveCommonSlot  = false;
            fb.haveBonesSlot   = false;
            fb.exhaustWarned   = false;
            fb.bonesWriteSlot  = 0;
            // CommonUBO ring buffer
            if (!CreateVkBuffer(commonRingSize, uboUsage, hostVisible,
                                fb.commonBuffer, fb.commonMemory)) {
                Log::Error("VKBackend: failed to create CommonUBO ring[%d]", i);
                return false;
            }
            void* mapped = nullptr;
            vkMapMemory(m_device, fb.commonMemory, 0, commonRingSize, 0, &mapped);
            fb.commonMapped = static_cast<uint8_t*>(mapped);

            // Initialise slot 0 with defaults so a draw before any
            // SetUniform call still reads sane data.
            CommonUBO initCommon;
            std::memcpy(fb.commonMapped, &initCommon, sizeof(CommonUBO));

            // BonesUBO ring buffer
            if (!CreateVkBuffer(bonesRingSize, uboUsage, hostVisible,
                                fb.bonesBuffer, fb.bonesMemory)) {
                Log::Error("VKBackend: failed to create BonesUBO ring[%d]", i);
                return false;
            }
            vkMapMemory(m_device, fb.bonesMemory, 0, bonesRingSize, 0, &mapped);
            fb.bonesMapped = static_cast<uint8_t*>(mapped);
            BonesUBO initBones;
            for (int k = 0; k < kMaxBones; ++k) initBones.bones[k] = glm::mat4(1.0f);
            std::memcpy(fb.bonesMapped, &initBones, sizeof(BonesUBO));

            // Allocate the portal descriptor set.
            VkDescriptorSetAllocateInfo allocInfo{};
            allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocInfo.descriptorPool     = m_descriptorPool;
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts        = &m_portalDescriptorLayout;
            if (vkAllocateDescriptorSets(m_device, &allocInfo, &fb.descriptorSet) != VK_SUCCESS) {
                Log::Error("VKBackend: failed to allocate portal descriptor set[%d]", i);
                return false;
            }

            // Wire CommonUBO (binding=0) and BonesUBO (binding=1) ONCE
            // here. Descriptor type is UNIFORM_BUFFER_DYNAMIC — the
            // pBufferInfo.range is the per-draw window size (one slot),
            // and the per-draw byte offset comes from pDynamicOffsets
            // passed to vkCmdBindDescriptorSets at draw time.
            VkDescriptorBufferInfo commonInfo{fb.commonBuffer, 0, sizeof(CommonUBO)};
            VkDescriptorBufferInfo bonesInfo {fb.bonesBuffer,  0, sizeof(BonesUBO)};
            VkWriteDescriptorSet writes[2]{};
            writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet          = fb.descriptorSet;
            writes[0].dstBinding      = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            writes[0].pBufferInfo     = &commonInfo;
            writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet          = fb.descriptorSet;
            writes[1].dstBinding      = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            writes[1].pBufferInfo     = &bonesInfo;
            vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);
        }
        return true;
    }

    void VKBackend::DestroyFrameUBOs() {
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            FrameUBOs& fb = m_frameUBOs[i];
            if (fb.commonMapped) { vkUnmapMemory(m_device, fb.commonMemory); fb.commonMapped = nullptr; }
            if (fb.bonesMapped)  { vkUnmapMemory(m_device, fb.bonesMemory);  fb.bonesMapped  = nullptr; }
            fb.commonWriteSlot = 0;
            fb.bonesWriteSlot  = 0;
            fb.haveCommonSlot  = false;
            fb.haveBonesSlot   = false;
            fb.exhaustWarned   = false;
            fb.bonesWriteSlot  = 0;
            if (fb.commonBuffer != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, fb.commonBuffer, nullptr); fb.commonBuffer = VK_NULL_HANDLE; }
            if (fb.commonMemory != VK_NULL_HANDLE) { vkFreeMemory(m_device,   fb.commonMemory, nullptr);  fb.commonMemory = VK_NULL_HANDLE; }
            if (fb.bonesBuffer  != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, fb.bonesBuffer,  nullptr); fb.bonesBuffer  = VK_NULL_HANDLE; }
            if (fb.bonesMemory  != VK_NULL_HANDLE) { vkFreeMemory(m_device,   fb.bonesMemory,  nullptr);  fb.bonesMemory  = VK_NULL_HANDLE; }
            // Descriptor sets are freed when the pool is destroyed.
        }
    }

    bool VKBackend::BindPortalDescriptorForDraw(VkCommandBuffer cmd, TextureHandle tex) {
        FrameUBOs& fb = m_frameUBOs[m_currentFrame];

        // A draw only needs a fresh slot for data that actually changed since
        // the previous draw; otherwise it rebinds the previous slot. This is
        // what keeps a mass detonation — thousands of per-entity draws that
        // never touch bones and rarely change the common block — from
        // exhausting the ring and silently mis-binding later draws (the held
        // item, drawn last in the frame, was the one that vanished).
        const bool needCommon = m_commonUBODirty || !fb.haveCommonSlot;
        const bool needBones  = m_bonesUBODirty  || !fb.haveBonesSlot;

        if ((needCommon && fb.commonWriteSlot >= kCommonSlotCount) ||
            (needBones  && fb.bonesWriteSlot  >= kBonesSlotCount)) {
            if (!fb.exhaustWarned) {
                fb.exhaustWarned = true;
                Log::Warning("VKBackend: UBO ring exhausted (%u common / %u bones "
                             "slots) — dropping further draws this frame rather "
                             "than corrupting earlier ones",
                             kCommonSlotCount, kBonesSlotCount);
            }
            // The caller must SKIP the draw: recording it anyway would run it
            // with whatever texture and UBO offsets the previous draw bound.
            return false;
        }

        if (needCommon) {
            const uint32_t slot = fb.commonWriteSlot++;
            fb.lastCommonOffset = slot * m_commonSlotStride;
            std::memcpy(fb.commonMapped + fb.lastCommonOffset, &m_commonUBOData, sizeof(CommonUBO));
            fb.haveCommonSlot = true;
            m_commonUBODirty  = false;
        }
        if (needBones) {
            const uint32_t slot = fb.bonesWriteSlot++;
            fb.lastBonesOffset = slot * m_bonesSlotStride;
            std::memcpy(fb.bonesMapped + fb.lastBonesOffset, &m_bonesUBOData, sizeof(BonesUBO));
            fb.haveBonesSlot = true;
            m_bonesUBODirty  = false;
        }

        auto tex0It = m_textures.find(tex);
        if (tex0It == m_textures.end() || tex0It->second.descriptorSet == VK_NULL_HANDLE) return false;
        TextureHandle tex1Handle = m_boundTextures[1] != INVALID_TEXTURE
                                 ? m_boundTextures[1] : tex;
        auto tex1It = m_textures.find(tex1Handle);
        VkDescriptorSet tex1Set = (tex1It != m_textures.end() && tex1It->second.descriptorSet != VK_NULL_HANDLE)
                                ? tex1It->second.descriptorSet
                                : tex0It->second.descriptorSet;
        VkDescriptorSet sets[3] = {
            tex0It->second.descriptorSet,
            fb.descriptorSet,
            tex1Set,
        };
        const uint32_t dynOffsets[2] = { fb.lastCommonOffset, fb.lastBonesOffset };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_portalPipelineLayout, 0, 3, sets, 2, dynOffsets);
        // The user uniform block (set 3), when one is bound — the terrain
        // mega buffer's section-origin table for the slab being drawn.
        if (m_boundUniformBuffer != INVALID_BUFFER && m_uniformBlockLayout != VK_NULL_HANDLE) {
            auto ubIt = m_buffers.find(m_boundUniformBuffer);
            if (ubIt != m_buffers.end() && ubIt->second.uniformSet != VK_NULL_HANDLE) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        m_portalPipelineLayout, 3, 1, &ubIt->second.uniformSet,
                                        1, &m_boundUniformOffset);
            }
            // The face map (set 4): whatever buffer texture is in slot 2 —
            // the terrain slab being drawn. Only buffer textures qualify;
            // a 2D texture in slot 2 has the wrong descriptor layout.
            if (m_texelBufferLayout != VK_NULL_HANDLE && m_boundTextures[2] != INVALID_TEXTURE) {
                auto fmIt = m_textures.find(m_boundTextures[2]);
                if (fmIt != m_textures.end() && fmIt->second.bufferView != VK_NULL_HANDLE &&
                    fmIt->second.descriptorSet != VK_NULL_HANDLE) {
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            m_portalPipelineLayout, 4, 1, &fmIt->second.descriptorSet,
                                            0, nullptr);
                }
            }
        }
        return true;
    }

    void VKBackend::RegisterShaderVertexLayout(ShaderHandle shader, const VertexLayout& layout) {
        auto it = m_shaders.find(shader);
        if (it != m_shaders.end()) it->second.vertexLayout = layout;
    }

    void VKBackend::RegisterShaderInstanceLayout(ShaderHandle shader, const VertexLayout& layout) {
        // Per-INSTANCE attributes (binding 1, VK_VERTEX_INPUT_RATE_INSTANCE);
        // see CreateGraphicsPipeline. The instanced block shader registers its
        // mat4-as-four-vec4 columns here.
        auto it = m_shaders.find(shader);
        if (it != m_shaders.end()) it->second.instanceLayout = layout;
    }

    ShaderHandle VKBackend::CreateShaderFromFilesPortal(const std::string& vertexPath,
                                                        const std::string& fragmentPath) {
        // Same SPV-file lookup as CreateShaderFromFiles, but stamps the
        // resulting shader with layoutType = 1 so GetOrCreatePipeline +
        // DrawIndexed pick the portal pipeline layout / descriptor set.
        ShaderHandle h = CreateShaderFromFiles(vertexPath, fragmentPath);
        if (h != INVALID_SHADER) {
            auto it = m_shaders.find(h);
            if (it != m_shaders.end()) it->second.layoutType = 1;
        }
        return h;
    }

    bool VKBackend::CreatePipelineCache() {
        // <obeycraft>/cache/vk_pipeline_cache.bin — the same directory tree
        // as saves, logs and options, so it is per-user and survives updates.
        {
            // The backend is created before GameDirectory::Initialize runs
            // (window first, then settings), so the instance path is empty
            // here; the static default is the same directory it will resolve.
            std::string gameDir = Platform::g_gameDirectory.GetGameDirectory();
            if (gameDir.empty()) gameDir = Platform::GameDirectory::GetDefaultGameDirectory();
            if (!gameDir.empty()) {
                m_pipelineCacheFile    = gameDir + "/cache/vk_pipeline_cache.bin";
                m_pipelineManifestFile = gameDir + "/cache/vk_pipeline_manifest.txt";
            }
        }

        std::vector<char> blob = LoadPipelineCacheBlob();

        VkPipelineCacheCreateInfo cacheInfo{};
        cacheInfo.sType           = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        cacheInfo.initialDataSize = blob.size();
        cacheInfo.pInitialData    = blob.empty() ? nullptr : blob.data();
        if (vkCreatePipelineCache(m_device, &cacheInfo, nullptr, &m_pipelineCache) == VK_SUCCESS) {
            if (!blob.empty()) {
                Log::Info("VKBackend: pipeline cache loaded (%zu bytes) from %s",
                          blob.size(), m_pipelineCacheFile.c_str());
            }
            return true;
        }
        // A driver may reject initial data it cannot parse even when the
        // header matched; an empty cache is always acceptable.
        if (!blob.empty()) {
            Log::Warning("VKBackend: driver rejected the on-disk pipeline cache — starting empty");
            cacheInfo.initialDataSize = 0;
            cacheInfo.pInitialData    = nullptr;
            return vkCreatePipelineCache(m_device, &cacheInfo, nullptr, &m_pipelineCache) == VK_SUCCESS;
        }
        return false;
    }

    std::vector<char> VKBackend::LoadPipelineCacheBlob() const {
        if (m_pipelineCacheFile.empty()) return {};
        std::vector<char> blob = ReadBinaryFile(m_pipelineCacheFile);
        if (blob.empty()) return {};

        // Validate the header ourselves rather than trusting the driver to:
        // the spec only says the data "must" have been produced by the same
        // device/driver, and feeding a stale blob to a driver that does not
        // check is undefined. The layout is fixed by VkPipelineCacheHeaderVersionOne.
        if (blob.size() < sizeof(VkPipelineCacheHeaderVersionOne)) {
            Log::Warning("VKBackend: pipeline cache file too small — ignoring");
            return {};
        }
        VkPipelineCacheHeaderVersionOne header{};
        std::memcpy(&header, blob.data(), sizeof(header));
        const bool valid =
            header.headerSize    == sizeof(VkPipelineCacheHeaderVersionOne) &&
            header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
            header.vendorID      == m_deviceProperties.vendorID &&
            header.deviceID      == m_deviceProperties.deviceID &&
            std::memcmp(header.pipelineCacheUUID, m_deviceProperties.pipelineCacheUUID,
                        VK_UUID_SIZE) == 0;
        if (!valid) {
            Log::Info("VKBackend: pipeline cache on disk is for a different device/driver — ignoring");
            return {};
        }
        return blob;
    }

    void VKBackend::SavePipelineCache() {
        if (m_pipelineCache == VK_NULL_HANDLE || m_pipelineCacheFile.empty()) return;
        m_pipelinesSinceSave = 0;

        size_t size = 0;
        if (vkGetPipelineCacheData(m_device, m_pipelineCache, &size, nullptr) != VK_SUCCESS || size == 0) return;
        std::vector<char> blob(size);
        if (vkGetPipelineCacheData(m_device, m_pipelineCache, &size, blob.data()) != VK_SUCCESS) return;
        blob.resize(size);

        // Write to a sibling and rename so a crash mid-write cannot leave a
        // truncated file that the next launch would hand to the driver.
        std::error_code ec;
        const std::filesystem::path path(m_pipelineCacheFile);
        std::filesystem::create_directories(path.parent_path(), ec);
        const std::filesystem::path tmp = path.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                Log::Warning("VKBackend: cannot write pipeline cache to %s", tmp.string().c_str());
                return;
            }
            out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
            if (!out.good()) {
                Log::Warning("VKBackend: pipeline cache write failed (%s)", tmp.string().c_str());
                return;
            }
        }
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            Log::Warning("VKBackend: pipeline cache rename failed: %s", ec.message().c_str());
            return;
        }
        Log::Info("VKBackend: pipeline cache saved (%zu bytes, %zu pipelines)", blob.size(), m_pipelines.size());
        SavePipelineManifest();
    }

    // ── Pipeline manifest + warm-up ──────────────────────────────────────
    // Two files, same format. Header `vkpm2 <sizeof(PipelineState)> <game
    // version>`, then one line per pipeline: `<vert path>\t<frag path>\t<state
    // bytes as hex>`. The state is written as raw bytes because that is
    // exactly what keys the pipeline; the size in the header rejects a file
    // from a build whose PipelineState differs.
    //
    //   SEED  <assets>/vk_pipeline_manifest.txt — ships with the game, a copy
    //         of the developer's list (tools/refresh_pipeline_manifest.sh),
    //         so a player's very first session is warm. Read whatever version
    //         stamp it carries: it was built for this build by definition.
    //   USER  <obeycraft>/cache/vk_pipeline_manifest.txt — what this machine
    //         has built, unioned across sessions. Read only if its version
    //         stamp is this game's: a new version starts the list over from
    //         the seed, so entries for shaders and states a release retired
    //         never pile up.
    namespace {
        std::string HexOf(const void* data, size_t n) {
            static const char* d = "0123456789abcdef";
            const auto* b = static_cast<const unsigned char*>(data);
            std::string out; out.reserve(n * 2);
            for (size_t i = 0; i < n; ++i) { out.push_back(d[b[i] >> 4]); out.push_back(d[b[i] & 15]); }
            return out;
        }
        bool UnhexInto(const std::string& hex, void* data, size_t n) {
            if (hex.size() != n * 2) return false;
            auto nib = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            auto* b = static_cast<unsigned char*>(data);
            for (size_t i = 0; i < n; ++i) {
                const int hi = nib(hex[2 * i]), lo = nib(hex[2 * i + 1]);
                if (hi < 0 || lo < 0) return false;
                b[i] = static_cast<unsigned char>((hi << 4) | lo);
            }
            return true;
        }
        std::string ManifestHeader() {
            return "vkpm2 " + std::to_string(sizeof(PipelineState)) + " " + GAME_VERSION;
        }
        // Lines of a manifest whose header is acceptable; empty otherwise.
        // `requireVersion`: the game-version field must match too.
        std::set<std::string> ReadManifest(const std::string& file, bool requireVersion) {
            std::set<std::string> lines;
            std::ifstream in(file);
            std::string header;
            if (!in.is_open() || !std::getline(in, header)) return lines;
            const std::string sizePrefix = "vkpm2 " + std::to_string(sizeof(PipelineState)) + " ";
            if (header.rfind(sizePrefix, 0) != 0) return lines;
            if (requireVersion && header != ManifestHeader()) return lines;
            std::string line;
            while (std::getline(in, line)) if (!line.empty()) lines.insert(line);
            return lines;
        }
        std::string SeedManifestPath() {
            const std::string assets = Platform::g_gameDirectory.GetAssetsDirectory();
            return assets.empty() ? std::string() : assets + "/vk_pipeline_manifest.txt";
        }
    }

    void VKBackend::SavePipelineManifest() {
        if (m_pipelineManifestFile.empty()) return;
        // Union with what this version already wrote: a session that never
        // visits the End must not forget the End's pipelines.
        std::set<std::string> lines = ReadManifest(m_pipelineManifestFile, /*requireVersion=*/true);
        for (const auto& [key, rec] : m_pipelines) {
            auto it = m_shaders.find(rec.shader);
            if (it == m_shaders.end() || it->second.vertPath.empty()) continue;
            lines.insert(it->second.vertPath + "\t" + it->second.fragPath + "\t" +
                         HexOf(&rec.state, sizeof(PipelineState)));
        }
        std::error_code ec;
        const std::filesystem::path path(m_pipelineManifestFile);
        std::filesystem::create_directories(path.parent_path(), ec);
        const std::filesystem::path tmp = path.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out.is_open()) return;
            out << ManifestHeader() << "\n";
            for (const auto& l : lines) out << l << "\n";
        }
        std::filesystem::rename(tmp, path, ec);
    }

    void VKBackend::WarmPipelines() {
        if (m_pipelinesWarmed) return;
        m_pipelinesWarmed = true;
        if (m_device == VK_NULL_HANDLE) return;
        PROFILE_ZONE_N("Vk.WarmPipelines");
        const auto t0 = std::chrono::steady_clock::now();

        const std::string seedFile = SeedManifestPath();
        std::set<std::string> lines = seedFile.empty() ? std::set<std::string>{}
                                                       : ReadManifest(seedFile, /*requireVersion=*/false);
        const size_t fromSeed = lines.size();
        if (!m_pipelineManifestFile.empty()) {
            const auto mine = ReadManifest(m_pipelineManifestFile, /*requireVersion=*/true);
            lines.insert(mine.begin(), mine.end());
        }
        if (lines.empty()) {
            Log::Info("VKBackend: no pipeline manifest (seed or cached) — first frames will build pipelines lazily");
            return;
        }
        // Shaders by the paths they were created from.
        std::unordered_map<std::string, ShaderHandle> byPath;
        for (const auto& [handle, info] : m_shaders) {
            if (!info.vertPath.empty()) byPath[info.vertPath + "\t" + info.fragPath] = handle;
        }
        size_t built = 0, unknownShader = 0;
        for (const std::string& line : lines) {
            const size_t t1 = line.find('\t');
            const size_t t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
            if (t2 == std::string::npos) continue;
            auto sh = byPath.find(line.substr(0, t2));
            if (sh == byPath.end()) { ++unknownShader; continue; }
            PipelineState state;
            if (!UnhexInto(line.substr(t2 + 1), &state, sizeof(PipelineState))) continue;
            const size_t before = m_pipelines.size();
            GetOrCreatePipeline(state, sh->second);
            if (m_pipelines.size() > before) ++built;
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        Log::Info("VKBackend: warmed %zu pipelines in %.0f ms from %zu manifest entries (%zu from the shipped seed; "
                  "%zu for shaders not loaded)", built, ms, lines.size(), fromSeed, unknownShader);
    }

    void VKBackend::DestroyAllPipelines() {
        for (auto& [key, rec] : m_pipelines) {
            if (rec.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, rec.pipeline, nullptr);
        }
        m_pipelines.clear();
        m_currentPipeline = VK_NULL_HANDLE;
    }

    void VKBackend::RebuildAllPipelines() {
        PROFILE_ZONE_N("Vk.RebuildAllPipelines");
        // One hitch here, now, instead of one per (state, shader) combination
        // spread across the next few frames as draws rediscover them.
        size_t rebuilt = 0;
        for (auto it = m_pipelines.begin(); it != m_pipelines.end(); ) {
            PipelineRecord& rec = it->second;
            if (rec.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, rec.pipeline, nullptr);
            rec.pipeline = CreateGraphicsPipeline(rec.state, rec.shader);
            if (rec.pipeline == VK_NULL_HANDLE) {
                it = m_pipelines.erase(it);   // shader gone; the draw path will retry lazily
            } else {
                ++rebuilt;
                ++it;
            }
        }
        m_currentPipeline = VK_NULL_HANDLE;
        Log::Info("VKBackend: rebuilt %zu pipelines for the new render pass", rebuilt);
    }

    // ========================================================================
    // SWAPCHAIN RECREATION
    // ========================================================================

    void VKBackend::RecreateSwapchain(GLFWwindow* window) {
        int width = 0, height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        while (width == 0 || height == 0) {
            glfwGetFramebufferSize(window, &width, &height);
            glfwWaitEvents();
        }
        vkDeviceWaitIdle(m_device);
        const VkFormat oldColorFormat = m_swapchainFormat;
        const VkFormat oldDepthFormat = m_depthFormat;
        CleanupSwapchain();

        CreateSwapchain(window);
        CreateImageViews();
        CreateDepthResources();

        // The render pass — and therefore every pipeline — only has to change
        // when an attachment FORMAT changes. Render-pass compatibility is
        // defined on attachment format + sample count alone (not extent, not
        // load/store ops, not layouts), and a VkPipeline is usable with any
        // render pass compatible with the one it was built against. A resize
        // or a vsync toggle keeps the same surface format and the depth
        // format is a fixed per-device choice, so the common case keeps the
        // render pass object itself and touches no pipeline at all. (Keeping
        // the object also keeps ImGui's pipeline valid — it was initialised
        // against this handle and was never told about a replacement.)
        const bool formatsChanged = (m_swapchainFormat != oldColorFormat) || (m_depthFormat != oldDepthFormat);
        if (m_renderPass == VK_NULL_HANDLE || formatsChanged) {
            if (m_renderPass != VK_NULL_HANDLE) {
                vkDestroyRenderPass(m_device, m_renderPass, nullptr);
                m_renderPass = VK_NULL_HANDLE;
            }
            CreateRenderPass();
            RebuildAllPipelines();
            Log::Info("VKBackend: Swapchain recreated with new attachment formats (render pass + pipelines rebuilt)");
        } else {
            Log::Info("VKBackend: Swapchain recreated (%ux%u), render pass and pipelines kept",
                      m_swapchainExtent.width, m_swapchainExtent.height);
        }
        CreateFramebuffers();
        // Image count can change with the present mode (vsync toggle), and the
        // render-finished semaphores are one per image.
        CreateRenderFinishedSemaphores();
        m_currentPipeline = VK_NULL_HANDLE;
    }

    void VKBackend::CleanupSwapchain() {
        // Deliberately leaves m_renderPass alone — see RecreateSwapchain.
        if (m_depthImageView != VK_NULL_HANDLE) vkDestroyImageView(m_device, m_depthImageView, nullptr);
        if (m_depthImage != VK_NULL_HANDLE) vkDestroyImage(m_device, m_depthImage, nullptr);
        if (m_depthMemory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_depthMemory, nullptr);
        for (auto fb : m_framebuffers) vkDestroyFramebuffer(m_device, fb, nullptr);
        for (auto iv : m_swapchainImageViews) vkDestroyImageView(m_device, iv, nullptr);
        if (m_swapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        m_framebuffers.clear();
        m_swapchainImageViews.clear();
        m_depthImageView = VK_NULL_HANDLE;
        m_depthImage = VK_NULL_HANDLE;
        m_depthMemory = VK_NULL_HANDLE;
        m_swapchain = VK_NULL_HANDLE;
    }

    // ========================================================================
    // UTILITY HELPERS
    // ========================================================================

    VKBackend::QueueFamilyIndices VKBackend::FindQueueFamilies(VkPhysicalDevice device) const {
        QueueFamilyIndices indices;
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

        for (uint32_t i = 0; i < count; i++) {
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                indices.graphicsFamily = i;
            VkBool32 presentSupport = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &presentSupport);
            if (presentSupport) indices.presentFamily = i;
            if (indices.IsComplete()) break;
        }
        return indices;
    }

    bool VKBackend::CheckDeviceExtensionSupport(VkPhysicalDevice device) const {
        uint32_t count;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> available(count);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data());

        std::set<std::string> required(s_deviceExtensions.begin(), s_deviceExtensions.end());
        for (const auto& ext : available) required.erase(ext.extensionName);
        return required.empty();
    }

    VkSurfaceFormatKHR VKBackend::ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const {
        // UNORM: all rendering is in gamma space like Minecraft, nothing is
        // linearised on the way in or encoded on the way out.
        //
        // Colour space: PASS_THROUGH when the surface offers it. With
        // SRGB_NONLINEAR, MoltenVK tags the CAMetalLayer as sRGB and macOS
        // colour-matches the frame into the panel's gamut, so on a Display
        // P3 Mac every colour is pulled in towards sRGB. Minecraft's OpenGL
        // window is untagged and is shown at the panel's native primaries —
        // that is what made the same sky pack look more vibrant there.
        // PASS_THROUGH leaves the layer's colorspace nil, which is exactly
        // the untagged GL behaviour (and what the GL backend here gets from
        // GLFW), so the two backends and Minecraft agree.
        if (m_hasSwapchainColorSpaceExt) {
            for (const auto& f : formats) {
                if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_PASS_THROUGH_EXT)
                    return f;
            }
        }
        for (const auto& f : formats) {
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                return f;
        }
        for (const auto& f : formats) {
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM)
                return f;
        }
        return formats[0];
    }

    VkPresentModeKHR VKBackend::ChooseSwapPresentMode(const std::vector<VkPresentModeKHR>& modes) const {
        if (!m_vsyncEnabled) {
            // Prefer MAILBOX (triple-buffered, no tearing, uncapped fps)
            for (auto mode : modes) {
                if (mode == VK_PRESENT_MODE_MAILBOX_KHR) return mode;
            }
            // Fallback to IMMEDIATE (may tear, but uncapped)
            for (auto mode : modes) {
                if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR) return mode;
            }
        }
        return VK_PRESENT_MODE_FIFO_KHR; // VSync on (guaranteed available)
    }

    void VKBackend::SetVSync(bool enabled) {
        if (m_vsyncEnabled == enabled) return;
        m_vsyncEnabled = enabled;
        // Don't recreate mid-frame — flag it for EndFrame to handle safely
        m_framebufferResized = true;
        Log::Info("VKBackend: VSync %s, swapchain will recreate next frame", enabled ? "enabled" : "disabled");
    }

    VkExtent2D VKBackend::ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& caps, GLFWwindow* window) const {
        if (caps.currentExtent.width != UINT32_MAX) return caps.currentExtent;
        int width, height;
        glfwGetFramebufferSize(window, &width, &height);
        VkExtent2D extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
        extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
        return extent;
    }

    uint32_t VKBackend::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const {
        // m_memProperties was captured in PickPhysicalDevice; it is immutable
        // for the life of the physical device, and this runs per allocation.
        for (uint32_t i = 0; i < m_memProperties.memoryTypeCount; i++) {
            if ((typeFilter & (1u << i)) && (m_memProperties.memoryTypes[i].propertyFlags & properties) == properties)
                return i;
        }
        Log::Error("VKBackend: Failed to find suitable memory type");
        return 0;
    }

    VkFormat VKBackend::FindDepthFormat() const {
        // Order matters: candidates are tried in sequence and the first one
        // the GPU supports wins. We require stencil bits because the portal
        // renderer (Phase 6+) uses stencil for the see-through pass — the
        // depth-only D32_SFLOAT format silently dropped stencil ops on the
        // floor here, which is hard to debug ("portals don't render"). The
        // packed S8 formats are universally supported on every desktop GPU,
        // so the no-stencil fallback at the end of the list realistically
        // never fires; it's there only so a dev who builds the engine on
        // an exotic device without packed depth-stencil still gets a depth
        // buffer.
        return FindSupportedFormat(
            {VK_FORMAT_D24_UNORM_S8_UINT,
             VK_FORMAT_D32_SFLOAT_S8_UINT,
             VK_FORMAT_D32_SFLOAT},
            VK_IMAGE_TILING_OPTIMAL,
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
    }

    // True iff the chosen depth format has a stencil component. Used to
    // pick the right aspect mask when creating the depth image view (image
    // views with VK_IMAGE_ASPECT_DEPTH_BIT only on a stencil-bearing format
    // would let the view bind, but you can't sample stencil from it; here
    // we don't sample either way, but the validation layer warns).
    static bool DepthFormatHasStencil(VkFormat fmt) {
        return fmt == VK_FORMAT_D24_UNORM_S8_UINT
            || fmt == VK_FORMAT_D32_SFLOAT_S8_UINT
            || fmt == VK_FORMAT_D16_UNORM_S8_UINT;
    }

    VkFormat VKBackend::FindSupportedFormat(const std::vector<VkFormat>& candidates,
                                           VkImageTiling tiling, VkFormatFeatureFlags features) const {
        for (VkFormat format : candidates) {
            VkFormatProperties props;
            vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &props);
            if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features) return format;
            if (tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features) return format;
        }
        return candidates[0];
    }

    bool VKBackend::CreateVkBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                  VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& memory) {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = size;
        bufInfo.usage = usage;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(m_device, &bufInfo, nullptr, &buffer) != VK_SUCCESS) return false;

        VkMemoryRequirements memReq;
        vkGetBufferMemoryRequirements(m_device, buffer, &memReq);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = FindMemoryType(memReq.memoryTypeBits, properties);
        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &memory) != VK_SUCCESS) return false;
        vkBindBufferMemory(m_device, buffer, memory, 0);
        return true;
    }

    bool VKBackend::CreateVkImage(uint32_t width, uint32_t height, uint32_t mipLevels,
                                 VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage,
                                 VkMemoryPropertyFlags properties, VkImage& image, VkDeviceMemory& memory) {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {width, height, 1};
        imageInfo.mipLevels = mipLevels;
        imageInfo.arrayLayers = 1;
        imageInfo.format = format;
        imageInfo.tiling = tiling;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = usage;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(m_device, &imageInfo, nullptr, &image) != VK_SUCCESS) return false;

        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(m_device, image, &memReq);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = FindMemoryType(memReq.memoryTypeBits, properties);
        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &memory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, image, memory, 0);
        return true;
    }

    VkImageView VKBackend::CreateImageView(VkImage image, VkFormat format,
                                           VkImageAspectFlags aspectFlags, uint32_t mipLevels) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = aspectFlags;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = mipLevels;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        VkImageView imageView;
        vkCreateImageView(m_device, &viewInfo, nullptr, &imageView);
        return imageView;
    }

    VkShaderModule VKBackend::CreateShaderModule(const std::vector<char>& code) const {
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = code.size();
        createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

        VkShaderModule shaderModule;
        if (vkCreateShaderModule(m_device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        return shaderModule;
    }

    VkCommandBuffer VKBackend::BeginSingleTimeCommands() {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandPool = m_commandPool;
        allocInfo.commandBufferCount = 1;
        VkCommandBuffer cmd;
        vkAllocateCommandBuffers(m_device, &allocInfo, &cmd);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);
        return cmd;
    }

    void VKBackend::EndSingleTimeCommands(VkCommandBuffer cmd) {
        // This is a full GPU drain (vkQueueWaitIdle). It belongs at load time
        // only; if the zone shows up inside a frame, whatever called it is
        // the hitch.
        PROFILE_ZONE_N("Vk.SingleTimeSubmit+Drain");
        vkEndCommandBuffer(cmd);
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);
        vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
    }

    void VKBackend::TransitionImageLayout(VkImage image, VkFormat format,
                                         VkImageLayout oldLayout, VkImageLayout newLayout, uint32_t mipLevels) {
        VkCommandBuffer cmd = BeginSingleTimeCommands();
        RecordImageLayoutTransition(cmd, image, format, oldLayout, newLayout, mipLevels);
        EndSingleTimeCommands(cmd);
    }

    void VKBackend::RecordImageLayoutTransition(VkCommandBuffer cmd, VkImage image, VkFormat format,
                                                VkImageLayout oldLayout, VkImageLayout newLayout,
                                                uint32_t mipLevels) {
        (void)format;

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = mipLevels;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;

        VkPipelineStageFlags srcStage, dstStage;

        if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        } else {
            srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        }

        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    void VKBackend::CopyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) {
        VkCommandBuffer cmd = BeginSingleTimeCommands();
        RecordCopyBufferToImage(cmd, buffer, image, width, height);
        EndSingleTimeCommands(cmd);
    }
    void VKBackend::RecordCopyBufferToImage(VkCommandBuffer cmd, VkBuffer buffer, VkImage image,
                                            uint32_t width, uint32_t height) {
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(cmd, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }

    void VKBackend::CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size) {
        VkCommandBuffer cmd = BeginSingleTimeCommands();
        VkBufferCopy copyRegion{};
        copyRegion.size = size;
        vkCmdCopyBuffer(cmd, srcBuffer, dstBuffer, 1, &copyRegion);
        EndSingleTimeCommands(cmd);
    }

    // ========================================================================
    // PIPELINE CREATION
    // ========================================================================

    size_t VKBackend::HashPipelineState(const PipelineState& state, ShaderHandle shader) const {
        // NOT a hash — a collision-free bit-packed key. Every field baked
        // into the VkPipeline gets dedicated bits, so two different
        // (shader, state) combinations can never map to the same cache
        // entry, and the m_pipelines lookup needs no equality check.
        //
        // The previous XOR-shift hash DID collide in practice: std::hash on
        // small ints is the identity, shader handles are small consecutive
        // integers, and each state field only flipped one or two low bits —
        // so e.g. (shaderA, depthTest off) could equal (shaderB, depthTest
        // on). Visible symptom: the Vulkan cloud pass reused a pipeline
        // baked with another draw's depth-test-off state and drew through
        // terrain. Blend factors also weren't keyed at all, so the sky
        // renderer's additive sun/moon draws (same shader, same flags, only
        // dstBlendFactor differs) reused the translucent sunrise pipeline.
        //
        // Bit budget: 35 bits of state + 1 bit layout + 27 bits of shader
        // handle = 63 of 64. Reference + masks stay out (dynamic state, set
        // per-draw via vkCmdSetStencil*); depth-bias CONSTANTS are baked
        // but keyed only by the enable bit — fine while every biased draw
        // uses the same constants (block break overlay only today).
        int layoutType = 0;
        {
            auto sit = m_shaders.find(shader);
            layoutType = (sit != m_shaders.end()) ? sit->second.layoutType : 0;
        }
        uint64_t key = 0;
        int bit = 0;
        auto pack = [&key, &bit](uint64_t value, int bits) {
            key |= (value & ((1ull << bits) - 1)) << bit;
            bit += bits;
        };
        pack(state.depthTestEnabled ? 1 : 0, 1);
        pack(state.depthWriteEnabled ? 1 : 0, 1);
        pack(static_cast<uint64_t>(state.depthCompareOp), 3);
        pack(state.blendEnabled ? 1 : 0, 1);
        pack(static_cast<uint64_t>(state.srcBlendFactor), 4);
        pack(static_cast<uint64_t>(state.dstBlendFactor), 4);
        pack(static_cast<uint64_t>(state.cullMode), 2);
        pack(static_cast<uint64_t>(state.frontFace), 1);
        pack(static_cast<uint64_t>(state.polygonMode), 1);
        pack(state.depthBiasEnabled ? 1 : 0, 1);
        pack(static_cast<uint64_t>(state.primitiveType), 2);
        pack(state.colorWriteEnabled ? 1 : 0, 1);
        pack(state.stencilTestEnabled ? 1 : 0, 1);
        pack(static_cast<uint64_t>(state.stencilCompareOp), 3);
        pack(static_cast<uint64_t>(state.stencilFailOp), 3);
        pack(static_cast<uint64_t>(state.stencilDepthFailOp), 3);
        pack(static_cast<uint64_t>(state.stencilPassOp), 3);
        pack((state.depthClampEnabled && m_depthClampSupported) ? 1 : 0, 1);
        pack(static_cast<uint64_t>(layoutType), 1);
        pack(shader, 27);
        return static_cast<size_t>(key);
    }

    static VkStencilOp ToVkStencilOp(StencilOp op) {
        switch (op) {
            case StencilOp::Keep:      return VK_STENCIL_OP_KEEP;
            case StencilOp::Zero:      return VK_STENCIL_OP_ZERO;
            case StencilOp::Replace:   return VK_STENCIL_OP_REPLACE;
            case StencilOp::IncrClamp: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
            case StencilOp::DecrClamp: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
            case StencilOp::Invert:    return VK_STENCIL_OP_INVERT;
            case StencilOp::IncrWrap:  return VK_STENCIL_OP_INCREMENT_AND_WRAP;
            case StencilOp::DecrWrap:  return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        }
        return VK_STENCIL_OP_KEEP;
    }

    VkPipeline VKBackend::GetOrCreatePipeline(const PipelineState& state, ShaderHandle shader) {
        size_t hash = HashPipelineState(state, shader);
        auto it = m_pipelines.find(hash);
        if (it != m_pipelines.end()) return it->second.pipeline;

        VkPipeline pipeline = CreateGraphicsPipeline(state, shader);
        if (pipeline != VK_NULL_HANDLE) {
            m_pipelines[hash] = PipelineRecord{state, shader, pipeline};
            ++m_pipelinesSinceSave;
            m_lastPipelineFrame = m_frameNumber;
        }
        return pipeline;
    }

    VkPipeline VKBackend::CreateGraphicsPipeline(const PipelineState& state, ShaderHandle shader) {
        // On MoltenVK this is a Metal shader/pipeline-state compile unless the
        // VkPipelineCache already holds it. Mid-frame, that is a hitch.
        PROFILE_ZONE_N("Vk.CreateGraphicsPipeline");
        auto shaderIt = m_shaders.find(shader);
        if (shaderIt == m_shaders.end()) return VK_NULL_HANDLE;

        // Shader stages
        VkPipelineShaderStageCreateInfo vertStage{};
        vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertStage.module = shaderIt->second.vertModule;
        vertStage.pName = "main";

        VkPipelineShaderStageCreateInfo fragStage{};
        fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragStage.module = shaderIt->second.fragModule;
        fragStage.pName = "main";

        VkPipelineShaderStageCreateInfo stages[] = {vertStage, fragStage};

        // Vertex input. If the shader has a registered VertexLayout, use
        // it (portal-feature shaders register theirs after creation). Else
        // fall back to the hardcoded 24-byte block layout for backward
        // compatibility with the existing chunk + crosshair + highlight
        // + GUI shaders that all share that format.
        VkVertexInputBindingDescription bindingDesc{};
        bindingDesc.binding   = 0;
        bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::vector<VkVertexInputAttributeDescription> attrDescs;
        {
            auto sit = m_shaders.find(shader);
            const VertexLayout* layout = (sit != m_shaders.end() && !sit->second.vertexLayout.attributes.empty())
                ? &sit->second.vertexLayout : nullptr;
            if (layout) {
                bindingDesc.stride = layout->stride;
                attrDescs.reserve(layout->attributes.size());
                for (const auto& a : layout->attributes) {
                    VkFormat fmt = VK_FORMAT_UNDEFINED;
                    if (a.type == AttribType::Float) {
                        switch (a.componentCount) {
                            case 1: fmt = VK_FORMAT_R32_SFLOAT;             break;
                            case 2: fmt = VK_FORMAT_R32G32_SFLOAT;          break;
                            case 3: fmt = VK_FORMAT_R32G32B32_SFLOAT;       break;
                            case 4: fmt = VK_FORMAT_R32G32B32A32_SFLOAT;    break;
                        }
                    } else if (a.type == AttribType::UShort) {
                        // unorm16 terrain tile rect (and any future 16-bit
                        // attribute). R16G16(B16A16)_UNORM are mandatory
                        // vertex-buffer formats in Vulkan, so no capability
                        // query is needed.
                        if (a.componentCount == 2) {
                            fmt = a.normalized ? VK_FORMAT_R16G16_UNORM
                                               : VK_FORMAT_R16G16_UINT;
                        } else if (a.componentCount == 4) {
                            fmt = a.normalized ? VK_FORMAT_R16G16B16A16_UNORM
                                               : VK_FORMAT_R16G16B16A16_UINT;
                        }
                    } else { // UByte
                        // 4 components: UNORM if normalized (vec4 0..1
                        // in the shader), UINT if not (uvec4 0..255 in
                        // the shader — Vulkan REQUIRES the shader-side
                        // attribute to be uvec4 when format is _UINT).
                        // USCALED would let us keep vec4 but it's
                        // optional in Vulkan and MoltenVK on Apple
                        // doesn't always support it for vertex inputs.
                        if (a.componentCount == 4) {
                            fmt = a.normalized ? VK_FORMAT_R8G8B8A8_UNORM
                                               : VK_FORMAT_R8G8B8A8_UINT;
                        }
                    }
                    if (fmt == VK_FORMAT_UNDEFINED) {
                        Log::Warning("VKBackend: unsupported attrib (loc=%u count=%u type=%d) — pipeline will fail",
                                     a.location, a.componentCount, (int)a.type);
                    }
                    attrDescs.push_back({a.location, 0, fmt, a.offset});
                }
            } else {
                // Default block layout (pos3f + uv2f + color4u8 normalized).
                bindingDesc.stride = 24;
                attrDescs = {
                    {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                    {1, 0, VK_FORMAT_R32G32_SFLOAT,    (uint32_t)(sizeof(float) * 3)},
                    {2, 0, VK_FORMAT_R8G8B8A8_UNORM,   (uint32_t)(sizeof(float) * 5)},
                };
            }
        }

        // Second binding for per-instance attributes when the shader
        // registered an instance layout (RegisterShaderInstanceLayout).
        VkVertexInputBindingDescription bindings[2] = { bindingDesc, {} };
        uint32_t bindingCount = 1;
        {
            const VertexLayout& inst = shaderIt->second.instanceLayout;
            if (!inst.attributes.empty()) {
                bindings[1].binding   = 1;
                bindings[1].stride    = inst.stride;
                bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
                bindingCount = 2;
                for (const auto& a : inst.attributes) {
                    VkFormat fmt = VK_FORMAT_UNDEFINED;
                    if (a.type == AttribType::Float) {
                        switch (a.componentCount) {
                            case 1: fmt = VK_FORMAT_R32_SFLOAT;          break;
                            case 2: fmt = VK_FORMAT_R32G32_SFLOAT;       break;
                            case 3: fmt = VK_FORMAT_R32G32B32_SFLOAT;    break;
                            case 4: fmt = VK_FORMAT_R32G32B32A32_SFLOAT; break;
                        }
                    } else if (a.type == AttribType::UShort) {
                        if (a.componentCount == 2) {
                            fmt = a.normalized ? VK_FORMAT_R16G16_UNORM
                                               : VK_FORMAT_R16G16_UINT;
                        } else if (a.componentCount == 4) {
                            fmt = a.normalized ? VK_FORMAT_R16G16B16A16_UNORM
                                               : VK_FORMAT_R16G16B16A16_UINT;
                        }
                    } else if (a.componentCount == 4) {
                        fmt = a.normalized ? VK_FORMAT_R8G8B8A8_UNORM
                                           : VK_FORMAT_R8G8B8A8_UINT;
                    }
                    attrDescs.push_back({a.location, 1, fmt, a.offset});
                }
            }
        }

        VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputInfo.vertexBindingDescriptionCount = bindingCount;
        vertexInputInfo.pVertexBindingDescriptions    = bindings;
        vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrDescs.size());
        vertexInputInfo.pVertexAttributeDescriptions    = attrDescs.data();

        // Input assembly
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        switch (state.primitiveType) {
            case PrimitiveType::Lines:         inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
            case PrimitiveType::LineStrip:     inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
            case PrimitiveType::TriangleStrip: inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
            default:                           inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
        }
        inputAssembly.primitiveRestartEnable = VK_FALSE;

        // Dynamic state (viewport + scissor + per-draw stencil ref/masks).
        // Treating stencil reference + read/write masks as dynamic means we
        // don't need a separate VkPipeline per (gunId × recursion-level)
        // combination — just call vkCmdSetStencilReference between draws.
        // Compare/fail/pass ops still go in the pipeline because changing
        // those is rare (each Phase 6 sub-pass uses a fixed op set).
        VkDynamicState dynamicStates[] = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_STENCIL_REFERENCE,
            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
            VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        };
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = sizeof(dynamicStates) / sizeof(dynamicStates[0]);
        dynamicState.pDynamicStates = dynamicStates;

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        // Rasterizer
        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.depthClampEnable = (state.depthClampEnabled && m_depthClampSupported) ? VK_TRUE : VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode = (state.polygonMode == PolygonMode::Fill) ? VK_POLYGON_MODE_FILL : VK_POLYGON_MODE_LINE;
        rasterizer.lineWidth = state.lineWidth;
        rasterizer.cullMode = (state.cullMode == CullMode::None) ? VK_CULL_MODE_NONE :
                             (state.cullMode == CullMode::Front) ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
        rasterizer.frontFace = (state.frontFace == FrontFace::CounterClockwise) ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
        rasterizer.depthBiasEnable = state.depthBiasEnabled ? VK_TRUE : VK_FALSE;
        rasterizer.depthBiasConstantFactor = state.depthBiasConstant;
        rasterizer.depthBiasSlopeFactor = state.depthBiasSlope;
        rasterizer.depthBiasClamp = 0.0f;

        // Multisampling
        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.sampleShadingEnable = VK_FALSE;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Depth stencil
        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = state.depthTestEnabled ? VK_TRUE : VK_FALSE;
        depthStencil.depthWriteEnable = state.depthWriteEnabled ? VK_TRUE : VK_FALSE;
        depthStencil.depthCompareOp = ToVkCompareOp(state.depthCompareOp);
        depthStencil.depthBoundsTestEnable = VK_FALSE;
        depthStencil.stencilTestEnable = state.stencilTestEnabled ? VK_TRUE : VK_FALSE;
        // Front + back use the same op set — keep the API minimal until a
        // feature actually wants asymmetric ops. compareMask, writeMask, and
        // reference are listed as dynamic state above, so the values set
        // here at pipeline-creation time are placeholders only — the real
        // values come from vkCmdSetStencil{Reference,CompareMask,WriteMask}
        // before each draw.
        VkStencilOpState stencilOp{};
        stencilOp.failOp      = ToVkStencilOp(state.stencilFailOp);
        stencilOp.passOp      = ToVkStencilOp(state.stencilPassOp);
        stencilOp.depthFailOp = ToVkStencilOp(state.stencilDepthFailOp);
        stencilOp.compareOp   = ToVkCompareOp(state.stencilCompareOp);
        stencilOp.compareMask = state.stencilReadMask;
        stencilOp.writeMask   = state.stencilWriteMask;
        stencilOp.reference   = state.stencilReference;
        depthStencil.front = stencilOp;
        depthStencil.back  = stencilOp;

        // Color blending
        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.colorWriteMask = state.colorWriteEnabled
            ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT)
            : 0;
        colorBlendAttachment.blendEnable = state.blendEnabled ? VK_TRUE : VK_FALSE;
        colorBlendAttachment.srcColorBlendFactor = ToVkBlendFactor(state.srcBlendFactor);
        colorBlendAttachment.dstColorBlendFactor = ToVkBlendFactor(state.dstBlendFactor);
        colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.logicOpEnable = VK_FALSE;
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &colorBlendAttachment;

        // Create pipeline
        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.pDynamicState = &dynamicState;
        // Pick pipeline layout based on shader's layoutType (0=block,
        // 1=portal). Portal-feature shaders need the descriptor layout
        // that includes the CommonUBO + BonesUBO bindings.
        VkPipelineLayout pl = m_pipelineLayout;
        {
            auto sit = m_shaders.find(shader);
            if (sit != m_shaders.end() && sit->second.layoutType == 1 &&
                m_portalPipelineLayout != VK_NULL_HANDLE) {
                pl = m_portalPipelineLayout;
            }
        }
        pipelineInfo.layout = pl;
        pipelineInfo.renderPass = m_renderPass;
        pipelineInfo.subpass = 0;

        VkPipeline pipeline;
        VkResult result = vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pipelineInfo, nullptr, &pipeline);
        if (result != VK_SUCCESS) {
            Log::Error("VKBackend: Failed to create graphics pipeline (VkResult=%d, shader=%u, cullMode=%d, blend=%d, depthTest=%d)",
                      static_cast<int>(result), shader,
                      static_cast<int>(state.cullMode), state.blendEnabled, state.depthTestEnabled);
            return VK_NULL_HANDLE;
        }
        return pipeline;
    }

    // ========================================================================
    // ENUM CONVERSIONS
    // ========================================================================

    VkCompareOp VKBackend::ToVkCompareOp(CompareOp op) const {
        switch (op) {
            case CompareOp::Never:        return VK_COMPARE_OP_NEVER;
            case CompareOp::Less:         return VK_COMPARE_OP_LESS;
            case CompareOp::Equal:        return VK_COMPARE_OP_EQUAL;
            case CompareOp::LessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
            case CompareOp::Greater:      return VK_COMPARE_OP_GREATER;
            case CompareOp::NotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
            case CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
            case CompareOp::Always:       return VK_COMPARE_OP_ALWAYS;
        }
        return VK_COMPARE_OP_LESS_OR_EQUAL;
    }

    VkBlendFactor VKBackend::ToVkBlendFactor(BlendFactor factor) const {
        switch (factor) {
            case BlendFactor::Zero:              return VK_BLEND_FACTOR_ZERO;
            case BlendFactor::One:               return VK_BLEND_FACTOR_ONE;
            case BlendFactor::SrcColor:          return VK_BLEND_FACTOR_SRC_COLOR;
            case BlendFactor::OneMinusSrcColor:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            case BlendFactor::DstColor:          return VK_BLEND_FACTOR_DST_COLOR;
            case BlendFactor::OneMinusDstColor:  return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
            case BlendFactor::SrcAlpha:          return VK_BLEND_FACTOR_SRC_ALPHA;
            case BlendFactor::OneMinusSrcAlpha:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            case BlendFactor::DstAlpha:          return VK_BLEND_FACTOR_DST_ALPHA;
            case BlendFactor::OneMinusDstAlpha:  return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        }
        return VK_BLEND_FACTOR_ONE;
    }

    VkFilter VKBackend::ToVkFilter(TextureFilter filter) const {
        switch (filter) {
            case TextureFilter::Nearest: return VK_FILTER_NEAREST;
            case TextureFilter::Linear:  return VK_FILTER_LINEAR;
            default: return VK_FILTER_NEAREST;
        }
    }

    VkSamplerAddressMode VKBackend::ToVkWrap(TextureWrap wrap) const {
        switch (wrap) {
            case TextureWrap::Repeat:         return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            case TextureWrap::ClampToEdge:    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            case TextureWrap::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        }
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }

    VkCullModeFlagBits VKBackend::ToVkCullMode(CullMode mode) const {
        switch (mode) {
            case CullMode::None:  return VK_CULL_MODE_NONE;
            case CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
            case CullMode::Back:  return VK_CULL_MODE_BACK_BIT;
        }
        return VK_CULL_MODE_BACK_BIT;
    }

    VkPolygonMode VKBackend::ToVkPolygonMode(PolygonMode mode) const {
        return mode == PolygonMode::Fill ? VK_POLYGON_MODE_FILL : VK_POLYGON_MODE_LINE;
    }

    VkFrontFace VKBackend::ToVkFrontFace(FrontFace face) const {
        return face == FrontFace::CounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    }

    std::vector<char> VKBackend::ReadBinaryFile(const std::string& path) const {
        std::ifstream file(path, std::ios::ate | std::ios::binary);
        if (!file.is_open()) return {};
        size_t size = static_cast<size_t>(file.tellg());
        std::vector<char> buffer(size);
        file.seekg(0);
        file.read(buffer.data(), size);
        return buffer;
    }

} // namespace Render

#endif // HAS_VULKAN

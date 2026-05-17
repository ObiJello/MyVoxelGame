// File: src/client/renderer/backend/vulkan/VKBackend.cpp
#ifdef HAS_VULKAN

#include "VKBackend.hpp"
#include "common/core/Log.hpp"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>

#include <set>
#include <fstream>
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
        if (m_portalDescriptorLayout != VK_NULL_HANDLE && !CreatePortalPipelineLayout()) {
            Log::Warning("VKBackend: portal pipeline layout create failed");
        }
        if (m_portalPipelineLayout != VK_NULL_HANDLE && !CreateFrameUBOs()) {
            Log::Warning("VKBackend: per-frame UBO setup failed");
        }
        if (!CreatePipelineCache()) return false;

        Log::Info("VKBackend: Vulkan initialization complete");
        return true;
    }

    void VKBackend::Shutdown() {
        if (m_device == VK_NULL_HANDLE) return;

        vkDeviceWaitIdle(m_device);

        // Flush all deferred deletion queues before destroying resources
        for (auto& queue : m_deletionQueues) {
            for (auto& del : queue) {
                if (del.buffer != INVALID_BUFFER) DestroyBuffer(del.buffer);
                if (del.mesh != INVALID_MESH) DestroyMesh(del.mesh);
            }
            queue.clear();
        }

        // Destroy persistent texture staging buffer
        if (m_texStagingBuffer != VK_NULL_HANDLE) {
            vkUnmapMemory(m_device, m_texStagingMemory);
            vkDestroyBuffer(m_device, m_texStagingBuffer, nullptr);
            vkFreeMemory(m_device, m_texStagingMemory, nullptr);
            m_texStagingBuffer = VK_NULL_HANDLE;
            m_texStagingMapped = nullptr;
            m_texStagingCapacity = 0;
        }
        m_pendingTextureUpdates.clear();

        // Destroy all user resources
        for (auto& [h, mesh] : m_meshes) { /* No VK objects to destroy */ }
        m_meshes.clear();

        for (auto& [h, buf] : m_buffers) {
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

        for (auto& [hash, pipeline] : m_pipelines) {
            vkDestroyPipeline(m_device, pipeline, nullptr);
        }
        m_pipelines.clear();

        // Destroy core resources
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
        if (m_pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        if (m_textureDescriptorLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(m_device, m_textureDescriptorLayout, nullptr);
        if (m_descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        if (m_imguiDescriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, m_imguiDescriptorPool, nullptr);

        CleanupSwapchain();

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            vkDestroySemaphore(m_device, m_imageAvailableSemaphores[i], nullptr);
            vkDestroySemaphore(m_device, m_renderFinishedSemaphores[i], nullptr);
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
        m_frameActive = false; // Only set true at end if everything succeeds

        // Wait for the previous frame with this index to finish (1 second timeout)
        VkResult fenceResult = vkWaitForFences(m_device, 1, &m_inFlightFences[m_currentFrame], VK_TRUE, 1'000'000'000);
        if (fenceResult == VK_TIMEOUT) {
            Log::Error("VKBackend: Fence wait timed out (1s) — possible GPU hang");
            // Reset the fence and try to continue rather than blocking forever
            vkResetFences(m_device, 1, &m_inFlightFences[m_currentFrame]);
        }

        // Flush deferred deletions for this frame slot — GPU is done with these resources
        for (auto& del : m_deletionQueues[m_currentFrame]) {
            if (del.buffer != INVALID_BUFFER) DestroyBuffer(del.buffer);
            if (del.mesh != INVALID_MESH) DestroyMesh(del.mesh);
        }
        m_deletionQueues[m_currentFrame].clear();

        // Acquire swapchain image (1 second timeout)
        VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain, 1'000'000'000,
            m_imageAvailableSemaphores[m_currentFrame], VK_NULL_HANDLE, &m_currentImageIndex);

        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            RecreateSwapchain(m_window);
            return;
        }

        if (result == VK_TIMEOUT) {
            Log::Error("VKBackend: Swapchain image acquire timed out (1s)");
            return;
        }

        vkResetFences(m_device, 1, &m_inFlightFences[m_currentFrame]);

        // Reset and begin command buffer
        vkResetCommandBuffer(m_commandBuffers[m_currentFrame], 0);

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
        FlushPendingTextureUpdates(m_commandBuffers[m_currentFrame]);

        vkCmdBeginRenderPass(m_commandBuffers[m_currentFrame], &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        // Reset per-frame state (each command buffer starts with nothing bound)
        m_currentPipeline = VK_NULL_HANDLE;
        m_boundShader = INVALID_SHADER;
        m_boundTexture = INVALID_TEXTURE;

        // Reset the per-frame UBO ring write cursor — each frame starts
        // writing into slot 0 of its own ring buffer. (The previous
        // frame may still be reading its ring; that's fine, this frame
        // gets a separate ring entirely.)
        m_frameUBOs[m_currentFrame].commonWriteSlot = 0;
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
        if (!m_frameActive) return; // BeginFrame failed — skip submit/present
        m_frameActive = false;

        // End render pass
        vkCmdEndRenderPass(m_commandBuffers[m_currentFrame]);
        vkEndCommandBuffer(m_commandBuffers[m_currentFrame]);

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

        VkSemaphore signalSemaphores[] = {m_renderFinishedSemaphores[m_currentFrame]};
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;

        vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_inFlightFences[m_currentFrame]);

        // Present
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &m_swapchain;
        presentInfo.pImageIndices = &m_currentImageIndex;

        VkResult result = vkQueuePresentKHR(m_presentQueue, &presentInfo);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || m_framebufferResized) {
            m_framebufferResized = false;
            RecreateSwapchain(window);
        }

        m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
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

    // ========================================================================
    // BUFFERS
    // ========================================================================

    BufferHandle VKBackend::CreateBuffer(BufferUsage usage, size_t size,
                                        const void* data, BufferAccess access) {
        VkBufferUsageFlags vkUsage = 0;
        switch (usage) {
            case BufferUsage::Vertex:  vkUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT; break;
            case BufferUsage::Index:   vkUsage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT; break;
            case BufferUsage::Uniform: vkUsage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT; break;
            case BufferUsage::Staging: vkUsage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; break;
        }

        if (access == BufferAccess::Static && data != nullptr) {
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
            // Host-visible buffer (dynamic/streaming)
            VkBuffer buffer;
            VkDeviceMemory memory;
            CreateVkBuffer(size, vkUsage,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          buffer, memory);

            if (data != nullptr) {
                void* mapped;
                vkMapMemory(m_device, memory, 0, size, 0, &mapped);
                std::memcpy(mapped, data, size);
                vkUnmapMemory(m_device, memory);
            }

            uint32_t handle = AllocHandle();
            m_buffers[handle] = {buffer, memory, size, usage};
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
        if (it == m_buffers.end()) return;

        void* mapped;
        vkMapMemory(m_device, it->second.memory, offset, size, 0, &mapped);
        std::memcpy(mapped, data, size);
        vkUnmapMemory(m_device, it->second.memory);
    }

    void VKBackend::DestroyBuffer(BufferHandle handle) {
        auto it = m_buffers.find(handle);
        if (it == m_buffers.end()) return;

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

    // ========================================================================
    // TEXTURES
    // ========================================================================

    TextureHandle VKBackend::CreateTexture2D(int width, int height,
                                            TextureFormat format, const void* data) {
        VkFormat vkFormat = VK_FORMAT_R8G8B8A8_UNORM;
        if (format == TextureFormat::SRGB8_A8) vkFormat = VK_FORMAT_R8G8B8A8_SRGB;

        VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * height * 4;

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
        TransitionImageLayout(image, vkFormat, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1);
        CopyBufferToImage(stagingBuffer, image, width, height);
        TransitionImageLayout(image, vkFormat, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1);

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

        VkDescriptorSet descriptorSet;
        vkAllocateDescriptorSets(m_device, &allocInfo, &descriptorSet);

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

        size_t dataSize = static_cast<size_t>(width) * height * 4;
        const auto* src = static_cast<const unsigned char*>(data);

        PendingTextureUpdate update;
        update.image = it->second.image;
        update.x = x;
        update.y = y;
        update.width = width;
        update.height = height;
        update.data.assign(src, src + dataSize);
        m_pendingTextureUpdates.push_back(std::move(update));
    }

    void VKBackend::EnsureTexStagingBuffer(size_t requiredSize) {
        if (requiredSize <= m_texStagingCapacity) return;

        // Destroy old buffer
        if (m_texStagingBuffer != VK_NULL_HANDLE) {
            vkUnmapMemory(m_device, m_texStagingMemory);
            vkDestroyBuffer(m_device, m_texStagingBuffer, nullptr);
            vkFreeMemory(m_device, m_texStagingMemory, nullptr);
            m_texStagingMapped = nullptr;
        }

        // Grow to at least 256KB or 2x the requirement
        m_texStagingCapacity = std::max(requiredSize, std::max(m_texStagingCapacity * 2, size_t(256 * 1024)));

        CreateVkBuffer(m_texStagingCapacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      m_texStagingBuffer, m_texStagingMemory);

        vkMapMemory(m_device, m_texStagingMemory, 0, m_texStagingCapacity, 0, &m_texStagingMapped);
    }

    void VKBackend::FlushPendingTextureUpdates(VkCommandBuffer cmd) {
        if (m_pendingTextureUpdates.empty()) return;

        // Calculate total staging size and ensure buffer is large enough
        size_t totalSize = 0;
        for (const auto& update : m_pendingTextureUpdates)
            totalSize += update.data.size();
        EnsureTexStagingBuffer(totalSize);

        // Copy all pixel data into the persistent staging buffer
        size_t offset = 0;
        for (const auto& update : m_pendingTextureUpdates) {
            std::memcpy(static_cast<char*>(m_texStagingMapped) + offset,
                       update.data.data(), update.data.size());
            offset += update.data.size();
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
            barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            barriers[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barriers[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        }
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr,
            static_cast<uint32_t>(barriers.size()), barriers.data());

        // Record all buffer-to-image copies
        offset = 0;
        for (const auto& update : m_pendingTextureUpdates) {
            VkBufferImageCopy region{};
            region.bufferOffset = offset;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageOffset = {update.x, update.y, 0};
            region.imageExtent = {static_cast<uint32_t>(update.width),
                                  static_cast<uint32_t>(update.height), 1};
            vkCmdCopyBufferToImage(cmd, m_texStagingBuffer, update.image,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            offset += update.data.size();
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
        info.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_NEAREST;

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
        it->second.minFilter = (min == TextureFilter::Linear) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
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

    void VKBackend::GenerateMipmaps(TextureHandle handle) {
        // TODO: Implement mipmap generation with vkCmdBlitImage
    }

    void VKBackend::DestroyTexture(TextureHandle handle) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end()) return;

        vkDeviceWaitIdle(m_device);
        vkDestroySampler(m_device, it->second.sampler, nullptr);
        vkDestroyImageView(m_device, it->second.imageView, nullptr);
        vkDestroyImage(m_device, it->second.image, nullptr);
        vkFreeMemory(m_device, it->second.memory, nullptr);

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
        m_memStats.shaderCount++;
        Log::Info("VKBackend: Loaded SPIR-V shaders: %s + %s", vertSpvPath.c_str(), fragSpvPath.c_str());
        return handle;
    }

    void VKBackend::DestroyShader(ShaderHandle handle) {
        auto it = m_shaders.find(handle);
        if (it == m_shaders.end()) return;
        vkDestroyShaderModule(m_device, it->second.vertModule, nullptr);
        vkDestroyShaderModule(m_device, it->second.fragModule, nullptr);
        m_memStats.shaderCount--;
        m_shaders.erase(it);
    }

    void VKBackend::BindShader(ShaderHandle handle) {
        m_boundShader = handle;
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
        if (name == "uMVP") {
            const glm::mat4 vkMVP = kVkZCorrect * value;
            m_pushConstants.uMVP    = vkMVP;
            m_commonUBOData.uMVP    = vkMVP;
            m_commonUBODirty = true;
        } else if (name == "uModel") {
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
        if (name == "uTint" || name == "uColor" || name == "uClipPlane" ||
            name == "uPortalClipPlane") {
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
        }
    }
    void VKBackend::SetUniformVec3(ShaderHandle, const std::string& name, const glm::vec3& value) {
        if (name == "uPortalColor") {
            m_pushConstants.uColor          = glm::vec4(value, m_pushConstants.uColor.a);
            m_commonUBOData.uPortalColor    = glm::vec4(value, m_commonUBOData.uPortalColor.a);
            m_commonUBODirty = true;
        } else if (name == "uColorDark") {
            m_commonUBOData.uColorDark      = glm::vec4(value, m_commonUBOData.uColorDark.a);
            m_commonUBODirty = true;
        } else if (name == "uColorHot") {
            m_commonUBOData.uColorHot       = glm::vec4(value, m_commonUBOData.uColorHot.a);
            m_commonUBODirty = true;
        } else if (name == "uKeyDir") {
            m_commonUBOData.uKeyDir         = glm::vec4(value, m_commonUBOData.uKeyDir.a);
            m_commonUBODirty = true;
        } else if (name == "uTint" || name == "uColor") {
            m_pushConstants.uColor          = glm::vec4(value, m_pushConstants.uColor.a);
            m_commonUBOData.uTint           = glm::vec4(value, m_commonUBOData.uTint.a);
            m_commonUBODirty = true;
        }
    }
    void VKBackend::SetUniformVec2(ShaderHandle, const std::string& name, const glm::vec2& value) {
        if (name == "uScreenSize") {
            m_pushConstants.uScreenSize     = value;
            m_commonUBOData.uScreenSize     = value;
            m_commonUBODirty = true;
        } else if (name == "uUVMin") {
            m_pushConstants.uUVRange.x = value.x; m_pushConstants.uUVRange.y = value.y;
            m_commonUBOData.uUVRange.x = value.x; m_commonUBOData.uUVRange.y = value.y;
            m_commonUBODirty = true;
        } else if (name == "uUVMax") {
            m_pushConstants.uUVRange.z = value.x; m_pushConstants.uUVRange.w = value.y;
            m_commonUBOData.uUVRange.z = value.x; m_commonUBOData.uUVRange.w = value.y;
            m_commonUBODirty = true;
        }
    }
    void VKBackend::SetUniformFloat(ShaderHandle, const std::string& name, float value) {
        // Push-constant block-style aliases:
        if (name == "uLineWidth")           { m_pushConstants.uLineWidth = value; }
        else if (name == "uAlphaTest")      { m_pushConstants.uAlphaTest = value; }
        // Portal renderer + crosshair scalar packing:
        else if (name == "uPulse")          { m_pushConstants.uScalars.x = value; m_commonUBOData.uPortalColor.a = value; m_commonUBODirty = true; }
        else if (name == "uOpenAmount")     { m_pushConstants.uScalars.z = value; m_commonUBOData.uColorDark.a    = value; m_commonUBODirty = true; }
        else if (name == "uOpenAmountVS")   { m_commonUBOData.uColorHot.a    = value; m_commonUBODirty = true; }
        else if (name == "uKeyIntensity")   { m_commonUBOData.uKeyDir.a      = value; m_commonUBODirty = true; }
        else if (name == "uTime")           { m_pushConstants.uScalars.y = value; m_commonUBOData.uScalarsA.x = value; m_commonUBODirty = true; }
        else if (name == "uTimeVS")         { m_commonUBOData.uScalarsA.y = value; m_commonUBODirty = true; }
        else if (name == "uStaticAmount")   { m_commonUBOData.uScalarsA.z = value; m_commonUBODirty = true; }
        else if (name == "uColorScale")     { m_commonUBOData.uScalarsA.w = value; m_commonUBODirty = true; }
        else if (name == "uPortalActive")   { m_commonUBOData.uScalarsB.x = value; m_commonUBODirty = true; }
        else if (name == "uForceFarDepth")  { m_commonUBOData.uScalarsB.y = value; m_commonUBODirty = true; }
        else if (name == "uOutlineMode")    { m_commonUBOData.uScalarsB.z = value; m_commonUBODirty = true; }
        else if (name == "uFlashIntensity") { m_pushConstants.uScalars.w = value; m_commonUBOData.uScalarsB.w = value; m_commonUBODirty = true; }
        else if (name == "uAmbient")        { m_commonUBOData.uScalarsC.x = value; m_commonUBODirty = true; }
        else if (name == "uAlphaCutoff")    { m_commonUBOData.uScalarsC.y = value; m_commonUBODirty = true; }
        else if (name == "uExposure")       { m_commonUBOData.uScalarsC.z = value; m_commonUBODirty = true; }
        else if (name == "uHasBloom")       { m_commonUBOData.uScalarsC.w = value; m_commonUBODirty = true; }
        else if (name == "uHasSprite")      { m_commonUBOData.uScalarsD.x = value; m_commonUBODirty = true; }
        else if (name == "uUseSkin")        { m_commonUBOData.uScalarsD.y = value; m_commonUBODirty = true; }
        else if (name == "uUseTextures")    { m_commonUBOData.uScalarsD.z = value; m_commonUBODirty = true; }
    }
    void VKBackend::SetUniformInt(ShaderHandle, const std::string& name, int value) {
        // Texture-sampler bindings come through as integers (legacy
        // GL pattern). Vulkan binds textures via descriptor sets, so
        // we ignore those names. Other ints fall through into the
        // float path's uUseTextures slot etc.
        if (name == "uUseTextures") {
            m_commonUBOData.uScalarsD.z = (float)value;
            m_commonUBODirty = true;
        } else if (name == "uHasSprite") {
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

        // Get or create pipeline for current state + shader
        VkPipeline pipeline = GetOrCreatePipeline(m_currentPipelineState, m_boundShader);
        if (pipeline == VK_NULL_HANDLE) return;

        VkCommandBuffer cmd = m_commandBuffers[m_currentFrame];

        if (pipeline != m_currentPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            m_currentPipeline = pipeline;
        }
        // Push per-draw stencil dynamic state. Sticky across pipeline binds
        // within a command buffer, so re-pushing every draw is mildly
        // wasteful but never wrong; the early-out on stencilTestEnabled
        // keeps the overhead at zero when stencil isn't in use.
        ApplyDynamicStencilState(cmd);

        // Select pipeline layout based on shader's layoutType.
        VkPipelineLayout pl = m_pipelineLayout;
        bool isPortalShader = false;
        {
            auto sit = m_shaders.find(m_boundShader);
            if (sit != m_shaders.end() && sit->second.layoutType == 1 &&
                m_portalPipelineLayout != VK_NULL_HANDLE) {
                pl = m_portalPipelineLayout;
                isPortalShader = true;
            }
        }

        vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                          0, sizeof(PushConstantBlock), &m_pushConstants);

        if (isPortalShader) {
            // Portal-feature shader: bind UBO+texture descriptor set
            // (BindPortalDescriptorForDraw also uploads any dirty UBO data).
            BindPortalDescriptorForDraw(cmd, m_boundTexture);
        } else {
            // Block-style shader: original texture-only descriptor path.
            auto texIt = m_textures.find(m_boundTexture);
            if (texIt != m_textures.end() && texIt->second.descriptorSet != VK_NULL_HANDLE) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl,
                                       0, 1, &texIt->second.descriptorSet, 0, nullptr);
            } else {
                return;
            }
        }

        VkBuffer vertexBuffers[] = {vbIt->second.buffer};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(cmd, ibIt->second.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, indexCount, 1, indexOffset, 0, 0);
    }

    void VKBackend::DrawArrays(MeshHandle mesh, uint32_t vertexCount, uint32_t firstVertex) {
        if (mesh == INVALID_MESH || m_boundShader == INVALID_SHADER || vertexCount == 0) return;

        auto meshIt = m_meshes.find(mesh);
        if (meshIt == m_meshes.end()) return;

        auto vbIt = m_buffers.find(meshIt->second.vertexBuffer);
        if (vbIt == m_buffers.end()) return;

        VkPipeline pipeline = GetOrCreatePipeline(m_currentPipelineState, m_boundShader);
        if (pipeline == VK_NULL_HANDLE) return;

        VkCommandBuffer cmd = m_commandBuffers[m_currentFrame];
        if (pipeline != m_currentPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            m_currentPipeline = pipeline;
        }
        // Push per-draw stencil dynamic state. Sticky across pipeline binds
        // within a command buffer, so re-pushing every draw is mildly
        // wasteful but never wrong; the early-out on stencilTestEnabled
        // keeps the overhead at zero when stencil isn't in use.
        ApplyDynamicStencilState(cmd);

        // Pipeline layout + descriptor binding (mirrors DrawIndexed).
        VkPipelineLayout pl = m_pipelineLayout;
        bool isPortalShader = false;
        {
            auto sit = m_shaders.find(m_boundShader);
            if (sit != m_shaders.end() && sit->second.layoutType == 1 &&
                m_portalPipelineLayout != VK_NULL_HANDLE) {
                pl = m_portalPipelineLayout;
                isPortalShader = true;
            }
        }
        vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                          0, sizeof(PushConstantBlock), &m_pushConstants);
        if (isPortalShader) {
            BindPortalDescriptorForDraw(cmd, m_boundTexture);
        } else {
            auto texIt = m_textures.find(m_boundTexture);
            if (texIt != m_textures.end() && texIt->second.descriptorSet != VK_NULL_HANDLE) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl,
                                       0, 1, &texIt->second.descriptorSet, 0, nullptr);
            }
        }

        VkBuffer vertexBuffers[] = {vbIt->second.buffer};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);

        vkCmdDraw(cmd, vertexCount, 1, firstVertex, 0);
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

    void VKBackend::DrawIndexedBaseVertex(uint32_t indexCount, size_t indexByteOffset, int32_t baseVertex) {
        if (m_boundShader == INVALID_SHADER || indexCount == 0) return;
        if (m_megaBoundVBO == INVALID_BUFFER || m_megaBoundIBO == INVALID_BUFFER) return;
        if (!m_frameActive) return;

        auto vbIt = m_buffers.find(m_megaBoundVBO);
        auto ibIt = m_buffers.find(m_megaBoundIBO);
        if (vbIt == m_buffers.end() || ibIt == m_buffers.end()) return;

        VkPipeline pipeline = GetOrCreatePipeline(m_currentPipelineState, m_boundShader);
        if (pipeline == VK_NULL_HANDLE) return;

        VkCommandBuffer cmd = m_commandBuffers[m_currentFrame];

        if (pipeline != m_currentPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            m_currentPipeline = pipeline;
        }
        // Push per-draw stencil dynamic state. Sticky across pipeline binds
        // within a command buffer, so re-pushing every draw is mildly
        // wasteful but never wrong; the early-out on stencilTestEnabled
        // keeps the overhead at zero when stencil isn't in use.
        ApplyDynamicStencilState(cmd);

        VkPipelineLayout pl = m_pipelineLayout;
        bool isPortalShader = false;
        {
            auto sit = m_shaders.find(m_boundShader);
            if (sit != m_shaders.end() && sit->second.layoutType == 1 &&
                m_portalPipelineLayout != VK_NULL_HANDLE) {
                pl = m_portalPipelineLayout;
                isPortalShader = true;
            }
        }
        vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                          0, sizeof(PushConstantBlock), &m_pushConstants);
        if (isPortalShader) {
            BindPortalDescriptorForDraw(cmd, m_boundTexture);
        } else {
            auto texIt = m_textures.find(m_boundTexture);
            if (texIt != m_textures.end() && texIt->second.descriptorSet != VK_NULL_HANDLE) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl,
                                       0, 1, &texIt->second.descriptorSet, 0, nullptr);
            } else {
                return;
            }
        }

        VkBuffer vertexBuffers[] = {vbIt->second.buffer};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(cmd, ibIt->second.buffer, 0, VK_INDEX_TYPE_UINT32);

        uint32_t firstIndex = static_cast<uint32_t>(indexByteOffset / sizeof(uint32_t));
        vkCmdDrawIndexed(cmd, indexCount, 1, firstIndex, baseVertex, 0);
    }

    void VKBackend::MultiDrawIndexedBaseVertex(const int32_t* indexCounts,
                                                const size_t* indexByteOffsets,
                                                const int32_t* baseVertices,
                                                uint32_t drawCount) {
        if (m_boundShader == INVALID_SHADER || drawCount == 0) return;
        if (m_megaBoundVBO == INVALID_BUFFER || m_megaBoundIBO == INVALID_BUFFER) return;
        if (!m_frameActive) return;

        auto vbIt = m_buffers.find(m_megaBoundVBO);
        auto ibIt = m_buffers.find(m_megaBoundIBO);
        if (vbIt == m_buffers.end() || ibIt == m_buffers.end()) return;

        VkPipeline pipeline = GetOrCreatePipeline(m_currentPipelineState, m_boundShader);
        if (pipeline == VK_NULL_HANDLE) return;

        VkCommandBuffer cmd = m_commandBuffers[m_currentFrame];

        if (pipeline != m_currentPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            m_currentPipeline = pipeline;
        }
        // Push per-draw stencil dynamic state. Sticky across pipeline binds
        // within a command buffer, so re-pushing every draw is mildly
        // wasteful but never wrong; the early-out on stencilTestEnabled
        // keeps the overhead at zero when stencil isn't in use.
        ApplyDynamicStencilState(cmd);

        VkPipelineLayout pl = m_pipelineLayout;
        bool isPortalShader = false;
        {
            auto sit = m_shaders.find(m_boundShader);
            if (sit != m_shaders.end() && sit->second.layoutType == 1 &&
                m_portalPipelineLayout != VK_NULL_HANDLE) {
                pl = m_portalPipelineLayout;
                isPortalShader = true;
            }
        }
        vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                          0, sizeof(PushConstantBlock), &m_pushConstants);
        if (isPortalShader) {
            BindPortalDescriptorForDraw(cmd, m_boundTexture);
        } else {
            auto texIt = m_textures.find(m_boundTexture);
            if (texIt != m_textures.end() && texIt->second.descriptorSet != VK_NULL_HANDLE) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl,
                                       0, 1, &texIt->second.descriptorSet, 0, nullptr);
            } else {
                return;
            }
        }

        // Bind VBO + IBO once for the entire batch
        VkBuffer vertexBuffers[] = {vbIt->second.buffer};
        VkDeviceSize vbOffsets[] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, vbOffsets);
        vkCmdBindIndexBuffer(cmd, ibIt->second.buffer, 0, VK_INDEX_TYPE_UINT32);

        // Issue one draw per section (cheap — just command buffer recording)
        for (uint32_t i = 0; i < drawCount; i++) {
            if (indexCounts[i] <= 0) continue;
            uint32_t firstIndex = static_cast<uint32_t>(indexByteOffsets[i] / sizeof(uint32_t));
            vkCmdDrawIndexed(cmd, indexCounts[i], 1, firstIndex, baseVertices[i], 0);
        }
    }

    // ========================================================================
    // GPU TIMERS (stub)
    // ========================================================================

    GPUTimerHandle VKBackend::BeginGPUTimer(const std::string& name) { return INVALID_GPU_TIMER; }
    void VKBackend::EndGPUTimer(GPUTimerHandle) {}
    float VKBackend::GetGPUTimerResultMs(GPUTimerHandle) { return 0.0f; }

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

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
        Log::Info("VKBackend: Selected GPU: %s", props.deviceName);
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
        auto presentMode = ChooseSwapPresentMode(presentModes);
        auto extent = ChooseSwapExtent(capabilities, window);

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
        m_renderFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
        m_inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);

        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            if (vkCreateSemaphore(m_device, &semInfo, nullptr, &m_imageAvailableSemaphores[i]) != VK_SUCCESS ||
                vkCreateSemaphore(m_device, &semInfo, nullptr, &m_renderFinishedSemaphores[i]) != VK_SUCCESS ||
                vkCreateFence(m_device, &fenceInfo, nullptr, &m_inFlightFences[i]) != VK_SUCCESS)
                return false;
        }
        return true;
    }

    bool VKBackend::CreateDescriptorPool() {
        // Pool sized for: many block textures (one descriptor set per
        // texture) + a handful of portal descriptor sets (one per
        // frame-in-flight × N portal pipeline layouts; just MAX_FRAMES
        // for now). UBO descriptors: 2 per portal set (Common + Bones).
        VkDescriptorPoolSize poolSizes[2]{};
        poolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[0].descriptorCount = 1000;
        // CommonUBO + BonesUBO are dynamic — pool must size that type.
        poolSizes[1].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        poolSizes[1].descriptorCount = 16;   // 2 × MAX_FRAMES + headroom

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes    = poolSizes;
        poolInfo.maxSets       = 1000 + MAX_FRAMES_IN_FLIGHT;
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
        VkDescriptorSetLayout sets[3] = { m_textureDescriptorLayout,
                                          m_portalDescriptorLayout,
                                          m_textureDescriptorLayout };

        VkPipelineLayoutCreateInfo info{};
        info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount         = 3;
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
            if (fb.commonBuffer != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, fb.commonBuffer, nullptr); fb.commonBuffer = VK_NULL_HANDLE; }
            if (fb.commonMemory != VK_NULL_HANDLE) { vkFreeMemory(m_device,   fb.commonMemory, nullptr);  fb.commonMemory = VK_NULL_HANDLE; }
            if (fb.bonesBuffer  != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, fb.bonesBuffer,  nullptr); fb.bonesBuffer  = VK_NULL_HANDLE; }
            if (fb.bonesMemory  != VK_NULL_HANDLE) { vkFreeMemory(m_device,   fb.bonesMemory,  nullptr);  fb.bonesMemory  = VK_NULL_HANDLE; }
            // Descriptor sets are freed when the pool is destroyed.
        }
    }

    void VKBackend::BindPortalDescriptorForDraw(VkCommandBuffer cmd, TextureHandle tex) {
        FrameUBOs& fb = m_frameUBOs[m_currentFrame];

        // Allocate this draw's slot in the ring buffer. Each draw gets
        // its own offset so the GPU reads exactly the uniforms set
        // between this draw and the previous one — vs. the broken
        // single-buffer design where every recorded draw read the LAST
        // value of every uniform at submit time.
        //
        // If we run out of slots, wrap and overwrite the oldest. The
        // ring is sized (256/32 slots) to comfortably cover an entire
        // frame's draws; wrapping only happens in pathological cases.
        // Wrapping silently corrupts earlier draws' uniforms (same bug
        // as the original single-buffer design), so log a warning if
        // we hit it so we can bump the slot count.
        if (fb.commonWriteSlot >= kCommonSlotCount) {
            static bool warned = false;
            if (!warned) {
                Log::Warning("VKBackend: CommonUBO ring overflowed (>%u draws/frame) — "
                             "earlier draws may render with wrong uniforms",
                             kCommonSlotCount);
                warned = true;
            }
        }
        if (fb.bonesWriteSlot >= kBonesSlotCount) {
            static bool warned = false;
            if (!warned) {
                Log::Warning("VKBackend: BonesUBO ring overflowed (>%u draws/frame)",
                             kBonesSlotCount);
                warned = true;
            }
        }
        const uint32_t commonSlot = fb.commonWriteSlot % kCommonSlotCount;
        const uint32_t bonesSlot  = fb.bonesWriteSlot  % kBonesSlotCount;
        fb.commonWriteSlot++;
        fb.bonesWriteSlot++;

        const uint32_t commonOffset = commonSlot * m_commonSlotStride;
        const uint32_t bonesOffset  = bonesSlot  * m_bonesSlotStride;

        // Always memcpy current working state into this draw's slot.
        // (Even when not "dirty" — the dirty flag was only valid for the
        // single-buffer design. With per-draw slots, each slot starts
        // uninitialised, so we must always write.)
        std::memcpy(fb.commonMapped + commonOffset, &m_commonUBOData, sizeof(CommonUBO));
        std::memcpy(fb.bonesMapped  + bonesOffset,  &m_bonesUBOData,  sizeof(BonesUBO));
        m_commonUBODirty = false;
        m_bonesUBODirty  = false;

        // Three sets to bind:
        //   set=0 → primary texture (slot 0)
        //   set=1 → per-frame UBO descriptor (dynamic offset = this draw's slot)
        //   set=2 → secondary texture (slot 1) — for portal renderer's
        //           noise + colour-ramp pair. Falls back to the primary
        //           texture when nothing else is bound at slot 1 (any valid
        //           descriptor satisfies the layout; the shader simply
        //           doesn't read it).
        auto tex0It = m_textures.find(tex);
        if (tex0It == m_textures.end() || tex0It->second.descriptorSet == VK_NULL_HANDLE) return;
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
        const uint32_t dynOffsets[2] = { commonOffset, bonesOffset };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_portalPipelineLayout, 0, 3, sets, 2, dynOffsets);
    }

    void VKBackend::RegisterShaderVertexLayout(ShaderHandle shader, const VertexLayout& layout) {
        auto it = m_shaders.find(shader);
        if (it != m_shaders.end()) it->second.vertexLayout = layout;
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
        VkPipelineCacheCreateInfo cacheInfo{};
        cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        return vkCreatePipelineCache(m_device, &cacheInfo, nullptr, &m_pipelineCache) == VK_SUCCESS;
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
        CleanupSwapchain();

        // Destroy old render pass — format or present mode may have changed
        if (m_renderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(m_device, m_renderPass, nullptr);
            m_renderPass = VK_NULL_HANDLE;
        }

        // Invalidate all cached pipelines (they reference the old render pass)
        for (auto& [hash, pipeline] : m_pipelines) {
            vkDestroyPipeline(m_device, pipeline, nullptr);
        }
        m_pipelines.clear();
        m_currentPipeline = VK_NULL_HANDLE;

        CreateSwapchain(window);
        CreateImageViews();
        CreateRenderPass();
        CreateDepthResources();
        CreateFramebuffers();

        Log::Info("VKBackend: Swapchain recreated (pipelines invalidated)");
    }

    void VKBackend::CleanupSwapchain() {
        if (m_depthImageView != VK_NULL_HANDLE) vkDestroyImageView(m_device, m_depthImageView, nullptr);
        if (m_depthImage != VK_NULL_HANDLE) vkDestroyImage(m_device, m_depthImage, nullptr);
        if (m_depthMemory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_depthMemory, nullptr);
        for (auto fb : m_framebuffers) vkDestroyFramebuffer(m_device, fb, nullptr);
        for (auto iv : m_swapchainImageViews) vkDestroyImageView(m_device, iv, nullptr);
        if (m_swapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        if (m_renderPass != VK_NULL_HANDLE) vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        m_depthImageView = VK_NULL_HANDLE;
        m_depthImage = VK_NULL_HANDLE;
        m_depthMemory = VK_NULL_HANDLE;
        m_swapchain = VK_NULL_HANDLE;
        m_renderPass = VK_NULL_HANDLE;
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
        // Use UNORM — all rendering is in gamma space like Minecraft.
        // macOS color management is disabled separately via CAMetalLayer.colorspace = nil.
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
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
            if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
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
        EndSingleTimeCommands(cmd);
    }

    void VKBackend::CopyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) {
        VkCommandBuffer cmd = BeginSingleTimeCommands();
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(cmd, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        EndSingleTimeCommands(cmd);
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
        size_t hash = std::hash<uint32_t>{}(shader);
        // Mix the shader's layout type into the cache key — pipelines
        // baked against m_pipelineLayout vs m_portalPipelineLayout must
        // never collide because vkCmdBindDescriptorSets uses a specific
        // layout at draw time and a mismatch is a validation error.
        {
            auto sit = m_shaders.find(shader);
            int lt = (sit != m_shaders.end()) ? sit->second.layoutType : 0;
            hash ^= std::hash<int>{}(lt) << 16;
        }
        hash ^= std::hash<bool>{}(state.depthTestEnabled) << 1;
        hash ^= std::hash<bool>{}(state.depthWriteEnabled) << 2;
        hash ^= std::hash<bool>{}(state.blendEnabled) << 3;
        hash ^= std::hash<int>{}(static_cast<int>(state.cullMode)) << 4;
        hash ^= std::hash<int>{}(static_cast<int>(state.polygonMode)) << 5;
        hash ^= std::hash<int>{}(static_cast<int>(state.depthCompareOp)) << 6;
        hash ^= std::hash<int>{}(static_cast<int>(state.primitiveType)) << 7;
        hash ^= std::hash<int>{}(static_cast<int>(state.frontFace)) << 8;
        hash ^= std::hash<bool>{}(state.depthBiasEnabled) << 9;
        // Stencil — must be in the cache key, otherwise two pipelines with
        // identical depth/blend/cull but different stencil state would
        // collide and we'd reuse the wrong one. Reference + masks are
        // dynamic in Vulkan (set per-draw via vkCmdSetStencil*) so they
        // don't need to be in the hash; only the bits that are baked into
        // VkPipeline at creation do.
        hash ^= std::hash<bool>{}(state.stencilTestEnabled)               << 10;
        hash ^= std::hash<int>{}(static_cast<int>(state.stencilCompareOp)) << 11;
        hash ^= std::hash<int>{}(static_cast<int>(state.stencilFailOp))    << 12;
        hash ^= std::hash<int>{}(static_cast<int>(state.stencilDepthFailOp)) << 13;
        hash ^= std::hash<int>{}(static_cast<int>(state.stencilPassOp))   << 14;
        // Color write mask is baked into the pipeline (no Vulkan dynamic
        // state for it before VK_EXT_extended_dynamic_state3) → fold into
        // the cache key so depth/stencil-only pipelines don't collide with
        // their color-writing counterparts.
        hash ^= std::hash<bool>{}(state.colorWriteEnabled)                << 15;
        return hash;
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
        if (it != m_pipelines.end()) return it->second;

        VkPipeline pipeline = CreateGraphicsPipeline(state, shader);
        if (pipeline != VK_NULL_HANDLE) {
            m_pipelines[hash] = pipeline;
        }
        return pipeline;
    }

    VkPipeline VKBackend::CreateGraphicsPipeline(const PipelineState& state, ShaderHandle shader) {
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

        VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputInfo.vertexBindingDescriptionCount = 1;
        vertexInputInfo.pVertexBindingDescriptions    = &bindingDesc;
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
        rasterizer.depthClampEnable = VK_FALSE;
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

// File: src/client/renderer/backend/vulkan/VKBackend.hpp
#pragma once

#ifdef HAS_VULKAN

#include "../RenderBackend.hpp"
#include <vulkan/vulkan.h>
#include <unordered_map>
#include <vector>
#include <optional>
#include <array>

namespace Render {

    // Maximum number of frames that can be in-flight simultaneously
    static constexpr int MAX_FRAMES_IN_FLIGHT = 2;

    class VKBackend : public RenderBackend {
    public:
        VKBackend();
        ~VKBackend() override;

        // Lifecycle
        bool Initialize(GLFWwindow* window) override;
        void Shutdown() override;
        BackendType GetType() const override { return BackendType::Vulkan; }
        const char* GetName() const override { return "Vulkan 1.0 (MoltenVK)"; }
        GLFWwindow* GetWindow() const override { return m_window; }
        void SetVSync(bool enabled) override;

        // Frame
        void BeginFrame() override;
        void EndFrame(GLFWwindow* window) override;
        void SetClearColor(float r, float g, float b, float a) override;
        void Clear(bool color, bool depth) override;
        void SetViewport(int x, int y, int width, int height) override;

        // Buffers
        BufferHandle CreateBuffer(BufferUsage usage, size_t size,
                                 const void* data, BufferAccess access) override;
        void UpdateBuffer(BufferHandle handle, size_t offset,
                         size_t size, const void* data) override;
        void DestroyBuffer(BufferHandle handle) override;
        void DeferredDestroyBuffer(BufferHandle handle) override;

        // Textures
        TextureHandle CreateTexture2D(int width, int height, TextureFormat format,
                                     const void* data) override;
        void UpdateTexture2D(TextureHandle handle, int x, int y,
                            int width, int height, const void* data) override;
        void SetTextureFilter(TextureHandle handle, TextureFilter min, TextureFilter mag) override;
        void SetTextureWrap(TextureHandle handle, TextureWrap s, TextureWrap t) override;
        void GenerateMipmaps(TextureHandle handle) override;
        void DestroyTexture(TextureHandle handle) override;
        void BindTexture(TextureHandle handle, uint32_t slot) override;
        uintptr_t GetNativeTextureID(TextureHandle handle) const override;

        // Shaders
        ShaderHandle CreateShader(const std::string& vertexSource,
                                 const std::string& fragmentSource) override;
        ShaderHandle CreateShaderFromFiles(const std::string& vertexPath,
                                          const std::string& fragmentPath) override;
        void DestroyShader(ShaderHandle handle) override;
        void BindShader(ShaderHandle handle) override;
        void SetUniformMat4(ShaderHandle handle, const std::string& name, const glm::mat4& value) override;
        void SetUniformVec3(ShaderHandle handle, const std::string& name, const glm::vec3& value) override;
        void SetUniformVec2(ShaderHandle handle, const std::string& name, const glm::vec2& value) override;
        void SetUniformFloat(ShaderHandle handle, const std::string& name, float value) override;
        void SetUniformInt(ShaderHandle handle, const std::string& name, int value) override;

        // Meshes
        MeshHandle CreateMesh(BufferHandle vertexBuffer, BufferHandle indexBuffer,
                             const VertexLayout& layout) override;
        void DestroyMesh(MeshHandle handle) override;
        void DeferredDestroyMesh(MeshHandle handle) override;

        // Pipeline state
        void SetPipelineState(const PipelineState& state) override;
        void InvalidateStateCache() override;

        // Drawing
        void DrawIndexed(MeshHandle mesh, uint32_t indexCount, uint32_t indexOffset) override;
        void DrawArrays(MeshHandle mesh, uint32_t vertexCount, uint32_t firstVertex) override;

        // Mega-buffer rendering
        void BindVertexBuffer(BufferHandle vbo, uint32_t stride) override;
        void BindIndexBuffer(BufferHandle ibo) override;
        void DrawIndexedBaseVertex(uint32_t indexCount, size_t indexByteOffset, int32_t baseVertex) override;
        void MultiDrawIndexedBaseVertex(const int32_t* indexCounts, const size_t* indexByteOffsets,
                                        const int32_t* baseVertices, uint32_t drawCount) override;

        // GPU timers
        GPUTimerHandle BeginGPUTimer(const std::string& name) override;
        void EndGPUTimer(GPUTimerHandle handle) override;
        float GetGPUTimerResultMs(GPUTimerHandle handle) override;

        // Debug/Memory
        GPUMemoryStats GetMemoryStats() const override;

        // ImGui
        void ImGuiInit(GLFWwindow* window) override;
        void ImGuiNewFrame() override;
        void ImGuiRender() override;
        void ImGuiShutdown() override;

    private:
        // ====================================================================
        // VULKAN CORE OBJECTS
        // ====================================================================
        VkInstance m_instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
        VkSurfaceKHR m_surface = VK_NULL_HANDLE;
        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE;
        VkQueue m_graphicsQueue = VK_NULL_HANDLE;
        VkQueue m_presentQueue = VK_NULL_HANDLE;

        // Queue family indices
        struct QueueFamilyIndices {
            std::optional<uint32_t> graphicsFamily;
            std::optional<uint32_t> presentFamily;
            bool IsComplete() const { return graphicsFamily.has_value() && presentFamily.has_value(); }
        };
        QueueFamilyIndices m_queueFamilies;

        // ====================================================================
        // SWAPCHAIN
        // ====================================================================
        VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
        VkFormat m_swapchainFormat = VK_FORMAT_B8G8R8A8_SRGB;
        VkExtent2D m_swapchainExtent = {0, 0};
        std::vector<VkImage> m_swapchainImages;
        std::vector<VkImageView> m_swapchainImageViews;

        // Depth buffer
        VkImage m_depthImage = VK_NULL_HANDLE;
        VkDeviceMemory m_depthMemory = VK_NULL_HANDLE;
        VkImageView m_depthImageView = VK_NULL_HANDLE;
        VkFormat m_depthFormat = VK_FORMAT_D32_SFLOAT;

        // ====================================================================
        // RENDER PASS & FRAMEBUFFERS
        // ====================================================================
        VkRenderPass m_renderPass = VK_NULL_HANDLE;
        std::vector<VkFramebuffer> m_framebuffers;

        // ====================================================================
        // COMMAND BUFFERS
        // ====================================================================
        VkCommandPool m_commandPool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> m_commandBuffers;

        // ====================================================================
        // SYNCHRONIZATION
        // ====================================================================
        std::vector<VkSemaphore> m_imageAvailableSemaphores;
        std::vector<VkSemaphore> m_renderFinishedSemaphores;
        std::vector<VkFence> m_inFlightFences;
        uint32_t m_currentFrame = 0;
        uint32_t m_currentImageIndex = 0;
        bool m_framebufferResized = false;
        bool m_frameActive = false;

        // Deferred deletion queue — resources destroyed after GPU is done with them
        struct DeferredDeletion {
            BufferHandle buffer = INVALID_BUFFER;
            MeshHandle mesh = INVALID_MESH;
        };
        std::array<std::vector<DeferredDeletion>, MAX_FRAMES_IN_FLIGHT> m_deletionQueues;

        // ====================================================================
        // BATCHED TEXTURE UPDATES
        // ====================================================================
        // UpdateTexture2D queues updates here; BeginFrame flushes them into the
        // frame command buffer before the render pass starts — zero vkQueueWaitIdle.
        struct PendingTextureUpdate {
            VkImage image;
            int x, y, width, height;
            std::vector<unsigned char> data;
        };
        std::vector<PendingTextureUpdate> m_pendingTextureUpdates;

        // Persistent staging buffer (reused across frames, persistently mapped)
        VkBuffer m_texStagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory m_texStagingMemory = VK_NULL_HANDLE;
        size_t m_texStagingCapacity = 0;
        void* m_texStagingMapped = nullptr;

        void FlushPendingTextureUpdates(VkCommandBuffer cmd);
        void EnsureTexStagingBuffer(size_t requiredSize);

        // ====================================================================
        // DESCRIPTOR POOL & LAYOUT
        // ====================================================================
        VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_textureDescriptorLayout = VK_NULL_HANDLE;

        // ====================================================================
        // PIPELINE
        // ====================================================================
        VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
        VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
        // We create pipelines per PipelineState
        struct PipelineKey {
            PipelineState state;
            ShaderHandle shader;
            // Simplified hash/equals for now
            bool operator==(const PipelineKey& other) const;
        };
        struct PipelineKeyHash {
            size_t operator()(const PipelineKey& key) const;
        };
        std::unordered_map<size_t, VkPipeline> m_pipelines; // hash -> pipeline

        // ====================================================================
        // RESOURCE TRACKING
        // ====================================================================
        uint32_t m_nextHandle = 1;
        uint32_t AllocHandle() { return m_nextHandle++; }

        struct VKBufferInfo {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            size_t size = 0;
            BufferUsage usage;
        };
        std::unordered_map<uint32_t, VKBufferInfo> m_buffers;

        struct VKTextureInfo {
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkImageView imageView = VK_NULL_HANDLE;
            VkSampler sampler = VK_NULL_HANDLE;
            VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
            int width = 0, height = 0;
            uint32_t mipLevels = 1;
            size_t memorySize = 0;
        };
        std::unordered_map<uint32_t, VKTextureInfo> m_textures;

        struct VKShaderInfo {
            VkShaderModule vertModule = VK_NULL_HANDLE;
            VkShaderModule fragModule = VK_NULL_HANDLE;
        };
        std::unordered_map<uint32_t, VKShaderInfo> m_shaders;

        struct VKMeshInfo {
            BufferHandle vertexBuffer = INVALID_BUFFER;
            BufferHandle indexBuffer = INVALID_BUFFER;
            VertexLayout layout;
        };
        std::unordered_map<uint32_t, VKMeshInfo> m_meshes;

        // Currently bound state
        ShaderHandle m_boundShader = INVALID_SHADER;
        TextureHandle m_boundTexture = INVALID_TEXTURE;

        // Mega-buffer bound state
        BufferHandle m_megaBoundVBO = INVALID_BUFFER;
        BufferHandle m_megaBoundIBO = INVALID_BUFFER;
        PipelineState m_currentPipelineState;
        VkPipeline m_currentPipeline = VK_NULL_HANDLE;

        // Push constant data
        struct PushConstantBlock {
            glm::mat4 uMVP = glm::mat4(1.0f);   // 64 bytes
            glm::vec2 uScreenSize = {0, 0};       // 8 bytes
            float uLineWidth = 0.0f;              // 4 bytes
            float uAlphaTest = 0.0f;              // 4 bytes (cutout threshold)
        } m_pushConstants;                        // 80 bytes total

        // Clear color
        VkClearColorValue m_clearColor = {{0.5f, 0.7f, 1.0f, 1.0f}};

        // Memory tracking
        GPUMemoryStats m_memStats;

        // ImGui
        VkDescriptorPool m_imguiDescriptorPool = VK_NULL_HANDLE;

        // ====================================================================
        // INITIALIZATION HELPERS
        // ====================================================================
        bool CreateInstance();
        bool SetupDebugMessenger();
        bool CreateSurface(GLFWwindow* window);
        bool PickPhysicalDevice();
        bool CreateLogicalDevice();
        bool CreateSwapchain(GLFWwindow* window);
        bool CreateImageViews();
        bool CreateDepthResources();
        bool CreateRenderPass();
        bool CreateFramebuffers();
        bool CreateCommandPool();
        bool CreateCommandBuffers();
        bool CreateSyncObjects();
        bool CreateDescriptorPool();
        bool CreateDescriptorSetLayout();
        bool CreatePipelineLayout();
        bool CreatePipelineCache();

        // ====================================================================
        // SWAPCHAIN RECREATION
        // ====================================================================
        void RecreateSwapchain(GLFWwindow* window);
        void CleanupSwapchain();

        // ====================================================================
        // UTILITY HELPERS
        // ====================================================================
        QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device) const;
        bool CheckDeviceExtensionSupport(VkPhysicalDevice device) const;
        VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const;
        VkPresentModeKHR ChooseSwapPresentMode(const std::vector<VkPresentModeKHR>& modes) const;
        VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& caps, GLFWwindow* window) const;
        uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
        VkFormat FindDepthFormat() const;
        VkFormat FindSupportedFormat(const std::vector<VkFormat>& candidates,
                                    VkImageTiling tiling, VkFormatFeatureFlags features) const;

        // Buffer/Image creation helpers
        bool CreateVkBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                           VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& memory);
        bool CreateVkImage(uint32_t width, uint32_t height, uint32_t mipLevels,
                          VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage,
                          VkMemoryPropertyFlags properties, VkImage& image, VkDeviceMemory& memory);
        VkImageView CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags, uint32_t mipLevels);
        VkShaderModule CreateShaderModule(const std::vector<char>& code) const;

        // Single-use command buffer helpers
        VkCommandBuffer BeginSingleTimeCommands();
        void EndSingleTimeCommands(VkCommandBuffer commandBuffer);
        void TransitionImageLayout(VkImage image, VkFormat format,
                                  VkImageLayout oldLayout, VkImageLayout newLayout, uint32_t mipLevels);
        void CopyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);
        void CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);

        // Pipeline creation
        VkPipeline GetOrCreatePipeline(const PipelineState& state, ShaderHandle shader);
        VkPipeline CreateGraphicsPipeline(const PipelineState& state, ShaderHandle shader);
        size_t HashPipelineState(const PipelineState& state, ShaderHandle shader) const;

        // Vulkan enum conversion
        VkFilter ToVkFilter(TextureFilter filter) const;
        VkSamplerAddressMode ToVkWrap(TextureWrap wrap) const;
        VkCompareOp ToVkCompareOp(CompareOp op) const;
        VkBlendFactor ToVkBlendFactor(BlendFactor factor) const;
        VkCullModeFlagBits ToVkCullMode(CullMode mode) const;
        VkPolygonMode ToVkPolygonMode(PolygonMode mode) const;
        VkFrontFace ToVkFrontFace(FrontFace face) const;

        // File reading
        std::vector<char> ReadBinaryFile(const std::string& path) const;

        // Debug callback
        static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
            VkDebugUtilsMessageSeverityFlagBitsEXT severity,
            VkDebugUtilsMessageTypeFlagsEXT type,
            const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
            void* userData);

        // VSync state
        bool m_vsyncEnabled = true;

        // Window reference for swapchain recreation
        GLFWwindow* m_window = nullptr;
    };

} // namespace Render

#endif // HAS_VULKAN
